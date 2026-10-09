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

## 3. Zero-copy data plane (fully resolved — byte-exact)

### 3.1 Shared-area layout (verified from both sides)

The ASIO DLL owns one **user-mode shared area of 0x189C38 = 1,612,856 bytes**:

```text
+0                engine A object (0xC4E0C = 806,412 B)
+806,412          engine B object (0xC4E0C)
+1,612,824        32-byte tail:
  +1,612,824 (+0)   event HANDLE  (DLL-created, kernel KeSetEvent target)
  +1,612,832 (+8)   thread HANDLE (DLL feeder thread, kernel boosts to 31)
  +1,612,840 (+16)  dword (context)
  +1,612,852 (+28)  dword overflow/latency counter (kernel-managed, Interlocked)
```

Each engine object = 12-byte header + 0xC4E00 (806,400 B) sample ring:

```c
struct PG_ENGINE {            // 0xC4E0C bytes, dword-indexed
    volatile ULONG writeIndex;   // +0   ring write cursor in DWORDS, wraps at 201600 (0x31380)
    volatile ULONG readIndex;    // +4   read cursor (consumer side)
    volatile ULONG totalWritten; // +8   running total of DWORDS ever written (position counter,
                                 //        NEVER reset; DLL: frames = totalWritten / bytesPerFrame)
    ULONG ring[201600];          // +12  806,400 B of 32-bit samples
};
```

In PGWinDevice the two engines are embedded and adjacent:
engine A at **+98888 (0x18248)** (ends exactly at engine B), engine B at
**+905300 (0xDD054)**; handles/state live after them at +1711712..+1711744.
`sub_180001A90` zeroes dword[0..2] + `memset(+12, 0, 0xC4E00)`.

### 3.2 Kernel ring writer `sub_F1007B30` (busb2902.sys, verbatim algorithm)

```c
write(slot->sharedVa + 806412, data, lenDwords):   // engine B = kernel→user (capture)
    n    = min(len, 201600 - totalWritten)         // clamp by accumulator
    free = 201600 - writeIndex
    if free < n:                                   // split at wrap
        copy free dwords   to &ring[writeIndex]
        copy (n-free) dwords to &ring[0]; writeIndex = n-free
    else:
        copy n dwords to &ring[writeIndex]; writeIndex += n
        if writeIndex >= 201600: writeIndex = 0
    InterlockedAdd(&totalWritten, n)
    return n                                       // caller compares vs len for overflow
```

Overflow accounting (`sub_F1017340`): bytes dropped = `frameSize * (len - written)`,
divided by a per-slot divisor; subtracted from `tail+28` via
`_InterlockedExchangeAdd`, floored at 0 by `_InterlockedExchange`.

### 3.3 Start/stop handshake `sub_18000B4D0` (x64, critical section at +1711744)

1. Requires the `\IO` handle (+98856) ≠ -1.
2. If direction unchanged (`startFlag == byte +98872`) → no-op success.
3. On stop with force flag: `0x22006C` (release stream slot).
4. Secondary/companion handle stored at +98840.
5. On start: re-init engines (`sub_18000A8D0`), stash engine-A VA via
   `sub_18000D720(desc+8, this+98888)`.
6. `DeviceIoControl(\IO, 0x220030, NULL, 0, desc16, 0x10, ...)`: 16-byte
   descriptor = `{ u8 startFlag @0; pad; u64 engineA user VA @+8 }`
   (x86 identical). `Sleep(10)` after the call.
7. On success: latch flag to +98872, `Sleep(200)`, then
   `DeviceIoControl(\IO, 0x2200A0, NULL, 0, &one, 4, ...)` (file-ready = 1).
8. On stop: clear +98872, `SetEvent` via `sub_18000BBB0` (event +18230).

### 3.4 DLL-side position readback (decompiler-verified)

- `sub_180001A60(engine)`: returns `engine->totalWritten`, outputs
  `201600 - totalWritten` ("frames remaining").
- `sub_18000B360(state)` (per wait-loop wakeup, `sub_18000BBE0`, 0x12C ms
  timeout on event +98864):
  - `state[0]` = consume `*(DWORD*)(dev+1711732)` (read-clear)
  - `state[5]` = `*(dev+1711736)`; `state[6]` = `*(dev+1711728)` (stream state,
    wait loop treats 2 and 5 as terminal)
  - `state[1..2]` = engine B position / bytesPerFrame `*(fmt+28)`
  - `state[3..4]` = engine A position / bytesPerFrame `*(fmt+36)`
  - i.e. **engine B (sharedVa+806412) is the capture (IN) ring, engine A is the
    playback (OUT) ring** — the kernel writes incoming audio to engine B and
    drains outgoing audio from engine A.
- The wait loop is reference-counted on `dev+18240` (`lock add ±1`).

### 3.5 Open/init sequence `sub_18000AAA0` (x64, per control handle)

- `0x220000` → name buffer 0xFF: `"<name>\xA7<24-byte LicenseKey>"`; key goes to
  a `LicenseKey` property, name cached at `this+16`.
- `0x220004` → second name 0xFF, cached at `this+48`.
- `0x22000C` → **0x18118-byte state dump cached at `this+80`** (whole-blob copy,
  layout in research 02 §3).
- `*(DWORD*)(this+98832) = 0`, then `0x2200A4` → 4 bytes into `this+98832`.
- `0x22004C` → 4-byte version; **must equal 3301** else hard error dialog
  ("old/newer USB driver installed").
- Clear `*(DWORD*)(this+98876)`, `*(BYTE*)(this+98828) = 0`;
  `sub_180001730(w, this+80, this+82)` + `sub_180008790` (device-id walk via
  dynamically loaded `cfgmgr32!CM_Get_Device_IDA/CM_Get_Parent`).
- If +98828 set: `0x220070` → 4 bytes into `this+98824`.
- `0x22009C` → 4-byte bool into `this+98829`.

### 3.6 Earlier notes superseded

Earlier drafts said "two 0xC4E00 rings + 56-byte tail" and "1,618,488 bytes":
ring **data** is 0xC4E00, the engine **object** is 0xC4E0C, the tail is **32**
bytes, and the area is 0x189C38 = 1,612,856 B. The registered VA is engine A's
object start; the kernel adds 0xC4E0C for engine B and 2×0xC4E0C = 0x189C18 for
the tail.

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
