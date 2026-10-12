/*
 *----------------------------------------------------------------------------
 *              usbvideo class: the camera's controls, by name
 *----------------------------------------------------------------------------
 * NSCMD_UV_GETCONTROL, NSCMD_UV_NEXTCONTROL and NSCMD_UV_SETCONTROL of
 * <devices/usbvideodevice.h>. Control requests as such are passed on and
 * mean nothing to the class (uvc_ctrl.c); this is where the standard ones
 * get a meaning. For every control the USB video class defines for a camera
 * terminal and a processing unit that is one number, the table below has
 * its bit in the unit's control bitmap, its selector, the layout of its data
 * and a name. The settings window and every program go by it.
 *
 * A row is one value a user sees. Two controls carry two values each in one
 * block of data (pan and tilt, the blue and the red white balance
 * component): they are two rows with the same selector.
 *
 * Not in the table are the movements - the relative controls - and the
 * controls with a layout of their own: the digital window, the region of
 * interest, the analog video standard and its lock. NSCMD_UV_REQUEST reaches
 * those.
 *
 * Everything here runs on the binding's subtask, which owns the EP0 pipe.
 */

#include "debug.h"
#include "usbvideo.class.h"

#define UVC_CONTROL_MAX_SIZE 8  /* no control of the table has more data: pan and tilt */
#define UVC_CHOICE_BITS      8  /* a control that gives each choice a bit of its own is one byte */

/* What the camera says of a control (GET_INFO) is handed to a program as it
   is, in the low bits of uvc_Flags */
#define UVCF_CAMERA (UVCF_GET|UVCF_SET|UVCF_INACTIVE|UVCF_VOLATILE|UVCF_SLOW)

_Static_assert((UVCF_GET == UVGIF_GET) && (UVCF_SET == UVGIF_SET) && (UVCF_INACTIVE == UVGIF_DISABLED_AUTO) &&
               (UVCF_VOLATILE == UVGIF_AUTOUPDATE) && (UVCF_SLOW == UVGIF_ASYNCHRONOUS),
               "the first control flags are the bits of GET_INFO");

struct UvcControlInfo
{
    UWORD              uci_ID;         /* UVCID_ */
    UBYTE              uci_UnitType;   /* UDST_VIDEO_CTRL_INPUT_TERMINAL (a camera) or _PROCESSING_UNIT */
    UBYTE              uci_Bit;        /* its bit in the unit's control bitmap */
    UBYTE              uci_Selector;
    UBYTE              uci_Size;       /* bytes of the control's data */
    UBYTE              uci_Offset;     /* where in them this value lies... */
    UBYTE              uci_Bytes;      /* ...and how long it is */
    UBYTE              uci_Kind;       /* UVCK_ */
    UBYTE              uci_Flags;      /* UCF_ */
    const char        *uci_Name;
    const char *const *uci_Choices;    /* UVCK_CHOICE: the names, NULL after the last */
};

#define UCF_SIGNED   0x01 /* the value is signed */
#define UCF_BITS     0x02 /* UVCK_CHOICE: choice n has the value 1 << n (else n), and GET_RES
                             answers with the choices the camera has, one bit each */
#define UCF_FIRST    0x04 /* it decides whether other controls can be set (an automatic
                             mode): to be written before them */

#define UCI_CT UDST_VIDEO_CTRL_INPUT_TERMINAL
#define UCI_PU UDST_VIDEO_CTRL_PROCESSING_UNIT

/* in the order of UVPLF_, UVEXP_ and UVFOC_ */
static const char *const UvcPowerLineChoices[] = { "Off", "50 Hz", "60 Hz", "Automatic", NULL };
static const char *const UvcExposureChoices[] = { "Manual", "Automatic", "Shutter priority", "Aperture priority", NULL };
static const char *const UvcFocusChoices[] = { "Full", "Macro", "People", "Scene", NULL };

/* In the order a settings window shows them, and the order they are listed in */
static const struct UvcControlInfo UvcControls[] =
{
    /* the picture: processing unit */
    { UVCID_BRIGHTNESS,         UCI_PU, UVPUB_BRIGHTNESS,         UVPUCS_BRIGHTNESS_CONTROL,         2, 0, 2, UVCK_RANGE,  UCF_SIGNED, "Brightness", NULL },
    { UVCID_CONTRAST_AUTO,      UCI_PU, UVPUB_CONTRAST_AUTO,      UVPUCS_CONTRAST_AUTO_CONTROL,      1, 0, 1, UVCK_SWITCH, UCF_FIRST,  "Contrast automatic", NULL },
    { UVCID_CONTRAST,           UCI_PU, UVPUB_CONTRAST,           UVPUCS_CONTRAST_CONTROL,           2, 0, 2, UVCK_RANGE,  0,          "Contrast", NULL },
    { UVCID_SATURATION,         UCI_PU, UVPUB_SATURATION,         UVPUCS_SATURATION_CONTROL,         2, 0, 2, UVCK_RANGE,  0,          "Saturation", NULL },
    { UVCID_HUE_AUTO,           UCI_PU, UVPUB_HUE_AUTO,           UVPUCS_HUE_AUTO_CONTROL,           1, 0, 1, UVCK_SWITCH, UCF_FIRST,  "Hue automatic", NULL },
    { UVCID_HUE,                UCI_PU, UVPUB_HUE,                UVPUCS_HUE_CONTROL,                2, 0, 2, UVCK_RANGE,  UCF_SIGNED, "Hue", NULL },
    { UVCID_SHARPNESS,          UCI_PU, UVPUB_SHARPNESS,          UVPUCS_SHARPNESS_CONTROL,          2, 0, 2, UVCK_RANGE,  0,          "Sharpness", NULL },
    { UVCID_GAMMA,              UCI_PU, UVPUB_GAMMA,              UVPUCS_GAMMA_CONTROL,              2, 0, 2, UVCK_RANGE,  0,          "Gamma", NULL },
    { UVCID_GAIN,               UCI_PU, UVPUB_GAIN,               UVPUCS_GAIN_CONTROL,               2, 0, 2, UVCK_RANGE,  0,          "Gain", NULL },
    { UVCID_BACKLIGHT,          UCI_PU, UVPUB_BACKLIGHT_COMP,     UVPUCS_BACKLIGHT_COMP_CONTROL,     2, 0, 2, UVCK_RANGE,  0,          "Backlight compensation", NULL },
    { UVCID_WB_AUTO,            UCI_PU, UVPUB_WB_TEMP_AUTO,       UVPUCS_WB_TEMP_AUTO_CONTROL,       1, 0, 1, UVCK_SWITCH, UCF_FIRST,  "White balance automatic", NULL },
    { UVCID_WB,                 UCI_PU, UVPUB_WB_TEMP,            UVPUCS_WB_TEMP_CONTROL,            2, 0, 2, UVCK_RANGE,  0,          "White balance (K)", NULL },
    { UVCID_WB_COMPONENT_AUTO,  UCI_PU, UVPUB_WB_COMPONENT_AUTO,  UVPUCS_WB_COMPONENT_AUTO_CONTROL,  1, 0, 1, UVCK_SWITCH, UCF_FIRST,  "White balance components automatic", NULL },
    { UVCID_WB_BLUE,            UCI_PU, UVPUB_WB_COMPONENT,       UVPUCS_WB_COMPONENT_CONTROL,       4, 0, 2, UVCK_RANGE,  0,          "White balance, blue", NULL },
    { UVCID_WB_RED,             UCI_PU, UVPUB_WB_COMPONENT,       UVPUCS_WB_COMPONENT_CONTROL,       4, 2, 2, UVCK_RANGE,  0,          "White balance, red", NULL },
    { UVCID_POWER_LINE,         UCI_PU, UVPUB_POWER_LINE_FREQ,    UVPUCS_POWER_LINE_FREQ_CONTROL,    1, 0, 1, UVCK_CHOICE, 0,          "Mains frequency", UvcPowerLineChoices },
    { UVCID_DIGITAL_ZOOM,       UCI_PU, UVPUB_DIGITAL_MULT,       UVPUCS_DIGITAL_MULT_CONTROL,       2, 0, 2, UVCK_RANGE,  0,          "Digital zoom", NULL },
    { UVCID_DIGITAL_ZOOM_LIMIT, UCI_PU, UVPUB_DIGITAL_MULT_LIMIT, UVPUCS_DIGITAL_MULT_LIMIT_CONTROL, 2, 0, 2, UVCK_RANGE,  0,          "Digital zoom limit", NULL },

    /* the camera: camera terminal */
    { UVCID_EXPOSURE_MODE,      UCI_CT, UVCTB_AE_MODE,            UVCTCS_AE_MODE_CONTROL,            1, 0, 1, UVCK_CHOICE, UCF_BITS|UCF_FIRST, "Exposure", UvcExposureChoices },
    { UVCID_EXPOSURE_PRIORITY,  UCI_CT, UVCTB_AE_PRIORITY,        UVCTCS_AE_PRIORITY_CONTROL,        1, 0, 1, UVCK_SWITCH, 0,          "Exposure may lower the frame rate", NULL },
    { UVCID_EXPOSURE_TIME,      UCI_CT, UVCTB_EXPOSURE_TIME_ABS,  UVCTCS_EXPOSURE_TIME_ABS_CONTROL,  4, 0, 4, UVCK_RANGE,  0,          "Exposure time (0.1 ms)", NULL },
    { UVCID_IRIS,               UCI_CT, UVCTB_IRIS_ABS,           UVCTCS_IRIS_ABS_CONTROL,           2, 0, 2, UVCK_RANGE,  0,          "Iris (f-number x 100)", NULL },
    { UVCID_FOCUS_AUTO,         UCI_CT, UVCTB_FOCUS_AUTO,         UVCTCS_FOCUS_AUTO_CONTROL,         1, 0, 1, UVCK_SWITCH, UCF_FIRST,  "Focus automatic", NULL },
    { UVCID_FOCUS,              UCI_CT, UVCTB_FOCUS_ABS,          UVCTCS_FOCUS_ABS_CONTROL,          2, 0, 2, UVCK_RANGE,  0,          "Focus", NULL },
    { UVCID_FOCUS_RANGE,        UCI_CT, UVCTB_FOCUS_SIMPLE,       UVCTCS_FOCUS_SIMPLE_CONTROL,       1, 0, 1, UVCK_CHOICE, 0,          "Focus range", UvcFocusChoices },
    { UVCID_ZOOM,               UCI_CT, UVCTB_ZOOM_ABS,           UVCTCS_ZOOM_ABS_CONTROL,           2, 0, 2, UVCK_RANGE,  0,          "Zoom", NULL },
    { UVCID_PAN,                UCI_CT, UVCTB_PANTILT_ABS,        UVCTCS_PANTILT_ABS_CONTROL,        8, 0, 4, UVCK_RANGE,  UCF_SIGNED, "Pan (arc seconds)", NULL },
    { UVCID_TILT,               UCI_CT, UVCTB_PANTILT_ABS,        UVCTCS_PANTILT_ABS_CONTROL,        8, 4, 4, UVCK_RANGE,  UCF_SIGNED, "Tilt (arc seconds)", NULL },
    { UVCID_ROLL,               UCI_CT, UVCTB_ROLL_ABS,           UVCTCS_ROLL_ABS_CONTROL,           2, 0, 2, UVCK_RANGE,  UCF_SIGNED, "Roll (degrees)", NULL },
    { UVCID_PRIVACY,            UCI_CT, UVCTB_PRIVACY,            UVCTCS_PRIVACY_CONTROL,            1, 0, 1, UVCK_SWITCH, 0,          "Privacy shutter", NULL },
    { UVCID_SCANNING_MODE,      UCI_CT, UVCTB_SCANNING_MODE,      UVCTCS_SCANNING_MODE_CONTROL,      1, 0, 1, UVCK_SWITCH, 0,          "Progressive scan", NULL }
};

#define UVC_NUM_CONTROLS (sizeof(UvcControls) / sizeof(UvcControls[0]))

/* /// "uvcControlRaw()" */
/* The value of a row in the data of its control, as the camera sent it */
static LONG uvcControlRaw(const struct UvcControlInfo *uci, const UBYTE *data)
{
    const UBYTE *field = data + uci->uci_Offset;

    switch(uci->uci_Bytes)
    {
        case sizeof(UBYTE):
            return((uci->uci_Flags & UCF_SIGNED) ? (LONG) (BYTE) field[0] : (LONG) field[0]);

        case sizeof(UWORD):
        {
            UWORD value = AROS_LE2WORD(*(const UWORD *) field);

            return((uci->uci_Flags & UCF_SIGNED) ? (LONG) (WORD) value : (LONG) value);
        }
    }
    return((LONG) AROS_LE2LONG(*(const ULONG *) field));
}
/* \\\ */

/* /// "uvcControlPut()" */
/* ...and a new one into it; the rest of the data stays as it is */
static void uvcControlPut(const struct UvcControlInfo *uci, UBYTE *data, LONG value)
{
    UBYTE *field = data + uci->uci_Offset;

    switch(uci->uci_Bytes)
    {
        case sizeof(UBYTE):
            *field = value;
            break;

        case sizeof(UWORD):
            *(UWORD *) field = AROS_WORD2LE(value);
            break;

        default:
            *(ULONG *) field = AROS_LONG2LE(value);
            break;
    }
}
/* \\\ */

/* /// "uvcControlNumber()" */
/* A value of the camera as the number a program sees. They differ for a
   choice that has a bit for each of its settings: a program counts the
   settings from 0. No bit at all counts as the first. */
static LONG uvcControlNumber(const struct UvcControlInfo *uci, LONG raw)
{
    LONG choice = 0;

    if(!(uci->uci_Flags & UCF_BITS))
    {
        return(raw);
    }
    while((choice < UVC_CHOICE_BITS) && !((raw >> choice) & 1))
    {
        choice++;
    }
    return((choice < UVC_CHOICE_BITS) ? choice : 0);
}
/* \\\ */

/* /// "uvcControlWire()" */
/* ...and a program's number as the value the camera takes. A choice beyond
   the bits there are becomes no bit, which the camera refuses. */
static LONG uvcControlWire(const struct UvcControlInfo *uci, LONG value)
{
    if(!(uci->uci_Flags & UCF_BITS))
    {
        return(value);
    }
    return(((ULONG) value < UVC_CHOICE_BITS) ? (1L << value) : 0);
}
/* \\\ */

/* /// "uvcControlPresent()" */
/* Does a terminal or unit have the control of a row? It has to be of the
   row's kind and to have the row's bit in its control bitmap. */
static BOOL uvcControlPresent(const struct UvcControlInfo *uci, const struct UvcEntity *ue)
{
    ULONG byte = uci->uci_Bit / 8;

    return((ue->ue_Type == uci->uci_UnitType) && (byte < ue->ue_ControlSize) &&
           ((ue->ue_Controls[byte] >> (uci->uci_Bit % 8)) & 1));
}
/* \\\ */

/* /// "uvcControlLocate()" */
/* One control of the camera: its row of the table, and in *ueptr the
   terminal or unit that has it. The controls of a camera have an order: the
   rows of the table, and for a row several units have, those units as the
   descriptors list them. Asked for a control - an ID, and a unit or 0 for
   any - this finds the first that fits. Asked for the next, it finds the
   one behind that, or the very first if no ID is given. NULL if there is
   none. */
static const struct UvcControlInfo * uvcControlLocate(const struct UvcFunction *uf, ULONG id, ULONG entity,
                                                      BOOL next, const struct UvcEntity **ueptr)
{
    BOOL passed = next && !id;

    for(ULONG row = 0; row < UVC_NUM_CONTROLS; row++)
    {
        const struct UvcControlInfo *uci = &UvcControls[row];

        for(ULONG cnt = 0; cnt < uf->uf_NumEntities; cnt++)
        {
            const struct UvcEntity *ue = &uf->uf_Entities[cnt];

            if(!uvcControlPresent(uci, ue))
            {
                continue;
            }
            BOOL named = (uci->uci_ID == id) && (!entity || (ue->ue_ID == entity));

            if(passed || (named && !next))
            {
                *ueptr = ue;
                return(uci);
            }
            passed = named;
        }
    }
    return(NULL);
}
/* \\\ */

/* /// "uvcControlAsk()" */
/* One of the GET requests for a row: the camera's answer as it sent it.
   FALSE if it does not answer; why not goes to *reason, if the caller wants
   it. */
static BOOL uvcControlAsk(struct NepClassVideo *ncv, const struct UvcControlInfo *uci, ULONG entity,
                          ULONG request, LONG *raw, UBYTE *reason)
{
    UBYTE data[UVC_CONTROL_MAX_SIZE];

    if(uvcRequest(ncv, request, entity, ncv->ncv_IfNum, uci->uci_Selector, data, uci->uci_Size, reason))
    {
        return(FALSE);
    }
    *raw = uvcControlRaw(uci, data);
    return(TRUE);
}
/* \\\ */

/* /// "uvcControlChoices()" */
/* The choices of a row that the camera has, one bit each, out of all the
   row names. A control that gives each choice a bit lists them itself
   (GET_RES); of any other the camera names the last one it has (GET_MAX).
   A camera that does not say has them all. */
static ULONG uvcControlChoices(struct NepClassVideo *ncv, const struct UvcControlInfo *uci, ULONG entity, ULONG all)
{
    LONG raw;

    if(uci->uci_Flags & UCF_BITS)
    {
        return(uvcControlAsk(ncv, uci, entity, UVUDR_GET_RES, &raw, NULL) ? ((ULONG) raw & all) : all);
    }
    if(!uvcControlAsk(ncv, uci, entity, UVUDR_GET_MAX, &raw, NULL) || ((ULONG) raw >= UVC_CHOICE_BITS))
    {
        return(all);
    }
    return(all & ((2UL << raw) - 1));
}
/* \\\ */

/* /// "uvcControlQuery()" */
/* NSCMD_UV_GETCONTROL, and with next set NSCMD_UV_NEXTCONTROL: all there is
   to say about one control. What the table knows is filled in first, then
   the camera is asked - what the control can do right now, its value, and
   what it leaves a program to choose from. A control that the camera does
   not let be read ends there; so does one it lists and then does not answer
   for, which is not an error: uvc_Flags says that it cannot be read, and
   uvc_Reason why the camera refused. Returns 0 or the error for io_Error. */
LONG uvcControlQuery(struct NepClassVideo *ncv, struct UsbVideoControl *uvc, BOOL next)
{
    const struct UvcEntity *ue;
    const struct UvcControlInfo *uci = uvcControlLocate(ncv->ncv_Function, uvc->uvc_ID, uvc->uvc_Entity, next, &ue);
    ULONG numchoices = 0;
    UBYTE info = 0;
    LONG raw = 0;

    if(!uci)
    {
        return(UVERR_NOCONTROL);
    }
    ULONG entity = ue->ue_ID;

    while(uci->uci_Choices && uci->uci_Choices[numchoices])
    {
        numchoices++;
    }
    memset(uvc, 0, sizeof(*uvc));
    uvc->uvc_ID = uci->uci_ID;
    uvc->uvc_Entity = entity;
    uvc->uvc_Selector = uci->uci_Selector;
    uvc->uvc_Kind = uci->uci_Kind;
    uvc->uvc_Flags = (uci->uci_Flags & UCF_FIRST) ? UVCF_GOVERNS : 0;
    uvc->uvc_Step = 1;
    uvc->uvc_Name = (CONST_STRPTR) uci->uci_Name;
    uvc->uvc_ChoiceNames = (const CONST_STRPTR *) uci->uci_Choices;
    if(uci->uci_Kind != UVCK_RANGE)
    {
        /* a switch is off or on; a choice is one of its names */
        uvc->uvc_Max = numchoices ? numchoices - 1 : 1;
        uvc->uvc_Choices = (1UL << numchoices) - 1;
        uvc->uvc_Flags |= UVCF_RANGE;
    }

    LONG ioerr = uvcRequest(ncv, UVUDR_GET_INFO, entity, ncv->ncv_IfNum, uci->uci_Selector,
                            &info, sizeof(info), &uvc->uvc_Reason);
    if(ioerr && (ioerr != UHIOERR_STALL))
    {
        return(UVERR_USB);
    }
    if(ioerr)
    {
        info = 0; /* it does not say: nothing can be done with it */
    }
    /* a control that says it can be read and then is not, cannot be read */
    if((info & UVGIF_GET) && !uvcControlAsk(ncv, uci, entity, UVUDR_GET_CUR, &raw, &uvc->uvc_Reason))
    {
        info &= ~UVGIF_GET;
    }
    uvc->uvc_Flags |= (info & UVCF_CAMERA) | ((info & UVGIF_DISABLED_COMMIT) ? UVCF_INACTIVE : 0);
    if(!(info & UVGIF_GET))
    {
        return(0);
    }
    uvc->uvc_Value = uvcControlNumber(uci, raw);
    if(uvcControlAsk(ncv, uci, entity, UVUDR_GET_DEF, &raw, NULL))
    {
        uvc->uvc_Default = uvcControlNumber(uci, raw);
        uvc->uvc_Flags |= UVCF_DEFAULT;
    }
    if(uci->uci_Kind == UVCK_CHOICE)
    {
        uvc->uvc_Choices = uvcControlChoices(ncv, uci, entity, uvc->uvc_Choices);
    }
    if(uci->uci_Kind == UVCK_RANGE)
    {
        LONG max;

        if(uvcControlAsk(ncv, uci, entity, UVUDR_GET_MIN, &raw, NULL) &&
           uvcControlAsk(ncv, uci, entity, UVUDR_GET_MAX, &max, NULL))
        {
            uvc->uvc_Min = raw;
            uvc->uvc_Max = max;
            uvc->uvc_Flags |= UVCF_RANGE;
        }
        /* a camera that names no step, or none that leads anywhere, counts in ones */
        if(uvcControlAsk(ncv, uci, entity, UVUDR_GET_RES, &raw, NULL) && (raw > 0))
        {
            uvc->uvc_Step = raw;
        }
    }
    return(0);
}
/* \\\ */

/* /// "uvcControlWrite()" */
/* A new value for one control: NSCMD_UV_SETCONTROL, and the values stored
   for a camera. The value goes to the camera as it is given. Where it
   shares its block of data with another value, the block is read first, so
   that the other stays as the camera has it. Returns 0 or the error for
   io_Error; why the camera refused goes to *reason, if the caller wants it. */
LONG uvcControlWrite(struct NepClassVideo *ncv, ULONG id, ULONG entity, LONG value, UBYTE *reason)
{
    const struct UvcEntity *ue;
    const struct UvcControlInfo *uci = uvcControlLocate(ncv->ncv_Function, id, entity, FALSE, &ue);
    UBYTE data[UVC_CONTROL_MAX_SIZE];
    LONG ioerr = 0;

    if(!uci)
    {
        return(UVERR_NOCONTROL);
    }
    memset(data, 0, sizeof(data));
    if(uci->uci_Bytes < uci->uci_Size)
    {
        ioerr = uvcRequest(ncv, UVUDR_GET_CUR, ue->ue_ID, ncv->ncv_IfNum, uci->uci_Selector,
                           data, uci->uci_Size, reason);
    }
    if(!ioerr)
    {
        uvcControlPut(uci, data, uvcControlWire(uci, value));
        ioerr = uvcRequest(ncv, UVUDR_SET_CUR, ue->ue_ID, ncv->ncv_IfNum, uci->uci_Selector,
                           data, uci->uci_Size, reason);
    }
    if(ioerr)
    {
        return((ioerr == UHIOERR_STALL) ? UVERR_REFUSED : UVERR_USB);
    }
    return(0);
}
/* \\\ */

/* /// "uvcControlCommand()" */
/* NSCMD_UV_GETCONTROL, NSCMD_UV_NEXTCONTROL and NSCMD_UV_SETCONTROL, for the
   subtask's loop: io_Data is the control (struct UsbVideoControl). Returns 0
   or the error for io_Error. */
LONG uvcControlCommand(struct NepClassVideo *ncv, struct IOStdReq *ioreq)
{
    struct UsbVideoControl *uvc = (struct UsbVideoControl *) ioreq->io_Data;
    LONG err;

    if(!uvc || (ioreq->io_Length < sizeof(struct UsbVideoControl)))
    {
        return(IOERR_BADLENGTH);
    }
    if(ioreq->io_Command == NSCMD_UV_SETCONTROL)
    {
        uvc->uvc_Reason = UVREC_NO_ERROR;
        err = uvcControlWrite(ncv, uvc->uvc_ID, uvc->uvc_Entity, uvc->uvc_Value, &uvc->uvc_Reason);
    } else {
        err = uvcControlQuery(ncv, uvc, ioreq->io_Command == NSCMD_UV_NEXTCONTROL);
    }
    if(!err)
    {
        ioreq->io_Actual = sizeof(struct UsbVideoControl);
    }
    return(err);
}
/* \\\ */
