/*
 * PROJECT:     ReactOS NT User Mode Library
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Boundary descriptors for private namespaces
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

/* INCLUDES *****************************************************************/

#include <ntdll.h>

#define NDEBUG
#include <debug.h>

/* PRIVATE FUNCTIONS ********************************************************/

static
NTSTATUS
RtlpAddBoundaryEntry(
    _Inout_ POBJECT_BOUNDARY_DESCRIPTOR *BoundaryDescriptor,
    _In_ BOUNDARY_ENTRY_TYPE EntryType,
    _In_reads_bytes_(DataSize) PVOID Data,
    _In_ ULONG DataSize)
{
    POBJECT_BOUNDARY_DESCRIPTOR Old = *BoundaryDescriptor, New;
    POBJECT_BOUNDARY_ENTRY Entry;
    ULONG EntrySize = sizeof(*Entry) + ALIGN_UP_BY(DataSize, sizeof(ULONG));

    if (EntrySize < DataSize || Old->TotalSize + EntrySize < EntrySize)
        return STATUS_INVALID_PARAMETER;

    New = RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY, Old->TotalSize + EntrySize);
    if (!New)
        return STATUS_NO_MEMORY;

    RtlCopyMemory(New, Old, Old->TotalSize);
    Entry = (POBJECT_BOUNDARY_ENTRY)((PUCHAR)New + Old->TotalSize);
    Entry->EntryType = EntryType;
    Entry->EntryDataSize = DataSize;
    RtlCopyMemory(Entry + 1, Data, DataSize);
    New->TotalSize += EntrySize;
    New->Items++;

    RtlFreeHeap(RtlGetProcessHeap(), 0, Old);
    *BoundaryDescriptor = New;
    return STATUS_SUCCESS;
}

/* PUBLIC FUNCTIONS *********************************************************/

POBJECT_BOUNDARY_DESCRIPTOR
NTAPI
RtlCreateBoundaryDescriptor(
    _In_ PCUNICODE_STRING Name,
    _In_ ULONG Flags)
{
    POBJECT_BOUNDARY_DESCRIPTOR Boundary;

    Boundary = RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*Boundary));
    if (!Boundary)
        return NULL;

    Boundary->Version = OBJECT_BOUNDARY_DESCRIPTOR_VERSION;
    Boundary->TotalSize = sizeof(*Boundary);
    Boundary->Flags = Flags;

    if (!NT_SUCCESS(RtlpAddBoundaryEntry(&Boundary, OBNS_Name, Name->Buffer, Name->Length)))
    {
        RtlFreeHeap(RtlGetProcessHeap(), 0, Boundary);
        return NULL;
    }

    return Boundary;
}

VOID
NTAPI
RtlDeleteBoundaryDescriptor(
    _In_ _Post_invalid_ POBJECT_BOUNDARY_DESCRIPTOR BoundaryDescriptor)
{
    RtlFreeHeap(RtlGetProcessHeap(), 0, BoundaryDescriptor);
}

NTSTATUS
NTAPI
RtlAddSIDToBoundaryDescriptor(
    _Inout_ POBJECT_BOUNDARY_DESCRIPTOR *BoundaryDescriptor,
    _In_ PSID RequiredSid)
{
    if (!RtlValidSid(RequiredSid))
        return STATUS_INVALID_PARAMETER;

    return RtlpAddBoundaryEntry(BoundaryDescriptor, OBNS_SID, RequiredSid, RtlLengthSid(RequiredSid));
}

NTSTATUS
NTAPI
RtlAddIntegrityLabelToBoundaryDescriptor(
    _Inout_ POBJECT_BOUNDARY_DESCRIPTOR *BoundaryDescriptor,
    _In_ PSID IntegrityLabel)
{
    static SID_IDENTIFIER_AUTHORITY MandatoryLabelAuthority = {SECURITY_MANDATORY_LABEL_AUTHORITY};
    POBJECT_BOUNDARY_ENTRY Entry;
    ULONG i;

    if (!RtlValidSid(IntegrityLabel) ||
        RtlSubAuthorityCountSid(IntegrityLabel)[0] != 1 ||
        !RtlEqualMemory(RtlIdentifierAuthoritySid(IntegrityLabel), &MandatoryLabelAuthority,
                        sizeof(MandatoryLabelAuthority)))
    {
        return STATUS_INVALID_PARAMETER;
    }

    /* A boundary has at most one integrity label */
    Entry = (POBJECT_BOUNDARY_ENTRY)(*BoundaryDescriptor + 1);
    for (i = 0; i < (*BoundaryDescriptor)->Items; i++)
    {
        if (Entry->EntryType == OBNS_IL)
            return STATUS_INVALID_PARAMETER;
        Entry = (POBJECT_BOUNDARY_ENTRY)((PUCHAR)(Entry + 1) + ALIGN_UP_BY(Entry->EntryDataSize, sizeof(ULONG)));
    }

    return RtlpAddBoundaryEntry(BoundaryDescriptor, OBNS_IL, IntegrityLabel, RtlLengthSid(IntegrityLabel));
}

/* EOF */
