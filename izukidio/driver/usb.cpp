// usb.cpp - USB configuration: select config, alternate settings, sample rate.
// Original used legacy USBD_CreateConfigurationRequestEx (research 01 §6);
// reimplementation uses the same USBD calls where still supported but through
// USBD_CreateHandle (modern entry point) so the driver links on current WDKs.
#include "common.h"

static NTSTATUS IzkConfigureEndpoints(PIZUK_DEVICE_EXTENSION dx)
{
    PUSBD_INTERFACE_INFORMATION iface = dx->InterfaceInfo;
    ULONG i;

    dx->IsoInPipe = nullptr;
    dx->IsoOutPipe = nullptr;

    for (i = 0; i < iface->NumberOfPipes; ++i) {
        USBD_PIPE_INFORMATION* pipe = &iface->Pipes[i];
        if (pipe->PipeType == UsbdPipeTypeIsochronous) {
            if (pipe->EndpointAddress & 0x80) {
                // IN endpoint: capture (ADC) or explicit feedback.
                // PCM2902: audio IN has the larger packet size; feedback is 3 bytes.
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
    irp->Tail.Overlay.DriverContext[0] = Urb;   // URB parameter for USB stack

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

    status = USBD_CreateHandle(dx->Self, dx->LowerDevice, USBD_CLIENT_CONTRACT_VERSION_600, 0, &dx->UsbdHandle);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    // Read config descriptor (PCM2902 exposes a single configuration).
    urb = (PURB)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(struct _URB_CONTROL_DESCRIPTOR_REQUEST), IZUK_TAG);
    if (urb == nullptr) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    UsbBuildGetDescriptorRequest(urb,
                                 (USHORT)sizeof(struct _URB_CONTROL_DESCRIPTOR_REQUEST),
                                 USB_CONFIGURATION_DESCRIPTOR_TYPE, 0, 0,
                                 nullptr, &dx->ConfigDescriptor, sizeof(USB_CONFIGURATION_DESCRIPTOR), nullptr);
    status = Izk_UsbSendUrbSync(dx, urb);
    ExFreePoolWithTag(urb, IZUK_TAG);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    // Fetch full configuration (all interfaces/alternates).
    urb = (PURB)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(struct _URB_CONTROL_DESCRIPTOR_REQUEST), IZUK_TAG);
    if (urb == nullptr) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    UsbBuildGetDescriptorRequest(urb,
                                 (USHORT)sizeof(struct _URB_CONTROL_DESCRIPTOR_REQUEST),
                                 USB_CONFIGURATION_DESCRIPTOR_TYPE, 0, 0,
                                 nullptr, &dx->ConfigDescriptor, dx->ConfigDescriptor->wTotalLength, nullptr);
    status = Izk_UsbSendUrbSync(dx, urb);
    ExFreePoolWithTag(urb, IZUK_TAG);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    // Select configuration with the audio streaming interface at alternate 0.
    // The original picks explicit (number, alternate) pairs, incl. alternate 1 for
    // streaming (research 01 §6: sub_F102BA80 alt-setting walk).
    RtlZeroMemory(interfaceList, sizeof(interfaceList));
    wanted[0].Number = 1; wanted[0].Alternate = 0;   // audio control / streaming
    wanted[1].Number = 2; wanted[1].Alternate = 0;   // second interface (HID/MIDI present on 2902)

    urb = (PURB)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(struct _URB_SELECT_CONFIGURATION), IZUK_TAG);
    if (urb == nullptr) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    for (i = 0; i < 2; ++i) {
        interfaceList[i].InterfaceDescriptor = USBD_ParseConfigurationDescriptorEx(
            dx->ConfigDescriptor, dx->ConfigDescriptor,
            wanted[i].Number, wanted[i].Alternate, -1, -1, -1);
        if (interfaceList[i].InterfaceDescriptor == nullptr) {
            // PCM2900 has only one interface; tolerate missing second interface.
            interfaceList[i].InterfaceDescriptor = nullptr;
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
        dx->AudioInterfaceNumber = interfaceList[0].InterfaceDescriptor->bInterfaceNumber;
        dx->AudioAlternateSetting = interfaceList[0].InterfaceDescriptor->bAlternateSetting;
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
    UCHAR ep = 0x01;                             // TODO: derive from endpoint descriptor

    urb = (PURB)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(struct _URB_CONTROL_VENDOR_OR_CLASS_REQUEST), IZUK_TAG);
    if (urb == nullptr) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    UsbBuildVendorRequest(urb,
                          (USHORT)sizeof(struct _URB_CONTROL_VENDOR_OR_CLASS_REQUEST),
                          0,                           // flags: out
                          0, 0,
                          UAC_SET_CUR,
                          (USHORT)(UAC_EP_SAM_FREQ_CTRL << 8),
                          (USHORT)((ep & 0x0F) | (dx->AudioInterfaceNumber << 8)),
                          nullptr, nullptr,
                          &rate, nullptr,
                          sizeof(rate));
    status = Izk_UsbSendUrbSync(dx, urb);
    ExFreePoolWithTag(urb, IZUK_TAG);
    if (NT_SUCCESS(status)) {
        dx->SampleRate = sampleRate;
    }
    return status;
}
