#ifndef CAMDUSBMIDI_CLASS_H
#define CAMDUSBMIDI_CLASS_H

/*
 *----------------------------------------------------------------------------
 *                       Includes for camdusbmidi class
 *----------------------------------------------------------------------------
 *                   By Chris Hodges <chrisly@platon42.de>
 */

#include "common.h"

#include <devices/usb_audio.h>
#include "usbmidi.h"

#include <string.h>
#include <stddef.h>
#include <stdio.h>

#include "camdusbmidi.h"

/* Protos */

struct NepClassMidi * usbAttemptInterfaceBinding(struct NepMidiBase *nh, struct PsdInterface *pif);
struct NepClassMidi * usbForceInterfaceBinding(struct NepMidiBase *nh, struct PsdInterface *pif);
void usbReleaseInterfaceBinding(struct NepMidiBase *nh, struct NepClassMidi *nch);

void nParseMidi(struct NepClassMidi *nch, UBYTE *buf, ULONG len);
void nTransmit(struct NepClassMidi *nch);
void nWriteDriver(struct NepClassMidi *nch);
UWORD nCableCount(struct NepClassMidi *nch, struct PsdEndpoint *pep);

struct NepClassMidi * nAllocMidi(void);
void nFreeMidi(struct NepClassMidi *nch);

void nMidiTask();

#endif /* CAMDUSBMIDI_CLASS_H */
