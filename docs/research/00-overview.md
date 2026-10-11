# BEHRINGER 2902 Driver Package 2.8.40 — Reverse Engineering Overview

Target: `BEHRINGER_2902_X64_2.8.40/` — Ploytec GmbH ("usb-audio.de") driver stack for
Behringer USB audio devices built on the Texas Instruments PCM2900/2902 USB codec
(USB `VID_08BB`, `PID_2900`/`PID_2902`, USB Audio Class 1.0).
Hardware family: UCA202, UCA222, UCG102, UFO202, UMA25S, iAXE, Xenyx-bundled devices.

Goal of this research: understand the original stack well enough to reimplement it as a
kernel driver that works on Windows 10/11 (and future versions), keeping the original
user-mode ASIO DLL (`busbasio_x64.dll`) usable.

## 1. Package architecture

The original product is a **three-part stack**, not a single driver:

```
+--------------------------- user mode ---------------------------+
|  ASIO host app (DAW)                                           |
|      | IASIO (COM)                                             |
|  busbasio_x64.dll  CLSID {581ABE50-4577-45C8-B41F-F202C45D32AE}|
|      | CreateFile / DeviceIoControl (synchronous, buffered)    |
+---------------------------------------------------------------+
                 |  device interface {090E2CEE-44C0-4263-8837-786AA85A49C6}
                 |  open paths: "\IO" (data plane), "\CONTROL", "\FW_UPDATE"
+--------------------------- kernel mode -------------------------+
|  busb2902.sys   (450 KB)  USB function driver for VID_08BB/2900|2902
|    - C++/STL inside kernel, links DirectShow base classes      |
|    - imports USBD.SYS directly (legacy WDM USB)                |
|    - exports getAsioDriverDefName() -> "BEHRINGER USB AUDIO"   |
|    - is also a BUS driver: enumerates child PDOs               |
|        DeviceType 0x8003 -> WDM audio adapter (MEDIA\BUSB_AUDIOADAPTER)
|        DeviceType 0x8004 -> MIDI child
|      children created/invalidated via IOCTL 0x2200E0 -> IoInvalidateDeviceRelations
|  busbwdm.sys    (48 KB)   PortCls miniport for the WDM child   |
|    - PcAddAdapterDevice / PcNewPort / PcNewMiniport (WaveCyclic)|
|    - exposes KS Wave/Topology interfaces (MME/WASAPI path)     |
+----------------------------------------------------------------+
                 | USB isochronous endpoints (PCM2902)
```

Documents in this folder:

| File | Content |
|---|---|
| `01-busb2902-sys.md` | Kernel USB function/bus driver analysis + IOCTL contract |
| `02-busbwdm-sys.md` | PortCls miniport (busbwdm.sys) analysis |
| `03-busbasio-dll.md` | User-mode ASIO COM DLL contract |
| `04-win10-11-compat.md` | Why it fails on Win10/11 and reimplementation strategy |
| `05-poc-design.md` | PoC driver design (what the VS project implements) |

## 2. Why it does not work on Windows 10/11 (short version)

1. **Old INF / install model.** `Class=USB`, Win9x remnants (`DevLoader=*ntkern`,
   `PreCopySection`), DriverVer 2009; the catalog cannot validate on modern systems and
   the INF layout is not accepted by the modern driver-store flow.
2. **Deprecated WDM USB surface.** Direct import of `USBD.SYS`
   (`USBD_GetUSBDIVersion`, `USBD_CreateConfigurationRequestEx`,
   `USBD_ParseConfigurationDescriptorEx`). Still shipped and mostly still functional on
   Win10/11, but deprecated; combined with the legacy `DevLoader=*ntkern` install the
   stack simply was never updated for the xHCI-era stack.
3. **PortCls WaveCyclic miniport** in `busbwdm.sys` — deprecated since Vista
   (WaveRT is the successor); still shipped, so this part is the *least* broken, but a
   reimplementation should target WaveRT or drop the WDM side entirely.
4. **User-mode gating.** `busbasio_x64.dll` calls `GetVersionExA` and whitelists only
   NT 5.0/5.1/5.2 — every capability flag is zeroed on anything newer.
5. **Signing.** x64 Windows 10/11 refuses any non-signed kernel driver without
   `testsigning`; the old catalog/signature does not satisfy modern requirements.

Full analysis: `04-win10-11-compat.md`.

## 3. Reimplementation verdict (short version)

- **kdmapper (manual mapping) is NOT viable** for this driver: a USB client driver needs
  a PnP FDO, select-configuration, and an INF binding; a manually mapped image has none
  of those, and modern HVCI/blocklist makes mapping fragile anyway. Use
  `bcdedit /set testsigning on` + a test-signed driver instead (the user's stated
  testsigning path is correct; kdmapper is not).
- **Recommended PoC:** a clean WDM/KMDF USB function driver for `VID_08BB&PID_2900|2902`
  that (a) selects the USB audio configuration, (b) runs isochronous IN/OUT streams,
  (c) exposes the same device interface GUID and the same IOCTL protocol so the original
  ASIO DLL works unchanged, and (d) optionally registers a child device for PortCls.
- **Fallback with zero kernel code:** the in-box `usbaudio.sys` (UAC1) already drives the
  PCM2902 on Win10/11; an ASIO4ALL-style KS/WaveRT wrapper DLL can provide ASIO without
  any custom driver. Less faithful (no original IOCTL protocol) but instantly working.

## 4. Method / tooling

- IDA Pro 9.4 (idalib, ida-domain API) — `busb2902.sys` decompilation (main driver).
- Ghidra (headless + MCP) — `busbwdm.sys`.
- radare2 + pefile — `busbasio_x64.dll` / `busbasio.dll` and cross-checks.
- USBPcap capture of the real stack driving a UMC22 — see `06-umc22-pcap.md`.
- Web research against Microsoft Learn (USBD deprecation, PortCls status, signing,
  isochronous transfer APIs) — see `04-win10-11-compat.md` for source URLs.

Everything here is from **static analysis only**; the driver was never executed.

## 5. Consolidated status

Read `07-findings.md` first for the current state: all major open questions
about the URB geometry, slot semantics, and rate repacking are resolved
against both the decompilation and a live USBPcap capture; the PoC status
and remaining divergences are tracked there and in `05-poc-design.md`.
