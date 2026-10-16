/*
 * PROJECT:     ReactOS NTFS driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Object IDs: the $OBJECT_ID attribute and the $O index of $Extend\$ObjId
 * COPYRIGHT:   Copyright 2026 ReactOS Team
 */

/*
 * A file's object ID is a 16-byte $OBJECT_ID attribute. Every object ID on the volume is
 * also a key of the $O index of $Extend\$ObjId, collated as ULONGs; the entry's data is
 * the file reference and the extended information (birth volume ID, birth object ID and
 * domain ID). The index is what keeps an ID unique on the volume, and what an open by
 * object ID looks the file up in.
 */

/* INCLUDES *****************************************************************/

#include "ntfs.h"

#define NDEBUG
#include <debug.h>

/* GLOBALS *****************************************************************/

#define NTFS_OBJECT_ID_LENGTH 16

/* The data of a $O entry */
typedef struct _NTFS_OBJECT_ID_DATA
{
    ULONGLONG FileReference;
    UCHAR ExtendedInfo[48];
} NTFS_OBJECT_ID_DATA, *PNTFS_OBJECT_ID_DATA;

C_ASSERT(sizeof(((PFILE_OBJECTID_BUFFER)NULL)->ExtendedInfo) == sizeof(((PNTFS_OBJECT_ID_DATA)NULL)->ExtendedInfo));

/* FUNCTIONS ****************************************************************/

/**
* @name NtfsFindObjectIdIndex
* @implemented
*
* Finds $Extend\$ObjId at mount. A volume without one (NTFS before 3.0) has no object IDs.
*/
VOID
NtfsFindObjectIdIndex(PDEVICE_EXTENSION DeviceExt)
{
    UNICODE_STRING Name = RTL_CONSTANT_STRING(L"$ObjId");
    ULONG FirstEntry = 0;
    NTSTATUS Status;

    Status = NtfsFindMftRecord(DeviceExt, NTFS_FILE_EXTEND, &Name, &FirstEntry, FALSE, FALSE,
                               &DeviceExt->ObjectIdIndexMftIndex, NULL);
    if (!NT_SUCCESS(Status))
        DeviceExt->ObjectIdIndexMftIndex = 0;
}

/**
* @name NtfsReadObjectId
*
* The object ID a file record carries, STATUS_OBJECTID_NOT_FOUND if it has none.
*/
static
NTSTATUS
NtfsReadObjectId(PDEVICE_EXTENSION DeviceExt,
                 PFILE_RECORD_HEADER FileRecord,
                 PUCHAR ObjectId)
{
    PNTFS_ATTR_CONTEXT AttrContext;
    NTSTATUS Status;

    Status = FindAttribute(DeviceExt, FileRecord, AttributeObjectId, L"", 0, &AttrContext, NULL);
    if (!NT_SUCCESS(Status))
        return STATUS_OBJECTID_NOT_FOUND;

    /* Older NTFS kept the extended information in the attribute too; the ID comes first */
    if (AttributeDataLength(AttrContext->pRecord) < NTFS_OBJECT_ID_LENGTH ||
        ReadAttribute(DeviceExt, AttrContext, 0, (PCHAR)ObjectId, NTFS_OBJECT_ID_LENGTH) != NTFS_OBJECT_ID_LENGTH)
    {
        Status = STATUS_FILE_CORRUPT_ERROR;
    }

    ReleaseAttributeContext(AttrContext);

    return Status;
}

/**
* @name NtfsLookupObjectId
*
* Finds an object ID in $O and returns the data of its entry.
*/
static
NTSTATUS
NtfsLookupObjectId(PDEVICE_EXTENSION DeviceExt,
                   PUCHAR ObjectId,
                   PNTFS_OBJECT_ID_DATA Data)
{
    PINDEX_ENTRY_ATTRIBUTE SearchEntry, Found;
    NTSTATUS Status;

    SearchEntry = NtfsAllocateViewIndexEntry(ObjectId, NTFS_OBJECT_ID_LENGTH, NULL, 0);
    if (SearchEntry == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = NtfsFindIndexEntry(DeviceExt, DeviceExt->ObjectIdIndexMftIndex, L"$O", 2, SearchEntry, &Found);
    ExFreePoolWithTag(SearchEntry, TAG_NTFS);

    if (Status == STATUS_OBJECT_NAME_NOT_FOUND)
        return STATUS_OBJECTID_NOT_FOUND;
    if (!NT_SUCCESS(Status))
        return Status;

    if (Found->Data.ViewIndex.DataLength < sizeof(NTFS_OBJECT_ID_DATA))
        Status = STATUS_FILE_CORRUPT_ERROR;
    else
        RtlCopyMemory(Data, (PUCHAR)Found + Found->Data.ViewIndex.DataOffset, sizeof(NTFS_OBJECT_ID_DATA));

    ExFreePoolWithTag(Found, TAG_NTFS);

    return Status;
}

/**
* @name NtfsIndexObjectId
*
* Adds an object ID to $O. An ID some file already has is STATUS_DUPLICATE_NAME.
*/
static
NTSTATUS
NtfsIndexObjectId(PDEVICE_EXTENSION DeviceExt,
                  PUCHAR ObjectId,
                  PNTFS_OBJECT_ID_DATA Data)
{
    PINDEX_ENTRY_ATTRIBUTE Entry;
    NTSTATUS Status;

    Entry = NtfsAllocateViewIndexEntry(ObjectId, NTFS_OBJECT_ID_LENGTH, Data, sizeof(*Data));
    if (Entry == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = NtfsInsertIndexEntry(DeviceExt, DeviceExt->ObjectIdIndexMftIndex, L"$O", 2, Entry, FALSE);
    ExFreePoolWithTag(Entry, TAG_NTFS);

    return (Status == STATUS_OBJECT_NAME_COLLISION) ? STATUS_DUPLICATE_NAME : Status;
}

/**
* @name NtfsUnindexObjectId
*
* Takes an object ID out of $O.
*/
static
NTSTATUS
NtfsUnindexObjectId(PDEVICE_EXTENSION DeviceExt,
                    PUCHAR ObjectId)
{
    PINDEX_ENTRY_ATTRIBUTE Entry;
    NTSTATUS Status;

    Entry = NtfsAllocateViewIndexEntry(ObjectId, NTFS_OBJECT_ID_LENGTH, NULL, 0);
    if (Entry == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = NtfsRemoveIndexEntry(DeviceExt, DeviceExt->ObjectIdIndexMftIndex, L"$O", 2, Entry, FALSE);
    ExFreePoolWithTag(Entry, TAG_NTFS);

    return Status;
}

/**
* @name NtfsSetObjectId
* @implemented
*
* Gives a file without an object ID the one Buffer holds, with its extended information:
* indexes it, then stores it in the file. STATUS_OBJECT_NAME_COLLISION if the file has an
* ID already, STATUS_DUPLICATE_NAME if another file has this one.
*/
NTSTATUS
NtfsSetObjectId(PDEVICE_EXTENSION DeviceExt,
                 PNTFS_FCB Fcb,
                 PFILE_RECORD_HEADER FileRecord,
                 PFILE_OBJECTID_BUFFER Buffer)
{
    UCHAR Existing[NTFS_OBJECT_ID_LENGTH];
    NTFS_OBJECT_ID_DATA Data;
    NTSTATUS Status;

    Status = NtfsReadObjectId(DeviceExt, FileRecord, Existing);
    if (NT_SUCCESS(Status))
        return STATUS_OBJECT_NAME_COLLISION;
    if (Status != STATUS_OBJECTID_NOT_FOUND)
        return Status;

    Data.FileReference = Fcb->MFTIndex | ((ULONGLONG)FileRecord->SequenceNumber << 48);
    RtlCopyMemory(Data.ExtendedInfo, Buffer->ExtendedInfo, sizeof(Data.ExtendedInfo));

    Status = NtfsIndexObjectId(DeviceExt, Buffer->ObjectId, &Data);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = NtfsSetAttributeValue(DeviceExt, FileRecord, AttributeObjectId, NULL, 0,
                                   Buffer->ObjectId, NTFS_OBJECT_ID_LENGTH);
    if (!NT_SUCCESS(Status))
        NtfsUnindexObjectId(DeviceExt, Buffer->ObjectId);

    return Status;
}

/**
* @name NtfsQueryObjectId
* @implemented
*
* Fills Buffer with the object ID of a file and the extended information $O keeps for it.
*/
NTSTATUS
NtfsQueryObjectId(PDEVICE_EXTENSION DeviceExt,
                  PFILE_RECORD_HEADER FileRecord,
                  PFILE_OBJECTID_BUFFER Buffer)
{
    NTFS_OBJECT_ID_DATA Data;
    NTSTATUS Status;

    RtlZeroMemory(Buffer, sizeof(*Buffer));

    Status = NtfsReadObjectId(DeviceExt, FileRecord, Buffer->ObjectId);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = NtfsLookupObjectId(DeviceExt, Buffer->ObjectId, &Data);
    if (NT_SUCCESS(Status))
        RtlCopyMemory(Buffer->ExtendedInfo, Data.ExtendedInfo, sizeof(Buffer->ExtendedInfo));

    /* An ID the index lost still is the file's ID, just without extended information */
    return (Status == STATUS_OBJECTID_NOT_FOUND) ? STATUS_SUCCESS : Status;
}

/**
* @name NtfsMakeObjectId
*
* A fresh object ID for CREATE_OR_GET: a new GUID, which is also its birth object ID, born
* on this volume, in no domain.
*/
static
NTSTATUS
NtfsMakeObjectId(PDEVICE_EXTENSION DeviceExt,
                 PFILE_OBJECTID_BUFFER Buffer)
{
    PFILE_RECORD_HEADER VolumeRecord;
    UUID Uuid;
    NTSTATUS Status;

    RtlZeroMemory(Buffer, sizeof(*Buffer));

    Status = ExUuidCreate(&Uuid);
    if (!NT_SUCCESS(Status))
        return Status;

    C_ASSERT(sizeof(Uuid) == NTFS_OBJECT_ID_LENGTH);
    RtlCopyMemory(Buffer->ObjectId, &Uuid, sizeof(Uuid));
    RtlCopyMemory(Buffer->BirthObjectId, &Uuid, sizeof(Uuid));

    /* The volume's own object ID, if it has one, is the birth volume ID */
    VolumeRecord = ExAllocateFromNPagedLookasideList(&DeviceExt->FileRecLookasideList);
    if (VolumeRecord == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = ReadFileRecord(DeviceExt, NTFS_FILE_VOLUME, VolumeRecord);
    if (NT_SUCCESS(Status))
    {
        Status = NtfsReadObjectId(DeviceExt, VolumeRecord, Buffer->BirthVolumeId);
        if (Status == STATUS_OBJECTID_NOT_FOUND)
            Status = STATUS_SUCCESS;
    }

    ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, VolumeRecord);

    return Status;
}

/**
* @name NtfsCreateOrGetObjectId
* @implemented
*
* FSCTL_CREATE_OR_GET_OBJECT_ID: the object ID of a file, made for it if it has none.
*/
NTSTATUS
NtfsCreateOrGetObjectId(PDEVICE_EXTENSION DeviceExt,
                        PNTFS_FCB Fcb,
                        PFILE_RECORD_HEADER FileRecord,
                        PFILE_OBJECTID_BUFFER Buffer)
{
    NTSTATUS Status;

    Status = NtfsQueryObjectId(DeviceExt, FileRecord, Buffer);
    if (Status != STATUS_OBJECTID_NOT_FOUND)
        return Status;

    Status = NtfsMakeObjectId(DeviceExt, Buffer);
    if (!NT_SUCCESS(Status))
        return Status;

    return NtfsSetObjectId(DeviceExt, Fcb, FileRecord, Buffer);
}

/**
* @name NtfsReleaseObjectId
* @implemented
*
* Takes the object ID of a file out of $O, if it has one: on FSCTL_DELETE_OBJECT_ID, and
* when the file is deleted, so the ID is free for another file. The caller removes the
* $OBJECT_ID attribute, or the whole record.
*/
NTSTATUS
NtfsReleaseObjectId(PDEVICE_EXTENSION DeviceExt,
                    PFILE_RECORD_HEADER FileRecord)
{
    UCHAR ObjectId[NTFS_OBJECT_ID_LENGTH];
    NTSTATUS Status;

    Status = NtfsReadObjectId(DeviceExt, FileRecord, ObjectId);
    if (Status == STATUS_OBJECTID_NOT_FOUND)
        return STATUS_SUCCESS;
    if (!NT_SUCCESS(Status))
        return Status;

    if (DeviceExt->ObjectIdIndexMftIndex == 0)
        return STATUS_SUCCESS;

    Status = NtfsUnindexObjectId(DeviceExt, ObjectId);

    /* An ID the index never had leaves nothing to release */
    return (Status == STATUS_OBJECT_NAME_NOT_FOUND) ? STATUS_SUCCESS : Status;
}

/**
* @name NtfsDeleteObjectId
* @implemented
*
* FSCTL_DELETE_OBJECT_ID: the file has no object ID afterwards, whether or not it had one.
*/
NTSTATUS
NtfsDeleteObjectId(PDEVICE_EXTENSION DeviceExt,
                   PFILE_RECORD_HEADER FileRecord)
{
    NTSTATUS Status;

    Status = NtfsReleaseObjectId(DeviceExt, FileRecord);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = NtfsDeleteAttributeFromFile(DeviceExt, FileRecord, AttributeObjectId, L"", 0);
    if (Status == STATUS_OBJECT_NAME_NOT_FOUND)
        return STATUS_SUCCESS;
    if (!NT_SUCCESS(Status))
        return Status;

    return UpdateFileRecord(DeviceExt, FileRecord->MFTRecordNumber, FileRecord);
}

/**
* @name NtfsSetObjectIdExtended
* @implemented
*
* FSCTL_SET_OBJECT_ID_EXTENDED: replaces the extended information of the object ID a file has.
*/
NTSTATUS
NtfsSetObjectIdExtended(PDEVICE_EXTENSION DeviceExt,
                        PNTFS_FCB Fcb,
                        PFILE_RECORD_HEADER FileRecord,
                        PUCHAR ExtendedInfo)
{
    FILE_OBJECTID_BUFFER Buffer;
    NTFS_OBJECT_ID_DATA Data;
    NTSTATUS Status;

    Status = NtfsReadObjectId(DeviceExt, FileRecord, Buffer.ObjectId);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = NtfsUnindexObjectId(DeviceExt, Buffer.ObjectId);
    if (!NT_SUCCESS(Status) && Status != STATUS_OBJECT_NAME_NOT_FOUND)
        return Status;

    Data.FileReference = Fcb->MFTIndex | ((ULONGLONG)FileRecord->SequenceNumber << 48);
    RtlCopyMemory(Data.ExtendedInfo, ExtendedInfo, sizeof(Data.ExtendedInfo));

    return NtfsIndexObjectId(DeviceExt, Buffer.ObjectId, &Data);
}

/**
* @name NtfsFindFileByObjectId
* @implemented
*
* The MFT index of the file an object ID belongs to, for an open by object ID.
*/
NTSTATUS
NtfsFindFileByObjectId(PDEVICE_EXTENSION DeviceExt,
                       PUCHAR ObjectId,
                       PULONGLONG MftIndex)
{
    NTFS_OBJECT_ID_DATA Data;
    NTSTATUS Status;

    if (DeviceExt->ObjectIdIndexMftIndex == 0)
        return STATUS_INVALID_PARAMETER;

    Status = NtfsLookupObjectId(DeviceExt, ObjectId, &Data);
    if (!NT_SUCCESS(Status))
        return Status;

    *MftIndex = Data.FileReference & NTFS_MFT_MASK;

    return STATUS_SUCCESS;
}

/* EOF */
