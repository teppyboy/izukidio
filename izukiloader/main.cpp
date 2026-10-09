// izukiloader - auto-loader for izukidio.sys, kdmapper embedded as a library.
//
// Both driver modes live in one .sys (see mapper.cpp). This launcher:
//   1. waits for the PCM2902 device to enumerate,
//   2. clears the in-box usbaudio/usbccgp Service binding (registry) and
//      restarts the device so our mapper attaches to a raw PDO,
//   3. maps izukidio.sys via the vendored kdmapper library (no separate
//      kdmapper.exe; MIT, see kdmapper/LICENSE),
//   4. verifies the driver published \\.\IZUKIDIO.
//
// Usage:
//   izukiloader.exe              load now (skips if already loaded)
//   izukiloader.exe --install    register ONSTART scheduled task (run as admin, once)
//   izukiloader.exe --uninstall  remove the scheduled task
//
// Only izukidio.sys is expected next to the exe.
// ponytail: no service wrapper, no retries beyond device wait; bring-up tool.

#include <windows.h>
#include <stdio.h>
#include <string>
#include <vector>
#include <filesystem>

#include "kdmapper/include/kdmapper.hpp"
#include "kdmapper/include/intel_driver.hpp"
#include "kdmapper/include/utils.hpp"

static std::wstring ExeDir()
{
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring dir(path);
    size_t slash = dir.find_last_of(L"\\/");
    return dir.substr(0, slash + 1);
}

static bool RunCapture(const std::wstring& cmdLine, DWORD& exitCode)
{
    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE readEnd = nullptr, writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, &sa, 0)) return false;
    SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = si.hStdError = writeEnd;
    PROCESS_INFORMATION pi{};

    std::wstring cmd = cmdLine;   // CreateProcess mutates the buffer
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW, nullptr, ExeDir().c_str(), &si, &pi)) {
        CloseHandle(readEnd); CloseHandle(writeEnd);
        return false;
    }
    CloseHandle(writeEnd);

    char buf[1024];
    DWORD n = 0;
    while (ReadFile(readEnd, buf, sizeof(buf) - 1, &n, nullptr) && n > 0) {
        buf[n] = 0;
        printf("%s", buf);
    }
    CloseHandle(readEnd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return true;
}

static bool DriverLoaded()
{
    HANDLE h = CreateFileW(L"\\\\.\\IZUKIDIO", GENERIC_READ | GENERIC_WRITE,
                           0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return GetLastError() != ERROR_FILE_NOT_FOUND;   // access denied still means: device exists
    }
    CloseHandle(h);
    return true;
}

// Clear the in-box function-driver binding (usbccgp / usbaudio) on every
// VID_08BB&PID_2900/2902 instance, then restart the device so a raw PDO is
// presented to the mapped driver.
static int PrepDevice()
{
    int touched = 0;
    const wchar_t* ids[] = {
        L"SYSTEM\\CurrentControlSet\\Enum\\USB\\VID_08BB&PID_2900",
        L"SYSTEM\\CurrentControlSet\\Enum\\USB\\VID_08BB&PID_2902",
    };
    for (const wchar_t* id : ids) {
        HKEY dev;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, id, 0, KEY_READ, &dev) != ERROR_SUCCESS) {
            continue;
        }
        wchar_t inst[128];
        for (DWORD i = 0; RegEnumKeyW(dev, i, inst, 128) == ERROR_SUCCESS; ++i) {
            std::wstring instKey = std::wstring(id) + L"\\" + inst;
            HKEY props;
            if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, instKey.c_str(), 0, KEY_SET_VALUE, &props) == ERROR_SUCCESS) {
                RegSetValueExW(props, L"Service", 0, REG_SZ,
                               (const BYTE*)L"", sizeof(wchar_t));
                RegCloseKey(props);
                touched++;
                std::wstring instanceId = std::wstring(L"USB\\") +
                    (wcsstr(id, L"2900") ? L"VID_08BB&PID_2900\\" : L"VID_08BB&PID_2902\\") + inst;
                DWORD rc = 0;
                wchar_t cmd[512];
                swprintf(cmd, 512, L"pnputil /restart-device \"%s\"", instanceId.c_str());
                RunCapture(cmd, rc);
                printf("[izukiloader] restarted device %ls (pnputil rc=%lu)\n", instanceId.c_str(), rc);
            }
        }
        RegCloseKey(dev);
    }
    return touched;
}

// Wait for the device to appear in the enumerator (boot race).
static bool WaitForDevice(DWORD seconds)
{
    for (DWORD elapsed = 0; elapsed < seconds; elapsed += 2) {
        HKEY dev;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                          L"SYSTEM\\CurrentControlSet\\Enum\\USB\\VID_08BB&PID_2902",
                          0, KEY_READ, &dev) == ERROR_SUCCESS) {
            RegCloseKey(dev);
            return true;
        }
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                          L"SYSTEM\\CurrentControlSet\\Enum\\USB\\VID_08BB&PID_2900",
                          0, KEY_READ, &dev) == ERROR_SUCCESS) {
            RegCloseKey(dev);
            return true;
        }
        Sleep(2000);
    }
    return false;
}

static bool MapOurDriver()
{
    const std::wstring driverPath = ExeDir() + L"izukidio.sys";
    if (!std::filesystem::exists(driverPath)) {
        printf("[izukiloader] izukidio.sys not found next to the exe\n");
        return false;
    }

    intel_driver::Load();
    if (intel_driver::hDevice == INVALID_HANDLE_VALUE) {
        printf("[izukiloader] failed to load iqvw64e.sys loader driver (blocklist still on? HVCI on?)\n");
        return false;
    }

    std::vector<uint8_t> rawImage = { 0 };
    if (!kdmUtils::ReadFileToMemory(driverPath, &rawImage)) {
        printf("[izukiloader] failed to read izukidio.sys\n");
        intel_driver::Unload();
        return false;
    }

    NTSTATUS driverExit = 0;
    // free=false: mapped driver must stay resident (our mapper bootstrap owns
    // the device for the session); passAllocationAddressAsFirstParam=true so
    // DriverEntry's bogus-param detection fires.
    ULONG64 mapped = kdmapper::MapDriver(rawImage.data(), 0, 0, false, true,
                                         kdmapper::AllocationMode::AllocatePool,
                                         true, nullptr, &driverExit);
    if (!intel_driver::Unload()) {
        printf("[izukiloader] warning: loader driver not fully unloaded\n");
    }
    if (mapped == 0) {
        printf("[izukiloader] mapping failed (driver exit 0x%lX)\n", (unsigned long)driverExit);
        return false;
    }
    return true;
}

static int Load()
{
    if (DriverLoaded()) {
        printf("[izukiloader] driver already loaded (\\\\.\\IZUKIDIO exists)\n");
        return 0;
    }
    if (!WaitForDevice(30)) {
        printf("[izukiloader] PCM2902 device not found after 30s - aborting\n");
        return 2;
    }
    PrepDevice();

    if (!MapOurDriver()) {
        return 4;
    }
    Sleep(1000);   // let the bootstrap finish registering the interface
    if (!DriverLoaded()) {
        printf("[izukiloader] mapping succeeded but \\\\.\\IZUKIDIO not found\n");
        return 5;
    }
    printf("[izukiloader] loaded OK - ASIO DLL can open the device\n");
    return 0;
}

static int Install()
{
    wchar_t cmd[1024];
    swprintf(cmd, 1024,
             L"schtasks /Create /TN IzukidioLoader /SC ONSTART /RU SYSTEM /RL HIGHEST /F "
             L"/TR \"\\\"%ls --boot\\\"\"", (ExeDir() + L"izukiloader.exe").c_str());
    DWORD rc = 0;
    if (!RunCapture(cmd, rc) || rc != 0) {
        printf("[izukiloader] schtasks failed (rc=%lu) - run as admin\n", rc);
        return 1;
    }
    printf("[izukiloader] registered ONSTART task 'IzukidioLoader' (SYSTEM)\n");
    return 0;
}

static int Uninstall()
{
    DWORD rc = 0;
    RunCapture(L"schtasks /Delete /TN IzukidioLoader /F", rc);
    printf("[izukiloader] task removed (rc=%lu)\n", rc);
    return 0;
}

int wmain(int argc, wchar_t** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc > 1 && wcscmp(argv[1], L"--install") == 0)  return Install();
    if (argc > 1 && wcscmp(argv[1], L"--uninstall") == 0) return Uninstall();
    return Load();
}
