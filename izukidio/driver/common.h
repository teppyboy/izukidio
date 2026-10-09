// common.h - shared declarations for the Ploytec2902 reimplementation driver.
// Reimplementation of Ploytec GmbH "usb-audio.de" driver for Behringer USB audio
// devices based on the TI PCM2900/2902 codec (USB VID_08BB, PID_2900/2902).
#pragma once

#define POOL_NX_OPTIN 1
#include <ntddk.h>
#define NTSTRSAFE_LIB
#include <ntstrsafe.h>
#include <usb.h>
#include <usbdlib.h>

#define P2902_TAG '229P'   // pool tag 'P292'

#define DEVICE_NAME_USBB L"\\Device\\BUSB2902"
#define DOSDEVICE_NAME_USBB L"\\DosDevices\\BUSB2902"
// Original driver also exposes a symbolic name the ASIO DLL opens (see protocol.h
// for the interface it uses). Kept as a legacy-style NT device name.

#define P2902_MAX_ENDPOINTS 4

// Per-endpoint isochronous stream state
typedef struct _P2902_ISO_ENDPOINT {
    PUSBD_PIPE_INFORMATION  Pipe;               // from interface descriptor
    ULONG                   MaxPacketSize;
    ULONG                   FramesPerUrb;       // ISO packets per URB
    ULONG                   BytesPerFrame;      // packed frame size (channels * bytes * align)
    BOOLEAN                 Inbound;            // capture (IN) vs render (OUT)
    BOOLEAN                 Active;
    KEVENT                  StopEvent;
    PIRP                    PendingIrp;
} P2902_ISO_ENDPOINT, *PP2902_ISO_ENDPOINT;

typedef struct _P2902_DEVICE_EXTENSION {
    PDEVICE_OBJECT  LowerDevice;        // USB PDO
    PDEVICE_OBJECT  Pdo;
    USBD_HANDLE     UsbdHandle;
    PUSB_DEVICE_DESCRIPTOR  DeviceDescriptor;
    PUSB_CONFIGURATION_DESCRIPTOR   ConfigDescriptor;
    PUSBD_INTERFACE_INFORMATION Interface;      // selected alt setting
    USBD_PIPE_HANDLE    IsoInPipe;              // PCM2902 ADC isoch IN
    USBD_PIPE_HANDLE    IsoOutPipe;             // PCM2902 DAC isoch OUT
    ULONG               PipeMaxPacketIn;
    ULONG               PipeMaxPacketOut;
    ULONG               SampleRate;             // 48000, 44100, ...
    ULONG               BytesPerSample;         // 2 (16-bit) / 3 (24-bit padded)
    ULONG               ChannelsIn;
    ULONG               ChannelsOut;
    LONG                ReferenceClock;         // feedback endpoint handling
    P2902_ISO_ENDPOINT  Endpoint[P2902_MAX_ENDPOINTS];
    PDEVICE_OBJECT      ControlDevice;          // IOCTL-facing CDO for ASIO DLL
    UNICODE_STRING      ControlSymLink;
    KEVENT              RemoveEvent;
    IO_REMOVE_LOCK      RemoveLock;
    BOOLEAN             Streaming;
} P2902_DEVICE_EXTENSION, *PP2902_DEVICE_EXTENSION;

extern "C" {

DRIVER_INITIALIZE DriverEntry;

NTSTATUS P2902_AddDevice(PDRIVER_OBJECT DriverObject, PDEVICE_OBJECT PhysicalDeviceObject);
NTSTATUS P2902_DispatchPnp(PDEVICE_OBJECT DeviceObject, __in PIRP Irp);
NTSTATUS P2902_DispatchPower(PDEVICE_OBJECT DeviceObject, __in PIRP Irp);
NTSTATUS P2902_DispatchSystemControl(PDEVICE_OBJECT DeviceObject, __in PIRP Irp);
NTSTATUS P2902_DispatchDeviceControl(PDEVICE_OBJECT DeviceObject, __in PIRP Irp);

NTSTATUS P2902_UsbConfigure(PP2902_DEVICE_EXTENSION dx);
NTSTATUS P2902_UsbUnconfigure(PP2902_DEVICE_EXTENSION dx);
NTSTATUS P2902_SelectAlternateInterface(PP2902_DEVICE_EXTENSION dx, __in UCHAR altSetting);
NTSTATUS P2902_SetSampleRate(PP2902_DEVICE_EXTENSION dx, __in ULONG sampleRate);

NTSTATUS P2902_IsochStart(PP2902_DEVICE_EXTENSION dx, BOOLEAN inbound, __in ULONG bufferSizeBytes);
NTSTATUS P2902_IsochStop(PP2902_DEVICE_EXTENSION dx, BOOLEAN inbound);

} // extern "C"
