# busbwdm.sys — Static Analysis (BEHRINGER USB WDM Audio 2.8.40)

**Analysis status: decompiler-grade (IDA Pro 9.4 / Hex-Rays, 170 functions, saved DB
`busbwdm.i64`).** Supersedes the earlier Ghidra-lane/r2 pass; uncertain items are
resolved (§ notes the fixes). Image base `0xF3000000`. Static analysis only; driver
never loaded/executed.

**Target:** `BEHRINGER_2902_X64_2.8.40/busbwdm.sys` — x64 PE native kernel DLL
(Subsystem NATIVE), PortCls adapter driver. Vendor strings: Ploytec GmbH
(`info@ploytec.com`). Compact binary: 27 strings, 170 functions.

**File layout:** `.text 0x8810` `.rdata 0x6d4` `.data 0x3d0` `.pdata 0x5dc` `INIT 0x2ce`
`.rsrc` `.reloc`. No exports (DriverEntry via entry point `0xF3001000`).

**Imports (complete):**
- `NTOSKRNL.exe`: `ExAllocatePool`, `ExFreePool`, `KeInitializeTimer`,
  `KeInitializeTimerEx`, `KeSetTimer`, `KeSetTimerEx`, `KeCancelTimer`,
  `KeInitializeDpc`, `KeDelayExecutionThread`, `ObfReferenceObject`,
  `ObfDereferenceObject`, `IoAllocateIrp`, `IoFreeIrp`, `IofCallDriver`, `memcmp`,
  `DbgPrint`, interlocked helpers.
- `portcls.sys`: `PcInitializeAdapterDriver`, `PcAddAdapterDevice`, `PcNewPort`,
  `PcNewMiniport`, `PcNewServiceGroup`, `PcRegisterSubdevice`,
  `PcRegisterPhysicalConnection`.

No `IoCreateDevice`, no DMA adapter calls (WaveCyclic software "DMA"), no USB imports.

---

## 1. DriverEntry / AddDevice

### DriverEntry `0xF3001000` (verified)
```
return PcInitializeAdapterDriver(DriverObject, RegistryPath, AddDevice = 0xF3001040);
```

### AddDevice `0xF3001040` (verified)
Calls the serialized wrapper `0xF3008DA0` with `StartDevice = 0xF3001080`,
`MaxObjects = 3`, `DeviceExtensionSize = 0`.

### Serialized PcAddAdapterDevice wrapper `0xF3008DA0` (decompiler-verified, details corrected)
- Global refcount `0xF300B3C0`, captured PDO `0xF300B3B8`.
- First caller (`_InterlockedIncrement` returns 1): stores PDO, calls
  `PcAddAdapterDevice(DriverObject, PDO, StartDevice, 3, 0)`; clears the PDO slot on
  failure via `0xF3008D70`.
- Second concurrent instance: decrement; `KeDelayExecutionThread(0, 1, -200000)` —
  **20 ms per retry** (corrects the r2 "0.5 ms" note), counter 5000 decremented by 20
  per loop = 250 retries = **~5 s budget**, then returns `STATUS_WAIT_TIMEOUT (258)`.
=> Only one adapter instance supported per boot.

### StartDevice callback `0xF3001080` → worker `0xF3001C90`
`NewAdapterCommon 0xF30014F0` allocates `0xF0` bytes; ctor `0xF3001670` stores
FDO `+0x40` (64), PDO `+0x48` (72, `ObReferenceObject`'d), pre-allocated control IRP
`+0x60` (96). Worker (decompiler-verified):
1. `IoAllocateIrp(pdo->StackSize)`; installs callbacks `0xF3002350/0xF3002310/0xF30025A0`
   at adapter+136..160 and the magic **`3301` (0x0CE5)** at adapter+168 — the same
   handshake constant busb2902.sys returns on IOCTL `0x22004C`.
2. Sends control command **0** to the PDO (`0xF3002110`) → config fetch; config blob
   pointer cached at adapter+200; **frame size at adapter+132** (corrects r2 "+0x84")
   defaults 512, overwritten from `config[+8]` (dword at blob+8).
3. Registers subdevices via `0xF3001B40`:

| Subdevice | Name | Port CLSID | Miniport |
|---|---|---|---|
| Topology | `"Topology"` | `CLSID_PortTopology` `B4C90A32-5791-11D0-86F9-00A0C911B544` | custom 0x30 B, ctor `0xF3003F40` |
| Wave | `"Wave"` | `CLSID_PortWaveCyclic` `B4C90A2A-5791-11D0-86F9-00A0C911B544` | custom 0xA8 B, ctor `0xF30055F0` |

4. Creates two 112-byte (0x70) channel objects (ctor `0xF3002E90`) at adapter+80/+88:
   render (`renderFlag=1`) and capture (`0`).
5. `PcRegisterPhysicalConnection(FDO)` — **Topology pin 1 → Wave pin 1**, then
   **Wave pin 3 → Topology pin 0** (corrects the r2 "pin3↔pin3" reading).
6. Marks started (adapter+40), calls `0xF3002C50`, sends control command **2**.

## 2. Private control protocol to busb2902.sys (command set enumerated)

Sender `0xF3002110(adapter, cmd)`: reuses the cached IRP; sets
`MajorFunction = 0x0F (IRP_MJ_INTERNAL_DEVICE_CONTROL)` and passes `cmd` as a 32-bit
value in the stack location (buffer glue `0xF3002DF0`, completion `0xF30025A0`);
spin-guarded via `0xF3002280/22A0/22C0`. `IofCallDriver(pdo)`.

**Observed commands** (callers decompiled):

| cmd | Sender | Use |
|---|---|---|
| 0 | StartDevice worker `0xF3001C90` | config fetch → 98584-byte blob |
| 1 | `0xF30018B0` | (pre-start/prepare call from adapter init path) |
| 2 | StartDevice worker | started/running notification |
| 3 | `0xF3002460`, `0xF3002860` | per-stream state ops |
| 5 | `0xF3002B40` | stream/channel op |
| 6 | `0xF3002920` | channel-level op (called with `adapter-32`, i.e. from channel object) |

Matches the busb2902-side handler `sub_F1025020` (child DeviceType 0x8003 routing in
`0xF10230D0`). **A reimplementation must reproduce this internal IRP protocol or merge
both roles into one driver** (izukidio approach: separate protocol doc, protocol.h).

## 3. Config blob (98584 bytes) — layout partially cracked (new finding)

The blob fetched via command 0 (cached adapter+200) is the same data user-mode reads
back with IOCTL `0x22000C`. Size `0x18118 = 98584`. Decompiler evidence:

- `config[+8]` (dword) = per-transaction frame size (→ adapter+132).
- Format matcher `0xF3008400` walks `base[12322*t + 10 + 770*i + 48*j]` on a
  **`__int16`-typed** base: terminal stride `12322*2 = 0x6044`, format-group stride
  `770*2 = 0x604`, record stride `48*2 = 0x60`, record size `0x4C` (WAVEFORMATEX-style:
  channels, min/max rate at +0x20/+0x24, bits, tags).
- `4 × 0x6044 + 8 = 0x18118` — **the blob is an 8-byte header + 4 terminal blocks of
  24644 bytes each** (render/capture × jack groups for the PCM2902 descriptors).
- Unit/terminal lookups `sub_F30077C0(buf, id, subid)` constants seen: `0xA4E`,
  `0x2040`, `0x6000`, `0x1210` (with sub-ids 24/25/26) — queries into the cached USB
  descriptor database.
- Pin builder `0xF30048F0`: 112-byte (0x70 = `PCPIN_DESCRIPTOR`) entries, count from
  `config[+0x14]`, array at `config[+0x18]`; per-pin fields +56 (0x38) zeroed,
  flow/communication at +72 (0x48) compared to 1 (in) / 2 (out).

## 4. Ports / pins / formats (verified structure)

- Topology miniport: static `PCFILTER_DESCRIPTOR` at `0xF300B358` — 4 pins
  (0x70 bytes each @`0xF300B0B0`), 4 nodes (0x20 @`0xF300B270`), 6 interfaces,
  KS node/property GUID families as in the r2 pass (volume/mute/sum +
  DAC/ADC-class); volume handlers `0xF30045E0/0xF3004820`.
- Wave miniport: descriptor built at runtime (`0xF3007170` → builder `0xF30048F0`,
  cached at this+160); 0 pins → `"No Inputs or Outputs for system sound found.\n"`,
  `STATUS_INVALID_DEVICE_REQUEST`. Ctor defaults: block size 256 (`+0x80`),
  **44100 Hz** (`+0x84`).
- Static `KSDATARANGE`s: TYPE_WAVE/AUDIO, SUBTYPE_PCM, FORMAT_WaveFormatEx specifier,
  `CLSID_MiniportDriverWaveCyclic` `B4C90A27-…` — WaveCyclic data ranges on the
  dynamically built render/capture pins.
- Format negotiation `0xF3008400` against the blob's per-terminal format tables (§3).
- `NewStream 0xF30061F0`: validates `WAVEFORMATEX` against cached lists
  (this+0x90/+0x98), allocates 0x58-byte stream object; per-op control IRPs
  (commands 3/5/6, §2); timer-simulated DMA (`Ke*Timer` + DPC), no hardware DMA.
- **100% WaveCyclic** — deprecated on Windows 10/11 (see §6).

## 5. INF / device interfaces

Driver for PDO enumerated by busb2902.sys with hardware id `MEDIA\BUSB_AUDIOADAPTER`
(INF-only string; not in the binary). Device interfaces: `KSCATEGORY_AUDIO` +
`"Wave"`/`"Topology"` (RENDER/CAPTURE), KS proxy CLSID
`{17CCA71B-ECD7-11D0-B908-00A0C9223196}`, FriendlyName "BEHRINGER USB AUDIO",
class MEDIA `{4d36e96c-…}`. `sbemul.sys` NTMPDriver reference is legacy noise.

## 6. Windows 10/11 compatibility notes

- **WaveCyclic dead-end**: modern replacement is `IMiniportWaveRT` or a class-driver
  route (`usbaudio.sys`/`usbaudio2.sys`). PortCls itself still ships.
- Single-instance AddDevice hack (refcount + 5 s retry loop) — fragile; a redesign
  should drop it.
- Private `IRP_MJ_INTERNAL_DEVICE_CONTROL` protocol (§2) is the kernel-side contract
  that must be preserved for the original pair, or eliminated by merging roles.
- 2009-signed binary: re-sign or testsigning needed regardless.

## 7. Remaining gaps

1. Full field map of the 24644-byte terminal block (header + format-group table
   boundaries beyond the strides confirmed in §3).
2. Commands 1/3/5/6 payload buffers (sizes + semantics) — finalize on hardware
   bring-up against the original DLL/driver pair.
3. Topology node-name ↔ GUID exact mapping (needs Windows KS GUID headers; cosmetic).
