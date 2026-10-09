# busbasio_x64.dll / busbasio.dll — Static Analysis (BEHRINGER ASIO 2.8.40)

**Analysis status: decompiler-grade (IDA Pro 9.4 / Hex-Rays; x64: 784 functions,
base 0x180000000, DB `busbasio_x64.i64`; x86: 755 functions, base 0x10000000, DB
`busbasio.i64`).** Supersedes the earlier static pass; the zero-copy data-plane
contract is now fully resolved (§3). Static analysis only; nothing was executed.

Both DLLs are the same Ploytec codebase (`PGAsioDriver`, `PGWinDevice`,
`PGDeviceProxy`, `AsioTimer`, `AsioRegistry`, `PaneASIO` classes) in 32/64-bit
builds; the user/kernel protocol is identical. Differences noted inline.

**Exports (both):** `DllRegisterServer`, `DllUnregisterServer`, `DllCanUnloadNow`,
`DllGetClassObject` (COM in-proc ASIO driver, CLSID
`{581ABE50-4577-45C8-B41F-F202C45D32AE}`), `getAsioDriverDefName`
(→ `"BEHRINGER USB AUDIO"`), `pt_GetAsioDriverVersion`.

---

## 1. Device open path (x64 `sub_180007450`, verified)

1. `SetupDiGetClassDevsA(&GUID, nullptr, nullptr, DIGCF_PRESENT|DIGCF_DEVICEINTERFACE)`
   over the bus driver's interface GUID `{090E2CEE-44C0-4263-8837-786AA85A49C6}`;
   `SetupDiEnumDeviceInterfaces` + `SetupDiGetDeviceInterfaceDetailA` (cbSize 8).
2. Optionally filters by `DEVINST` (multi-device support via device instance).
3. `CreateFileA(detail->DevicePath, GENERIC_READ|GENERIC_WRITE (0xC0000000),
   FILE_SHARE_READ|WRITE, nullptr, OPEN_EXISTING, 0, nullptr)` → **control handle**
   (PGWinDevice+98848 x64 / +98768 x86).
4. Second `CreateFileA(path + "\\IO", ...)` → **\IO streaming handle** (+98856/+98776).
   Confirms the sub-open contract (`\IO` on top of the interface path).
5. Global named mutex `PGDeviceMutex_%d` (device instance) guards driver access
   (`sub_18000A680` ctor). PGWinDevice object is **0x1A1EC0 = 1,711,328 bytes**.
6. Manual-reset event created (+98864) — this is the handle the **kernel signals**
   (§3). `GetVersionExA` NT5 whitelist (`sub_18000A8D0`): on non-Win2000/XP the
   secondary event (+98840) is disabled; the whitelist logic is version-gated
   legacy, inert on Win10/11.

## 2. IOCTL usage (decompiler-verified, exact sizes)

All `DeviceIoControl` calls are synchronous, no overlapped. Handle column: C =
control handle, IO = `\IO` handle. "OUT" = DLL receives.

| IOCTL | Handle | Dir/size (as called) | Purpose (x64 senders) |
|---|---|---|---|
| `0x220000` | C | OUT 255 B | product name string; **embeds `0xA7` (`§`) delimiter + 24-byte license key** which the DLL extracts and stores as `LicenseKey` |
| `0x220004` | C | OUT 255 B | manufacturer string |
| `0x22000C` | C | OUT **0x18118 (98,584 B)** | full state dump, cached at PGDeviceProxy+80; source of format/terminal tables (research 02 §3 layout) |
| `0x2200A4` | C | OUT 4 B | driver build id (0x0207A800) |
| `0x22004C` | C | OUT 4 B | **must return 3301 (0x0CE5)**; `!= 3301` → "There is a old/newer USB driver installed..." error path |
| `0x220070`, `0x22009C` | C | OUT 4 B | capability/state words |
| `0x220014` | C | 76 B (0x4C) | commit config struct (sample rate/period/format) |
| `0x220018` | C | OUT 4 B | set stream unit param (kernel clamps ≥1) |
| `0x220024` | C | 24 B (0x18) | parameter block |
| `0x220064` / `0x220068` | C | 0xF04 (3,844 B) | routing matrix set/read |
| `0x2200B0` | IO | OUT 4 B = `GetCurrentProcessId()` | register client PID (direction quirk: value passed via output buffer) |
| `0x2200B4` / `0x2200B8` | — | OUT 4 B | shared-handle count / position read |
| `0x2200BC` | C | 8 B | 64-bit clock set |
| `0x220038` | IO | OUT 8 B | latched 64-bit sample clock (read-clears kernel-side) |
| `0x220030` | IO | OUT 16 B | **shared-area registration** (§3) and ready-flag toggle |
| `0x22006C` | IO | none | release stream slot (called before re-register and on stop) |
| `0x2200A0` | IO | 4 B | set file-ready flag (sent after successful 0x220030 registration; x86 sends value 1) |
| `0x2200D0` | C | IN/OUT **0x104 (260 B)** | property dispatcher; DLL uses **cmd 13 (get)** and **cmd 14 (set, arg in [1])** — priority get/set per kernel naming |

**Not used by either DLL:** `0x220008` (vendor/class), `0x22001C` (stop/cycle),
`0x220094` (init-once), `0x220098` (hw info), and the stream data IOCTLs
`0x2200C0/0x2200C4/0x2200C8` — those belong to other clients (control panel /
MME-era paths). An ASIO-compatible reimplementation does **not** need them first.

## 3. Zero-copy data plane (resolved — replaces all earlier guesses)

x64 `0x220030` sequence (senders `0x18000b4d0`, kernel side `sub_F101D7B0` case
`0x220030` → `sub_F101A060`/`sub_F1017660`):

1. The DLL owns a **1,618,488-byte (0x189C38) user-mode shared area**:
   two engine rings of **0xC4E00 = 806,912 bytes** each (render + capture; each
   ring object has a 4-byte position counter at +8, ceiling constant 201,600),
   followed by a 56-byte tail: **event HANDLE at +0x189D8 (1612824)**, **thread
   HANDLE at +1612832**, dword counter at +1612840.
2. The DLL sends `0x220030` with a 16-byte buffer: `[0]` = register (1) /
   unregister (0) flag, `+8` = shared-area user VA (x86 same struct).
3. Kernel: finds the per-file `\IO` slot, `IoAllocateMdl` on the user VA (length
   0x189C38), `MmProbeAndLockPages(UserMode, IoModifyAccess)`,
   `MmMapLockedPagesSpecifyCache` fallback — then:
   - `ObReferenceObjectByHandle` on the event handle → kernel **signals the ASIO
     event directly from the isoch completion path**;
   - `ObReferenceObjectByHandle` on the thread handle → `KeSetPriorityThread(31)`
     (**priority boost for the ASIO feeder thread**).
   (kernel `sub_F10340B0` @ `0xF10340B0`, called from `sub_F1017660`.)
4. DLL follows with `0x2200A0` (file-ready = 1), `Sleep(200)` after register,
   `Sleep(10)` after the toggle (x86 `sub_10009770`).
5. Position/clock exchange during streaming: `0x220038` (latched 8-byte clock
   from the `\IO` slot, read-clear) and the per-ring counters in the mapped
   memory (`sub_180001A60`: reports `201600 - counter`).

**Kernel-side ring accounting** matches: 98584-byte blob header frame size,
0x6044 terminal blocks for format tables (research 02 §3), transaction pools
32 IN / 4 OUT, 512-byte packet stride at high-speed, feedback-callback resync
(research 01 §5).

## 4. Version gate / registry

- `SOFTWARE\ASIO` + `SOFTWARE\ASIO\<name>` registry keys (COM/ASIO registration,
  `AsioRegistry` class); driver name "BEHRINGER USB AUDIO"; marker block
  `#$PTASIONAMESTRT$#…#$PTASIONAMESTOP$#` shared with busb2902.sys
  `getAsioDriverDefName`.
- `0x22004C` returning ≠3301 → hard error with install-mismatch dialogs.
- `GetVersionExA` whitelist (both builds): NT4/98/ME → secondary event disabled;
  NT5 family accepted. On Win10/11 `GetVersionExA` lies (returns 6.2 manifest-
  capped), so the whitelist takes the "not NT5" branch — the DLL still works
  (secondary event optional).

## 5. Implications for izukidio (contract changes vs. earlier plan)

1. **`0x220030` must implement the shared-area registration** (MDL lock of the
   user VA, event-handle reference + `KeSetEvent` from isoch completion, thread
   priority boost) — this is the real ASIO data path; the stream-copy IOCTLs
   `0x2200C0/C4` are secondary.
2. `0x22004C` = 3301 and `0x2200A4` = 0x0207A800 are hard gates in the DLL.
3. `0x220000` response may embed `0xA7` + 24-byte license key — DLL tolerates
   its absence (strchr NULL).
4. Property commands in use: 13/14 (priority get/set) — not 6/8 as earlier
   assumed from the kernel side alone; implement the 0x2200D0 command set from
   research 01 §3 (0–22) with 13/14 verified.
5. `0x2200B0` PID registration arrives in the *output* buffer (original quirk);
   accept both directions.
6. `0x22000C` consumers parse terminal blocks — zeroed dump is not enough for
   real ASIO; fill header (+8 frame size) and format tables on bring-up.
