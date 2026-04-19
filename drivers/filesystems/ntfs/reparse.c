/*
 * COPYRIGHT:        See COPYING in the top level directory
 * PROJECT:          ReactOS kernel
 * FILE:             drivers/filesystem/ntfs/reparse.c
 * PURPOSE:          NTFS filesystem driver -- per-file $REPARSE_POINT round-trip
 *                   workers (set / get / delete).
 *
 * The on-disk $REPARSE_POINT attribute stores a 4-byte reparse tag, a 2-byte
 * reserved field (matches REPARSE_DATA_BUFFER's Reserved), a 2-byte data
 * length, and the variable-length body -- matching the REPARSE_POINT_ATTRIBUTE
 * struct in ntfs.h.  This slice only supports resident $REPARSE_POINT
 * attributes; non-resident is rejected with STATUS_NOT_IMPLEMENTED so callers
 * see a stable fallback instead of silent corruption.
 *
 * FRH flags:  NTFS_FILE_TYPE_REPARSE (0x400) in $STANDARD_INFORMATION tracks
 * "this file has a reparse point".  We set/clear it in lockstep with the
 * attribute so NtfsFCBIsReparsePoint stays consistent.
 */

/* INCLUDES *****************************************************************/

#include "ntfs.h"

#define NDEBUG
#include <debug.h>

/* FUNCTIONS ****************************************************************/

/* Toggle NTFS_FILE_TYPE_REPARSE in the $STANDARD_INFORMATION FileAttribute
 * field of @p FileRecord to match the new "has reparse point" state. */
static VOID
ReparseSyncFlagInStandardInfo(PDEVICE_EXTENSION Vcb,
                              PFILE_RECORD_HEADER FileRecord,
                              BOOLEAN HasReparse)
{
    PSTANDARD_INFORMATION Si = GetStandardInformationFromRecord(Vcb, FileRecord);

    if (Si == NULL)
        return;

    if (HasReparse)
        Si->FileAttribute |= NTFS_FILE_TYPE_REPARSE;
    else
        Si->FileAttribute &= ~NTFS_FILE_TYPE_REPARSE;
}

NTSTATUS
NtfsSetReparsePointOnRecord(PNTFS_VCB Vcb,
                            PFILE_RECORD_HEADER FileRecord,
                            ULONG Tag,
                            const VOID *Data,
                            ULONG DataLength)
{
    NTSTATUS Status;
    PNTFS_ATTR_CONTEXT AttrCtx = NULL;
    ULONG Needed;
    PREPARSE_POINT_ATTRIBUTE Blob = NULL;

    if (DataLength > 0xFFFFU)
        return STATUS_IO_REPARSE_DATA_INVALID;

    /* Build the on-disk serialisation. */
    Needed = FIELD_OFFSET(REPARSE_POINT_ATTRIBUTE, Data) + DataLength;
    Blob = ExAllocatePoolWithTag(NonPagedPool, Needed, TAG_NTFS);
    if (Blob == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;
    Blob->ReparseTag = Tag;
    Blob->DataLength = (USHORT)DataLength;
    Blob->Reserved = 0;
    if (DataLength != 0 && Data != NULL)
        RtlCopyMemory(Blob->Data, Data, DataLength);

    Status = FindAttribute(Vcb, FileRecord, AttributeReparsePoint,
                           L"", 0, &AttrCtx, NULL);
    if (NT_SUCCESS(Status))
    {
        ULONGLONG ExistingLength = AttributeDataLength(AttrCtx->pRecord);
        ULONG Written = 0;

        if ((ULONG)ExistingLength != Needed)
        {
            /* Resizing a resident $REPARSE_POINT requires InternalSetResidentAttributeLength
             * + slot-packing; delete-then-set is the simple path. */
            ReleaseAttributeContext(AttrCtx);
            Status = NtfsDeleteReparsePointFromRecord(Vcb, FileRecord);
            if (!NT_SUCCESS(Status))
                goto Cleanup;

            Status = AddResidentAttribute(Vcb, FileRecord, AttributeReparsePoint,
                                          L"", 0, Blob, Needed);
        }
        else
        {
            Status = WriteAttribute(Vcb, AttrCtx, 0, (PUCHAR)Blob, Needed,
                                    &Written, FileRecord);
            ReleaseAttributeContext(AttrCtx);
        }
    }
    else
    {
        if (Needed > Vcb->NtfsInfo.BytesPerFileRecord)
        {
            Status = STATUS_NOT_IMPLEMENTED;
            goto Cleanup;
        }
        Status = AddResidentAttribute(Vcb, FileRecord, AttributeReparsePoint,
                                      L"", 0, Blob, Needed);
    }

    if (NT_SUCCESS(Status))
        ReparseSyncFlagInStandardInfo(Vcb, FileRecord, TRUE);

Cleanup:
    if (Blob != NULL)
        ExFreePoolWithTag(Blob, TAG_NTFS);
    return Status;
}

NTSTATUS
NtfsGetReparsePointFromRecord(PNTFS_VCB Vcb,
                              PFILE_RECORD_HEADER FileRecord,
                              PULONG TagOut,
                              PVOID Buffer,
                              ULONG BufferLength,
                              PULONG LengthOut)
{
    NTSTATUS Status;
    PNTFS_ATTR_CONTEXT AttrCtx = NULL;
    ULONGLONG AttrLen;
    PREPARSE_POINT_ATTRIBUTE Blob = NULL;
    ULONG BlobLen;

    if (LengthOut != NULL)
        *LengthOut = 0;

    Status = FindAttribute(Vcb, FileRecord, AttributeReparsePoint,
                           L"", 0, &AttrCtx, NULL);
    if (!NT_SUCCESS(Status))
        return STATUS_NOT_A_REPARSE_POINT;

    AttrLen = AttributeDataLength(AttrCtx->pRecord);
    if (AttrLen < FIELD_OFFSET(REPARSE_POINT_ATTRIBUTE, Data) ||
        AttrLen > MAXULONG)
    {
        ReleaseAttributeContext(AttrCtx);
        return STATUS_IO_REPARSE_DATA_INVALID;
    }

    BlobLen = (ULONG)AttrLen;
    Blob = ExAllocatePoolWithTag(NonPagedPool, BlobLen, TAG_NTFS);
    if (Blob == NULL)
    {
        ReleaseAttributeContext(AttrCtx);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    ReadAttribute(Vcb, AttrCtx, 0, (PCHAR)Blob, BlobLen);
    ReleaseAttributeContext(AttrCtx);

    if (TagOut != NULL)
        *TagOut = Blob->ReparseTag;

    if (LengthOut != NULL)
        *LengthOut = Blob->DataLength;

    if (BufferLength < Blob->DataLength)
    {
        Status = STATUS_BUFFER_TOO_SMALL;
        goto Done;
    }

    if (Buffer != NULL && Blob->DataLength != 0)
        RtlCopyMemory(Buffer, Blob->Data, Blob->DataLength);

    Status = STATUS_SUCCESS;

Done:
    ExFreePoolWithTag(Blob, TAG_NTFS);
    return Status;
}

NTSTATUS
NtfsDeleteReparsePointFromRecord(PNTFS_VCB Vcb,
                                 PFILE_RECORD_HEADER FileRecord)
{
    NTSTATUS Status;
    PNTFS_ATTR_CONTEXT AttrCtx = NULL;
    ULONG AttrOffset = 0;
    PNTFS_ATTR_RECORD Slot;

    Status = FindAttribute(Vcb, FileRecord, AttributeReparsePoint,
                           L"", 0, &AttrCtx, &AttrOffset);
    if (!NT_SUCCESS(Status))
        return STATUS_NOT_A_REPARSE_POINT;

    if (AttrCtx->pRecord->IsNonResident || AttrCtx->MigratedToMFTIndex != 0)
    {
        ReleaseAttributeContext(AttrCtx);
        return STATUS_NOT_IMPLEMENTED;
    }

    Slot = (PNTFS_ATTR_RECORD)((ULONG_PTR)FileRecord + AttrOffset);
    Status = RemoveResidentAttribute(Vcb, FileRecord, Slot);
    ReleaseAttributeContext(AttrCtx);

    if (NT_SUCCESS(Status))
        ReparseSyncFlagInStandardInfo(Vcb, FileRecord, FALSE);
    return Status;
}

/* EOF */
