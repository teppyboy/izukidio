// usb.cpp - USB configuration: select config, alternate settings, sample rate.
// Original used legacy USBD_CreateConfigurationRequestEx (research 01 §6);
// reimplementation uses the same USBD calls where still supported but through
// USBD_CreateHandle (modern entry point) so the driver links on current WDKs.
#include "common.h"

static NTSTATUS IzkConfigureEndpoints(PIZUK_DEVICE_EXTENSION dx)
{
    // Playback (IF1, EP 0x02 OUT) and capture (IF2, EP 0x84 IN) live on
    // different interfaces - scan both (descriptor: docs/izuki/usb-report.txt).
    PUSBD_INTERFACE_INFORMATION ifaces[2] = { dx->InterfaceInfo, dx->CaptureInterfaceInfo };
    ULONG i, j;

    dx->IsoInPipe = nullptr;
    dx->IsoOutPipe = nullptr;

    for (j = 0; j < 2; ++j) {
        if (ifaces[j] == nullptr) {
            continue;
        }
        for (i = 0; i < ifaces[j]->NumberOfPipes; ++i) {
            USBD_PIPE_INFORMATION* pipe = &ifaces[j]->Pipes[i];
            if (pipe->PipeType != UsbdPipeTypeIsochronous) {
                continue;
            }
            if (pipe->EndpointAddress & 0x80) {
                // IN endpoint: capture (ADC) or explicit feedback.
                // UMC22/PCM2902: audio IN = EP 0x84, 196 B; no feedback EP on
                // this hardware (OUT is Adaptive, IN is Asynchronous).
                if (pipe->MaximumPacketSize > 3 && dx->IsoInPipe == nullptr) {
                    dx->IsoInPipe = pipe->PipeHandle;
                    dx->MaxPacketIn = pipe->MaximumPacketSize;
                } else if (dx->FeedbackPipe == nullptr) {
                    dx->FeedbackPipe = pipe->PipeHandle;
                }
            } else {
                if (dx->IsoOutPipe == nullptr) {
                    dx->IsoOutPipe = pipe->PipeHandle;
                    dx->MaxPacketOut = pipe->MaximumPacketSize;
                }
            }
        }
    }

    if (dx->IsoInPipe == nullptr || dx->IsoOutPipe == nullptr) {
        DbgPrint("IZUKIDIO: isoch endpoints not found (in=%p out=%p)\n", dx->IsoInPipe, dx->IsoOutPipe);
        return STATUS_INVALID_DEVICE_STATE;
    }
    return STATUS_SUCCESS;
}

EXTERN_C NTSTATUS Izk_UsbSendUrbSync(PIZUK_DEVICE_EXTENSION dx, PURB Urb)
{
    KEVENT event;
    IO_STATUS_BLOCK iosb;
    PIRP irp;
    NTSTATUS status;

    KeInitializeEvent(&event, NotificationEvent, FALSE);
    irp = IoBuildDeviceIoControlRequest(IOCTL_INTERNAL_USB_SUBMIT_URB,
                                        dx->LowerDevice,
                                        nullptr, 0,
                                        nullptr, 0,
                                        TRUE,
                                        &event,
                                        &iosb);
    if (irp == nullptr) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    // USB stack expects the URB in Parameters.Others.Argument1 of the next
    // stack location (documented IOCTL_INTERNAL_USB_SUBMIT_URB pattern).
    IoGetNextIrpStackLocation(irp)->Parameters.Others.Argument1 = Urb;

    status = IoCallDriver(dx->LowerDevice, irp);
    if (status == STATUS_PENDING) {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, nullptr);
        status = iosb.Status;
    }
    return status;
}

EXTERN_C NTSTATUS Izk_UsbConfigure(PIZUK_DEVICE_EXTENSION dx)
{
    NTSTATUS status;
    PURB urb = nullptr;
    USBD_INTERFACE_LIST_ENTRY interfaceList[2];
    ULONG i;
    struct {
        UCHAR Number;
        UCHAR Alternate;
    } wanted[2];

    if (dx->ConfigDescriptor != nullptr) {
        return STATUS_SUCCESS;  // already configured (resume)
    }

    // USBD_CLIENT_CONTRACT_VERSION_600 was removed from modern WDKs; the 0x600
    // interface version constant is the same value USBD_CreateHandle expects.
    status = USBD_CreateHandle(dx->Self, dx->LowerDevice, USBD_INTERFACE_VERSION_600, 0, &dx->UsbdHandle);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    // Read config descriptor header (9 B) to learn wTotalLength, then the
    // full configuration. Buffers are pool-allocated - the descriptor bytes
    // must not overwrite the dx->ConfigDescriptor pointer itself.
    urb = (PURB)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(struct _URB_CONTROL_DESCRIPTOR_REQUEST), IZUK_TAG);
    if (urb == nullptr) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    {
        USB_CONFIGURATION_DESCRIPTOR cfgHeader;
        RtlZeroMemory(&cfgHeader, sizeof(cfgHeader));
        UsbBuildGetDescriptorRequest(urb,
                                     (USHORT)sizeof(struct _URB_CONTROL_DESCRIPTOR_REQUEST),
                                     USB_CONFIGURATION_DESCRIPTOR_TYPE, 0, 0,
                                     &cfgHeader, nullptr, sizeof(cfgHeader), nullptr);
        status = Izk_UsbSendUrbSync(dx, urb);
        if (NT_SUCCESS(status) && cfgHeader.wTotalLength >= sizeof(cfgHeader)) {
            dx->ConfigDescriptor = (PUSB_CONFIGURATION_DESCRIPTOR)ExAllocatePool2(
                POOL_FLAG_NON_PAGED, cfgHeader.wTotalLength, IZUK_TAG);
            if (dx->ConfigDescriptor == nullptr) {
                status = STATUS_INSUFFICIENT_RESOURCES;
            } else {
                RtlZeroMemory(dx->ConfigDescriptor, cfgHeader.wTotalLength);
                UsbBuildGetDescriptorRequest(urb,
                                             (USHORT)sizeof(struct _URB_CONTROL_DESCRIPTOR_REQUEST),
                                             USB_CONFIGURATION_DESCRIPTOR_TYPE, 0, 0,
                                             dx->ConfigDescriptor, nullptr,
                                             cfgHeader.wTotalLength, nullptr);
                status = Izk_UsbSendUrbSync(dx, urb);
                if (!NT_SUCCESS(status)) {
                    ExFreePoolWithTag(dx->ConfigDescriptor, IZUK_TAG);
                    dx->ConfigDescriptor = nullptr;
                }
            }
        } else if (NT_SUCCESS(status)) {
            status = STATUS_INVALID_DEVICE_STATE;
        }
    }
    ExFreePoolWithTag(urb, IZUK_TAG);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    // Select BOTH streaming interfaces at alternate 1 (docs/izuki/
    // usb-report.txt, measured on real UMC22):
    //   IF1 alt1 = playback, EP 0x02 OUT iso Adaptive, stereo 16-bit,
    //              32/44.1/48k, wMaxPacket 192 (SET_CUR picks the rate)
    //   IF2 alt1 = capture, EP 0x84 IN iso Asynchronous, stereo 16-bit 48k
    //              only, wMaxPacket 196 (192 + 4 B drift headroom)
    // Alt 0 is zero-bandwidth (no endpoints). Rate selection for capture is
    // via alternate setting, not SET_CUR; PoC targets 48k stereo only.
    RtlZeroMemory(interfaceList, sizeof(interfaceList));
    wanted[0].Number = 1; wanted[0].Alternate = 1;   // playback
    wanted[1].Number = 2; wanted[1].Alternate = 1;   // capture

    for (i = 0; i < 2; ++i) {
        interfaceList[i].InterfaceDescriptor = USBD_ParseConfigurationDescriptorEx(
            dx->ConfigDescriptor, dx->ConfigDescriptor,
            wanted[i].Number, wanted[i].Alternate, -1, -1, -1);
        if (interfaceList[i].InterfaceDescriptor == nullptr) {
            return STATUS_NO_SUCH_DEVICE;
        }
    }
    status = USBD_SelectConfigUrbAllocateAndBuild(dx->UsbdHandle,
                                                  dx->ConfigDescriptor,
                                                  interfaceList,
                                                  &urb);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    status = Izk_UsbSendUrbSync(dx, urb);
    if (NT_SUCCESS(status)) {
        dx->InterfaceInfo = interfaceList[0].Interface;
        dx->CaptureInterfaceInfo = interfaceList[1].Interface;
        dx->AudioInterfaceNumber = interfaceList[0].InterfaceDescriptor->bInterfaceNumber;
        dx->AudioAlternateSetting = interfaceList[0].InterfaceDescriptor->bAlternateSetting;
        // Measured descriptor facts (UMC22, rev 0x0100): stereo 16-bit only.
        // 4-byte frame = exactly one ring dword slot (isoch.cpp).
        dx->SampleRate = 48000;
        dx->BytesPerSample = 2;
        dx->ChannelsIn = 2;
        dx->ChannelsOut = 2;
        status = IzkConfigureEndpoints(dx);
    }
    USBD_UrbFree(dx->UsbdHandle, urb);
    return status;
}

// Pass a client-supplied vendor/class request through to the control pipe
// (IOCTL 0x220008, research 01 §4).
EXTERN_C NTSTATUS Izk_UsbVendorClassRequest(PIZUK_DEVICE_EXTENSION dx,
                                            PIZUK_VENDOR_OR_CLASS_REQUEST req,
                                            PUCHAR payload, ULONG payloadLen)
{
    PURB urb;
    NTSTATUS status;
    BOOLEAN inbound = (req->bmRequestType & 0x80) != 0;

    urb = (PURB)ExAllocatePool2(POOL_FLAG_NON_PAGED,
                                sizeof(struct _URB_CONTROL_VENDOR_OR_CLASS_REQUEST), IZUK_TAG);
    if (urb == nullptr) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    UsbBuildVendorRequest(urb,
                          URB_FUNCTION_VENDOR_DEVICE,
                          (USHORT)sizeof(struct _URB_CONTROL_VENDOR_OR_CLASS_REQUEST),
                          inbound ? USBD_TRANSFER_DIRECTION_IN : USBD_TRANSFER_DIRECTION_OUT,
                          0,                            // reserved bits
                          req->bRequest,
                          req->wValue,
                          req->wIndex,
                          payload,
                          nullptr,
                          payloadLen,
                          nullptr);
    status = Izk_UsbSendUrbSync(dx, urb);
    ExFreePoolWithTag(urb, IZUK_TAG);
    return status;
}

// IOCTL_INTERNAL_USB_CYCLE_PORT (0x22001F) to the USB PDO: makes the hub
// driver re-enumerate the device, mirroring the original stop path (research
// 01 §3: sub_F10203E0, waits out STATUS_PENDING).
EXTERN_C NTSTATUS Izk_UsbCyclePort(PIZUK_DEVICE_EXTENSION dx)
{
    KEVENT event;
    IO_STATUS_BLOCK iosb;
    PIRP irp;
    NTSTATUS status;

    KeInitializeEvent(&event, NotificationEvent, FALSE);
    irp = IoBuildDeviceIoControlRequest(0x22001F,   // IOCTL_INTERNAL_USB_CYCLE_PORT
                                        dx->LowerDevice,
                                        nullptr, 0,
                                        nullptr, 0,
                                        TRUE,
                                        &event,
                                        &iosb);
    if (irp == nullptr) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    status = IoCallDriver(dx->LowerDevice, irp);
    if (status == STATUS_PENDING) {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, nullptr);
        status = iosb.Status;
    }
    DbgPrint("IZUKIDIO: cycle port status %08X\n", status);
    return STATUS_SUCCESS;  // original treats cycle result as informational
}

EXTERN_C void Izk_UsbUnconfigure(PIZUK_DEVICE_EXTENSION dx)
{
    if (dx->ConfigDescriptor != nullptr) {
        ExFreePoolWithTag(dx->ConfigDescriptor, IZUK_TAG);
        dx->ConfigDescriptor = nullptr;
    }
    dx->InterfaceInfo = nullptr;
    dx->CaptureInterfaceInfo = nullptr;
    dx->IsoInPipe = nullptr;
    dx->IsoOutPipe = nullptr;
    dx->FeedbackPipe = nullptr;
    if (dx->UsbdHandle != nullptr) {
        USBD_CloseHandle(dx->UsbdHandle);
        dx->UsbdHandle = nullptr;
    }
}

EXTERN_C NTSTATUS Izk_UsbSelectAlternate(PIZUK_DEVICE_EXTENSION dx, UCHAR alternateSetting)
{
    NTSTATUS status;
    PURB urb = nullptr;
    USBD_INTERFACE_LIST_ENTRY interfaceList[2];
    PUSB_INTERFACE_DESCRIPTOR id;

    if (dx->ConfigDescriptor == nullptr) {
        return STATUS_INVALID_DEVICE_STATE;
    }

    id = USBD_ParseConfigurationDescriptorEx(dx->ConfigDescriptor, dx->ConfigDescriptor,
                                             dx->AudioInterfaceNumber, alternateSetting,
                                             -1, -1, -1);
    if (id == nullptr) {
        return STATUS_NO_SUCH_DEVICE;
    }

    RtlZeroMemory(interfaceList, sizeof(interfaceList));
    interfaceList[0].InterfaceDescriptor = id;
    interfaceList[0].Interface = dx->InterfaceInfo;

    status = USBD_SelectConfigUrbAllocateAndBuild(dx->UsbdHandle,
                                                  dx->ConfigDescriptor,
                                                  interfaceList,
                                                  &urb);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    status = Izk_UsbSendUrbSync(dx, urb);
    if (NT_SUCCESS(status)) {
        dx->InterfaceInfo = interfaceList[0].Interface;
        dx->AudioAlternateSetting = alternateSetting;
        status = IzkConfigureEndpoints(dx);
    }
    USBD_UrbFree(dx->UsbdHandle, urb);
    return status;
}

EXTERN_C NTSTATUS Izk_UsbSetSampleRate(PIZUK_DEVICE_EXTENSION dx, ULONG sampleRate)
{
    // Class-specific SET_CUR to the streaming endpoint sampling-frequency control:
    // bmRequestType 0x22, bRequest SET_CUR, wValue 0x0100 (EP ctrl), wIndex (ep|iface).
    // PCM2902 only accepts rates of its active alternate (44100/48000 families).
    NTSTATUS status;
    PURB urb;
    ULONG rate = RtlUlongByteSwap(sampleRate);   // USB is big-endian
    UCHAR ep = 0x01;                             // ponytail: derive from endpoint descriptor

    urb = (PURB)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(struct _URB_CONTROL_VENDOR_OR_CLASS_REQUEST), IZUK_TAG);
    if (urb == nullptr) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    // Class-specific SET_CUR (sampling frequency) to the streaming endpoint:
    // bmRequestType 0x22 (class|EP|out), wValue 0x0100 (CS=1, channel 0),
    // wIndex = endpoint | interface<<8, 3-byte big-endian rate.
    UsbBuildVendorRequest(urb,
                          URB_FUNCTION_VENDOR_DEVICE,
                          (USHORT)sizeof(struct _URB_CONTROL_VENDOR_OR_CLASS_REQUEST),
                          USBD_TRANSFER_DIRECTION_OUT,
                          0x22,                        // bmRequestType reserved bits
                          UAC_SET_CUR,
                          (USHORT)(UAC_EP_SAM_FREQ_CTRL),   // wValue: CS | channel 0
                          (USHORT)((ep & 0x0F) | (dx->AudioInterfaceNumber << 8)),
                          &rate, nullptr,
                          3,                           // 24-bit sampling frequency
                          nullptr);
    status = Izk_UsbSendUrbSync(dx, urb);
    ExFreePoolWithTag(urb, IZUK_TAG);
    if (NT_SUCCESS(status)) {
        dx->SampleRate = sampleRate;
    }
    return status;
}
