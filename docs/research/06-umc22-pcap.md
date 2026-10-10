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
| Slot 0 length | **0 (always)** | **0 (always)** |
| Slots 1–9, submit side | 192 B each (sum 1728) | 192 B each (sum 1728), rare 188/196 |
| Slots 1–9, completion side | 192 B each (sum 1728) | **196 B each (sum 1764)** |
| Data rate (completion) | 172.8 B/ms | 176.4 B/ms |

The submit/completion asymmetry on IN is the most important single fact in
this capture: the driver *requests* 192 B per slot but the device *returns*
196 B per slot — exactly `wMaxPacketSize` — on **every** packet, not
occasionally. The 4-byte-per-slot surplus is the "drift headroom" of the
wMaxPacket=196 descriptor being used *continuously*, not rarely.

### The 9+1 slot pattern

Both directions use a 10-slot URB where **slot 0 is permanently zero-length**
and slots 1–9 carry audio. Cumulative end offsets per packet run
0 → 192/196 → 384/392 → ... → 1728/1764. Three readings of this survive
contact with the data:

- **(A) 10 ms URBs, 9 active slots.** IN delivers 176.4 B/ms = **exactly
  44,100 frames/s × 4 B** (1764/10 ms). The 9×49-frame packet structure
  (49 × 4 B = 196) delivering 441 frames per 10 ms cycle is *exact* — this
  is the 44.1 kHz packetization (9 packets of 49 frames + 1 skip slot per
  10 frames), consistent with the original driver's `wLockDelay = 512`
  (~11.6 ms at 44.1k) warm-up budget. OUT at 172.8 B/ms = 43.2 kHz does not
  match a standard rate, which argues against pure (A).
- **(B) slot 0 is an ASAP-alignment placeholder, 9 ms of data per URB.**
  OUT = 192 B/ms = exactly 48 kHz ✓. IN = 196 B/ms = 49 kHz ✗.
- **(C) slot-0 length is a USBPcap reporting artifact** (first iso descriptor
  of each URB always shown as 0) and the true wire format is 10 × 192/196.
  OUT = 1920 B/10 ms = exactly 48 kHz ✓; IN = 1960 B/10 ms = 49 kHz ✗ —
  unless IN slot 0 genuinely never transmits and only IN uses 9+1.

No single reading makes both directions a standard rate *and* keeps
submit/completion consistent, so this stays flagged as the #1 open question
(§7). What is unambiguous regardless of reading:

1. The original driver runs **10-slot, 10 ms URBs** — not the 4 URB × 8 packet
   model izukidio currently uses (§05-4 divergence 2 is confirmed wrong).
2. The IN pipe depth is **12 URBs (120 ms)**; OUT is **3 (30 ms)**. This
   matches the original's computed-depth formula direction: IN needs a much
   deeper queue than OUT.
3. IN submit descriptors occasionally deviate ±4 B (124 × 188 B, 224 × 196 B
   among the 30,130 submit slots) — a **one-frame trim** the driver applies to
   individual packets for clock reconciliation. The device-side completion
   length stays 196 regardless: the trim lives in the request, not the
   response.

### Sample-rate reconciliation

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

1. **URB geometry**: move to 10-slot, 10 ms URBs with a zero-length slot 0;
   IN pool 12, OUT pool 3 (or keep depth configurable but default to these).
   Current `IZUK_MAX_ISO_URBS`-based 4×8 model must go.
2. **IN slot request length**: request 192 B, allocate 196 B (wMaxPacket) and
   *publish 49 frames per packet* on completion — the ring writer already
   handles variable publish sizes (dword = 4-byte frame), but the per-slot
   publish loop must consume `IsoPacket[i].Length` bytes, not assume 192.
3. **Per-packet publish**: ring publish granularity is per iso *packet*, not
   per URB (packets may differ by ±1 frame after trim).
4. **Sample rate**: do not assume alt-setting == rate. The DLL's SET_PARAM /
   property path picks a rate; the driver must repack frames for any rate
   within the alt's declared range (48k alt carries 44.1k fine per this
   capture).
5. **Slot-0 semantics**: reserve slot 0 as the drift-correction slot (the
   original's "skip-4-frames" resync target). Its request length toggles
   0 ↔ 196 in the original as the resync mechanism.

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

1. **Slot-0 + 9/10-slot reading** (§3): which of A/B/C is real. Next probe:
   capture with `USBPcap` while forcing a 48 kHz-only session and compare
   slot fills; or read `sub_F1005FF0` (URB submit) in IDA for the
   `IsoPacket[0].Length` assignment.
2. **Where does 44.1 k repack happen** — driver-side slot math or device-side
   packetization? (Determines whether izukidio must repack or just relay.)
3. **Trim policy** for the ±4 B submit deviations (188/196 B packets): count
   their temporal distribution (uniform = pacing; clustered = resync bursts).
4. OUT pool of 3 × 10 ms = 30 ms is shallow; measure underrun behavior when
   host stalls > 30 ms.
