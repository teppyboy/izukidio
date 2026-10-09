# busb2902.sys — Static Analysis (BEHRINGER USB bus/function driver 2.8.40)

**Task note:** This deliverable was originally requested via IDA MCP. IDA MCP is **not available** in this session (no `mcp__ida` tools; Ghidra MCP also absent). Analysis performed with **radare2 6.2.2** (`/opt/homebrew/bin/r2 -A`), **python3+pefile** (`/tmp/rev/bin/python`), and `strings`/`objdump` fallback. No decompiler installed — findings are from r2 disassembly + manual x64 reading + data-section decoding. Sections that need deeper (decompiler-grade) analysis are flagged. Static analysis only; nothing was executed or loaded.

**Target:** `BEHRINGER_2902_X64_2.8.40/busb2902.sys` — x64 PE native kernel driver, `.text 0x66D24` (419 KB of code), `.rdata 0x4520`, `.data 0x1248`, `.pdata`, `.edata`, `INIT 0x834`. 566 functions found by `r2 -A`. Image base used below: `0xF1000000`.

**Version resource:** "BEHRINGER USB AUDIO DRIVER", v2.8.40, Copyright (C) Ploytec GmbH 2000-2009, OriginalFilename `busb2902.sys`.

---

## 1. Exports / imports

**Export (1):** `getAsioDriverDefName` @ `0xF1034810` — 8-byte function, returns pointer to ASCII/UTF-16 string `"BEHRINGER USB AUDIO"` (`0xF106BD28`). The ASIO DLL uses it to learn the driver/ASIO instance name. Related marker string in `.data`: `#$PTASIONAMESTRT$#--------------------------------#$PTASIONAMESTOP$#` (Ploytec ASIO name block delimiter used by the user-mode side).

**Imports:**
- `ntoskrnl.exe` (64): full WDM stack — `IoCreateDevice`, `IoRegisterDeviceInterface`, `IoSetDeviceInterfaceState`, `IoAttachDeviceToDeviceStack`, `IoInvalidateDeviceRelations`, `IoBuildDeviceIoControlRequest`, `IoAllocateIrp/IoFreeIrp/IofCallDriver/IofCompleteRequest`, `PoRequestPowerIrp/PoCallDriver/PoStartNextPowerIrp/PoSetPowerState`, `PsCreateSystemThread/PsTerminateSystemThread`, `KeWaitForSingleObject/KeInitializeEvent/KeSetEvent/KeInitializeMutex/KeReleaseMutex/KeAcquireSpinLockRaiseToDpc/KeReleaseSpinLock`, `KeInitializeTimer/KeSetTimer/KeCancelTimer/KeInitializeDpc`, `MmProbeAndLockPages/IoAllocateMdl/IoFreeMdl/MmMapLockedPagesSpecifyCache/MmUnlockPages`, `ObReferenceObjectByHandle/ObfReferenceObject/ObfDereferenceObject`, `RtlQueryRegistryValues/RtlWriteRegistryValue/ZwOpenKey/ZwEnumerateKey/ZwDeleteKey/ZwClose`, `RtlCompareUnicodeString`, `KeDelayExecutionThread`, `DbgPrint`, `__C_specific_handler` (SEH `__try` used — expect try/except around USB calls).
- `USBD.SYS` (3, legacy USB interface): `USBD_GetUSBDIVersion`, `USBD_CreateConfigurationRequestEx`, `USBD_ParseConfigurationDescriptorEx`.

No WDF/WDM-USBD modern stack (`WdfUsbTargetDevice*` absent), no `IoSetCompletionRoutineEx`, no `KsXxx`.

## 2. DriverEntry / AddDevice / dispatch table

**DriverEntry** (`entry0 @ 0xF10257F0`):
1. `0xF10347F0()` → init a 2 KB global config block at `0xF106D850` (zeroed in `0xF10258E0`), `DbgPrint("LOAD %s (USB)\n")`.
2. `InitDispatch(DriverObject)` (`0xF10258E0`): sets `DriverUnload = 0xF10258B0`, then `0xF101A820` sets the MajorFunction table; copies `RegistryPath` into global `0xF106D850`.
3. Registry: reads `DRIVERSETUPACTIVE` value (helper `0xF1008100` = `RtlQueryRegistryValues` wrapper); if active, touches `\REGISTRY\Machine\System\CurrentControlSet\SERVICES\BEHRINGER_2902\Parameters`; reads/writes `Check` value under `...\SERVICES\BEHRINGER_2902` (helper `0xF1008200`).

**Dispatch table** (set in `0xF101A820`; x64 offsets in DRIVER_OBJECT):

| IRP | Offset | Handler |
|---|---|---|
| AddDevice | DriverExtension+0x8 | `0xF101B730` |
| DriverUnload | DO+0x68 | `0xF10258B0` |
| MJ_CREATE | DO+0x70 | `0xF101EE70` |
| MJ_CLOSE | DO+0x80 | `0xF101F360` |
| MJ_DEVICE_CONTROL | DO+0xE0 | `0xF101D7B0` |
| MJ_INTERNAL_DEVICE_CONTROL | DO+0xE8 | `0xF10230D0` |
| MJ_POWER | DO+0x120 | `0xF101BD50` |
| MJ_SYSTEM_CONTROL | DO+0x128 | `0xF101D580` |
| MJ_PNP | DO+0x148 | `0xF101D420` |

**AddDevice** (`0xF101B730`): `IoCreateDevice(DriverObject, DeviceExtensionSize=0x8A0 (2208), Name=NULL, DeviceType=0x8002 (vendor-specific, non-standard), Exclusive=...)`; attaches to device stack; registers the device interface (see §3) via `0xF101B6D0` and enables it with `IoSetDeviceInterfaceState`. Device type `0x8002` is verified later in the IOCTL handler (`cmp [iosl->DeviceObject->DeviceType], 0x8002`).

**Child PDOs** (`0xF101F800`): `IoCreateDevice(..., ExtensionSize=0x228, DeviceType=0x8004, ...)` with names/ids including `"Midi Device"` (`0xF106AE00`) — bus-enumeration style children (paired with `IoInvalidateDeviceRelations` import). This matches the INF's `MEDIA\BUSB_AUDIOADAPTER` pattern: this binary is both the USB *bus filter/function* layer and the parent of the WDM audio child that busbwdm.sys binds.

## 3. Device interfaces, namespace, IOCTLs

**Device interface GUID** (passed to `IoRegisterDeviceInterface` @ `0xF101B6FA`):
`{090E2CEE-44C0-4263-8837-786AA85A49C6}` (data at `0xF106AFC0`). User-mode (busbasio_x64.dll `CreateFileA`) opens through this interface.

**Namespace strings** (compared in MJ_CREATE `0xF101EE70` via `RtlCompareUnicodeString`): `"\FW_UPDATE"` (`0xF106AD98/ADD0`), `"\IO"` (`0xF106ADB0`), `"\CONTROL"` (`0xF106ADB8`). So the control device exposes sub-opens: `\CONTROL` (main control/ASIO channel), `\FW_UPDATE` (firmware updater path — see `FWUPDATER` registry value), `\IO` (streaming I/O), plus a `"Port"` interface string.

**IOCTL dispatch** (`0xF101D7B0`):
- Validates `DeviceType == 0x8002` and completes rejects otherwise; also checks `[iosl+0x48]` (`0x8002` comparison) early-out.
- Computes `index = IoControlCode - 0x220000`, bounds-check `<= 0xE0`, then `jmp [table 0xF101ECB8 + index*4]` — a 225-entry switch. r2's static table parse is reliable only for the first 32 entries (higher entries parse as garbage; either the table is shorter with chained compares, or r2 mis-derived the base — **flagged for deeper analysis**).
- Verified case→handler map (function codes 0x00–0x1F, i.e. IOCTLs `0x220000 + 4*case` for method-0/any-access codes):

| case (code low byte) | Handler |
|---|---|
| 0x00 | `0xF103CB63` |
| 0x01 | `0xF103CC91` |
| 0x02 | `0xF103CDBF` |
| 0x03 | `0xF103CF28` |
| 0x04 | `0xF103CF94` |
| 0x05 | `0xF103D20B` |
| 0x06 | `0xF103D357` |
| 0x07 | `0xF103D27D` |
| 0x08 | `0xF103CFFE` |
| 0x09 | `0xF103D0DD` |
| 0x0A | `0xF103C91F` |
| 0x0B | `0xF103C85C` |
| 0x0C | `0xF103C800` |
| 0x0D | `0xF103D1A4` |
| 0x0E | `0xF103C8BE` |
| 0x0F | `0xF103D3B0` |
| 0x10 | `0xF103D2DD` |
| 0x11 | `0xF103C7A1` |
| 0x12 | `0xF103C6BF` |
| 0x13 | `0xF103C96E` |
| 0x14 | `0xF103D424` |
| 0x15 | `0xF103D4C3` |
| 0x16 | `0xF103D561` |
| 0x17 | `0xF103D5D9` |
| 0x18 | `0xF103D64D` |
| 0x19 | `0xF103D770` |
| 0x1A | `0xF103D6D2` |
| 0x1B | `0xF103D814` |
| 0x1C | `0xF103C9BD` |
| 0x1D | `0xF103CA7D` |
| 0x1E | `0xF103D895` |
| 0x1F | `0xF103D90F` |

- **User-mode side cross-check** (busbasio_x64.dll / busbasio.dll `DeviceIoControl` constants, scanned by value): dense cluster `0x220000, 0x220004, 0x22000C, 0x220014, 0x220018, 0x220024, 0x220030, 0x220038, 0x22004C, 0x220064, 0x220068, 0x22006C, 0x220070, 0x22009C, 0x2200A0, 0x2200A4, 0x2200B0, 0x2200B4, 0x2200B8, 0x2200BC, 0x2200C7, 0x2200D0` — i.e. ASIO talks almost exclusively in the `0x2200xx` function-code range decoded above (0x2200C7 = method-3 code, likely "raw request" passthrough). Additional values shared with the driver's `.text`: `0x222444` (in both busb2902.sys and busbasio_x64.dll — likely a real but higher function code, case table region not statically decoded), `0x22B918` (both). `.rdata` of the ASIO DLL also holds `0x220400/0x220800/0x220900/0x220D00/0x220E00/0x221200/0x221300/0x221400/0x221700` — stride-0x100 pattern, likely a second enum space (USB message/pipe ids) rather than CTL_CODEs. **Not fully decoded — needs deeper analysis.**
- No `CTL_CODE(0x22,...)` constants appear inside busb2902.sys itself (byte-level scan of all sections found only misaligned coincidences); the `0x22xxxx` codes are constructed on the user-mode side and matched by the driver's `sub 0x220000` switch. Device type used by the driver object is the vendor type `0x8002`, not `FILE_DEVICE_UNKNOWN`.

## 4. Registry configuration surface (strings, `RtlQueryRegistryValues`/`RtlWriteRegistryValue`/`ZwOpenKey`)

Keys under `\REGISTRY\Machine\System\CurrentControlSet\SERVICES\BEHRINGER_2902\...`:
- `Parameters` and `Parameters\` (+ `ParametersXCorpio\` variant — debug/alternate build string)
- `Parameters\MME` — wave/MME side settings
- `WM8776_1`, `WM8776_2` — Wolfson WM8776 codec control banks (I2C-controlled external codec on some Ploytec-based hardware; values `Mute`, `NotGang`, `Volume`, `Frequency`, `ADC_BOOST`, `VOLUME_OUT_L/R`, `VOLUME_MONITOR`, `VOLUME_ADC`, `OutSelection`, `InSelection`, `SyncSelection`, `MonitorSelection`)
- Channel/stream config values: `OutChannels`, `InChannels`, `OutResolution`, `InResolution`, `PerformanceMode`, `FramesPerTransaction`, `BufferSize`, `Configuration`, `ROUTING_IN_0/1`, `ROUTING_OUT_0/1`, `ClockSource`, `USB1Mode`, `DigitalOutMode`, `DigitalOutSelector`, `InputMonitoringASIOControlled`, `InputMonitoringOn`, `Monitor`, `RoutingAccess`, `OutChannels`, `Version`
- Control values: `DRIVERSETUPACTIVE`, `FWUPDATER`, `Check`; per-mixer `MuteLIn/MuteRIn/VolumeLIn/VolumeRIn/MuteLOut/MuteROut/VolumeLOut/VolumeROut`, `EsuCPLDByte` (CPLD firmware byte)

Note: `WM8776`/`EsuCPLDByte`/`XCorpio` indicate this is Ploytec's shared codebase covering more hardware than the PCM2900/2902 (strings also contain `"Maya 5.1 USB"` — AUDIOTRAK Maya, another Ploytec design). For PCM2902 reimplementation most of that surface is inert.

## 5. USB engine

- Configuration: `USBD_GetUSBDIVersion` checked, `USBD_CreateConfigurationRequestEx` (`0xF1020660` caller) + `USBD_ParseConfigurationDescriptorEx` — classic pre-WDF interface/pipe selection; `"USB: Configuration failed"` error path.
- Streaming: isochronous with explicit feedback handling — debug strings `"ISOC FEEDBACK CALLBACK to late current:%d diff:%d NTF:%d lostFrames:%d"` (feedback endpoint monitoring, frame-loss accounting), `"bulkAudioGo numInRequests:%d"`, `"PGDevice::bulkAudioRun() - NO PIPES"`, `"PGDevice::initNextTransaction() mbInInitNextTransaction already set"`, `"mNumPattern:%d m_bUSB2:%d"` (USB2 vs USB1 path switch, matches `USB1Mode` registry value).
- Per-channel codec plumbing: `"chooseDMAEncoder for PROD_PLOYTEC_DOTEC_i64_USB chan:%d"` / `"chooseDMADecoder ..."` — product enum `PROD_PLOYTEC_DOTEC_i64_USB`; per-channel encode/decode callbacks for the isoc packet stream.
- Synchronous control: `"USB::sendAndWait ..."` family (`INVALID IRQL %d in USB::sendAndWait`, `STATUS_TIMEOUT UrbStatus:%08X IrpStatus:%08X`, `STATUS_ALERTED`, `STATUS_USER_APC`, `second wait`) — a `KeWaitForSingleObject`-based sync URB sender with IRQL validation.
- Buffers: `MmProbeAndLockPages`/`IoAllocateMdl`/`MmMapLockedPagesSpecifyCache` — kernel-locked, MDL-mapped ring buffers (the mechanism the ASIO DLL gets zero-copy access through; exact user-mapping call sites flagged for deeper analysis).
- Threading: `PsCreateSystemThread` (call site `0xF1008346` in `0xF1008290`-region) — dedicated worker thread(s) (transaction pump / stream feeder), plus `KeInitializeTimer`+DPC for timeouts/feedback cadence.

## 6. Windows 10/11 compatibility notes

- `USBD_CreateConfigurationRequestEx`/`USBD_ParseConfigurationDescriptorEx` are the **legacy USBD interface** — still present in ntoskrnl for compat but deprecated; a modern reimplementation should use WDF USB or `URB` building via `USBD_IsochUrb` structures directly. The 3-function USBD usage is small enough to port mechanically.
- `PoStartNextPowerIrp` is required pre-Vista, a no-op on Win10/11 — harmless.
- Vendor `DeviceType = 0x8002` (and `0x8004` for children) is non-standard; user-mode must not rely on `FILE_DEVICE_UNKNOWN` semantics. Kernel does not enforce device-type matching on IOCTLs, so the 0x22xxxx codes work regardless.
- Device-namespace opens (`\CONTROL`, `\FW_UPDATE`, `\IO`) + single device interface GUID `{090E2CEE-44C0-4263-8837-786AA85A49C6}` are the user-mode contract that must be preserved (ASIO DLL depends on it).
- `IoInvalidateDeviceRelations` child-enumeration of a `"Midi Device"` PDO will create a spurious MIDI device node on Win10/11 unless suppressed; on the original PCM2900/2902 hardware the child may fail to start (no MIDI interface) — harmless on Vista, noisy today.
- Direct `MmMapLockedPagesSpecifyCache` user-mapping of kernel buffers (if confirmed at the flagged call sites) is exactly what ASIO needs but is restricted (UserMode mapping from kernel) — modern equivalent: `MmMapLockedPagesSpecifyCache` still works from kernel for session-space processes but needs care with AddressElevation; alternatives: `AllocateUserPhysicalPages` or keep MDL mapping at PASSIVE_LEVEL in the calling process context.

## 7. Gaps needing deeper (decompiler) analysis

1. IOCTL case handlers 0x00–0x1F individual semantics (each 100–500 bytes; one per config knob — likely get/set pairs for the registry values in §4).
2. Full 225-entry switch: case indices ≥ 0x20 (table decode unreliable beyond 0x1F here).
3. `0xF10230D0` (IRP_MJ_INTERNAL_DEVICE_CONTROL) — this is the private channel busbwdm.sys uses (`IRP_MJ_INTERNAL_DEVICE_CONTROL` + command byte, see busbwdm-ghidra.md §3); needs full command-code enumeration.
4. ASIO-side ring buffer: exact `MmMapLockedPagesSpecifyCache` call sites and the lock-free protocol between `\IO` streaming IOCTLs and the worker thread.
5. URB construction for isoc transfers (`0xF1020660` and callers) — pipe/packet sizes per alternate setting.
6. Firmware-update path (`\FW_UPDATE`, `FWUPDATER`, `EsuCPLDByte`) — intentionally not analyzed in depth (risk surface, not needed for audio reimplementation).
