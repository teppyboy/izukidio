# Static recon: `busbasio_x64.dll` / `busbasio.dll` (BEHRINGER USB ASIO COM DLLs, 2.8.40)

Scope: user-mode ASIO COM DLLs only. Static analysis (radare2 6.2.2, strings, python3+pefile). Driver never loaded/executed. All x64 VAs use ImageBase `0x180000000`; x86 uses `0x10000000`.

Both DLLs are the same codebase (Ploytec "PG*" classes, RTTI: `PGAsioDriver`, `PGWinDevice`, `PGDeviceProxy`, `PGDeviceManager`, `AsioRegistry`, `AsioTimer`, `PCMBuffer`, `PaneASIO`). The x86 build is older: it lacks 6 of the 21 IOCTL codes and one exported COM helper path differs.

---

## 1. COM registration data

Exports (both DLLs):

| Export | x64 VA | Notes |
|---|---|---|
| `DllGetClassObject` | `0x180016010` | standard |
| `DllCanUnloadNow` | `0x1800161d0` | standard |
| `DllRegisterServer` | `0x1800030e0` | standard |
| `DllUnregisterServer` | `0x180003220` | standard |
| `getAsioDriverDefName` | `0x1800173a0` | `lea rax, "BEHRINGER USB AUDIO"; ret` |
| `pt_GetAsioDriverVersion` | `0x180002a90` | `mov eax, 0x2082800; ret` |

**ASIO driver name string:** `"BEHRINGER USB AUDIO"` (single constant at x64 `0x18002df70`, returned by both `getAsioDriverDefName` and the internal getter `0x180017360`; `getDriverName` copies it into a 32-char buffer).

**CLSID (the ASIO COM class):** `{581ABE50-4577-45C8-B41F-F202C45D32AE}`
- Raw 16 bytes at x64 `0x18002c650`, x86 `0x100272dc`.
- Xrefs: `DllRegisterServer` (`0x180003144`), `DllUnregisterServer` (`0x180003249`), `PGAsioDriver` inner object (`0x180003013`), class factory path (`0x18002b082`).

**Device interface GUID (opened via SetupAPI):** `{090E2CEE-44C0-4263-8837-786AA85A49C6}`
- Raw 16 bytes at x64 `0x18002c640`, x86 `0x100272cc`.
- Passed to `SetupDiGetClassDevsA` with flags `0x12` = `DIGCF_PRESENT | DIGCF_DEVICEINTERFACE`, then `SetupDiEnumDeviceInterfaces` / `SetupDiGetDeviceInterfaceDetailA` (A-variants) to obtain the symbolic link path.

**Registration layout written by `DllRegisterServer`:**
- Opens/creates `HKLM\SOFTWARE\ASIO` (string at `0x18002c668`).
- Standard COM self-reg helper `0x1800162d0` invoked with: CLSID bytes, `"busbasio_x64.dll"` (x86: `"busbasio.dll"`), threading model `"Apartment"`, and the driver name `"BEHRINGER USB AUDIO"` — i.e. produces `HKLM\SOFTWARE\ASIO\BEHRINGER USB AUDIO\CLSID = {581ABE50-4577-45C8-B41F-F202C45D32AE}` plus the normal `HKCR\CLSID\{...}\InprocServer32` registration.
- Additional per-driver settings subtree `SOFTWARE\ASIO\<name>\Settings` (string `SOFTWARE\ASIO\` + `\Settings`, used by the `AsioRegistry` class for persisted control-panel settings).
- Failure UI: `"Register Server failed ! (%d)"` MessageBox.

Other registry reads: `RegOpenKeyExA/RegQueryValueExA/RegEnumKeyA/RegDeleteKeyA` used for enumerating installed ASIO drivers under `SOFTWARE\ASIO` (control panel list).

Version-comparison strings (driven by `GetFileVersionInfoA/VerQueryValueA` against the installed `busb2902.sys`/package file version):
- `"There is a newer USB driver installed. Please install the corresponding new ASIO driver."`
- `"There is a old USB driver installed. Please install the corresponding new USB driver."`

## 2. Device open path

Enumeration + open is `PGWinDevice` (device opened in `method.PGWinDevice.virtual_72`, x64 `0x18000b080`):

1. `SetupDiGetClassDevsA(&GUID_{090E2CEE-...}, NULL, NULL, 0x12)` → enumerate device interfaces → `SetupDiGetDeviceInterfaceDetailA` returns the device symbolic link path (e.g. `\\?\usb#vid_08bb&pid_2900#...#{090e2cee-44c0-4263-8837-786aa85a49c6}`); the DLL stores no literal device-path string, the path comes from the interface detail.
2. **Control handle** → stored at `this+0x18220`:
   `CreateFileA(<interface path>, 0xC0000000 (GENERIC_READ|WRITE), 3 (FILE_SHARE_READ|WRITE), NULL, OPEN_EXISTING, 0, NULL)`.
3. **Stream handle** → stored at `this+0x18228`: the same path with the literal suffix `"\IO"` appended (string at x64 `0x18002d390`), i.e. the driver exposes a second symbolic link `<devicepath>\IO` for the data-plane. Same access/share/disposition.
4. Immediately after opening the `\IO` handle it sends **IOCTL 0x2200B0** with the 4-byte `GetCurrentProcessId()` (see §3) — a client-registration call on the streaming handle.
- Single-instance / multi-client protection: named mutexes `"PGDeviceMutex_%d"` (per device instance, `%d` = device index) and `"PGDeviceManagerMutex_%s"`; window classes `"PGDeviceManagerWndClass"`/`"PGDeviceManagerWnd"` for the control panel.
- Error path strings: `"USB hardware not found."` and `"USB hardware not found. (It's disconnected or the USB-Driver is not available.)"`.

## 3. DeviceIoControl call sites (x64) and the kernel-user protocol

All IOCTLs are `FILE_DEVICE_UNKNOWN (0x22)`, `FILE_ANY_ACCESS`, `METHOD_BUFFERED` (low 2 bits = 0), function codes 0..52: `CTL_CODE(0x22, fn, 0, 0)` = `0x220000 | (fn << 2)`. Handle field is `this+0x18220` (control) or `this+0x18228` (`\IO` stream); buffers are stacked (`lpInBuffer=NULL` on all sites found — protocol is OUT (device→host) reads plus a few small IN writes).

| IOCTL | Site (x64) | Handle | Out buf | In buf | Context / interpretation |
|---|---|---|---|---|---|
| `0x220000` | `0x18000ab99`, `0x18000aca5` | ctrl | 0xFF (255) | — | read 255-byte device descriptor/name string (twice, in device-open/introspection `PGWinDevice.virtual_56`) |
| `0x220004` | `0x18000aca5` | ctrl | 0xFF (255) | — | second 255-byte string (second identity string of the same open sequence) |
| `0x22000C` | `0x18000ad0e` | ctrl | **0x18118 (98,584)** | — | bulk state/attribute dump into `this+0x50` — large fixed-size structure read at open time |
| `0x2200A4` | `0x18000ad70` | ctrl | 4 → `this+0x18210` | — | read 4-byte capability/state word |
| `0x22004C` | `0x18000adbd` | ctrl | 4 | — | read 4-byte word (query; result kept in local) |
| `0x220070` | `0x18000af73` | ctrl | 4 → `this+0x18208` | — | read 4-byte word (another capability/state flag) |
| `0x22009C` | `0x18000afc0` | ctrl | 4 | — | read 4-byte word |
| `0x2200B0` | `0x18000b232` | **\IO** | (stack) | **4 = GetCurrentProcessId()** | register client PID with the streaming endpoint; sent once right after opening `\IO` |
| `0x22006C` | `0x18000b5c8` (also `0x18000c3e2`) | \IO | 4 | — | transport state query (called by start/stop helper `0x18000b4d0`) |
| `0x220030` | `0x18000b69b` | \IO | 16 | — | read 16-byte status block (latency/clock info; called from start/stop helper `0x18000b4d0`) |
| `0x2200A0` | `0x18000b75e` | \IO | 4 | 4 (value 1) | write 4-byte command dword (=1) — transport command |
| `0x220014` | `0x18000b80f` | ctrl | 4 | **0x4C (76)** | send 76-byte configuration struct (copied from `this+0x20`) — format/period config commit. Helper `0x18000b790`, called from ASIOInit, `setSampleRate`, control panel, createBuffers paths |
| `0x220018` | `0x18000b89a` (`PGWinDevice.virtual_64`) | ctrl | 4 | 4 | read/write 4-byte parameter (parameter get/set, e.g. clock/period) |
| `0x220024` | `0x18000ba5c` (`fcn.18000b8b0`) | ctrl | 4 | **24 (0x18)** | send 24-byte parameter block (mixed get/set in helper `0x18000b8b0`) |
| `0x220068` | `0x18000bf12` (`PGWinDevice.virtual_40`) | \IO | — | **0xF04 (3844)** | send 3844-byte table (3,844 = 961×4; e.g. per-channel/gain/EQ table upload) |
| `0x220064` | `0x18000bfdd` (`PGWinDevice.virtual_48`) | \IO | — | **0xF04 (3844)** | second 3844-byte table upload (input side; paired with 0x220068 output side) |
| `0x2200B4` | `0x18000c0d0` (`PGWinDevice.virtual_96`) | \IO | 4 | — | read 4-byte counter (e.g. input sample-position register) |
| `0x2200B8` | `0x18000c160` (`PGWinDevice.virtual_104`) | \IO | 4 | — | read 4-byte counter (output side counterpart) |
| `0x2200BC` | `0x18000c205` (`PGWinDevice.virtual_112`) | ctrl | **8** | — | read 64-bit value (8 bytes) — 64-bit sample counter / clock |
| `0x220038` | `0x18000c2d8` (`fcn.18000c270`) | \IO | 8 | — | read 64-bit value (called from the buffer-switch thread loop `0x180002a60..`) |
| `0x2200D0` | `0x18000c480` (`fcn.18000c410`/`0x18000c4c0`) | ctrl | **0x104 (260)** | — | read 260-byte string (ANSI name/path of length 259+NUL) |

Protocol summary: two symbolic links per device. The control link carries configuration (76-byte config struct via `0x220014`, 24-byte param block via `0x220024`, big 0x18118-byte state dump via `0x22000C`, string/identity reads, 64-bit clock via `0x2200BC`). The `\IO` link carries the data-plane: client PID registration (`0x2200B0`), transport start command (`0x2200A0`=1), position counters (`0x2200B4/B8/38`), and 3844-byte DSP table uploads (`0x220064/68`). All calls are synchronous METHOD_BUFFERED, `lpOverlapped=NULL` (no OVERLAPPED anywhere), `lpBytesReturned` checked.

Driver-side cross-reference (`busb2902.sys`, x86): opcode-anchored immediate scan finds `mov ecx/edx, imm32` compares with `0x220000` (×2), and `0x220003/0x220007/0x220013/0x22001F` — i.e. the dispatch compares against `METHOD_NEITHER` (method=3) encodings of function codes 0, 1, 4, 7 (or range checks near them), while the DLL issues the `METHOD_BUFFERED` (method=0) encodings of the same function numbers. The driver dispatch (x86, 450 KB) was not fully lifted here; the low-2-bit difference suggests the DLL-visible codes and some driver-side compare constants differ in method bits (possibly the driver masks `code & ~3` or supports both buffered and neither forms). Flagged for the driver-phase recon.

## 4. ASIO interface (IASIO) implementation

The COM object's vtable is at x64 `0x18002ca28` (24 slots: slots 0–2 = IUnknown thunks delegating to an inner object at `this+8`; slots 3–23 = ASIO methods, 21 of the standard 23 — `getLatency` and `getErrorCode` are not present in this vtable).

| Slot | Method | x64 impl | Behavior observed |
|---|---|---|---|
| 0–2 | QI / AddRef / Release | `0x180004940`, `0x180004ea0`, `0x180004ee0` | thunks to inner object vtable |
| 3 | `init` | `0x180003980` | stores sysHandle at `this+0x78`; sets error string `"USB hardware not found. (…)"` at `this+0x9f8c` on failure |
| 4 | `getDriverName` | `0x1800038a0` | copies `"BEHRINGER USB AUDIO"` (max 32 chars + NUL) |
| 5 | `getDriverVersion` | `0x180003900` | returns `0x2082800` (encoded 2.8.40) |
| 6 | `getErrorMessage` | `0x180003910` | copies `this+0x9f8c` (max 124 chars + NUL) |
| 7 | `start` | `0x180003bc0` | sets running flags (`this+0xa00d/0xa040/0x9f89`), starts worker via `0x180002630` (CreateThread + SetThreadPriority), signals |
| 8 | `stop` | `0x180003cf0` | clears running flag, waits worker exit (WaitForSingleObject on `this+0xa010` event, `0x180002740`), tears down via `0x180007e00` |
| 9 | `getChannels` | `0x180003d40` | `*out1 = this+0x3c`, `*out2 = this+0x44`, minimum-2 clamp |
| 10 | `getLatencies` | `0x180003dd0` | `this+0x9f74` / `this+0x9f78` |
| 11 | `getBufferSize` | `0x180003e10` | min = max = preferred = `this+0x28`, granularity = −1 (power-of-2 only) |
| 12 | `canSampleRate` | `0x180003e70` | double (ASIOSamples) arg |
| 13 | `getSampleRate` | `0x180003f30` | `(double)this->sampleRate` (`this+0x38`) |
| 14 | `setSampleRate` | `0x180003f60` | `OutputDebugStringA("PGAsioDriver::setSampleRate :%d\n")`; delegates to inner vtable+0x60; if changed, copies the 76-byte config struct (from `this+0x20`) and re-commits via IOCTL `0x220014` (helper `0x18000b790`) |
| 15 | `getClockSource` | `0x1800043b0` | — |
| 16 | `setClockSource` | `0x1800044c0` | validates arg==0 else `ASE_NotPresent (0xfffffc18 = -1000)` |
| 17 | `getSamplePosition` | `0x1800044e0` | 64-bit sample counter from `this+0x9d10` (maintained from `\IO` counter reads `0x2200B4/B8/38`), timestamp via `AsioTimer` (`0x18000a3a0`) |
| 18 | `getChannelInfo` | `0x180004560` | — |
| 19 | `createBuffers` | `0x1800049a0` | allocates `PCMBuffer`s (`0x18000e030/e1d0`), pushes channel setup through inner device vtable (+0x48, +0xa0) — drives the 76-byte config commit and \IO transport |
| 20 | `disposeBuffers` | `0x180004f20` | — |
| 21 | `controlPanel` | `0x180004fc0` | spawns the control panel window path (`0x180004ff0`) |
| 22 | `future` | `0x1800055d0` | — |
| 23 | `outputReady` | `0x180005ad0` | sets `this+0xa040` (output-ready flag consumed by the worker) |

Error constants seen: `0xfffffc18` = −1000 (`ASE_NotPresent`), `0xfffffc1d` = −995 (`ASE_InvalidSampleRate` in `setSampleRate` when the inner device rejects).

Data flow to kernel: ASIO callbacks are fed by the worker thread (thread proc `0x1800027c0`): loop = `WaitForSingleObject(kernel event, 10000 ms)` → on timeout `Sleep(50)` → `timeGetTime()` → `0x180002a60/0x180002a80` (which poll the `\IO` handle, incl. `0x220038`/`0x2200BC` 8-byte counters) → invoke the registered buffer-switch callback `0x180003cb0(this, bufferStruct, 0)`.

## 5. Latency / synchronization details

- **Timing source:** `WINMM.timeGetTime()`; `AsioTimer` (`fcn.18000a200`) converts to microseconds (`× 0xf4240 = 1,000,000`), keeps a small ring of deltas (fields `+0x9c60/0x9c70/0x9c74/0x9c80`) for smoothing — used for `getSamplePosition` timestamps.
- **Worker thread:** `CreateThread` in `0x180002630` (thread proc `0x1800027c0`), followed by `SetThreadPriority(thread, this->priority)` where priority comes from the driver object struct (`[this+0x28]`). Wait pattern: kernel-event driven with 10 s timeout, 50 ms fallback sleep.
- **Process priority (control panel, `PaneASIO.virtual_24` `0x18000e030` region / sites `0x18000efed..0x18000f059`):** `SetPriorityClass` selector — `0x40` (IDLE), `0x4000` (BELOW_NORMAL), `0x20` (NORMAL), `0x8000` (ABOVE_NORMAL), plus a 5th branch (next case) — user-selectable, persisted under `SOFTWARE\ASIO\...\Settings`.
- **Buffer sizes:** `getBufferSize` reports min = max = preferred = `this+0x28` with granularity −1 (single fixed power-of-2 period; the host cannot negotiate). The actual period comes from the device config struct (76 bytes at `this+0x20`) committed with IOCTL `0x220014`; the 64-bit clock/counter reads (`0x2200BC`, `0x220038`) supply hardware sample position for latency accounting (`getLatencies` returns `this+0x9f74`/`+0x9f78`, populated from the device state dump `0x22000C`).
- **Synchronization:** host blocks on a kernel event (reset via `CreateEventA` manual-reset pair in `0x18000a680`; `SetEvent/ResetEvent`), i.e. the driver signals buffer completion rather than the DLL polling blindly; the 50 ms sleep + `timeGetTime` path is only the timeout/fallback branch.
- **Concurrency guards:** named mutexes (`PGDeviceMutex_%d`, `PGDeviceManagerMutex_%s`), `CRITICAL_SECTION`s, and on x64 `InterlockedExchangeAdd/Exchange` in `PGWinDevice::prepareOutput` (assert strings preserved in the binary).

## 6. Windows version checks (`GetVersionExA`)

Two call sites:
1. `0x18000a8d0` (called from `PGWinDevice` init `0x18000a7c2` and the \IO helper `0x18000b613`): `OSVERSIONINFOA.dwOSVersionInfoSize = 0x94`, then:
   - `dwPlatformId == 1 (Win9x) && dwMajorVersion == 4 && (build == 0x0A || build == 0x5A)` → returns **false** and zeroes `this+0x18218` (disables a capability — the Win9x/old-VxD-era path flag; on non-NT5 the extended features are off).
   - `dwPlatformId == 2 (NT) && dwMajorVersion == 5 && (minor == 0 || 1 || 2)` → returns **true** (Win2000/XP/2003 explicitly whitelisted).
   Gates the `this+0x18218` feature flag (used with the second `CreateFileA`/`\IO` handle logic) and initializes several `PGWinDevice` state fields.
2. `0x180016240` — MSVC CRT internal use only, no product logic.

No `VerifyVersionInfo`/manifest-based checks. The binary targets NT 5.x explicitly; nothing checks Vista/7 specifically — the package's Vista/7 support comes from the driver, not this DLL.

## Tool notes

- radare2 6.2.2 (`r2 -A`) used for CFG/analysis on both DLLs and IOCTL immediate scan on `busb2902.sys`; pefile (in `/tmp/rev/bin/python`) for imports/exports/section/GUID extraction; `strings` for string corpus. No tool gaps affecting findings. `busb2902.sys` dispatch function-level analysis deferred to the driver-phase task (only constant-level cross-reference done here).

## Reimplementation cheat-sheet (kernel-user protocol, x64 build)

- Expose device interface `{090E2CEE-44C0-4263-8837-786AA85A49C6}` + child symbolic link `<dev>\IO`.
- Control handle: `0x220000/04` identity strings (255 B), `0x22000C` 98,584-byte state dump, `0x220014` 76-byte config commit, `0x220024` 24-byte param block, `0x220018` 4-byte param rw, `0x2200A4/4C/70/9C` capability words, `0x2200BC` 64-bit sample clock, `0x2200D0` 260-byte name.
- `\IO` handle: `0x2200B0` in=PID (client register), `0x2200A0` in=1 (transport start), `0x22006C/30` status (4/16 B), `0x2200B4/B8/38` position counters (4/4/8 B), `0x220064/68` in=3844-byte tables.
- All METHOD_BUFFERED, FILE_ANY_ACCESS, synchronous.
- COM: CLSID `{581ABE50-4577-45C8-B41F-F202C45D32AE}`, `HKLM\SOFTWARE\ASIO\BEHRINGER USB AUDIO`, driver name `"BEHRINGER USB AUDIO"`, version encodes as `0x2082800` = 2.8.40.
