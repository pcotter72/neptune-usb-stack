#ifndef CAMDUSBMIDI_H
#define CAMDUSBMIDI_H

#include <exec/types.h>
#include <exec/libraries.h>
#include <exec/lists.h>
#include <utility/hooks.h>
#include <midi/camddevices.h>   /* CAMDXmitFunc, CAMDRecvFunc */

/* Per-port state of the MIDI byte stream -> USB-MIDI event packet converter (nTxByte,
 * camdusbmidi.class.c, which describes both formats). A packet holds one whole message, so
 * the bytes of a message are held here until its last byte arrives. */
#define USBMIDI_TX_UNKNOWN   0   /* no message in progress; data bytes are dropped */
#define USBMIDI_TX_1PARAM    1   /* status held, one data byte completes the message */
#define USBMIDI_TX_2PARAM_1  2   /* status held, two data bytes to come */
#define USBMIDI_TX_2PARAM_2  3   /* status and first data byte held, one more to come */
#define USBMIDI_TX_SYSEX_0   4   /* inside SysEx, no byte held */
#define USBMIDI_TX_SYSEX_1   5   /* inside SysEx, one byte held (a packet takes three) */
#define USBMIDI_TX_SYSEX_2   6   /* inside SysEx, two bytes held */

struct UsbMidiTx
{
    UBYTE               tx_State;      /* USBMIDI_TX_* */
    UBYTE               tx_Status;     /* status byte of the message being assembled; kept
                                          after a channel message for running status */
    UBYTE               tx_Data[2];    /* data (or SysEx) bytes held until the packet is full */
};

/* Ports per CAMD device (MidiDeviceData.NPorts of the embedded driver). A USB-MIDI cable
 * number is 4 bits, so 16 covers any device. */
#define CAMD_MAX_PORTS 16

/* One open CAMD port, allocated by usbCAMDOpenPort and freed by usbCAMDClosePort only, so
 * the pointer the driver keeps stays valid across an unplug (which only detaches it). All
 * open ports of all devices are on nh_Ports. The list and the class-owned fields change
 * under Forbid(), and the class walks the list under Forbid(); the driver reads ca_SigTask
 * under Forbid() (camd's senders are tasks, camd.intro) and owns ca_IsOpen/ca_TXActive and
 * the two hooks. */
struct CAMDAdapter
{
    struct Node         ca_Node;       /* on nh_Ports */
    struct NepClassMidi *ca_Binding;   /* owning binding; NULL = device gone */
    UBYTE               ca_IsOpen;     /* driver side ready: hooks valid, camd functions callable */
    UBYTE               ca_TXActive;   /* camd has output queued (ActivateXmit) until the end marker */
    UBYTE               ca_PortNum;    /* CAMD port number = USB-MIDI cable number */
    struct Task        *ca_SigTask;    /* class task woken by ActivateXmit; NULL once detached */
    ULONG               ca_SigMask;
    CAMDXmitFunc        ca_TXFunc;     /* camd's transmit function */
    CAMDRecvFunc        ca_RXFunc;     /* camd's receive function */
    APTR                ca_UserData;
    struct Hook         ca_CAMDRXFunc; /* driver: deliver bytes to camd (a2 = bytes, a1 = count) */
    struct Hook         ca_CAMDTXFunc; /* driver: pull bytes from camd (a2 = buffer, a1 = max) -> count */
    struct UsbMidiTx    ca_TX;         /* USB-MIDI packetizer state (class side) */
};

struct NepMidiBase
{
    struct Library      nh_Library;       /* standard */
    UWORD               nh_Flags;         /* various flags */

    BPTR                nh_SegList;       /* load seglist (stored by the class skeleton) */
    struct Library     *nh_UtilityBase;   /* utility base */
    struct List         nh_Bindings;      /* List of bindings created */
    struct List         nh_Ports;         /* open CAMD ports (struct CAMDAdapter) */
};

struct NepClassMidi
{
    struct Node         nch_Node;         /* Node linkage */
    struct NepMidiBase *nch_ClsBase;      /* Up linkage */
    struct Library     *nch_Base;         /* neptune.library base */
    struct PsdDevice   *nch_Device;       /* Up linkage */
    struct PsdConfig   *nch_Config;       /* Up linkage */
    struct PsdInterface *nch_Interface;   /* Up linkage */
    struct PsdEndpoint *nch_EPIn;         /* Bulk IN endpoint (MIDI data from the device) */
    struct PsdPipe     *nch_EPInPipe;     /* its pipe */
    struct PsdEndpoint *nch_EPOut;        /* Bulk OUT endpoint */
    struct PsdPipe     *nch_EPOutPipe;    /* its pipe */
    UBYTE              *nch_EPInBuf;      /* Packet buffer for bulk IN */
    UBYTE              *nch_EPOutBuf;     /* Packet buffer for bulk OUT */
    struct Task        *nch_ReadySigTask; /* Task to send ready signal to */
    LONG                nch_ReadySignal;  /* Signal to send when ready */
    struct Task        *nch_Task;         /* Subtask */
    struct MsgPort     *nch_TaskMsgPort;  /* Message Port of Subtask */
    IPTR                nch_IfNum;        /* Interface Number */
    UBYTE               nch_InCables;     /* embedded jacks on the IN endpoint (RX cables) */
    UBYTE               nch_OutCables;    /* embedded jacks on the OUT endpoint (TX cables) */
    UBYTE               nch_NumCables;    /* max of both = CAMD ports offered */
    UBYTE               nch_ShortID[32];  /* generated ID string for driver */
};

#endif /* CAMDUSBMIDI_H */
