// common.h - izukidio: experimental reimplementation of the Behringer USB Audio
// 2.8.40 driver (Ploytec busb2902.sys/busbasio stack) for Windows 10/11.
//
// Research source: docs/research/00-overview.md, 01-busb2902-sys.md, 03-busbasio-dll.md
#pragma once

#include <ntddk.h>
#define NTSTRSAFE_LIB
#include <ntstrsafe.h>
#include <usb.h>
#include <usbdlib.h>

#include "protocol.h"
#include "pcm2902.h"

#define IZUK_TAG 'KUZI'   // pool tag 'IZUK'

// Legacy-compatible device types used by the original driver (research 01 §2).
// We keep 0x8002 for the FDO so DeviceIoControl validation matches the original.
#define IZUK_DEVICE_TYPE_FDO   0x8002

#define IZUK_NT_DEVNAME   L"\\Device\\IZUKIDIO"
#define IZUK_DOS_DEVNAME  L"\\DosDevices\\IZUKIDIO"

#define IZUK_MAX_ISO_URBS      4
#define IZUK_ISO_PACKETS_PER_URB 8          // USB full-speed: 1 ms frames; pack 8 ms per URB
#define IZUK_MAX_CHANNELS      8

// One isochronous endpoint engine (IN = capture, OUT = render).
typedef struct _IZUK_ISO_ENDPOINT {
    USBD_PIPE_HANDLE    PipeHandle;
    ULONG               MaxPacketSize;
    ULONG               BytesPerFrame;      // packed audio bytes per USB frame
    ULONG               FramesPerUrb;       // ISO packets per URB
    BOOLEAN             Inbound;
    BOOLEAN             Active;
    PIRP                UrbIrp[IZUK_MAX_ISO_URBS];
    PURB                Urb[IZUK_MAX_ISO_URBS];
    PMDL                Mdl[IZUK_MAX_ISO_URBS];
    PUCHAR              TransferBuffer[IZUK_MAX_ISO_URBS];
    KSPIN_LOCK          Lock;
    LONG                PendingUrbCount;
    ULONG               ErrorCount;
    LONG64              FramesTransferred64;    // drives the 64-bit sample clock
    KEVENT              StopEvent;
} IZUK_ISO_ENDPOINT, *PIZUK_ISO_ENDPOINT;

typedef struct _IZUK_DEVICE_EXTENSION {
    PDEVICE_OBJECT  Self;                   // our FDO
    PDEVICE_OBJECT  LowerDevice;            // USB PDO
    PDEVICE_OBJECT  Pdo;
    USBD_HANDLE     UsbdHandle;
    PUSB_DEVICE_DESCRIPTOR          DeviceDescriptor;
    PUSB_CONFIGURATION_DESCRIPTOR   ConfigDescriptor;
    PUSBD_INTERFACE_INFORMATION     InterfaceInfo;      // selected audio alt setting
    UCHAR           AudioInterfaceNumber;
    UCHAR           AudioAlternateSetting;

    USBD_PIPE_HANDLE    IsoInPipe;          // PCM2902 ADC isoch IN endpoint
    USBD_PIPE_HANDLE    IsoOutPipe;         // PCM2902 DAC isoch OUT endpoint
    USBD_PIPE_HANDLE    FeedbackPipe;       // optional explicit-feedback IN endpoint
    ULONG               MaxPacketIn;
    ULONG               MaxPacketOut;

    ULONG               SampleRate;         // 44100 / 48000
    ULONG               StreamUnitParam;    // IOCTL 0x220018 value, clamped >= 1
    UCHAR               RoutingIn[IZUK_ROUTING_TABLE_SIZE];    // 0x220064
    UCHAR               RoutingOut[IZUK_ROUTING_TABLE_SIZE];   // 0x220068
    ULONG               BytesPerSample;     // 2 or 3 (24-bit padded in 4-byte slots)
    ULONG               ChannelsIn;
    ULONG               ChannelsOut;

    // Streaming state shared with the ASIO DLL via IOCTLs (research 03 §3)
    LARGE_INTEGER       SampleClock;        // 64-bit sample position (IOCTL 0x2200BC/0x220038)
    LONG                StreamingClients;   // registered PIDs (IOCTL 0x2200B0)
    KEVENT              ClientEvent;        // completion event signaled per buffer period
    PKEVENT             AsioEvent;          // referenced user event (shared-area tail)
    PKTHREAD            AsioThread;         // referenced ASIO feeder thread
    PVOID               AsioSharedVa;       // kernel mapping of the shared area (engine A at +0)
    BOOLEAN             TransportActive;

    IZUK_ISO_ENDPOINT   In;                 // capture engine
    IZUK_ISO_ENDPOINT   Out;                // render engine

    IO_REMOVE_LOCK      RemoveLock;
    BOOLEAN             ConfigFailed;       // USB configuration failed at start
    UNICODE_STRING      InterfaceSymbolicLink; // {090E2CEE-...} interface
    UNICODE_STRING      DosSymLink;         // \DosDevices\IZUKIDIO
    UNICODE_STRING      NtNameBuffer;       // \Device\IZUKIDIO
} IZUK_DEVICE_EXTENSION, *PIZUK_DEVICE_EXTENSION;

// Per-open context (FsContext). Registered \IO clients own the ASIO
// shared area (research 03 §3).
typedef struct _IZUK_FILE_CONTEXT {
    LONG    OpenKind;           // IZUK_OPEN_* (device.cpp)
    LONG    Slot;               // stream slot index (original: per-file +84)
    BOOLEAN FileReady;          // original m_bIsFileReady (IOCTL 0x2200A0)
    ULONG   ClientPid;          // registered via IOCTL 0x2200B0
    ULONG   Position;           // per-handle position latch
    PMDL    SharedMdl;          // locked ASIO shared area (0x220030)
    PVOID   SharedKernelVa;     // kernel mapping of SharedMdl
} IZUK_FILE_CONTEXT, *PIZUK_FILE_CONTEXT;

// device.cpp shared-area registration (IOCTL 0x220030, research 03 §3).
void     Izk_SharedAreaUnregister(PIZUK_DEVICE_EXTENSION dx, PIZUK_FILE_CONTEXT ctx);
NTSTATUS Izk_SharedAreaRegister(PIZUK_DEVICE_EXTENSION dx, PIZUK_FILE_CONTEXT ctx,
                                BOOLEAN registerArea, PVOID userVa);

extern "C" {

DRIVER_INITIALIZE DriverEntry;
DRIVER_UNLOAD     IzkDriverUnload;

// driver.cpp
NTSTATUS Izk_AddDevice(PDRIVER_OBJECT DriverObject, PDEVICE_OBJECT PhysicalDeviceObject);

// device.cpp
NTSTATUS Izk_DispatchCreateClose(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTSTATUS Izk_DispatchDeviceControl(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTSTATUS Izk_DispatchInternalDeviceControl(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTSTATUS Izk_DispatchPnp(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTSTATUS Izk_DispatchPower(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTSTATUS Izk_DispatchSystemControl(PDEVICE_OBJECT DeviceObject, PIRP Irp);

// usb.cpp
NTSTATUS Izk_UsbConfigure(PIZUK_DEVICE_EXTENSION dx);
void     Izk_UsbUnconfigure(PIZUK_DEVICE_EXTENSION dx);
NTSTATUS Izk_UsbSelectAlternate(PIZUK_DEVICE_EXTENSION dx, UCHAR alternateSetting);
NTSTATUS Izk_UsbSetSampleRate(PIZUK_DEVICE_EXTENSION dx, ULONG sampleRate);
NTSTATUS Izk_UsbSendUrbSync(PIZUK_DEVICE_EXTENSION dx, PURB Urb);
NTSTATUS Izk_UsbCyclePort(PIZUK_DEVICE_EXTENSION dx);
NTSTATUS Izk_UsbVendorClassRequest(PIZUK_DEVICE_EXTENSION dx,
                                   struct _IZUK_VENDOR_OR_CLASS_REQUEST* req,
                                   PUCHAR payload, ULONG payloadLen);

// isoch.cpp
NTSTATUS Izk_IsoStart(PIZUK_DEVICE_EXTENSION dx, BOOLEAN inbound);
NTSTATUS Izk_IsoStop(PIZUK_DEVICE_EXTENSION dx, BOOLEAN inbound);

} // extern "C"
