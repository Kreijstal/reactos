/*
 * PROJECT:     ReactOS Kernel Cryptography Next Generation Provider
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Driver entry points, initialization and common helpers
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

/* INCLUDES *******************************************************************/

#include "cngp.h"

#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

#define CNG_INIT_NONE       0
#define CNG_INIT_RUNNING    1
#define CNG_INIT_DONE       2

static volatile LONG CngInitState = CNG_INIT_NONE;

/* FUNCTIONS ******************************************************************/

/*
 * cng.sys can be started as a boot driver (DriverEntry), or be loaded as an
 * import of another driver (DllInitialize). Either can come first, and an
 * importer may also call in before our DriverEntry ran, so every entry point
 * that needs SymCrypt goes through here. Nothing in here touches paged memory,
 * so it is safe at DISPATCH_LEVEL.
 */
VOID
CngEnsureInitialized(VOID)
{
    if (InterlockedCompareExchange(&CngInitState, CNG_INIT_DONE, CNG_INIT_DONE) == CNG_INIT_DONE)
        return;

    if (InterlockedCompareExchange(&CngInitState, CNG_INIT_RUNNING, CNG_INIT_NONE) == CNG_INIT_NONE)
    {
        SymCryptInit();
        CngInitializeRng();
        InterlockedExchange(&CngInitState, CNG_INIT_DONE);
        return;
    }

    /* Someone else is initializing */
    while (InterlockedCompareExchange(&CngInitState, CNG_INIT_DONE, CNG_INIT_DONE) != CNG_INIT_DONE)
    {
        YieldProcessor();
    }
}

/*
 * Allocates a zeroed object from non-paged pool, aligned for SymCrypt.
 * Pool blocks are only 8-byte aligned on x86.
 */
PVOID
CngAllocateObject(
    _In_ SIZE_T Size,
    _In_ ULONG Tag,
    _Out_ PVOID *Allocation)
{
    PVOID Block;
    PVOID Object;

    *Allocation = NULL;

    if (Size > MAXULONG - CNG_ALIGN_SLACK)
        return NULL;

    Block = ExAllocatePoolWithTag(NonPagedPool, Size + CNG_ALIGN_SLACK, Tag);
    if (!Block)
        return NULL;

    Object = ALIGN_UP_POINTER_BY(Block, CNG_ALIGNMENT);
    RtlZeroMemory(Object, Size);
    *Allocation = Block;
    return Object;
}

VOID
CngFreeObject(
    _In_ PVOID Allocation,
    _In_ ULONG Tag)
{
    ExFreePoolWithTag(Allocation, Tag);
}

/*
 * Compares a property value with a string. Callers usually pass the size
 * including the terminating NUL, but a value without it is accepted as well.
 */
BOOLEAN
CngIsString(
    _In_reads_bytes_(Size) PUCHAR Buffer,
    _In_ ULONG Size,
    _In_ PCWSTR String)
{
    PCWSTR Value = (PCWSTR)Buffer;
    SIZE_T Length = wcslen(String);
    SIZE_T Count = Size / sizeof(WCHAR);

    if (!Buffer || Count < Length)
        return FALSE;

    if (_wcsnicmp(Value, String, Length) != 0)
        return FALSE;

    return (Count == Length || Value[Length] == UNICODE_NULL);
}

NTSTATUS
CngCopyProperty(
    _In_reads_bytes_(Length) const VOID *Value,
    _In_ ULONG Length,
    _Out_writes_bytes_to_opt_(OutputLength, *ResultLength) PUCHAR Output,
    _In_ ULONG OutputLength,
    _Out_ ULONG *ResultLength)
{
    *ResultLength = Length;

    if (!Output)
        return STATUS_SUCCESS;

    if (OutputLength < Length)
        return STATUS_BUFFER_TOO_SMALL;

    RtlCopyMemory(Output, Value, Length);
    return STATUS_SUCCESS;
}

/* SymCrypt only uses these for asymmetric algorithms, which we do not expose */
PVOID
SYMCRYPT_CALL
SymCryptCallbackAlloc(
    SIZE_T Size)
{
    PVOID Block;
    PVOID *Object;

    if (Size > MAXULONG - SYMCRYPT_ASYM_ALIGN_VALUE - sizeof(PVOID))
        return NULL;

    Block = ExAllocatePoolWithTag(NonPagedPool,
                                  Size + SYMCRYPT_ASYM_ALIGN_VALUE + sizeof(PVOID),
                                  TAG_CNG_BUFFER);
    if (!Block)
        return NULL;

    Object = ALIGN_UP_POINTER_BY((PUCHAR)Block + sizeof(PVOID), SYMCRYPT_ASYM_ALIGN_VALUE);
    Object[-1] = Block;
    return Object;
}

VOID
SYMCRYPT_CALL
SymCryptCallbackFree(
    PVOID Pointer)
{
    if (Pointer)
        ExFreePoolWithTag(((PVOID *)Pointer)[-1], TAG_CNG_BUFFER);
}

NTSTATUS
NTAPI
DllInitialize(
    _In_ PUNICODE_STRING RegistryPath)
{
    UNREFERENCED_PARAMETER(RegistryPath);

    CngEnsureInitialized();
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    UNREFERENCED_PARAMETER(DriverObject);
    UNREFERENCED_PARAMETER(RegistryPath);

    CngEnsureInitialized();
    DPRINT("CNG provider initialized\n");
    return STATUS_SUCCESS;
}
