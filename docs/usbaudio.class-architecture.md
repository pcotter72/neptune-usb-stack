# usbaudio.class - Architecture (reverse-engineered)

> Scope: the **`usbaudio.class`** USB Audio class driver. This is a *consumer* of
> `poseidon.library` (the stack core - see
> [poseidon.library-architecture.md](poseidon.library-architecture.md)) on its lower edge and a
> *provider* to `ahi.device` (the Amiga audio system) on its upper edge. Familiarity with the
> core doc's §7 (class-driver binding) and §5 (RT-ISO / pipes) helps.
>
> Sources reverse-engineered: `classes/audio/usbaudio.class.c` (~3.9k lines),
> `classes/audio/usbaudio.h`, `classes/audio/usbaudio.class.h`, the shared class skeleton
> `classes/class_main.c`, and the headers `include/devices/usb_audio.h` (USB Audio Class 1.0
> descriptors), `include/devices/usbhardware.h` (RT-ISO transport), plus the AHI SDK headers
> `<devices/ahi.h>` / `<libraries/ahi_sub.h>`. Line numbers are indicative.

---

## Table of contents

1. [What this driver is](#1-what-this-driver-is)
2. [Layering](#2-layering)
3. [The dual-library structure](#3-the-dual-library-structure)
4. [Object model](#4-object-model)
5. [Binding lifecycle](#5-binding-lifecycle)
6. [USB-Audio descriptor parsing and the topology model](#6-usb-audio-descriptor-parsing-and-the-topology-model)
7. [AHI mode registration and realization](#7-ahi-mode-registration-and-realization)
8. [The AHI sub-driver ABI](#8-the-ahi-sub-driver-abi)
9. [The live isochronous data path](#9-the-live-isochronous-data-path)
10. [The control path - volume and selectors](#10-the-control-path--volume-and-selectors)
11. [Config GUI and persistence](#11-config-gui-and-persistence)
12. [End-to-end: attach, play, detach](#12-end-to-end-attach-play-detach)
13. [State machines](#13-state-machines)
14. [Notable quirks and refactoring hazards](#14-notable-quirks-and-refactoring-hazards)
15. [Appendix - maps and indexes](#15-appendix--maps-and-indexes)

---

## 1. What this driver is

`usbaudio.class` bridges a **USB Audio Class 1.0** device to **AHI**, the Amiga retargetable
audio API. It does three jobs:

1. **Binds** the USB Audio *Control* interface through the standard Poseidon class protocol
   (`UCM_*` methods), and parses the device's audio *topology* (terminals, feature units,
   selectors) and its *streaming* formats.
2. **Publishes** each usable streaming format as an **AHI audio mode** in the system AHI mode
   database, so any AHI application sees "USB audio" output/record modes.
3. **Streams** PCM between AHI's software mixer and the device's isochronous endpoint, using
   Poseidon's **RT-ISO** (real-time isochronous) handler mechanism, with format conversion and
   sample-rate matching.

The defining structural feature is that it is **two Exec libraries in one binary** (§3): the
Poseidon class (`NepAudioBase`) and an embedded AHI sub-driver named `usbaudio.audio`
(`NepAudioSubLibBase`).

It targets **USB Audio 1.0 only** (`bcdADC == 0x0100`); 2.0 devices are declined. It is a
**post-mixing** AHI driver (`AHISF_MIXING`): AHI does channel mixing in software and hands this
driver a finished buffer to convert and ship.

---

## 2. Layering

```mermaid
flowchart TB
    APP["audio application"]
    AHI["ahi.device, software mixer"]
    subgraph UAC["usbaudio.class binary"]
        CLS["class side, NepAudioBase"]
        AUD["AHI sub-driver, usbaudio.audio"]
    end
    PS["poseidon.library"]
    HCD["HCD device, e.g. xhci.device"]
    DEV[("USB audio device")]

    APP --> AHI
    AHI -->|"AHI mode + subLib* vectors"| AUD
    AUD --- CLS
    CLS -->|"bind, descriptors, EP0 control, RT-ISO"| PS
    AUD -->|"RT-ISO isochronous streaming"| PS
    PS -->|"IOUsbHWReq, UHCMD_*"| HCD
    HCD --> DEV
```

The class sits between two subsystems and speaks a different protocol to each:

* **Down to `poseidon.library`** - the `psd*` API: `usbGetAttrs`/`psdFindInterface`/
  `psdFindDescriptor` (topology), `psdAllocPipe`/`psdDoPipe` (EP0 class requests), and
  `psdAllocRTIsoHandler`/`psdStartRTIso` (isochronous streaming). It *implements* the
  `usbclass` ABI (`usbGetAttrsA`/`usbSetAttrsA`/`usbDoMethodA`) that the core calls.
* **Up to `ahi.device`** - the AHI **sub-driver** ABI (`<libraries/ahi_sub.h>`): the 19
  `subLib*` vectors AHI calls to allocate/start/stop/query a mode, plus `AHI_AddAudioMode` to
  register modes. It opens `ahi.device` *per binding* to register its modes.

---

## 3. The dual-library structure

`usbaudio.class` is simultaneously **two Exec libraries** built from one binary:

| Library | Base struct | `ln_Name` | Role |
|---|---|---|---|
| Poseidon USB class | `NepAudioBase` (`usbaudio.h:144`) | `usbaudio.class` | implements the `usbclass` ABI + MUI GUI |
| AHI sub-driver | `NepAudioSubLibBase` (`usbaudio.h:210`) | `usbaudio.audio` | implements the AHI `ahi_sub` ABI (19 vectors) |

```mermaid
flowchart LR
    subgraph BIN["one binary"]
        CLASS["NepAudioBase, ln_Name usbaudio.class<br/>usbclass ABI plus MUI GUI"]
        AUDLIB["NepAudioSubLibBase, ln_Name usbaudio.audio<br/>AHI ahi_sub ABI, 19 vectors"]
    end
    CLASS -->|"libInit: MakeLibrary then AddLibrary"| AUDLIB
    PS["poseidon.library"] -->|"usbDoMethod UCM_*"| CLASS
    AHI["ahi.device"] -->|"OpenLibrary usbaudio.audio, then subLib* calls"| AUDLIB
    CLASS -->|"psd* API, RT-ISO, EP0 control"| PS
    AUDLIB -.->|"shares address space, AHIDB_NepAudioMode pointer"| CLASS
```

**How the second library is born** (`libInit`, `usbaudio.class.c:59`). After opening
`utility.library`, the class calls `MakeLibrary(SubLibFuncTable, NULL, subLibInit,
sizeof(NepAudioSubLibBase), NULL)` (`:78`), which allocates the jump-table + data regions, builds
the `_LVO` vectors from `SubLibFuncTable[]` (`:23`), and runs `subLibInit` (`:2376`). It then sets
`nh_SubLibBase->nas_ClsBase = nh`, **`AddLibrary`**s the new base into the Exec library list under
the name `usbaudio.audio` (`:87`), and bumps its open count so it can't be expunged while the
class is resident.

**The load-bearing trick:** because the sub-lib is `AddLibrary`'d as `usbaudio.audio`, when AHI
later wants the driver named `"usbaudio"` (the `AHIDB_Driver` string), its
`OpenLibrary("usbaudio.audio", …)` resolves to **this in-memory node** instead of loading a file
from `DEVS:AHI/`. The AHI sub-driver therefore lives *inside* the USB class binary and **shares
its address space** - which is what makes the `AHIDB_NepAudioMode` pointer bridge (§7) work. No
separate `.audio` file is shipped.

The two bases cross-link: `NepAudioBase.nh_SubLibBase` → sub-lib; `NepAudioSubLibBase.nas_ClsBase`
→ class; each `NepAudioMode.nam_SubLibBase` caches the sub-lib too. Teardown is driven by the
class's `libExpunge` (`:109`), which frees all bindings/modes and `RemLibrary`s the sub-lib.

---

## 4. Object model

```mermaid
flowchart TD
    BASE["NepAudioBase - usbaudio.class library base"]
    SUB["NepAudioSubLibBase - usbaudio.audio AHI sub-driver"]
    BASE -->|nh_SubLibBase| SUB
    SUB -->|nas_ClsBase| BASE
    BASE --> UNITS["nh_Units : list of NepClassAudio"]
    UNITS --> NCH["NepClassAudio - one bound AC interface = one AHI unit"]
    NCH --> MODES["nch_AudioModes : list of NepAudioMode"]
    NCH --> AUNITS["nch_AudioUnits : list of NepAudioUnit"]
    NCH --> TASK["nch_Task - per-binding subtask (nAudioTask)"]
    NCH --> AHIREQ["nch_AHIReq - opens ahi.device per binding"]
    MODES --> NAM["NepAudioMode - one AHI mode (playback or record)"]
    AUNITS --> NAU["NepAudioUnit - one AC topology node"]
    NAM -->|"nam_RecStream / nam_PartOf"| NAM2["record NepAudioMode (full-duplex pair, or a record-only mode's alternate)"]
    NAM -->|nam_RootUnit| NAU
    NAM -.->|"registered into"| AHIDB["AHI mode DB (AHIDB_NepAudioMode back-pointer)"]
```

* **`NepClassAudio`** = one **binding** = one bound USB Audio Control interface = one AHI "unit"
  (`nch_UnitNo`). Holds the per-binding subtask, the EP0 control pipe, the AHI device handle, and
  the two lists below. **Cached across re-plug:** it is freed only in `libExpunge`, so its parsed
  modes survive a release/re-bind.
* **`NepAudioMode`** = one **AHI mode** = one usable streaming format (one streaming alt-setting).
  Carries the USB realization state (alt-interface, iso endpoint, RT-ISO handler), the format
  (resolution/channels/frame size), the supported-frequency list, the double-buffers and clock
  fields, and two links: `nam_RecStream` from a playback mode to the record stream that goes
  with it (full duplex), `nam_PartOf` from a record stream to the AHI mode it belongs to.
* **`NepAudioUnit`** = one **USB-Audio topology node** (Input/Output Terminal, Feature, Selector,
  Mixer, …) with graph adjacency (`nau_InputUnit[]`/`nau_OutputUnit[]`) and the discovered
  control info (`nau_VolumeUnit`, `nau_VolCtrlMask`, min/max dB).

---

## 5. Binding lifecycle

The class implements the `usbclass` ABI via the shared skeleton (`class_main.c`): the LVO table
exposes `usbGetAttrsA`/`usbSetAttrsA`/`usbDoMethodA`. `usbDoMethodA` (`:455`) is the method
dispatcher the core calls:

| `UCM_*` method | Action |
|---|---|
| `UCM_AttemptInterfaceBinding` | `usbAttemptInterfaceBinding` - validate, then bind |
| `UCM_ForceInterfaceBinding` | `usbForceInterfaceBinding` - bind without the class-code check |
| `UCM_ReleaseInterfaceBinding` | `usbReleaseInterfaceBinding` - tear down |
| `UCM_OpenCfgWindow` | `nOpenCfgWindow` - spawn the MUI prefs GUI |
| `UCM_ConfigChangedEvent` | `nLoadClassConfig` - reload IFF prefs |
| `UCM_AttemptSuspendDevice` | refuse (FALSE) **while AHI holds the unit** (`nch_CurrentMode` set, from `AHIsub_AllocAudio` to `FreeAudio`), else allow. The library's idle sweep never asks while a stream runs: `psdStartRTIso` keeps `pd_IOBusyCount` up |
| `UCM_AttemptResumeDevice` | signal the subtask, return TRUE |

`usbGetAttrsA` answers `UGA_CLASS` queries: `UCCA_Priority = 0`, description "USB Audio Streaming
Interface class", `UCCA_HasClassCfgGUI = TRUE`, `UCCA_SupportsSuspend = TRUE`,
`UCCA_AfterDOSRestart = FALSE`.

```mermaid
sequenceDiagram
    autonumber
    participant PS as poseidon core
    participant CLS as usbaudio.class dispatcher
    participant FB as usbForceInterfaceBinding
    participant T as nAudioTask subtask

    PS->>CLS: usbDoMethodA UCM_AttemptInterfaceBinding, pif
    CLS->>CLS: nVerifyAudioStreamDevice, AC class and ADC 1.0 and has AS sibling
    CLS->>FB: usbForceInterfaceBinding nh pif
    note over FB: allocate or reuse NepClassAudio, add to nh_Units,<br/>arm ready handshake
    FB->>T: psdSpawnSubTask nAudioTask, pass nch via tc_UserData
    note over FB: psdBorrowLocksWait on the ready signal
    T->>T: nAllocAudio, open ahi.device, alloc EP0 pipe
    T->>T: nExamineAudioDescriptors, build NepAudioMode list
    T->>T: nExamineAudioUnits, build NepAudioUnit graph
    T->>T: nAddAudioModes, AHI_AddAudioMode for each playback mode
    T-->>FB: signal ready, nch_Task non-NULL means success
    FB-->>PS: binding pointer nch
    note over T: service loop, nch_UpdateFlags drive EP0 SET_CUR
```

Key points:

* **Bindability test** (`nVerifyAudioStreamDevice`, `:162`): the interface must be
  `AUDIO_CLASSCODE` + `AUDIO_CTRL_SUBCLASS` (it binds the **Audio Control** interface, not the
  streaming interfaces), the class-specific AC header must report `bcdADC == 0x0100`, and at least
  one of the AudioStreaming interfaces it collects (`bInCollection`/`baInterfaceNr[]`) must exist.
* **Subtask handshake**: the binding object is created in the caller's context, but all device IO
  runs in a dedicated `nAudioTask` subtask. `usbForceInterfaceBinding` spawns it via
  `psdSpawnSubTask` and blocks in `psdBorrowLocksWait` until the subtask signals back; success is
  read from `nch_Task != NULL` (the subtask sets it only after `nAllocAudio` fully succeeds). This
  is the standard Poseidon "make the async bind synchronous" pattern (core doc §10 borrow-lock).
* **`nAllocAudio`** (`:1968`, subtask context): opens `poseidon.library`, opens **`ahi.device`
  per binding** (`nch_AHIReq`), allocates the EP0 control pipe, then parses descriptors
  (`nExamineAudioDescriptors` → modes; `nExamineAudioUnits` → unit graph) and registers modes
  (`nAddAudioModes`).
* **Release** (`usbReleaseInterfaceBinding`, `:351`): sets `nch_DenyRequests`, removes the binding
  from `nh_Units`, signals the subtask `SIGBREAKF_CTRL_C`, and waits for `nch_Task` to clear
  (`nFreeAudio` removes the AHI modes, frees the units/pipe, closes `ahi.device`). The `nch`
  struct and its mode list are **not** freed here - only in `libExpunge` - preserving them for a
  later re-plug.

---

## 6. USB-Audio descriptor parsing and the topology model

Parsing runs in two stages from `nAllocAudio`: `nExamineAudioDescriptors` builds the **mode list**
(streaming formats), then `nExamineAudioUnits` builds the **unit graph** (control topology) and
links the two.

### 6.1 Mode list - `nExamineAudioDescriptors` (`:1015`)

Walks each AudioStreaming alt-setting's class-specific descriptors:
`UDST_AUDIO_STREAM_GENERAL` (format tag, `bTerminalLink`) and `UDST_AUDIO_STREAM_FMT_TYPE`
(Type-I PCM only). A usable format is mono or stereo of 8, 16 or 17 to 32 bits; for each it
allocates a `NepAudioMode` and fills: `nam_NumChannels`, `nam_Resolution`,
`nam_FrameSize`/`nam_SampleSize` from the format descriptor,
freq/pitch-control flags from the endpoint descriptor, and the supported-frequency list
(`nam_FreqArray[64]`) - discrete from `bSamFreqType`, or a continuous range intersected with a
common-rates table, **clamped to ≤64 kHz** because AHI represents frequency in 16 bits.
`nam_TerminalID = bTerminalLink` is the key matched against the unit graph. The mode-ID base is
set here: `AHI_USB_MODE_BASE + (nch_UnitNo << 12)`, with each mode getting the next sequential ID.

### 6.2 Unit graph - `nExamineAudioUnits` (`:1344`)

A `NepAudioUnit` is created per class-specific AC descriptor (terminals, feature, selector, mixer,
processing, extension), in five stages:

1. **Allocate** one unit per descriptor (`nau_Type`, `nau_UnitID`, raw `nau_Descriptor`).
2. **Connect edges** (`nFindAndConnectAudioUnit`) from each unit's `bSourceID`(s) - wiring
   `nau_InputUnit[]`/`nau_OutputUnit[]` bidirectionally (capped at 8).
3. **Channel/terminal attributes** - `nau_TermType`, `nau_OutChannels`, `nau_ChannelCfg`; a
   *streaming* terminal (`UAUTT_STREAMING`) sets `nau_RootUnit = self` and links the matching
   `NepAudioMode` (a streaming **Input** Terminal → playback mode `nam_IsInput = FALSE`; a
   streaming **Output** Terminal → record mode `nam_IsInput = TRUE`).
4. **Flow propagation** - `nFlowUp`/`nFlowDown` recurse the graph from terminals, propagating
   `nau_RootUnit` and building human-readable `nau_Name`s; `nFlowUpToUSBSource`/
   `nFlowDownToUSBSink` tag every node with the USB streaming terminal it comes from
   (`nau_SourceUnit`) or goes to (`nau_SinkUnit`).
5. **Control discovery** - for each Feature Unit, decode the per-channel control bitmaps and
   classify by graph position: rooted at a USB *input* stream → **master volume**; flows to a USB
   stream → **input gain**; otherwise → **monitor**. Build `nau_VolCtrlMask` (master/left/right)
   and query the dB range over EP0 (`GET_MIN`/`GET_MAX` on the feature unit); `nFindLogVolume`
   turns it into the linear range AHI is told (`nau_MinVolume`/`nau_MaxVolume`, see §10). Selector units attach
   to their chain's sink (`nau_SelectorUnit`). Finally, **record↔playback modes are paired** by
   matching resolution/channels/frame-size/frequency and linked both ways: the playback
   mode's `nam_RecStream`, the record mode's `nam_PartOf` (enabling full-duplex). The modes outlive a binding, so what is paired stays paired at a
   replug. A record mode that no playback mode matches - every mode of a device that only
   records, a camera's microphone for one - becomes an AHI mode of its own (§7.1). Such modes
   of one interface that differ in nothing but the alternate setting are **one** mode: a
   camera has an alternate setting per sample rate. The first of them takes the rates of all
   into its list, and the others point to it with `nam_PartOf` - which for a record mode
   therefore always means "the AHI mode this one belongs to", a playback mode or the record
   mode that leads its group; NULL means it is an AHI mode itself. `nAltForFreq()` finds the alternate that carries a rate.

```mermaid
flowchart LR
    subgraph PLAY["playback chain - USB to speaker"]
        IT1["USB streaming Input Terminal"]
        FU1["Feature Unit, master volume"]
        OT1["Speaker Output Terminal"]
        IT1 --> FU1 --> OT1
    end
    subgraph REC["record chain - mic to USB"]
        IT2["Microphone Input Terminal"]
        FU2["Feature Unit, input gain"]
        OT2["USB streaming Output Terminal"]
        IT2 --> FU2 --> OT2
    end
    IT1 -.->|nau_RootUnit| MP["NepAudioMode playback, nam_IsInput false"]
    OT2 -.->|nau_RootUnit| MR["NepAudioMode record, nam_IsInput true"]
    MP -. "nam_RecStream / nam_PartOf" .- MR
```

---

## 7. AHI mode registration and realization

There are two distinct phases - **declarative registration** at attach time, and **imperative
realization** when an application actually opens a mode.

### 7.1 Registration - `nAddAudioModes` (`:1902`, at attach)

Called from `nAllocAudio`. For each **playback** mode (its record stream comes along, not
as an AHI mode of its own) and for each **record mode that is part of none** (`nam_PartOf`
NULL) it builds an AHI
mode name and a tag list and calls `AHI_AddAudioMode`. A record-only mode is named
"DevName: Rec 16 bit stereo (4 bpf)" and lists the rates of all its alternate settings
(§6.2). `nRecMode()` gives the recording half of a mode
whichever kind it is, `nGetOutputUnit()` the unit with its playback controls - none for a
mode that only records, so it reports no output volume and no outputs.

| Tag | Value |
|---|---|
| `AHIDB_AudioID` | `nam_AHIModeID` (`0x003b0000 + (unit << 12) + n`) |
| `AHIDB_Name` | e.g. "DevName: HiFi 24 bit stereo (6 bpf)" |
| `AHIDB_Driver` | `"usbaudio"` → resolves to the `usbaudio.audio` sub-lib |
| `AHIDB_Volume` / `Panning` / `Stereo` / `HiFi` | capability flags from the topology |
| **`AHIDB_NepAudioMode`** | `(IPTR) nam` - raw back-pointer to the `NepAudioMode` |

**The `AHIDB_NepAudioMode` bridge** (`AHIDB_UserBase+0`): the raw `NepAudioMode *` is stored in
the AHI mode database. Because the sub-driver shares the class's address space (§3), when AHI later
calls `subLibAllocAudio`/`subLibGetAttr` with the mode's tag list, the driver recovers its object
with `GetTagData(AHIDB_NepAudioMode, …)` - no lookup table, just a pointer round-tripped through
AHI. Modes are removed in `nFreeAudio` via `AHI_RemoveAudioMode`.

Registration reserves **no** USB bandwidth - the device stays in its zero-bandwidth alt-setting.

### 7.2 Realization - `nSelectAudioMode` (`:2976`, at AHI open)

```mermaid
sequenceDiagram
    autonumber
    participant APP as audio application
    participant AHI as ahi.device
    participant SUB as usbaudio.audio sub-driver
    participant NS as nSelectAudioMode
    participant DEV as USB device via poseidon

    APP->>AHI: AHI_AllocAudio, a USB mode id
    AHI->>SUB: subLibAllocAudio, tags carry AHIDB_NepAudioMode
    SUB->>SUB: recover nam via GetTagData AHIDB_NepAudioMode
    SUB->>NS: nSelectAudioMode nam
    NS->>DEV: psdSetAltInterface to the bandwidth alt-setting
    NS->>DEV: EP0 SET_CUR sampling frequency, class request
    NS->>DEV: psdAllocRTIsoHandler on the iso endpoint
    SUB->>SUB: size buffers, install player int, pick conversion hook
    SUB-->>AHI: AHISF_MIXING and capability flags
    APP->>AHI: start playback
    AHI->>SUB: subLibStart AHISF_PLAY
    SUB->>DEV: psdStartRTIso, streaming begins
```

`nSelectAudioMode` is where a mode is physically programmed onto the device: switch to the
bandwidth-bearing alt-interface (`psdSetAltInterface(nam_Interface)`), find the iso endpoint,
program the sampling frequency over EP0 (`SET_CUR(SAMPLING_FREQ_CONTROL)`), and
`psdAllocRTIsoHandler` on the endpoint (installing the data-path hooks of §9). `subLibFreeAudio`
reverses this, switching back to the **zero-bandwidth** alt-setting (`nam_ZeroBWIF`) so an idle
device reserves no USB isochronous bandwidth.

---

## 8. The AHI sub-driver ABI

`SubLibFuncTable[]` (`:23`) is the LVO vector array. Entries 0-3 are the Exec life-cycle vectors
(`subLibOpen/Close/Expunge/Reserved`); entries 4-18 are the AHI `ahi_sub` ABI:

| Vector | AHI meaning | What it does here |
|---|---|---|
| `subLibAllocAudio` | claim hardware for a mode | recover `nam`, open poseidon/timer/EP0, `nSelectAudioMode`, size buffers, install player int + conversion hook; set `AHISF_MIXING\|KNOWSTEREO\|KNOWHIFI` (+`CANRECORD` if it has a record stream) |
| `subLibFreeAudio` | release hardware | stop+free RT-ISO, free buffers, **revert to zero-bandwidth alt-interface**, close everything |
| `subLibDisable` / `subLibEnable` | bracket the audio interrupt | **no-ops** (the data path runs on the RT-ISO/USB interrupt + a soft-int, not a driver-owned IRQ) |
| `subLibStart` | begin streaming | `psdStartRTIso` (or `Cause` the fallback player); start the record stream on `AHISF_RECORD` |
| `subLibUpdate` | player frequency changed | recompute `nam_PlayerFrac` |
| `subLibStop` | halt streaming | `psdStopRTIso` (or abort the fallback timer) |
| `subLibSetVol`/`SetFreq`/`SetSound`/`SetEffect`/`LoadSound`/`UnloadSound` | per-channel mixing ops | **stubs** returning `AHIS_UNKNOWN` - AHI does the mixing; this driver only consumes the finished buffer |
| `subLibGetAttr` | query mode capabilities | answer `AHIDB_Bits`, the frequency list, record/full-duplex flags, and volume/gain/monitor ranges + input/output names from the `NepAudioUnit` graph |
| `subLibHardwareControl` | live mixer/monitor/input/output controls | store the value, set a `nch_UpdateFlags` bit, and `Signal` the class subtask (the actual USB `SET_CUR` is issued there - §10) |

`subLibInit`/`Open`/`Close`/`Expunge` are the standard Exec library life-cycle for the
`usbaudio.audio` base (version `AHI_SUB_LIB_VERSION = 4`); its seglist is NULL (created by
`MakeLibrary`, not `LoadSeg`), so real teardown is driven by the class's `RemLibrary`.

---

## 9. The live isochronous data path

This is the heart of the driver: moving PCM between AHI's mixer and the device's isochronous
endpoint, one USB frame at a time, through Poseidon's RT-ISO callbacks.

### 9.1 The RT-ISO contract

The HCD owns the isochronous schedule. The class registers a `struct IOUsbHWRTIso` of `Hook *`
callbacks (`psdAllocRTIsoHandler`); on each iso frame the RT-ISO engine calls a hook with a
`struct IOUsbHWBufferReq` (`ubr_Buffer`/`ubr_Length`/`ubr_Frame`/`ubr_Flags`). The hooks are:
`urti_OutReqHook` → `nOutReqHook` (playback), `urti_InReqHook` → `nInReqHook` and
`urti_InDoneHook` → `nInDoneHook` (record), plus a `RTA_ReleaseHook` → `nReleaseHook`
(device-removal). RT-ISO uses the **task path**, not QuickIO (core doc §5 / RT-ISO).

> **Transport note (unchanged for this class).** The `struct IOUsbHWRTIso`/`IOUsbHWBufferReq`
> contract above is the class-facing API and is stable. On a **context HCD** (xhci.device) the
> library lowers it onto the generalized clock-driven iso-hook ops
> (`NSCMD_USB_REGISTER_HOOKS`/`START_STREAM`, ABI doc §10.3) that Phase 7 introduced - passing the
> class's `IOUsbHWRTIso` block as the hook object, so `nOutReqHook` & co. run **byte-for-byte
> unchanged, with no trampoline**. This superseded the Phase-5 interim RT-ISO re-key ops; usbaudio
> needed no edit. On a legacy HCD the contract goes to the driver as before.

### 9.2 Playback

```mermaid
sequenceDiagram
    autonumber
    participant HCD as HCD RT-ISO engine
    participant OUT as nOutReqHook, iso interrupt
    participant PI as player softint, subLibPlayerIntV4 or V6
    participant AHI as AHI mixer
    participant CONV as nam_SamConvHook
    participant BUF as nam_PlayBuffer 0 and 1

    HCD->>OUT: per iso frame, IOUsbHWBufferReq
    note over OUT: decrement nam_PlayerTimer by nam_Interval
    alt player tick due
        OUT->>PI: Cause nam_PlayerInt, deferred soft-int
        PI->>AHI: ahiac_PlayerFunc then ahiac_MixerFunc into nam_MixBuffer
        PI->>CONV: convert ahiac_BuffSamples into play buffer nam_FillBuf
        PI->>BUF: flip, nam_SendBuf = written, nam_FillBuf = other
    end
    note over OUT: if nam_SendLeft is used up, latch nam_PlayBuffer of nam_SendBuf into ubr_Buffer
    note over OUT: advance 16.16 nam_SendCarry by nam_FramesPerMS times nam_Interval,<br/>ubr_Length = whole frames times nam_FrameSize
    OUT-->>HCD: ubr_Buffer and ubr_Length for this frame
```

The split is deliberate: `nOutReqHook` runs in **iso-interrupt context** and only *schedules*
mixing via `Cause()`; the heavy AHI mix runs in the lower-priority **player soft-int**
(`subLibPlayerIntV4`/`V6`), which calls `ahiac_PlayerFunc`, then `nMixAndConvert`:
`ahiac_MixerFunc` into `nam_MixBuffer`, `nam_SamConvHook` to convert into the play buffer
`nam_FillBuf` names, and the **flip** of the two indices. The interrupt hook then feeds the
play buffer `nam_SendBuf` names to the wire.

* **Double-buffering is playback-only** (`nam_PlayBuffer[2]`); record has one
  `nam_PacketBuffer`.
* **Sample-rate matching** uses a **16.16 fractional accumulator**: `nam_FramesPerMS` is
  frames per millisecond at the mix rate; each frame `nam_SendCarry += nam_FramesPerMS ×
  nam_Interval` and the whole part × `nam_FrameSize` is that frame's `ubr_Length`, with the
  fractional remainder carried across frames *and* buffer flips (`nam_PlayCarry[]`). This
  reconciles the fixed USB frame cadence with an arbitrary AHI mix rate without drift.
* **What one player pass mixes** comes from `nPlayerTick`, shared with the timer clock: the
  milliseconds to the next player call (`nam_PlayerMS`, from `nam_PlayerFrac` with the part of a
  millisecond carried) × `nam_FramesPerMS`, whole and part frames kept apart so a slow player at
  a high rate cannot overflow 16.16. It is capped at `nam_MaxMixFrames`, which `nam_MixBuffer`
  and the play buffers hold: `nMaxMixFrames` sizes them at `subLibAllocAudio` for the slowest
  player rate the program gave AHI (its rate or its minimum), at least 100 ms per millisecond of
  interval. `ahi.doc` allows a pass up to 65535 frames; the cap only acts on a program that goes
  below the minimum it declared.

### 9.3 Record

Simpler and clock-driven by the callbacks (no soft-int): `nInReqHook` hands the HCD the fixed
`nam_PacketBuffer`, which holds one service interval - `nam_IntervalBytes`, set in
`nSelectAudioMode` from the endpoint (max packet size × transactions per microframe; the
companion's bytes per interval at SuperSpeed) - and cuts `ubr_Length` to that. After the
interval arrives `nInDoneHook` converts the received bytes (a `nRec*` hook) into
`nam_RecBuffer` as **16-bit stereo**. Each of its two blocks has room for `REC_BLOCK_FRAMES`
and one service interval more (`nRecBlockFrames`), so the interval that fills a block up
arrives whole. AHI gets them via `ahiac_SamplerFunc` with a
persistent `AHIRecordMessage` (`AHIST_S16S`) **in blocks of `REC_BLOCK_FRAMES` (1024) frames,
from two buffers in turn**, not interval by interval: the block handed over stays as it is
until the next one goes. AHI's other drivers hand over blocks of that size; the few frames a
millisecond brings, each time in the same buffer, sounded robotic in a recording; in blocks the same recording is clean. The record message itself keeps the block being filled and its fill
count. When a recording is stopped the class logs the frames that arrived against what the
rate should have brought in that time - the one measurement of this path; lost intervals
show there as too few frames. The record node is the `nam_RecStream` of a
playback node, or the mode itself if it only records. That is why the record buffer is not
`nam_MixBuffer`: a mode that only records needs both, the one AHI mixes into (and nobody
hears) and the one the recorded samples are handed over in.

### 9.4 Sample conversion table

One `nam_SamConvHook` whose entry `nConverter` takes from a table by `nam_SampleSize` (1-4) and
`nam_NumChannels` (1-2), `PlayConverters` or `RecConverters`:

* **Playback** `nConv{8,16,24,32}Bit{Mono,Stereo}` - AHI native (16/32-bit) → little-endian USB
  wire format.
* **Record** `nRec{8,16,24,32}Bit{Mono,Stereo}` - USB wire → AHI, always widened to 16-bit
  stereo (`AHIST_S16S`): an 8-bit sample becomes the high byte (until 6.3 it went in as the low
  byte, 48 dB too quiet), 24/32-bit keep their top 16 bits.

### 9.5 Player clock and the fallback timer

Normally the **USB SOF is the clock** (the iso `nOutReqHook` drives the player soft-int). If the
device is **unplugged mid-stream**, `nReleaseHook` stops the RT-ISO stream - the USB clock is gone
- but AHI must keep being serviced until the application stops. So it swaps the player int's code
to `subLibPlayerIntDummy`, sets `nam_FallbackTimer`, and - if AHI is playing (`nam_Playing`;
otherwise `subLibStart` does it) - arms a `timer.device` loop whose reply port is a
`PA_SOFTINT` port targeting the player int. Each timer reply re-triggers the soft-int, which
mixes and **discards** the output - a self-sustaining timer→soft-int→timer loop that drains AHI
to silence rather than hanging.

**A mode that only records has the timer for its clock from the start** (`subLibAllocAudio`
sets `nam_FallbackTimer` and the dummy player): AHI's player and mixer have to be called
whether or not anything is heard, or a program that plays on such a mode would wait for
ever. Two things follow, and hold for the unplugged sound card as well:

* The soft-int takes the timer request off the port (`GetMsg`), works out the next player pass
  with `nPlayerTick` (its part frame carried in `nam_SendCarry`, which nothing sends on the timer
  clock), sends the request for `nam_PlayerMS` at once - before mixing, so the mix does not
  stretch the interval - and then mixes that many frames.
* `nAbortTimer()` is the one way the timer is stopped. A record-only mode has the timer for a
  clock before it was ever started, and `AbortIO()` is only for a request that was sent
  (NDK `exec.doc`): the request counts as sent once the soft-int has put its command into
  it. `subLibStart` aborts before it starts, because AHI may start what already runs.
  timer.device's `AbortIO()` runs in the caller's context (NDK `timer.doc`), so the request is
  back in the port when it returns; `WaitIO()` in `subLibFreeAudio` and the restart in
  `subLibStart` depend on that, since this port has no task or signal to wait on.
  `subLibStop` aborts and leaves the reply in the port; `subLibFreeAudio` aborts and collects
  it with `WaitIO()` - one removal, never two.

---

## 10. The control path - volume and selectors

Volume/gain/monitor/input/output changes do **not** travel the audio data path. `subLibHardwareControl`
(called by AHI in its own context) stores the new value in the `NepAudioMode`, sets a `UAF_*` bit
in `nch_UpdateFlags`, and `Signal`s the binding's subtask. `nAudioTask`'s service loop (`:586`)
wakes, and for the current mode issues the corresponding **USB Audio class `SET_CUR` request on
the EP0 pipe**:

* `UAF_MASTER_VOLUME` / `UAF_INPUT_GAIN` / `UAF_MONITOR_VOLUME` → `nSetVolume` on the mode's
  output volume unit(s) (`nam_OutVolume`, else every output's own `nao_Volume`, see
  "Several outputs" in §14), or the input's volume or monitor unit (via `nGetInputUnit`):
  `SET_CUR(VOLUME)` per channel in `nau_VolCtrlMask` (bit 0 the master). A master Control is
  cascaded after the channel Controls (UAC 1.0 §3.5.5), so a unit with one gets the level on the
  master and its channels at 0 dB; a unit without gets the level on each channel. Until 6.3 every
  Control got the level, and a unit with both applied it twice.

**Units.** AHI's gains and volumes are linear factors (`Fixed`, 1.0 = unchanged); a Volume
Control works in 1/256 dB, 0x8001 (-127.9961 dB) to 0x7FFF (+127.9961 dB), 0x8000 being silence
for `CUR` only (UAC 1.0 §5.2.2.4.3.2). `nFindLogVolume` reports the unit's range to AHI as factors
(`nDbToFixed`: 10^(dB/20)), so a slider spans exactly what the device can do; `nSetVolume`
converts AHI's factor back (`nFixedToDb`: 20·log10) and clamps it to the unit's range - 6 dB per
halving of the factor. Both are integer (the stack has no floating point): one table of
2^(i/32) with linear interpolation, within 0.03 dB wherever 16.16 resolves it (above about
-48 dB); `Fixed` itself holds -96 to +90 dB, and a range beyond that is cut to it. Until 6.3 the
range was reported as 0..1.0 and the factor spread evenly over the dB range, so a factor of 0.5
landed halfway down it: the slider lost most of the level in its first steps.
* `UAF_SELECT_INPUT` → `SET_CUR` on the recording path's selector unit.
* `UAF_SELECT_OUTPUT` → `nSetMute` on each output's own Mute (`nao_Mute`): all unmuted for
  "All outputs", else every output but the chosen one muted. Set only for a mode whose outputs
  can be switched (`nam_OutputSwitch`).

This keeps all blocking USB control traffic in the subtask, out of AHI's (and the iso interrupt's)
context.

---

## 11. Config GUI and persistence

`UCM_OpenCfgWindow` → `nOpenCfgWindow` (`:2133`) spawns the MUI GUI subtask `nGUITask` (`:2158`),
guarded against a second instance by `nh_GUITask`. The window currently exposes **no real
settings** (a "None" placeholder) - the machinery exists for future options.

Two notable mechanisms:

* **Per-instance MUI base.** Because the class is ROM-residentable it can't use a writable global
  MUI base. `nGUITask` rebinds `MUIMasterBase` as a task-local macro that fishes the base out of
  `ThisTask->tc_UserData->nh_MUIBase` (via `mui_base.h`, bound by
  `MUI_BASE_USERDATA`/`MUI_BASE_FIELD` in `usbaudio.h`). It also force-includes `mui_compat.h`
  to replace the SDK's `__inline MUI_NewObject`, which passes `&tags` and is broken at any
  optimization level.
* **Persistence** uses Poseidon's IFF config store: `nLoadClassConfig` reads the `ClsGlobalCfg`
  chunk via `psdGetClsCfg`/`psdGetCfgChunk` (length-clamped for forward/back compat); the GUI
  writes it back with `psdAddCfgEntry` + `psdSaveCfgToDisk`.

---

## 12. End-to-end: attach, play, detach

```mermaid
sequenceDiagram
    autonumber
    participant PS as poseidon core
    participant CLS as usbaudio.class
    participant AHIDB as AHI mode DB
    participant APP as audio app
    participant DEV as USB device

    Note over PS,DEV: ATTACH
    PS->>CLS: UCM_AttemptInterfaceBinding
    CLS->>CLS: bind, spawn nAudioTask, parse descriptors and units
    CLS->>AHIDB: AHI_AddAudioMode for each playback mode
    Note over PS,DEV: PLAY
    APP->>CLS: AHI_AllocAudio then start, via subLib*
    CLS->>DEV: select bandwidth alt-IF, set sample rate, start RT-ISO
    DEV-->>CLS: iso frames drive nOutReqHook and the player soft-int
    Note over PS,DEV: DETACH while playing
    PS->>CLS: device removed, RT-ISO release hook
    CLS->>CLS: nReleaseHook stops RT-ISO, switch to fallback timer clock
    CLS->>APP: AHI keeps draining to silence until app stops
    APP->>CLS: AHI_FreeAudio, subLibFreeAudio
    PS->>CLS: UCM_ReleaseInterfaceBinding, nFreeAudio, AHI_RemoveAudioMode
```

---

## 13. State machines

Two small state machines are worth drawing. (The class reuses Poseidon's binding/subtask patterns;
those FSMs are in the core doc §13-14.)

**Per-mode lifecycle** - registered (advertised) → allocated (USB programmed) → streaming:

```mermaid
stateDiagram-v2
    [*] --> Registered: nAddAudioModes, AHI_AddAudioMode
    Registered --> Allocated: subLibAllocAudio, nSelectAudioMode, RT-ISO allocated
    Allocated --> Streaming: subLibStart
    Streaming --> Allocated: subLibStop
    Allocated --> Registered: subLibFreeAudio, revert to zero bandwidth
    Registered --> [*]: nFreeAudio, AHI_RemoveAudioMode
```

**Streaming clock source** - normal (USB-driven) vs fallback (timer-driven after removal):

```mermaid
stateDiagram-v2
    [*] --> Stopped
    Stopped --> Streaming: subLibStart, psdStartRTIso
    Streaming --> Stopped: subLibStop, psdStopRTIso
    Streaming --> Fallback: nReleaseHook, device removed mid-stream
    Stopped --> Fallback: subLibStart after the device was removed
    note right of Streaming
      USB SOF drives nOutReqHook,
      which Causes the player soft-int
    end note
    note right of Fallback
      player int swapped to subLibPlayerIntDummy,
      timer.device reply re-triggers the soft-int,
      AHI drains to silence
    end note
    Fallback --> [*]: subLibFreeAudio
    Stopped --> [*]: subLibFreeAudio
```

The **double-buffer ping-pong** (`nam_FillBuf`/`nam_SendBuf`, flipped in the player soft-int and
consumed in `nOutReqHook`) is a third, implicit state carried by those two indices plus the signed
`nam_SendLeft` drain counter.

---

## 14. Notable quirks and refactoring hazards

* **Dual library in one binary.** The AHI sub-driver is `MakeLibrary`'d and `AddLibrary`'d as
  `usbaudio.audio` in the class's `libInit`; AHI reaches it by name. Both bases share one address
  space - the `AHIDB_NepAudioMode` pointer bridge depends on that. Don't split them.
* **`AHIDB_NepAudioMode` is a raw pointer in the AHI mode DB.** It is only valid while the class is
  resident and the `NepAudioMode` lives; a stale AHI mode entry pointing at a freed mode would
  crash. Mode removal (`AHI_RemoveAudioMode`) must stay paired with `NepAudioMode` lifetime.
* **Bindings are cached across re-plug**, freed only in `libExpunge` - the parsed mode/unit data
  survives release. A refactor that frees `nch` in `nFreeAudio` breaks re-plug reuse.
* **Registration vs realization split.** `nAddAudioModes` (attach) reserves no bandwidth;
  `nSelectAudioMode` (AHI open) switches to the bandwidth alt-interface and allocates RT-ISO;
  `subLibFreeAudio` must revert to `nam_ZeroBWIF` or the device keeps reserving iso bandwidth.
* **Channel/mixing ops are stubs** (`AHISF_MIXING`): AHI mixes; the driver only converts and ships.
  Don't implement `subLibSetVol`/`SetSound` etc. - volume goes through `subLibHardwareControl` →
  `nch_UpdateFlags` → subtask `SET_CUR`.
* **Interrupt/soft-int split** in playback: `nOutReqHook` (iso interrupt) must stay lightweight and
  only `Cause()` the player; the actual mix runs in the soft-int.
* **Fallback timer clock** keeps AHI serviced after device removal - `nReleaseHook` swaps the player
  int to `subLibPlayerIntDummy` and drives it from `timer.device`. Removing this re-introduces a
  hang on unplug-during-playback. It is also the only clock a record-only mode has (§9.5).
* **Intervals are milliseconds here.** The library reports the interval of a high speed or
  SuperSpeed endpoint in microframes; `nSelectAudioMode()` turns it into milliseconds. An
  endpoint served more often than once a millisecond is taken for one that is not: it would
  record, and play at the wrong pace. (Before, a high speed UAC 1.0 device - in practice a
  webcam's microphone - had every interval taken eight times too long.)
* **The rate is chosen before the alternate setting** in `subLibAllocAudio`, because for a
  record-only mode the rate decides which alternate setting is selected. The sampling
  frequency request therefore carries the rate AHI really gets, not the one it asked for.
* **Inputs and outputs AHI is offered** come from `nInputName` / `nOutputName`. Inputs: the
  recording path's selector unit's inputs, else the input terminals that flow to that recording
  stream (`nau_SinkUnit`) - a webcam reports its one "Microphone". Outputs: the output terminals
  the playback stream flows to (`nau_SourceUnit`). `nModeTerminal` decides which terminals a
  mode's stream reaches (never USB streaming terminals); `nInputNames` and `nOutputTerminals`
  collect them per mode (at most `MAX_PORT_NAMES`). If the flow tagging found none, every
  terminal of that kind, so a mode keeps names (outputs: `nMapOutputs`, below). The same
  count bounds `AHIDB_Input` / `AHIDB_Output` (NULL beyond it) and `AHIC_Input` / `AHIC_Output`
  (FALSE for an index never offered).
* **Several outputs.** UAC 1.0 has no unit that routes a stream to one of several outputs: a unit
  has one Output Pin (§3.5), a Selector Unit chooses between sources (§3.5.4) and is attached to
  its USB streaming sink, so it only ever chooses what is recorded; a stream that fans out to a
  speaker and headphones plays both. A device makes them switchable with a Feature Unit with a
  Mute Control (§5.2.2.4.3.1) on each output's own branch. `nMapOutputs` finds per playback mode
  the output terminals the stream reaches (`nam_Outputs`, `nOutputTerminals`), on each one's own branch - the units
  between it and the first unit that also feeds another of them - its first Mute (`nao_Mute`) and
  Volume (`nao_Volume`, `nOutputBranch`), and the last Feature Unit with volume upstream of every
  output (`nam_OutVolume`, `nSharedVolume`, the one the class always took on a single path). `nIsUpstream` walks the
  graph no deeper than `UNIT_DEPTH_MAX`, as descriptors may wire a loop. AHI is offered: one
  output by its name; several as "All outputs" (number 0, what the device does by itself),
  followed by each of them if every one has its own Mute (`nam_OutputSwitch`) - choosing one
  mutes the others (`nSetMute`); a record-only mode none. The output volume goes to
  `nam_OutVolume`, else to every `nao_Volume`, muted ones included, so switching needs no new
  volume; AHI is told the range of the first of them (`nOutVolumeUnit`). Until 6.3 every mode
  listed every terminal of the device, a device without a selector reported no inputs, an index
  from AHI was used unchecked, an output "switch" set a flag nothing acted on, and the output
  volume went to whichever Feature Unit with volume came last in the descriptors.
* **USB Audio 1.0 only.** `bcdADC` must be `0x0100`; 2.0 is declined. Adding 2.0 is a real project,
  not a tweak (different descriptor layout, clock model, and feature-unit control encodings).
* **24-bit** modes mix in AHI's HiFi format (32 bit) and are converted to 24 bits on the wire;
  recording keeps the top 16 bits, the only format AHI records in.
* **Frequencies clamped to ≤64 kHz** because AHI stores frequency in 16 bits.

---

## 15. Appendix - maps and indexes

### 15.1 Function index (by area)

* **Library / dual-lib:** `libInit` / `libOpen` / `libExpunge`, `SubLibFuncTable[]`,
  `subLibInit` / `subLibOpen` / `subLibClose` / `subLibExpunge` / `subLibReserved`.
* **Binding (usbclass ABI):** `usbDoMethodA`, `usbGetAttrsA`, `usbSetAttrsA`,
  `nVerifyAudioStreamDevice`, `usbAttemptInterfaceBinding`, `usbForceInterfaceBinding`,
  `usbReleaseInterfaceBinding`.
* **Per-binding subtask:** `nAudioTask`, `nAllocAudio`, `nFreeAudio`.
* **Topology parsing:** `nExamineAudioDescriptors`, `nExamineAudioUnits`, `nFindAudioUnit`,
  `nFindAndConnectAudioUnit`, `nFlowUp` / `nFlowDown`, `nFlowUpToUSBSource` /
  `nFlowDownToUSBSink`, `nGetInputUnit`, `nFindLogVolume` (with `nDbToFixed` / `nFixedToDb`),
  `nSetVolume`.
* **AHI mode registration / realization:** `nAddAudioModes`, `nSelectAudioMode`.
* **AHI sub-driver ABI:** `subLibAllocAudio` / `subLibFreeAudio`, `subLibEnable` /
  `subLibDisable`, `subLibStart` / `subLibUpdate` / `subLibStop`, `subLibGetAttr`,
  `subLibHardwareControl`, and the `subLibSet*` / `subLib*Sound` stubs.
* **Data path:** `nOutReqHook`, `nInReqHook`, `nInDoneHook`, `nReleaseHook`,
  `subLibPlayerIntV4` / `V6` / `Dummy`, `nMixAndConvert`, `nConv*` (playback) / `nRec*` (record)
  conversion hooks, chosen by `nConverter`.
* **GUI / config:** `nOpenCfgWindow`, `nGUITask`, `nGUITaskCleanup`, `nLoadClassConfig`.

### 15.2 Key structures (in `usbaudio.h`)

| Struct | Role |
|---|---|
| `NepAudioBase` | `usbaudio.class` library base; `nh_SubLibBase`, `nh_Units`, GUI/MUI state |
| `NepAudioSubLibBase` | the `usbaudio.audio` AHI sub-driver base; `nas_ClsBase` |
| `NepClassAudio` | one binding = one AHI unit; subtask, EP0 pipe, AHI handle, mode/unit lists, `nch_UpdateFlags` |
| `NepAudioMode` | one AHI mode; USB realization, format, frequency list, double-buffers, player clock, `nam_RecStream` / `nam_PartOf`, its outputs (`nam_Outputs`, `nam_OutVolume`, `nam_OutputSwitch`) |
| `NepAudioOutput` | one output terminal of a playback mode, as an AHI output: name, terminal, its own Mute and Volume Feature Units |
| `NepAudioUnit` | one USB-Audio topology node; graph edges, control discovery (volume/gain/monitor, `nau_MuteCtrlMask`) |

### 15.3 File map

| File | Contents |
|---|---|
| `classes/audio/usbaudio.class.c` | all class + AHI sub-driver + data-path code |
| `classes/audio/usbaudio.h` | private structs (`NepAudioBase`/`ClassAudio`/`Mode`/`Unit`/`SubLibBase`), constants |
| `classes/audio/usbaudio.class.h` | prototypes (the `subLib*` AHI vectors, hooks) |
| `classes/audio/numtostr.c` | string tables (terminal/spatial-location names) |
| `classes/class_main.c` | the shared `*.class` skeleton (romtag + LVO table) |
| `include/devices/usb_audio.h` | USB Audio Class 1.0 descriptor / request constants |
| `include/devices/usbhardware.h` | RT-ISO transport (`IOUsbHWRTIso`, `IOUsbHWBufferReq`) |
| `<devices/ahi.h>` / `<libraries/ahi_sub.h>` | AHI device + sub-driver ABI (toolchain SDK) |

### 15.4 Relationship to `poseidon.library`

The class consumes these core APIs (see the core doc): `usbGetAttrs`/`psdGetAttrs`,
`psdFindInterface`/`psdFindEndpoint`/`psdFindDescriptor` (topology), `psdAllocPipe`/`psdPipeSetup`/
`psdDoPipe` (EP0 class requests), `psdSetAltInterface` (bandwidth alt-interface), and
`psdAllocRTIsoHandler`/`psdFreeRTIsoHandler`/`psdStartRTIso`/`psdStopRTIso` (isochronous streaming,
which the core forwards to the HCD as `UHCMD_ADDISOHANDLER`/`STARTRTISO`/`STOPRTISO`). It is bound
and torn down through the `UCM_*` interface-binding protocol of core doc §7.
</content>
