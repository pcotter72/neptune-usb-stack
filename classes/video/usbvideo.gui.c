/*
 *----------------------------------------------------------------------------
 *            usbvideo class: the settings window of a camera
 *----------------------------------------------------------------------------
 * Opened from Trident for one camera (UCM_OpenBindingCfgWindow); a task of
 * its own, like every window of a class.
 *
 * The window knows no control by itself. It asks for the camera's controls
 * the way any program does (NSCMD_UV_NEXTCONTROL, <devices/usbvideodevice.h>)
 * and is told which there are, what each is called and what it can do right
 * now. From that the pages are built when the window opens:
 *
 *   Picture   the controls of the picture
 *   Camera    the controls of the camera
 *   Mode      the default mode of each stream
 *
 * A control acts at once - the window is a remote control, the values are
 * the camera's. It is a client of the binding's subtask as an application is
 * one of the device: its requests are the device's commands for controls,
 * sent as messages to the subtask's command port, and one NSCMD_UV_WAITEVENT
 * of its own waits in the binding's queue. It never goes through the device,
 * so it works while a program has the unit open and streams.
 *
 * Save and Use store the values the camera then has, and the default modes,
 * in the library's settings for this device; the subtask applies them at
 * every bind (nApplyConfig). Cancel writes back what the camera had when the
 * window opened.
 */

#include "debug.h"
#include "usbvideo.class.h"

#include <libraries/mui.h>
#include <libraries/gadtools.h>

/* ROM-safe MUI base: this task carries the binding in tc_UserData; the
   shared accessor reaches ncv_MUIBase through it. See classes/mui_base.h. */
#define MUI_BASE_USERDATA struct NepClassVideo
#define MUI_BASE_FIELD    ncv_MUIBase
#include "mui_base.h"

static const STRPTR libname = CLASS_NAME;

#define GUI_MAX_ITEMS    48   /* controls the window has room for */
#define GUI_MAX_CHOICES  8
#define GUI_MAX_STREAMS  4    /* streams of one camera the Mode page has room for */
#define GUI_MAX_RATE     120  /* pictures per second, the top of the slider */
#define GUI_BUSY_TRIES   25   /* a control that is still moving is asked again this often... */
#define GUI_BUSY_DELAY   20   /* ...every so many milliseconds */
#define GUI_WRITE_PASSES 2    /* several controls are written in two passes (nGuiWritePass()) */

#define ID_SAVE          1
#define ID_USE           2
#define ID_DEFAULTS      3
#define ID_ABOUT         4
#define ID_ITEM          0x1000 /* + the number of the control */

/* One value on the Picture or the Camera page: a control of the camera */
struct UvcGuiItem
{
    struct UsbVideoControl ugi_Ctl;       /* what the camera said of it when it was last asked;
                                             its value is what the camera has, as far as the
                                             window knows */
    Object             *ugi_Obj;          /* slider, checkmark or cycle */
    UBYTE               ugi_Choices[GUI_MAX_CHOICES];        /* cycle entry -> number of the choice */
    STRPTR              ugi_ChoiceNames[GUI_MAX_CHOICES + 1]; /* the cycle's entries */
};

/* The default mode of one stream on the Mode page */
struct UvcGuiMode
{
    struct NepVideoStream *ugm_Unit;
    struct UvcFormat   *ugm_Format;       /* the format a request without one gets */
    Object             *ugm_SizeObj;      /* cycle: the camera's choice, then its picture sizes */
    Object             *ugm_RateObj;      /* slider: pictures per second, 0 = the camera's choice */
    STRPTR             *ugm_SizeNames;    /* the cycle's entries; all but the first are allocated */
};

struct UvcGui
{
    struct NepClassVideo *ug_Binding;
    struct Library     *ug_PsdBase;
    struct Library     *ug_IntBase;
    struct MsgPort     *ug_ReqPort;       /* the subtask's replies to requests */
    struct MsgPort     *ug_EventPort;     /* ...and the event that was waited for */
    struct IOStdReq     ug_Req;
    struct IOStdReq     ug_EventReq;
    BOOL                ug_EventWaits;    /* in the binding's queue, or on its way back */
    UBYTE               ug_EventBuf[32];
    struct UvcGuiItem  *ug_Items;
    ULONG               ug_NumItems;
    struct UvcGuiMode   ug_Modes[GUI_MAX_STREAMS];
    ULONG               ug_NumModes;
    struct UvcCfgChunk *ug_Before;        /* the control values when the window opened */

    Object             *ug_App;
    Object             *ug_Window;
    Object             *ug_Pages[2];      /* Picture, Camera */
    Object             *ug_ModePage;
    Object             *ug_StatusObj;
    Object             *ug_SaveObj;
    Object             *ug_UseObj;
    Object             *ug_DefaultsObj;
    Object             *ug_CancelObj;
    Object             *ug_AboutMI;
    Object             *ug_MUIPrefsMI;
    UBYTE               ug_Status[100];
};

static const char *const GuiPages[] = { "Picture", "Camera", "Mode", NULL };

/* /// "nOpenBindingCfgWindow()" */
/* Open the settings window of a camera: a task of its own, at most one per
   binding. FALSE = it is open already, or the task could not be started. */
LONG nOpenBindingCfgWindow(struct NepVideoBase *nh, struct NepClassVideo *ncv)
{
    struct Library *ps;

    KPRINTF(10, ("Opening GUI...\n"));
    if(!(ps = OpenLibrary(NEPTUNENAME, NEPTUNE_LIB_MIN_VERSION)))
    {
        return FALSE;
    }
    Forbid();
    if(!ncv->ncv_GUITask)
    {
        if((ncv->ncv_GUITask = psdSpawnSubTask(CLASS_NAME " GUI", nGUITask, ncv)))
        {
            Permit();
            CloseLibrary(ps);
            return TRUE;
        }
    }
    Permit();
    CloseLibrary(ps);
    return FALSE;
}
/* \\\ */

/**************************************************************************/

#undef ps
#define ps gui->ug_PsdBase
#undef IntuitionBase
#define IntuitionBase gui->ug_IntBase

/* /// "nGuiRequest()" */
/* One command to the binding's subtask, done before this returns. The
   subtask is there for as long as the window is: a release ends the window
   first. */
static LONG nGuiRequest(struct UvcGui *gui, ULONG command, APTR data, ULONG len)
{
    struct IOStdReq *ioreq = &gui->ug_Req;

    ioreq->io_Message.mn_Node.ln_Type = NT_MESSAGE;
    ioreq->io_Command = command;
    ioreq->io_Data = data;
    ioreq->io_Length = len;
    ioreq->io_Actual = 0;
    ioreq->io_Error = 0;
    PutMsg(gui->ug_Binding->ncv_CmdPort, &ioreq->io_Message);
    WaitPort(gui->ug_ReqPort);
    GetMsg(gui->ug_ReqPort);
    return ioreq->io_Error;
}
/* \\\ */

/* /// "nGuiReason()" */
/* Why the camera did not take a value, in words; reason is its error code
   (UVREC_ in <devices/usb_video.h>) */
static const char * nGuiReason(ULONG reason)
{
    switch(reason)
    {
        case UVREC_NOT_READY:       return "the camera is not ready for it";
        case UVREC_WRONG_STATE:     return "not in the state the camera is in";
        case UVREC_POWER:           return "not in this power mode";
        case UVREC_OUT_OF_RANGE:    return "the value is out of range";
        case UVREC_INVALID_UNIT:    return "the camera has no such unit";
        case UVREC_INVALID_CONTROL: return "the camera has no such control";
        case UVREC_INVALID_REQUEST: return "the camera does not know the request";
        case UVREC_INVALID_VALUE:   return "the camera takes no such value";
    }
    /* the camera gave no reason, or one that is not in the list */
    return "it could not be set";
}
/* \\\ */

/* /// "nGuiStatus()" */
/* The line under the pages. With a name: that control was not set, and why
   the camera says so. Without one: the line is cleared. */
static void nGuiStatus(struct UvcGui *gui, STRPTR name, ULONG reason)
{
    gui->ug_Status[0] = 0;
    if(name)
    {
        psdSafeRawDoFmt(gui->ug_Status, sizeof(gui->ug_Status), "%s: %s.", name, (STRPTR) nGuiReason(reason));
    }
    set(gui->ug_StatusObj, MUIA_Text_Contents, (IPTR) gui->ug_Status);
}
/* \\\ */

/* /// "nGuiChoiceEntry()" */
/* The cycle entry that stands for a choice; the first if none does */
static ULONG nGuiChoiceEntry(const struct UvcGuiItem *item, LONG value)
{
    ULONG entry = 0;

    while(item->ugi_ChoiceNames[entry] && (item->ugi_Choices[entry] != value))
    {
        entry++;
    }
    return item->ugi_ChoiceNames[entry] ? entry : 0;
}
/* \\\ */

/* /// "nGuiItemInit()" */
/* Can the window show a control? Not one the camera does not let be read,
   and not a number whose range the camera does not name. Of a choice the
   cycle's entries are made here: the choices the camera has. */
static BOOL nGuiItemInit(struct UvcGuiItem *item)
{
    const struct UsbVideoControl *uvc = &item->ugi_Ctl;
    ULONG entries = 0;

    if(!(uvc->uvc_Flags & UVCF_GET))
    {
        return FALSE;
    }
    switch(uvc->uvc_Kind)
    {
        case UVCK_RANGE:
            return (uvc->uvc_Flags & UVCF_RANGE) && (uvc->uvc_Min < uvc->uvc_Max);

        case UVCK_SWITCH:
            return TRUE;

        case UVCK_CHOICE:
            for(ULONG choice = 0; (entries < GUI_MAX_CHOICES) && uvc->uvc_ChoiceNames[choice]; choice++)
            {
                if(uvc->uvc_Choices & (1UL << choice))
                {
                    item->ugi_Choices[entries] = choice;
                    item->ugi_ChoiceNames[entries++] = (STRPTR) uvc->uvc_ChoiceNames[choice];
                }
            }
            return entries > 0;
    }
    return FALSE; /* a kind of control from after this window */
}
/* \\\ */

/* /// "nGuiItemShow()" */
/* What is known of a control into its gadget, without the gadget reporting
   it back as a change */
static void nGuiItemShow(struct UvcGui *gui, struct UvcGuiItem *item)
{
    const struct UsbVideoControl *uvc = &item->ugi_Ctl;

    switch(uvc->uvc_Kind)
    {
        case UVCK_RANGE:
            nnset(item->ugi_Obj, MUIA_Numeric_Value, uvc->uvc_Value);
            break;

        case UVCK_SWITCH:
            nnset(item->ugi_Obj, MUIA_Selected, uvc->uvc_Value != 0);
            break;

        case UVCK_CHOICE:
            nnset(item->ugi_Obj, MUIA_Cycle_Active, nGuiChoiceEntry(item, uvc->uvc_Value));
            break;
    }
    /* read only, or switched off by an automatic mode */
    set(item->ugi_Obj, MUIA_Disabled, (uvc->uvc_Flags & (UVCF_SET|UVCF_INACTIVE)) != UVCF_SET);
}
/* \\\ */

/* /// "nGuiItemWanted()" */
/* What the gadget says, as a value the camera takes */
static LONG nGuiItemWanted(struct UvcGui *gui, struct UvcGuiItem *item)
{
    const struct UsbVideoControl *uvc = &item->ugi_Ctl;
    IPTR raw = 0;
    LONG value;

    switch(uvc->uvc_Kind)
    {
        case UVCK_RANGE:
            /* the nearest value the camera has a step for */
            get(item->ugi_Obj, MUIA_Numeric_Value, &raw);
            value = uvc->uvc_Min + (((LONG) raw - uvc->uvc_Min + uvc->uvc_Step / 2) / uvc->uvc_Step) * uvc->uvc_Step;
            return (value > uvc->uvc_Max) ? value - uvc->uvc_Step : value;

        case UVCK_SWITCH:
            get(item->ugi_Obj, MUIA_Selected, &raw);
            return raw ? 1 : 0;
    }
    get(item->ugi_Obj, MUIA_Cycle_Active, &raw);
    return item->ugi_Choices[raw];
}
/* \\\ */

/* /// "nGuiItemRead()" */
/* A control as the camera has it now */
static void nGuiItemRead(struct UvcGui *gui, struct UvcGuiItem *item)
{
    nGuiRequest(gui, NSCMD_UV_GETCONTROL, &item->ugi_Ctl, sizeof(item->ugi_Ctl));
    nGuiItemShow(gui, item);
}

static void nGuiReadAll(struct UvcGui *gui)
{
    for(ULONG cnt = 0; cnt < gui->ug_NumItems; cnt++)
    {
        nGuiItemRead(gui, &gui->ug_Items[cnt]);
    }
}
/* \\\ */

/* /// "nGuiItemWrite()" */
/* A new value to the camera. A control that is still busy with the last one
   says so - a motor on its way - and is asked again for a while. FALSE = the
   camera did not take it; the line under the pages says why. */
static BOOL nGuiItemWrite(struct UvcGui *gui, struct UvcGuiItem *item, LONG value)
{
    struct UsbVideoControl *uvc = &item->ugi_Ctl;
    LONG was = uvc->uvc_Value;
    ULONG tries = 0;
    LONG err;

    uvc->uvc_Value = value;
    while((err = nGuiRequest(gui, NSCMD_UV_SETCONTROL, uvc, sizeof(*uvc))) &&
          (uvc->uvc_Reason == UVREC_NOT_READY) && (tries++ < GUI_BUSY_TRIES))
    {
        psdDelayMS(GUI_BUSY_DELAY);
    }
    if(err)
    {
        uvc->uvc_Value = was;
        nGuiStatus(gui, (STRPTR) uvc->uvc_Name, uvc->uvc_Reason);
        return FALSE;
    }
    nGuiStatus(gui, NULL, 0);
    return TRUE;
}
/* \\\ */

/* /// "nGuiItemChanged()" */
/* A gadget was moved. The latest value wins: what counts is where the gadget
   is now, not every place it has been through, and what the camera already
   has is not sent again. */
static void nGuiItemChanged(struct UvcGui *gui, struct UvcGuiItem *item)
{
    LONG value = nGuiItemWanted(gui, item);

    if(value != item->ugi_Ctl.uvc_Value)
    {
        if(!nGuiItemWrite(gui, item, value))
        {
            nGuiItemRead(gui, item);
            return;
        }
        if(item->ugi_Ctl.uvc_Flags & UVCF_GOVERNS)
        {
            /* an automatic mode: what the controls it governs can do is different now */
            nGuiReadAll(gui);
            return;
        }
    }
    /* a slider between two steps goes to the one that was sent */
    nGuiItemShow(gui, item);
}
/* \\\ */

/* /// "nGuiWritePass()" */
/* Whatever writes several controls does it in GUI_WRITE_PASSES passes over
   them: the automatic modes in the first, what they govern in the second.
   This is the pass a control is written in. */
static ULONG nGuiWritePass(const struct UvcGuiItem *item)
{
    return (item->ugi_Ctl.uvc_Flags & UVCF_GOVERNS) ? 0 : 1;
}
/* \\\ */

/* /// "nGuiDefaults()" */
/* Every control to the camera's own default: the automatic modes first, then
   what they leave to be set */
static void nGuiDefaults(struct UvcGui *gui)
{
    for(ULONG pass = 0; pass < GUI_WRITE_PASSES; pass++)
    {
        for(ULONG cnt = 0; cnt < gui->ug_NumItems; cnt++)
        {
            struct UvcGuiItem *item = &gui->ug_Items[cnt];
            const struct UsbVideoControl *uvc = &item->ugi_Ctl;

            if((uvc->uvc_Flags & UVCF_DEFAULT) && (nGuiWritePass(item) == pass) &&
               ((uvc->uvc_Flags & (UVCF_SET|UVCF_INACTIVE)) == UVCF_SET))
            {
                nGuiItemWrite(gui, item, uvc->uvc_Default);
            }
        }
        nGuiReadAll(gui);
    }
}
/* \\\ */

/* /// "nGuiSnapshot()" */
/* The values the window knows the camera to have, as records for
   nApplyControls(): every control that can be set, the automatic modes
   before the others. */
static struct UvcCfgChunk * nGuiSnapshot(struct UvcGui *gui)
{
    struct UvcCfgChunk *chunk = psdAllocVec(sizeof(struct UvcCfgChunk) + gui->ug_NumItems * sizeof(struct UvcCfgValue));

    if(!chunk)
    {
        return NULL;
    }
    struct UvcCfgValue *ucv = (struct UvcCfgValue *) chunk->ucc_Data;

    for(ULONG pass = 0; pass < GUI_WRITE_PASSES; pass++)
    {
        for(ULONG cnt = 0; cnt < gui->ug_NumItems; cnt++)
        {
            struct UvcGuiItem *item = &gui->ug_Items[cnt];
            const struct UsbVideoControl *uvc = &item->ugi_Ctl;

            if((nGuiWritePass(item) != pass) || !(uvc->uvc_Flags & UVCF_SET))
            {
                continue;
            }
            ucv->ucv_ID = uvc->uvc_ID;
            ucv->ucv_Entity = uvc->uvc_Entity;
            ucv->ucv_Value = uvc->uvc_Value;
            ucv++;
        }
    }
    chunk->ucc_ChunkID = AROS_LONG2BE(UVCFG_VALUES);
    chunk->ucc_Length = AROS_LONG2BE((ULONG) ((UBYTE *) ucv - chunk->ucc_Data));
    return chunk;
}
/* \\\ */

/* /// "nGuiStore()" */
/* Save and Use: the control values and the default modes into the library's
   settings for this device - written to disk or kept until the next reset -
   and the subtask is told to take them from there. */
static void nGuiStore(struct UvcGui *gui, BOOL todisk)
{
    struct NepClassVideo *ncv = gui->ug_Binding;

    /* an automatic mode may have moved what it governs since the window last looked */
    nGuiReadAll(gui);
    struct UvcCfgChunk *controls = nGuiSnapshot(gui);
    struct UvcCfgChunk *modes = psdAllocVec(sizeof(struct UvcCfgChunk) + GUI_MAX_STREAMS * sizeof(struct UvcCfgMode));
    struct PsdIFFContext *pic;

    if(controls && modes)
    {
        /* the chunk's data is a row of records, one for each stream */
        struct UvcCfgMode *records = (struct UvcCfgMode *) modes->ucc_Data;

        for(ULONG cnt = 0; cnt < gui->ug_NumModes; cnt++)
        {
            struct UvcGuiMode *ugm = &gui->ug_Modes[cnt];
            struct UvcCfgMode *ucm = &records[cnt];
            IPTR size = 0;
            IPTR rate = 0;

            get(ugm->ugm_SizeObj, MUIA_Cycle_Active, &size);
            get(ugm->ugm_RateObj, MUIA_Numeric_Value, &rate);
            ucm->ucm_IfNum = ugm->ugm_Unit->ns_IfNum;
            /* entry 0 is the camera's own choice; the others are its frames in their order */
            if(size)
            {
                ucm->ucm_Width = ugm->ugm_Format->ufo_Frames[size - 1].ufr_Width;
                ucm->ucm_Height = ugm->ugm_Format->ufo_Frames[size - 1].ufr_Height;
            }
            ucm->ucm_Interval = rate ? 10000000 / rate : 0;
        }
        modes->ucc_ChunkID = AROS_LONG2BE(UVCFG_MODES);
        modes->ucc_Length = AROS_LONG2BE(gui->ug_NumModes * sizeof(struct UvcCfgMode));

        pic = psdGetUsbDevCfg((STRPTR) libname, ncv->ncv_DevIDString, ncv->ncv_IfIDString);
        if(!pic)
        {
            psdSetUsbDevCfg((STRPTR) libname, ncv->ncv_DevIDString, ncv->ncv_IfIDString, NULL);
            pic = psdGetUsbDevCfg((STRPTR) libname, ncv->ncv_DevIDString, ncv->ncv_IfIDString);
        }
        if(pic && psdAddCfgEntry(pic, controls) && psdAddCfgEntry(pic, modes) && todisk)
        {
            psdSaveCfgToDisk(NULL);
        }
        Signal(ncv->ncv_Task, SIGBREAKF_CTRL_D);
    }
    psdFreeVec(controls);
    psdFreeVec(modes);
}
/* \\\ */

/* /// "nGuiEventWait()" */
/* The window's own wait for what the camera reports by itself */
static void nGuiEventWait(struct UvcGui *gui)
{
    struct IOStdReq *ioreq = &gui->ug_EventReq;

    ioreq->io_Command = NSCMD_UV_WAITEVENT;
    ioreq->io_Data = gui->ug_EventBuf;
    ioreq->io_Length = sizeof(gui->ug_EventBuf);
    ioreq->io_Actual = 0;
    ioreq->io_Error = 0;
    gui->ug_EventWaits = (uvcEventWait(gui->ug_Binding, ioreq) == RC_DONTREPLY);
}
/* \\\ */

/* /// "nGuiEvent()" */
/* The camera reported something: if it is about a control the window shows -
   an automatic mode moved it, or changed what it can do - show it as it is
   now. Then wait for the next. */
static void nGuiEvent(struct UvcGui *gui)
{
    const struct UsbVideoStatusVC *sp = (const struct UsbVideoStatusVC *) gui->ug_EventBuf;

    if(!GetMsg(gui->ug_EventPort))
    {
        return;
    }
    gui->ug_EventWaits = FALSE;
    if(gui->ug_EventReq.io_Error)
    {
        return; /* the camera has stopped reporting */
    }
    if((gui->ug_EventReq.io_Actual >= sizeof(*sp)) &&
       ((sp->bStatusType & UVSPT_MASK) == UVSPT_VIDEOCONTROL))
    {
        for(ULONG cnt = 0; cnt < gui->ug_NumItems; cnt++)
        {
            struct UvcGuiItem *item = &gui->ug_Items[cnt];

            if((item->ugi_Ctl.uvc_Entity == sp->bOriginator) &&
               (item->ugi_Ctl.uvc_Selector == sp->bSelector))
            {
                nGuiItemRead(gui, item);
            }
        }
    }
    nGuiEventWait(gui);
}
/* \\\ */

/* /// "nGuiAddItems()" */
/* The controls of the camera onto their pages. Asks for them one after the
   other and gives each one the window can show a label and a gadget. */
static void nGuiAddItems(struct UvcGui *gui)
{
    struct UsbVideoControl ctl;
    ULONG onpage[2] = { 0, 0 };

    if(!(gui->ug_Items = psdAllocVec(GUI_MAX_ITEMS * sizeof(struct UvcGuiItem))))
    {
        return;
    }
    memset(&ctl, 0, sizeof(ctl));
    while((gui->ug_NumItems < GUI_MAX_ITEMS) && !nGuiRequest(gui, NSCMD_UV_NEXTCONTROL, &ctl, sizeof(ctl)))
    {
        struct UvcGuiItem *item = &gui->ug_Items[gui->ug_NumItems];
        ULONG page = (UVCID_GROUP(ctl.uvc_ID) == UVCG_PICTURE) ? 0 : 1;
        Object *gadget = NULL;

        memset(item, 0, sizeof(*item));
        item->ugi_Ctl = ctl;
        if(!nGuiItemInit(item))
        {
            continue;
        }
        switch(ctl.uvc_Kind)
        {
            case UVCK_RANGE:
                gadget = item->ugi_Obj = SliderObject, SliderFrame,
                    MUIA_CycleChain, 1,
                    MUIA_Slider_Horiz, TRUE,
                    MUIA_Numeric_Min, ctl.uvc_Min,
                    MUIA_Numeric_Max, ctl.uvc_Max,
                    MUIA_Numeric_Value, ctl.uvc_Value,
                    End;
                break;

            case UVCK_SWITCH:
                gadget = HGroup,
                    Child, (IPTR)(item->ugi_Obj = ImageObject, ImageButtonFrame,
                        MUIA_Background, MUII_ButtonBack,
                        MUIA_CycleChain, 1,
                        MUIA_InputMode, MUIV_InputMode_Toggle,
                        MUIA_Image_Spec, MUII_CheckMark,
                        MUIA_Image_FreeVert, TRUE,
                        MUIA_Selected, ctl.uvc_Value != 0,
                        MUIA_ShowSelState, FALSE,
                        End),
                    Child, (IPTR)HSpace(0),
                    End;
                break;

            case UVCK_CHOICE:
                gadget = item->ugi_Obj = CycleObject,
                    MUIA_CycleChain, 1,
                    MUIA_Cycle_Entries, (IPTR)item->ugi_ChoiceNames,
                    MUIA_Cycle_Active, nGuiChoiceEntry(item, ctl.uvc_Value),
                    End;
                break;
        }
        if(!gadget || !item->ugi_Obj)
        {
            continue;
        }
        DoMethod(gui->ug_Pages[page], OM_ADDMEMBER, Label((IPTR) ctl.uvc_Name));
        DoMethod(gui->ug_Pages[page], OM_ADDMEMBER, gadget);
        DoMethod(item->ugi_Obj, MUIM_Notify,
                 (ctl.uvc_Kind == UVCK_RANGE) ? MUIA_Numeric_Value :
                 ((ctl.uvc_Kind == UVCK_SWITCH) ? MUIA_Selected : MUIA_Cycle_Active), MUIV_EveryTime,
                 gui->ug_App, 2, MUIM_Application_ReturnID, ID_ITEM + gui->ug_NumItems);
        nGuiItemShow(gui, item);
        gui->ug_NumItems++;
        onpage[page]++;
    }
    for(ULONG page = 0; page < 2; page++)
    {
        if(!onpage[page])
        {
            DoMethod(gui->ug_Pages[page], OM_ADDMEMBER, Label((IPTR) "This camera has no such controls."));
            DoMethod(gui->ug_Pages[page], OM_ADDMEMBER, HSpace(0));
        }
    }
}
/* \\\ */

/* /// "nGuiAddModes()" */
/* The Mode page: for every stream of the camera the size and the rate a
   program gets that asks for none */
static void nGuiAddModes(struct UvcGui *gui)
{
    struct NepClassVideo *ncv = gui->ug_Binding;
    struct NepVideoStream *ns;

    /* the units of this camera; they and what they point at stay while the binding does */
    Forbid();
    ns = (struct NepVideoStream *) ncv->ncv_ClsBase->nh_Units.lh_Head;
    while(ns->ns_Unit.unit_MsgPort.mp_Node.ln_Succ && (gui->ug_NumModes < GUI_MAX_STREAMS))
    {
        if(ns->ns_Binding == ncv)
        {
            gui->ug_Modes[gui->ug_NumModes++].ugm_Unit = ns;
        }
        ns = (struct NepVideoStream *) ns->ns_Unit.unit_MsgPort.mp_Node.ln_Succ;
    }
    Permit();

    for(ULONG cnt = 0; cnt < gui->ug_NumModes; cnt++)
    {
        struct UvcGuiMode *ugm = &gui->ug_Modes[cnt];
        ULONG active = 0;

        ns = ugm->ugm_Unit;
        ugm->ugm_Format = uvcFindFormat(ns->ns_Desc, 0);
        if(!ugm->ugm_Format ||
           !(ugm->ugm_SizeNames = psdAllocVec((ugm->ugm_Format->ufo_NumFrames + 2) * sizeof(STRPTR))))
        {
            gui->ug_NumModes = cnt; /* no further */
            break;
        }
        ugm->ugm_SizeNames[0] = (STRPTR) "The camera's choice";
        for(ULONG frame = 0; frame < ugm->ugm_Format->ufo_NumFrames; frame++)
        {
            struct UvcFrame *ufr = &ugm->ugm_Format->ufo_Frames[frame];

            ugm->ugm_SizeNames[frame + 1] = psdCopyStrFmt("%ld x %ld", (ULONG) ufr->ufr_Width, (ULONG) ufr->ufr_Height);
            if((ufr->ufr_Width == ns->ns_Default.ud_Width) && (ufr->ufr_Height == ns->ns_Default.ud_Height))
            {
                active = frame + 1;
            }
        }
        ugm->ugm_SizeObj = CycleObject,
            MUIA_CycleChain, 1,
            MUIA_Cycle_Entries, (IPTR)ugm->ugm_SizeNames,
            MUIA_Cycle_Active, active,
            End;
        ugm->ugm_RateObj = SliderObject, SliderFrame,
            MUIA_CycleChain, 1,
            MUIA_Slider_Horiz, TRUE,
            MUIA_Numeric_Min, 0,
            MUIA_Numeric_Max, GUI_MAX_RATE,
            MUIA_Numeric_Value, ns->ns_Default.ud_Interval
                                ? (10000000 + ns->ns_Default.ud_Interval / 2) / ns->ns_Default.ud_Interval : 0,
            End;
        if(!ugm->ugm_SizeObj || !ugm->ugm_RateObj)
        {
            gui->ug_NumModes = cnt;
            break;
        }
        DoMethod(gui->ug_ModePage, OM_ADDMEMBER, Label((IPTR) "Picture size:"));
        DoMethod(gui->ug_ModePage, OM_ADDMEMBER, ugm->ugm_SizeObj);
        DoMethod(gui->ug_ModePage, OM_ADDMEMBER, Label((IPTR) "Pictures per second:"));
        DoMethod(gui->ug_ModePage, OM_ADDMEMBER, ugm->ugm_RateObj);
    }
}
/* \\\ */

/* /// "nGuiCleanup()" */
/* The window's task ends: everything it took goes back */
static void nGuiCleanup(struct UvcGui *gui)
{
    struct NepClassVideo *ncv = gui->ug_Binding;

    if(gui->ug_EventWaits)
    {
        /* called back or on its way back: either way it arrives at the port */
        uvcEventAbort(ncv, &gui->ug_EventReq);
        WaitPort(gui->ug_EventPort);
        GetMsg(gui->ug_EventPort);
    }
    if(gui->ug_App)
    {
        MUI_DisposeObject(gui->ug_App);
    }
    if(ps)
    {
        for(ULONG cnt = 0; cnt < GUI_MAX_STREAMS; cnt++)
        {
            STRPTR *names = gui->ug_Modes[cnt].ugm_SizeNames;

            /* the first entry is not an allocated one */
            for(ULONG name = 1; names && names[name]; name++)
            {
                psdFreeVec(names[name]);
            }
            psdFreeVec(names);
        }
        psdFreeVec(gui->ug_Items);
        psdFreeVec(gui->ug_Before);
        CloseLibrary(ps);
    }
    if(gui->ug_EventPort)
    {
        DeleteMsgPort(gui->ug_EventPort);
    }
    if(gui->ug_ReqPort)
    {
        DeleteMsgPort(gui->ug_ReqPort);
    }
    if(IntuitionBase)
    {
        CloseLibrary(IntuitionBase);
    }
    if(ncv->ncv_MUIBase)
    {
        CloseLibrary(ncv->ncv_MUIBase);
        ncv->ncv_MUIBase = NULL;
    }
    /* stays forbidden until the task has ended: who waits for that must not
       find the class gone under it */
    Forbid();
    ncv->ncv_GUITask = NULL;
    if(ncv->ncv_ReadySigTask)
    {
        Signal(ncv->ncv_ReadySigTask, 1UL<<ncv->ncv_ReadySignal);
    }
    --ncv->ncv_ClsBase->nh_Library.lib_OpenCnt;
}
/* \\\ */

/* /// "nGUITask()" */
/* The task of a camera's settings window */
void nGUITask()
{
    struct Task *thistask = FindTask(NULL);
    struct NepClassVideo *ncv = thistask->tc_UserData;
    struct UvcGui guidata;
    struct UvcGui *gui = &guidata;
    STRPTR devname = NULL;

    memset(gui, 0, sizeof(*gui));
    gui->ug_Binding = ncv;
    ++ncv->ncv_ClsBase->nh_Library.lib_OpenCnt;

    if(!(ncv->ncv_MUIBase = OpenLibrary(MUIMASTER_NAME, MUIMASTER_VMIN)) ||
       !(IntuitionBase = OpenLibrary("intuition.library", 39)) ||
       !(ps = OpenLibrary(NEPTUNENAME, NEPTUNE_LIB_MIN_VERSION)) ||
       !(gui->ug_ReqPort = CreateMsgPort()) ||
       !(gui->ug_EventPort = CreateMsgPort()))
    {
        KPRINTF(10, ("Couldn't open what the window needs.\n"));
        nGuiCleanup(gui);
        return;
    }
    gui->ug_Req.io_Message.mn_ReplyPort = gui->ug_ReqPort;
    gui->ug_Req.io_Message.mn_Length = sizeof(struct IOStdReq);
    gui->ug_EventReq.io_Message.mn_ReplyPort = gui->ug_EventPort;
    gui->ug_EventReq.io_Message.mn_Length = sizeof(struct IOStdReq);
    psdGetAttrs(PGA_DEVICE, ncv->ncv_Device, DA_ProductName, &devname, TAG_END);

    gui->ug_App = ApplicationObject,
        MUIA_Application_Title      , (IPTR)libname,
        MUIA_Application_Version    , (IPTR)VERSION_STRING,
        MUIA_Application_Copyright  , (IPTR)NEPTUNE_DIST_NAME,
        MUIA_Application_Description, (IPTR)"Settings for a USB camera",
        MUIA_Application_Base       , (IPTR)"USBVIDEO",
        MUIA_Application_HelpFile   , (IPTR)"HELP:Poseidon.guide",
        MUIA_Application_Menustrip  , (IPTR)MenustripObject,
            Child, (IPTR)MenuObjectT((IPTR)"Project"),
                Child, (IPTR)(gui->ug_AboutMI = MenuitemObject,
                    MUIA_Menuitem_Title, (IPTR)"About...",
                    MUIA_Menuitem_Shortcut, (IPTR)"?",
                    End),
                End,
            Child, (IPTR)MenuObjectT((IPTR)"Settings"),
                Child, (IPTR)(gui->ug_MUIPrefsMI = MenuitemObject,
                    MUIA_Menuitem_Title, (IPTR)"MUI Settings",
                    MUIA_Menuitem_Shortcut, (IPTR)"M",
                    End),
                End,
            End,

        SubWindow, (IPTR)(gui->ug_Window = WindowObject,
            MUIA_Window_ID   , MAKE_ID('M','A','I','N'),
            MUIA_Window_Title, (IPTR)(devname ? devname : libname),
            MUIA_HelpNode, (IPTR)libname,

            WindowContents, (IPTR)VGroup,
                Child, (IPTR)RegisterGroup(GuiPages),
                    MUIA_CycleChain, 1,
                    Child, (IPTR)VGroup,
                        Child, (IPTR)(gui->ug_Pages[0] = ColGroup(2), End),
                        Child, (IPTR)VSpace(0),
                        End,
                    Child, (IPTR)VGroup,
                        Child, (IPTR)(gui->ug_Pages[1] = ColGroup(2), End),
                        Child, (IPTR)VSpace(0),
                        End,
                    Child, (IPTR)VGroup,
                        Child, (IPTR)(gui->ug_ModePage = ColGroup(2), End),
                        Child, (IPTR)TextObject,
                            MUIA_Text_Contents, (IPTR)"\33cWhat a program gets that asks for no size or rate of its own.\n"
                                                      "At 0 pictures per second the camera chooses.",
                            End,
                        Child, (IPTR)VSpace(0),
                        End,
                    End,
                Child, (IPTR)(gui->ug_StatusObj = TextObject, TextFrame,
                    MUIA_Background, MUII_TextBack,
                    MUIA_Text_Contents, (IPTR)"",
                    End),
                Child, (IPTR)HGroup,
                    MUIA_Group_SameWidth, TRUE,
                    Child, (IPTR)(gui->ug_SaveObj = TextObject, ButtonFrame,
                        MUIA_Background, MUII_ButtonBack,
                        MUIA_CycleChain, 1,
                        MUIA_InputMode, MUIV_InputMode_RelVerify,
                        MUIA_Text_Contents, (IPTR)"\33c Save ",
                        End),
                    Child, (IPTR)(gui->ug_UseObj = TextObject, ButtonFrame,
                        MUIA_Background, MUII_ButtonBack,
                        MUIA_CycleChain, 1,
                        MUIA_InputMode, MUIV_InputMode_RelVerify,
                        MUIA_Text_Contents, (IPTR)"\33c Use ",
                        End),
                    Child, (IPTR)(gui->ug_DefaultsObj = TextObject, ButtonFrame,
                        MUIA_Background, MUII_ButtonBack,
                        MUIA_CycleChain, 1,
                        MUIA_InputMode, MUIV_InputMode_RelVerify,
                        MUIA_Text_Contents, (IPTR)"\33c Camera defaults ",
                        End),
                    Child, (IPTR)(gui->ug_CancelObj = TextObject, ButtonFrame,
                        MUIA_Background, MUII_ButtonBack,
                        MUIA_CycleChain, 1,
                        MUIA_InputMode, MUIV_InputMode_RelVerify,
                        MUIA_Text_Contents, (IPTR)"\33c Cancel ",
                        End),
                    End,
                End,
            End),
        End;

    if(!gui->ug_App)
    {
        KPRINTF(10, ("Couldn't create application\n"));
        nGuiCleanup(gui);
        return;
    }

    /* what the camera has and what it is set to: asked now, shown as gadgets */
    nGuiAddItems(gui);
    nGuiAddModes(gui);
    gui->ug_Before = nGuiSnapshot(gui);
    nGuiEventWait(gui);

    DoMethod(gui->ug_Window, MUIM_Notify, MUIA_Window_CloseRequest, TRUE,
             gui->ug_App, 2, MUIM_Application_ReturnID, MUIV_Application_ReturnID_Quit);
    DoMethod(gui->ug_SaveObj, MUIM_Notify, MUIA_Pressed, FALSE,
             gui->ug_App, 2, MUIM_Application_ReturnID, ID_SAVE);
    DoMethod(gui->ug_UseObj, MUIM_Notify, MUIA_Pressed, FALSE,
             gui->ug_App, 2, MUIM_Application_ReturnID, ID_USE);
    DoMethod(gui->ug_DefaultsObj, MUIM_Notify, MUIA_Pressed, FALSE,
             gui->ug_App, 2, MUIM_Application_ReturnID, ID_DEFAULTS);
    DoMethod(gui->ug_CancelObj, MUIM_Notify, MUIA_Pressed, FALSE,
             gui->ug_App, 2, MUIM_Application_ReturnID, MUIV_Application_ReturnID_Quit);
    DoMethod(gui->ug_AboutMI, MUIM_Notify, MUIA_Menuitem_Trigger, MUIV_EveryTime,
             gui->ug_App, 2, MUIM_Application_ReturnID, ID_ABOUT);
    DoMethod(gui->ug_MUIPrefsMI, MUIM_Notify, MUIA_Menuitem_Trigger, MUIV_EveryTime,
             gui->ug_App, 2, MUIM_Application_OpenConfigWindow, 0);

    {
        ULONG eventmask = 1UL << gui->ug_EventPort->mp_SigBit;
        IPTR isopen = 0;
        IPTR iconify = 0;
        ULONG sigs = 0;
        BOOL done = FALSE;

        get(gui->ug_App, MUIA_Application_Iconified, &iconify);
        set(gui->ug_Window, MUIA_Window_Open, TRUE);
        get(gui->ug_Window, MUIA_Window_Open, &isopen);
        if(!(isopen || iconify))
        {
            nGuiCleanup(gui);
            return;
        }
        while(!done)
        {
            LONG retid = DoMethod(gui->ug_App, MUIM_Application_NewInput, &sigs);

            if((retid >= ID_ITEM) && ((ULONG) (retid - ID_ITEM) < gui->ug_NumItems))
            {
                nGuiItemChanged(gui, &gui->ug_Items[retid - ID_ITEM]);
            }
            switch(retid)
            {
                case ID_SAVE:
                case ID_USE:
                    nGuiStore(gui, retid == ID_SAVE);
                    done = TRUE;
                    break;

                case MUIV_Application_ReturnID_Quit:
                    /* Cancel, or the window was closed: as if it had not been opened */
                    if(gui->ug_Before)
                    {
                        nGuiRequest(gui, UVCMD_APPLY, gui->ug_Before, 0);
                    }
                    done = TRUE;
                    break;

                case ID_DEFAULTS:
                    nGuiDefaults(gui);
                    break;

                case ID_ABOUT:
                    MUI_RequestA(gui->ug_App, gui->ug_Window, 0, NULL, "OK", VERSION_STRING, NULL);
                    break;
            }
            if(!done && sigs)
            {
                sigs = Wait(sigs | eventmask | SIGBREAKF_CTRL_C);
                if(sigs & SIGBREAKF_CTRL_C)
                {
                    break; /* the camera is going: nothing to put back */
                }
                if(sigs & eventmask)
                {
                    nGuiEvent(gui);
                }
            }
        }
        set(gui->ug_Window, MUIA_Window_Open, FALSE);
    }
    nGuiCleanup(gui);
}
/* \\\ */
