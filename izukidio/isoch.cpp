// isoch.cpp - isochronous stream engine.
// Original (research 01 §7): system-thread worker + per-CPU timer/DPC bank re-submitting
// ISOCH URBs; 480/512-byte packets at full speed. Reimplementation: pre-queued
// URB_FUNCTION_ISOCH_TRANSFER ring with completion-routine re-submission.
#include "common.h"

static const ULONG IzkFramesPerUrb = IZUK_ISO_PACKETS_PER_URB;   // 8 ms per URB @ FS

static VOID IzkIsochCompletion(PDEVICE_OBJECT DeviceObject, PIRP Irp, PVOID Context);

// Wrap-around helper: index in [0, count)
#define NEXT_SLOT(i) (((i) + 1) % IZUK_MAX_ISO_URBS)

// ASIO shared-area ring access (research 03 §3.1–3.2, byte-exact port of
// busb2902.sys sub_F1007B30). Engine = dword array: [0]=writeIndex, [1]=readIndex,
// [2]=totalWritten (position counter), [3..]=ring of 201600 dwords.
// Capture (kernel→user) writes engine B (sharedVa+IZUK_ENGINE_SIZE);
// playback (user→kernel) drains engine A (sharedVa+0).
static ULONG Izk_RingWrite(PULONG engine, const PUCHAR data, ULONG lenDwords)
{
    ULONG n = lenDwords;
    ULONG freeDwords;
    PULONG ring = engine + 3;

    if (lenDwords > IZUK_RING_DWORDS - engine[2]) {   // clamp by accumulator (verbatim)
        n = IZUK_RING_DWORDS - engine[2];
    }
    freeDwords = IZUK_RING_DWORDS - engine[0];
    if (freeDwords < n) {
        if (data != nullptr) {
            if (freeDwords != 0) {
                RtlCopyMemory(&ring[engine[0]], data, 4 * freeDwords);
            }
            RtlCopyMemory(ring, data + 4 * freeDwords, 4 * (n - freeDwords));
        }
        engine[0] = n - freeDwords;
    } else {
        if (data != nullptr) {
            RtlCopyMemory(&ring[engine[0]], data, 4 * n);
        }
        engine[0] += n;
        if (engine[0] >= IZUK_RING_DWORDS) {
            engine[0] = 0;
        }
    }
    InterlockedAdd((volatile LONG*)&engine[2], (LONG)n);
    return n;
}

// Mirror of the write path for draining the playback ring (user-written,
// kernel-consumed): reads from readIndex, never overtakes writeIndex.
static ULONG Izk_RingRead(PULONG engine, PUCHAR dst, ULONG lenDwords)
{
    ULONG n = lenDwords;
    ULONG first;
    PULONG ring = engine + 3;
    ULONG avail = (engine[0] + IZUK_RING_DWORDS - engine[1]) % IZUK_RING_DWORDS;

    if (n > avail) {
        n = avail;
    }
    first = IZUK_RING_DWORDS - engine[1];
    if (first > n) {
        first = n;
    }
    RtlCopyMemory(dst, &ring[engine[1]], 4 * first);
    if (n > first) {
        RtlCopyMemory(dst + 4 * first, ring, 4 * (n - first));
    }
    engine[1] = (engine[1] + n) % IZUK_RING_DWORDS;
    return n;
}

// Account dropped capture bytes into tail+28 (research 03 §3.2,
// sub_F1017340 overflow accounting; frame size 4 bytes per dword slot).
static VOID Izk_AddOverflow(PIZUK_DEVICE_EXTENSION dx, ULONG droppedDwords)
{
    volatile LONG* p = (volatile LONG*)((PUCHAR)dx->AsioSharedVa
                                        + IZUK_SHARED_TAIL_OFFSET + IZUK_TAIL_OVERFLOW_OFFS);
    if (droppedDwords != 0) {
        InterlockedAdd(p, -(LONG)droppedDwords);
    }
}

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

    // Playback: pull this buffer's samples out of the user ring (engine A)
    // before submission; shortfall stays zero-filled.
    if (!ep->Inbound && dx->AsioSharedVa != nullptr) {
        ULONG dwords = ep->FramesPerUrb * ep->BytesPerFrame / 4;
        RtlZeroMemory(ep->TransferBuffer[slot], ep->FramesPerUrb * ep->BytesPerFrame);
        Izk_RingRead((PULONG)dx->AsioSharedVa, ep->TransferBuffer[slot], dwords);
    }

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
        urb->UrbIsochronousTransfer.IsoPacket[i].Offset = i * (ep->Inbound ? ep->MaxPacketSize : ep->BytesPerFrame);
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
        dx->SampleClock.QuadPart = ep->FramesTransferred64;
        // Publish capture audio into the user ring (engine B) and wake the
        // ASIO client (registered via 0x220030, research 03 §3).
        if (ep->Inbound && dx->AsioSharedVa != nullptr) {
            ULONG dwords = ep->FramesPerUrb * ep->BytesPerFrame / 4;
            ULONG written = Izk_RingWrite(
                (PULONG)((PUCHAR)dx->AsioSharedVa + IZUK_ENGINE_SIZE),
                ep->TransferBuffer[slot], dwords);
            Izk_AddOverflow(dx, dwords - written);
        }
        if (dx->AsioEvent != nullptr) {
            KeSetEvent(dx->AsioEvent, IO_NO_INCREMENT, FALSE);
        }
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
    // Capture buffers use MaxPacketSize: PCM2902 IN can deliver 196 B/frame
    // (wMaxPacketSize) vs the 192 B nominal payload, due to clock drift.
    ULONG frameStride = ep->Inbound ? ep->MaxPacketSize : ep->BytesPerFrame;
    ULONG i;

    ep->FramesPerUrb = IzkFramesPerUrb;
    for (i = 0; i < IZUK_MAX_ISO_URBS; ++i) {
        ep->Urb[i] = (PURB)ExAllocatePool2(POOL_FLAG_NON_PAGED, urbSize, IZUK_TAG);
        ep->TransferBuffer[i] = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED, ep->FramesPerUrb * frameStride, IZUK_TAG);
        ep->UrbIrp[i] = IoAllocateIrp((CCHAR)(dx->LowerDevice->StackSize + 1), FALSE);
        if (ep->Urb[i] == nullptr || ep->TransferBuffer[i] == nullptr || ep->UrbIrp[i] == nullptr) {
            status = STATUS_INSUFFICIENT_RESOURCES;
            break;
        }
        ep->Mdl[i] = IoAllocateMdl(ep->TransferBuffer[i], ep->FramesPerUrb * frameStride, FALSE, FALSE, nullptr);
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
