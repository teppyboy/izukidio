// ioctl.cpp - DeviceIoControl contract, byte-compatible with busbasio_x64.dll.
// Contract source: docs/research/03-busbasio-dll.md (DLL call sites, buffer sizes)
// and docs/research/01-busb2902-sys.md (original kernel handler semantics).
#include "common.h"

static PIZUK_FILE_CONTEXT GetFileContext(PIRP Irp)
{
    PIO_STACK_LOCATION iosl = IoGetCurrentIrpStackLocation(Irp);
    return (PIZUK_FILE_CONTEXT)iosl->FileObject->FsContext;
}

static NTSTATUS CopyOut(PIRP Irp, const void* src, ULONG srcLen, ULONG outLen)
{
    ULONG n = (outLen < srcLen) ? outLen : srcLen;
    if (Irp->AssociatedIrp.SystemBuffer == nullptr && n != 0) {
        return STATUS_INVALID_USER_BUFFER;
    }
    RtlCopyMemory(Irp->AssociatedIrp.SystemBuffer, src, n);
    Irp->IoStatus.Information = n;
    return STATUS_SUCCESS;
}

static NTSTATUS CopyIn(PIRP Irp, void* dst, ULONG dstLen, ULONG inLen)
{
    ULONG n = (inLen < dstLen) ? inLen : dstLen;
    if (Irp->AssociatedIrp.SystemBuffer == nullptr) {
        return STATUS_INVALID_USER_BUFFER;
    }
    RtlCopyMemory(dst, Irp->AssociatedIrp.SystemBuffer, n);
    return STATUS_SUCCESS;
}

static NTSTATUS HandleProperty(PIZUK_DEVICE_EXTENSION dx, PIRP Irp, ULONG inLen, ULONG outLen)
{
    // 0x2200D0/D4: generic property dispatcher.
    // Buffer: [0]=cmd, [1..]=arg/return. Known commands (research 01 §4, sub_F1012C90):
    //  1=stop, 2=get sample rate, 3=set, 6=get ch count, 8=get buffer info,
    //  10=get version, 13/14=priority get/set, 19/20=get device name strings.
    PIZUK_PROPERTY_STRUCT p = (PIZUK_PROPERTY_STRUCT)Irp->AssociatedIrp.SystemBuffer;
    NTSTATUS status = STATUS_SUCCESS;

    if (p == nullptr || inLen < IZUK_PROPERTY_STRUCT_SIZE || outLen < IZUK_PROPERTY_STRUCT_SIZE) {
        return STATUS_INVALID_PARAMETER;
    }

    switch (p->Command) {
    case 2:     // get sample rate
        p->Arg[0] = dx->SampleRate;
        break;
    case 3:     // set sample rate
        status = Izk_UsbSetSampleRate(dx, p->Arg[0]);
        break;
    case 6:     // channel count
        p->Arg[0] = dx->ChannelsOut;
        break;
    case 8:     // buffer info: min/max/preferred/granularity (ASIO getBufferSize)
        p->Arg[0] = dx->Out.FramesPerUrb * IZUK_ISO_PACKETS_PER_URB;
        p->Arg[1] = p->Arg[0];
        p->Arg[2] = p->Arg[0];
        p->Arg[3] = (ULONG)-1;
        break;
    case 10:    // driver version
        p->Arg[0] = IZUK_MAGIC_BUILD;
        break;
    case 12:    // performance-mode priority names, original order (research 01 §3)
        // Arg[] space is small; the original returns a name table via strings.
        // PoC: report index-based priority (0 = "highspeed" ... 5 = "relaxed").
        p->Arg[0] = 0;
        break;
    case 13:    // get priority (research 03 §5: DLL uses 13/14, not 6)
        p->Arg[0] = 0;
        break;
    case 14:    // set priority
        // PoC: accepted, no scheduling change (we have no feeder thread yet).
        break;
    case 1:     // stop transport
        Izk_IsoStop(dx, TRUE);
        Izk_IsoStop(dx, FALSE);
        dx->TransportActive = FALSE;
        break;
    default:
        status = STATUS_INVALID_PARAMETER;
        break;
    }
    Irp->IoStatus.Information = IZUK_PROPERTY_STRUCT_SIZE;
    return status;
}

EXTERN_C NTSTATUS Izk_DispatchDeviceControl(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIZUK_DEVICE_EXTENSION dx = (PIZUK_DEVICE_EXTENSION)DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION iosl = IoGetCurrentIrpStackLocation(Irp);
    ULONG ioctl = iosl->Parameters.DeviceIoControl.IoControlCode;
    ULONG inLen = iosl->Parameters.DeviceIoControl.InputBufferLength;
    ULONG outLen = iosl->Parameters.DeviceIoControl.OutputBufferLength;
    PIZUK_FILE_CONTEXT ctx;
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    IZUK_HWINFO hw;
    ULONG value32;
    LARGE_INTEGER clock64;

    ctx = GetFileContext(Irp);

    switch (ioctl) {
    case IOCTL_IZUK_GET_NAME_A:
    case IOCTL_IZUK_GET_NAME_B:
        // Original returns USB string descriptors fetched from the device
        // (research 01 §4: stored at ext+5881/+6137, 255 B max).
        status = CopyOut(Irp,
                         (ioctl == IOCTL_IZUK_GET_NAME_A) ? "BEHRINGER USB AUDIO"
                                                          : "BEHRINGER",
                         20, outLen);
        break;

    case IOCTL_IZUK_GET_STATE:
        // 98,584 B state dump (research 02 §3): 8-byte header + 4 terminal
        // blocks. busbwdm.sys reads dword [+8] as the per-transaction frame
        // size. Format tables stay zeroed until hardware bring-up
        // (ponytail: ASIO needs real 0x6044-blocks; add on first device run).
        if (outLen != IZUK_STATE_DUMP_SIZE) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        RtlZeroMemory(Irp->AssociatedIrp.SystemBuffer, outLen);
        *((ULONG*)((PUCHAR)Irp->AssociatedIrp.SystemBuffer + IZUK_STATE_HEADER_SIZE)) =
            dx->Out.FramesPerUrb * IZUK_ISO_PACKETS_PER_URB * dx->BytesPerSample;
        Irp->IoStatus.Information = outLen;
        status = STATUS_SUCCESS;
        break;

    case IOCTL_IZUK_COMMIT_CONFIG:
        // 76 B config struct: [0..3] sample rate, [4..7] period, format fields.
        // Ponytail: exact field map TBD against the DLL on first hardware run;
        // we decode rate + period and re-configure the alternate setting.
        if (inLen != IZUK_CONFIG_STRUCT_SIZE) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        RtlCopyMemory(&value32, Irp->AssociatedIrp.SystemBuffer, sizeof(value32));
        status = Izk_UsbSetSampleRate(dx, value32);
        Irp->IoStatus.Information = 0;
        break;

    case IOCTL_IZUK_SET_PARAM:
        // Original sub_F100FEC0: stream-unit parameter, clamped >= 1
        // (stored at ext+7264) - NOT an alternate-setting selector.
        if (inLen != 4) { status = STATUS_INVALID_PARAMETER; break; }
        RtlCopyMemory(&value32, Irp->AssociatedIrp.SystemBuffer, 4);
        if (value32 < 1) { value32 = 1; }
        dx->StreamUnitParam = value32;
        status = STATUS_SUCCESS;
        Irp->IoStatus.Information = 0;
        break;

    case IOCTL_IZUK_STOP_CYCLE:
        // Original: abort pipes, tear down, then IOCTL_INTERNAL_USB_CYCLE_PORT
        // to the USB PDO (port cycle re-enumerates the device, research 01 §3).
        Izk_IsoStop(dx, TRUE);
        Izk_IsoStop(dx, FALSE);
        status = Izk_UsbCyclePort(dx);
        Irp->IoStatus.Information = 0;
        break;

    case IOCTL_IZUK_SET_PARAM_BLOCK:
        status = (inLen == 24) ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
        Irp->IoStatus.Information = 0;
        break;

    case IOCTL_IZUK_SHARED_AREA:
        // 16 B descriptor: [0] = register(1)/unregister(0), +8 = user VA of the
        // 0x189C38 ASIO shared area (research 03 §3).
        if (inLen != 16 || ctx == nullptr) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        {
            ULONG flag = 0;
            PVOID userVa = nullptr;
            RtlCopyMemory(&flag, Irp->AssociatedIrp.SystemBuffer, 4);
            RtlCopyMemory(&userVa, (PUCHAR)Irp->AssociatedIrp.SystemBuffer + 8, 8);
            status = Izk_SharedAreaRegister(dx, ctx, flag != 0, userVa);
        }
        Irp->IoStatus.Information = 0;
        break;

    case IOCTL_IZUK_GET_SAMPLE_CLOCK:
        clock64 = dx->SampleClock;
        status = CopyOut(Irp, &clock64, sizeof(clock64), outLen);
        break;

    case IOCTL_IZUK_GET_VERSION:
        value32 = IZUK_MAGIC_VERSION;
        status = CopyOut(Irp, &value32, 4, outLen);
        break;

    case IOCTL_IZUK_GET_CAP:
        value32 = 0;    // capability word: none advertised in PoC
        status = CopyOut(Irp, &value32, 4, outLen);
        break;

    case IOCTL_IZUK_INIT_ONCE:
        status = STATUS_SUCCESS;
        Irp->IoStatus.Information = 0;
        break;

    case IOCTL_IZUK_GET_HWINFO:
        hw.UsbVendorId = IZUK_USB_VID;
        hw.UsbProductId = dx->DeviceDescriptor ? dx->DeviceDescriptor->idProduct : 0;
        hw.ChipRevision = dx->DeviceDescriptor ? dx->DeviceDescriptor->bcdDevice : 0;
        hw.UsbSpeed = 0;
        status = CopyOut(Irp, &hw, sizeof(hw), outLen);
        break;

    case IOCTL_IZUK_GET_STATE_BYTE:
        value32 = dx->TransportActive ? 1 : 0;
        status = CopyOut(Irp, &value32, 4, outLen);
        break;

    case IOCTL_IZUK_SET_FILE_READY:
        if (inLen != 4 || ctx == nullptr) { status = STATUS_INVALID_PARAMETER; break; }
        RtlCopyMemory(&value32, Irp->AssociatedIrp.SystemBuffer, 4);
        ctx->FileReady = (value32 != 0);
        status = STATUS_SUCCESS;
        Irp->IoStatus.Information = 0;
        break;

    case IOCTL_IZUK_GET_BUILD:
        value32 = IZUK_MAGIC_BUILD;
        status = CopyOut(Irp, &value32, 4, outLen);
        break;

    case IOCTL_IZUK_REGISTER_CLIENT:
        if (inLen != 4 || ctx == nullptr) { status = STATUS_INVALID_PARAMETER; break; }
        RtlCopyMemory(&ctx->ClientPid, Irp->AssociatedIrp.SystemBuffer, 4);
        InterlockedIncrement(&dx->StreamingClients);
        status = STATUS_SUCCESS;
        Irp->IoStatus.Information = 0;
        break;

    case IOCTL_IZUK_GET_SHARED_COUNT:
        // Original sub_F1016A60: number of OTHER handles sharing our stream slot.
        value32 = (ctx != nullptr && ctx->ClientPid != 0) ? 1 : 0;   // PoC: single client
        status = CopyOut(Irp, &value32, 4, outLen);
        break;

    case IOCTL_IZUK_GET_POSITION:
        value32 = (ULONG)dx->Out.FramesTransferred64;
        status = CopyOut(Irp, &value32, 4, outLen);
        break;

    case IOCTL_IZUK_SET_CLOCK64:
        if (inLen != 8) { status = STATUS_INVALID_PARAMETER; break; }
        RtlCopyMemory(&dx->SampleClock, Irp->AssociatedIrp.SystemBuffer, 8);
        status = STATUS_SUCCESS;
        Irp->IoStatus.Information = 0;
        break;

    case IOCTL_IZUK_READ_STREAM:
    case IOCTL_IZUK_READ_STREAM_ALT:
    case IOCTL_IZUK_WRITE_STREAM:
        // PCM copy plane (0x2200C0/C4/C8). The ASIO DLL never calls these - the
        // zero-copy path is the 0x220030 shared-area registration (research 03
        // §3). Left NOT_IMPLEMENTED until a client for them shows up.
        status = STATUS_NOT_IMPLEMENTED;
        break;

    case IOCTL_IZUK_GET_STREAMINFO:
        if (outLen != IZUK_STREAMINFO_SIZE) { status = STATUS_INVALID_PARAMETER; break; }
        RtlZeroMemory(Irp->AssociatedIrp.SystemBuffer, outLen);
        Irp->IoStatus.Information = outLen;
        status = STATUS_SUCCESS;
        break;

    case IOCTL_IZUK_PROPERTY_CTRL:
    case IOCTL_IZUK_PROPERTY_FILE:
        status = HandleProperty(dx, Irp, inLen, outLen);
        break;

    case IOCTL_IZUK_INVALIDATE_REL:
        // Original re-enumerates its children here; PoC has no children yet.
        status = STATUS_SUCCESS;
        Irp->IoStatus.Information = inLen;
        break;

    case IOCTL_IZUK_VENDOR_CLASS_REQ: {
        // 8-byte setup-style header + variable payload -> control URB
        // (research 01 §4; header struct in pcm2902.h).
        PIZUK_VENDOR_OR_CLASS_REQUEST req;
        ULONG payloadLen;

        if (inLen < sizeof(IZUK_VENDOR_OR_CLASS_REQUEST)) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        req = (PIZUK_VENDOR_OR_CLASS_REQUEST)Irp->AssociatedIrp.SystemBuffer;
        payloadLen = inLen - sizeof(IZUK_VENDOR_OR_CLASS_REQUEST);
        if (payloadLen != 0 && req->wLength != 0 && payloadLen < req->wLength) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        status = Izk_UsbVendorClassRequest(dx, req,
                    (PUCHAR)Irp->AssociatedIrp.SystemBuffer + sizeof(IZUK_VENDOR_OR_CLASS_REQUEST),
                    payloadLen);
        Irp->IoStatus.Information = (req->bmRequestType & 0x80) ? payloadLen : 0;
        break;
    }

    case IOCTL_IZUK_SET_ROUTING_IN:
        // 3,844 B routing matrix (research 01 §3: sub_F100F9A0).
        // PoC: latch the table; the isoch path does not consume routing yet.
        if (inLen != IZUK_ROUTING_TABLE_SIZE) { status = STATUS_INVALID_PARAMETER; break; }
        RtlCopyMemory(dx->RoutingIn, Irp->AssociatedIrp.SystemBuffer, IZUK_ROUTING_TABLE_SIZE);
        Irp->IoStatus.Information = 0;
        status = STATUS_SUCCESS;
        break;

    case IOCTL_IZUK_GET_ROUTING_OUT:
        if (outLen != IZUK_ROUTING_TABLE_SIZE) { status = STATUS_INVALID_PARAMETER; break; }
        status = CopyOut(Irp, dx->RoutingOut, IZUK_ROUTING_TABLE_SIZE, outLen);
        break;

    case IOCTL_IZUK_CLOSE_FILE:
        // Release of the per-\IO-handle stream slot (sub_F1016F10). PoC has a
        // single implicit slot; nothing to free beyond the file context.
        status = STATUS_SUCCESS;
        Irp->IoStatus.Information = 0;
        break;

    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}
