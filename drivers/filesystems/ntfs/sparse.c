/*
 * PROJECT:     ReactOS NTFS driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Sparse files: FSCTL_SET_SPARSE, FSCTL_SET_ZERO_DATA, FSCTL_QUERY_ALLOCATED_RANGES
 * COPYRIGHT:   Copyright 2026 ReactOS Team
 */

/*
 * A sparse stream is a non-resident $DATA with NTFS_ATTR_SPARSE: its runs may be holes,
 * which read as zeros, and it's allocated in units of 16 clusters, like Windows does. The
 * file carries FILE_ATTRIBUTE_SPARSE_FILE.
 */

/* INCLUDES *****************************************************************/

#include "ntfs.h"

#define NDEBUG
#include <debug.h>

/* FUNCTIONS ****************************************************************/

/* A file's stream, read for a change: its record, the record holding the stream, and the stream */
typedef struct _NTFS_STREAM_EDIT
{
    PFILE_RECORD_HEADER FileRecord;
    PFILE_RECORD_HEADER Holder;
    PNTFS_ATTR_CONTEXT Context;
    ULONG Offset;
} NTFS_STREAM_EDIT, *PNTFS_STREAM_EDIT;

static
NTSTATUS
NtfsOpenStreamEdit(PDEVICE_EXTENSION Vcb,
                   PNTFS_FCB Fcb,
                   PNTFS_STREAM_EDIT Edit)
{
    NTSTATUS Status;

    RtlZeroMemory(Edit, sizeof(*Edit));

    Edit->FileRecord = ExAllocateFromNPagedLookasideList(&Vcb->FileRecLookasideList);
    if (Edit->FileRecord == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = ReadFileRecord(Vcb, Fcb->MFTIndex, Edit->FileRecord);
    if (NT_SUCCESS(Status))
        Status = FindAttribute(Vcb, Edit->FileRecord, AttributeData, Fcb->Stream, (ULONG)wcslen(Fcb->Stream),
                               &Edit->Context, &Edit->Offset);
    if (NT_SUCCESS(Status))
        Status = NtfsReadAttributeHolder(Vcb, Edit->Context, Edit->FileRecord, &Edit->Holder);

    return Status;
}

static
VOID
NtfsCloseStreamEdit(PDEVICE_EXTENSION Vcb,
                    PNTFS_STREAM_EDIT Edit)
{
    if (Edit->Holder != NULL)
        NtfsReleaseAttributeHolder(Vcb, Edit->FileRecord, Edit->Holder);
    if (Edit->Context != NULL)
        ReleaseAttributeContext(Edit->Context);
    if (Edit->FileRecord != NULL)
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, Edit->FileRecord);
}

/**
* @name NtfsMarkSparse
*
* Sets FILE_ATTRIBUTE_SPARSE_FILE wherever the file keeps its attributes: its
* $STANDARD_INFORMATION, its $FILE_NAMEs and the directory entries for them, and the FCB.
*/
static
NTSTATUS
NtfsMarkSparse(PDEVICE_EXTENSION Vcb,
               PNTFS_FCB Fcb)
{
    PFILE_RECORD_HEADER FileRecord;
    PSTANDARD_INFORMATION StdInfo;
    PFILENAME_ATTRIBUTE FileName;
    NTSTATUS Status;

    FileRecord = ExAllocateFromNPagedLookasideList(&Vcb->FileRecLookasideList);
    if (FileRecord == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = ReadFileRecord(Vcb, Fcb->MFTIndex, FileRecord);
    if (NT_SUCCESS(Status))
    {
        StdInfo = GetStandardInformationFromRecord(Vcb, FileRecord);
        FileName = GetBestFileNameFromRecord(Vcb, FileRecord);
        if (StdInfo == NULL || FileName == NULL)
        {
            Status = STATUS_FILE_CORRUPT_ERROR;
        }
        else
        {
            StdInfo->FileAttribute |= NTFS_FILE_TYPE_SPARSE;
            FileName->FileAttributes |= NTFS_FILE_TYPE_SPARSE;
            Fcb->Entry.FileAttributes |= NTFS_FILE_TYPE_SPARSE;
            Status = NtfsUpdateDuplicatedInformation(Vcb, FileRecord, Fcb->MFTIndex, NTFS_FILENAME_UPDATE_ATTRS, FALSE);
        }
    }

    ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, FileRecord);

    return Status;
}

/**
* @name NtfsSetSparse
* @implemented
*
* FSCTL_SET_SPARSE (MS-FSA 2.1.5.10.31): makes the file's stream sparse. Turning sparseness
* off isn't supported, as on NTFS before Windows Vista.
*/
NTSTATUS
NtfsSetSparse(PDEVICE_EXTENSION Vcb,
              PNTFS_FCB Fcb,
              BOOLEAN SetSparse)
{
    NTFS_STREAM_EDIT Edit;
    LARGE_INTEGER Size;
    NTSTATUS Status;

    if (NtfsFCBIsDirectory(Fcb))
        return STATUS_INVALID_PARAMETER;

    if (!SetSparse)
        return (Fcb->Entry.FileAttributes & NTFS_FILE_TYPE_SPARSE) ? STATUS_NOT_SUPPORTED : STATUS_SUCCESS;

    Status = NtfsOpenStreamEdit(Vcb, Fcb, &Edit);

    /* A sparse stream has runs, so a resident one moves to clusters first */
    if (NT_SUCCESS(Status) && !Edit.Context->pRecord->IsNonResident)
    {
        Size.QuadPart = Edit.Context->pRecord->Resident.ValueLength;
        Status = ConvertResidentAttributeToNonResident(Vcb, Edit.Context, Edit.Offset, Edit.Holder, &Size);
    }

    if (NT_SUCCESS(Status) && !(Edit.Context->pRecord->Flags & NTFS_ATTR_SPARSE))
    {
        if (Edit.Context->pRecord->Flags & NTFS_ATTR_COMPRESSION_MASK)
        {
            Status = STATUS_NOT_IMPLEMENTED;
        }
        else
        {
            Status = NtfsMakeAttributeSparse(Vcb, Edit.Context, Edit.Offset, Edit.Holder);
            if (NT_SUCCESS(Status))
                Status = UpdateFileRecord(Vcb, Edit.Context->FileMFTIndex & NTFS_MFT_MASK, Edit.Holder);
        }
    }

    if (NT_SUCCESS(Status))
        Fcb->RFCB.AllocationSize.QuadPart = Edit.Context->pRecord->NonResident.AllocatedSize;

    NtfsCloseStreamEdit(Vcb, &Edit);

    if (NT_SUCCESS(Status))
        Status = NtfsMarkSparse(Vcb, Fcb);

    return Status;
}

/**
* @name NtfsZeroRange
*
* Writes zeros over [Start, End) of a stream, where it has clusters.
*/
static
NTSTATUS
NtfsZeroRange(PDEVICE_EXTENSION Vcb,
              PNTFS_STREAM_EDIT Edit,
              ULONGLONG Start,
              ULONGLONG End)
{
    PUCHAR Zeros;
    ULONG Length, Written;
    NTSTATUS Status = STATUS_SUCCESS;

    if (Start >= End)
        return STATUS_SUCCESS;

    /* A resident value is written in the record */
    if (!Edit->Context->pRecord->IsNonResident)
    {
        Length = (ULONG)(End - Start);
        Zeros = ExAllocatePoolWithTag(NonPagedPool, Length, TAG_NTFS);
        if (Zeros == NULL)
            return STATUS_INSUFFICIENT_RESOURCES;
        RtlZeroMemory(Zeros, Length);
        Status = WriteAttribute(Vcb, Edit->Context, Start, Zeros, Length, &Written, Edit->Holder);
        ExFreePoolWithTag(Zeros, TAG_NTFS);
        return Status;
    }

    return NtfsZeroAttributeRange(Vcb, Edit->Context, Edit->Holder, Start, End);
}

/**
* @name NtfsSetZeroData
* @implemented
*
* FSCTL_SET_ZERO_DATA (MS-FSA 2.1.5.10.33): [FileOffset, BeyondFinalZero) reads as zeros
* afterwards. A sparse stream gives back the whole units inside the range; whatever else is
* in it is written over with zeros. Nothing past the end of file changes.
*/
NTSTATUS
NtfsSetZeroData(PDEVICE_EXTENSION Vcb,
                PNTFS_FCB Fcb,
                PFILE_ZERO_DATA_INFORMATION Zero)
{
    ULONGLONG Start = Zero->FileOffset.QuadPart;
    ULONGLONG End = Zero->BeyondFinalZero.QuadPart;
    NTFS_STREAM_EDIT Edit;
    NTSTATUS Status;

    if (Zero->FileOffset.QuadPart < 0 || Zero->BeyondFinalZero.QuadPart < Zero->FileOffset.QuadPart)
        return STATUS_INVALID_PARAMETER;
    if (NtfsFCBIsDirectory(Fcb))
        return STATUS_INVALID_PARAMETER;

    Status = NtfsOpenStreamEdit(Vcb, Fcb, &Edit);
    if (NT_SUCCESS(Status))
    {
        ULONGLONG Size = AttributeDataLength(Edit.Context->pRecord);

        End = min(End, Size);
        if (Start < End && NtfsAttributeIsSparse(Edit.Context))
        {
            ULONG UnitBytes = NtfsSparseUnitClusters(Edit.Context) * Vcb->NtfsInfo.BytesPerCluster;
            ULONGLONG FirstUnit = ALIGN_UP_BY(Start, UnitBytes);
            ULONGLONG LastUnit = ALIGN_DOWN_BY(End, UnitBytes);

            /* A range that ends at the end of file takes the unit holding the end too */
            if (End == Size)
                LastUnit = ALIGN_UP_BY(End, UnitBytes);

            if (FirstUnit < LastUnit)
            {
                Status = NtfsDeallocateSparseRange(Vcb, Edit.Context, Edit.Offset, Edit.Holder,
                                                   FirstUnit / Vcb->NtfsInfo.BytesPerCluster,
                                                   (LastUnit - FirstUnit) / Vcb->NtfsInfo.BytesPerCluster);
                if (NT_SUCCESS(Status))
                    Status = UpdateFileRecord(Vcb, Edit.Context->FileMFTIndex & NTFS_MFT_MASK, Edit.Holder);
                if (NT_SUCCESS(Status))
                    Status = NtfsZeroRange(Vcb, &Edit, Start, min(FirstUnit, End));
                if (NT_SUCCESS(Status))
                    Status = NtfsZeroRange(Vcb, &Edit, max(LastUnit, Start), End);
            }
            else
            {
                Status = NtfsZeroRange(Vcb, &Edit, Start, End);
            }
        }
        else if (Start < End)
        {
            Status = NtfsZeroRange(Vcb, &Edit, Start, End);
        }
    }

    NtfsCloseStreamEdit(Vcb, &Edit);

    return Status;
}

/* Where a FSCTL_QUERY_ALLOCATED_RANGES answer goes */
typedef struct _NTFS_RANGE_OUTPUT
{
    PFILE_ALLOCATED_RANGE_BUFFER Ranges;
    ULONG Room;
    ULONG Count;
    BOOLEAN Overflow;
} NTFS_RANGE_OUTPUT, *PNTFS_RANGE_OUTPUT;

/* Adds [Start, End) to the answer, merged with the range before it when they touch */
static
VOID
NtfsAddRange(PNTFS_RANGE_OUTPUT Output,
             ULONGLONG Start,
             ULONGLONG End)
{
    PFILE_ALLOCATED_RANGE_BUFFER Last = Output->Count ? &Output->Ranges[Output->Count - 1] : NULL;

    if (Start >= End)
        return;

    if (Last != NULL && (ULONGLONG)(Last->FileOffset.QuadPart + Last->Length.QuadPart) == Start)
    {
        Last->Length.QuadPart += End - Start;
        return;
    }

    if (Output->Count == Output->Room)
    {
        Output->Overflow = TRUE;
        return;
    }

    Output->Ranges[Output->Count].FileOffset.QuadPart = Start;
    Output->Ranges[Output->Count].Length.QuadPart = End - Start;
    Output->Count++;
}

/**
* @name NtfsQueryAllocatedRanges
* @implemented
*
* FSCTL_QUERY_ALLOCATED_RANGES (MS-FSA 2.1.5.10.22): the parts of [FileOffset, FileOffset +
* Length) below the end of file that have clusters; for a stream that isn't sparse, all of
* it. STATUS_BUFFER_OVERFLOW when the ranges don't all fit.
*/
NTSTATUS
NtfsQueryAllocatedRanges(PDEVICE_EXTENSION Vcb,
                         PNTFS_FCB Fcb,
                         PFILE_ALLOCATED_RANGE_BUFFER Query,
                         PFILE_ALLOCATED_RANGE_BUFFER Ranges,
                         ULONG RangesLength,
                         PULONG Returned)
{
    NTFS_RANGE_OUTPUT Output = {Ranges, RangesLength / sizeof(*Ranges), 0, FALSE};
    ULONGLONG Start = Query->FileOffset.QuadPart;
    ULONGLONG End;
    NTFS_STREAM_EDIT Edit;
    NTSTATUS Status;

    *Returned = 0;

    if (Query->FileOffset.QuadPart < 0 || Query->Length.QuadPart < 0 ||
        Query->FileOffset.QuadPart > MAXLONGLONG - Query->Length.QuadPart)
    {
        return STATUS_INVALID_PARAMETER;
    }
    End = Start + Query->Length.QuadPart;

    Status = NtfsOpenStreamEdit(Vcb, Fcb, &Edit);
    if (NT_SUCCESS(Status))
    {
        End = min(End, AttributeDataLength(Edit.Context->pRecord));

        if (!NtfsAttributeIsSparse(Edit.Context))
        {
            NtfsAddRange(&Output, Start, End);
        }
        else
        {
            ULONG BytesPerCluster = Vcb->NtfsInfo.BytesPerCluster;
            ULONGLONG Offset = ALIGN_DOWN_BY(Start, BytesPerCluster);

            while (Offset < End && !Output.Overflow)
            {
                LONGLONG Lbn, Count;
                ULONGLONG RunEnd;

                if (!FsRtlLookupLargeMcbEntry(&Edit.Context->DataRunsMCB, Offset / BytesPerCluster,
                                              &Lbn, &Count, NULL, NULL, NULL))
                {
                    break;
                }

                RunEnd = Offset + Count * BytesPerCluster;
                if (Lbn != -1)
                    NtfsAddRange(&Output, max(Offset, Start), min(RunEnd, End));
                Offset = RunEnd;
            }
        }
    }

    NtfsCloseStreamEdit(Vcb, &Edit);

    if (!NT_SUCCESS(Status))
        return Status;

    *Returned = Output.Count * sizeof(*Ranges);

    return Output.Overflow ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS;
}

/* EOF */
