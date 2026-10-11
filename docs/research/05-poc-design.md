# 05 — PoC Driver Design (izukidio)

What the VS project in `izukidio/` implements, mapped against the verified
research (01: busb2902.sys, 02: busbwdm.sys, 03: busbasio DLL). Read those
first; this file is the design/implementation map, not the protocol reference.

## 1. Goal and non-goals

**Goal:** a Windows 10/11 x64 WDM function driver for `USB\VID_08BB&PID_2900/2902`
(TI PCM2902, USB Audio Class 1.0) that keeps the **original ASIO DLL
(`busbasio_x64.dll`) usable unchanged** by reproducing its device interface,
IOCTL contract, and zero-copy shared area.

**Non-goals (tracked, not implemented):**
- MME/WASAPI playback path (original: busbwdm.sys PortCls WaveCyclic miniport on
  the 0x8003 child PDO) — deprecated design; WASAPI users should use the
  in-box `usbaudio.sys` stack instead. izukidio binds the raw USB device, so
  coexistence needs an INF filter decision (below).
- MIDI child (0x8004 PDO).
- Firmware update channel (`\FW_UPDATE`, FWUPDATER — intentionally not analyzed).
- Windows 7/8 compatibility (GetVersionExA whitelisting is DLL-side, harmless).

## 2. Architecture

```text
busbasio_x64.dll (unmodified)
  | SetupDiGetClassDevsA over {090E2CEE-44C0-4263-8837-786AA85A49C6}
  | CreateFile "<if>\IO" (data) / "<if>" (control)
  v
izukidio.sys  (WDM, FDO DeviceType 0x8002 — matches original validation)
  driver.cpp  DriverEntry/AddDevice/dispatch wiring, DosSymLink
  device.cpp  create/close, PnP start (IzkPnpStartCompletion), shared-area
              register/unregister (0x220030), 0x22000C state blob, props
  usb.cpp     USBD_CreateHandle, select config (ifc1 alt1 + ifc2 alt1), dual
              endpoint scan, vendor/class passthrough (0x220008), SET_CUR sample rate
  isoch.cpp   ISOCH URB ring, ring writer/reader port of sub_F1007B30,
              overflow accounting, ASIO event signal
  ioctl.cpp   IOCTL contract (below)
  protocol.h  byte-exact contract constants
  pcm2902.h   UAC request shapes (bmRequestType/bRequest/wValue/wIndex/wLength)
```

## 3. IOCTL contract status

| IOCTL | Name | Status | Notes |
|---|---|---|---|
| 0x220000 | GET_NAME | done | `"name\xA7<24B LicenseKey>"` (0xA7 delimiter) |
| 0x220004 | GET_NAME2 | done | second name |
| 0x220008 | VENDOR_CLASS_REQ | done | passthrough, 8-byte setup-shaped header |
| 0x22000C | GET_STATE | done | 0x18118 blob, frame size at +8 (busbwdm reads it) |
| 0x220014 | GET_CONFIG | done | 76 B (0x4C) config struct |
| 0x220018 | SET_PARAM | done | StreamUnitParam, clamped ≥1 (was alt-select — fixed) |
| 0x22001C | STOP_CYCLE | done | ends with IOCTL_INTERNAL_USB_CYCLE_PORT (0x22001F) |
| 0x220024 | GET_HWINFO | done | 24 B (0x18) |
| 0x220030 | SHARED_AREA | done | 16 B desc `{u8 start @0; u64 engineA VA @+8}` → MDL lock, kernel map, event ref (EVENT_MODIFY_STATE), thread ref (THREAD_SET_INFORMATION) + priority 31 |
| 0x220038 | GET_CLOCK | done | 8 B latched clock, read-clear |
| 0x22004C | GET_VERSION | done | must return 3301 (0x0CE5) or the DLL hard-errors |
| 0x220064/68 | GET/SET_ROUTING | done | 3,844 B (0xF04) routing tables, latched |
| 0x22006C | RELEASE_SLOT | done | stream slot release |
| 0x220070/09C | misc status | done | 4 B values (DLL init path) |
| 0x2200A0 | FILE_READY | done | file-ready latch (=1 after shared-area start) |
| 0x2200A4 | GET_BUILD | done | 0x0207A800 |
| 0x2200B0 | REGISTER_PID | done | PID in OUT buffer (4 B) |
| 0x2200B4 | GET_SHARED_COUNT | done | 4 B read-clear |
| 0x2200B8 | GET_POSITION | done | 4 B |
| 0x2200BC | GET_CLOCK2 | done | 8 B |
| 0x2200C0/C4/C8 | stream copy | n/a | **not used by ASIO DLL** (MME-side in original); 0x2200C8 reserved READ_STREAM_ALT |
| 0x2200D0 | PROPERTY | done | `IZUK_PROPERTY_STRUCT{Command, Arg[64]}`; cmds 10 version, 12 priority names, 13/14 priority get/set, 19/20 names |

## 4. Zero-copy data plane (the core)

Verified byte-exact (research 03 §3, 01 §5.1/5.2):

- Shared area **0x189C38 B** = engine A (0xC4E0C) + engine B (0xC4E0C) + 32 B tail.
- Engine = `{ULONG writeIndex; ULONG readIndex; ULONG totalWritten; ULONG ring[201600];}`.
- Engine A = playback (kernel drains at `readIndex`), engine B = capture
  (kernel publishes at `writeIndex`), tail = event/thread handles + overflow dword.
- `Izk_RingWrite` is a verbatim port of `sub_F1007B30` (wrap at 201600 dwords,
  accumulator clamp, `InterlockedAdd`); `Izk_RingRead` is the mirror drain.
- Capture completion publishes the URB buffer into engine B, accounts dropped
  dwords into `tail+28`, then `KeSetEvent` on the registered user event.
- Playback submission drains engine A into the URB buffer (zero-filled on
  underrun) before each `URB_FUNCTION_ISOCH_TRANSFER` with ASAP.

Divergences from the original (deliberate, revisit on hardware bring-up):
1. Original walks **512-byte packets** with a per-packet payload parser and
   channel DMA encode/decode callbacks; izukidio copies whole-URB payload
   assuming 4-byte slots (`channels × bytesPerSample`, padded to 4).
2. Original runs **10-slot, 10 ms URBs** in pools of 12 IN / 3 OUT (measured,
   research 06 §3; submit-side per-slot length = bytes pulled from the client
   queue — a zero-length slot means starvation, research 01 §5.3). izukidio
   now matches: 12/3 URBs × 10 packets, absolute StartFrame pacing seeded
   from `URB_FUNCTION_GET_CURRENT_FRAME_NUMBER` and advanced by the packet
   count per cycle (01 §5.3), and per-packet completion publish (06 §3.1).
   Remaining sub-divergence: izukidio derives per-slot request lengths from
   the rate pattern (`IzkSlotRequestBytes`, uniform BytesPerFrame except
   +1 dword every 10th slot at 44.1 kHz) instead of pulling per-slot bytes
   from the client queue — the ring model gives the same average rate.
3. ~~Overflow counter is not floored at 0~~ **CLOSED**: `Izk_AddOverflow` now
   floors via a CAS loop (original semantics).
4. Feedback endpoint (`sub_F1013EB0` resync, skip-4-frames) not yet consumed;
   the real UMC22 has **no** feedback endpoint (research 06 §6), so this stays
   dormant — the original's resync mechanism is per-slot request trims and
   queue-starvation zero-length packets (research 01 §5.3), not a reserved
   slot.
5. **Rate ≠ alternate setting**: the measured session served 44.1 kHz audio
   through the 48 kHz alternate (research 06 §3). izukidio now follows
   delivered `IsoPacket[i].Length` on IN (rate-agnostic) and applies the
   44.1 kHz request pattern on OUT (research 01 §5.3); the alt-setting is
   still fixed at 48 kHz — matching the measured original behavior.

## 5. Driver bring-up order (Windows machine)

1. Build Release x64; confirm `.sys` links (link errors = shim/wdk mismatch,
   report exactly).
2. `bcdedit /set testsigning on`, reboot, `pnputil /add-driver izukidio.inf /install`.
3. Plug device; check `!devnode 0 1`, service start, interface GUID published
   (`IoSetDeviceInterfaceState` on PnP start completion).
4. Run the original DLL's control plane first (regsvr32 + ASIO panel) —
   validates IOCTLs 0x220000–0x2200D0 without streaming.
5. ASIO host (reaper/foobar2000 ASIO): validate 0x220030 handshake, ring
   positions, then audio.
6. Debug via `DbgPrint` ("IZUKIDIO:" prefix) + WinDbg `!wdfkd` not needed (WDM).

## 6. Risks / open items

- **Coexistence with usbaudio.sys**: the INF claims the raw USB device; the
  in-box USB Audio stack will not also bind. If MME/WASAPI is needed in
  parallel, plan a lower-filter or swap the INF to a custom class.
- **URB geometry** (divergence 2): pools now match (12 IN / 3 OUT × 10 slots).
  Remaining gap: absolute StartFrame pacing vs ASAP, and per-slot request
  lengths from the client queue (research 01 §5.3) instead of uniform fills.
- **Rate repacking** (divergence 5): 44.1 k through the 48 k alt is measured
  behavior of the original (research 06 §3); izukidio is 48 k-only until the
  repack model lands.
- **xHCI timing**: ASAP isoched URBs pace fine, but original used StartFrame
  math; revisit if stutter appears.
- Property command IDs 10/12/19/20 verified from DLL strings, exact payloads
  pending first live session.
- Detailed packet-level evidence for everything above: **research 06**.
