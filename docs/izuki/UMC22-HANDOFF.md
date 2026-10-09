# Handoff — UMC22 Open-Source ASIO Driver (clean-room)

## 1. Thiết bị mục tiêu

- Behringer U-PHORIA UMC22 = USB Audio CODEC TI PCM2902.
- `VID 0x08BB (TI Japan) / PID 0x2902 / REV 0x0100`.
- Strings: Manufacturer `Burr-Brown from TI`, Product `USB Audio CODEC`.
- USB 1.1 Full-Speed 12Mbps, bus-powered 100mA, `bMaxPacketSize0=8`.
- PnP hiện tại (máy dev Win11 Build 28000): Composite `usbccgp`,
  `MI_00` audio (Microsoft `wdma_usb.inf 10.0.28000.2956`, tên `USB Audio CODEC`),
  `MI_03` HID nút volume. Cắm XHCI USB3 Port1, chạy Full-Speed.
- File descriptor gốc: xem `usb-report.txt` (USBTreeView dump đầy đủ).

## 2. Topology USB (từ descriptor)

- IF0 Audio Control: 2 cặp Terminal (USB Streaming <-> Speaker qua Feature Unit mute/volume;
  Microphone -> USB Streaming). Feature Unit ID 3: master Mute, ch1/ch2 Volume.
- IF1 OUT playback (host -> loa): `EP 0x02 OUT`, Isochronous **Adaptive**, `bInterval=1ms`.
  Alt1 dùng chính: stereo, 16-bit, 32/44.1/48k, `wMaxPacket=192`.
  Check: 48k*2ch*2B = 192B/ms = khớp. Các alt khác: mono/8-bit/8–48k (bỏ qua ở bản 1).
  `wLockDelay=512 samples (~10.6ms@48k)` = floor latency đường ra.
  Không có feedback endpoint.
- IF2 IN record (mic -> host): `EP 0x84 IN`, Isochronous **Asynchronous** (trừ vài alt
  rate thấp là Synchronous), `bInterval=1ms`. Alt1: stereo 16-bit 48k duy nhất,
  `wMaxPacket=196` (192 + 4 headroom drift). Các alt khác: mono/44.1/32k/22.05/16/11.025/8k.
- IF3 HID: `EP 0x85 IN` Interrupt, 1 byte / 10ms (nút consumer control).

## 3. Bài toán clock kép (quan trọng nhất)

- OUT Adaptive = DAC chạy theo SOF 1kHz của host.
- IN Asynchronous = ADC chạy theo thạch anh local của chip.
- Hai thạch anh lệch 20–100ppm là bình thường (100ppm@48k = 4.8 sample/s).
- ASIO/DAW chỉ có 1 clock callback -> bắt buộc quy 2 stream về 1 mốc,
  nếu không sẽ tràn/xẹp buffer định kỳ (tách/pop).
- Fix cứng 16-bit/48k stereo KHÔNG hết drift, chỉ đơn giản hóa:
  packet danh nghĩa cố định 192B/ms, tỉ lệ SRC ~1.0 ± 1e-4, bỏ chuyển alt-setting.
- Giải pháp: đo byte IN thực/ms trong 30s -> tỉ lệ Fs_dev/Fs_host ->
  lấy IN làm master, steer OUT (47/49 sample/frame) hoặc SRC mềm 1 chiều.
  Jitter buffer lớn = ít dropout nhưng cộng thẳng vào latency.

## 4. Số liệu latency

- Khung USB FS: 1ms. Lock delay OUT: 512 sample ~10.6ms.
- Round-trip thực tế khó dưới ~8–12ms dù tối ưu.
- Mục tiêu bản 1: ổn định ở 128 samples, thử 64 samples, đo bằng REAPER
  (WASAPI Exclusive làm baseline trước khi có driver mới).

## 5. Driver gốc 2.8.40 làm gì (phân tích kiến trúc, không copy)

- Nguồn: `BEHRINGER_2902_X64_2.8.40/` (Ploytec usb-audio.de, build 30/10/2009, Vista/Win7).
- 3 tầng: `busb2902.sys` (USB function, giành VID/PID, tự build URB isoch,
  import USBD.SYS, thread/bulk/timestamp riêng) + `busbwdm.sys` (WDM/KS cho Windows
  thấy loa/mic, chậm, không liên quan ASIO) + `busbasio.dll/x64` (ASIO COM
  `PGAsioDriver/IASIO`, key `SOFTWARE\ASIO`, chỉ import SETUPAPI+CreateFile+DeviceIoControl,
  không qua waveIn/KS/WASAPI = bypass stack Windows).
- Kết luận cũ: ASIO thật, nhanh hơn ASIO4ALL vài ms, nhưng kẹt ở chip 16-bit/48k
  và thiết kế khóa cổng USB, lỗi thời trên Win10/11.
- Nguyên tắc clean-room: tự định nghĩa IOCTL/GUID/topology, tham chiếu timing từ
  Linux `snd-usb-audio` + descriptor thật, không dùng binary/string/IOCTL của Ploytec.
  ASIO SDK dùng bản dual-license GPLv3 (từ 10/2025) cho phát hành open-source.

## 6. Kiến trúc đề xuất cho codebase mới

```
DAW -> asio_umc22.dll (IASIO COM, GPLv3 SDK, format fix 2ch/16-bit/48k)
      -> DeviceIoControl (IOCTL tự định nghĩa) -> umc22.sys (KMDF)
      -> USBD.SYS -> UMC22 (IF1 alt1 + IF2 alt1)
```
- Bản 1 chỉ hỗ trợ: playback stereo 48k/16-bit + record stereo 48k/16-bit.
- Kernel: chuỗi URB isoch 8–16 frame/URB mỗi hướng, double-buffer kernel
  2×512 + 2×192 sample, drift-compensation như mục 3, expose 1 device interface GUID riêng.
- User-mode: ASIO callback đơn, báo latency thật (input+output), Control Panel tối giản.
- Không đụng `MI_03` HID, không claim toàn bộ composite — chỉ audio function qua usbccgp.

## 7. Toolchain & môi trường

- Máy code: VS2022 + SDK/WDK cùng version (22621 hoặc 26100).
- Máy test riêng/VM Win11 stable (không dùng máy dev Build 28000 Insider để test kernel).
  Máy test: `bcdedit /set testsigning on`, tắt Secure Boot, cài WinDbg Preview,
  USBTreeView (uwe-sieber.de), Wireshark+USBPcap.
- Phát hành Win10/11 cần EV cert + attestation signing (dev thì self-sign đủ).

## 8. Dữ liệu còn thiếu (việc tiếp theo)

1. USBPcap ~30s duplex 48k stereo (phát + thu cùng lúc) -> tính ppm drift thật.
2. Đo baseline REAPER WASAPI Exclusive 128/64 samples (underrun + latency báo).
3. Quyết định master clock (khuyến nghị: IN) + kích thước jitter buffer.
4. Dựng skeleton: `umc22.sys` (KMDF, claim IF1/IF2, set alt, isoch loop) +
   `asio_umc22.dll` (IASIO minimal, IOCTL khung).

## 9. Đo đạc thật trên máy dev (09/10/2026, duplex 30s)

- Cách đo: script C# waveOut/waveIn trực tiếp endpoint UMC22 (2ch/16-bit/48k),
  phát sine 440Hz + thu đồng thời, capture `\\.\USBPcap1` bằng USBPcapCMD.
  File: `umc22_duplex.pcap` (13MB, 12448 gói), `tone48k.wav`, `rec48k.raw`.
- OUT `EP 0x02` adaptive: 3042/3042 URB = 1920B (10 frame × 192B), tuyệt đối đều.
- IN `EP 0x84` async: 2956 URB 1920B + 23 URB 1924B + 22 URB 1916B.
  Dither ngắn hạn ±4B/URB (~±0.4B/frame), trung bình dài hạn khớp danh nghĩa.
- Fit tuyến tính byte-tích-lũy theo timestamp host, span 30s:
  IN 191999.9 B/s (-1ppm), OUT 191999.5 B/s (-3ppm), IN-vs-OUT +2ppm.
- Lưu ý: luồng đi qua WASAPI shared-mode + Audio Engine (có SRC/chỉnh clock),
  nên số dài hạn đẹp một phần nhờ Windows. Driver kernel raw sẽ thấy dither
  thô trực tiếp -> vẫn bắt buộc khối drift-compensation (mục 3).
  Muốn đặc tả crystal cần đo 5+ phút và khi máy nóng/lạnh.
  