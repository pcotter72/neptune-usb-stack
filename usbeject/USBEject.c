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

/* /// "ShowReq()" */
static void ShowReq(CONST_STRPTR body, CONST_STRPTR arg1, CONST_STRPTR arg2)
{
    struct EasyStruct es = { sizeof(struct EasyStruct), 0,
                             (STRPTR) "USBEject", (STRPTR) body, (STRPTR) _(MSG_REQ_OK) };
    IPTR args[2];
    args[0] = (IPTR) arg1;
    args[1] = (IPTR) arg2;
    EasyRequestArgs(NULL, &es, NULL, (APTR) args);
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
    const ULONG lflags = LDF_DEVICES|LDF_VOLUMES|LDF_READ;
    struct DosList *dl = LockDosList(lflags);
    struct DosList *dev = FindDosEntry(dl, (STRPTR) dname, LDF_DEVICES);
    if(!dev)
    {
        /* a volume: its device is the one served by the same handler port */
        struct DosList *vol = FindDosEntry(dl, (STRPTR) dname, LDF_VOLUMES);
        if(vol && vol->dol_Task)
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
        psdSafeRawDoFmt(why, whylen, "%s: no such device or volume", dname);
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
    char why[96];
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
