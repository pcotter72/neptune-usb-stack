/*
 *----------------------------------------------------------------------------
 *              usbvideo class: the camera's status events
 *----------------------------------------------------------------------------
 * A camera may have an interrupt endpoint on its control interface, over
 * which it reports what happens without being asked: a control changed its
 * value or its state, the button on the camera went down. The binding's
 * subtask keeps one transfer waiting there from bind to release.
 *
 * The class passes the packets on unread (<devices/usb_video.h>, UVSP_) to
 * whoever waits for one: the NSCMD_UV_WAITEVENT requests of the units and
 * the one of the settings window, in one queue per binding, because an event
 * is the camera's and not a stream's. Applications, the window, the subtask
 * and CloseDevice() share that queue: every access under Forbid(), like the
 * read queues.
 */

#include "debug.h"
#include "usbvideo.class.h"

#define ps ncv->ncv_Base

#define STATUS_MAX_ERRORS 8 /* failed transfers in a row after which the endpoint is given up */

/* /// "uvcEventReply()" */
/* A waiting request goes back to its owner */
static void uvcEventReply(struct IOStdReq *ioreq, LONG error)
{
    ioreq->io_Error = error;
    ReplyMsg(&ioreq->io_Message);
}
/* \\\ */

/* /// "uvcEventHasUnit()" */
/* Is a request of this unit in the list? The settings window's requests have
   no unit, and that is told apart like any unit. */
static BOOL uvcEventHasUnit(const struct List *list, const struct Unit *unit)
{
    const struct IOStdReq *ioreq = (const struct IOStdReq *) list->lh_Head;

    while(ioreq->io_Message.mn_Node.ln_Succ)
    {
        if(ioreq->io_Unit == unit)
        {
            return TRUE;
        }
        ioreq = (const struct IOStdReq *) ioreq->io_Message.mn_Node.ln_Succ;
    }
    return FALSE;
}
/* \\\ */

/* /// "uvcEventDeliver()" */
/* One status packet to the first waiting request of everyone who waits: each
   unit's opener and the settings window (whose request has no unit) see
   every event once. What a streaming interface reports - the button - is not
   for the units of the other streams. */
static void uvcEventDeliver(struct NepClassVideo *ncv, const UBYTE *packet, ULONG len)
{
    const struct UsbVideoStatusVS *sp = (const struct UsbVideoStatusVS *) packet;
    BOOL fromstream = (len > offsetof(struct UsbVideoStatusVS, bOriginator)) &&
                      ((sp->bStatusType & UVSPT_MASK) == UVSPT_VIDEOSTREAMING);
    struct List served;
    struct IOStdReq *ioreq;
    struct IOStdReq *next;

    NewList(&served);
    Forbid();
    ioreq = (struct IOStdReq *) ncv->ncv_Status.ust_Queue.lh_Head;
    while((next = (struct IOStdReq *) ioreq->io_Message.mn_Node.ln_Succ))
    {
        struct NepVideoStream *ns = (struct NepVideoStream *) ioreq->io_Unit;
        BOOL forit = !fromstream || !ns || (ns->ns_IfNum == sp->bOriginator);

        /* its first request takes the packet, its others wait for the next */
        if(forit && !uvcEventHasUnit(&served, ioreq->io_Unit))
        {
            Remove(&ioreq->io_Message.mn_Node);
            AddTail(&served, &ioreq->io_Message.mn_Node);
        }
        ioreq = next;
    }
    while((ioreq = (struct IOStdReq *) RemHead(&served)))
    {
        ioreq->io_Actual = (len < ioreq->io_Length) ? len : ioreq->io_Length;
        memcpy(ioreq->io_Data, packet, ioreq->io_Actual);
        uvcEventReply(ioreq, 0);
    }
    Permit();
}
/* \\\ */

/* /// "uvcEventWait()" */
/* A request starts to wait for the next event. The caller has made sure the
   binding stays: the device by looking it up under Forbid(), the window by
   being its own. Returns RC_DONTREPLY, or the error. */
LONG uvcEventWait(struct NepClassVideo *ncv, struct IOStdReq *ioreq)
{
    LONG ret = UVERR_NOSTATUS;

    if(!ioreq->io_Data)
    {
        return IOERR_BADLENGTH;
    }
    Forbid();
    if(ncv->ncv_Status.ust_Pipe)
    {
        ioreq->io_Flags &= ~IOF_QUICK;
        ioreq->io_Message.mn_Node.ln_Type = NT_MESSAGE;
        AddTail(&ncv->ncv_Status.ust_Queue, &ioreq->io_Message.mn_Node);
        ret = RC_DONTREPLY;
    }
    Permit();
    return ret;
}
/* \\\ */

/* /// "uvcEventAbort()" */
/* Call a waiting request back. TRUE = it was waiting and has been replied. */
BOOL uvcEventAbort(struct NepClassVideo *ncv, struct IOStdReq *ioreq)
{
    struct IOStdReq *iocmp;
    BOOL found = FALSE;

    Forbid();
    iocmp = (struct IOStdReq *) ncv->ncv_Status.ust_Queue.lh_Head;
    while(iocmp->io_Message.mn_Node.ln_Succ && (iocmp != ioreq))
    {
        iocmp = (struct IOStdReq *) iocmp->io_Message.mn_Node.ln_Succ;
    }
    if(iocmp->io_Message.mn_Node.ln_Succ)
    {
        Remove(&ioreq->io_Message.mn_Node);
        uvcEventReply(ioreq, IOERR_ABORTED);
        found = TRUE;
    }
    Permit();
    return found;
}
/* \\\ */

/* /// "uvcEventFlush()" */
/* The requests of one unit that still wait go back to its opener */
void uvcEventFlush(struct NepClassVideo *ncv, struct Unit *unit, LONG error)
{
    struct IOStdReq *ioreq;
    struct IOStdReq *next;

    Forbid();
    ioreq = (struct IOStdReq *) ncv->ncv_Status.ust_Queue.lh_Head;
    while((next = (struct IOStdReq *) ioreq->io_Message.mn_Node.ln_Succ))
    {
        if(ioreq->io_Unit == unit)
        {
            Remove(&ioreq->io_Message.mn_Node);
            uvcEventReply(ioreq, error);
        }
        ioreq = next;
    }
    Permit();
}
/* \\\ */

/* /// "uvcStatusStart()" */
/* At bind, before the units appear: the queue, and the first transfer if the
   camera has a status endpoint. One transfer is one packet of the endpoint,
   which is one event. */
void uvcStatusStart(struct NepClassVideo *ncv)
{
    struct PsdEndpoint *pep;
    IPTR pktsize = 0;

    NewList(&ncv->ncv_Status.ust_Queue);
    pep = psdFindEndpoint(ncv->ncv_Interface, NULL,
                          EA_IsIn, TRUE,
                          EA_TransferType, USEAF_INTERRUPT,
                          TAG_END);
    if(!pep)
    {
        return;
    }
    psdGetAttrs(PGA_ENDPOINT, pep, EA_MaxPktSize, &pktsize, TAG_END);
    if(pktsize && (ncv->ncv_Status.ust_Buf = psdAllocVec(pktsize)))
    {
        if((ncv->ncv_Status.ust_Pipe = psdAllocPipe(ncv->ncv_Device, ncv->ncv_TaskMsgPort, pep)))
        {
            /* an event is as long as it is */
            psdSetAttrs(PGA_PIPE, ncv->ncv_Status.ust_Pipe,
                        PPA_AllowRuntPackets, TRUE,
                        TAG_END);
            ncv->ncv_Status.ust_Size = pktsize;
            psdSendPipe(ncv->ncv_Status.ust_Pipe, ncv->ncv_Status.ust_Buf, ncv->ncv_Status.ust_Size);
            return;
        }
        psdFreeVec(ncv->ncv_Status.ust_Buf);
        ncv->ncv_Status.ust_Buf = NULL;
    }
}
/* \\\ */

/* /// "uvcStatusDone()" */
/* The transfer came back: pass the packet on and wait for the next. An
   endpoint that keeps failing is given up - the camera works without. */
void uvcStatusDone(struct NepClassVideo *ncv)
{
    LONG ioerr = psdGetPipeError(ncv->ncv_Status.ust_Pipe);

    if(!ioerr)
    {
        ULONG len = psdGetPipeActual(ncv->ncv_Status.ust_Pipe);

        ncv->ncv_Status.ust_Errors = 0;
        /* an empty packet is not an event; the C930e sends one now and then */
        if(len)
        {
            uvcEventDeliver(ncv, ncv->ncv_Status.ust_Buf, len);
        }
    }
    else if(++ncv->ncv_Status.ust_Errors >= STATUS_MAX_ERRORS)
    {
        struct PsdPipe *pp = ncv->ncv_Status.ust_Pipe;
        struct IOStdReq *ioreq;

        KPRINTF(10, ("Status endpoint given up after error %ld\n", ioerr));
        /* who waits is told that there is nothing to wait for any more */
        Forbid();
        ncv->ncv_Status.ust_Pipe = NULL;
        while((ioreq = (struct IOStdReq *) RemHead(&ncv->ncv_Status.ust_Queue)))
        {
            uvcEventReply(ioreq, UVERR_NOSTATUS);
        }
        Permit();
        psdFreePipe(pp);
        return;
    } else {
        KPRINTF(5, ("Status transfer failed: %ld\n", ioerr));
        psdDelayMS(20);
    }
    psdSendPipe(ncv->ncv_Status.ust_Pipe, ncv->ncv_Status.ust_Buf, ncv->ncv_Status.ust_Size);
}
/* \\\ */

/* /// "uvcStatusStop()" */
/* At release, after the units have been cut off and the window has gone:
   nothing new can come to wait, and what still does fails */
void uvcStatusStop(struct NepClassVideo *ncv)
{
    struct IOStdReq *ioreq;

    if(ncv->ncv_Status.ust_Pipe)
    {
        psdAbortPipe(ncv->ncv_Status.ust_Pipe);
        psdWaitPipe(ncv->ncv_Status.ust_Pipe);
        psdFreePipe(ncv->ncv_Status.ust_Pipe);
        ncv->ncv_Status.ust_Pipe = NULL;
    }
    Forbid();
    while((ioreq = (struct IOStdReq *) RemHead(&ncv->ncv_Status.ust_Queue)))
    {
        uvcEventReply(ioreq, UVERR_GONE);
    }
    Permit();
    psdFreeVec(ncv->ncv_Status.ust_Buf);
    ncv->ncv_Status.ust_Buf = NULL;
}
/* \\\ */
