#ifndef SIMPLEMIDI_CLASS_H
#define SIMPLEMIDI_CLASS_H

/*
 *----------------------------------------------------------------------------
 *                       Includes for simplemidi class
 *----------------------------------------------------------------------------
 *                   By Chris Hodges <chrisly@platon42.de>
 */

#include "common.h"

#include <devices/keyboard.h>
#include <libraries/gadtools.h>

#include <devices/usb_audio.h>

#include <string.h>
#include <stddef.h>
#include <stdio.h>

#include "simplemidi.h"

/* Protos */

struct NepClassSimpleMidi * usbAttemptInterfaceBinding(struct NepSimpleMidiBase *nh, struct PsdInterface *pif);
struct NepClassSimpleMidi * usbForceInterfaceBinding(struct NepSimpleMidiBase *nh, struct PsdInterface *pif);
void usbReleaseInterfaceBinding(struct NepSimpleMidiBase *nh, struct NepClassSimpleMidi *nch);

void nParseMidi(struct NepClassSimpleMidi *nch, UBYTE *buf, ULONG len);

struct NepClassSimpleMidi * nAllocMidi(void);
void nFreeMidi(struct NepClassSimpleMidi *nch);

BOOL nLoadClassConfig(struct NepSimpleMidiBase *nh);
LONG nOpenCfgWindow(struct NepSimpleMidiBase *nh);

void nGUITaskCleanup(struct NepSimpleMidiBase *nh);

void nMidiTask();
void nGUITask();

#endif /* SIMPLEMIDI_CLASS_H */
