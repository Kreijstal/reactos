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

typedef struct _ARM64_SLIST_HEADER
{
    SLIST_ENTRY Next;
    USHORT Depth;
    USHORT Sequence;
} ARM64_SLIST_HEADER, *PARM64_SLIST_HEADER;

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
    PARM64_SLIST_HEADER Header = (PARM64_SLIST_HEADER)SListHead;
    PSLIST_ENTRY OldEntry;

    OldEntry = Header->Next.Next;
    SListEntry->Next = OldEntry;
    Header->Next.Next = SListEntry;
    Header->Depth++;
    Header->Sequence++;
    return OldEntry;
}

PSLIST_ENTRY
NTAPI
RtlInterlockedPopEntrySList(
    _Inout_ PSLIST_HEADER SListHead)
{
    PARM64_SLIST_HEADER Header = (PARM64_SLIST_HEADER)SListHead;
    PSLIST_ENTRY OldEntry;

    OldEntry = Header->Next.Next;
    if (OldEntry == NULL)
        return NULL;

    Header->Next.Next = OldEntry->Next;
    Header->Depth--;
    Header->Sequence++;
    return OldEntry;
}

PSLIST_ENTRY
NTAPI
RtlInterlockedFlushSList(
    _Inout_ PSLIST_HEADER SListHead)
{
    PARM64_SLIST_HEADER Header = (PARM64_SLIST_HEADER)SListHead;
    PSLIST_ENTRY OldEntry;

    OldEntry = Header->Next.Next;
    Header->Next.Next = NULL;
    Header->Depth = 0;
    Header->Sequence++;
    return OldEntry;
}
