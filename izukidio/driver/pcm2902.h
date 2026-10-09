// pcm2902.h - TI PCM2900/2902 USB codec constants (USB VID_08BB PID_2900/2902).
// USB Audio Class 1.0 device: isochronous audio IN/OUT + optional HID + MIDI.
// Source: docs/research/01-busb2902-sys.md and public PCM2902 datasheet.
#pragma once

#define IZUK_USB_VID          0x08BB   // Texas Instruments
#define IZUK_USB_PID_2900     0x2900
#define IZUK_USB_PID_2902     0x2902

// USB Audio Class requests (control endpoint)
#define UAC_SET_CUR           0x01
#define UAC_GET_CUR           0x02
#define UAC_CS_ENDPOINT        0x02    // entity type: endpoint
#define UAC_EP_SAM_FREQ_CTRL   0x01    // bControlSelector: sampling frequency control

// PCM2902 supports the 44.1 kHz family and 48 kHz family at 16-bit stereo;
// 24-bit is exposed as 3-byte-packed on the wire in some alternates.
#define IZUK_RATE_44100       44100
#define IZUK_RATE_48000       48000

// USB class-specific interfaces inside the PCM2902 configuration
#define IZUK_IFCLASS_AUDIO    0x01
#define IZUK_IFCLASS_HID      0x03
#define IZUK_IFCLASS_MIDI     0x01     // subclass of audio class

// Vendor/class request header used by IOCTL_IZUK_VENDOR_CLASS_REQ
// (original driver DbgPrint names it VENDOR_OR_CLASS_REQUEST; 8-byte header
// followed by variable data, research 01 §4).
typedef struct _IZUK_VENDOR_OR_CLASS_REQUEST {
    ULONG   Flags;          // bit7: direction (0x80 = device-to-host IN)
    ULONG   Reserved;
    // variable payload follows
} IZUK_VENDOR_OR_CLASS_REQUEST, *PIZUK_VENDOR_OR_CLASS_REQUEST;

// 16-byte hardware info blob returned by IOCTL_IZUK_GET_HWINFO (offsets observed
// in the original state area at extension +1682, research 01 §4).
typedef struct _IZUK_HWINFO {
    ULONG   UsbVendorId;
    ULONG   UsbProductId;
    ULONG   ChipRevision;
    ULONG   UsbSpeed;       // 0 = full-speed
} IZUK_HWINFO, *PIZUK_HWINFO;
