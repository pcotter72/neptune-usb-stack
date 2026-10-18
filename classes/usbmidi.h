/* usbmidi.h - USB MIDI 1.0 helpers shared by the MIDIStreaming classes (camdusbmidi,
 * simplemidi). Include after common.h: the helpers call poseidon.library through `ps`. */
#ifndef USBMIDI_H
#define USBMIDI_H

#include <devices/usb_audio.h>

/* MIDI 1.0 status bytes, named as in CAMD's <midi/mididefs.h> (same spelling, so including
   both is harmless). A byte with MIDI_STATUSBIT set is a status byte, else a data byte. */
#define MIDI_STATUSBIT 0x80
#ifndef MIDI_MIDIDEFS_H
#define MS_Prog       0xC0
#define MS_ChanPress  0xD0
#define MS_StatBits   0xF0
#define MS_System     0xF0
#define MS_SysEx      0xF0
#define MS_QtrFrame   0xF1
#define MS_SongPos    0xF2
#define MS_SongSelect 0xF3
#define MS_TuneReq    0xF6
#define MS_EOX        0xF7
#define MS_RealTime   0xF8
#endif

/* Return TRUE unless the interface's class-specific MS header says it is not MIDIStreaming
   1.0. A USB MIDI 2.0 device carries its 1.0 function on alternate setting 0 (bcdMSC 0x0100)
   and the Universal MIDI Packet one on alternate 1 (0x0200), which the MIDI 1.0 classes
   cannot read. No header is taken as 1.0. A skipped interface is logged under `origin`. */
static inline BOOL nIsMidi10(struct Library *ps, struct PsdInterface *pif, STRPTR origin)
{
    struct PsdConfig *pc;
    struct PsdDevice *pd;
    struct UsbMIDIStreamingHeaderDesc *hdr = NULL;
    APTR desc;
    UWORD bcdmsc;

    psdGetAttrs(PGA_INTERFACE, pif, IFA_Config, &pc, TAG_END);
    psdGetAttrs(PGA_CONFIG, pc, CA_Device, &pd, TAG_END);
    desc = psdFindDescriptor(pd, NULL,
                             DDA_Interface, pif,
                             DDA_DescriptorType, UDT_CS_INTERFACE,
                             DDA_CS_SubType, UDST_AUDIO_MIDI_HEADER,
                             TAG_END);
    if(desc)
    {
        psdGetAttrs(PGA_DESCRIPTOR, desc, DDA_DescriptorData, &hdr, TAG_END);
    }
    if(!hdr)
    {
        return TRUE;
    }
    bcdmsc = hdr->bcdMSC0 | (hdr->bcdMSC1 << 8);
    if(bcdmsc == 0x0100)
    {
        return TRUE;
    }
    psdAddErrorMsg(RETURN_WARN, origin,
                   "MIDIStreaming %lx.%02lx interface skipped, only 1.0 is supported.",
                   (ULONG) (bcdmsc >> 8), (ULONG) (bcdmsc & 0xff));
    return FALSE;
}

#endif /* USBMIDI_H */
