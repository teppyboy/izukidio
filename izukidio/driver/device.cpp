// device.cpp - PnP/power/WMI dispatch and create/close handling.
// Mirrors original dispatch selection (research 01 §2): CREATE validates the
// FileObject->FileName sub-open ("\IO", "\CONTROL", "\FW_UPDATE").
#include "common.h"

static const PCWSTR IzkOpenNames[] = {
    L"\\IO", L"\\CONTROL", L"\\FW_UPDATE"
};
#define IZUK_OPEN_IO      0
#define IZUK_OPEN_CONTROL 1
#define IZUK_OPEN_FWUPD   2

typedef struct _IZUK_FILE_CONTEXT {
    LONG    OpenKind;           // IZUK_OPEN_*
    LONG    Slot;               // stream slot index (original: per-file +84)
    BOOLEAN FileReady;          // original m_bIsFileReady (IOCTL 0x2200A0)
    ULONG   ClientPid;          // registered via IOCTL 0x2200B0
    ULONG   Position;           // per-handle position latch
} IZUK_FILE_CONTEXT, *PIZUK_FILE_CONTEXT;

// Start-device lower-stack wait: completion routine signals the caller event
// and holds the IRP (STATUS_MORE_PROCESSING_REQUIRED) until we complete it.
static NTSTATUS IzkPnpStartCompletion(PDEVICE_OBJECT DeviceObject, PIRP Irp, PVOID Context)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);
    KeSetEvent((PKEVENT)Context, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS IzkCreate(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    NTSTATUS status = STATUS_SUCCESS;
    PIZUK_DEVICE_EXTENSION dx;
    PIO_STACK_LOCATION iosl;
    PIZUK_FILE_CONTEXT ctx = nullptr;
    int kind = -1;
    unsigned i;

    dx = (PIZUK_DEVICE_EXTENSION)DeviceObject->DeviceExtension;
    iosl = IoGetCurrentIrpStackLocation(Irp);

    if (dx->ConfigFailed) {
        status = STATUS_DEVICE_NOT_READY;
        goto done;
    }

    if (iosl->FileObject != nullptr && iosl->FileObject->FileName.Length > 0) {
        for (i = 0; i < RTL_NUMBER_OF(IzkOpenNames); ++i) {
            UNICODE_STRING name;
            RtlInitUnicodeString(&name, IzkOpenNames[i]);
            if (RtlEqualUnicodeString(&iosl->FileObject->FileName, &name, FALSE)) {
                kind = (int)i;
                break;
            }
        }
        if (kind < 0) {
            // Original rejects unknown sub-open names with STATUS_INVALID_PARAMETER.
            status = STATUS_INVALID_PARAMETER;
            goto done;
        }
    } else {
        kind = IZUK_OPEN_CONTROL;   // bare open of the interface = control plane
    }

    ctx = (PIZUK_FILE_CONTEXT)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(IZUK_FILE_CONTEXT), IZUK_TAG);
    if (ctx == nullptr) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto done;
    }
    RtlZeroMemory(ctx, sizeof(IZUK_FILE_CONTEXT));
    ctx->OpenKind = kind;
    iosl->FileObject->FsContext = ctx;

done:
    Irp->IoStatus.Status = status;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}

static NTSTATUS IzkClose(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIZUK_DEVICE_EXTENSION dx;
    PIO_STACK_LOCATION iosl;
    PIZUK_FILE_CONTEXT ctx;

    dx = (PIZUK_DEVICE_EXTENSION)DeviceObject->DeviceExtension;
    iosl = IoGetCurrentIrpStackLocation(Irp);
    ctx = (PIZUK_FILE_CONTEXT)iosl->FileObject->FsContext;
    if (ctx != nullptr) {
        if (ctx->ClientPid != 0) {
            InterlockedDecrement(&dx->StreamingClients);
        }
        ExFreePoolWithTag(ctx, IZUK_TAG);
        iosl->FileObject->FsContext = nullptr;
    }
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

EXTERN_C NTSTATUS Izk_DispatchCreateClose(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION iosl = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS status;

    if (DeviceObject == nullptr || DeviceObject->DeviceType != IZUK_DEVICE_TYPE_FDO) {
        Irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    status = IoAcquireRemoveLock(&((PIZUK_DEVICE_EXTENSION)DeviceObject->DeviceExtension)->RemoveLock, Irp);
    if (!NT_SUCCESS(status)) {
        Irp->IoStatus.Status = status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return status;
    }

    status = (iosl->MajorFunction == IRP_MJ_CREATE) ? IzkCreate(DeviceObject, Irp)
                                                    : IzkClose(DeviceObject, Irp);
    IoReleaseRemoveLock(&((PIZUK_DEVICE_EXTENSION)DeviceObject->DeviceExtension)->RemoveLock, Irp);
    return status;
}

EXTERN_C NTSTATUS Izk_DispatchInternalDeviceControl(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    // Original forwards internal IOCTLs (IOCTL_INTERNAL_USB_*, research 01 §5) down.
    PIZUK_DEVICE_EXTENSION dx = (PIZUK_DEVICE_EXTENSION)DeviceObject->DeviceExtension;
    IoSkipCurrentIrpStackLocation(Irp);
    return IoCallDriver(dx->LowerDevice, Irp);
}

EXTERN_C NTSTATUS Izk_DispatchSystemControl(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    // WMI not implemented; forward.
    PIZUK_DEVICE_EXTENSION dx = (PIZUK_DEVICE_EXTENSION)DeviceObject->DeviceExtension;
    IoSkipCurrentIrpStackLocation(Irp);
    return IoCallDriver(dx->LowerDevice, Irp);
}

EXTERN_C NTSTATUS Izk_DispatchPower(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIZUK_DEVICE_EXTENSION dx = (PIZUK_DEVICE_EXTENSION)DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION iosl = IoGetCurrentIrpStackLocation(Irp);

    if (iosl->MinorFunction == IRP_MN_WAIT_WAKE) {
        PoStartNextPowerIrp(Irp);
        IoSkipCurrentIrpStackLocation(Irp);
        return PoCallDriver(dx->LowerDevice, Irp);
    }

    // Stop isoch engines before leaving D0.
    if (iosl->MinorFunction == IRP_MN_SET_POWER &&
        iosl->Parameters.Power.Type == DevicePowerState &&
        iosl->Parameters.Power.State.DeviceState != PowerDeviceD0) {
        Izk_IsoStop(dx, TRUE);
        Izk_IsoStop(dx, FALSE);
    }

    PoStartNextPowerIrp(Irp);
    IoSkipCurrentIrpStackLocation(Irp);
    return PoCallDriver(dx->LowerDevice, Irp);
}

EXTERN_C NTSTATUS Izk_DispatchPnp(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIZUK_DEVICE_EXTENSION dx = (PIZUK_DEVICE_EXTENSION)DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION iosl = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS status = STATUS_SUCCESS;
    KEVENT event;
    BOOLEAN completeHere = FALSE;

    status = IoAcquireRemoveLock(&dx->RemoveLock, Irp);
    if (!NT_SUCCESS(status)) {
        Irp->IoStatus.Status = status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return status;
    }

    switch (iosl->MinorFunction) {
    case IRP_MN_START_DEVICE:
        // Wait for the lower stack to finish starting, then configure USB
        // (select config, alternate setting).
        KeInitializeEvent(&event, NotificationEvent, FALSE);
        IoCopyCurrentIrpStackLocationToNext(Irp);
        IoSetCompletionRoutine(Irp, IzkPnpStartCompletion, &event, TRUE, TRUE, TRUE);
        status = IoCallDriver(dx->LowerDevice, Irp);
        if (status == STATUS_PENDING) {
            KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, nullptr);
        }
        status = Irp->IoStatus.Status;
        if (!NT_SUCCESS(status)) {
            break;  // complete the IRP with the lower status below
        }
        status = Izk_UsbConfigure(dx);
        dx->ConfigFailed = !NT_SUCCESS(status);
        if (NT_SUCCESS(status)) {
            IoSetDeviceInterfaceState(&dx->InterfaceSymbolicLink, TRUE);
        }
        completeHere = TRUE;
        break;

    case IRP_MN_REMOVE_DEVICE:
        IoSetDeviceInterfaceState(&dx->InterfaceSymbolicLink, FALSE);
        Izk_IsoStop(dx, TRUE);
        Izk_IsoStop(dx, FALSE);
        Izk_UsbUnconfigure(dx);

        IoCopyCurrentIrpStackLocationToNext(Irp);
        status = IoCallDriver(dx->LowerDevice, Irp);
        IoReleaseRemoveLockAndWait(&dx->RemoveLock, Irp);
        IoDeleteSymbolicLink(&dx->DosSymLink);
        RtlFreeUnicodeString(&dx->InterfaceSymbolicLink);
        IoDetachDevice(dx->LowerDevice);
        IoDeleteDevice(dx->Self);
        return status;

    case IRP_MN_QUERY_STOP_DEVICE:
    case IRP_MN_QUERY_REMOVE_DEVICE:
        status = STATUS_SUCCESS;
        break;

    default:
        break;
    }

    if (!completeHere) {
        IoSkipCurrentIrpStackLocation(Irp);
        status = IoCallDriver(dx->LowerDevice, Irp);
        IoReleaseRemoveLock(&dx->RemoveLock, Irp);
        return status;
    }

    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    IoReleaseRemoveLock(&dx->RemoveLock, Irp);
    return status;
}
