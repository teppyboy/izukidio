// Stub of usbdlib.h for static analysis on non-Windows hosts only.
#pragma once
#include "usb.h"
typedef PVOID USBD_HANDLE, USBD_PIPE_HANDLE, *PUSBD_INTERFACE_LIST_ENTRY_stub;
typedef struct _USBD_VERSION_INFORMATION { ULONG USBDI_Version; ULONG Supported_USB_Version; } USBD_VERSION_INFORMATION, *PUSBD_VERSION_INFORMATION;
