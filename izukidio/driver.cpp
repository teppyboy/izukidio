// driver.cpp - izukidio DriverEntry / AddDevice / unload.
// Experimental reimplementation of Behringer USB Audio 2.8.40 (Ploytec stack).
// Research: docs/research/01-busb2902-sys.md (original DriverEntry/AddDevice behavior).
#include <initguid.h>   // DEFINE_GUID instantiates IZUK_DEVICE_INTERFACE in this TU only
#include "common.h"

#pragma code_seg("INIT")

EXTERN_C NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    UNREFERENCED_PARAMETER(RegistryPath);

    DbgPrint("IZUKIDIO: DriverEntry (Behringer USB Audio 2.8.40 reimplementation)\n");

    DriverObject->DriverUnload = IzkDriverUnload;
    DriverObject->MajorFunction[IRP_MJ_CREATE]                  = Izk_DispatchCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE]                   = Izk_DispatchCreateClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL]          = Izk_DispatchDeviceControl;
    DriverObject->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL] = Izk_DispatchInternalDeviceControl;
    DriverObject->MajorFunction[IRP_MJ_SYSTEM_CONTROL]          = Izk_DispatchSystemControl;
    DriverObject->MajorFunction[IRP_MJ_PNP]                     = Izk_DispatchPnp;
    DriverObject->MajorFunction[IRP_MJ_POWER]                   = Izk_DispatchPower;

    return STATUS_SUCCESS;
}

#pragma code_seg()

// AddDevice: mirror of the original PGKernelDevice::DISPAddDevice
// (research 01 §2): named-less FDO, DeviceType 0x8002, DO_DIRECT_IO,
// attach to USB PDO stack, register + enable device interface GUID.
EXTERN_C NTSTATUS Izk_AddDevice(PDRIVER_OBJECT DriverObject, PDEVICE_OBJECT PhysicalDeviceObject)
{
    NTSTATUS status;
    PDEVICE_OBJECT deviceObject = nullptr;
    PIZUK_DEVICE_EXTENSION dx;

    DbgPrint("IZUKIDIO: AddDevice\n");

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
    // (research 03 §2). Enabled at PnP start device.
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

    deviceObject->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
}

EXTERN_C void IzkDriverUnload(PDRIVER_OBJECT DriverObject)
{
    UNREFERENCED_PARAMETER(DriverObject);
    DbgPrint("IZUKIDIO: unload\n");
}
