/*
 * PROJECT:     ReactOS Run-Time Library
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     ARM64 RTL stubs
 */

#include <rtl.h>

#define NDEBUG
#include <debug.h>

typedef
_Function_class_(GET_RUNTIME_FUNCTION_CALLBACK)
PRUNTIME_FUNCTION
GET_RUNTIME_FUNCTION_CALLBACK(
    _In_ DWORD64 ControlPc,
    _In_opt_ PVOID Context);
typedef GET_RUNTIME_FUNCTION_CALLBACK *PGET_RUNTIME_FUNCTION_CALLBACK;

static
PSLIST_ENTRY
RtlpArm64FirstEntrySList(
    _In_ const SLIST_HEADER *SListHead)
{
    union
    {
        ULONG64 Region;
        struct
        {
            ULONG64 Reserved:4;
            ULONG64 NextEntry:39;
            ULONG64 Reserved2:21;
        } Bits;
    } Pointer;

    if (SListHead->Header16.HeaderType)
        return (PVOID)(SListHead->Region & ~0xFULL);

    if (SListHead->Header8.NextEntry == 0)
        return NULL;

    Pointer.Region = (ULONG64)SListHead;
    Pointer.Bits.NextEntry = SListHead->Header8.NextEntry;
    return (PVOID)Pointer.Region;
}

static
VOID
RtlpArm64SetNextEntrySList(
    _Inout_ SLIST_HEADER *SListHead,
    _In_opt_ PSLIST_ENTRY NextEntry)
{
    if (SListHead->Header16.HeaderType)
    {
        SListHead->Region = (ULONG64)NextEntry;
        return;
    }

    SListHead->Header8.NextEntry = (ULONG64)NextEntry >> 4;
}

PLIST_ENTRY
NTAPI
RtlGetFunctionTableListHead(VOID)
{
    return NULL;
}

BOOLEAN
NTAPI
RtlInstallFunctionTableCallback(
    _In_ ULONG_PTR TableIdentifier,
    _In_ ULONG_PTR BaseAddress,
    _In_ ULONG Length,
    _In_ PGET_RUNTIME_FUNCTION_CALLBACK Callback,
    _In_ PVOID Context,
    _In_opt_z_ PCWSTR OutOfProcessCallbackDll)
{
    (VOID)TableIdentifier;
    (VOID)BaseAddress;
    (VOID)Length;
    (VOID)Callback;
    (VOID)Context;
    (VOID)OutOfProcessCallbackDll;
    return FALSE;
}

PSLIST_ENTRY
NTAPI
RtlInterlockedPushEntrySList(
    _Inout_ PSLIST_HEADER SListHead,
    _Inout_ __drv_aliasesMem PSLIST_ENTRY SListEntry)
{
    PSLIST_ENTRY OldEntry;

    OldEntry = RtlpArm64FirstEntrySList(SListHead);
    SListEntry->Next = OldEntry;
    RtlpArm64SetNextEntrySList(SListHead, SListEntry);
    SListHead->Header8.Depth++;
    SListHead->Header8.Sequence++;
    return OldEntry;
}

PSLIST_ENTRY
NTAPI
RtlInterlockedPopEntrySList(
    _Inout_ PSLIST_HEADER SListHead)
{
    PSLIST_ENTRY OldEntry;

    OldEntry = RtlpArm64FirstEntrySList(SListHead);
    if (OldEntry == NULL)
        return NULL;

    RtlpArm64SetNextEntrySList(SListHead, OldEntry->Next);
    SListHead->Header8.Depth--;
    SListHead->Header8.Sequence++;
    return OldEntry;
}

PSLIST_ENTRY
NTAPI
RtlInterlockedFlushSList(
    _Inout_ PSLIST_HEADER SListHead)
{
    PSLIST_ENTRY OldEntry;

    OldEntry = RtlpArm64FirstEntrySList(SListHead);
    RtlpArm64SetNextEntrySList(SListHead, NULL);
    SListHead->Header8.Depth = 0;
    SListHead->Header8.Sequence++;
    return OldEntry;
}
