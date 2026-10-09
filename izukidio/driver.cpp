// driver.cpp - izukidio DriverEntry / AddDevice / unload.
// Experimental reimplementation of Behringer USB Audio 2.8.40 (Ploytec stack).
// Research: docs/research/01-busb2902-sys.md (original DriverEntry/AddDevice behavior).
#include "common.h"

#pragma code_seg("INIT")

// Fill the dispatch table (shared by INF mode and kdmapper mode).
void Izk_SetDispatch(PDRIVER_OBJECT DriverObject)
{
    DriverObject->MajorFunction[IRP_MJ_CREATE]                  = Izk_DispatchCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE]                   = Izk_DispatchCreateClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL]          = Izk_DispatchDeviceControl;
    DriverObject->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL] = Izk_DispatchInternalDeviceControl;
    DriverObject->MajorFunction[IRP_MJ_SYSTEM_CONTROL]          = Izk_DispatchSystemControl;
    DriverObject->MajorFunction[IRP_MJ_PNP]                     = Izk_DispatchPnp;
    DriverObject->MajorFunction[IRP_MJ_POWER]                   = Izk_DispatchPower;
}

EXTERN_C NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    UNREFERENCED_PARAMETER(RegistryPath);

    // kdmapper invokes the PE entry with user-supplied integers, not a real
    // DRIVER_OBJECT. A real PnP load always passes a valid one — detect and
    // branch (mapper-mode bootstrap lives in mapper.cpp).
    if (DriverObject == nullptr || DriverObject->Type != IO_TYPE_DRIVER ||
        DriverObject->Size != sizeof(DRIVER_OBJECT)) {
        return Izk_MapperBootstrap((PVOID)DriverObject, (PVOID)RegistryPath);
    }

    DbgPrint("IZUKIDIO: DriverEntry (Behringer USB Audio 2.8.40 reimplementation)\n");

    DriverObject->DriverUnload = IzkDriverUnload;
    Izk_SetDispatch(DriverObject);

    return STATUS_SUCCESS;
}

#pragma code_seg()

// FDO creation shared by INF AddDevice and the mapper bootstrap.
// enableNow: mapper mode has no PnP start IRP — register + enable the
// interface GUID and configure USB immediately.
EXTERN_C NTSTATUS Izk_CreateFDO(PDRIVER_OBJECT DriverObject, PDEVICE_OBJECT PhysicalDeviceObject,
                                BOOLEAN enableNow)
{
    NTSTATUS status;
    PDEVICE_OBJECT deviceObject = nullptr;
    PIZUK_DEVICE_EXTENSION dx;

    DbgPrint("IZUKIDIO: creating FDO\n");

    status = IoCreateDevice(DriverObject,
                            sizeof(IZUK_DEVICE_EXTENSION),
                            nullptr,                    // unnamed FDO
                            IZUK_DEVICE_TYPE_FDO,
                            0,                          // characteristics
                            FALSE,                      // not exclusive
                            &deviceObject);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    dx = (PIZUK_DEVICE_EXTENSION)deviceObject->DeviceExtension;
    RtlZeroMemory(dx, sizeof(IZUK_DEVICE_EXTENSION));

    dx->Self        = deviceObject;
    dx->Pdo         = PhysicalDeviceObject;
    dx->LowerDevice = IoAttachDeviceToDeviceStack(deviceObject, PhysicalDeviceObject);
    if (dx->LowerDevice == nullptr) {
        IoDeleteDevice(deviceObject);
        return STATUS_NO_SUCH_DEVICE;
    }

    deviceObject->Flags |= DO_DIRECT_IO;
    deviceObject->Flags |= DO_POWER_PAGABLE;
    deviceObject->AlignmentRequirement = FILE_BYTE_ALIGNMENT;

    IoInitializeRemoveLock(&dx->RemoveLock, IZUK_TAG, 0, 0);
    KeInitializeEvent(&dx->ClientEvent, NotificationEvent, FALSE);
    KeInitializeSpinLock(&dx->In.Lock);
    KeInitializeSpinLock(&dx->Out.Lock);

    RtlInitUnicodeString(&dx->NtNameBuffer, IZUK_NT_DEVNAME);
    RtlInitUnicodeString(&dx->DosSymLink, IZUK_DOS_DEVNAME);

    // Register the same device interface GUID the original ASIO DLL enumerates
    // (research 03 §2). INF mode: enabled at PnP start device; mapper mode:
    // enabled right here (no PnP dispatcher involved).
    status = IoRegisterDeviceInterface(PhysicalDeviceObject,
                                       &IZUK_DEVICE_INTERFACE,
                                       nullptr,
                                       &dx->InterfaceSymbolicLink);
    if (!NT_SUCCESS(status)) {
        IoDetachDevice(dx->LowerDevice);
        IoDeleteDevice(deviceObject);
        return status;
    }

    // Legacy-friendly NT name (mirrors "\Device\BUSB2902" role). DosSymLink
    // points at the literal in the driver image (valid for the driver's
    // lifetime) - the previous pool-copy left MaximumLength unset so
    // RtlCopyUnicodeString copied nothing.
    IoCreateSymbolicLink(&dx->DosSymLink, &dx->NtNameBuffer); // non-fatal: GUID interface is primary

    if (enableNow) {
        IoSetDeviceInterfaceState(&dx->InterfaceSymbolicLink, TRUE);
        status = Izk_UsbConfigure(dx);
        dx->ConfigFailed = !NT_SUCCESS(status);
        if (NT_SUCCESS(status)) {
            DbgPrint("IZUKIDIO: mapper mode configured, interface enabled\n");
        } else {
            DbgPrint("IZUKIDIO: mapper mode USB configure failed %08X\n", status);
        }
    }

    deviceObject->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
}

// INF-mode AddDevice: device creation deferred to the PnP start IRP path.
EXTERN_C NTSTATUS Izk_AddDevice(PDRIVER_OBJECT DriverObject, PDEVICE_OBJECT PhysicalDeviceObject)
{
    return Izk_CreateFDO(DriverObject, PhysicalDeviceObject, FALSE);
}

EXTERN_C void IzkDriverUnload(PDRIVER_OBJECT DriverObject)
{
    UNREFERENCED_PARAMETER(DriverObject);
    DbgPrint("IZUKIDIO: unload\n");
}
