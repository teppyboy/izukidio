# 07 — Consolidated findings & PoC status (updated)

Consolidates everything resolved since `00-overview.md`. Read this first;
the per-binary deep dives (01–03), the PoC design (05), and the USBPcap
deep dive (06) remain the detailed evidence base.

## 1. Status of every open question

| # | Question | Status | Answer |
|---|---|---|---|
| 1 | pcap "slot 0 permanently zero-length" (06 §3) | **RESOLVED** | Parse artifact: USBPcap iso descriptor +4 field is an *exclusive-prefix* offset, so packet 0 always reads 0. No reserved slot-0 skip exists. IN = 9 × 196 B + one zero-length **final** slot (1764 B/10 ms = 44.1 kHz exact); OUT = 10 × 192 B (1920 B/10 ms = 48 kHz exact). (06 §3.1) |
| 2 | Where does 44.1 kHz repack happen | **RESOLVED** | Device-side. The PCM2902 packetizer emits 49-frame (196 B) IN packets on its own schedule while the host requested 192 B/slot at the 48 kHz alt; the USB stack overwrites the completion descriptors with delivered bytes. izukidio relays `IsoPacket[i].Length` per completion. |
| 3 | Where `IsoPacket[i].Length` is assigned at submit (01 gap) | **RESOLVED** | `sub_F1022390` (01 §5.3): Length = bytes pulled from the client queue for that slot; 0 when the queue starves. `TransferBufferLength = Σ lengths`. No ASAP flag — `StartFrame += packetCount` per cycle (`sub_F1015E10`). |
| 4 | Per-rate packet geometry | **RESOLVED** | `sub_F1018BE0` pattern tables (01 §5.3): 48k uniform 48 dwords; 44.1k 44 dwords +1 every 10th (avg 44.1 = 176.4 B/ms); envelope ±2 dwords. |
| 5 | Trim policy (±4 B submit deviations) | Open | 124 × 188 B among 30,130 submit slots. Temporal distribution not yet measured — pacing vs resync bursts unknown. |
| 6 | OUT underrun behavior (30 ms pool) | Open | Needs live hardware. |
| 7 | OUT rate at 44.1 k sessions | Open (noted) | Measured capture ran OUT at 48 kHz while IN delivered 44.1 kHz. Whether the original ever trims OUT slots is unverified. |

## 2. PoC (izukidio) completeness vs the original

Implemented and verified against the measured/decompiled model:

- Dual-mode `DriverEntry` (INF/PnP + kdmapper bootstrap, `mapper.cpp`).
- SELECT_INTERFACE alt1 for both audio interfaces (IF1 EP 0x02 OUT, IF2
  EP 0x84 IN), pipe scan for both (usb.cpp/device.cpp).
- Shared-area contract ported byte-exact: engines (201600 dwords each),
  32-byte tail with overflow counter, ASIO event (03 §3).
- Ring writer/reader (`Izk_RingWrite`/`Izk_RingRead`, port of
  `sub_F1007B30`) + fan-out overflow accounting.
- Isoch engines: 10-slot 10 ms URBs, pools 12 IN / 3 OUT, per-packet
  publish from `IsoPacket[i].Length`, IN buffer stride = MaxPacketSize.
- 44.1 kHz OUT pattern (44 dwords +1 every 10th, `IzkSlotRequestBytes`)
  and absolute `StartFrame` pacing (+10 per cycle, matching
  `sub_F1022390`/`sub_F1015E10`).
- Overflow counter floored at 0 via `InterlockedExchange` (original
  behavior, divergence 3 closed).

Remaining known divergences (deliberate or hardware-gated): see 05 §5.

## 3. Build & CI

- Local: `build.bat` (MSBuild + WDK, Release|x64) — needs a Windows box.
- CI: `.github/workflows/release.yml` builds the solution on
  `windows-latest` on **every push** and via `workflow_dispatch`, packages
  the outputs into a password-protected archive (password `izuki`) and
  publishes a `ci-N` release per run.
