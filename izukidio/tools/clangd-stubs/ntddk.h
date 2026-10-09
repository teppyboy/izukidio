// Kernel stub headers for static analysis on non-Windows hosts only.
// NOT compiled on Windows: the real WDK (ntddk.h) is used there.
#pragma once
#include <stdint.h>
#include <stddef.h>

typedef uint8_t UCHAR, *PUCHAR, BOOLEAN, *PBOOLEAN;
typedef uint16_t USHORT, *PUSHORT;
typedef uint32_t ULONG, *PULONG, UINT32, NTSTATUS, DWORD;
typedef int32_t LONG, *PLONG, INT;
typedef uint64_t ULONG64, UINT64, SIZE_T_;
typedef int64_t LONGLONG;
typedef void* PVOID, *LPVOID;
typedef const void* PCVOID;
typedef unsigned char KIRQL;
typedef unsigned long ULONG_PTR, *PULONG_PTR;
typedef long LONG_PTR, *PLONG_PTR;
typedef struct _DEVICE_OBJECT* PDEVICE_OBJECT;
typedef struct _IRP* PIRP;
typedef struct _DRIVER_OBJECT* PDRIVER_OBJECT;
typedef struct _KTHREAD* PKTHREAD;
typedef PVOID HANDLE, *PHANDLE;
typedef wchar_t WCHAR, *PWSTR;
typedef const wchar_t* PCWSTR;

#define __analysis_noreturn

typedef struct _UNICODE_STRING { USHORT Length; USHORT MaximumLength; PWSTR Buffer; } UNICODE_STRING, *PUNICODE_STRING;
typedef signed char CHAR, *PCHAR;
typedef void (*DRIVER_INITIALIZE)(PDRIVER_OBJECT, PUNICODE_STRING);
typedef struct _KEVENT { ULONG dummy; } KEVENT, *PKEVENT;
typedef struct _KSEMAPHORE { ULONG dummy; } KSEMAPHORE, *PKSEMAPHORE;
typedef struct _KSPIN_LOCK { ULONG dummy; } KSPIN_LOCK, *PKSPIN_LOCK;
typedef struct _KTIMER { ULONG dummy; } KTIMER, *PKTIMER;
typedef struct _KDPC { ULONG dummy; } KDPC, *PKDPC;
typedef struct _IO_REMOVE_LOCK { ULONG dummy; } IO_REMOVE_LOCK, *PIO_REMOVE_LOCK;
typedef struct _OBJECT_ATTRIBUTES { ULONG dummy; } OBJECT_ATTRIBUTES, *POBJECT_ATTRIBUTES;
typedef union _LARGE_INTEGER { LONGLONG QuadPart; } LARGE_INTEGER, *PLARGE_INTEGER;
typedef struct _MDL { ULONG dummy; } MDL, *PMDL;
typedef struct _IO_STATUS_BLOCK { union { LONG Status; PVOID Pointer; }; ULONG Information; } IO_STATUS_BLOCK, *PIO_STATUS_BLOCK;

// Minimal kernel API surface used by the PoC driver (signatures only).
extern "C" {
NTSTATUS IoCreateDevice(PDRIVER_OBJECT, ULONG, PUNICODE_STRING, ULONG, ULONG, BOOLEAN, PDEVICE_OBJECT*);
void IoDeleteDevice(PDEVICE_OBJECT);
PDEVICE_OBJECT IoAttachDeviceToDeviceStack(PDEVICE_OBJECT, PDEVICE_OBJECT);
void IoDetachDevice(PDEVICE_OBJECT);
void IoCompleteRequest(PIRP, signed char);
NTSTATUS IoCallDriver(PDEVICE_OBJECT, PIRP);
PIRP IoAllocateIrp(unsigned char, BOOLEAN);
void IoFreeIrp(PIRP);
NTSTATUS IoCreateSymbolicLink(PUNICODE_STRING, PUNICODE_STRING);
void IoDeleteSymbolicLink(PUNICODE_STRING);
NTSTATUS IoRegisterDeviceInterface(PDEVICE_OBJECT, const unsigned char*, PUNICODE_STRING, PUNICODE_STRING);
void IoSetDeviceInterfaceState(PUNICODE_STRING, BOOLEAN);
void IoInvalidateDeviceRelations(PDEVICE_OBJECT, int);
PIRP IoBuildDeviceIoControlRequest(ULONG, PDEVICE_OBJECT, PVOID, ULONG, PVOID, ULONG, BOOLEAN, PKEVENT, PIO_STATUS_BLOCK);
void KeInitializeEvent(PKEVENT, int, BOOLEAN);
LONG KeSetEvent(PKEVENT, LONG, BOOLEAN);
LONG KeWaitForSingleObject(PVOID, int, KIRQL, BOOLEAN, PLARGE_INTEGER);
void KeDelayExecutionThread(KIRQL, BOOLEAN, PLARGE_INTEGER);
KIRQL KeGetCurrentIrql();
NTSTATUS PsCreateSystemThread(PHANDLE, ULONG, POBJECT_ATTRIBUTES, HANDLE, PULONG, PVOID, PVOID);
NTSTATUS PsTerminateSystemThread(NTSTATUS);
PVOID ExAllocatePoolWithTag(ULONG, SIZE_T_, ULONG);
void ExFreePoolWithTag(PVOID, ULONG);
void RtlInitUnicodeString(PUNICODE_STRING, PCWSTR);
void RtlFreeUnicodeString(PUNICODE_STRING);
NTSTATUS RtlAppendUnicodeToString(PUNICODE_STRING, PCWSTR);
NTSTATUS RtlUnicodeStringToAnsiString(PVOID, PUNICODE_STRING, BOOLEAN);
LONG RtlCompareUnicodeString(PUNICODE_STRING, PUNICODE_STRING, BOOLEAN);
NTSTATUS RtlIntegerToUnicodeString(ULONG, ULONG, PUNICODE_STRING);
NTSTATUS RtlQueryRegistryValues(ULONG, PCWSTR, PVOID, PVOID, PVOID);
NTSTATUS RtlWriteRegistryValue(ULONG, PCWSTR, PCWSTR, ULONG, PVOID, ULONG);
ULONG DbgPrint(const char*, ...);
void ObReferenceObjectByHandle(HANDLE, ULONG, PVOID, KIRQL, PVOID*, PVOID);
void ObfDereferenceObject(PVOID);
PVOID MmGetSystemAddressForMdlSafe(PMDL, ULONG);
PVOID MmMapLockedPagesSpecifyCache(PMDL, int, int, PVOID, ULONG, int);
PMDL IoAllocateMdl(PVOID, ULONG, BOOLEAN, BOOLEAN, PIRP);
void IoFreeMdl(PMDL);
void MmBuildMdlForNonPagedPool(PMDL);
void KeInitializeSpinLock(PKSPIN_LOCK);
KIRQL KeAcquireSpinLockRaiseToDpc(PKSPIN_LOCK);
void KeReleaseSpinLock(PKSPIN_LOCK, KIRQL);
void KeInitializeTimerEx(PKTIMER, int);
BOOLEAN KeSetTimerEx(PKTIMER, LARGE_INTEGER, LONG, PKDPC);
BOOLEAN KeCancelTimer(PKTIMER);
void KeInitializeDpc(PKDPC, PVOID, PVOID);
}
