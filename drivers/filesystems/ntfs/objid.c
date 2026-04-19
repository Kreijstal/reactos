/*
 * COPYRIGHT:        See COPYING in the top level directory
 * PROJECT:          ReactOS kernel
 * FILE:             drivers/filesystem/ntfs/objid.c
 * PURPOSE:          NTFS filesystem driver -- per-file $OBJECT_ID round-trip
 *                   workers (set / get / delete).
 *
 * On-disk layout: a resident $OBJECT_ID attribute storing 16 bytes (a GUID).
 * Extended owning-volume / original / domain IDs (the full 64-byte form used
 * on later NTFS) are NOT materialised yet; this slice is the core 16-byte
 * form only.  \$Extend\$ObjId:$O (the SID->file-ref lookup index) is a
 * separate follow-up slice -- this worker targets the per-file attribute
 * only.
 */

/* INCLUDES *****************************************************************/

#include "ntfs.h"

#define NDEBUG
#include <debug.h>

/* FUNCTIONS ****************************************************************/

NTSTATUS
NtfsSetObjectIdOnRecord(PNTFS_VCB Vcb,
                        PFILE_RECORD_HEADER FileRecord,
                        const UCHAR ObjectId[16])
{
    NTSTATUS Status;
    PNTFS_ATTR_CONTEXT AttrCtx = NULL;

    if (ObjectId == NULL)
        return STATUS_INVALID_PARAMETER;

    Status = FindAttribute(Vcb, FileRecord, AttributeObjectId,
                           L"", 0, &AttrCtx, NULL);
    if (NT_SUCCESS(Status))
    {
        ULONGLONG ExistingLen = AttributeDataLength(AttrCtx->pRecord);
        ULONG Written = 0;

        if (ExistingLen != 16)
        {
            /* Non-16-byte form (extended) -- out of scope; rewrite wholesale
             * by deleting and re-adding the 16-byte form. */
            ReleaseAttributeContext(AttrCtx);
            Status = NtfsDeleteObjectIdFromRecord(Vcb, FileRecord);
            if (!NT_SUCCESS(Status))
                return Status;
            return AddResidentAttribute(Vcb, FileRecord, AttributeObjectId,
                                        L"", 0, ObjectId, 16);
        }

        Status = WriteAttribute(Vcb, AttrCtx, 0, (PUCHAR)ObjectId, 16,
                                &Written, FileRecord);
        ReleaseAttributeContext(AttrCtx);
        return Status;
    }

    return AddResidentAttribute(Vcb, FileRecord, AttributeObjectId,
                                L"", 0, ObjectId, 16);
}

NTSTATUS
NtfsGetObjectIdFromRecord(PNTFS_VCB Vcb,
                          PFILE_RECORD_HEADER FileRecord,
                          UCHAR ObjectIdOut[16])
{
    NTSTATUS Status;
    PNTFS_ATTR_CONTEXT AttrCtx = NULL;
    ULONGLONG AttrLen;

    if (ObjectIdOut == NULL)
        return STATUS_INVALID_PARAMETER;

    Status = FindAttribute(Vcb, FileRecord, AttributeObjectId,
                           L"", 0, &AttrCtx, NULL);
    if (!NT_SUCCESS(Status))
        return STATUS_OBJECTID_NOT_FOUND;

    AttrLen = AttributeDataLength(AttrCtx->pRecord);
    if (AttrLen < 16)
    {
        ReleaseAttributeContext(AttrCtx);
        return STATUS_OBJECTID_NOT_FOUND;
    }

    ReadAttribute(Vcb, AttrCtx, 0, (PCHAR)ObjectIdOut, 16);
    ReleaseAttributeContext(AttrCtx);
    return STATUS_SUCCESS;
}

NTSTATUS
NtfsDeleteObjectIdFromRecord(PNTFS_VCB Vcb,
                             PFILE_RECORD_HEADER FileRecord)
{
    NTSTATUS Status;
    PNTFS_ATTR_CONTEXT AttrCtx = NULL;
    ULONG AttrOffset = 0;
    PNTFS_ATTR_RECORD Slot;

    Status = FindAttribute(Vcb, FileRecord, AttributeObjectId,
                           L"", 0, &AttrCtx, &AttrOffset);
    if (!NT_SUCCESS(Status))
        return STATUS_OBJECTID_NOT_FOUND;

    if (AttrCtx->pRecord->IsNonResident || AttrCtx->MigratedToMFTIndex != 0)
    {
        ReleaseAttributeContext(AttrCtx);
        return STATUS_NOT_IMPLEMENTED;
    }

    Slot = (PNTFS_ATTR_RECORD)((ULONG_PTR)FileRecord + AttrOffset);
    Status = RemoveResidentAttribute(Vcb, FileRecord, Slot);
    ReleaseAttributeContext(AttrCtx);
    return Status;
}

/* EOF */
