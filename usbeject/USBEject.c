/*
** USBEject -- Workbench "safely remove hardware" for Neptune
**
** Resident WBStartup daemon: publishes one "Eject <device>" menu item per
** attached device that can be ejected (DA_CanSafeEject), kept current via
** Neptune events. Selecting one runs psdSafeEjectDevice(), which has the
** bound classes flush and cleanly unmount their volumes - a volume with open
** files vetoes the whole eject - and then takes the device off the bus,
** whereupon we tell the user it is safe to unplug.
**
** Two presentations, decided at runtime (see MenuSync): our own "USB" menu in
** the menu bar where workbench.library V45 hierarchical AppMenus work, and
** plain items in the Tools menu everywhere else.  The fallback is what makes
** this work under a Workbench replacement: Directory Opus 5 patches
** AddAppMenuItemA and lists the items in its own Tools menu (source/Library/
** wb.c, source/Program/menus.c), but its emulation ignores the V45 title/key
** tags, so it hands back no key to hang sub-items on.  Note DOpus only shows
** them with "Show Tools menu" enabled in its Environment display options.
*/

#include <exec/exec.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/filehandler.h>
#include <intuition/intuition.h>
#include <workbench/workbench.h>
#include <devices/timer.h>
#include <libraries/neptune.h>
#include <libraries/neptune_class.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/wb.h>
#include <proto/locale.h>
#include <proto/neptune.h>
#include <proto/neptune_class.h>

#include <rexx/storage.h>
#include <rexx/errors.h>
/* our own base name: the inline stubs pass the base as void *, so its type does not matter, and
   rexxsyslib.library stays optional (opened only when Directory Opus is running) */
#define REXXSYSLIB_BASE_NAME RexxLib
#include <proto/rexxsyslib.h>

#include <neptune_version.h>

#include "locale.h"

static const char version[] __attribute__((used)) = NEP_VER("USBEject");

#define PORTNAME       "USBEject"
#define LOGNAME        ((STRPTR) "USBEject")   /* originator in the Neptune error log */
#define RETRY_SECS     5   /* AddAppMenuItem() fails while Workbench is not up yet */
#define DETACH_SECS    5   /* how long to wait for the device to drop off the bus */

/* CRT-provided; never define these ourselves (the definition would kill
   libnix's auto-open stub under -fno-common). */
extern struct ExecBase *SysBase;
extern struct DosLibrary *DOSBase;

struct IntuitionBase *IntuitionBase = NULL;
struct Library *WorkbenchBase = NULL;
struct LocaleBase *LocaleBase = NULL;   /* optional - English defaults if NULL */
struct Library *ps = NULL;

struct EjectDev
{
    struct Node         ed_Node;
    APTR                ed_Device;   /* Neptune pd - a key only, always revalidated */
    ULONG               ed_Id;       /* AppMenuItem id cookie, matched against am_ID */
    struct AppMenuItem *ed_Item;
    char                ed_Name[48];  /* the device, for the requesters */
    char                ed_Label[56]; /* "Eject <device>"; the host may keep the pointer,
                                         so it lives as long as the menu item does */
};

static struct MsgPort *appport = NULL;      /* AppMessages; public port doubling as the single-instance guard */
static BOOL portadded = FALSE;              /* ...but only once AddPort() ran */
static struct MsgPort *eventport = NULL;    /* Neptune event notes */
static struct MsgPort *timerport = NULL;
static struct timerequest *timerreq = NULL;
static BOOL timeropen = FALSE;
static BOOL timerpending = FALSE;
static APTR eventhandler = NULL;
static struct List devlist;
static struct AppMenuItem *titleitem = NULL;
static ULONG titlekey = 0;
static ULONG nextid = 0;
static BOOL dirty = FALSE;
static BOOL nomenuwarned = FALSE;   /* the "no AppMenu" warning is logged once, not every retry */
static struct RDArgs *rdargs = NULL;   /* one-shot mode's arguments, freed in Cleanup() */
static struct Library *RexxLib = NULL;  /* rexxsyslib.library, only while talking to Directory Opus */

static APTR DeviceForUnit(ULONG unit, STRPTR name, ULONG namelen);
static LONG CloseDOpusListers(APTR pd, STRPTR drive, ULONG drivelen);

/* /// "Timer helpers" */
static void TimerStop(void)
{
    if(timerpending)
    {
        AbortIO((struct IORequest *) timerreq);
        WaitIO((struct IORequest *) timerreq);
        timerpending = FALSE;
    }
}

static void TimerStart(ULONG secs)
{
    TimerStop();
    timerreq->tr_node.io_Command = TR_ADDREQUEST;
    timerreq->tr_time.tv_secs = secs;
    timerreq->tr_time.tv_micro = 0;
    SendIO((struct IORequest *) timerreq);
    timerpending = TRUE;
}

/* TRUE exactly once per expiry, so the caller can just ask. */
static BOOL TimerFired(void)
{
    if(timerpending && CheckIO((struct IORequest *) timerreq))
    {
        WaitIO((struct IORequest *) timerreq);
        timerpending = FALSE;
        return(TRUE);
    }
    return(FALSE);
}
/* \\\ */

/* /// "DrainEvents()" */
/* Reply every queued event note. Relevant events mark the menu dirty.
   Returns TRUE if an EHMB_REMDEVICE for watchpd (if any) came through. */
static BOOL DrainEvents(APTR watchpd)
{
    BOOL removed = FALSE;
    struct Message *pen;
    while((pen = GetMsg(eventport)))
    {
        IPTR eid = 0;
        IPTR param1 = 0;
        psdGetAttrs(PGA_EVENTNOTE, pen,
                    ENA_EventID, &eid,
                    ENA_Param1, &param1,
                    TAG_END);
        ReplyMsg(pen);
        switch(eid)
        {
            case EHMB_REMDEVICE:
                if(watchpd && ((APTR) param1 == watchpd))
                {
                    removed = TRUE;
                }
                /* fall through */
            case EHMB_ADDBINDING:
            case EHMB_REMBINDING:
                dirty = TRUE;
                break;
        }
    }
    return(removed);
}
/* \\\ */

/* /// "MenuTeardown()" */
static void MenuTeardown(void)
{
    struct EjectDev *ed;
    while((ed = (struct EjectDev *) RemHead(&devlist)))
    {
        if(ed->ed_Item)
        {
            RemoveAppMenuItem(ed->ed_Item);
        }
        FreeVec(ed);
    }
    if(titleitem)
    {
        RemoveAppMenuItem(titleitem);
        titleitem = NULL;
    }
    titlekey = 0;
}
/* \\\ */

/* /// "MenuSync()" */
/* Full rebuild from the current device list. Simple and self-healing: a
   couple of AddAppMenuItem() calls per attached drive, on rare events only.
   Stale ids in AppMessages already in flight simply match nothing. */
static void MenuSync(void)
{
    MenuTeardown();

    psdLockReadPBase();
    APTR pd = NULL;
    while((pd = psdGetNextDevice(pd)))
    {
        IPTR caneject = FALSE;
        STRPTR prodname = NULL;
        psdGetAttrs(PGA_DEVICE, pd,
                    DA_CanSafeEject, &caneject,
                    DA_ProductName, &prodname,
                    TAG_END);
        if(caneject)
        {
            struct EjectDev *ed;
            if((ed = (struct EjectDev *) AllocVec(sizeof(struct EjectDev), MEMF_CLEAR)))
            {
                ed->ed_Device = pd;
                psdSafeRawDoFmt(ed->ed_Name, sizeof(ed->ed_Name), "%s",
                                (prodname && *prodname) ? prodname : (STRPTR) _(MSG_FALLBACK_NAME));
                AddTail(&devlist, &ed->ed_Node);
            }
        }
    }
    psdUnlockPBase();

    struct EjectDev *ed = (struct EjectDev *) devlist.lh_Head;
    if(!ed->ed_Node.ln_Succ)
    {
        return;   /* nothing to offer - no "USB" title either */
    }

    /* Probe for hierarchical AppMenus by asking for a title key: a V45
       workbench.library fills one in, a host that only emulates the flat
       AppMenu (Directory Opus) leaves it at zero.  A NULL item, on the other
       hand, means there is no AppMenu to add to at all - Workbench has not
       opened yet - so try again later.  The title stays invisible until its
       first child is added, and is removed again if it turns out we cannot
       hang anything under it. */
    titlekey = 0;
    titleitem = AddAppMenuItem(0, 0, (STRPTR) _(MSG_MENU_TITLE), appport,
                               WBAPPMENUA_GetTitleKey, (IPTR) &titlekey,
                               TAG_DONE);
    if(!titleitem)
    {
        if(!nomenuwarned)
        {
            nomenuwarned = TRUE;
            psdAddErrorMsg(RETURN_WARN, LOGNAME,
                           "Workbench is not open - no USB eject menu yet (retrying every %ld seconds). "
                           "Neptune USB's Eject button works regardless.",
                           (ULONG) RETRY_SECS);
        }
        MenuTeardown();   /* nothing published yet, so this just drops the list */
        TimerStart(RETRY_SECS);
        return;
    }
    nomenuwarned = FALSE;
    if(!titlekey)
    {
        /* flat host: the items go into the Tools menu, so the title would
           only sit there doing nothing */
        RemoveAppMenuItem(titleitem);
        titleitem = NULL;
    }

    struct TagItem undertitle[2] = { { WBAPPMENUA_UseKey, titlekey }, { TAG_DONE, 0 } };
    ULONG count = 0;
    while(ed->ed_Node.ln_Succ)
    {
        ed->ed_Id = ++nextid;
        psdSafeRawDoFmt(ed->ed_Label, sizeof(ed->ed_Label), (STRPTR) _(MSG_MENU_EJECT), ed->ed_Name);
        ed->ed_Item = AddAppMenuItemA(ed->ed_Id, 0, ed->ed_Label, appport,
                                      titlekey ? undertitle : NULL);
        if(ed->ed_Item)
        {
            count++;
        }
        ed = (struct EjectDev *) ed->ed_Node.ln_Succ;
    }
    psdAddErrorMsg(RETURN_OK, LOGNAME,
                   "%ld drive(s) offered in the %s menu.",
                   count, titlekey ? (STRPTR) _(MSG_MENU_TITLE) : (STRPTR) "Tools");
}
/* \\\ */

/* /// "CentredRequest()" */
/* EasyRequestArgs() with no reference window, centred on its screen. With
   NULL, EasyRequestArgs() opened the requester in the screen's top-left
   corner (seen on the A1200, OS 3.9), and the autodocs give no way to place
   it. So: BuildEasyRequestArgs(), ChangeWindowBox() to the middle, then the
   input loop from the SysReqHandler() autodoc. All V36. Returns what
   EasyRequestArgs() would. */
static LONG CentredRequest(struct EasyStruct *es, APTR args)
{
    struct Window *w = BuildEasyRequestArgs(NULL, es, 0, args);
    if(((IPTR) w) <= 1)
    {
        return((LONG) (IPTR) w);   /* no window: 0 or 1 is the answer (autodoc) */
    }
    struct Screen *s = w->WScreen;
    LONG left = (s->Width - w->Width) / 2;
    LONG top = (s->Height - w->Height) / 2;
    ChangeWindowBox(w, (left > 0) ? left : 0, (top > 0) ? top : 0, w->Width, w->Height);
    LONG res;
    /* -2: not answered yet; -1: an extra IDCMP event, and we asked for none */
    while((res = SysReqHandler(w, NULL, TRUE)) < 0)
    {
    }
    FreeSysRequest(w);
    return(res);
}
/* \\\ */

/* /// "ShowReq()" */
static void ShowReq(CONST_STRPTR body, CONST_STRPTR arg1, CONST_STRPTR arg2)
{
    struct EasyStruct es = { sizeof(struct EasyStruct), 0,
                             (STRPTR) "USBEject", (STRPTR) body, (STRPTR) _(MSG_REQ_OK) };
    IPTR args[2];
    args[0] = (IPTR) arg1;
    args[1] = (IPTR) arg2;
    CentredRequest(&es, (APTR) args);
}
/* \\\ */

/* /// "DoEject()" */
static void DoEject(ULONG id)
{
    struct EjectDev *ed = (struct EjectDev *) devlist.lh_Head;
    while(ed->ed_Node.ln_Succ)
    {
        if(ed->ed_Id == id)
        {
            break;
        }
        ed = (struct EjectDev *) ed->ed_Node.ln_Succ;
    }
    if(!ed->ed_Node.ln_Succ)
    {
        return;   /* stale menu message - device already gone */
    }

    APTR pd = ed->ed_Device;
    STRPTR devname = ed->ed_Name;   /* the node outlives us: MenuSync runs in the main loop */

    /* The pd is only a key: make sure it is still on the bus before using it
       (menu messages can outlive an unplug). */
    BOOL valid = FALSE;
    psdLockReadPBase();
    APTR chk = NULL;
    while((chk = psdGetNextDevice(chk)) && !valid)
    {
        valid = (chk == pd);
    }
    psdUnlockPBase();
    if(!valid)
    {
        dirty = TRUE;
        return;
    }

    char busydrive[128];
    busydrive[0] = 0;
    if(CloseDOpusListers(pd, busydrive, sizeof(busydrive)) != RETURN_OK)
    {
        ShowReq(_(MSG_REQ_DOPUSBUSY), devname, busydrive);
        dirty = TRUE;
        return;
    }

    char busyname[64];
    busyname[0] = 0;
    IPTR res = psdSafeEjectDevice(pd, busyname, sizeof(busyname));

    switch(res)
    {
        case SAFEEJECT_OK:
        {
            /* the port disable runs in the hub task, so EHMB_REMDEVICE is the
               confirmation that the device is really gone */
            BOOL removed = FALSE;
            TimerStart(DETACH_SECS);
            while(!removed && !TimerFired())
            {
                Wait((1UL << eventport->mp_SigBit) | (1UL << timerport->mp_SigBit));
                removed = DrainEvents(pd);
            }
            TimerStop();
            ShowReq(removed ? _(MSG_REQ_SAFE) : _(MSG_REQ_NODETACH), devname, NULL);
            break;
        }

        case SAFEEJECT_BUSY:
            ShowReq(_(MSG_REQ_BUSY), devname, busyname);
            break;

        case SAFEEJECT_NOT_SUPPORTED:
            ShowReq(_(MSG_REQ_TOOOLD), devname, NULL);
            break;

        default:
            ShowReq(_(MSG_REQ_FAILED), devname, NULL);
            break;
    }
    dirty = TRUE;
}
/* \\\ */

/* /// "One-shot eject" */
/* "USBEject DEVICE=UMSD0:" - from a Shell, or a Directory Opus filetype on the
   disk icon: eject the USB device that holds that DOS device (or volume), then
   exit. No menu, no single-instance port, so it runs next to the daemon.
   The DOS device must sit on usbscsi.device. Opening that unit hands back
   massstorage's unit structure in io_Unit (classes/massstorage/dev.c devOpen),
   and for LUN 0 that is the interface binding Neptune reports as IFA_Binding
   (the class's attach returns its first unit). The other LUNs of a card reader
   do not match: refuse rather than eject a guess. */

/* The usbscsi.device unit behind a DOS device or volume name ("UMSD0:",
   "GOTEK:"). FALSE with the reason in why. */
static BOOL DOSUnit(CONST_STRPTR name, ULONG *unitp, STRPTR why, ULONG whylen)
{
    char dname[32];
    ULONG len = 0;
    while(name[len] && (name[len] != ':') && (len < sizeof(dname) - 1))
    {
        dname[len] = name[len];
        len++;
    }
    dname[len] = 0;

    BOOL ok = FALSE;
    ULONG mounted = 0;
    const ULONG lflags = LDF_DEVICES|LDF_VOLUMES|LDF_READ;
    struct DosList *dl = LockDosList(lflags);
    struct DosList *dev = FindDosEntry(dl, (STRPTR) dname, LDF_DEVICES);
    if(!dev)
    {
        /* A volume: its device is the one served by the same handler port.
           FindDosEntry() would take the first node of that name, which may be
           a node left behind with no handler (fat95 leaves one on inhibit,
           measured) or the other of two sticks with the same label. So walk
           them all and accept exactly one mounted volume of that name. */
        struct DosList *vol = NULL;
        struct DosList *v = dl;
        while((v = NextDosEntry(v, LDF_VOLUMES)))
        {
            UBYTE *vn = (UBYTE *) BADDR(v->dol_Name);
            ULONG vlen = vn ? vn[0] : 0;
            if(vlen && !vn[vlen])   /* a closing NUL may be counted, as in fssm_Device */
            {
                vlen--;
            }
            BOOL same = (vn && (vlen == len));
            for(ULONG i = 0; same && (i < len); i++)
            {
                /* ASCII and Latin-1 letters, upper and lower case alike */
                UBYTE a = vn[i + 1];
                UBYTE b = (UBYTE) dname[i];
                if(((a >= 'a') && (a <= 'z')) || ((a >= 0xe0) && (a <= 0xfe) && (a != 0xf7)))
                {
                    a -= 0x20;
                }
                if(((b >= 'a') && (b <= 'z')) || ((b >= 0xe0) && (b <= 0xfe) && (b != 0xf7)))
                {
                    b -= 0x20;
                }
                same = (a == b);
            }
            if(same && v->dol_Task)
            {
                vol = v;
                mounted++;
            }
        }
        if(mounted == 1)
        {
            struct DosList *d = dl;
            while((d = NextDosEntry(d, LDF_DEVICES)))
            {
                if(d->dol_Task == vol->dol_Task)
                {
                    dev = d;
                    break;
                }
            }
        }
    }
    if(!dev)
    {
        if(mounted > 1)
        {
            psdSafeRawDoFmt(why, whylen, "%s: %ld mounted volumes have this name - use the device name, e.g. UMSD0:",
                            dname, mounted);
        } else {
            psdSafeRawDoFmt(why, whylen, "%s: no such device or volume", dname);
        }
    } else {
        /* dol_Startup is not always a FileSysStartupMsg: check before use */
        struct FileSysStartupMsg *fssm = (struct FileSysStartupMsg *) BADDR(dev->dol_misc.dol_handler.dol_Startup);
        struct DosEnvec *de = NULL;
        UBYTE *bdev = NULL;
        if(fssm && TypeOfMem(fssm) && (*(UBYTE *) fssm == 0))
        {
            de = (struct DosEnvec *) BADDR(fssm->fssm_Environ);
            bdev = (UBYTE *) BADDR(fssm->fssm_Device);
        }
        if(de && TypeOfMem(de) && ((de->de_TableSize & 0xffffff00) == 0) && bdev && TypeOfMem(bdev))
        {
            static const char want[] = "usbscsi.device";
            /* the BSTR length often counts a closing NUL: measured 15 for
               massstorage's own mounts, and on other drivers' mountlists too */
            ULONG blen = bdev[0];
            if(blen && !bdev[blen])
            {
                blen--;
            }
            BOOL same = (blen == sizeof(want) - 1);
            for(ULONG i = 0; same && (i < sizeof(want) - 1); i++)
            {
                UBYTE c = bdev[i + 1];
                if((c >= 'A') && (c <= 'Z'))
                {
                    c += 'a' - 'A';
                }
                same = (c == (UBYTE) want[i]);
            }
            if(same)
            {
                *unitp = fssm->fssm_Unit;
                ok = TRUE;
            }
        }
        if(!ok)
        {
            psdSafeRawDoFmt(why, whylen, "%s: not a drive on usbscsi.device", dname);
        }
    }
    UnLockDosList(lflags);
    return(ok);
}

/* The Neptune device whose interface binding is massstorage's unit 'unit'
   (LUN 0 only, see above), NULL if none. Copies its product name to name. */
static APTR DeviceForUnit(ULONG unit, STRPTR name, ULONG namelen)
{
    APTR unitptr = NULL;
    struct MsgPort *mp = CreateMsgPort();
    struct IOStdReq *io = mp ? (struct IOStdReq *) CreateIORequest(mp, sizeof(struct IOStdReq)) : NULL;
    if(io && !OpenDevice("usbscsi.device", unit, (struct IORequest *) io, 0))
    {
        unitptr = io->io_Unit;
        /* closed before the eject: our own open must not count as a user */
        CloseDevice((struct IORequest *) io);
    }
    if(io)
    {
        DeleteIORequest((struct IORequest *) io);
    }
    if(mp)
    {
        DeleteMsgPort(mp);
    }
    if(!unitptr)
    {
        return(NULL);
    }

    APTR found = NULL;
    psdLockReadPBase();
    APTR pd = NULL;
    while(!found && (pd = psdGetNextDevice(pd)))
    {
        struct List *cfgs = NULL;
        psdGetAttrs(PGA_DEVICE, pd, DA_ConfigList, &cfgs, TAG_END);
        struct Node *pc = cfgs ? cfgs->lh_Head : NULL;
        while(!found && pc && pc->ln_Succ)
        {
            struct List *ifs = NULL;
            psdGetAttrs(PGA_CONFIG, pc, CA_InterfaceList, &ifs, TAG_END);
            struct Node *pif = ifs ? ifs->lh_Head : NULL;
            while(!found && pif && pif->ln_Succ)
            {
                IPTR binding = 0;
                psdGetAttrs(PGA_INTERFACE, pif, IFA_Binding, &binding, TAG_END);
                if(binding && ((APTR) binding == unitptr))
                {
                    found = pd;
                }
                pif = pif->ln_Succ;
            }
            pc = pc->ln_Succ;
        }
    }
    if(found)
    {
        STRPTR prodname = NULL;
        psdGetAttrs(PGA_DEVICE, found, DA_ProductName, &prodname, TAG_END);
        psdSafeRawDoFmt(name, namelen, "%s",
                        (prodname && *prodname) ? prodname : (STRPTR) _(MSG_FALLBACK_NAME));
    }
    psdUnlockPBase();
    return(found);
}

/* /// "Directory Opus listers" */
/* Neptune refuses an eject while anything holds a lock on a volume of the
   device (massstorage nInhibitAll: the handler leaves its locks parked), and
   Directory Opus 5 holds one for every lister that shows a drive - it opens a
   lister by itself when a stick appears. So before the eject we ask Directory
   Opus to close its listers on the drive, through its ARexx port, with its
   internal command CloseListers (Directory Opus CaffeineOS Edition, given
   all the drive's DOS device names in one call): it closes every lister whose
   current directory is on one of them, or on another DOS device of the same
   exec device and unit, waits until they have let go, and closes none at all
   if one of them is busy - all or none, as the eject itself. ARexx "lister
   close" will not do: it returns before the lister has let go, and it aborts
   the job of a busy lister.
   The eject takes every DOS device on every unit of the USB device, so every
   one of them is named: the usbscsi.device units of the device, and the
   virtual drives on usbscsivd.device (unit = parent unit * 10 + slot, mounter
   vdrive.h VDRIVE_PARENT). A unit whose interface binding is not the device
   (the other LUNs of a card reader, see "One-shot eject") cannot be matched,
   so listers there stay open and the eject then names their volume as in
   use.
   Only CloseListers' result 5 (a lister is busy, nothing closed) stops the
   eject. Anything else goes on to the eject, which refuses if a lock is still
   held: 1 done (also: nothing to close, names unknown, or an older Directory
   Opus that has no CloseListers - it answers an unknown internal command with
   1 too), 10 a lister still held a lock after 5 s, 0 the function did not run
   to its end, no result, or no Directory Opus at all. Directory Opus takes the
   first free port name DOPUS.1, DOPUS.2, ... and each copy has listers of its
   own, so every copy that is running is asked. */

#define DOPUS_PORTFMT     "DOPUS.%ld"
#define DOPUS_PORTMAX     99   /* the range Directory Opus names its ports from */
#define DOPUS_RC_BUSY     5    /* CloseListers: a lister on one of the drives is busy, nothing closed */
#define DOPUS_MAXDRIVES   24
#define VDRIVE_SLOTS      10                        /* mounter vdrive.h */

/* TRUE if the BSTR names 'want' (lower case), ignoring case and a closing NUL
   that the length may count (measured 15 for massstorage's own mounts) */
static BOOL BstrIs(const UBYTE *bstr, CONST_STRPTR want)
{
    ULONG wlen = 0;
    while(want[wlen])
    {
        wlen++;
    }
    ULONG blen = bstr[0];
    if(blen && !bstr[blen])
    {
        blen--;
    }
    if(blen != wlen)
    {
        return(FALSE);
    }
    for(ULONG i = 0; i < wlen; i++)
    {
        UBYTE c = bstr[i + 1];
        if((c >= 'A') && (c <= 'Z'))
        {
            c += 'a' - 'A';
        }
        if(c != (UBYTE) want[i])
        {
            return(FALSE);
        }
    }
    return(TRUE);
}

struct DOpusDrive
{
    char  dd_Name[32];   /* "UMSD0:" */
    ULONG dd_Parent;     /* the usbscsi.device unit it rides on */
};

/* The DOS devices with a running handler on usbscsi.device or usbscsivd.device,
   with the usbscsi.device unit each rides on. Nothing is sent while the list is
   locked. Return: how many went into 'out'. */
static ULONG CollectUSBDrives(struct DOpusDrive *out, ULONG max)
{
    ULONG count = 0;
    struct DosList *dl = LockDosList(LDF_DEVICES|LDF_READ);
    while((count < max) && (dl = NextDosEntry(dl, LDF_DEVICES)))
    {
        if(!dl->dol_Task)
        {
            continue;   /* handler never started: it holds no locks */
        }
        /* dol_Startup is not always a FileSysStartupMsg: check before use */
        struct FileSysStartupMsg *fssm = (struct FileSysStartupMsg *) BADDR(dl->dol_misc.dol_handler.dol_Startup);
        if(!fssm || !TypeOfMem(fssm) || (*(UBYTE *) fssm != 0))
        {
            continue;
        }
        struct DosEnvec *de = (struct DosEnvec *) BADDR(fssm->fssm_Environ);
        UBYTE *bdev = (UBYTE *) BADDR(fssm->fssm_Device);
        if(!de || !TypeOfMem(de) || (de->de_TableSize & 0xffffff00) || !bdev || !TypeOfMem(bdev))
        {
            continue;
        }
        ULONG parent;
        if(BstrIs(bdev, "usbscsi.device"))
        {
            parent = fssm->fssm_Unit;
        }
        else if(BstrIs(bdev, "usbscsivd.device"))
        {
            parent = fssm->fssm_Unit / VDRIVE_SLOTS;
        } else {
            continue;
        }
        UBYTE *bname = (UBYTE *) BADDR(dl->dol_Name);
        ULONG nlen = bname ? bname[0] : 0;
        if(nlen && !bname[nlen])
        {
            nlen--;
        }
        if(!nlen || (nlen > sizeof(out[count].dd_Name) - 2))
        {
            continue;
        }
        for(ULONG i = 0; i < nlen; i++)
        {
            out[count].dd_Name[i] = bname[i + 1];
        }
        out[count].dd_Name[nlen] = ':';
        out[count].dd_Name[nlen + 1] = 0;
        out[count].dd_Parent = parent;
        count++;
    }
    UnLockDosList(LDF_DEVICES|LDF_READ);
    return(count);
}

/* Send one command to Directory Opus's ARexx port and wait for the reply.
   Directory Opus answers "command wait ..." with rm_Result1 0 and the
   function's return code as a decimal string in rm_Result2 - only if the
   message asks for a result (RXFF_RESULT; Directory Opus rexx_proc.c).
   Return: that code; -1 if Directory Opus is not running or the message
   could not be built; -2 if it refused the command (rm_Result1 != 0, then
   rm_Result2 is an error number, not a string). */
static LONG DOpusCommand(CONST_STRPTR portname, struct MsgPort *reply, CONST_STRPTR cmd)
{
    LONG rc = -1;
    ULONG len = 0;
    while(cmd[len])
    {
        len++;
    }
    struct RexxMsg *rm = CreateRexxMsg(reply, NULL, NULL);
    if(!rm)
    {
        return(rc);
    }
    rm->rm_Args[0] = (IPTR) CreateArgstring((STRPTR) cmd, len);
    rm->rm_Action = RXCOMM | RXFF_RESULT;
    if(rm->rm_Args[0])
    {
        /* found and sent under Forbid(): the port may go away with Directory Opus */
        Forbid();
        struct MsgPort *port = FindPort((STRPTR) portname);
        if(port)
        {
            PutMsg(port, (struct Message *) rm);
        }
        Permit();
        if(port)
        {
            WaitPort(reply);
            GetMsg(reply);
            if(rm->rm_Result1)
            {
                rc = -2;
            }
            else if(rm->rm_Result2)
            {
                CONST_STRPTR s = (CONST_STRPTR) rm->rm_Result2;
                BOOL neg = (*s == '-');
                if(neg)
                {
                    s++;
                }
                rc = 0;
                while((*s >= '0') && (*s <= '9'))
                {
                    rc = rc * 10 + (*s++ - '0');
                }
                if(neg)
                {
                    rc = -rc;
                }
                DeleteArgstring((UBYTE *) rm->rm_Result2);
            } else {
                rc = 0;
            }
        }
        DeleteArgstring((UBYTE *) rm->rm_Args[0]);
    }
    DeleteRexxMsg(rm);
    return(rc);
}

/* Ask every running Directory Opus to close its listers on all drives of 'pd'.
   Return: RETURN_OK go on with the eject (Directory Opus not running
   included), RETURN_WARN a lister on one of the drives is busy and nothing
   was closed; 'drive' then names the drives asked for. */
static LONG CloseDOpusListers(APTR pd, STRPTR drive, ULONG drivelen)
{
    /* nothing to do, and no usbscsi.device opens, without a Directory Opus */
    BOOL any = FALSE;
    for(LONG n = 1; !any && (n <= DOPUS_PORTMAX); n++)
    {
        char portname[12];
        psdSafeRawDoFmt(portname, sizeof(portname), DOPUS_PORTFMT, n);
        Forbid();
        any = (FindPort((STRPTR) portname) != NULL);
        Permit();
    }
    if(!any)
    {
        return(RETURN_OK);
    }
    struct DOpusDrive *drives = AllocVec(DOPUS_MAXDRIVES * sizeof(struct DOpusDrive), MEMF_CLEAR);
    ULONG cmdlen = 48 + DOPUS_MAXDRIVES * sizeof(drives[0].dd_Name);
    STRPTR cmd = drives ? AllocVec(cmdlen, MEMF_CLEAR) : NULL;
    if(!cmd)
    {
        FreeVec(drives);
        return(RETURN_OK);   /* the eject itself still refuses if a lister holds a lock */
    }
    /* "command wait CloseListers DEVICE=UMSD0: UMSD1:" - every name ends with
       its colon, separated by blanks (template DEVICE/F) */
    static const char head[] = "command wait CloseListers DEVICE=";
    ULONG pos = sizeof(head) - 1;
    CopyMem((APTR) head, cmd, pos);
    ULONG named = 0;
    ULONG count = CollectUSBDrives(drives, DOPUS_MAXDRIVES);
    for(ULONG i = 0; i < count; i++)
    {
        char dummy[4];
        if(DeviceForUnit(drives[i].dd_Parent, dummy, sizeof(dummy)) != pd)
        {
            continue;
        }
        ULONG nlen = 0;
        while(drives[i].dd_Name[nlen])
        {
            nlen++;
        }
        if(pos + 1 + nlen + 1 > cmdlen)
        {
            break;
        }
        if(named)
        {
            cmd[pos++] = ' ';
        }
        CopyMem(drives[i].dd_Name, cmd + pos, nlen);
        pos += nlen;
        cmd[pos] = 0;
        named++;
    }
    FreeVec(drives);

    LONG result = RETURN_OK;
    struct MsgPort *reply = NULL;
    for(LONG n = 1; named && (n <= DOPUS_PORTMAX) && (result == RETURN_OK); n++)
    {
        char portname[12];
        psdSafeRawDoFmt(portname, sizeof(portname), DOPUS_PORTFMT, n);
        Forbid();
        BOOL running = (FindPort((STRPTR) portname) != NULL);
        Permit();
        if(!running)
        {
            continue;
        }
        if(!RexxLib && !(RexxLib = OpenLibrary("rexxsyslib.library", 36)))
        {
            break;
        }
        if(!reply && !(reply = CreateMsgPort()))
        {
            break;
        }
        LONG rc = DOpusCommand(portname, reply, cmd);
        if(rc == DOPUS_RC_BUSY)
        {
            psdSafeRawDoFmt(drive, drivelen, "%s", cmd + sizeof(head) - 1);
            psdAddErrorMsg(RETURN_WARN, LOGNAME,
                           "Not ejected: a Directory Opus lister on %s is busy (%s).",
                           cmd + sizeof(head) - 1, portname);
            result = RETURN_WARN;
        }
        else if(rc != 1)
        {
            psdAddErrorMsg(RETURN_WARN, LOGNAME,
                           "Directory Opus (%s) CloseListers on %s returned %ld; ejecting anyway.",
                           portname, cmd + sizeof(head) - 1, rc);
        }
    }
    if(reply)
    {
        DeleteMsgPort(reply);
    }
    if(RexxLib)
    {
        CloseLibrary(RexxLib);
        RexxLib = NULL;
    }
    FreeVec(cmd);
    return(result);
}
/* \\\ */

/* A message as a requester, or on the Shell with QUIET */
static void Tell(BOOL quiet, CONST_STRPTR body, CONST_STRPTR arg1, CONST_STRPTR arg2)
{
    if(quiet)
    {
        IPTR args[2];
        args[0] = (IPTR) arg1;
        args[1] = (IPTR) arg2;
        VPrintf((STRPTR) body, (APTR) args);
        PutStr("\n");
    } else {
        ShowReq(body, arg1, arg2);
    }
}

/* RETURN_OK ejected, RETURN_WARN still in use, RETURN_ERROR not a USB drive
   that can be found, RETURN_FAIL the eject failed. */
static LONG OneShotEject(CONST_STRPTR name, BOOL quiet)
{
    char why[128];
    char prodname[48];
    ULONG unit = 0;
    APTR pd = NULL;

    if(DOSUnit(name, &unit, why, sizeof(why)))
    {
        pd = DeviceForUnit(unit, prodname, sizeof(prodname));
        if(!pd)
        {
            psdSafeRawDoFmt(why, sizeof(why), "%s: no ejectable USB device found for usbscsi.device unit %ld",
                            (STRPTR) name, unit);
        }
    }
    if(!pd)
    {
        psdAddErrorMsg(RETURN_WARN, LOGNAME, "%s", why);
        if(quiet)
        {
            Tell(TRUE, "%s", why, NULL);
        } else {
            ShowReq(_(MSG_REQ_FAILED), name, NULL);
        }
        return(RETURN_ERROR);
    }

    char busydrive[128];
    busydrive[0] = 0;
    if(CloseDOpusListers(pd, busydrive, sizeof(busydrive)) != RETURN_OK)
    {
        Tell(quiet, _(MSG_REQ_DOPUSBUSY), prodname, busydrive);
        return(RETURN_WARN);
    }

    char busyname[64];
    busyname[0] = 0;
    IPTR res = psdSafeEjectDevice(pd, busyname, sizeof(busyname));
    switch(res)
    {
        case SAFEEJECT_OK:
        {
            /* the port disable runs in the hub task: EHMB_REMDEVICE confirms it */
            BOOL removed = FALSE;
            TimerStart(DETACH_SECS);
            while(!removed && !TimerFired())
            {
                Wait((1UL << eventport->mp_SigBit) | (1UL << timerport->mp_SigBit));
                removed = DrainEvents(pd);
            }
            TimerStop();
            Tell(quiet, removed ? _(MSG_REQ_SAFE) : _(MSG_REQ_NODETACH), prodname, NULL);
            return(RETURN_OK);
        }

        case SAFEEJECT_BUSY:
            Tell(quiet, _(MSG_REQ_BUSY), prodname, busyname);
            return(RETURN_WARN);

        case SAFEEJECT_NOT_SUPPORTED:
            Tell(quiet, _(MSG_REQ_TOOOLD), prodname, NULL);
            return(RETURN_FAIL);

        default:
            Tell(quiet, _(MSG_REQ_FAILED), prodname, NULL);
            return(RETURN_FAIL);
    }
}
/* \\\ */

/* /// "Cleanup()" */
static void Cleanup(void)
{
    if(WorkbenchBase)
    {
        MenuTeardown();
    }
    if(appport)
    {
        /* drain AppMessages that raced the removal (wb.doc requirement) */
        struct Message *msg;
        while((msg = GetMsg(appport)))
        {
            ReplyMsg(msg);
        }
        if(portadded)
        {
            RemPort(appport);
            portadded = FALSE;
        }
        DeleteMsgPort(appport);
        appport = NULL;
    }
    if(eventhandler)
    {
        psdRemEventHandler(eventhandler);   /* drains + replies queued notes */
        eventhandler = NULL;
    }
    if(timeropen)
    {
        TimerStop();
        CloseDevice((struct IORequest *) timerreq);
        timeropen = FALSE;
    }
    if(timerreq)
    {
        DeleteIORequest((struct IORequest *) timerreq);
        timerreq = NULL;
    }
    if(timerport)
    {
        DeleteMsgPort(timerport);
        timerport = NULL;
    }
    if(eventport)
    {
        DeleteMsgPort(eventport);
        eventport = NULL;
    }
    Locale_Deinitialize();
    if(rdargs)
    {
        FreeArgs(rdargs);
        rdargs = NULL;
    }
    if(LocaleBase)     { CloseLibrary((struct Library *) LocaleBase);    LocaleBase = NULL; }
    if(ps)             { CloseLibrary(ps);                              ps = NULL; }
    if(WorkbenchBase)  { CloseLibrary(WorkbenchBase);                   WorkbenchBase = NULL; }
    if(IntuitionBase)  { CloseLibrary((struct Library *) IntuitionBase); IntuitionBase = NULL; }
}
/* \\\ */

/* /// "main()" */
int main(int argc, char *argv[])
{
    NewList(&devlist);

    /* From a Shell, DEVICE selects the one-shot eject (see "One-shot eject");
       without it, or from Workbench (argc 0), this is the daemon as before. */
    IPTR oneshot[2] = { 0, 0 };   /* DEVICE, QUIET */
    if(argc > 0)
    {
        if(!(rdargs = ReadArgs((STRPTR) "DEVICE,QUIET/S", (LONG *) oneshot, NULL)))
        {
            PrintFault(IoErr(), (STRPTR) "USBEject");
            return(RETURN_FAIL);
        }
    }

    IntuitionBase = (struct IntuitionBase *) OpenLibrary("intuition.library", 39);
    /* AddAppMenuItemA() itself is V36. WBAPPMENUA_GetTitleKey is V45, but MenuSetup() probes for
       it rather than assuming it: a V40 workbench.library ignores the tag, leaves the key at zero,
       and we hang the eject items flat in the Tools menu -- so 36 is the honest floor. */
    WorkbenchBase = OpenLibrary("workbench.library", 36);
    LocaleBase    = (struct LocaleBase *) OpenLibrary("locale.library", 38);
    ps            = OpenLibrary(NEPTUNENAME, NEPTUNE_LIB_MIN_VERSION);
    if((!IntuitionBase) || (!WorkbenchBase) || (!ps))
    {
        Cleanup();
        return(RETURN_FAIL);
    }
    Locale_Initialize();

    if((!(appport = CreateMsgPort())) ||
       (!(eventport = CreateMsgPort())) ||
       (!(timerport = CreateMsgPort())) ||
       (!(timerreq = (struct timerequest *) CreateIORequest(timerport, sizeof(struct timerequest)))))
    {
        Cleanup();
        return(RETURN_FAIL);
    }
    if(OpenDevice("timer.device", UNIT_VBLANK, (struct IORequest *) timerreq, 0))
    {
        Cleanup();
        return(RETURN_FAIL);
    }
    timeropen = TRUE;

    if(oneshot[0])
    {
        /* no menu and no named port: it runs next to the daemon */
        eventhandler = psdAddEventHandler(eventport, EHMF_REMDEVICE);
        LONG rc = OneShotEject((CONST_STRPTR) oneshot[0], oneshot[1] ? TRUE : FALSE);
        Cleanup();
        return(rc);
    }

    /* Single instance: the named AppMessage port is the guard, so claiming the
       name and publishing it have to be one atomic step. */
    appport->mp_Node.ln_Name = (STRPTR) PORTNAME;
    Forbid();
    if(FindPort((STRPTR) PORTNAME))
    {
        Permit();
        Cleanup();
        return(RETURN_WARN);
    }
    AddPort(appport);
    portadded = TRUE;
    Permit();

    eventhandler = psdAddEventHandler(eventport, EHMF_ADDBINDING|EHMF_REMBINDING|EHMF_REMDEVICE);

    psdAddErrorMsg(RETURN_OK, LOGNAME, "Safe-eject menu handler started.");
    MenuSync();

    for(;;)
    {
        ULONG sigs = Wait((1UL << appport->mp_SigBit) |
                          (1UL << eventport->mp_SigBit) |
                          (1UL << timerport->mp_SigBit) |
                          SIGBREAKF_CTRL_C);
        if(sigs & SIGBREAKF_CTRL_C)
        {
            break;
        }
        DrainEvents(NULL);
        if(TimerFired())
        {
            dirty = TRUE;   /* time to retry the menu */
        }
        struct AppMessage *amsg;
        while((amsg = (struct AppMessage *) GetMsg(appport)))
        {
            /* our ids start at 1, so 0 means "not one of our menu items" */
            ULONG id = (amsg->am_Type == AMTYPE_APPMENUITEM) ? amsg->am_ID : 0;
            ReplyMsg((struct Message *) amsg);   /* reply BEFORE the long eject */
            if(id)
            {
                DoEject(id);
            }
        }
        if(dirty)
        {
            dirty = FALSE;
            MenuSync();
        }
    }

    psdAddErrorMsg(RETURN_OK, LOGNAME, "Safe-eject menu handler stopped.");
    Cleanup();
    return(RETURN_OK);
}
/* \\\ */
