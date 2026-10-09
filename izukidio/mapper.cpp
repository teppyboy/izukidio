// mapper.cpp - kdmapper bootstrap (Secure Boot stays on; blocklist off).
// kdmapper manually maps this image and calls its entry with user-supplied
// integers, so no PnP dispatcher ever calls AddDevice. This module:
//   1. hand-builds a DRIVER_OBJECT (kdmapper does not supply one),
//   2. finds the PCM2902 PDO under \Driver\USBHUB3/USBHUB by hardware ID,
//   3. creates + attaches our FDO via the shared Izk_CreateFDO(enableNow=TRUE),
//      which registers and enables the original interface GUID
//      {090E2CEE-44C0-4263-8837-786AA85A49C6} so the ASIO DLL sees the device.
// PnP/Power IRPs are forwarded untouched (no PnP state machine of our own).
// ponytail: unloading a mapped driver is not supported (kdmapper --free leaks
// the image); bring-up tool only.
#include "common.h"

static PDRIVER_OBJECT IzkMapperDriverObject = nullptr;

static UNICODE_STRING IzkMapperDriverName =
    RTL_CONSTANT_STRING(L"\\Driver\\izukidio");

static BOOLEAN Izk_MatchTargetPdo(PDEVICE_OBJECT dev)
{
    WCHAR hwId[256];
    ULONG len = 0;

    // Only PDOs carry device properties; hub FDOs filter out here.
    if (!NT_SUCCESS(IoGetDeviceProperty(dev, DevicePropertyHardwareId,
                                        sizeof(hwId), hwId, &len))) {
        return FALSE;
    }
    hwId[255] = 0;
    // Exact parent hardware IDs. Composite children end with "&MI_xx" and do
    // not match (original busb2902.sys also bound the whole device).
    return wcscmp(hwId, L"USB\\VID_08BB&PID_2900") == 0 ||
           wcscmp(hwId, L"USB\\VID_08BB&PID_2902") == 0;
}

// Walk a hub driver's device list looking for the PCM2902 PDO.
// Returns a REFERENCED device object, or nullptr.
static PDEVICE_OBJECT Izk_FindTargetPdo(PCWSTR hubName)
{
    UNICODE_STRING name;
    PDRIVER_OBJECT hubDriver = nullptr;
    PDEVICE_OBJECT* list = nullptr;
    ULONG count = 0, size = 64, i;
    PDEVICE_OBJECT found = nullptr;
    NTSTATUS status;

    RtlInitUnicodeString(&name, hubName);
    status = ObReferenceObjectByName(&name, OBJ_CASE_INSENSITIVE, nullptr, 0,
                                     *IoDriverObjectType, KernelMode, nullptr,
                                     (PVOID*)&hubDriver);
    if (!NT_SUCCESS(status)) {
        return nullptr;
    }

    for (;;) {
        list = (PDEVICE_OBJECT*)ExAllocatePool2(POOL_FLAG_NON_PAGED,
                                                size * sizeof(PDEVICE_OBJECT), IZUK_TAG);
        if (list == nullptr) {
            break;
        }
        status = IoEnumerateDeviceObjectList(hubDriver, list,
                                             size * sizeof(PDEVICE_OBJECT), &count);
        if (NT_SUCCESS(status)) {
            break;
        }
        ExFreePoolWithTag(list, IZUK_TAG);
        list = nullptr;
        if (status == STATUS_BUFFER_TOO_SMALL) {
            size = count + 16;
            continue;
        }
        break;
    }

    if (list != nullptr) {
        for (i = 0; i < count; ++i) {
            if (found == nullptr && Izk_MatchTargetPdo(list[i])) {
                found = list[i];   // keep this reference
            } else {
                ObDereferenceObject(list[i]);
            }
        }
        ExFreePoolWithTag(list, IZUK_TAG);
    }
    ObDereferenceObject(hubDriver);
    return found;
}

static PDRIVER_OBJECT Izk_BuildDriverObject(VOID)
{
    PDRIVER_OBJECT dobj =
        (PDRIVER_OBJECT)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(DRIVER_OBJECT), IZUK_TAG);
    if (dobj == nullptr) {
        return nullptr;
    }
    RtlZeroMemory(dobj, sizeof(DRIVER_OBJECT));
    dobj->Type = IO_TYPE_DRIVER;
    dobj->Size = sizeof(DRIVER_OBJECT);
    dobj->DriverName = IzkMapperDriverName;   // static storage, valid forever
    Izk_SetDispatch(dobj);
    return dobj;
}

// Forward-only PnP/Power: in mapper mode we own no PnP state machine.
EXTERN_C NTSTATUS Izk_MapperForwardPnp(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIZUK_DEVICE_EXTENSION dx = (PIZUK_DEVICE_EXTENSION)DeviceObject->DeviceExtension;

    IoSkipCurrentIrpStackLocation(Irp);
    return IoCallDriver(dx->LowerDevice, Irp);
}

EXTERN_C NTSTATUS Izk_MapperBootstrap(PVOID param1, PVOID param2)
{
    PDEVICE_OBJECT pdo;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(param1);
    UNREFERENCED_PARAMETER(param2);

    DbgPrint("IZUKIDIO: mapper bootstrap (kdmapper mode)\n");

    IzkMapperDriverObject = Izk_BuildDriverObject();
    if (IzkMapperDriverObject == nullptr) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    // USB3 stack names it USBHUB3; legacy controllers still use USBHUB.
    pdo = Izk_FindTargetPdo(L"\\Driver\\USBHUB3");
    if (pdo == nullptr) {
        pdo = Izk_FindTargetPdo(L"\\Driver\\USBHUB");
    }
    if (pdo == nullptr) {
        DbgPrint("IZUKIDIO: no USB\\VID_08BB&PID_2900/2902 PDO found - plug in the device first\n");
        return STATUS_NO_SUCH_DEVICE;
    }

    // enableNow = TRUE: register + enable the interface GUID immediately and
    // run Izk_UsbConfigure (no PnP start IRP will ever arrive).
    status = Izk_CreateFDO(IzkMapperDriverObject, pdo, TRUE);
    ObDereferenceObject(pdo);
    if (NT_SUCCESS(status)) {
        // No PnP state machine behind us: forward PnP instead of processing it.
        IzkMapperDriverObject->MajorFunction[IRP_MJ_PNP] = Izk_MapperForwardPnp;
        DbgPrint("IZUKIDIO: mapper attach complete - ASIO DLL should see the device\n");
    }
    return status;
}
