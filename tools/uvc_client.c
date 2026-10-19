/*
 *----------------------------------------------------------------------------
 *              UVC tools: usbvideo.device client helpers
 *----------------------------------------------------------------------------
 * Implements uvc_client.h, in the same section order. Device API only, no
 * poseidon.library calls.
 */

#include <proto/dos.h>
#include <proto/exec.h>

#include <dos/dos.h>
#include <exec/errors.h>
#include <exec/memory.h>

#include <stdio.h>
#include <string.h>

#include "uvc_client.h"
#include "mjpegdht.h"

#define NAME_SIZE   256 /* frame file name buffer */
#define DIGITS_MAX  10  /* decimal digits of a ULONG */
#define FOURCC_LEN  4
#define ENDING_SIZE (1 + DIGITS_MAX + 1 + FOURCC_LEN + 1) /* suffix after the stem: "-", number,
                                                            ".", extension, NUL */

/*
 * Camera
 */

/* Return a device error code as text */
STRPTR uvcErrorText(LONG err)
{
    switch(err)
    {
        case 0:                 return "ok";
        case UVERR_NOMODE:      return "no mode has been set";
        case UVERR_BADMODE:     return "the camera has no such mode";
        case UVERR_REFUSED:     return "the camera refused";
        case UVERR_NOBANDWIDTH: return "the mode does not fit the bus";
        case UVERR_BUSY:        return "busy";
        case UVERR_OVERFLOW:    return "frame longer than the buffer";
        case UVERR_GONE:        return "the camera is gone";
        case UVERR_USB:         return "USB transfer failed";
        case UVERR_NOSTATUS:    return "the camera has no status endpoint";
        case IOERR_NOCMD:       return "command not supported";
        case IOERR_BADLENGTH:   return "bad length";
        case IOERR_ABORTED:     return "aborted";
    }
    return "error";
}

/* Execute one command synchronously (DoIO); return io_Error */
LONG uvcCommand(struct IOStdReq *ioreq, UWORD command, APTR data, ULONG len)
{
    ioreq->io_Command = command;
    ioreq->io_Data = data;
    ioreq->io_Length = len;
    return DoIO((struct IORequest *) ioreq);
}

/* Read the mode table (NSCMD_UV_GETMODES) into an AllocVec'd buffer.
   On failure print the reason and return FALSE. */
static BOOL readModes(struct UvcCamera *cm)
{
    ULONG size = cm->cm_Info.uvi_ModeTableSize;

    if(!size)
    {
        return TRUE; /* no format the class can stream: empty table */
    }
    if(!(cm->cm_Modes = AllocVec(size, MEMF_ANY)))
    {
        PutStr("Not enough memory for the list of modes\n");
        return FALSE;
    }

    LONG err = uvcCommand(cm->cm_Request, NSCMD_UV_GETMODES, cm->cm_Modes, size);

    if(err)
    {
        Printf("Could not read the modes: %s\n", uvcErrorText(err));
        return FALSE;
    }
    cm->cm_ModesSize = cm->cm_Request->io_Actual;
    return TRUE;
}

/* Create port and request, open the unit, read NSCMD_UV_GETINFO and the
   mode table. On failure print the reason and return FALSE. Call
   uvcCloseCamera() in either case. */
BOOL uvcOpenCamera(struct UvcCamera *cm, ULONG unit)
{
    memset(cm, 0, sizeof(*cm));
    cm->cm_Unit = unit;
    cm->cm_Port = CreateMsgPort();
    cm->cm_Request = (struct IOStdReq *) CreateIORequest(cm->cm_Port, sizeof(struct IOVideoReq));
    if(!cm->cm_Request)
    {
        PutStr("Not enough memory\n");
        return FALSE;
    }

    LONG err = OpenDevice(USBVIDEO_NAME, unit, (struct IORequest *) cm->cm_Request, 0);

    if(err)
    {
        Printf((err == IOERR_UNITBUSY) ? "The camera on unit %ld is in use by another program.\n"
                                       : "There is no camera on unit %ld.\n", unit);
        return FALSE;
    }
    cm->cm_Open = TRUE;
    if((err = uvcCommand(cm->cm_Request, NSCMD_UV_GETINFO, &cm->cm_Info, sizeof(cm->cm_Info))))
    {
        Printf("The camera did not answer: %s\n", uvcErrorText(err));
        return FALSE;
    }
    return readModes(cm);
}

/* Free everything uvcOpenCamera() allocated; safe after a partial open */
void uvcCloseCamera(struct UvcCamera *cm)
{
    FreeVec(cm->cm_Modes);
    cm->cm_Modes = NULL;
    if(cm->cm_Open)
    {
        CloseDevice((struct IORequest *) cm->cm_Request);
        cm->cm_Open = FALSE;
    }
    DeleteIORequest((struct IORequest *) cm->cm_Request);
    DeleteMsgPort(cm->cm_Port);
}

/*
 * Text
 */

/* Print the frame rate with one decimal for an interval in 100 ns units;
   "?" for 0 */
void uvcPrintRate(ULONG interval)
{
    if(interval)
    {
        Printf("%ld.%ld", UVC_INTERVAL_UNITS / interval, (10 * UVC_INTERVAL_UNITS / interval) % 10);
    } else {
        PutStr("?");
    }
}

/* Print a FourCC as four characters */
void uvcPrintFourCC(ULONG fourcc)
{
    UBYTE name[FOURCC_LEN + 1] = { fourcc >> 24, fourcc >> 16, fourcc >> 8, fourcc, 0 };

    PutStr(name);
}

/* Convert a string to a FourCC, space-padded to four characters, case
   kept; return 0 for NULL or empty */
ULONG uvcParseFourCC(STRPTR text)
{
    ULONG fourcc = 0;

    if(!text || !text[0])
    {
        return 0;
    }
    for(ULONG cnt = 0; cnt < FOURCC_LEN; cnt++)
    {
        fourcc = (fourcc << 8) | (UBYTE) (*text ? *text++ : ' ');
    }
    return fourcc;
}

/* Print a one-line camera summary: unit, product, manufacturer, VID:PID,
   serial number, UVC version, transfer type, mode count */
void uvcPrintCamera(const struct UvcCamera *cm)
{
    const struct UsbVideoInfo *uvi = &cm->cm_Info;

    Printf("Unit %ld: %s by %s (%04lx:%04lx), serial number %s, UVC %lx.%02lx, %s, %ld modes\n", cm->cm_Unit,
           uvi->uvi_ProductName, uvi->uvi_Manufacturer[0] ? uvi->uvi_Manufacturer : (const UBYTE *) "n/a",
           (ULONG) uvi->uvi_VendorID, (ULONG) uvi->uvi_ProductID,
           uvi->uvi_SerialNumber,
           (ULONG) uvi->uvi_Version >> 8, (ULONG) uvi->uvi_Version & 0xff,
           (uvi->uvi_Flags & UVIF_BULK) ? "bulk" : "isochronous", uvi->uvi_NumModes);
}

/* Print the mode table, one line per mode */
void uvcPrintModes(const struct UvcCamera *cm)
{
    for(ULONG pos = 0; pos < cm->cm_ModesSize; )
    {
        const struct UsbVideoMode *uvm = (const struct UsbVideoMode *) ((const UBYTE *) cm->cm_Modes + pos);

        PutStr("  ");
        uvcPrintFourCC(uvm->uvm_FourCC);
        Printf(" %4ldx%-4ld  ", (ULONG) uvm->uvm_Width, (ULONG) uvm->uvm_Height);
        if(uvm->uvm_Flags & UVMF_CONTINUOUS)
        {
            uvcPrintRate(uvm->uvm_Intervals[1]);
            PutStr(" to ");
            uvcPrintRate(uvm->uvm_Intervals[0]);
            PutStr(" fps");
        } else {
            for(ULONG cnt = 0; cnt < uvm->uvm_NumIntervals; cnt++)
            {
                uvcPrintRate(uvm->uvm_Intervals[cnt]);
                PutStr((cnt + 1 < uvm->uvm_NumIntervals) ? ", " : " fps");
            }
        }
        PutStr(", default ");
        uvcPrintRate(uvm->uvm_DefInterval);
        PutStr((uvm->uvm_Flags & UVMF_DEFAULT) ? "  <- the default (the settings window's, else the camera's)\n" : "\n");
        pos += uvm->uvm_Size;
    }
}

/* Print the negotiated mode returned by NSCMD_UV_SETMODE: format, size,
   frame rate */
void uvcPrintMode(const struct UsbVideoModeReq *uvq)
{
    uvcPrintFourCC(uvq->uvq_FourCC);
    Printf(" %ldx%ld at ", (ULONG) uvq->uvq_Width, (ULONG) uvq->uvq_Height);
    uvcPrintRate(uvq->uvq_Interval);
    PutStr(" fps");
}

/* Print ", <bit rate>, <key frame interval>" if the mode's encoder accepted
   either; print nothing otherwise */
void uvcPrintEncoder(const struct UsbVideoModeReq *uvq)
{
    if(uvq->uvq_BitRate || uvq->uvq_KeyInterval)
    {
        Printf(", %ld bits a second, a key frame every %ld ms", uvq->uvq_BitRate, uvq->uvq_KeyInterval);
    }
}

/*
 * Capture
 */

/* Create UVC_NUM_READS CMD_READ requests with framesize-byte buffers on the
   camera port and submit them. Requests that fail to allocate are skipped,
   the rest are submitted anyway. Return TRUE if all were submitted. */
static BOOL makeReads(struct UvcCapture *cp, ULONG framesize)
{
    struct IOStdReq *ioreq = cp->cp_Camera->cm_Request;
    ULONG sent = 0;

    for(ULONG cnt = 0; cnt < UVC_NUM_READS; cnt++)
    {
        struct IOVideoReq *ivr = (struct IOVideoReq *) CreateIORequest(cp->cp_Camera->cm_Port, sizeof(struct IOVideoReq));

        cp->cp_Reads[cnt] = ivr;
        if(!ivr)
        {
            continue;
        }
        ivr->ivr_Std.io_Device = ioreq->io_Device;
        ivr->ivr_Std.io_Unit = ioreq->io_Unit;
        ivr->ivr_Std.io_Command = CMD_READ;
        ivr->ivr_Std.io_Length = framesize;
        if((ivr->ivr_Std.io_Data = AllocVec(framesize, MEMF_ANY)))
        {
            SendIO((struct IORequest *) ivr);
            sent++;
        }
    }
    return sent == UVC_NUM_READS;
}

/* Reclaim and free all reads: abort and WaitIO() those still at the
   device, then free buffers and requests. cp_InHand is the only read off
   the device; it was taken with GetMsg() and must not be WaitIO()'d */
static void takeReadsBack(struct UvcCapture *cp)
{
    for(ULONG cnt = 0; cnt < UVC_NUM_READS; cnt++)
    {
        struct IOVideoReq *ivr = cp->cp_Reads[cnt];

        if(!ivr)
        {
            continue;
        }
        if(ivr->ivr_Std.io_Data)
        {
            if(ivr != cp->cp_InHand)
            {
                if(!CheckIO((struct IORequest *) ivr))
                {
                    AbortIO((struct IORequest *) ivr);
                }
                WaitIO((struct IORequest *) ivr);
            }
            FreeVec(ivr->ivr_Std.io_Data);
        }
        DeleteIORequest((struct IORequest *) ivr);
        cp->cp_Reads[cnt] = NULL;
    }
    cp->cp_InHand = NULL;
}

/* Return the next replied read, waiting if none is queued; NULL on Ctrl-C.
   Ctrl-C is checked before every GetMsg() so a continuous frame flow cannot
   starve it. */
static struct IOVideoReq *nextRead(struct UvcCapture *cp)
{
    struct MsgPort *port = cp->cp_Camera->cm_Port;
    ULONG signals = (1UL << port->mp_SigBit) | SIGBREAKF_CTRL_C;

    while(!(SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C))
    {
        struct IOVideoReq *ivr = (struct IOVideoReq *) GetMsg(port);

        if(ivr)
        {
            return ivr;
        }
        if(Wait(signals) & SIGBREAKF_CTRL_C)
        {
            break;
        }
    }
    return NULL;
}

/* Add the sequence gap since the previous frame to cp_Missed */
static void countGap(struct UvcCapture *cp, const struct IOVideoReq *ivr)
{
    if(cp->cp_LastSeq)
    {
        cp->cp_Missed += ivr->ivr_Sequence - cp->cp_LastSeq - 1;
    }
    cp->cp_LastSeq = ivr->ivr_Sequence;
}

/* Start a capture in the mode already set: submit the reads, open the
   output if stem is not NULL, send CMD_START. On failure print the reason
   and return FALSE. Call uvcStopCapture() in either case. */
BOOL uvcStartCapture(struct UvcCapture *cp, struct UvcCamera *cm, const struct UsbVideoModeReq *uvq, STRPTR stem)
{
    memset(cp, 0, sizeof(*cp));
    cp->cp_Camera = cm;
    if(!makeReads(cp, uvq->uvq_MaxFrameSize))
    {
        Printf("Not enough memory for %ld buffers of %ld bytes\n", (ULONG) UVC_NUM_READS, uvq->uvq_MaxFrameSize);
        return FALSE;
    }
    if(!uvcOpenFiles(&cp->cp_Files, stem, uvq->uvq_FourCC))
    {
        return FALSE;
    }

    LONG err = uvcCommand(cm->cm_Request, CMD_START, NULL, 0);

    if(err)
    {
        Printf("Could not start the stream: %s\n", uvcErrorText(err));
        return FALSE;
    }
    cp->cp_Streaming = TRUE;
    return TRUE;
}

/* Return the read holding the next complete frame. Resubmit cp_InHand
   first, count sequence gaps, drop UVERR_OVERFLOW frames, write the frame
   if the capture has output. NULL ends the capture; cp_Error,
   cp_Interrupted or cp_NotSaved gives the reason. */
struct IOVideoReq *uvcNextFrame(struct UvcCapture *cp)
{
    struct IOVideoReq *ivr;

    if(cp->cp_InHand)
    {
        SendIO((struct IORequest *) cp->cp_InHand);
        cp->cp_InHand = NULL;
    }
    while((ivr = nextRead(cp)))
    {
        LONG err = ivr->ivr_Std.io_Error;

        if(err == UVERR_OVERFLOW)
        {
            countGap(cp, ivr);
            cp->cp_TooLong++;
            SendIO((struct IORequest *) ivr);
            continue;
        }
        /* kept off the device until the next call, also when it ends the capture */
        cp->cp_InHand = ivr;
        if(err)
        {
            cp->cp_Error = err;
            return NULL;
        }
        countGap(cp, ivr);
        if(!uvcWriteFrame(&cp->cp_Files, ivr->ivr_Std.io_Data, ivr->ivr_Std.io_Actual))
        {
            cp->cp_NotSaved = TRUE;
            return NULL;
        }
        return ivr;
    }
    cp->cp_Interrupted = TRUE;
    return NULL;
}

/* End a capture: CMD_STOP if streaming, reclaim and free all reads, close
   the output */
void uvcStopCapture(struct UvcCapture *cp)
{
    if(cp->cp_Streaming)
    {
        uvcCommand(cp->cp_Camera->cm_Request, CMD_STOP, NULL, 0);
        cp->cp_Streaming = FALSE;
    }
    takeReadsBack(cp);
    uvcCloseFiles(&cp->cp_Files);
}

/*
 * Files
 */

/* Return TRUE for formats whose frames are segments of one elementary
   stream, saved to a single file */
BOOL uvcIsStream(ULONG fourcc)
{
    return fourcc == UVFCC_H264;
}

/* Write the NUL-terminated file extension for a FourCC: "jpg" for MJPG,
   else the FourCC lowercased with spaces removed (yuy2, h264) */
static void extension(ULONG fourcc, UBYTE *ext)
{
    ULONG len = 0;

    if(fourcc == UVFCC_MJPG)
    {
        strcpy(ext, "jpg");
        return;
    }
    for(LONG shift = (FOURCC_LEN - 1) * 8; shift >= 0; shift -= 8)
    {
        UBYTE ch = (UBYTE) (fourcc >> shift);

        if(ch != ' ')
        {
            ext[len++] = ((ch >= 'A') && (ch <= 'Z')) ? ch + ('a' - 'A') : ch;
        }
    }
    ext[len] = 0;
}

/* Build the file name for a frame number: <stem>-NNNN.<ext>, or
   <stem>.<ext> for a stream format. uvcOpenFiles() guarantees the result
   fits NAME_SIZE; snprintf() would cut it rather than overrun. */
static void fileName(const struct UvcFiles *fl, ULONG number, UBYTE *name)
{
    UBYTE ext[FOURCC_LEN + 1];

    extension(fl->fl_FourCC, ext);
    if(uvcIsStream(fl->fl_FourCC))
    {
        snprintf((char *) name, NAME_SIZE, "%s.%s", (char *) fl->fl_Stem, (char *) ext);
    } else {
        snprintf((char *) name, NAME_SIZE, "%s-%04lu.%s", (char *) fl->fl_Stem, number, (char *) ext);
    }
}

/* Write len bytes; return TRUE if all were written */
static BOOL writeAll(BPTR fh, const UBYTE *data, ULONG len)
{
    return Write(fh, (APTR) data, len) == (LONG) len;
}

/* Write one frame to an open file. An MJPEG frame without DHT gets the
   standard Huffman tables inserted before SOS (MJPEG cameras omit them,
   JPEG decoders need them); other formats are written unchanged. */
static BOOL writeFrameData(BPTR fh, ULONG fourcc, const UBYTE *frame, ULONG len)
{
    ULONG dhtpos = (fourcc == UVFCC_MJPG) ? mjpegMissingDHT(frame, len) : 0;

    if(!dhtpos)
    {
        return writeAll(fh, frame, len);
    }
    return writeAll(fh, frame, dhtpos) && writeAll(fh, MJPEGStdDHT, MJPEG_STD_DHT_SIZE) &&
           writeAll(fh, frame + dhtpos, len - dhtpos);
}

/* Initialise the output; open the file now for a stream format. A NULL
   stem disables saving. If the stem is too long for NAME_SIZE or the stream
   file cannot be opened, print the reason, leave saving disabled and
   return FALSE. */
BOOL uvcOpenFiles(struct UvcFiles *fl, STRPTR stem, ULONG fourcc)
{
    fl->fl_Stem = NULL;
    fl->fl_FourCC = fourcc;
    fl->fl_Stream = 0;
    fl->fl_Saved = 0;
    if(!stem)
    {
        return TRUE;
    }
    if(strlen(stem) + ENDING_SIZE > NAME_SIZE)
    {
        Printf("The name '%s' is too long\n", stem);
        return FALSE;
    }
    fl->fl_Stem = stem;
    if(!uvcIsStream(fourcc))
    {
        return TRUE;
    }

    UBYTE name[NAME_SIZE];

    fileName(fl, 0, name);
    if(!(fl->fl_Stream = Open(name, MODE_NEWFILE)))
    {
        Printf("Could not open '%s' for writing\n", name);
        fl->fl_Stem = NULL;
        return FALSE;
    }
    return TRUE;
}

/* Append a frame to the open stream file, as it came. The file name is
   built only for the failure message. */
static BOOL appendToStream(struct UvcFiles *fl, const UBYTE *frame, ULONG len)
{
    if(writeAll(fl->fl_Stream, frame, len))
    {
        return TRUE;
    }

    UBYTE name[NAME_SIZE];

    fileName(fl, 0, name);
    Printf("Could not write '%s'\n", name);
    return FALSE;
}

/* Write a frame to a new file of its own, named after its number. On
   failure print the reason and return FALSE. */
static BOOL writeFrameFile(struct UvcFiles *fl, const UBYTE *frame, ULONG len)
{
    UBYTE name[NAME_SIZE];

    fileName(fl, fl->fl_Saved + 1, name);

    BPTR fh = Open(name, MODE_NEWFILE);

    if(!fh)
    {
        Printf("Could not open '%s' for writing\n", name);
        return FALSE;
    }

    BOOL ok = writeFrameData(fh, fl->fl_FourCC, frame, len);

    Close(fh);
    if(!ok)
    {
        Printf("Could not write '%s'\n", name);
    }
    return ok;
}

/* Write the next frame to a new file, or append it to the stream file. On
   failure print the reason and return FALSE. */
BOOL uvcWriteFrame(struct UvcFiles *fl, const UBYTE *frame, ULONG len)
{
    if(!fl->fl_Stem)
    {
        return TRUE;
    }

    BOOL ok = fl->fl_Stream ? appendToStream(fl, frame, len) : writeFrameFile(fl, frame, len);

    if(ok)
    {
        fl->fl_Saved++;
    }
    return ok;
}

/* Close the stream file, if open */
void uvcCloseFiles(struct UvcFiles *fl)
{
    if(fl->fl_Stream)
    {
        Close(fl->fl_Stream);
        fl->fl_Stream = 0;
    }
}

/* Print the number of frames saved and the first..last file name; print
   nothing if no frame was saved */
void uvcPrintSaved(const struct UvcFiles *fl)
{
    if(!fl->fl_Saved)
    {
        return;
    }

    UBYTE first[NAME_SIZE];

    fileName(fl, 1, first);
    Printf("%ld %s saved as %s", fl->fl_Saved, (fl->fl_Saved == 1) ? "frame" : "frames", first);
    if(!uvcIsStream(fl->fl_FourCC) && (fl->fl_Saved > 1))
    {
        UBYTE last[NAME_SIZE];

        fileName(fl, fl->fl_Saved, last);
        Printf(" ... %s", last);
    }
    PutStr("\n");
}
