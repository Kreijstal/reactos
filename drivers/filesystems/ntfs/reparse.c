/*
 * PROJECT:     ReactOS NTFS driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Reparse points: FSCTL_SET/GET/DELETE_REPARSE_POINT
 * COPYRIGHT:   Copyright 2026 ReactOS Team
 */

/*
 * A reparse point is a $REPARSE_POINT attribute holding the reparse buffer the way
 * FSCTL_SET_REPARSE_POINT received it: a REPARSE_DATA_BUFFER for a Microsoft tag, a
 * REPARSE_GUID_DATA_BUFFER (with its GUID) for any other. The file also carries
 * FILE_ATTRIBUTE_REPARSE_POINT, and its $FILE_NAMEs the tag, where directory listings
 * report it.
 */

/* INCLUDES *****************************************************************/

#include "ntfs.h"

#define NDEBUG
#include <debug.h>

/* FUNCTIONS ****************************************************************/

/* Size of the reparse buffer header that goes with a tag */
static
ULONG
NtfsReparseHeaderSize(ULONG Tag)
{
    return IsReparseTagMicrosoft(Tag) ? REPARSE_DATA_BUFFER_HEADER_SIZE : REPARSE_GUID_DATA_BUFFER_HEADER_SIZE;
}

/**
* @name NtfsValidateReparseBuffer
*
* Checks a reparse buffer handed to FSCTL_SET_REPARSE_POINT (MS-FSCC 2.1.2): the length
* must be the header's plus ReparseDataLength, and the tag not a reserved one.
*/
static
NTSTATUS
NtfsValidateReparseBuffer(PREPARSE_DATA_BUFFER Buffer,
                          ULONG Length)
{
    if (Length < REPARSE_DATA_BUFFER_HEADER_SIZE || Length > MAXIMUM_REPARSE_DATA_BUFFER_SIZE)
        return STATUS_IO_REPARSE_DATA_INVALID;

    if (Length < NtfsReparseHeaderSize(Buffer->ReparseTag) ||
        Length != NtfsReparseHeaderSize(Buffer->ReparseTag) + Buffer->ReparseDataLength)
    {
        return STATUS_IO_REPARSE_DATA_INVALID;
    }

    if (Buffer->ReparseTag == IO_REPARSE_TAG_RESERVED_ZERO ||
        Buffer->ReparseTag == IO_REPARSE_TAG_RESERVED_ONE)
    {
        return STATUS_IO_REPARSE_TAG_INVALID;
    }

    return STATUS_SUCCESS;
}

/**
* @name NtfsMatchReparsePoint
*
* A request to change or delete a reparse point has to name it: the same tag, and for a
* non-Microsoft tag the same GUID.
*/
static
NTSTATUS
NtfsMatchReparsePoint(PREPARSE_DATA_BUFFER Existing,
                      PREPARSE_DATA_BUFFER Request)
{
    if (Existing->ReparseTag != Request->ReparseTag)
        return STATUS_IO_REPARSE_TAG_MISMATCH;

    if (!IsReparseTagMicrosoft(Request->ReparseTag) &&
        !IsEqualGUID(&((PREPARSE_GUID_DATA_BUFFER)Existing)->ReparseGuid,
                     &((PREPARSE_GUID_DATA_BUFFER)Request)->ReparseGuid))
    {
        return STATUS_REPARSE_ATTRIBUTE_CONFLICT;
    }

    return STATUS_SUCCESS;
}

/**
* @name NtfsReadReparsePoint
*
* Reads the reparse buffer of a file into a pool buffer the caller frees.
*
* @return
* STATUS_SUCCESS, STATUS_NOT_A_REPARSE_POINT for a file that isn't one, or an error.
*/
static
NTSTATUS
NtfsReadReparsePoint(PDEVICE_EXTENSION DeviceExt,
                     PFILE_RECORD_HEADER FileRecord,
                     PREPARSE_DATA_BUFFER *Buffer,
                     PULONG Length)
{
    PNTFS_ATTR_CONTEXT AttrContext;
    ULONGLONG AttrLength;
    NTSTATUS Status;

    *Buffer = NULL;
    *Length = 0;

    Status = FindAttribute(DeviceExt, FileRecord, AttributeReparsePoint, L"", 0, &AttrContext, NULL);
    if (!NT_SUCCESS(Status))
        return STATUS_NOT_A_REPARSE_POINT;

    AttrLength = AttributeDataLength(AttrContext->pRecord);
    if (AttrLength < REPARSE_DATA_BUFFER_HEADER_SIZE || AttrLength > MAXIMUM_REPARSE_DATA_BUFFER_SIZE)
    {
        ReleaseAttributeContext(AttrContext);
        return STATUS_IO_REPARSE_DATA_INVALID;
    }

    *Buffer = ExAllocatePoolWithTag(NonPagedPool, (ULONG)AttrLength, TAG_NTFS);
    if (*Buffer == NULL)
    {
        ReleaseAttributeContext(AttrContext);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    if (ReadAttribute(DeviceExt, AttrContext, 0, (PCHAR)*Buffer, (ULONG)AttrLength) != AttrLength)
    {
        ExFreePoolWithTag(*Buffer, TAG_NTFS);
        *Buffer = NULL;
        ReleaseAttributeContext(AttrContext);
        return STATUS_FILE_CORRUPT_ERROR;
    }

    ReleaseAttributeContext(AttrContext);
    *Length = (ULONG)AttrLength;

    return STATUS_SUCCESS;
}

/**
* @name NtfsGetReparseTag
*
* The reparse tag of a file, 0 if it has none.
*/
ULONG
NtfsGetReparseTag(PDEVICE_EXTENSION DeviceExt,
                  PFILE_RECORD_HEADER FileRecord)
{
    PREPARSE_DATA_BUFFER Buffer;
    ULONG Length, Tag;

    if (!NT_SUCCESS(NtfsReadReparsePoint(DeviceExt, FileRecord, &Buffer, &Length)))
        return 0;

    Tag = Buffer->ReparseTag;
    ExFreePoolWithTag(Buffer, TAG_NTFS);

    return Tag;
}

/**
* @name NtfsMarkReparsePoint
*
* Sets or clears FILE_ATTRIBUTE_REPARSE_POINT and the tag wherever the file keeps them:
* $STANDARD_INFORMATION, its $FILE_NAME and the directory entry for it, and the FCB.
* Writes the base record.
*/
static
NTSTATUS
NtfsMarkReparsePoint(PDEVICE_EXTENSION DeviceExt,
                     PNTFS_FCB Fcb,
                     PFILE_RECORD_HEADER FileRecord,
                     ULONG Tag)
{
    PSTANDARD_INFORMATION StdInfo;
    PFILENAME_ATTRIBUTE FileName;

    StdInfo = GetStandardInformationFromRecord(DeviceExt, FileRecord);
    FileName = GetBestFileNameFromRecord(DeviceExt, FileRecord);
    if (StdInfo == NULL || FileName == NULL)
        return STATUS_FILE_CORRUPT_ERROR;

    if (Tag != 0)
    {
        StdInfo->FileAttribute |= NTFS_FILE_TYPE_REPARSE;
        Fcb->Entry.FileAttributes |= NTFS_FILE_TYPE_REPARSE;
    }
    else
    {
        StdInfo->FileAttribute &= ~NTFS_FILE_TYPE_REPARSE;
        Fcb->Entry.FileAttributes &= ~NTFS_FILE_TYPE_REPARSE;
    }

    FileName->Extended.ReparseTag = Tag;
    Fcb->Entry.Extended.ReparseTag = Tag;

    return NtfsUpdateDuplicatedInformation(DeviceExt, FileRecord, Fcb->MFTIndex, NTFS_FILENAME_UPDATE_ATTRS, FALSE);
}

/**
* @name NtfsWriteReparsePoint
* @implemented
*
* Makes a file a reparse point, or replaces the data of the one it is (MS-FSA
* 2.1.5.10.36). A mount point goes only on an empty directory. An existing reparse point
* keeps its tag (STATUS_IO_REPARSE_TAG_MISMATCH otherwise) and, for a non-Microsoft tag,
* its GUID (STATUS_REPARSE_ATTRIBUTE_CONFLICT).
*
* @param Buffer
* The reparse buffer FSCTL_SET_REPARSE_POINT received, Length bytes long.
*/
NTSTATUS
NtfsWriteReparsePoint(PDEVICE_EXTENSION DeviceExt,
                      PNTFS_FCB Fcb,
                      PREPARSE_DATA_BUFFER Buffer,
                      ULONG Length)
{
    PREPARSE_DATA_BUFFER Existing = NULL;
    PFILE_RECORD_HEADER FileRecord;
    ULONG ExistingLength;
    NTSTATUS Status;

    Status = NtfsValidateReparseBuffer(Buffer, Length);
    if (!NT_SUCCESS(Status))
        return Status;

    if (Buffer->ReparseTag == IO_REPARSE_TAG_MOUNT_POINT)
    {
        BOOLEAN Empty;

        if (!NtfsFCBIsDirectory(Fcb))
            return STATUS_NOT_A_DIRECTORY;

        KeEnterCriticalRegion();
        ExAcquireResourceSharedLite(&DeviceExt->IndexResource, TRUE);
        Status = NtfsIsDirectoryEmpty(DeviceExt, Fcb->MFTIndex, &Empty);
        ExReleaseResourceLite(&DeviceExt->IndexResource);
        KeLeaveCriticalRegion();

        if (NT_SUCCESS(Status) && !Empty)
            Status = STATUS_DIRECTORY_NOT_EMPTY;
        if (!NT_SUCCESS(Status))
            return Status;
    }

    FileRecord = ExAllocateFromNPagedLookasideList(&DeviceExt->FileRecLookasideList);
    if (FileRecord == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = ReadFileRecord(DeviceExt, Fcb->MFTIndex, FileRecord);
    if (NT_SUCCESS(Status))
    {
        Status = NtfsReadReparsePoint(DeviceExt, FileRecord, &Existing, &ExistingLength);
        if (Status == STATUS_NOT_A_REPARSE_POINT)
            Status = STATUS_SUCCESS;
    }

    if (NT_SUCCESS(Status) && Existing != NULL)
        Status = NtfsMatchReparsePoint(Existing, Buffer);

    if (NT_SUCCESS(Status))
        Status = NtfsSetAttributeValue(DeviceExt, FileRecord, AttributeReparsePoint, NULL, 0, Buffer, Length);

    if (NT_SUCCESS(Status))
        Status = NtfsMarkReparsePoint(DeviceExt, Fcb, FileRecord, Buffer->ReparseTag);

    if (Existing != NULL)
        ExFreePoolWithTag(Existing, TAG_NTFS);
    ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);

    return Status;
}

/**
* @name NtfsQueryReparsePoint
* @implemented
*
* Copies the reparse buffer of a file into Output (MS-FSA 2.1.5.10.18). An Output that
* holds only the header gets it with STATUS_BUFFER_OVERFLOW; one too small for the header
* is STATUS_BUFFER_TOO_SMALL.
*
* @param Copied
* Receives the bytes copied.
*/
NTSTATUS
NtfsQueryReparsePoint(PDEVICE_EXTENSION DeviceExt,
                      PNTFS_FCB Fcb,
                      PVOID Output,
                      ULONG OutputLength,
                      PULONG Copied)
{
    PFILE_RECORD_HEADER FileRecord;
    PREPARSE_DATA_BUFFER Buffer;
    ULONG Length;
    NTSTATUS Status;

    *Copied = 0;

    FileRecord = ExAllocateFromNPagedLookasideList(&DeviceExt->FileRecLookasideList);
    if (FileRecord == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = ReadFileRecord(DeviceExt, Fcb->MFTIndex, FileRecord);
    if (NT_SUCCESS(Status))
        Status = NtfsReadReparsePoint(DeviceExt, FileRecord, &Buffer, &Length);

    ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);

    if (!NT_SUCCESS(Status))
        return Status;

    if (OutputLength < NtfsReparseHeaderSize(Buffer->ReparseTag))
    {
        Status = STATUS_BUFFER_TOO_SMALL;
    }
    else
    {
        *Copied = min(Length, OutputLength);
        RtlCopyMemory(Output, Buffer, *Copied);
        Status = (OutputLength < Length) ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS;
    }

    ExFreePoolWithTag(Buffer, TAG_NTFS);

    return Status;
}

/**
* @name NtfsRemoveReparsePoint
* @implemented
*
* Makes a reparse point an ordinary file or directory again (MS-FSA 2.1.5.10.8). Request
* names the tag, and for a non-Microsoft tag the GUID, of the reparse point it deletes, and
* carries no data.
*/
NTSTATUS
NtfsRemoveReparsePoint(PDEVICE_EXTENSION DeviceExt,
                       PNTFS_FCB Fcb,
                       PREPARSE_DATA_BUFFER Request,
                       ULONG Length)
{
    PREPARSE_DATA_BUFFER Existing = NULL;
    PFILE_RECORD_HEADER FileRecord;
    ULONG ExistingLength;
    NTSTATUS Status;

    /* The request carries no data. A Microsoft tag may come in either header,
     * as Windows takes a REPARSE_GUID_DATA_BUFFER header for any tag (Wine's
     * test_reparse_points deletes a mount point that way); a non-Microsoft tag
     * needs the GUID one. */
    if (Request == NULL || Length < REPARSE_DATA_BUFFER_HEADER_SIZE ||
        (Length != NtfsReparseHeaderSize(Request->ReparseTag) &&
         Length != REPARSE_GUID_DATA_BUFFER_HEADER_SIZE) ||
        Request->ReparseDataLength != 0)
    {
        return STATUS_IO_REPARSE_DATA_INVALID;
    }

    FileRecord = ExAllocateFromNPagedLookasideList(&DeviceExt->FileRecLookasideList);
    if (FileRecord == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = ReadFileRecord(DeviceExt, Fcb->MFTIndex, FileRecord);
    if (NT_SUCCESS(Status))
        Status = NtfsReadReparsePoint(DeviceExt, FileRecord, &Existing, &ExistingLength);

    if (NT_SUCCESS(Status))
        Status = NtfsMatchReparsePoint(Existing, Request);

    if (NT_SUCCESS(Status))
        Status = NtfsDeleteAttributeFromFile(DeviceExt, FileRecord, AttributeReparsePoint, L"", 0);

    if (NT_SUCCESS(Status))
        Status = NtfsMarkReparsePoint(DeviceExt, Fcb, FileRecord, 0);

    if (Existing != NULL)
        ExFreePoolWithTag(Existing, TAG_NTFS);
    ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);

    return Status;
}

/* EOF */
