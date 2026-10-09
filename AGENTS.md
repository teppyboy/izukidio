# izukidio — experimental reimplementation of the Behringer USB Audio 2.8.40 driver

Modern Windows 10/11 kernel-driver reimplementation of the Ploytec/Behringer USB audio
driver stack (`busb2902.sys` + `busbwdm.sys`, TI PCM2900/2902 hardware, USB
`VID_08BB` `PID_2900`/`PID_2902`), goal: work on latest Windows versions.

## READ FIRST (accelerate: prior research exists)

All reverse-engineering results live in `docs/research/`. **Read these before touching
any code** — they contain the extracted device contract, IOCTL table, GUIDs, and the
Win10/11 compatibility analysis, so you do not need to re-reverse the binaries:

| File | Content |
|---|---|
| `docs/research/00-overview.md` | Package architecture, stack diagram, verdict summary |
| `docs/research/01-busb2902-sys.md` | `busb2902.sys` kernel driver: dispatch table, device types, interface GUID, IOCTL table, USB usage, registry keys |
| `docs/research/02-busbwdm-sys.md` | `busbwdm.sys` PortCls WaveCyclic miniport: subdevices, pins, topology, private IRP protocol to busb2902 |
| `docs/research/03-busbasio-dll.md` | `busbasio_x64.dll` user-mode ASIO COM DLL: CLSID, device open path, 21-IOCTL protocol, IASIO vtable, sync model |
| `docs/research/04-win10-11-compat.md` | Why the original fails on Win10/11; USBD/PortCls deprecation status; signing; kdmapper NOT viable; architecture recommendation |
| `docs/research/05-poc-design.md` | Design of the `izukidio` driver in this repo |

Key facts (from research, do not re-derive):

- Device interface GUID the ASIO DLL opens: `{090E2CEE-44C0-4263-8837-786AA85A49C6}`,
  sub-open names `\IO` (data plane), `\CONTROL`, `\FW_UPDATE`.
- ASIO COM DLL: CLSID `{581ABE50-4577-45C8-B41F-F202C45D32AE}`, driver name
  `"BEHRINGER USB AUDIO"`, registers `HKLM\SOFTWARE\ASIO\BEHRINGER USB AUDIO`.
- All user-visible IOCTLs: `CTL_CODE(0x22, fn, FILE_ANY_ACCESS, METHOD_BUFFERED)`
  = `0x220000 | (fn << 2)`, function codes 0x800-range documented in research 01/03.
- Original used direct `USBD.SYS` imports and WaveCyclic — both deprecated; the
  reimplementation uses modern WDM/KMDF USB APIs.
- kdmapper/manual mapping cannot work for a USB client driver (needs PnP FDO +
  select-configuration). Use `bcdedit /set testsigning on` instead.

## Repository layout

```
izukidio/                  <- this directory; the Visual Studio WDK solution
  izukidio.sln
  driver/                  <- kernel driver project (izukidio.vcxproj)
    izukidio.inf           <- driver INF (binds USB\VID_08BB&PID_2900/2902)
    common.h protocol.h pcm2902.h
    driver.cpp device.cpp usb.cpp isoch.cpp ioctl.cpp
  README.md                <- build + deploy instructions (testsigning)
  tools/clangd-stubs/      <- macOS/clangd-only stub headers for static analysis;
                              NOT part of the Windows build (real WDK is used there)
docs/research/             <- reverse-engineering research (read first, see above)
BEHRINGER_2902_X64_2.8.40/ <- original driver package (analysis artifacts, .i64 DB)
docs/research/*.i64        <- saved IDA database for busb2902.sys, reuse it
```

## Conventions

- Driver: WDM, x64, no CRT dependency beyond ntoskrnl; pool tags `'IZUK'`.
- Any new kernel/user contract change must be reflected in `docs/research/05-poc-design.md`.
- The original binaries are reference-only; never load them on modern Windows.
