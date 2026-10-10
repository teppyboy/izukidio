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

### Requirements

- Windows 10/11 x64 (build machine)
- **Visual Studio 2022 Build Tools** with:
  - MSVC v143 x64/x86 toolset (`Microsoft.VisualStudio.Component.VC.Tools.x86.x64`)
  - Spectre-mitigated v143 libraries (`Microsoft.VisualStudio.Component.VC.14.44.17.14.x86.x64.Spectre`)
  - **Windows Driver Kit component** (`Component.Microsoft.Windows.DriverKit.BuildTools`)
- **WDK 10.0.26100.6584** (+ its matching SDK) — e.g. `winget install Microsoft.WindowsWDK.10.0.26100`
- .NET Framework 4.5 targeting (ships with VS)

> [!NOTE]
> **Visual Studio 2026 (v18) IDE cannot build this solution**: WDK 26100 ships
> only `Microsoft.DriverKit.Build.Tasks.17.0.dll`, while MSBuild 18 requires the
> `18.0` one, so the build fails with `ValidateNTTargetVersion ... could not be
> loaded`. Native VS2026 driver builds need WDK 28000.2526. Use `rebuild.bat`
> (below) instead — it always builds with the VS2022 Build Tools **amd64**
> MSBuild (the 32-bit one lacks `x86\InfVerif.dll` and fails the INF step).

### Build

From the repo root:

```bat
rebuild.bat            :: Debug (default)
rebuild.bat Release
```

Output: `build\Debug\izukidio.sys` / `build\Release\izukidio.sys` (+ INF, PDB).
The .cat is intentionally not generated (`EnableInf2cat=false`); test signing is
done out of band.

### Install / test-signing

1. Create a test certificate / sign, or run under testsigning:

```bat
bcdedit /set testsigning on      :: reboot once
pnputil /add-driver izukidio\izukidio.inf /install
```

The INF binds `USB\VID_08BB&PID_2900` and `USB\VID_08BB&PID_2902`.

### izukiloader (unsigned-load path, no testsigning)

When Secure Boot keeps testsigning off, the loader maps the same
`izukidio.sys` through the kdmapper library (MIT, TheCruZ, pinned as a
git submodule at `izukiloader/kdmapper` — run
`git submodule update --init` after cloning) — no separate `kdmapper.exe`:

```bat
:: after building the solution, from build\Release:
izukiloader.exe              :: wait for device, unbind in-box driver, map, verify
izukiloader.exe --install    :: ONSTART scheduled task (SYSTEM) for auto-load at boot
izukiloader.exe --uninstall  :: remove the task
```

Requires `izukidio.sys` next to the exe and the vulnerable-driver
blocklist disabled for the Intel driver it uses.

macOS/Linux: the driver sources can be syntax-checked with clang using the
git-ignored shims in `.local/wdk-shims/` (see `AGENTS.md`); full builds need
Windows + WDK. Do **not** mix MSYS2 (clang) with the MSVC build — keep the two
toolchains separate. This repo's clangd config (`.clangd`) only points the
editor indexer at the installed WDK headers; builds are pure MSVC/MSBuild.

## Development

See [DEVELOPMENT.md](DEVELOPMENT.md)

## License

[Apache-2.0](LICENSE)
