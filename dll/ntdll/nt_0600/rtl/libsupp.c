/*
 * PROJECT:     ReactOS System Libraries
 * LICENSE:     GPL-2.0-or-later (see COPYING)
 * PURPOSE:     Allocation support for the standalone Vista RTL library
 */

#include "../ntdll_vista.h"

PVOID
NTAPI
RtlpAllocateMemory(SIZE_T Bytes, ULONG Tag)
{
    UNREFERENCED_PARAMETER(Tag);
    return RtlAllocateHeap(RtlGetProcessHeap(), 0, Bytes);
}

VOID
NTAPI
RtlpFreeMemory(PVOID Memory, ULONG Tag)
{
    UNREFERENCED_PARAMETER(Tag);
    RtlFreeHeap(RtlGetProcessHeap(), 0, Memory);
}
