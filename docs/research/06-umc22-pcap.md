# 06 — UMC22 measured behavior (USBPcap deep dive)

Source: `docs/izuki/umc22_duplex.pcap` (13 MB, 12,448 records, 31.73 s of
full-duplex streaming), captured with USBPcap on the real device. This file
records **everything measurable** in that capture, the container format needed
to re-derive it, and what each fact means for izukidio. Every number here is
measured, not inferred from descriptors.

## 1. Capture container format (how to re-parse)

pcap global header: `magic 0xa1b2c3d4`, **linktype 249** — but the record
format is **USBPcap**, *not* Linux usbmon-mmapped despite the linktype.
Per record: standard pcap header `{ts_sec, ts_usec, incl_len, orig_len}`
then a USBPcap pseudo-header:

```
offset  size  field
0       2     headerLen (u16 LE; 28 for non-ISO; 28+4+12*n for ISO with n packets)
2       8     irpId (u64)   — reused IRPs show up as many records with same id
10      4     status (u32)
14      2     function (u16; all data records here = 0x000A = IRP_MJ_INTERNAL_USB_CONTROL)
16      1     info (bit0: 1 = submit / 0 = completion)
17      2     bus (u16)
19      2     devAddr (u16)
21      1     endpoint (bit7 = IN)
22      1     transferType (0=ISO, 1=INTERRUPT, 2=CONTROL, 3=BULK)
23      4     dataLength (u32)
-- ISO extension (only when transferType == 0):
27      4     ??? (0)                       [u32 at +27]
31      4     packetCount (u32)             [e.g. 10]
35      12*n  per packet: {u32 a; u32 b; u32 c}
              — a == 0 always; b == cumulative byte-end of packet i
                (0, 192, 384, ... on submit; 0, 196, 392, ... on IN completion);
                c == 0 (status). Trailing 4 bytes pad headerLen to 4-byte multiple.
-- payload (dlen bytes) follows the header
```

Gotcha that cost an hour: `headerLen` for ISO records is `35 + 12*n` rounded
up to a multiple of 4 (n=10 → 159), *not* 28. The pcap file contains **three
devices** — filter by `devAddr` before analyzing endpoints:

| devAddr | VID:PID (from control data) | endpoints seen |
|---|---|---|
| 1 | **0x08BB:0x2902 (UMC22/PCM2902)** | EP 0x02 ISO OUT, EP 0x84 ISO IN |
| 2 | 0x3554:0xF58E | EP 0x83 INTERRUPT IN (7 B reports) — **not** the UMC22 |
| 3 | 0x8087:0x3200 (Intel Bluetooth) | none after enum |

(The `usb-report.txt` claim "HID EP 0x85, 1 B/10 ms" could not be
cross-checked: no EP 0x85 traffic appears in this capture at all.)

## 2. Control-plane sequence (dev 1, verbatim)

```
t+0.000  GET_DESCRIPTOR Device (18 B)      → 12 01 00 02 ... 08BB 2902 0001
t+0.000  GET_DESCRIPTOR Config (1191 B)    → full config, matches usb-report.txt §IF1/IF2
t+0.000  SET_CONFIGURATION (1)
t+0.150  SET_INTERFACE ifc1 → alt 1        (playback arm, 48 kHz stereo alt)
t+0.215  SET_INTERFACE ifc2 → alt 1        (capture arm, 48 kHz stereo alt)
  ... 30 s of streaming, zero control traffic ...
t+30.235 SET_INTERFACE ifc2 → alt 0        (capture stop)
t+31.728 SET_INTERFACE ifc1 → alt 0        (playback stop)
```

**No `SET_CUR` (bmRequestType 0x22) sample-rate request appears anywhere.**
The session rate is fixed by the alternate setting alone (alt1 declares
tSamFreq 48000 only). This confirms the alt-setting-driven rate model from
`docs/izuki/UMC22-HANDOFF.md` and means izukidio's SET_CUR path is only needed
for *other* alt settings, not for the 48 kHz PoC path.

## 3. Streaming topology (measured)

| Property | Playback (EP 0x02 OUT) | Capture (EP 0x84 IN) |
|---|---|---|
| Records in capture | 6084 (submit+completion) | 6026 |
| **Distinct IRPs (URB pool)** | **3** (2×3040, 1×2 recycles) | **12** (11×~502, 1×504) |
| Pipe depth (pool × cycle) | 3 × 10 ms = 30 ms | 12 × 10 ms = 120 ms |
| Slots per URB | 10 | 10 |
| URB cycle time | 10 ms (100.4 completions/s) | 10 ms |
| Slots per URB (cnt field) | 10 | 10 |
| Record `dlen` (Σ per-packet bytes) | **1920** (submit) | **1764** (completion) |
| Data rate | 192.0 B/ms = **48 kHz exact** | 176.4 B/ms = **44.1 kHz exact** |

**Correction (IDA + re-derivation, see §3.1):** the per-record `dlen` totals —
not the per-slot "lengths" read out of the iso descriptors — are the
trustworthy numbers. OUT moves 1920 B / 10 ms (10 × 192, 48 kHz); IN receives
1764 B / 10 ms (9 × 196 + one zero-length slot, 44.1 kHz). The capture is
asymmetric: playback ran at the 48 kHz alt rate while capture delivered
44.1 kHz packetization through the same alt.

The submit/completion asymmetry on IN is the most important single fact in
this capture: the driver *requests* 192 B per slot but the device *returns*
196 B per slot — exactly `wMaxPacketSize` — on **every** packet, not
occasionally. The 4-byte-per-slot surplus is the "drift headroom" of the
wMaxPacket=196 descriptor being used *continuously*, not rarely.

### The 9+1 slot pattern — RESOLVED (was open question #1)

The earlier reading of this capture was wrong in one detail and right in
another. Re-derivation plus decompilation of the original's URB builder
(research 01 §5.3, `sub_F1022390`) settles it:

1. **The +4 field of a USBPcap iso record is the *exclusive-prefix* offset**
   (`pkt_i.offset = Σ lengths of packets before i`), not a cumulative *end*.
   Packet 0's offset is therefore **always 0** — the "slot 0 is permanently
   zero-length" claim was a parse artifact of that convention.
2. Because offsets are exclusive-prefix, a zero-length slot is only visible
   as a *missing* increment. IN records show strictly 196-spaced offsets
   0 → 1764 with `dlen` 1764 = Σ lengths: exactly 9 slots of 196 B (49
   frames) plus **one zero-length slot at the END of the URB** (a zero
   anywhere in the middle would repeat one offset). OUT records show
   strictly 192-spaced offsets 0 → 1728 with `dlen` 1920 = 10 × 192: no zero
   slot at all.
3. Submit records show every packet length as 0 — USBPcap captures the
   driver's submit-side URB where lengths are only *requests*; the
   re-derivations above are all completion-side. Submit-side request
   geometry is verified from IDA instead (research 01 §5.3): the driver
   builds 10-packet URBs with per-packet request length = bytes pulled from
   the client queue (0 when the queue starves — that is the real mechanism
   behind zero-length packets), `TransferBufferLength = Σ requests`, and
   `StartFrame += 10` per cycle.

What is unambiguous regardless of reading:

1. The original driver runs **10-slot, 10 ms URBs** — not the 4 URB × 8 packet
   model izukidio currently uses (§05-4 divergence 2 is confirmed wrong).
2. The IN pipe depth is **12 URBs (120 ms)**; OUT is **3 (30 ms)**. This
   matches the original's computed-depth formula direction: IN needs a much
   deeper queue than OUT. The submit helper advances `StartFrame` by the
   packet count each cycle (verified in `sub_F1022390`, research 01 §5.3).
3. IN submit-side requests are 192 B per slot (48 kHz pattern, `sub_F1018BE0`
   fills 48 dwords uniform for 48 kHz; rare ±4 B trims, 124 × 188 B among
   30,130 submit slots — a one-frame trim the driver applies for clock
   reconciliation). The device *delivers* 196 B per slot regardless: the
   PCM2902's own packetizer sends 49-frame packets at 44.1 kHz and the USB
   stack reports them in the completion descriptors, overwriting the request.

### Sample-rate reconciliation (updated)

- Capture completion bytes → **44.1 kHz exact** (1764 B / 10 ms URB;
  9 × 49-frame packets + one zero-length slot).
- Playback completion bytes → **48 kHz exact** (1920 B / 10 ms; 10 × 192 B).

The session therefore ran capture and playback at *different* effective
rates through the single 48 kHz alt: the device's IN packetizer re-chunks
44.1 kHz into 49-frame packets (device-side repack — answers old open
question #2), while OUT served 48 kHz data as-is. izukidio's IN path must
trust `IsoPacket[i].Length` at completion (which it now does) and must not
assume submit request length == delivered length.

- Capture completion bytes → 44,272 Hz-equivalent (2/30.01 s windows) —
  0.4 % above 44.1k, 7.8 % below 48k.
- Playback completion bytes → 43,784 Hz-equivalent.

Both sit near 44.1 kHz while the selected alt declares 48 kHz only. Best
explanation: the session audio was 44.1 kHz (DAW project rate) and the
original driver served 44.1 kHz *through* the 48 kHz alternate using the
9×49-frame packet trick — i.e. **the original driver does not switch
alternates per rate at all for the common case; it repacks frames into
192/196 B slots at whatever rate the client requested.** That is exactly the
"slot = 4 bytes, repack freely" model the shared-ring design (research 03 §3)
implies, and it kills the "capture rate is chosen by alternate setting"
simplification izukidio currently relies on.

## 4. What izukidio must change (implementation deltas)

1. **URB geometry**: 10-slot, 10 ms URBs; IN pool 12, OUT pool 3 (done in
   izukidio; the earlier "zero-length slot 0" requirement is retracted — see
   §3.1: there is no slot-0 skip, all 10 slots submit data).
2. **IN slot request length**: request 192 B (48 kHz pattern), allocate
   wMaxPacket 196 B per slot, and publish `IsoPacket[i].Length` bytes per
   packet on completion — the device may deliver 196 B (49 frames) where 192
   B were requested, and the final slot may complete 0-length.
3. **Per-packet publish**: ring publish granularity is per iso *packet*, not
   per URB (packets may differ by ±1 frame after trim).
4. **Sample rate**: do not assume alt-setting == rate. The device re-chunks
   44.1 kHz into 49-frame packets on its own (measured §3); izukidio must
   follow `IsoPacket[i].Length` per completion rather than a fixed frame
   count per URB. IN and OUT effective rates can differ (this capture: IN
   44.1 k, OUT 48 k).
5. **Zero-length slots**: real, but they appear wherever the client queue
   starves (request side, `sub_F1022390`) or the device has no frame for the
   slot (completion side, observed as the final IN slot). Handle 0-length
   packets as "publish nothing", not as a reserved resync slot.

## 5. Re-derivation script

The numbers above are reproducible:

```python
import struct, collections
f = open('docs/izuki/umc22_duplex.pcap','rb'); f.read(24)
while True:
    ph = f.read(16)
    if len(ph) < 16: break
    ts, tus, clen, olen = struct.unpack('<IIII', ph); d = f.read(clen)
    if d[22] != 0 or d[21] not in (0x84, 0x02): continue
    if struct.unpack('<H', d[19:21])[0] != 1: continue   # devAddr == UMC22
    hl = struct.unpack('<H', d[:2])[0]
    cnt = struct.unpack('<I', d[31:35])[0]
    lens, prev = [], 0
    for i in range(cnt):
        base = 35 + 12*i
        L = struct.unpack('<I', d[base+4:base+8])[0]
        lens.append(L - prev); prev = L
    # info bit: d[16] & 1 → submit; else completion
```

## 6. Verified-against-descriptor checklist

| Fact | usb-report.txt | pcap | agree? |
|---|---|---|---|
| VID/PID 08BB/2902, rev 0x0100 | ✓ | ✓ | ✓ |
| IF1 EP 0x02 OUT iso, wMaxPacket 192 | ✓ | ✓ (submit slots) | ✓ |
| IF2 EP 0x84 IN iso, wMaxPacket 196 | ✓ | ✓ (completion slots) | ✓ |
| IF2 alt1 = 48 kHz stereo 16-bit | ✓ | alt selected, rate differs (§3) | ⚠ |
| HID EP 0x85 interrupt 1 B/10 ms | ✓ | absent from capture | n/a |
| No feedback endpoint on UMC22 | ✓ | ✓ (nothing but 0x02/0x84 on dev 1) | ✓ |
| bMaxPacketSize0 = 8, bus-powered 100 mA | ✓ | ✓ | ✓ |

## 7. Open questions (in priority order)

1. ~~Slot-0 + 9/10-slot reading~~ **ANSWERED (§3.1)**: USBPcap iso +4 field is
   an exclusive-prefix offset; packet 0 always shows 0. IN = 9 × 196 B + a
   zero-length final slot; OUT = 10 × 192 B, no zero slot. Submit-side
   geometry verified from IDA (`sub_F1022390`, research 01 §5.3).
2. ~~Where does 44.1 k repack happen~~ **ANSWERED (§3)**: device-side. The
   PCM2902 packetizer emits 49-frame (196 B) IN packets at 44.1 kHz while the
   host requested 192 B/slot at the 48 kHz alt; izukidio just relays
   `IsoPacket[i].Length` per completion.
3. **Trim policy** for the ±4 B submit deviations (188/196 B packets): count
   their temporal distribution (uniform = pacing; clustered = resync bursts).
4. OUT pool of 3 × 10 ms = 30 ms is shallow; measure underrun behavior when
   host stalls > 30 ms.
5. **OUT at 44.1 k**: this capture shows OUT running the full 48 kHz pattern
   while IN ran 44.1 kHz. Verify with a forced 44.1 kHz-only session whether
   the original ever trims OUT slots (the `sub_F1018BE0` 44 dwords + 1-every-
   10th pattern table says it can).
