# busbwdm.sys — Static Analysis (BEHRINGER USB WDM Audio 2.8.40)

**Target:** `BEHRINGER_2902_X64_2.8.40/busbwdm.sys` — x64 PE native kernel DLL (Subsystem NATIVE), PortCls adapter driver.
**Vendor strings:** Ploytec GmbH (`info@ploytec.com`), signed (GlobalSign ObjectSign CA, VeriSign timestamp).
**Tooling note:** Ghidra MCP tools are NOT available in this session's tool allowlist (`mcp__ghidra` namespace absent). Analysis performed instead with **radare2 6.2.2** (`/opt/homebrew/bin/r2 -A`), **objdump/strings**, and **python3 + pefile** (`/tmp/rev/bin/python`). No decompiler (r2ghidra) installed — everything below is from r2 disassembly (`pdf`), manual x64 reading, and data-section decoding. All addresses use image base `0xF3000000`. Static analysis only; driver never loaded/executed.

**File layout:** `.text 0x8810` `.rdata 0x6d4` `.data 0x3d0` `.pdata 0x5dc` `INIT 0x2ce` `.rsrc` `.reloc`. ~100 functions, no exports.

**Imports (complete):**
- `NTOSKRNL.exe`: `ExAllocatePool`, `ExFreePool`, `KeInitializeTimer`, `KeInitializeTimerEx`, `KeSetTimer`, `KeSetTimerEx`, `KeCancelTimer`, `KeInitializeDpc`, `KeDelayExecutionThread`, `ObfReferenceObject`, `ObfDereferenceObject`, `IoAllocateIrp`, `IoFreeIrp`, `IofCallDriver`, `memcmp`, `DbgPrint`
- `portcls.sys`: `PcInitializeAdapterDriver`, `PcAddAdapterDevice`, `PcNewPort`, `PcNewMiniport`, `PcNewServiceGroup`, `PcRegisterSubdevice`, `PcRegisterPhysicalConnection`

No `IoCreateDevice`, no DMA adapter calls (`HalGetDmaAdapter`/`GetDmaAdapter` absent — PortCls/WaveCyclic DMA is software-based, see §2/§5). No direct USB imports (goes through busb2902.sys, §3).

---

## 1. DriverEntry / AddDevice

### DriverEntry (`0xF3001000`, in `.text` not INIT)
```
DriverEntry(DriverObject, RegistryPath):
    return PcInitializeAdapterDriver(DriverObject, RegistryPath, AddDevice = 0xF3001040)
```
Standard PortCls adapter pattern. The `INIT` section (0x2CE bytes) contains an additional one-time init/exit helper block, not DriverEntry itself.

### AddDevice (`0xF3001040`)
```
AddDevice(DriverObject, PhysicalDeviceObject):
    return SerializeAndAddAdapter(DriverObject, PDO, StartDevice = 0xF3001080,
                                  MaxSections = 3, DeviceExtensionSize = 0)
```

### Serialized wrapper around PcAddAdapterDevice (`0xF3008DA0`)
Notable vendor quirk — single-instance enforcement via a global refcount + sleep loop:
- Global counter at `.data:0xF300B3C0`, saved PDO stored at global `0xF300B3B8`.
- `InterlockedIncrement`; if the result is 1 (first caller), it writes the PDO to `0xF300B3B8` and calls `PcAddAdapterDevice(DriverObject, PDO, StartDevice, 3, 0)`.
- Otherwise (a second device instance arriving): decrement, `KeDelayExecutionThread(KernelMode, alertable=1, 0.5 ms)` — up to 20 retries (budget counter starts at `0x1388` = 5000 × 100 ns units) — then return `STATUS_WAIT_TIMEOUT (0x102)`.
- On `PcAddAdapterDevice` failure it calls `0xF3008D70` which clears the global PDO slot.
=> **Only one BEHRINGER USB audio adapter instance is supported per boot.**

### StartDevice callback (`0xF3001080` → worker `0xF3001C90`)
`StartDevice(DeviceObject, Irp, ResourceList)`:
1. `context = GetPdoFromGlobal(0xF3008D70)` — fetches the PDO captured by AddDevice (global `0xF300B3B8`) and zeroes the global.
2. `adapter = NewAdapterCommon(DeviceObject, pdo)` (`0xF30014F0`): allocates `0xF0` bytes, constructs via `0xF3001670`, AddRef'd. Constructor stores:
   - `+0x40` = FunctionalDeviceObject, `+0x48` = the busb2902-owned **PDO** (ObReferenceObject'd) — this is the target for all control IRPs (§3).
   - `+0x20` = secondary vtable `0xF300A140` (power/IUnknown helpers), `+0x84 = 0x200` (512, default "packet/frame size" — overwritten later from bus-provided config), `+0x28` = started flag.
3. `StartDeviceWorker(adapter, ResourceList)` (`0xF3001C90`), see §2.
4. `adapter->vtbl+0x10` (Release) on failure paths.

---

## 2. Ports / miniports / pins / formats

### Subdevice registration (`0xF3001C90` worker)
Worker does (in order):
1. Allocates one IRP for the bus control channel: `IoAllocateIrp(pdo->StackSize)` stored at `adapter+0x60`; target PDO at `adapter+0x48`.
2. Sends initial control IRP command `0` to the PDO (§3) — this triggers busb2902 to export its configuration; result cached (`adapter+0xC8`), and `adapter+0x84` (frame size) overwritten from `[config+8]`.
3. Registers two subdevices via helper `0xF3001B40(AdapterCommon, type, ResourceList, Name, CLSID, &outFilter)`:

| Subdevice | Name | Port CLSID | Miniport |
|---|---|---|---|
| Topology | `"Topology"` | `CLSID_PortTopology` `B4C90A32-5791-11D0-86F9-00A0C911B544` | custom, `0x30` bytes, ctor `0xF3003F40` (helper `0xF30015F0`) |
| Wave | `"Wave"` | `CLSID_PortWaveCyclic` `B4C90A2A-5791-11D0-86F9-00A0C911B544` | custom, `0xA8` bytes, ctor `0xF30055F0` (helper `0xF3001570`) |

Helper details (`0xF3001B40`): `PcNewPort(&port, rclsid)`; for `type==0` builds topology miniport, `type==1` builds wave miniport (a `PcNewMiniport` fallback branch exists for other `type` values but is unreachable with the two calls present); `port->vtbl+0x18` = `IPort::Init(miniport, ResourceList, ...)`; `PcRegisterSubdevice(DeviceObject, Name, port)`; `QueryInterface(port, IID_IUnknown, out)` returns the filter unknown.

4. Creates two internal "channel" objects (class ctor `0xF3002E90`, size `0x70`, vtables `0xF300A180/A190`): `new(adapter+0x88, renderFlag)`. First with `renderFlag=1` → `adapter+0x50`, second with `0` → `adapter+0x58`. Byte `+0x24` selects render vs capture behaviour. These are the render/capture engine objects used by the wave streams.
5. **Physical connections** (`PcRegisterPhysicalConnection`, DeviceObject = `adapter+0x40`):
   - Topology filter **pin 1** → Wave filter **pin 1**
   - Wave filter **pin 3** → Topology filter **pin 3**
   (bridge-pin wiring: topology bridge pins ↔ wave pins; asymmetric pin numbers imply the wave miniport exposes ≥3 pins, i.e. render and capture paths on separate pins.)
6. Sends control IRP command `2` (adapter "started"/running notification, §3).

### Topology miniport descriptor (static, `.data`)
`IMiniportTopology::GetDescription` (`0xF3004200`) returns a `PCFILTER_DESCRIPTOR` at `0xF300B358` (pointer `0xF300B350` returned; struct fields land at +8):
- `PinSize=0x70, PinCount=4`, pins at `0xF300B0B0`
- `NodeSize=0x20, NodeCount=4`, nodes at `0xF300B270`
- `InterfaceCount=6`, interfaces at `0xF300B2F0` (`KSPIN_INTERFACE` set `KSINTERFACESETID_Standard` family, IDs 0–3)
- `CategoryCount=0` (no filter categories in descriptor — INF supplies `KSCATEGORY_AUDIO/RENDER/CAPTURE` via device interfaces)

Pins (each: 1 interface @`0xF300B040`, 1 medium @`KSMEDIUMSETID_Standard` `6DBA3190-67BD-11CF-A0F7-0020AFD156E4`, one data-range ptr, category GUID):
- pin0/2 reference ranges at `0xF300A2E8`; pin1/3 at `0xF300A2D8` and `0xF300A2C8`; pin category GUIDs `KSCATEGORY_AUDIO` and node types `DFF21CE1-0F07-11D1-B917-00A0C9223196` (KSNODETYPE_SPEAKER-family), `DFF21FE3-0F07-11D1-B917-00A0C9223196` (synth/legacy-audio family), `DFF21FE4-...` — classic render+capture bridge pin layout.
- 4 nodes (`PCNODE_DESCRIPTOR`), type GUIDs from the `185FEDEx-9905-11D1-95A9-00C04FB925D3` family (`185FEDE5`, `185FEDE6`, `185FEDEB`, `185FEDEC`, `185FEDF7` — the KS volume/mute/sum-class node-type family) plus `3A5ACC00-C557-11D0-8A2B-00A0C9255AC1` and `02B223C0-C557-11D0-8A2B-00A0C9255AC1` (DAC/ADC-class `C557-11D0-8A2B` family). Exact node-name mapping not verifiable offline; the pattern is: volume/mute nodes feeding speaker (out) and mic-class (in) pins.
- A Volume property handler pair exists in the miniport (`0xF30045E0`, `0xF3004820` — get/set style, 452/200 bytes) and GUID `A80F29C4-5498-11D2-95D9-00C04FB925D3` at `0xF300A328` (property-set family; likely volume property set used by the topology nodes).

### Wave miniport: **descriptor is built at runtime**
`IMiniportWaveCyclic::GetDescription` (`0xF3005B60`) calls builder `0xF3007170`/`0xF30048F0`, caches result at `this+0xA0`, and returns it. If the built descriptor has **0 pins** it `DbgPrint`s:
`"No Inputs or Outputs for system sound found.\n"` and fails with `STATUS_INVALID_DEVICE_REQUEST (0xC0000184)`.
- Wave miniport ctor defaults: `+0x80 = 0x100` (256 — default transfer block size), `+0x84 = 0xAC44` (**44100 Hz** default sample rate).
- Builder walks the config table retrieved from busb2902.sys (§3): pin entries are `0x70`-byte records (matches `PCPIN_DESCRIPTOR` size), counts taken from `[config+0x14]`, array at `[config+0x18]`; per-pin fields include data-flow/communication (values `1`/`2` compared; `0x48` field = flow type, `0x38` zeroed for both).
- Builder also queries device identity: reads two `WORD`s (`[vid]`, `[vid+2]` — VID/PID of the USB device) and runs a set of small config lookups (`0xF30077C0(buf, 0x0A4E, 0x4040)` / `0x2040` / …) — feature-unit/terminal queries against the bus driver's cached USB descriptor database.
- Static `KSDATARANGE` data in `.rdata` used for the dynamically built pins:
  - `KSDATAFORMAT_TYPE_WAVE` `{00000001-0000-0010-8000-00AA00389B71}` @`0xF300A3D0`
  - `KSDATAFORMAT_TYPE_AUDIO` ('auds') @`0xF300A3E0`
  - `KSDATAFORMAT_SUBTYPE_PCM` `{0F64 17D6-C318-11D0-A43F-00A0C9223196}` @`0xF300A3F0`
  - `FORMAT_WaveFormatEx` `{05589F81-C356-11CE-BF01-00AA0055595A}` @`0xF300A3C0` (specifier)
  - `CLSID_MiniportDriverWaveCyclic`-class GUID `B4C90A27-5791-11D0-86F9-00A0C911B544` @`0xF300A3B0`; stream/miniport IIDs `B4C90A24…`/`B4C90A28…` (`0xF300A308`/`0xF300A4A0`), `518590A2-A184-11D0-8522-00C04FD9BAF3` (`0xF300A490`) — QI targets for the wave pin interface arrays.
- **Format negotiation** (`0xF3008400`, the big format matcher): walks a per-pin table with outer stride `0x6044` per terminal, inner entries of `0x604` bytes per format group, each group holding `0x60`-stride records of `0x4C` bytes — a `WAVEFORMATEX`-style record (channels @0, min/max sample-rate range @0x20/0x24 compared, bits, plus tag bytes). Matching a stream's format picks the USB alternate setting/format slot. => **Sample rates and channel counts are not hard-coded; they come from the PCM2900/2902 USB descriptors fetched at runtime via busb2902.sys** (PCM2902 supports 48k/44.1k families; 44100 is the driver default).
- `IMiniportWaveCyclic::Init` (`0xF30057E0`): QI's the port for `84CCD035-9EB5-4257-8700-7E92965BDEB0` (portcls port helper interface, exact name unverified), `PcNewServiceGroup`, initializes the two channel objects (`0xF3002AF0/0xF30029E0/0xF3002A50` family) and registers DPC/timer glue.
- `NewStream` (`0xF30061F0`): validates `WAVEFORMATEX` against cached format lists (`this+0x90/+0x98`), allocates a `0x58`-byte stream object (ctors `0xF30078A0`/`0xF3007A80`), wires it to a render or capture channel object.
- Stream methods (`0xF3006BF0` etc.): per-operation control IRPs (`0xF3008400` calls at `0xF3006D3D/D B3/E34/EBA` — set-format, set-state; `0xF3006BB0` calls — position/pointer reads). Uses `KeSetTimer/KeCancelTimer/KeInitializeTimerEx` (`0xF30011E2/1222/13F7`) for simulated DMA timing — **WaveCyclic-style software timer "DMA"**, no hardware scatter/gather.

---

## 3. Connection to busb2902.sys

- **Enumeration:** the driver is the *function* driver for the `MEDIA\BUSB_AUDIOADAPTER` device node (INF `busbwdm.inf`: `%AUDIO_DEVICE_USB.DeviceDesc%=BUSB_AUDIO,, MEDIA\BUSB_AUDIOADAPTER`). busb2902.sys enumerates that PDO from the USB device; busbwdm attaches above it. No `MEDIA\BUSB_AUDIOADAPTER` string exists inside busbwdm.sys itself (INF-only).
- **Control channel:** AddDevice stashes the PDO in global `0xF300B3B8`; StartDevice moves it to `AdapterCommon+0x48` (ObReferenceObject'd). One IRP is pre-allocated (`IoAllocateIrp`, `adapter+0x60`).
- **Command IRPs:** `0xF3002110(adapter, command)`:
  - builds the stack location: `MajorFunction = 0x0F (IRP_MJ_INTERNAL_DEVICE_CONTROL)`, a small command code in the parameters (`0` = initial config fetch, `2` = start/running), buffer glue via `0xF3002DF0`, completion routine `0xF30025A0` with context `0xF3002350`,
  - `IofCallDriver(pdo, irp)`; synchronous handling (spin-guard helpers `0xF3002280/22A0/22C0` = interlocked refcount wrappers; KeDelayExecutionThread wait loop if busy).
  - Callers: StartDevice (cmd 0 / cmd 2), `0xF3002460`, `0xF3002860`, `0xF3002B40`, `0xF3002CE0` — the control-request surface toward busb2902.sys (config fetch, device state, per-stream ops).
- The returned config blob (`adapter+0xC8`, `+0x84` frame size, format table consumed by `0xF3008400`, pin table consumed by `0xF30048F0`, VID/PID via `0xF3002AD0`) is the entire contract with the bus driver — **a private IRP-based protocol, not USB requests from this driver**. busb2902.sys owns the USB pipe access; reimplementers must reproduce this private internal-device-control protocol or merge both roles into one driver.
- ASIO side note: `busbasio_x64.dll` talks to busb2902.sys via `CreateFileA`/`DeviceIoControl` (user-mode), i.e. two distinct channels into the bus driver exist (kernel internal-control IRPs from busbwdm, user-mode IOCTLs from the ASIO DLL).

## 4. KS interfaces / topology

- **Device interfaces (from busbwdm.inf, matching the registered subdevices):** `KSCATEGORY_AUDIO` + `"Wave"` (also RENDER/CAPTURE), `KSCATEGORY_AUDIO` + `"Topology"`; proxy CLSID `{17CCA71B-ECD7-11D0-B908-00A0C9223196}` (standard KS proxy); FriendlyName "BEHRINGER USB AUDIO"; wdmaud/swmidi/redbook associated filters; driver class MEDIA `{4d36e96c-e325-11ce-bfc1-08002be10318}`.
- Topology filter: 4 pins / 4 nodes / volume-mute-class nodes (see §2); physical connections pin1↔pin1, pin3↔pin3 to the Wave filter.
- Wave filter: dynamic pins (render + capture), WaveCyclic data ranges (TYPE_WAVE/AUDIO + SUBTYPE_PCM + WaveFormatEx specifier).

## 5. Windows 10/11 compatibility notes

- **WaveCyclic is dead on Windows 10/11**: `IMiniportWaveCyclic` / `PortWaveCyclic` are deprecated since Vista, removed from modern driver models (PortCls still ships the port in newer builds, but WaveCyclic miniports are legacy-only; Microsoft's replacement path is `IMiniportWaveRT` or, preferably, a USB Audio 2.0 class driver / `usbaudio2.sys`, or kmdf + `IUsbDevice`-based custom). This driver is 100% WaveCyclic (`CLSID_PortWaveCyclic` imported, `CLSID_MiniportDriverWaveCyclic` GUID present, timer-simulated DMA).
- **No WaveRT**: no `RegisterDmaBuffer`, no `KSPROPERTY_RTAUDIO` usage, no DMA adapter — the timing model (KeTimer + DPC + packet-size `0x200` accounting at `adapter+0x84`) is fundamentally WaveCyclic and would need full rework for WaveRT.
- **PortCls expectations**: `PcInitializeAdapterDriver` + `PcAddAdapterDevice` pattern still works on Win10/11 (portcls.sys present), but the driver would need re-signing (it is a 2009 Ploytec-signed binary; WHQL/signing enforcement differs) and the single-instance AddDevice hack (`0xF300B3B8/B3C0` globals + 10 ms sleep loop) is not multi-instance-safe.
- `IRP_MJ_INTERNAL_DEVICE_CONTROL` private protocol to busb2902.sys must be preserved or eliminated; a modern redesign would merge the bus function driver + miniport roles.
- `KeDelayExecutionThread` with alertable=1 in AddDevice serialization is unusual and fragile under modern PnP (AddDevice at PASSIVE_LEVEL is fine, but the 20×0.5 ms stall is a red flag).
- NTMPDriver references `sbemul.sys` (INF) — a legacy Sound Blaster emulation shim, meaningless on Win10/11.

## Function inventory (r2 labels)

| Address | Size | Role (inferred) |
|---|---|---|
| `0xF3001000` | 50 | DriverEntry → PcInitializeAdapterDriver |
| `0xF3001040` | 63 | AddDevice |
| `0xF3001080` | 149 | StartDevice callback |
| `0xF3001C90` | 1029 | StartDevice worker (subdevices, physical connections) |
| `0xF3001B40` | 327 | RegisterSubdevice helper (PcNewPort+miniport+Init+Register) |
| `0xF30014F0` | 117 | NewAdapterCommon (0xF0 obj) |
| `0xF3001670` | 525 | AdapterCommon ctor |
| `0xF3001570/15F0` | 118 | New wave / topology miniport |
| `0xF30055F0` | 152 | Wave miniport ctor (0xA8 obj) |
| `0xF3003F40` | 74 | Topology miniport ctor (0x30 obj) |
| `0xF3004200` | 28 | Topology GetDescription (static desc `0xF300B358`) |
| `0xF3005B60` | 113 | Wave GetDescription (dynamic desc, cache +0xA0) |
| `0xF3007170` | 870 | Descriptor builder part 1 (pins, VID/PID, config queries) |
| `0xF30048F0` | 2601 | Descriptor builder part 2 (0x70-byte pin entries) |
| `0xF3008400` | 1827 | Format matcher (0x6044/0x604/0x4C-stride tables) |
| `0xF30061F0` | 720 | NewStream |
| `0xF30078A0/7A80` | 210/272 | Stream object ctor (0x58 obj) |
| `0xF3006BF0` | 1397 | Stream property/state handlers |
| `0xF3002E90` | 255 | Channel object ctor (0x70 obj, render/capture flag) |
| `0xF3002110` | 356 | Control IRP sender (IRP_MJ_INTERNAL_DEVICE_CONTROL → PDO) |
| `0xF3002DF0` | 157 | IRP buffer builder |
| `0xF30025F0/2460` | 621/287 | IRP completion handling |
| `0xF3008DA0` | 275 | Serialized PcAddAdapterDevice wrapper |
| `0xF30045E0/4820` | 452/200 | Volume-related property handlers |
| `0xF3008C80` | 78 | ExAllocatePool wrapper (NonPaged) |

## Confidence / gaps

- Decomilation was manual (r2 disassembly only, no decompiler) — control-flow summaries are faithful; exact struct field names for the runtime pin/format tables are reconstructed from stride patterns (0x70/0x6044/0x604/0x4C), not verified symbolically.
- Some GUIDs intentionally left partially identified (marked "family"); exact `KSNODETYPE_*` mapping unverified offline.
- The `84CCD035-9EB5-4257-8700-7E92965BDEB0` interface QI'd in `IMiniportWaveCyclic::Init` and the `A80F29C4-5498-11D2-95D9-00C04FB925D3` property set are not named in this report (no offline GUID DB).
- INIT section block and `0xF3009220`-region (0xF3008F60–0xF30097F0, nondelegating QI dispatch) analyzed only superficially.
