# Izukidio

An experimental reimplementation of Behringer USB Audio 2.8.40 legacy audio driver.

> [!WARNING]
> This project is entirely vibe-coded, expect bugs and slow maintenance.

## Usage

izukidio exposes the **same device interface and IOCTL contract as the original
BEHRINGER 2.8.40 driver**, so the original user-mode ASIO DLL
(`busbasio_x64.dll`) works unchanged against it:

1. Install the driver (below) and plug in a PCM2902 device
   (UCA202, UCG102, Xenyx, … — anything enumerated as
   `USB\VID_08BB&PID_2900` or `PID_2902`).
2. Register the original ASIO COM DLL once (from the BEHRINGER 2.8.40 package):
   `regsvr32 busbasio_x64.dll` (admin).
3. Select the ASIO driver "BEHRINGER USB AUDIO" in your DAW.

Status: **prototype**. ASIO path (control plane + zero-copy isoch ring) is
implemented; the legacy MME/WDM-audio child (busbwdm path) and MIDI child are
not — see `docs/research/05-poc-design.md`.

## Building

On a **Windows 10/11 x64 machine** with Visual Studio 2022 and the WDK
(Windows Driver Kit) installed:

1. Open `izukidio.sln` (repo root).
2. Build `Release | x64` → `build\Release\izukidio.sys`.
3. Create a test certificate / sign, or run under testsigning:

```bat
bcdedit /set testsigning on      :: reboot once
pnputil /add-driver izukidio\izukidio.inf /install
```

The INF binds `USB\VID_08BB&PID_2900` and `USB\VID_08BB&PID_2902`.

macOS/Linux: the driver sources can be syntax-checked with clang using the
git-ignored shims in `.local/wdk-shims/` (see `AGENTS.md`); full builds need
Windows + WDK.

## Development

See [DEVELOPMENT.md](DEVELOPMENT.md)

## License

[Apache-2.0](LICENSE)
