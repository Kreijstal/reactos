/*
 * PROJECT:     ReactOS NTFS driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     The USN change journal, $Extend\$UsnJrnl
 * COPYRIGHT:   Copyright 2026 ReactOS Team
 */

/*
 * The change journal is $Extend\$UsnJrnl: its $Max stream holds the journal's parameters
 * and ID, its $J stream the USN_RECORD_V2s, each at the offset that is its USN. A record
 * never crosses a USN_PAGE_SIZE boundary; the end of a page it doesn't fill reads as zeros.
 *
 * What changes a file adds reasons to its FCB. The first time a reason is added a record
 * carrying all of them is written, and when the last handle goes one more with
 * USN_REASON_CLOSE, after which the file starts over. Every record's USN also goes into
 * the file's $STANDARD_INFORMATION.
 *
 * The journal is never trimmed yet: $J only grows, and FirstUsn stays at LowestValidUsn.
 */

/* INCLUDES *****************************************************************/

#include "ntfs.h"

#define NDEBUG
#include <debug.h>

/* GLOBALS *****************************************************************/

/* $Max of $UsnJrnl */
typedef struct _NTFS_USN_MAX
{
    ULONGLONG MaximumSize;
    ULONGLONG AllocationDelta;
    ULONGLONG UsnJournalId;
    USN LowestValidUsn;
} NTFS_USN_MAX, *PNTFS_USN_MAX;

#define NTFS_USN_RECORD_NAME_OFFSET FIELD_OFFSET(USN_RECORD, FileName)

/* FUNCTIONS ****************************************************************/

/**
* @name NtfsUsnRecordLength
*
* The length of the USN_RECORD carrying a name of NameLength bytes.
*/
static
ULONG
NtfsUsnRecordLength(ULONG NameLength)
{
    return ALIGN_UP_BY(NTFS_USN_RECORD_NAME_OFFSET + NameLength, sizeof(ULONGLONG));
}

/**
* @name NtfsReadUsnMax
*
* Reads $Max of the journal into the VCB, and where the next record goes: the end of $J.
*/
static
NTSTATUS
NtfsReadUsnMax(PDEVICE_EXTENSION Vcb,
               ULONGLONG JournalMftIndex)
{
    PFILE_RECORD_HEADER Record;
    PNTFS_ATTR_CONTEXT Context;
    NTFS_USN_MAX Max;
    ULONGLONG JSize = 0;
    NTSTATUS Status;

    Record = ExAllocateFromNPagedLookasideList(&Vcb->FileRecLookasideList);
    if (Record == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = ReadFileRecord(Vcb, JournalMftIndex, Record);
    if (NT_SUCCESS(Status))
        Status = FindAttribute(Vcb, Record, AttributeData, L"$Max", 4, &Context, NULL);
    if (NT_SUCCESS(Status))
    {
        if (AttributeDataLength(Context->pRecord) < sizeof(Max) ||
            ReadAttribute(Vcb, Context, 0, (PCHAR)&Max, sizeof(Max)) != sizeof(Max))
        {
            Status = STATUS_FILE_CORRUPT_ERROR;
        }
        ReleaseAttributeContext(Context);
    }
    if (NT_SUCCESS(Status))
    {
        Status = FindAttribute(Vcb, Record, AttributeData, L"$J", 2, &Context, NULL);
        if (NT_SUCCESS(Status))
        {
            JSize = AttributeDataLength(Context->pRecord);
            ReleaseAttributeContext(Context);
        }
    }

    ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, Record);

    if (!NT_SUCCESS(Status))
        return Status;

    Vcb->UsnJournalMftIndex = JournalMftIndex;
    Vcb->UsnJournalId = Max.UsnJournalId;
    Vcb->UsnMaximumSize = Max.MaximumSize;
    Vcb->UsnAllocationDelta = Max.AllocationDelta;
    Vcb->UsnLowestValidUsn = Max.LowestValidUsn;
    Vcb->UsnNextUsn = max(JSize, (ULONGLONG)Max.LowestValidUsn);

    return STATUS_SUCCESS;
}

/**
* @name NtfsFindUsnJournalFile
*
* The MFT index of $Extend\$UsnJrnl, STATUS_OBJECT_NAME_NOT_FOUND if there is none.
*/
static
NTSTATUS
NtfsFindUsnJournalFile(PDEVICE_EXTENSION Vcb,
                       PULONGLONG MftIndex)
{
    UNICODE_STRING Name = RTL_CONSTANT_STRING(L"$UsnJrnl");
    ULONG FirstEntry = 0;
    NTSTATUS Status;

    Status = NtfsFindMftRecord(Vcb, NTFS_FILE_EXTEND, &Name, &FirstEntry, FALSE, FALSE, MftIndex, NULL);
    if (Status == STATUS_OBJECT_PATH_NOT_FOUND)
        Status = STATUS_OBJECT_NAME_NOT_FOUND;

    return Status;
}

/**
* @name NtfsLoadUsnJournal
* @implemented
*
* Finds the journal at mount, if the volume has one.
*/
VOID
NtfsLoadUsnJournal(PDEVICE_EXTENSION Vcb)
{
    ULONGLONG MftIndex;

    ExInitializeResourceLite(&Vcb->UsnResource);
    Vcb->UsnJournalMftIndex = 0;

    if (Vcb->NtfsInfo.MajorVersion < 3)
        return;

    if (NT_SUCCESS(NtfsFindUsnJournalFile(Vcb, &MftIndex)))
        NtfsReadUsnMax(Vcb, MftIndex);
}

/**
* @name NtfsGetUsnName
*
* The name and parent reference a record about Fcb carries: the name it was opened by.
*/
static
NTSTATUS
NtfsGetUsnName(PDEVICE_EXTENSION Vcb,
               PNTFS_FCB Fcb,
               PFILE_RECORD_HEADER FileRecord,
               PUNICODE_STRING Name,
               PULONGLONG ParentReference)
{
    PFILENAME_ATTRIBUTE *Names;
    ULONG Count, i;
    NTSTATUS Status;

    RtlInitUnicodeString(Name, Fcb->ObjectName + (Fcb->ObjectName[0] == L'\\' ? 1 : 0));
    *ParentReference = Fcb->ParentMFTIndex;

    Status = NtfsGetFileNames(Vcb, FileRecord, &Names, &Count);
    if (!NT_SUCCESS(Status))
        return Status;

    for (i = 0; i < Count; i++)
    {
        if ((Names[i]->DirectoryFileReferenceNumber & NTFS_MFT_MASK) == Fcb->ParentMFTIndex)
        {
            *ParentReference = Names[i]->DirectoryFileReferenceNumber;
            break;
        }
    }

    NtfsFreeFileNames(Names, Count);

    return STATUS_SUCCESS;
}

/**
* @name NtfsBuildUsnRecord
*
* Builds the USN_RECORD_V2 describing a file record, Usn left for the caller to fill in.
*
* @param Buffer
* Receives the record; NtfsUsnRecordLength(Name->Length) bytes long.
*/
static
VOID
NtfsBuildUsnRecord(PDEVICE_EXTENSION Vcb,
                   PFILE_RECORD_HEADER FileRecord,
                   PCUNICODE_STRING Name,
                   ULONGLONG ParentReference,
                   ULONG Reason,
                   PUSN_RECORD Buffer)
{
    PSTANDARD_INFORMATION StdInfo = GetStandardInformationFromRecord(Vcb, FileRecord);
    ULONG Length = NtfsUsnRecordLength(Name->Length);
    ULONG Attributes = 0;

    RtlZeroMemory(Buffer, Length);
    Buffer->RecordLength = Length;
    Buffer->MajorVersion = 2;
    Buffer->MinorVersion = 0;
    Buffer->FileReferenceNumber = FileRecord->MFTRecordNumber | ((ULONGLONG)FileRecord->SequenceNumber << 48);
    Buffer->ParentFileReferenceNumber = ParentReference;
    KeQuerySystemTime(&Buffer->TimeStamp);
    Buffer->Reason = Reason;

    if (StdInfo != NULL)
    {
        NtfsFileFlagsToAttributes(StdInfo->FileAttribute, &Attributes);
        Buffer->SecurityId = StdInfo->SecurityId;
        Buffer->Usn = StdInfo->Usn;
    }
    if (FileRecord->Flags & FRH_DIRECTORY)
        Attributes |= FILE_ATTRIBUTE_DIRECTORY;
    Buffer->FileAttributes = Attributes ? Attributes : FILE_ATTRIBUTE_NORMAL;

    Buffer->FileNameLength = Name->Length;
    Buffer->FileNameOffset = NTFS_USN_RECORD_NAME_OFFSET;
    RtlCopyMemory(Buffer->FileName, Name->Buffer, Name->Length);
}

/**
* @name NtfsAppendUsnRecord
*
* Gives a record the next USN and writes it to $J. The caller holds UsnResource exclusively.
*/
static
NTSTATUS
NtfsAppendUsnRecord(PDEVICE_EXTENSION Vcb,
                    PUSN_RECORD Record)
{
    USN Usn = Vcb->UsnNextUsn;
    NTSTATUS Status;

    ASSERT(Record->RecordLength <= USN_PAGE_SIZE);

    /* A record doesn't cross a page */
    if ((Usn % USN_PAGE_SIZE) + Record->RecordLength > USN_PAGE_SIZE)
        Usn = ALIGN_UP_BY(Usn, USN_PAGE_SIZE);

    Record->Usn = Usn;
    Status = NtfsWriteStreamAt(Vcb, Vcb->UsnJournalMftIndex, L"$J", 2, Usn, Record, Record->RecordLength);
    if (NT_SUCCESS(Status))
        Vcb->UsnNextUsn = Usn + Record->RecordLength;

    return Status;
}

/**
* @name NtfsWriteUsnChange
*
* Writes a record about Fcb with Reason, and its USN into the file's $STANDARD_INFORMATION.
*/
static
NTSTATUS
NtfsWriteUsnChange(PDEVICE_EXTENSION Vcb,
                   PNTFS_FCB Fcb,
                   ULONG Reason)
{
    PFILE_RECORD_HEADER FileRecord;
    PSTANDARD_INFORMATION StdInfo;
    PUSN_RECORD Record = NULL;
    UNICODE_STRING Name;
    ULONGLONG ParentReference;
    NTSTATUS Status;

    FileRecord = ExAllocateFromNPagedLookasideList(&Vcb->FileRecLookasideList);
    if (FileRecord == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = ReadFileRecord(Vcb, Fcb->MFTIndex, FileRecord);
    if (NT_SUCCESS(Status))
        Status = NtfsGetUsnName(Vcb, Fcb, FileRecord, &Name, &ParentReference);

    if (NT_SUCCESS(Status))
    {
        Record = ExAllocatePoolWithTag(PagedPool, NtfsUsnRecordLength(Name.Length), TAG_NTFS);
        if (Record == NULL)
            Status = STATUS_INSUFFICIENT_RESOURCES;
    }

    if (NT_SUCCESS(Status))
    {
        NtfsBuildUsnRecord(Vcb, FileRecord, &Name, ParentReference, Reason, Record);
        Status = NtfsAppendUsnRecord(Vcb, Record);
    }

    /* A $STANDARD_INFORMATION older than NTFS 3.0 has no room for the USN */
    StdInfo = GetStandardInformationFromRecord(Vcb, FileRecord);
    if (NT_SUCCESS(Status) && StdInfo != NULL && NtfsHasNtfs3StandardInformation(Vcb, FileRecord))
    {
        StdInfo->Usn = Record->Usn;
        Status = UpdateFileRecord(Vcb, Fcb->MFTIndex, FileRecord);
    }

    if (Record != NULL)
        ExFreePoolWithTag(Record, TAG_NTFS);
    ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, FileRecord);

    return Status;
}

/**
* @name NtfsPostUsnChange
* @implemented
*
* Notes that Fcb's file changed for Reason: the first time since the last close, a record
* carrying every reason so far goes into the journal.
*/
VOID
NtfsPostUsnChange(PDEVICE_EXTENSION Vcb,
                  PNTFS_FCB Fcb,
                  ULONG Reason)
{
    NTSTATUS Status;

    if (Fcb->Flags & (FCB_IS_VOLUME | FCB_IS_VOLUME_STREAM))
        return;

    KeEnterCriticalRegion();
    ExAcquireResourceExclusiveLite(&Vcb->UsnResource, TRUE);

    if (Vcb->UsnJournalMftIndex != 0 && (Fcb->UsnReasons & Reason) != Reason)
    {
        Fcb->UsnReasons |= Reason;
        Status = NtfsWriteUsnChange(Vcb, Fcb, Fcb->UsnReasons);
        if (!NT_SUCCESS(Status))
            DPRINT1("Couldn't journal a change of '%wS' (Status %lx)\n", Fcb->ObjectName, Status);
    }

    /* A rename's old name is reported once; the new name starts without it */
    if (Reason & USN_REASON_RENAME_OLD_NAME)
        Fcb->UsnReasons &= ~USN_REASON_RENAME_OLD_NAME;

    ExReleaseResourceLite(&Vcb->UsnResource);
    KeLeaveCriticalRegion();
}

/**
* @name NtfsPostUsnClose
* @implemented
*
* The last handle of Fcb is going: a record with USN_REASON_CLOSE and the reasons gathered
* since the last one, if there are any, plus Reason (FILE_DELETE when the file goes).
*/
VOID
NtfsPostUsnClose(PDEVICE_EXTENSION Vcb,
                 PNTFS_FCB Fcb,
                 ULONG Reason)
{
    NTSTATUS Status;

    if (Fcb->Flags & (FCB_IS_VOLUME | FCB_IS_VOLUME_STREAM))
        return;

    KeEnterCriticalRegion();
    ExAcquireResourceExclusiveLite(&Vcb->UsnResource, TRUE);

    if (Vcb->UsnJournalMftIndex != 0 && (Fcb->UsnReasons | Reason) != 0)
    {
        Status = NtfsWriteUsnChange(Vcb, Fcb, Fcb->UsnReasons | Reason | USN_REASON_CLOSE);
        if (!NT_SUCCESS(Status))
            DPRINT1("Couldn't journal the close of '%wS' (Status %lx)\n", Fcb->ObjectName, Status);
    }
    Fcb->UsnReasons = 0;

    ExReleaseResourceLite(&Vcb->UsnResource);
    KeLeaveCriticalRegion();
}

/**
* @name NtfsQueryUsnJournal
* @implemented
*
* FSCTL_QUERY_USN_JOURNAL.
*/
NTSTATUS
NtfsQueryUsnJournal(PDEVICE_EXTENSION Vcb,
                    PUSN_JOURNAL_DATA Data)
{
    NTSTATUS Status = STATUS_SUCCESS;

    KeEnterCriticalRegion();
    ExAcquireResourceSharedLite(&Vcb->UsnResource, TRUE);

    if (Vcb->UsnJournalMftIndex == 0)
    {
        Status = STATUS_JOURNAL_NOT_ACTIVE;
    }
    else
    {
        Data->UsnJournalID = Vcb->UsnJournalId;
        Data->FirstUsn = Vcb->UsnLowestValidUsn;
        Data->NextUsn = Vcb->UsnNextUsn;
        Data->LowestValidUsn = Vcb->UsnLowestValidUsn;
        Data->MaxUsn = MAXLONGLONG & ~(USN_PAGE_SIZE - 1);
        Data->MaximumSize = Vcb->UsnMaximumSize;
        Data->AllocationDelta = Vcb->UsnAllocationDelta;
    }

    ExReleaseResourceLite(&Vcb->UsnResource);
    KeLeaveCriticalRegion();

    return Status;
}

/**
* @name NtfsMakeUsnJournalFile
*
* Creates $Extend\$UsnJrnl with an empty $J and the $Max given.
*/
static
NTSTATUS
NtfsMakeUsnJournalFile(PDEVICE_EXTENSION Vcb,
                       PNTFS_USN_MAX Max,
                       PULONGLONG MftIndex)
{
    UNICODE_STRING Name = RTL_CONSTANT_STRING(L"$UsnJrnl");
    PFILE_RECORD_HEADER Record;
    PSTANDARD_INFORMATION StdInfo;
    ULONGLONG ExtendReference, Reference;
    ULONG SecurityId = 0;
    NTSTATUS Status;

    Record = ExAllocateFromNPagedLookasideList(&Vcb->FileRecLookasideList);
    if (Record == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    /* It lives in $Extend, with $Extend's descriptor */
    Status = ReadFileRecord(Vcb, NTFS_FILE_EXTEND, Record);
    if (NT_SUCCESS(Status))
    {
        ExtendReference = NTFS_FILE_EXTEND | ((ULONGLONG)Record->SequenceNumber << 48);
        StdInfo = GetStandardInformationFromRecord(Vcb, Record);
        if (StdInfo != NULL && NtfsHasNtfs3StandardInformation(Vcb, Record))
            SecurityId = StdInfo->SecurityId;

        Status = NtfsCreateFileRecord(Vcb, ExtendReference, &Name,
                                      FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM,
                                      FALSE, SecurityId, FALSE, TRUE, &Reference);
    }

    /* The unnamed $DATA of a new file makes way for $Max and $J */
    if (NT_SUCCESS(Status))
    {
        *MftIndex = Reference & NTFS_MFT_MASK;
        Status = ReadFileRecord(Vcb, *MftIndex, Record);
    }
    if (NT_SUCCESS(Status))
        Status = NtfsDeleteAttributeFromFile(Vcb, Record, AttributeData, L"", 0);
    if (NT_SUCCESS(Status))
        Status = NtfsAddResidentAttributeToFile(Vcb, Record, AttributeData, L"$J", 2, NULL, 0);
    if (NT_SUCCESS(Status))
        Status = NtfsAddResidentAttributeToFile(Vcb, Record, AttributeData, L"$Max", 4, Max, sizeof(*Max));
    if (NT_SUCCESS(Status))
        Status = UpdateFileRecord(Vcb, *MftIndex, Record);

    ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, Record);

    return Status;
}

/**
* @name NtfsCreateUsnJournal
* @implemented
*
* FSCTL_CREATE_USN_JOURNAL: creates the journal, or gives the one there is the new sizes.
* The caller holds DirResource exclusively.
*/
NTSTATUS
NtfsCreateUsnJournal(PDEVICE_EXTENSION Vcb,
                     PCREATE_USN_JOURNAL_DATA Create)
{
    NTFS_USN_MAX Max;
    ULONGLONG MftIndex;
    LARGE_INTEGER Now;
    NTSTATUS Status;

    if (Vcb->NtfsInfo.MajorVersion < 3)
        return STATUS_VOLUME_NOT_UPGRADED;

    KeEnterCriticalRegion();
    ExAcquireResourceExclusiveLite(&Vcb->UsnResource, TRUE);

    if (Vcb->UsnJournalMftIndex != 0)
    {
        Max.MaximumSize = Create->MaximumSize;
        Max.AllocationDelta = Create->AllocationDelta;
        Max.UsnJournalId = Vcb->UsnJournalId;
        Max.LowestValidUsn = Vcb->UsnLowestValidUsn;
        Status = NtfsWriteStreamAt(Vcb, Vcb->UsnJournalMftIndex, L"$Max", 4, 0, &Max, sizeof(Max));
        if (NT_SUCCESS(Status))
        {
            Vcb->UsnMaximumSize = Max.MaximumSize;
            Vcb->UsnAllocationDelta = Max.AllocationDelta;
        }
    }
    else
    {
        /* The journal ID is the time it was made */
        KeQuerySystemTime(&Now);
        Max.MaximumSize = Create->MaximumSize;
        Max.AllocationDelta = Create->AllocationDelta;
        Max.UsnJournalId = Now.QuadPart;
        Max.LowestValidUsn = 0;
        Status = NtfsMakeUsnJournalFile(Vcb, &Max, &MftIndex);
        if (NT_SUCCESS(Status))
            Status = NtfsReadUsnMax(Vcb, MftIndex);
    }

    ExReleaseResourceLite(&Vcb->UsnResource);
    KeLeaveCriticalRegion();

    return Status;
}

/**
* @name NtfsDeleteUsnJournal
* @implemented
*
* FSCTL_DELETE_USN_JOURNAL: deletes the journal the ID names. The caller holds DirResource
* exclusively.
*/
NTSTATUS
NtfsDeleteUsnJournal(PDEVICE_EXTENSION Vcb,
                     PDELETE_USN_JOURNAL_DATA Delete)
{
    NTSTATUS Status;

    if (Delete->DeleteFlags == 0 || (Delete->DeleteFlags & ~USN_DELETE_VALID_FLAGS))
        return STATUS_INVALID_PARAMETER;

    KeEnterCriticalRegion();
    ExAcquireResourceExclusiveLite(&Vcb->UsnResource, TRUE);

    if (Vcb->UsnJournalMftIndex == 0)
    {
        Status = STATUS_JOURNAL_NOT_ACTIVE;
    }
    else if (Delete->UsnJournalID != Vcb->UsnJournalId)
    {
        Status = STATUS_INVALID_PARAMETER;
    }
    else if (!(Delete->DeleteFlags & USN_DELETE_FLAG_DELETE))
    {
        /* Only waiting for a deletion in progress, and none is */
        Status = STATUS_SUCCESS;
    }
    else
    {
        Status = NtfsDeleteFileRecord(Vcb, Vcb->UsnJournalMftIndex, FALSE);
        if (NT_SUCCESS(Status))
            Vcb->UsnJournalMftIndex = 0;
    }

    ExReleaseResourceLite(&Vcb->UsnResource);
    KeLeaveCriticalRegion();

    return Status;
}

/* Where a READ_USN_JOURNAL is: what it asked for, and what it returns */
typedef struct _NTFS_USN_READ
{
    PREAD_USN_JOURNAL_DATA Request;
    PUCHAR Output;
    ULONG OutputLength;
    ULONG Returned;         /* bytes of Output used */
    USN Resume;             /* where the next call starts */
    BOOLEAN Full;
} NTFS_USN_READ, *PNTFS_USN_READ;

/**
* @name NtfsReturnUsnPage
*
* Returns the records of one page of $J that the read asks for.
*
* @param Page
* The page, Length bytes of it, whose first byte is USN PageUsn.
*/
static
NTSTATUS
NtfsReturnUsnPage(PNTFS_USN_READ Read,
                  PUCHAR Page,
                  ULONG Length,
                  USN PageUsn)
{
    ULONG Offset = 0;

    while (Offset + NTFS_USN_RECORD_NAME_OFFSET <= Length)
    {
        PUSN_RECORD Record = (PUSN_RECORD)(Page + Offset);

        /* The rest of the page is empty */
        if (Record->RecordLength == 0)
            break;

        if (Record->RecordLength < NTFS_USN_RECORD_NAME_OFFSET ||
            Offset + Record->RecordLength > Length ||
            Record->Usn != PageUsn + Offset)
        {
            return STATUS_FILE_CORRUPT_ERROR;
        }

        if (Record->Usn >= Read->Request->StartUsn &&
            (Record->Reason & Read->Request->ReasonMask) &&
            (!Read->Request->ReturnOnlyOnClose || (Record->Reason & USN_REASON_CLOSE)))
        {
            if (Read->Returned + Record->RecordLength > Read->OutputLength)
            {
                Read->Full = TRUE;
                Read->Resume = Record->Usn;
                return STATUS_SUCCESS;
            }

            RtlCopyMemory(Read->Output + Read->Returned, Record, Record->RecordLength);
            Read->Returned += Record->RecordLength;
        }

        Offset += Record->RecordLength;
    }

    return STATUS_SUCCESS;
}

/**
* @name NtfsReadUsnJournal
* @implemented
*
* FSCTL_READ_USN_JOURNAL: the USN to read from next, then the records from StartUsn on that
* the reason mask asks for, as many as Output holds. Doesn't wait for new records.
*/
NTSTATUS
NtfsReadUsnJournal(PDEVICE_EXTENSION Vcb,
                   PREAD_USN_JOURNAL_DATA Request,
                   PUCHAR Output,
                   ULONG OutputLength,
                   PULONG Returned)
{
    NTFS_USN_READ Read = {Request, Output, OutputLength, sizeof(USN), 0, FALSE};
    PFILE_RECORD_HEADER Record;
    PNTFS_ATTR_CONTEXT Context = NULL;
    PUCHAR Page;
    USN PageUsn;
    NTSTATUS Status;

    *Returned = 0;

    if (OutputLength < sizeof(USN))
        return STATUS_BUFFER_TOO_SMALL;

    Page = ExAllocatePoolWithTag(PagedPool, USN_PAGE_SIZE, TAG_NTFS);
    Record = ExAllocateFromNPagedLookasideList(&Vcb->FileRecLookasideList);
    if (Page == NULL || Record == NULL)
    {
        if (Page != NULL)
            ExFreePoolWithTag(Page, TAG_NTFS);
        if (Record != NULL)
            ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, Record);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    KeEnterCriticalRegion();
    ExAcquireResourceSharedLite(&Vcb->UsnResource, TRUE);

    if (Vcb->UsnJournalMftIndex == 0)
        Status = STATUS_JOURNAL_NOT_ACTIVE;
    else if (Request->UsnJournalID != Vcb->UsnJournalId)
        Status = STATUS_INVALID_PARAMETER;
    else if (Request->StartUsn < Vcb->UsnLowestValidUsn)
        Status = STATUS_JOURNAL_ENTRY_DELETED;
    else
        Status = ReadFileRecord(Vcb, Vcb->UsnJournalMftIndex, Record);

    if (NT_SUCCESS(Status))
        Status = FindAttribute(Vcb, Record, AttributeData, L"$J", 2, &Context, NULL);

    Read.Resume = Vcb->UsnNextUsn;
    for (PageUsn = ALIGN_DOWN_BY(max(Request->StartUsn, 0), USN_PAGE_SIZE);
         NT_SUCCESS(Status) && !Read.Full && PageUsn < Vcb->UsnNextUsn;
         PageUsn += USN_PAGE_SIZE)
    {
        ULONG Length = (ULONG)min(USN_PAGE_SIZE, Vcb->UsnNextUsn - PageUsn);

        if (ReadAttribute(Vcb, Context, PageUsn, (PCHAR)Page, Length) != Length)
            Status = STATUS_FILE_CORRUPT_ERROR;
        else
            Status = NtfsReturnUsnPage(&Read, Page, Length, PageUsn);
    }

    if (NT_SUCCESS(Status))
    {
        if (!Read.Full)
            Read.Resume = max(Vcb->UsnNextUsn, Request->StartUsn);
        *(USN *)Output = Read.Resume;
        *Returned = Read.Returned;
    }

    ExReleaseResourceLite(&Vcb->UsnResource);
    KeLeaveCriticalRegion();

    if (Context != NULL)
        ReleaseAttributeContext(Context);
    ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, Record);
    ExFreePoolWithTag(Page, TAG_NTFS);

    return Status;
}

/**
* @name NtfsDescribeFileRecord
*
* Builds the USN_RECORD an MFT enumeration or FSCTL_READ_FILE_USN_DATA returns for a file:
* its best name, its USN, no reason. STATUS_BUFFER_TOO_SMALL with Needed if it doesn't fit.
*/
static
NTSTATUS
NtfsDescribeFileRecord(PDEVICE_EXTENSION Vcb,
                       PFILE_RECORD_HEADER FileRecord,
                       PFILENAME_ATTRIBUTE FileName,
                       PUCHAR Output,
                       ULONG OutputLength,
                       PULONG Needed)
{
    UNICODE_STRING Name;

    Name.Buffer = FileName->Name;
    Name.Length = Name.MaximumLength = FileName->NameLength * sizeof(WCHAR);

    *Needed = NtfsUsnRecordLength(Name.Length);
    if (*Needed > OutputLength)
        return STATUS_BUFFER_TOO_SMALL;

    NtfsBuildUsnRecord(Vcb, FileRecord, &Name, FileName->DirectoryFileReferenceNumber, 0, (PUSN_RECORD)Output);

    return STATUS_SUCCESS;
}

/**
* @name NtfsReadFileUsnData
* @implemented
*
* FSCTL_READ_FILE_USN_DATA: the USN_RECORD of the file Fcb is open on.
*/
NTSTATUS
NtfsReadFileUsnData(PDEVICE_EXTENSION Vcb,
                    PNTFS_FCB Fcb,
                    PUCHAR Output,
                    ULONG OutputLength,
                    PULONG Returned)
{
    PFILE_RECORD_HEADER FileRecord;
    PUSN_RECORD Record = NULL;
    UNICODE_STRING Name;
    ULONGLONG ParentReference;
    NTSTATUS Status;

    *Returned = 0;

    FileRecord = ExAllocateFromNPagedLookasideList(&Vcb->FileRecLookasideList);
    if (FileRecord == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = ReadFileRecord(Vcb, Fcb->MFTIndex, FileRecord);
    if (NT_SUCCESS(Status))
        Status = NtfsGetUsnName(Vcb, Fcb, FileRecord, &Name, &ParentReference);
    if (NT_SUCCESS(Status) && NtfsUsnRecordLength(Name.Length) > OutputLength)
        Status = STATUS_BUFFER_TOO_SMALL;

    if (NT_SUCCESS(Status))
    {
        Record = (PUSN_RECORD)Output;
        NtfsBuildUsnRecord(Vcb, FileRecord, &Name, ParentReference, 0, Record);
        *Returned = Record->RecordLength;
    }

    ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, FileRecord);

    return Status;
}

/**
* @name NtfsEnumUsnData
* @implemented
*
* FSCTL_ENUM_USN_DATA: from StartFileReferenceNumber on, a USN_RECORD for each file in use
* whose USN is between LowUsn and HighUsn, behind the file reference to continue from.
* STATUS_END_OF_FILE once past the last record of the MFT.
*/
NTSTATUS
NtfsEnumUsnData(PDEVICE_EXTENSION Vcb,
                PMFT_ENUM_DATA Enum,
                PUCHAR Output,
                ULONG OutputLength,
                PULONG Returned)
{
    ULONGLONG RecordCount = AttributeDataLength(Vcb->MFTContext->pRecord) / Vcb->NtfsInfo.BytesPerFileRecord;
    ULONGLONG Index = Enum->StartFileReferenceNumber & NTFS_MFT_MASK;
    PFILE_RECORD_HEADER FileRecord;
    ULONG Used = sizeof(ULONGLONG);
    NTSTATUS Status = STATUS_SUCCESS;

    *Returned = 0;

    if (OutputLength < sizeof(ULONGLONG))
        return STATUS_BUFFER_TOO_SMALL;
    if (Index >= RecordCount)
        return STATUS_END_OF_FILE;

    FileRecord = ExAllocateFromNPagedLookasideList(&Vcb->FileRecLookasideList);
    if (FileRecord == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    for (; Index < RecordCount; Index++)
    {
        PFILENAME_ATTRIBUTE FileName;
        PSTANDARD_INFORMATION StdInfo;
        ULONG Needed;

        if (!NT_SUCCESS(ReadFileRecord(Vcb, Index, FileRecord)) ||
            !(FileRecord->Flags & FRH_IN_USE) ||
            FileRecord->BaseFileRecord != 0)
        {
            continue;
        }

        FileName = GetBestFileNameFromRecord(Vcb, FileRecord);
        StdInfo = GetStandardInformationFromRecord(Vcb, FileRecord);
        if (FileName == NULL || StdInfo == NULL)
            continue;

        if (NtfsHasNtfs3StandardInformation(Vcb, FileRecord) &&
            (StdInfo->Usn < Enum->LowUsn || StdInfo->Usn > Enum->HighUsn))
        {
            continue;
        }

        Status = NtfsDescribeFileRecord(Vcb, FileRecord, FileName, Output + Used, OutputLength - Used, &Needed);
        if (Status == STATUS_BUFFER_TOO_SMALL)
        {
            /* The next call starts with this file; a buffer that holds none fails */
            Status = (Used > sizeof(ULONGLONG)) ? STATUS_SUCCESS : STATUS_BUFFER_TOO_SMALL;
            break;
        }

        Used += Needed;
    }

    if (NT_SUCCESS(Status))
    {
        *(PULONGLONG)Output = Index;
        *Returned = Used;
    }

    ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, FileRecord);

    return Status;
}

/* EOF */
