// isoch.cpp - isochronous stream engine.
// Original (research 01 §7): system-thread worker + per-CPU timer/DPC bank re-submitting
// ISOCH URBs; 480/512-byte packets at full speed. Reimplementation: pre-queued
// URB_FUNCTION_ISOCH_TRANSFER ring with completion-routine re-submission.
#include "common.h"

static const ULONG IzkPacketsPerUrb = IZUK_ISO_PACKETS_PER_URB;  // 10 slots / 10 ms (research 06 §3)

static NTSTATUS IzkIsochCompletion(PDEVICE_OBJECT DeviceObject, PIRP Irp, PVOID Context);

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
// The original floors the counter at 0 (divergence 3 in 05 §5): a CAS loop
// clamps negative results instead of letting the counter go below zero.
static VOID Izk_AddOverflow(PIZUK_DEVICE_EXTENSION dx, ULONG droppedDwords)
{
    volatile LONG* p = (volatile LONG*)((PUCHAR)dx->AsioSharedVa
                                        + IZUK_SHARED_TAIL_OFFSET + IZUK_TAIL_OVERFLOW_OFFS);
    LONG oldVal;
    LONG newVal;

    if (droppedDwords == 0) {
        return;
    }
    for (;;) {
        oldVal = *p;
        newVal = oldVal - (LONG)droppedDwords;
        if (newVal > 0) {
            newVal = 0;
        }
        if (InterlockedCompareExchange(p, newVal, oldVal) == oldVal) {
            break;
        }
    }
}

// Per-slot request length in bytes (research 01 §5.3, sub_F1018BE0 pattern
// tables). Non-integral-ms rates (44100/88200/176400) need fractional-byte
// slots: at 44.1 kHz the original requests 44 dwords + 1 dword every 10th
// slot (avg 44.1 dwords = 176.4 B/ms). We spread the +1 dword over each
// 10-slot URB (slot 0 of every URB), which preserves the exact average.
// ponytail: only exact for 4-byte frames (16-bit stereo, UMC22 reality);
// other frame sizes fall back to a uniform request.
static ULONG IzkSlotRequestBytes(PIZUK_DEVICE_EXTENSION dx, PIZUK_ISO_ENDPOINT ep, ULONG slot)
{
    ULONG bytes = ep->BytesPerFrame;

    if (dx->SampleRate % 1000 != 0 && ep->BytesPerFrame == 4) {
        if (slot % 10 == 0) {
            bytes += 4;
        }
    }
    return bytes;
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
    // before submission; shortfall stays zero-filled. OUT slots are packed at
    // their cumulative request offsets (44.1 kHz pattern, research 01 §5.3);
    // IN slots use the MaxPacketSize stride (device may deliver 196 B).
    if (!ep->Inbound && dx->AsioSharedVa != nullptr) {
        PUCHAR dst = ep->TransferBuffer[slot];
        RtlZeroMemory(dst, ep->FramesPerUrb * ep->MaxPacketSize);
        for (i = 0; i < ep->FramesPerUrb; ++i) {
            ULONG slotBytes = IzkSlotRequestBytes(dx, ep, i);
            Izk_RingRead((PULONG)dx->AsioSharedVa, dst, slotBytes / 4);
            dst += slotBytes;
        }
    }

    urb->UrbIsochronousTransfer.Hdr.Length = (USHORT)urbSize;
    urb->UrbIsochronousTransfer.Hdr.Function = URB_FUNCTION_ISOCH_TRANSFER;
    urb->UrbIsochronousTransfer.Hdr.Status = USBD_STATUS_SUCCESS;
    urb->UrbIsochronousTransfer.PipeHandle = ep->PipeHandle;
    // Absolute StartFrame pacing like the original (sub_F1022390, research
    // 01 §5.3): no ASAP, StartFrame += packet count per cycle.
    urb->UrbIsochronousTransfer.TransferFlags =
        (ep->Inbound ? USBD_TRANSFER_DIRECTION_IN : 0);
    urb->UrbIsochronousTransfer.StartFrame = ep->NextStartFrame;
    urb->UrbIsochronousTransfer.TransferBufferMDL = mdl;
    urb->UrbIsochronousTransfer.NumberOfPackets = ep->FramesPerUrb;
    urb->UrbIsochronousTransfer.UrbLink = nullptr;

    // All 10 slots submit data (research 06 §3.1). IN requests BytesPerFrame
    // uniformly (measured: 192 B @48k alt); OUT follows the per-rate pattern
    // (44.1 kHz: +1 dword every 10th slot). Offsets are exclusive-prefix
    // cumulative, same convention as sub_F1022390.
    {
        ULONG inStride = ep->MaxPacketSize;
        ULONG running = 0;
        for (i = 0; i < ep->FramesPerUrb; ++i) {
            ULONG slotBytes = ep->Inbound ? ep->BytesPerFrame
                                          : IzkSlotRequestBytes(dx, ep, i);
            urb->UrbIsochronousTransfer.IsoPacket[i].Offset =
                ep->Inbound ? i * inStride : running;
            urb->UrbIsochronousTransfer.IsoPacket[i].Length = slotBytes;
            urb->UrbIsochronousTransfer.IsoPacket[i].Status = USBD_STATUS_SUCCESS;
            running += slotBytes;
        }
        urb->UrbIsochronousTransfer.TransferBufferLength = running;
    }

    IoSetCompletionRoutine(ep->UrbIrp[slot], IzkIsochCompletion, dx, TRUE, TRUE, TRUE);
    {
        NTSTATUS st = IoCallDriver(dx->LowerDevice, ep->UrbIrp[slot]);
        if (NT_SUCCESS(st) || st == STATUS_PENDING) {
            ep->NextStartFrame += ep->FramesPerUrb;   // sub_F1015E10 tail: StartFrame += packetCount
        }
        return st;
    }
}

static NTSTATUS IzkIsochCompletion(PDEVICE_OBJECT DeviceObject, PIRP Irp, PVOID Context)
{
    PIZUK_DEVICE_EXTENSION dx = (PIZUK_DEVICE_EXTENSION)Context;
    PIZUK_ISO_ENDPOINT ep = nullptr;
    PURB urb;
    ULONG slot = MAXULONG;
    ULONG i;
    KIRQL oldIrql;

    UNREFERENCED_PARAMETER(DeviceObject);

    if (Irp->Cancel || Irp->IoStatus.Status == STATUS_CANCELLED) {
        return STATUS_SUCCESS; // stopping; no re-submit
    }

    // Find which engine owns this IRP.
    for (i = 0; i < dx->In.UrbCount; ++i) {
        if (dx->In.UrbIrp[i] == Irp)  { ep = &dx->In;  slot = i; break; }
    }
    if (ep == nullptr) {
        for (i = 0; i < dx->Out.UrbCount; ++i) {
            if (dx->Out.UrbIrp[i] == Irp) { ep = &dx->Out; slot = i; break; }
        }
    }
    for (i = 0; ep == nullptr && i < IZUK_MAX_ISO_URBS; ++i) {
        if (dx->In.UrbIrp[i] == Irp)  { ep = &dx->In;  slot = i; break; }
        if (dx->Out.UrbIrp[i] == Irp) { ep = &dx->Out; slot = i; break; }
    }
    if (slot == MAXULONG) {
        return STATUS_SUCCESS;
    }

    urb = ep->Urb[slot];
    ep->ErrorCount += urb->UrbIsochronousTransfer.ErrorCount;

    // Advance the sample clock by the frames actually transferred.
    if (NT_SUCCESS(Irp->IoStatus.Status)) {
        // Publish per iso packet (research 06 §3.1): the device may deliver
        // 49-frame (196 B) packets where 192 B were requested, and a
        // zero-length slot anywhere (queue starvation / device-side). IN ring
        // slots are 4-byte frames, publish len/4 dwords per packet.
        ULONG frameStride = ep->Inbound ? ep->MaxPacketSize : ep->BytesPerFrame;
        ULONG bytesDone = 0;
        for (i = 0; i < ep->FramesPerUrb; ++i) {
            ULONG len = urb->UrbIsochronousTransfer.IsoPacket[i].Length;
            if (ep->Inbound) {
                // Completion Length on IN = bytes the device delivered
                // (measured: 196 B = 49 frames per data packet, one
                // zero-length final slot at 44.1 kHz, research 06 §3.1).
                if (dx->AsioSharedVa != nullptr && len >= 4) {
                    ULONG dwords = len / 4;
                    ULONG written = Izk_RingWrite(
                        (PULONG)((PUCHAR)dx->AsioSharedVa + IZUK_ENGINE_SIZE),
                        ep->TransferBuffer[slot] + i * frameStride, dwords);
                    Izk_AddOverflow(dx, dwords - written);
                }
            }
            bytesDone += len;
        }
        // Convert delivered bytes to frames: 4-byte frames count by dwords
        // (variable 176/180 B slots at 44.1 kHz, research 01 §5.3).
        ULONG framesDone = (ep->BytesPerFrame == 4) ? bytesDone / 4
                                                    : bytesDone / ep->BytesPerFrame;
        InterlockedAdd64(&ep->FramesTransferred64, (LONG64)framesDone);
        dx->SampleClock.QuadPart = ep->FramesTransferred64;
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
    return STATUS_SUCCESS;
}

static NTSTATUS IzkAllocateEndpointBuffers(PIZUK_DEVICE_EXTENSION dx, PIZUK_ISO_ENDPOINT ep)
{
    NTSTATUS status = STATUS_SUCCESS;
    ULONG urbSize = GET_ISO_URB_SIZE(ep->FramesPerUrb);
    // Capture buffers use MaxPacketSize: measured IN completions return the
    // full 196 B (49 frames) per packet, not the 192 B request (research 06).
    ULONG frameStride = ep->Inbound ? ep->MaxPacketSize : ep->BytesPerFrame;
    ULONG i;

    ep->FramesPerUrb = IzkPacketsPerUrb;
    ep->UrbCount = ep->Inbound ? IZUK_ISO_URBS_IN : IZUK_ISO_URBS_OUT;
    for (i = 0; i < ep->UrbCount; ++i) {
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
    for (i = 0; i < IZUK_MAX_ISO_URBS; ++i) {   // free all, active count may vary
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
    // 44100 Hz needs occasional +1-dword over-slots (IzkSlotRequestBytes).
    ep->BytesPerFrame = (inbound ? dx->ChannelsIn : dx->ChannelsOut) * dx->BytesPerSample;

    // Seed absolute StartFrame pacing from the bus frame number
    // (research 01 §5.3; sub_F1022390 takes an absolute StartFrame).
    {
        struct _URB_GET_CURRENT_FRAME_NUMBER frameUrb;
        RtlZeroMemory(&frameUrb, sizeof(frameUrb));
        frameUrb.Length = sizeof(frameUrb);
        frameUrb.Function = URB_FUNCTION_GET_CURRENT_FRAME_NUMBER;
        status = Izk_UsbSendUrbSync(dx, (PURB)&frameUrb);
        if (NT_SUCCESS(status)) {
            ep->NextStartFrame = frameUrb.FrameNumber + 1;
        } else {
            ep->NextStartFrame = 0;
        }
    }

    KeInitializeEvent(&ep->StopEvent, NotificationEvent, FALSE);
    ep->ErrorCount = 0;
    ep->FramesTransferred64 = 0;
    ep->PendingUrbCount = (LONG)ep->UrbCount;
    ep->Active = TRUE;

    status = IzkAllocateEndpointBuffers(dx, ep);
    if (!NT_SUCCESS(status)) {
        ep->Active = FALSE;
        IzkFreeEndpointBuffers(dx, ep);
        return status;
    }

    for (i = 0; i < ep->UrbCount; ++i) {
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

    for (i = 0; i < ep->UrbCount; ++i) {
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
