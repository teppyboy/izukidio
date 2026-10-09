// protocol.h - user/kernel contract of the original driver, preserved so the
// original busbasio_x64.dll ASIO DLL can talk to izukidio.sys unchanged.
// Source: docs/research/03-busbasio-dll.md (DLL side) and 01-busb2902-sys.md (kernel side).
#pragma once

// Device interface GUID the ASIO DLL opens via SetupDiGetClassDevs.
// {090E2CEE-44C0-4263-8837-786AA85A49C6}
DEFINE_GUID(IZUK_DEVICE_INTERFACE,
    0x090E2CEE, 0x44C0, 0x4263, 0x88, 0x37, 0x78, 0x6A, 0xA8, 0x5A, 0x49, 0xC6);

// Sub-open names inside the device (FileObject->FileName in IRP_MJ_CREATE):
//   "\IO"      - streaming data plane
//   "\CONTROL" - control/config plane
//   "\FW_UPDATE" - firmware update path (original feature; may return not-supported)

// All original user-mode IOCTLs:
//   CTL_CODE(FILE_DEVICE_UNKNOWN /*0x22*/, fn, FILE_ANY_ACCESS, METHOD_BUFFERED)
//   = 0x220000 | (fn << 2)
#define IZUK_IOCTL(code) CTL_CODE(FILE_DEVICE_UNKNOWN, (code) / 4, FILE_ANY_ACCESS, METHOD_BUFFERED)

#define IOCTL_IZUK_GET_NAME_A        0x220000   // OUT: <=255 B USB string (product name)
#define IOCTL_IZUK_GET_NAME_B        0x220004   // OUT: <=255 B USB string (manufacturer)
#define IOCTL_IZUK_VENDOR_CLASS_REQ  0x220008   // IN: VENDOR_OR_CLASS_REQUEST hdr + data
#define IOCTL_IZUK_GET_STATE         0x22000C   // OUT: 98584 B full device state dump
#define IOCTL_IZUK_COMMIT_CONFIG     0x220014   // IN:  76 B config struct (sample rate, period, format)
#define IOCTL_IZUK_SET_PARAM         0x220018   // IN:  4 B parameter (min 1) -> stream unit
#define IOCTL_IZUK_STOP_CYCLE        0x22001C   // stop + IOCTL_INTERNAL_USB_CYCLE_PORT re-enum
#define IOCTL_IZUK_SET_PARAM_BLOCK   0x220024   // IN:  24 B parameter block
#define IOCTL_IZUK_FILE_STATUS       0x220030   // OUT: 16 B per-\IO-handle status block
#define IOCTL_IZUK_GET_SAMPLE_CLOCK  0x220038   // OUT: 8 B 64-bit sample clock (latched)
#define IOCTL_IZUK_GET_VERSION       0x22004C   // OUT: 4 B = 0x0CE5 (3301, magic shared with busbwdm)
#define IOCTL_IZUK_SET_ROUTING_IN    0x220064   // IN:  3844 B routing/matrix table (input side)
#define IOCTL_IZUK_SET_ROUTING_OUT   0x220068   // IN:  3844 B routing/matrix table (output side)
#define IOCTL_IZUK_CLOSE_FILE        0x22006C   // release per-\IO-handle stream slot
#define IOCTL_IZUK_GET_CAP           0x220070   // OUT: 4 B capability word
#define IOCTL_IZUK_INIT_ONCE         0x220094   // one-time init flag (idempotent)
#define IOCTL_IZUK_GET_HWINFO        0x220098   // OUT: 16 B hardware info blob
#define IOCTL_IZUK_GET_STATE_BYTE    0x22009C   // OUT: 4 B state byte
#define IOCTL_IZUK_SET_FILE_READY    0x2200A0   // IN:  4 B -> per-file m_bIsFileReady
#define IOCTL_IZUK_GET_BUILD         0x2200A4   // OUT: 4 B = 0x0207A800 (build 2.8.40)
#define IOCTL_IZUK_REGISTER_CLIENT   0x2200B0   // IN:  4 B client PID (on \IO handle)
#define IOCTL_IZUK_GET_POSITION_IN   0x2200B4   // OUT: 4 B input sample position
#define IOCTL_IZUK_GET_POSITION_OUT  0x2200B8   // OUT: 4 B output sample position
#define IOCTL_IZUK_SET_CLOCK64       0x2200BC   // IN:  8 B 64-bit clock value
#define IOCTL_IZUK_READ_STREAM       0x2200C0   // IN/OUT: arbitrary-length PCM read (capture)
#define IOCTL_IZUK_WRITE_STREAM      0x2200C4   // IN: arbitrary-length PCM write (render)
#define IOCTL_IZUK_GET_STREAMINFO    0x2200CC   // OUT: 64 B stream info
#define IOCTL_IZUK_PROPERTY_CTRL     0x2200D0   // IN/OUT: 260 B property get/set dispatcher
#define IOCTL_IZUK_PROPERTY_FILE     0x2200D4   // IN/OUT: 260 B property with \IO file object
#define IOCTL_IZUK_INVALIDATE_REL    0x2200E0   // re-enumerate bus children (BusRelations)

// Sizes enforced by the original dispatch:
#define IZUK_STATE_DUMP_SIZE       98584
#define IZUK_CONFIG_STRUCT_SIZE       76
#define IZUK_ROUTING_TABLE_SIZE     3844
#define IZUK_PROPERTY_STRUCT_SIZE    260
#define IZUK_STREAMINFO_SIZE           64
#define IZUK_HWINFO_SIZE               16
#define IZUK_FILE_STATUS_SIZE          16
#define IZUK_MAGIC_VERSION        0x0CE5
#define IZUK_MAGIC_BUILD       0x0207A800
