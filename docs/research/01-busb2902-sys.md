# busb2902.sys — Static Analysis (BEHRINGER USB bus/function driver 2.8.40)

**Analysis status: decompiler-grade (IDA Pro 9.4 / Hex-Rays, 694 functions decompiled,
saved DB `busb2902.i64`).** Supersedes the earlier radare2-only pass; the r2-era
uncertainties (IOCTL jump-table decode, handler attribution, "gaps" in §7 of the old
version) are resolved. Static analysis only; nothing was executed or loaded.

**Target:** `BEHRINGER_2902_X64_2.8.40/busb2902.sys` — x64 PE native kernel driver,
`.text 0x66D24` (419 KB code), image base `0xF1000000`. C++ with STL and DirectShow
base classes linked into kernel (lib names: `CBaseRenderer`, `CDeferredCommand`,
`std::locale`), Ploytec "PG*" class prefix. It is Ploytec's shared USB-audio engine
("PGDevice", `PROD_PLOYTEC_DOTEC_i64_USB`), OEM-configured for Behringer — strings for
other hardware (WM8776 codec, "Maya 5.1 USB", XCorpio) are inert for PCM2900/2902.

**Version resource:** "BEHRINGER USB AUDIO DRIVER", v2.8.40, (C) Ploytec GmbH 2000-2009.

---

## 1. Exports / imports

**Export (1):** `getAsioDriverDefName` @ `0xF1034810` — returns the ASIO instance name
string `"BEHRINGER USB AUDIO"` (`0xF106BD28`). Marker string in `.data`:
`#$PTASIONAMESTRT$#...#$PTASIONAMESTOP$#` (Ploytec ASIO-name block used by the DLL).

**Imports:**
- `ntoskrnl.exe` (~64): WDM stack — `IoCreateDevice/IoCreateSymbolicLink? no —
  IoRegisterDeviceInterface/IoSetDeviceInterfaceState/IoAttachDeviceToDeviceStack/
  IoInvalidateDeviceRelations/IoBuildDeviceIoControlRequest/IoAllocateIrp/IofCallDriver/
  IofCompleteRequest/IoCancelIrp`, full `Po*` power set, `PsCreateSystemThread/
  PsTerminateSystemThread`, `Ke*` events/mutexes/spinlocks/timers/DPCs,
  `MmProbeAndLockPages/IoAllocateMdl/MmMapLockedPagesSpecifyCache/MmUnlockPages`,
  `Ob*`, `Rtl*` registry + string, `ZwOpenKey/ZwEnumerateKey/ZwDeleteKey/ZwClose`,
  `KeDelayExecutionThread`, `DbgPrint`, `__C_specific_handler` (SEH in use).
- `USBD.SYS` (3, legacy interface): `USBD_GetUSBDIVersion`,
  `USBD_CreateConfigurationRequestEx`, `USBD_ParseConfigurationDescriptorEx`.

No WDF, no `IoSetCompletionRoutineEx`, no KS/portcls imports.

## 2. DriverEntry / AddDevice / dispatch table

**DriverEntry** `0xF10257F0`: prints `"LOAD %s (USB)\n"`, init-dispatch via
`0xF10258E0` (sets `DriverUnload = 0xF10258B0`, copies `RegistryPath` into the 2 KB
global config block at `0xF106D850`), then registry: reads `DRIVERSETUPACTIVE`
(`0xF1008100`); if set, processes `\REGISTRY\Machine\...\SERVICES\BEHRINGER_2902\
Parameters` (setup-mode cleanup path); writes/checks `Check` under
`...BEHRINGER_2902`, `...\Parameters`, `...\Parameters\MME` (`0xF1008200`).

**Dispatch table** (set in `0xF101A820`; MajorFunction base 0x70 on x64 — verified):

| IRP | Offset | Handler | Notes |
|---|---|---|---|
| AddDevice | DriverExtension+0x8 | `0xF101B730` | |
| DriverUnload | DO+0x68 | `0xF10258B0` | |
| MJ_CREATE | 0x70 | `0xF101EE70` | sub-open name routing, allocates 24-byte FsContext |
| MJ_CLOSE | 0x78* | `0xF101F360` (set at 0x80) | see note |
| MJ_DEVICE_CONTROL | 0xE0 | `0xF101D7B0` | single handler, inline sparse switch |
| MJ_INTERNAL_DEVICE_CONTROL | 0xE8 | `0xF10230D0` | routes by child DeviceType |
| MJ_POWER | 0x120 | `0xF101BD50` | |
| MJ_SYSTEM_CONTROL | 0x128 | `0xF101D580` | WMI passthrough |
| MJ_PNP | 0x148 | `0xF101D420` | |

Note: the assignment list observed in `0xF101A820` writes slots 0x70 (`0xF101EE70`),
0x80 (`0xF101F360`), 0xE0, 0xE8, 0x120, 0x128, 0x148 — i.e. CREATE, CLOSE-ish slot 2
(0x78 is CLOSE in the standard layout; the driver writes 0x80 = READ slot, consistent
with the original author using a DDK-era table; READ/WRITE/CLEANUP/SHUTDOWN majors are
left at the invalid-request default and are never used by the contract).

**AddDevice** `0xF101B730`: `IoCreateDevice(ExtensionSize=0x8A0, Name=NULL,
DeviceType=0x8002, Characteristics=0x80)`; clears `DO_DEVICE_INITIALIZING`, sets
`DO_POWER_PAGABLE`; `IoAttachDeviceToDeviceStack`; two spare IRPs
(`IoAllocateIrp`, stored in ext); `IoRegisterDeviceInterface` + enable (§3).
Also calls `USBD_GetUSBDIVersion` once.

## 3. Device interfaces, namespace, IOCTL contract

**Device interface GUID** `{090E2CEE-44C0-4263-8837-786AA85A49C6}` (data `0xF106AFC0`),
registered in AddDevice on the USB PDO, enabled at start. `busbasio_x64.dll` opens it
via `SetupDiGetClassDevsA`.

**Sub-open names** (MJ_CREATE `0xF101EE70` compares `FileObject->FileName`):
`\IO` (streaming data plane), `\CONTROL` (control plane), `\FW_UPDATE` (firmware
updater; rejected with `STATUS_SHARING_VIOLATION`-class check while device is active).
Each successful open allocates a 24-byte per-file context in `FsContext`
(`\CONTROL` context holds a sub-handle from `sub_F1011860`; `\IO` context has fields
used by the position/ready IOCTLs). Unknown names → `STATUS_INVALID_PARAMETER`.

**IOCTL dispatch** `0xF101D7B0` — *single function, inline switch* (the old r2 "225-entry
table with handlers 0xF103CB63…" was a mis-parse; corrected here). Validation:
`DeviceObject->DeviceType == 0x8002` required (verified — the old claim "kernel does not
enforce device type" is wrong), file context validity, then
`switch (IoControlCode)` over a **sparse set** (all METHOD_BUFFERED semantics; the
SystemBuffer/UserBuffer decompiler split is an IRP-union artifact — one buffer):

| IOCTL | Dir/Size | Semantics (from decompilation) |
|---|---|---|
| `0x220000` | OUT ≤255 B | copy product name string (USB string, ext+5881) |
| `0x220004` | OUT ≤255 B | copy manufacturer string (ext+6137) |
| `0x220008` | IN 8+N | vendor/class request passthrough: `VENDOR_OR_CLASS_REQUEST` 8-byte header + payload → URB |
| `0x22000C` | OUT 98584 B | full state dump (`sub_F100A940`, 0x18118 bytes) |
| `0x220014` | IN 76 B | commit config struct (`sub_F100FCF0`) — rate/period/format |
| `0x220018` | IN 4 B | set stream unit param, clamped ≥1 (ext+7264) |
| `0x22001C` | — | stop: abort pipes (`sub_F1015A30`), tear down (`sub_F101FFD0`) |
| `0x220024` | IN 24 B | parameter block set (`sub_F1010170`) |
| `0x220030` | OUT 16 B | per-file status (`sub_F1017660` on FsContext sub-handle) |
| `0x220038` | OUT 8 B | 64-bit latched clock/position from `\IO` file ctx+4, read-clears |
| `0x22004C` | OUT 4 B | returns `0x0CE5` (3301) — handshake magic shared with busbwdm.sys |
| `0x220064` | IN 3844 B | routing table IN (`sub_F100F9A0`) |
| `0x220068` | OUT 3844 B | routing table OUT (`sub_F100F960`) |
| `0x22006C` | — | release per-`\IO`-handle stream slot (`sub_F1016F10`) |
| `0x220070` | OUT 4 B | `sub_F1010A30` value (capability/state word) |
| `0x220094` | — | one-time init (flag ext+5841; idempotent) |
| `0x220098` | OUT 16 B | 16-byte hardware info blob (ext+1682) |
| `0x22009C` | OUT 4 B | `sub_F1007190` state byte |
| `0x2200A0` | IN 4 B | set per-file `m_bIsFileReady` |
| `0x2200A4` | OUT 4 B | `0x0207A800` (build id 2.8.40) |
| `0x2200B0` | IN 4 B | per-file stream slot field (+84) = client PID |
| `0x2200B4` | OUT 4 B | count of other handles sharing the slot (`sub_F1016A60`) |
| `0x2200B8` | OUT 4 B | `sub_F1016AF0` position/state |
| `0x2200BC` | IN 8 B | set 8-byte value via `sub_F1011730` → submits URB (sync event path) |
| `0x2200C0` | OUT N | stream read (`sub_F1002470`, ctx at ext+11784) |
| `0x2200C4` | IN N | stream write (`sub_F1002C20`) |
| `0x2200C8` | OUT N | stream read variant (`sub_F1002B60`) |
| `0x2200CC` | OUT 64 B | stream info (`sub_F1002CE0`) |
| `0x2200D0` | IN/OUT 260 B | generic property dispatcher (`sub_F1012C90`): cmds 0–22 incl. stop(1), get rate(2), set rate(3), ch count(6), buffer info(8), version(10), priority names "highspeed/rapid/fast/normal/relaxed normal/relaxed"(12), name strings (19/20) |
| `0x2200D4` | IN/OUT 260 B | property dispatcher with `\IO` file object (`sub_F1013800`) |
| `0x2200E0` | — | `IoInvalidateDeviceRelations(BusRelations)` re-enumeration trigger |
| default | — | `STATUS_INVALID_DEVICE_REQUEST` (0xC0000010) |

The `0x220003/07/13/1F` constants seen in `.text` are **not** user IOCTLs: they are
`IOCTL_INTERNAL_USB_SUBMIT_URB` (0x220003), `IOCTL_INTERNAL_USB_GET_HUB_COUNT`
(0x220007), `IOCTL_INTERNAL_USB_RESET_PORT` (0x220013) and
`IOCTL_INTERNAL_USB_CYCLE_PORT` (0x22001F) — built via `IoBuildDeviceIoControlRequest`
with major 0x0F and forwarded to the USB PDO (e.g. `0xF10203E0` = cycle-port on stop;
status 259 → explicit wait).

**MJ_INTERNAL_DEVICE_CONTROL** `0xF10230D0`: FDO (0x8002) → forward down. Child 0x8003
(WDM audio, busbwdm.sys) → `sub_F1025020` (private command channel: config fetch /
start, see `02-busbwdm-sys.md`). Child 0x8004 (MIDI) → `sub_F1025A70` (bulk/interrupt
MIDI read/write).

## 4. Registry configuration surface

Keys under `\REGISTRY\Machine\System\CurrentControlSet\SERVICES\BEHRINGER_2902\...`
(unchanged from r2 pass; verified strings):
`Parameters` (+`ParametersXCorpio` debug variant), `Parameters\MME`, `WM8776_1/2`
(Wolfson codec banks: `Mute`, `NotGang`, `Volume`, `Frequency`, `ADC_BOOST`,
`VOLUME_OUT_L/R`, `VOLUME_MONITOR`, `VOLUME_ADC`, `OutSelection`, `InSelection`,
`SyncSelection`, `MonitorSelection`), stream config: `OutChannels`, `InChannels`,
`OutResolution`, `InResolution`, `PerformanceMode`, `FramesPerTransaction`,
`BufferSize`, `Configuration`, `ROUTING_IN_0/1`, `ROUTING_OUT_0/1`, `ClockSource`,
`USB1Mode`, `DigitalOutMode`, `DigitalOutSelector`, `InputMonitoringASIOControlled`,
`InputMonitoringOn`, `Monitor`, `RoutingAccess`, `Version`; controls:
`DRIVERSETUPACTIVE`, `FWUPDATER`, `Check`; per-mixer `MuteLIn/...`, `EsuCPLDByte`.

WM8776/XCorpio/"Maya 5.1 USB" = Ploytec shared codebase; inert for PCM2900/2902.

## 5. USB engine (decompiler-verified)

- **Select config** `0xF1020660`: `USBD_ParseConfigurationDescriptorEx` +
  `USBD_CreateConfigurationRequestEx` over explicit (interface#-alt#) pairs taken
  from per-interface objects (fields +2756/+2757); submits via sync helper
  `sub_F10087F0` (send-`URB`-and-wait with IRQL checks, `"USB::sendAndWait"` strings,
  STATUS_TIMEOUT/ALERTED/USER_APC handling). Pipe handles copied into per-endpoint
  objects (+64); isoch IN/OUT and feedback endpoints classified by helpers
  (`sub_F102D610/D660/C070/C000`); alternate-setting walk `sub_F102BA80` (alt 1 for
  streaming).
- **Start path** `0xF101F800` ("PGDevice::start"): URB `GET_DESCRIPTOR` (function 11,
  length 136) → device descriptor; **obfuscated VID/PID gate** — compares
  `0xEFB6/0xF0AD/0x80CD` word pairs and `XOR 0x2378FF15` expressions, matching the
  Behringer 08BB/2900|2902 identifiers; on match calls `sub_F1022AB0` (fetch USB
  string descriptors 256 B → ext+5881/+6137 → the `0x220000/0x220004` names),
  then creates the two child PDOs:
  - `"Midi Device"` — `IoCreateDevice(0x228, DeviceType 0x8004)`, only if a MIDI
    interface was found;
  - `"MME Device"` — `IoCreateDevice(0x228, DeviceType 0x8003)`, hardware id
    `MEDIA\BUSB_AUDIOADAPTER`, friendly text `"BEHRINGER USB AUDIO"` — the PDO that
    `busbwdm.sys` binds to.
  Child creation is conditional on `sub_F1010C90`/`sub_F1011130` state; children are
  (re)published with `IoInvalidateDeviceRelations(BusRelations)` via IOCTL
  `0x2200E0`.
- **Streaming**: pre-built transaction pools — **32 records × 64 B for IN
  (ext+17480), 4 records × 64 B for OUT (ext+19528)** — each holding a ready IRP
  (+16); `bulkAudioGo` (`0xF1005E80`) computes queued-depth from sample rate and
  buffer time (`30*rate/(1000*buf)`, clamped 4..31) and submits via `IofCallDriver`
  with completion callbacks (`sub_F1007030` IN / `sub_F10065B0` OUT). Packet fill
  uses per-channel DMA encoder/decoder callbacks (`chooseDMAEncoder/Decoder`,
  product `PROD_PLOYTEC_DOTEC_i64_USB`) and **512-byte packet stride** (`m_bUSB2`
  path; `USB1Mode` registry selects the legacy path). Per-packet header bytes
  written at packet+480/+481 (`0xFD` default, monitor bits `0x18`).
- **Feedback/sync**: `"ISOC FEEDBACK CALLBACK to late ..."` handler `0xF1013EB0` —
  compares the feedback-reported frame against expected (+1792), skips forward 4
  frames on overrun, then resyncs (`sub_F1013BD0`). `"lostFrames"` accounting.
- **User mapping**: single `MmMapLockedPagesSpecifyCache` site `0xF10340B0` — the
  kernel ring buffer mapped into the requesting process (zero-copy ASIO path).
- **Worker thread**: `PsCreateSystemThread` at `0xF1008290` (thread proc
  `StartRoutine` 0xF10083B0 → `0xF1008410`), plus `KeInitializeTimer/DPC` banks
  (`KernelThreadBank`) and completion DPCs re-submitting transactions.

## 5.1 Zero-copy shared-area contract (deep dive, verified both sides)

The "kernel ring buffer" in §5 **is the ASIO DLL's shared area**, mapped with
`IoAllocateMdl` + `MmProbeAndLockPages(UserMode, IoModifyAccess)` +
`MmMapLockedPartsSpecifyCache` (`sub_F10340B0`, single site) after the DLL's
`0x220030` register call. Full layout and the ring writer `sub_F1007B30`
(cursor semantics, wrap at 201600 dwords, `totalWritten` position counter) are
documented in research 03 §3.1–3.2 — do not re-derive from this file.

Kernel-side anchors (this binary):

- `sub_F1017340(slot, data, len)`: fan-out write into each registered client's
  mapped area (`sub_F10342B0`/`sub_F1034300` = acquire/release the mapping),
  `sub_F1007B30(sharedVa + 806412, ...)` = capture ring (engine B); overflow
  accounting into `tail+28` (`_InterlockedExchangeAdd`, frame-size/divisor from
  slot+72/+76).
- 0x220030 handler locks the 16-byte descriptor `{ u8 startFlag @0; u64 VA @+8 }`,
  maps 0x189C38 bytes, `ObReferenceObjectByHandle` on tail event (signal) and
  tail thread (`KeSetPriorityThread(31)`).

## 5.2 Isoch worker & IN data path (deep dive, decompiler-verified)

Thread model:

- `sub_F1008290` creates a system thread (`PsCreateSystemThread`) with context
  `{ callback fn @+0, thread obj @+8, handle @+16, stop flag @+24, KEVENT @+32,
     work queue @+64, arg @+56 }`; `StartRoutine` (0xF10083B0) loops
  `sub_F1008410`: pop item from the queue (`sub_F1029DE0`, timeout 1) → call
  `callback(item, arg)`; on empty queue wait the KEVENT; stop flag ends thread.

IN (capture) submission:

- `bulkAudioGo` (`0xF1005E80`): flush both stream objects (ext+1704/+1712, via
  `sub_F1033980`), depth = `30*rate/(1000*bufferTimeMs)` clamped 4..31 (field
  ext+19872 = buffer time), queues that many IN transactions (`sub_F1005FF0`)
  plus 3 OUT transactions (`sub_F1006150`).
- `sub_F1005FF0`: guarded by start flags (ext+1796 run, ext+1628 streaming,
  ext+5864 suspend); scans the **32-slot IN pool (ext+17480, 64-byte stride)**
  for a free record; if the IRP at slot+16 is present, (re)initializes it via
  `sub_F1003180(irp, completion sub_F1007030, slot, 1,1,1)` and
  `IofCallDriver(lowerPdo)` — the PDO pointer lives at
  `*(ext+5832) + 768`. No free slot → `sub_F10044A0(slot)` (recycle).

IN completion → ring:

- `sub_F1007030(slot)`: on non-cancelled, non-error status and valid context,
  calls `sub_F1006660(streamCtx, slot)`, then recycles the slot
  (`sub_F10044A0`) and completes with `0xC0000236`.
- `sub_F1006660`: walks the isoch transfer buffer in **512-byte packets**
  (`v5`=VA from slot+40, `v4`=length from slot+36); per packet:
  1. `(*(ext+19792))(&ctx)` — parser callback, returns payload length;
  2. optional filter callbacks `(*(ext+1648))(...)` / `(*(ext+19808))(...)`;
  3. two internal consumers at `ext+696` (fill IN ring state ext+8..32) and
     `ext+704` (scratch ext+352..376, 1016 B);
  4. **fan-out**: for each registered stream slot in the pool at `ext+11216`
     (count `ext+11232`), `sub_F1017340(slot, payload, len)` — the zero-copy
     ring write of §5.1.
  Then re-arms (`sub_F1005FF0`) and `sub_F1021CC0` (housekeeping). USB-stall
  status (-1073741667 = 0xC0000... DEVICE_*) or reset path takes
  `sub_F1015A30` + `sub_F1011AB0(2,0)` (resync) instead.

## 6. Windows 10/11 compatibility notes (verified)

- Legacy USBD interface (3 imports) — deprecated but present on Win10/11; portable.
- `PoStartNextPowerIrp` — harmless no-op on modern Windows.
- Vendor `DeviceType 0x8002/0x8003/0x8004`; the IOCTL handler *does* enforce
  DeviceType==0x8002 — a reimplementation must keep the FDO device type or patch the
  check (izukidio keeps 0x8002).
- User-mode contract to preserve: interface GUID `{090E2CEE-44C0-4263-8837-786AA85A49C6}`,
  sub-opens `\IO`/`\CONTROL`/`\FW_UPDATE`, the IOCTL table above, magic returns
  `0x0CE5` (version) and `0x0207A800` (build).
- `MmMapLockedPagesSpecifyCache` user-mode mapping from kernel: still possible but
  restricted/careful on modern Windows (VAD/process context); izukidio PoC uses
  METHOD_BUFFERED copies instead and can adopt zero-copy later.
- Child PDO with hardware id `MEDIA\BUSB_AUDIOADAPTER` + `busbwdm.sys` WaveCyclic
  miniport is the legacy MME/WASAPI path; deprecated design (see 02/04).
- Obfuscated VID/PID check is hard-wired; other Ploytec OEM products use the same
  binary with different gates.

## 7. Remaining gaps (reduced from r2 pass)

1. `sub_F1025020` / `sub_F1025A70` full command enumeration for the MIDI child and
   the 0x8003 private channel (busbwdm side covered in 02).
2. Exact field map of the 76-byte config struct (`0x220014`) — only rate/period slots
   identified; finalize on first hardware bring-up with the original DLL.
3. 98584-byte state dump layout (`0x22000C`) — offsets +1682 (hw info), +5881/+6137
   (names) known; remainder low-value for PoC.
4. Firmware-update path (`\FW_UPDATE`, `FWUPDATER`, `EsuCPLDByte`) — intentionally
   not analyzed (risk surface, not needed for audio).
