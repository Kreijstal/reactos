/*
 * PROJECT:     ReactOS NTFS driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Extended attributes: $EA, $EA_INFORMATION, IRP_MJ_QUERY_EA and IRP_MJ_SET_EA
 * COPYRIGHT:   Copyright 2026 ReactOS Team
 */

/*
 * A file's EAs are its $EA attribute: FILE_FULL_EA_INFORMATION entries, each at a 4-byte
 * boundary, with upper-case names. $EA_INFORMATION goes with it: the size of the list
 * packed (each entry without its NextEntryOffset), how many entries have FILE_NEED_EA,
 * and the size of $EA. A file without EAs has neither attribute.
 */

/* INCLUDES *****************************************************************/

#include "ntfs.h"

#define NDEBUG
#include <debug.h>

/* GLOBALS *****************************************************************/

/* $EA_INFORMATION */
typedef struct _NTFS_EA_INFORMATION
{
    USHORT PackedEaSize;
    USHORT NeedEaCount;
    ULONG UnpackedEaSize;
} NTFS_EA_INFORMATION, *PNTFS_EA_INFORMATION;

/* The packed size limit of a file's EAs */
#define NTFS_MAX_PACKED_EA_SIZE 0xFFFF

/* FUNCTIONS ****************************************************************/

/* Size of an entry without its NextEntryOffset, and with it, aligned */
static
ULONG
NtfsEaPackedSize(PFILE_FULL_EA_INFORMATION Ea)
{
    return FIELD_OFFSET(FILE_FULL_EA_INFORMATION, EaName) - sizeof(ULONG) +
           Ea->EaNameLength + 1 + Ea->EaValueLength;
}

static
ULONG
NtfsEaEntrySize(PFILE_FULL_EA_INFORMATION Ea)
{
    return FIELD_OFFSET(FILE_FULL_EA_INFORMATION, EaName) + Ea->EaNameLength + 1 + Ea->EaValueLength;
}

static
PFILE_FULL_EA_INFORMATION
NtfsNextEa(PFILE_FULL_EA_INFORMATION Ea)
{
    return Ea->NextEntryOffset ? (PFILE_FULL_EA_INFORMATION)((PUCHAR)Ea + Ea->NextEntryOffset) : NULL;
}

/**
* @name NtfsEaNamesEqual
*
* EA names compare without regard to case.
*/
static
BOOLEAN
NtfsEaNamesEqual(PCHAR Name1,
                 UCHAR Length1,
                 PCHAR Name2,
                 UCHAR Length2)
{
    ANSI_STRING String1, String2;

    String1.Buffer = Name1;
    String1.Length = String1.MaximumLength = Length1;
    String2.Buffer = Name2;
    String2.Length = String2.MaximumLength = Length2;

    return RtlEqualString(&String1, &String2, TRUE);
}

/**
* @name NtfsReadEas
*
* Reads the $EA of a file. A file without EAs is STATUS_NO_EAS_ON_FILE.
*/
static
NTSTATUS
NtfsReadEas(PDEVICE_EXTENSION Vcb,
            PFILE_RECORD_HEADER FileRecord,
            PFILE_FULL_EA_INFORMATION *Eas,
            PULONG Length)
{
    PNTFS_ATTR_CONTEXT Context;
    ULONGLONG Size;
    NTSTATUS Status = STATUS_SUCCESS;
    ULONG ErrorOffset;

    *Eas = NULL;
    *Length = 0;

    if (!NT_SUCCESS(FindAttribute(Vcb, FileRecord, AttributeEA, L"", 0, &Context, NULL)))
        return STATUS_NO_EAS_ON_FILE;

    Size = AttributeDataLength(Context->pRecord);
    if (Size == 0)
    {
        ReleaseAttributeContext(Context);
        return STATUS_NO_EAS_ON_FILE;
    }

    if (Size > NTFS_MAX_PACKED_EA_SIZE + 0x10000)
        Status = STATUS_FILE_CORRUPT_ERROR;

    if (NT_SUCCESS(Status))
    {
        *Eas = ExAllocatePoolWithTag(PagedPool, (ULONG)Size, TAG_NTFS);
        if (*Eas == NULL)
            Status = STATUS_INSUFFICIENT_RESOURCES;
        else if (ReadAttribute(Vcb, Context, 0, (PCHAR)*Eas, (ULONG)Size) != Size ||
                 !NT_SUCCESS(IoCheckEaBufferValidity(*Eas, (ULONG)Size, &ErrorOffset)))
            Status = STATUS_FILE_CORRUPT_ERROR;
    }

    ReleaseAttributeContext(Context);

    if (!NT_SUCCESS(Status) && *Eas != NULL)
    {
        ExFreePoolWithTag(*Eas, TAG_NTFS);
        *Eas = NULL;
    }
    else if (NT_SUCCESS(Status))
    {
        *Length = (ULONG)Size;
    }

    return Status;
}

/**
* @name NtfsGetEaSize
* @implemented
*
* The packed size of a file's EAs, from its $EA_INFORMATION; 0 without EAs. This is the
* EaSize FileEaInformation and directory listings report.
*/
ULONG
NtfsGetEaSize(PDEVICE_EXTENSION Vcb,
              PFILE_RECORD_HEADER FileRecord)
{
    PNTFS_ATTR_CONTEXT Context;
    NTFS_EA_INFORMATION Info;
    ULONG Size = 0;

    if (!NT_SUCCESS(FindAttribute(Vcb, FileRecord, AttributeEAInformation, L"", 0, &Context, NULL)))
        return 0;

    if (AttributeDataLength(Context->pRecord) >= sizeof(Info) &&
        ReadAttribute(Vcb, Context, 0, (PCHAR)&Info, sizeof(Info)) == sizeof(Info))
    {
        Size = Info.PackedEaSize;
    }

    ReleaseAttributeContext(Context);

    return Size;
}

/**
* @name NtfsWriteEas
*
* Gives a file the EA list Eas, with its $EA_INFORMATION; an empty list removes both.
* Writes the base record.
*/
static
NTSTATUS
NtfsWriteEas(PDEVICE_EXTENSION Vcb,
             PFILE_RECORD_HEADER FileRecord,
             PFILE_FULL_EA_INFORMATION Eas,
             ULONG Length)
{
    NTFS_EA_INFORMATION Info = {0, 0, Length};
    PFILE_FULL_EA_INFORMATION Ea;
    ULONG Packed = 0;
    NTSTATUS Status;

    if (Length == 0)
    {
        Status = NtfsDeleteAttributeFromFile(Vcb, FileRecord, AttributeEA, L"", 0);
        if (NT_SUCCESS(Status) || Status == STATUS_OBJECT_NAME_NOT_FOUND)
            Status = NtfsDeleteAttributeFromFile(Vcb, FileRecord, AttributeEAInformation, L"", 0);
        if (Status == STATUS_OBJECT_NAME_NOT_FOUND)
            Status = STATUS_SUCCESS;
        if (NT_SUCCESS(Status))
            Status = UpdateFileRecord(Vcb, FileRecord->MFTRecordNumber, FileRecord);
        return Status;
    }

    for (Ea = Eas; Ea != NULL; Ea = NtfsNextEa(Ea))
    {
        Packed += NtfsEaPackedSize(Ea);
        if (Ea->Flags & FILE_NEED_EA)
            Info.NeedEaCount++;
    }

    if (Packed > NTFS_MAX_PACKED_EA_SIZE)
        return STATUS_EA_TOO_LARGE;
    Info.PackedEaSize = (USHORT)Packed;

    Status = NtfsSetAttributeValue(Vcb, FileRecord, AttributeEAInformation, NULL, 0, &Info, sizeof(Info));
    if (NT_SUCCESS(Status))
        Status = NtfsSetAttributeValue(Vcb, FileRecord, AttributeEA, NULL, 0, Eas, Length);

    return Status;
}

/**
* @name NtfsMergeEas
*
* Applies a list of changes to a list of EAs: an entry with a value adds or replaces the EA
* of that name, one without removes it. Names are upper-cased.
*
* @param Merged
* Receives the new list, Length bytes long, 0 if no EA is left. Free it with
* ExFreePoolWithTag(TAG_NTFS).
*/
static
NTSTATUS
NtfsMergeEas(PFILE_FULL_EA_INFORMATION Existing,
             ULONG ExistingLength,
             PFILE_FULL_EA_INFORMATION Changes,
             ULONG ChangesLength,
             PFILE_FULL_EA_INFORMATION *Merged,
             PULONG MergedLength)
{
    ULONG Capacity = ExistingLength + ChangesLength + 2 * sizeof(ULONG);
    PFILE_FULL_EA_INFORMATION Change, Old;
    PFILE_FULL_EA_INFORMATION Last = NULL;
    PUCHAR Buffer;
    ULONG Used = 0;
    ULONG i;

    Buffer = ExAllocatePoolWithTag(PagedPool, Capacity, TAG_NTFS);
    if (Buffer == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    /* The EAs no change names keep their place */
    for (Old = (ExistingLength ? Existing : NULL); Old != NULL; Old = NtfsNextEa(Old))
    {
        BOOLEAN Changed = FALSE;

        for (Change = Changes; Change != NULL && !Changed; Change = NtfsNextEa(Change))
            Changed = NtfsEaNamesEqual(Old->EaName, Old->EaNameLength, Change->EaName, Change->EaNameLength);
        if (Changed)
            continue;

        Used = ALIGN_UP_BY(Used, sizeof(ULONG));
        if (Last != NULL)
            Last->NextEntryOffset = (ULONG)(Buffer + Used - (PUCHAR)Last);
        Last = (PFILE_FULL_EA_INFORMATION)(Buffer + Used);
        RtlCopyMemory(Last, Old, NtfsEaEntrySize(Old));
        Last->NextEntryOffset = 0;
        Used += NtfsEaEntrySize(Old);
    }

    /* Then each new value, the last one given for a name winning */
    for (Change = Changes; Change != NULL; Change = NtfsNextEa(Change))
    {
        PFILE_FULL_EA_INFORMATION Later;
        BOOLEAN Superseded = FALSE;

        for (Later = NtfsNextEa(Change); Later != NULL && !Superseded; Later = NtfsNextEa(Later))
            Superseded = NtfsEaNamesEqual(Change->EaName, Change->EaNameLength, Later->EaName, Later->EaNameLength);
        if (Superseded || Change->EaValueLength == 0)
            continue;

        Used = ALIGN_UP_BY(Used, sizeof(ULONG));
        ASSERT(Used + NtfsEaEntrySize(Change) <= Capacity);
        if (Last != NULL)
            Last->NextEntryOffset = (ULONG)(Buffer + Used - (PUCHAR)Last);
        Last = (PFILE_FULL_EA_INFORMATION)(Buffer + Used);
        RtlCopyMemory(Last, Change, NtfsEaEntrySize(Change));
        Last->NextEntryOffset = 0;
        Last->Flags &= FILE_NEED_EA;
        for (i = 0; i < Last->EaNameLength; i++)
            Last->EaName[i] = RtlUpperChar(Last->EaName[i]);
        Used += NtfsEaEntrySize(Change);
    }

    if (Used == 0)
    {
        ExFreePoolWithTag(Buffer, TAG_NTFS);
        Buffer = NULL;
    }

    *Merged = (PFILE_FULL_EA_INFORMATION)Buffer;
    *MergedLength = Used;

    return STATUS_SUCCESS;
}

/**
* @name NtfsCheckEaNames
*
* An EA name is 1 to 255 characters, none of them a control character or one of the
* characters a FAT name can't hold (MS-FSCC 2.4.15).
*/
static
NTSTATUS
NtfsCheckEaNames(PFILE_FULL_EA_INFORMATION Eas,
                 PULONG ErrorOffset)
{
    PFILE_FULL_EA_INFORMATION Ea;
    ULONG i;

    for (Ea = Eas; Ea != NULL; Ea = NtfsNextEa(Ea))
    {
        BOOLEAN Valid = (Ea->EaNameLength != 0);

        for (i = 0; i < Ea->EaNameLength && Valid; i++)
        {
            UCHAR Char = (UCHAR)Ea->EaName[i];
            Valid = (Char >= 0x20 && strchr("\"*+,/:;<=>?[\\]|", Char) == NULL);
        }

        if (!Valid)
        {
            *ErrorOffset = (ULONG)((PUCHAR)Ea - (PUCHAR)Eas);
            return STATUS_INVALID_EA_NAME;
        }
    }

    return STATUS_SUCCESS;
}

/**
* @name NtfsCheckEaList
* @implemented
*
* Checks an EA list before it's applied: well formed (IoCheckEaBufferValidity) and with
* valid names.
*/
NTSTATUS
NtfsCheckEaList(PFILE_FULL_EA_INFORMATION Eas,
                ULONG Length,
                PULONG ErrorOffset)
{
    NTSTATUS Status;

    *ErrorOffset = 0;

    Status = IoCheckEaBufferValidity(Eas, Length, ErrorOffset);
    if (NT_SUCCESS(Status))
        Status = NtfsCheckEaNames(Eas, ErrorOffset);

    return Status;
}

/**
* @name NtfsSetEas
* @implemented
*
* Applies an EA list given to IRP_MJ_SET_EA or to a create to the file Fcb is open on
* (MS-FSA 2.1.5.16). Bad entries fail it with the offset of the first in ErrorOffset.
*/
NTSTATUS
NtfsSetEas(PDEVICE_EXTENSION Vcb,
           PNTFS_FCB Fcb,
           PFILE_FULL_EA_INFORMATION Changes,
           ULONG ChangesLength,
           PULONG ErrorOffset)
{
    PFILE_RECORD_HEADER FileRecord;
    PFILE_FULL_EA_INFORMATION Existing = NULL, Merged = NULL;
    ULONG ExistingLength = 0, MergedLength = 0;
    NTSTATUS Status;

    Status = NtfsCheckEaList(Changes, ChangesLength, ErrorOffset);
    if (!NT_SUCCESS(Status))
        return Status;

    FileRecord = ExAllocateFromNPagedLookasideList(&Vcb->FileRecLookasideList);
    if (FileRecord == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = ReadFileRecord(Vcb, Fcb->MFTIndex, FileRecord);
    if (NT_SUCCESS(Status))
    {
        Status = NtfsReadEas(Vcb, FileRecord, &Existing, &ExistingLength);
        if (Status == STATUS_NO_EAS_ON_FILE)
            Status = STATUS_SUCCESS;
    }
    if (NT_SUCCESS(Status))
        Status = NtfsMergeEas(Existing, ExistingLength, Changes, ChangesLength, &Merged, &MergedLength);
    if (NT_SUCCESS(Status))
        Status = NtfsWriteEas(Vcb, FileRecord, Merged, MergedLength);

    /* Directory listings report the packed size from the $FILE_NAMEs */
    if (NT_SUCCESS(Status) && !(NtfsGetReparseTag(Vcb, FileRecord)))
    {
        PFILENAME_ATTRIBUTE FileName = GetBestFileNameFromRecord(Vcb, FileRecord);

        if (FileName != NULL)
        {
            FileName->Extended.EaInfo.PackedEaSize = (USHORT)NtfsGetEaSize(Vcb, FileRecord);
            FileName->Extended.EaInfo.AlignmentOrReserved = 0;
            Status = NtfsUpdateDuplicatedInformation(Vcb, FileRecord, Fcb->MFTIndex, NTFS_FILENAME_UPDATE_ATTRS, FALSE);
        }
    }

    if (Existing != NULL)
        ExFreePoolWithTag(Existing, TAG_NTFS);
    if (Merged != NULL)
        ExFreePoolWithTag(Merged, TAG_NTFS);
    ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, FileRecord);

    return Status;
}

/* A query in progress: where its output goes */
typedef struct _NTFS_EA_OUTPUT
{
    PUCHAR Buffer;
    ULONG Length;
    ULONG Used;
    PFILE_FULL_EA_INFORMATION Last;
} NTFS_EA_OUTPUT, *PNTFS_EA_OUTPUT;

/**
* @name NtfsReturnEa
*
* Appends one entry to a query's output, FALSE if it doesn't fit.
*/
static
BOOLEAN
NtfsReturnEa(PNTFS_EA_OUTPUT Output,
             PCHAR Name,
             UCHAR NameLength,
             PFILE_FULL_EA_INFORMATION Ea)
{
    ULONG Start = ALIGN_UP_BY(Output->Used, sizeof(ULONG));
    ULONG Size = FIELD_OFFSET(FILE_FULL_EA_INFORMATION, EaName) + NameLength + 1 +
                 (Ea != NULL ? Ea->EaValueLength : 0);
    PFILE_FULL_EA_INFORMATION Entry;

    if (Start + Size > Output->Length)
        return FALSE;

    Entry = (PFILE_FULL_EA_INFORMATION)(Output->Buffer + Start);
    if (Ea != NULL)
    {
        RtlCopyMemory(Entry, Ea, Size);
    }
    else
    {
        /* An EA the file doesn't have comes back with no value */
        RtlZeroMemory(Entry, Size);
        Entry->EaNameLength = NameLength;
        RtlCopyMemory(Entry->EaName, Name, NameLength);
    }
    Entry->NextEntryOffset = 0;

    if (Output->Last != NULL)
        Output->Last->NextEntryOffset = (ULONG)((PUCHAR)Entry - (PUCHAR)Output->Last);
    Output->Last = Entry;
    Output->Used = Start + Size;

    return TRUE;
}

/**
* @name NtfsQueryEasByName
*
* Returns the EAs an FILE_GET_EA_INFORMATION list names, in its order.
*/
static
NTSTATUS
NtfsQueryEasByName(PFILE_FULL_EA_INFORMATION Eas,
                   PFILE_GET_EA_INFORMATION List,
                   ULONG ListLength,
                   PNTFS_EA_OUTPUT Output)
{
    PFILE_GET_EA_INFORMATION Get = List;

    for (;;)
    {
        PFILE_FULL_EA_INFORMATION Ea;

        if ((PUCHAR)Get + FIELD_OFFSET(FILE_GET_EA_INFORMATION, EaName) + Get->EaNameLength + 1 >
            (PUCHAR)List + ListLength)
        {
            return STATUS_EA_LIST_INCONSISTENT;
        }

        for (Ea = Eas; Ea != NULL; Ea = NtfsNextEa(Ea))
        {
            if (NtfsEaNamesEqual(Ea->EaName, Ea->EaNameLength, Get->EaName, Get->EaNameLength))
                break;
        }

        if (!NtfsReturnEa(Output, Get->EaName, Get->EaNameLength, Ea))
            return (Output->Last == NULL) ? STATUS_BUFFER_TOO_SMALL : STATUS_BUFFER_OVERFLOW;

        if (Get->NextEntryOffset == 0)
            return STATUS_SUCCESS;
        Get = (PFILE_GET_EA_INFORMATION)((PUCHAR)Get + Get->NextEntryOffset);
    }
}

/**
* @name NtfsQueryEasFrom
*
* Returns the EAs from the one-based Index on, one or as many as fit, and moves Index past
* them.
*/
static
NTSTATUS
NtfsQueryEasFrom(PFILE_FULL_EA_INFORMATION Eas,
                 PULONG Index,
                 BOOLEAN Single,
                 PNTFS_EA_OUTPUT Output)
{
    PFILE_FULL_EA_INFORMATION Ea = Eas;
    ULONG Position;

    for (Position = 1; Ea != NULL && Position < *Index; Position++)
        Ea = NtfsNextEa(Ea);

    if (Ea == NULL)
        return STATUS_NO_MORE_EAS;

    for (; Ea != NULL; Ea = NtfsNextEa(Ea))
    {
        if (!NtfsReturnEa(Output, Ea->EaName, Ea->EaNameLength, Ea))
            return (Output->Last == NULL) ? STATUS_BUFFER_TOO_SMALL : STATUS_BUFFER_OVERFLOW;

        (*Index)++;
        if (Single)
            break;
    }

    return STATUS_SUCCESS;
}

/**
* @name NtfsQueryEa
* @implemented
*
* IRP_MJ_QUERY_EA (MS-FSA 2.1.5.4): the EAs an EaList names, or the next ones after the
* handle's cursor, or from EaIndex. STATUS_NO_EAS_ON_FILE for a file without any,
* STATUS_NO_MORE_EAS past the last one.
*/
NTSTATUS
NtfsQueryEa(PNTFS_IRP_CONTEXT IrpContext)
{
    PIRP Irp = IrpContext->Irp;
    PIO_STACK_LOCATION Stack = IrpContext->Stack;
    PDEVICE_EXTENSION Vcb = IrpContext->DeviceObject->DeviceExtension;
    PNTFS_FCB Fcb = Stack->FileObject->FsContext;
    PNTFS_CCB Ccb = Stack->FileObject->FsContext2;
    NTFS_EA_OUTPUT Output = {NULL, Stack->Parameters.QueryEa.Length, 0, NULL};
    PFILE_RECORD_HEADER FileRecord;
    PFILE_FULL_EA_INFORMATION Eas = NULL;
    ULONG EasLength, Index;
    PVOID UserBuffer;
    NTSTATUS Status;

    if (Fcb == NULL || IrpContext->DeviceObject == NtfsGlobalData->DeviceObject ||
        (Fcb->Flags & (FCB_IS_VOLUME | FCB_IS_VOLUME_STREAM)))
    {
        return STATUS_INVALID_PARAMETER;
    }

    Output.Buffer = ExAllocatePoolWithTag(PagedPool, max(Output.Length, 1), TAG_NTFS);
    FileRecord = ExAllocateFromNPagedLookasideList(&Vcb->FileRecLookasideList);
    if (Output.Buffer == NULL || FileRecord == NULL)
    {
        if (Output.Buffer != NULL)
            ExFreePoolWithTag(Output.Buffer, TAG_NTFS);
        if (FileRecord != NULL)
            ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, FileRecord);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    ExAcquireResourceSharedLite(&Fcb->MainResource, TRUE);

    Status = ReadFileRecord(Vcb, Fcb->MFTIndex, FileRecord);
    if (NT_SUCCESS(Status))
        Status = NtfsReadEas(Vcb, FileRecord, &Eas, &EasLength);

    if (NT_SUCCESS(Status))
    {
        if (Stack->Parameters.QueryEa.EaList != NULL)
        {
            Status = NtfsQueryEasByName(Eas, Stack->Parameters.QueryEa.EaList,
                                        Stack->Parameters.QueryEa.EaListLength, &Output);
        }
        else
        {
            /* The cursor of the handle: the one-based index of the next EA */
            if (Stack->Flags & SL_INDEX_SPECIFIED)
                Index = Stack->Parameters.QueryEa.EaIndex;
            else if (Stack->Flags & SL_RESTART_SCAN)
                Index = 1;
            else
                Index = Ccb->EaIndex ? Ccb->EaIndex : 1;

            Status = NtfsQueryEasFrom(Eas, &Index, BooleanFlagOn(Stack->Flags, SL_RETURN_SINGLE_ENTRY), &Output);
            Ccb->EaIndex = Index;
        }
    }

    ExReleaseResourceLite(&Fcb->MainResource);

    if (NT_SUCCESS(Status) || Status == STATUS_BUFFER_OVERFLOW)
    {
        UserBuffer = NtfsGetUserBuffer(Irp, FALSE);
        _SEH2_TRY
        {
            RtlCopyMemory(UserBuffer, Output.Buffer, Output.Used);
            Irp->IoStatus.Information = Output.Used;
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            Status = _SEH2_GetExceptionCode();
        }
        _SEH2_END;
    }

    if (Eas != NULL)
        ExFreePoolWithTag(Eas, TAG_NTFS);
    ExFreePoolWithTag(Output.Buffer, TAG_NTFS);
    ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, FileRecord);

    return Status;
}

/**
* @name NtfsSetEa
* @implemented
*
* IRP_MJ_SET_EA: applies the list in the system buffer to the file.
*/
NTSTATUS
NtfsSetEa(PNTFS_IRP_CONTEXT IrpContext)
{
    PIRP Irp = IrpContext->Irp;
    PIO_STACK_LOCATION Stack = IrpContext->Stack;
    PDEVICE_EXTENSION Vcb = IrpContext->DeviceObject->DeviceExtension;
    PNTFS_FCB Fcb = Stack->FileObject->FsContext;
    ULONG ErrorOffset;
    NTSTATUS Status;

    if (Fcb == NULL || IrpContext->DeviceObject == NtfsGlobalData->DeviceObject ||
        (Fcb->Flags & (FCB_IS_VOLUME | FCB_IS_VOLUME_STREAM)))
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (!NtfsGlobalData->EnableWriteSupport)
        return STATUS_MEDIA_WRITE_PROTECTED;

    /* NtSetEaFile always hands the list over in a system buffer */
    ASSERT(Irp->AssociatedIrp.SystemBuffer != NULL);

    ExAcquireResourceExclusiveLite(&Fcb->MainResource, TRUE);
    Status = NtfsSetEas(Vcb, Fcb, Irp->AssociatedIrp.SystemBuffer, Stack->Parameters.SetEa.Length, &ErrorOffset);
    if (NT_SUCCESS(Status))
        NtfsPostUsnChange(Vcb, Fcb, USN_REASON_EA_CHANGE);
    ExReleaseResourceLite(&Fcb->MainResource);

    if (!NT_SUCCESS(Status))
        Irp->IoStatus.Information = ErrorOffset;

    return Status;
}

/* EOF */
