// isoch.cpp - isochronous stream engine.
// Original (research 01 §7): system-thread worker + per-CPU timer/DPC bank re-submitting
// ISOCH URBs; 480/512-byte packets at full speed. Reimplementation: pre-queued
// URB_FUNCTION_ISOCH_TRANSFER ring with completion-routine re-submission.
#include "common.h"

static const ULONG IzkFramesPerUrb = IZUK_ISO_PACKETS_PER_URB;   // 8 ms per URB @ FS

static VOID IzkIsochCompletion(PDEVICE_OBJECT DeviceObject, PIRP Irp, PVOID Context);

// Wrap-around helper: index in [0, count)
#define NEXT_SLOT(i) (((i) + 1) % IZUK_MAX_ISO_URBS)

static NTSTATUS IzkBuildAndSubmitUrb(PIZUK_DEVICE_EXTENSION dx, PIZUK_ISO_ENDPOINT ep, ULONG slot)
{
    PURB urb;
    PMDL mdl;
    ULONG urbSize;
    ULONG i;

    urbSize = GET_ISO_URB_SIZE(ep->FramesPerUrb);
    urb = ep->Urb[slot];
    RtlZeroMemory(urb, urbSize);

    mdl = ep->Mdl[slot];

    urb->UrbIsochronousTransfer.Hdr.Length = (USHORT)urbSize;
    urb->UrbIsochronousTransfer.Hdr.Function = URB_FUNCTION_ISOCH_TRANSFER;
    urb->UrbIsochronousTransfer.Hdr.Status = USBD_STATUS_SUCCESS;
    urb->UrbIsochronousTransfer.PipeHandle = ep->PipeHandle;
    urb->UrbIsochronousTransfer.TransferFlags = USBD_START_ISO_TRANSFER_ASAP |
                                                (ep->Inbound ? USBD_TRANSFER_DIRECTION_IN : 0);
    urb->UrbIsochronousTransfer.TransferBufferMDL = mdl;
    urb->UrbIsochronousTransfer.NumberOfPackets = ep->FramesPerUrb;
    urb->UrbIsochronousTransfer.UrbLink = nullptr;

    for (i = 0; i < ep->FramesPerUrb; ++i) {
        urb->UrbIsochronousTransfer.IsoPacket[i].Offset = i * ep->BytesPerFrame;
        urb->UrbIsochronousTransfer.IsoPacket[i].Length = (ep->Inbound ? 0 : ep->BytesPerFrame);
        urb->UrbIsochronousTransfer.IsoPacket[i].Status = USBD_STATUS_SUCCESS;
    }

    IoSetCompletionRoutine(ep->UrbIrp[slot], IzkIsochCompletion, dx, TRUE, TRUE, TRUE);
    return IoCallDriver(dx->LowerDevice, ep->UrbIrp[slot]);
}

static VOID IzkIsochCompletion(PDEVICE_OBJECT DeviceObject, PIRP Irp, PVOID Context)
{
    PIZUK_DEVICE_EXTENSION dx = (PIZUK_DEVICE_EXTENSION)Context;
    PIZUK_ISO_ENDPOINT ep;
    PURB urb;
    ULONG slot = MAXULONG;
    ULONG i;
    KIRQL oldIrql;

    UNREFERENCED_PARAMETER(DeviceObject);

    if (Irp->Cancel || Irp->IoStatus.Status == STATUS_CANCELLED) {
        return; // stopping; no re-submit
    }

    // Find which engine owns this IRP.
    for (i = 0; i < IZUK_MAX_ISO_URBS; ++i) {
        if (dx->In.UrbIrp[i] == Irp)  { ep = &dx->In;  slot = i; break; }
        if (dx->Out.UrbIrp[i] == Irp) { ep = &dx->Out; slot = i; break; }
    }
    if (slot == MAXULONG) {
        return;
    }

    urb = ep->Urb[slot];
    ep->ErrorCount += urb->UrbIsochronousTransfer.ErrorCount;

    // Advance the sample clock by the frames actually transferred.
    if (NT_SUCCESS(Irp->IoStatus.Status)) {
        InterlockedAdd64(&ep->FramesTransferred64, (LONG64)ep->FramesPerUrb);
        dx->SampleClock.QuadPart = ep->FramesTransferred64 * (ep->Inbound ? 1 : 1);
    }

    if (ep->Active) {
        // Re-submit the same slot; ASAP keeps pacing.
        IzkBuildAndSubmitUrb(dx, ep, slot);
    } else {
        KeAcquireSpinLock(&ep->Lock, &oldIrql);
        if (--ep->PendingUrbCount == 0) {
            KeSetEvent(&ep->StopEvent, IO_NO_INCREMENT, FALSE);
        }
        KeReleaseSpinLock(&ep->Lock, oldIrql);
    }
}

static NTSTATUS IzkAllocateEndpointBuffers(PIZUK_DEVICE_EXTENSION dx, PIZUK_ISO_ENDPOINT ep)
{
    NTSTATUS status = STATUS_SUCCESS;
    ULONG urbSize = GET_ISO_URB_SIZE(ep->FramesPerUrb);
    ULONG i;

    ep->FramesPerUrb = IzkFramesPerUrb;
    for (i = 0; i < IZUK_MAX_ISO_URBS; ++i) {
        ep->Urb[i] = (PURB)ExAllocatePool2(POOL_FLAG_NON_PAGED, urbSize, IZUK_TAG);
        ep->TransferBuffer[i] = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED, ep->FramesPerUrb * ep->BytesPerFrame, IZUK_TAG);
        ep->UrbIrp[i] = IoAllocateIrp((CCHAR)(dx->LowerDevice->StackSize + 1), FALSE);
        if (ep->Urb[i] == nullptr || ep->TransferBuffer[i] == nullptr || ep->UrbIrp[i] == nullptr) {
            status = STATUS_INSUFFICIENT_RESOURCES;
            break;
        }
        ep->Mdl[i] = IoAllocateMdl(ep->TransferBuffer[i], ep->FramesPerUrb * ep->BytesPerFrame, FALSE, FALSE, nullptr);
        if (ep->Mdl[i] == nullptr) {
            status = STATUS_INSUFFICIENT_RESOURCES;
            break;
        }
        MmBuildMdlForNonPagedPool(ep->Mdl[i]);
    }
    return status;
}

static void IzkFreeEndpointBuffers(PIZUK_DEVICE_EXTENSION dx, PIZUK_ISO_ENDPOINT ep)
{
    ULONG i;
    UNREFERENCED_PARAMETER(dx);
    for (i = 0; i < IZUK_MAX_ISO_URBS; ++i) {
        if (ep->Mdl[i] != nullptr)          { IoFreeMdl(ep->Mdl[i]); ep->Mdl[i] = nullptr; }
        if (ep->TransferBuffer[i] != nullptr){ ExFreePoolWithTag(ep->TransferBuffer[i], IZUK_TAG); ep->TransferBuffer[i] = nullptr; }
        if (ep->Urb[i] != nullptr)          { ExFreePoolWithTag(ep->Urb[i], IZUK_TAG); ep->Urb[i] = nullptr; }
        if (ep->UrbIrp[i] != nullptr)       { IoFreeIrp(ep->UrbIrp[i]); ep->UrbIrp[i] = nullptr; }
    }
}

EXTERN_C NTSTATUS Izk_IsoStart(PIZUK_DEVICE_EXTENSION dx, BOOLEAN inbound)
{
    PIZUK_ISO_ENDPOINT ep = inbound ? &dx->In : &dx->Out;
    NTSTATUS status;
    ULONG i;

    if (ep->Active) {
        return STATUS_SUCCESS;
    }
    if (ep->PipeHandle == nullptr) {
        return STATUS_INVALID_DEVICE_STATE;
    }

    // Bytes per frame: channels * sample size; FS isoch = 1 ms per frame.
    // 44100 Hz needs occasional 9-byte over-packets; PoC pads to the 48000-sized
    // slot (PCM2902 alternates encode this via wMaxPacketSize).
    ep->BytesPerFrame = (inbound ? dx->ChannelsIn : dx->ChannelsOut) * dx->BytesPerSample;

    KeInitializeEvent(&ep->StopEvent, NotificationEvent, FALSE);
    ep->ErrorCount = 0;
    ep->FramesTransferred64 = 0;
    ep->PendingUrbCount = IZUK_MAX_ISO_URBS;
    ep->Active = TRUE;

    status = IzkAllocateEndpointBuffers(dx, ep);
    if (!NT_SUCCESS(status)) {
        ep->Active = FALSE;
        IzkFreeEndpointBuffers(dx, ep);
        return status;
    }

    for (i = 0; i < IZUK_MAX_ISO_URBS; ++i) {
        status = IzkBuildAndSubmitUrb(dx, ep, i);
        if (!NT_SUCCESS(status) && status != STATUS_PENDING) {
            ep->Active = FALSE;
            Izk_IsoStop(dx, inbound);
            return status;
        }
    }
    return STATUS_SUCCESS;
}

EXTERN_C NTSTATUS Izk_IsoStop(PIZUK_DEVICE_EXTENSION dx, BOOLEAN inbound)
{
    PIZUK_ISO_ENDPOINT ep = inbound ? &dx->In : &dx->Out;
    ULONG i;
    LARGE_INTEGER timeout;

    if (!ep->Active) {
        return STATUS_SUCCESS;
    }
    ep->Active = FALSE;

    for (i = 0; i < IZUK_MAX_ISO_URBS; ++i) {
        if (ep->UrbIrp[i] != nullptr) {
            IoCancelIrp(ep->UrbIrp[i]);
        }
    }

    timeout.QuadPart = -(10LL * 1000 * 10000);   // 10 s
    KeWaitForSingleObject(&ep->StopEvent, Executive, KernelMode, FALSE, &timeout);

    IzkFreeEndpointBuffers(dx, ep);
    ep->PendingUrbCount = 0;
    return STATUS_SUCCESS;
}
