/*
 * PROJECT:     ReactOS NTFS driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Security descriptors: $Secure, IRP_MJ_QUERY_SECURITY and IRP_MJ_SET_SECURITY
 * COPYRIGHT:   Copyright 2026 ReactOS Team
 */

/*
 * NTFS 3.0 keeps every distinct security descriptor once, in the $SDS stream of $Secure,
 * and a file names its descriptor by the SecurityId of its $STANDARD_INFORMATION. $SII
 * indexes the descriptors by SecurityId, $SDH by hash and SecurityId, which is how a
 * descriptor already stored is found again and shared.
 *
 * $SDS is a run of 256 KiB blocks in pairs: each descriptor, behind a header that repeats
 * its hash, ID, offset and length, goes in the first block of a pair at a 16-byte boundary
 * and never crosses into the second, which holds a mirror copy 256 KiB further on.
 *
 * A file of NTFS before 3.0 carries its own $SECURITY_DESCRIPTOR attribute instead; that
 * is read too, and replaced by a SecurityId when the descriptor is set.
 */

/* INCLUDES *****************************************************************/

#include "ntfs.h"

#define NDEBUG
#include <debug.h>

/* GLOBALS *****************************************************************/

#define NTFS_SDS_BLOCK_SIZE      0x40000
#define NTFS_SDS_ALIGNMENT       16
#define NTFS_FIRST_SECURITY_ID   0x100

#include <pshpack4.h>
/* What precedes a descriptor in $SDS, and is the data of its $SII and $SDH entries */
typedef struct _NTFS_SECURITY_HEADER
{
    ULONG Hash;
    ULONG SecurityId;
    ULONGLONG Offset;
    ULONG Length;               /* header included */
} NTFS_SECURITY_HEADER, *PNTFS_SECURITY_HEADER;
#include <poppack.h>

C_ASSERT(sizeof(NTFS_SECURITY_HEADER) == 20);

/* The key of $SDH */
typedef struct _NTFS_SDH_KEY
{
    ULONG Hash;
    ULONG SecurityId;
} NTFS_SDH_KEY, *PNTFS_SDH_KEY;

/* FUNCTIONS ****************************************************************/

/**
* @name NtfsSecurityHash
*
* The hash $SDH is keyed by: each ULONG of the descriptor added to the hash so far rotated
* left by 3.
*/
static
ULONG
NtfsSecurityHash(PSECURITY_DESCRIPTOR Sd,
                 ULONG Length)
{
    PULONG Ulongs = Sd;
    ULONG Hash = 0;
    ULONG i;

    for (i = 0; i < Length / sizeof(ULONG); i++)
        Hash = Ulongs[i] + ((Hash << 3) | (Hash >> 29));

    return Hash;
}

/**
* @name NtfsSdsOffsetFor
*
* Where in $SDS a descriptor of Length bytes (header included) goes, after End, the end of
* the last one: in the first block of a pair, not crossing into its mirror.
*/
static
ULONGLONG
NtfsSdsOffsetFor(ULONGLONG End,
                 ULONG Length)
{
    ULONGLONG Offset = ALIGN_UP_BY(End, NTFS_SDS_ALIGNMENT);

    ASSERT(Length <= NTFS_SDS_BLOCK_SIZE);

    if ((Offset % (2 * NTFS_SDS_BLOCK_SIZE)) + Length > NTFS_SDS_BLOCK_SIZE)
        Offset = ALIGN_UP_BY(Offset, 2 * NTFS_SDS_BLOCK_SIZE);

    return Offset;
}

/**
* @name NtfsInitializeSecurity
* @implemented
*
* Finds $Secure at mount, and where the next descriptor and SecurityId go: after the last
* one $SII holds. A volume without $Secure (before NTFS 3.0) keeps descriptors per file.
*/
VOID
NtfsInitializeSecurity(PDEVICE_EXTENSION DeviceExt)
{
    PFILE_RECORD_HEADER SecureRecord;
    PNTFS_ATTR_CONTEXT SdsContext;
    PINDEX_ENTRY_ATTRIBUTE Last;
    PNTFS_SECURITY_HEADER Header;
    NTSTATUS Status;

    DeviceExt->SecureMftIndex = 0;
    DeviceExt->NextSecurityId = NTFS_FIRST_SECURITY_ID;
    DeviceExt->SdsEnd = 0;

    /* NTFS 3.0 put $Secure where $Quota was, in record 9 */
    if (DeviceExt->NtfsInfo.MajorVersion < 3)
        return;

    SecureRecord = ExAllocateFromNPagedLookasideList(&DeviceExt->FileRecLookasideList);
    if (SecureRecord == NULL)
        return;

    Status = ReadFileRecord(DeviceExt, NTFS_FILE_SECURE, SecureRecord);
    if (NT_SUCCESS(Status))
        Status = FindAttribute(DeviceExt, SecureRecord, AttributeData, L"$SDS", 4, &SdsContext, NULL);
    if (NT_SUCCESS(Status))
    {
        ReleaseAttributeContext(SdsContext);
        DeviceExt->SecureMftIndex = NTFS_FILE_SECURE;
    }

    ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, SecureRecord);

    if (DeviceExt->SecureMftIndex == 0)
    {
        DPRINT1("NTFS %u.%u volume without $Secure\n", DeviceExt->NtfsInfo.MajorVersion, DeviceExt->NtfsInfo.MinorVersion);
        return;
    }

    Status = NtfsFindLastIndexEntry(DeviceExt, DeviceExt->SecureMftIndex, L"$SII", 4, &Last);
    if (!NT_SUCCESS(Status))
        return;

    ASSERT(Last->Data.ViewIndex.DataLength >= sizeof(NTFS_SECURITY_HEADER));
    Header = (PNTFS_SECURITY_HEADER)((PUCHAR)Last + Last->Data.ViewIndex.DataOffset);
    DeviceExt->NextSecurityId = Header->SecurityId + 1;
    DeviceExt->SdsEnd = Header->Offset + Header->Length;

    ExFreePoolWithTag(Last, TAG_NTFS);
}

/**
* @name NtfsReadSds
*
* Reads Length bytes of $SDS at Offset.
*/
static
NTSTATUS
NtfsReadSds(PDEVICE_EXTENSION DeviceExt,
            ULONGLONG Offset,
            PVOID Buffer,
            ULONG Length)
{
    PFILE_RECORD_HEADER SecureRecord;
    PNTFS_ATTR_CONTEXT SdsContext;
    NTSTATUS Status;

    SecureRecord = ExAllocateFromNPagedLookasideList(&DeviceExt->FileRecLookasideList);
    if (SecureRecord == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = ReadFileRecord(DeviceExt, DeviceExt->SecureMftIndex, SecureRecord);
    if (NT_SUCCESS(Status))
        Status = FindAttribute(DeviceExt, SecureRecord, AttributeData, L"$SDS", 4, &SdsContext, NULL);

    if (NT_SUCCESS(Status))
    {
        if (Offset + Length > AttributeDataLength(SdsContext->pRecord) ||
            ReadAttribute(DeviceExt, SdsContext, Offset, Buffer, Length) != Length)
        {
            Status = STATUS_FILE_CORRUPT_ERROR;
        }
        ReleaseAttributeContext(SdsContext);
    }

    ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, SecureRecord);

    return Status;
}

/**
* @name NtfsReadStoredDescriptor
*
* Reads the descriptor a $SII or $SDH entry header points at, checking it is that entry's.
*/
static
NTSTATUS
NtfsReadStoredDescriptor(PDEVICE_EXTENSION DeviceExt,
                         PNTFS_SECURITY_HEADER Header,
                         PSECURITY_DESCRIPTOR *Sd,
                         PULONG SdLength)
{
    PNTFS_SECURITY_HEADER Stored;
    NTSTATUS Status;

    *Sd = NULL;

    if (Header->Length <= sizeof(NTFS_SECURITY_HEADER) || Header->Length > NTFS_SDS_BLOCK_SIZE)
        return STATUS_FILE_CORRUPT_ERROR;

    Stored = ExAllocatePoolWithTag(PagedPool, Header->Length, TAG_NTFS);
    if (Stored == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = NtfsReadSds(DeviceExt, Header->Offset, Stored, Header->Length);
    if (NT_SUCCESS(Status) &&
        (Stored->SecurityId != Header->SecurityId || Stored->Length != Header->Length))
    {
        Status = STATUS_FILE_CORRUPT_ERROR;
    }

    if (NT_SUCCESS(Status))
    {
        *SdLength = Header->Length - sizeof(NTFS_SECURITY_HEADER);
        *Sd = ExAllocatePoolWithTag(PagedPool, *SdLength, TAG_NTFS);
        if (*Sd == NULL)
            Status = STATUS_INSUFFICIENT_RESOURCES;
        else
            RtlCopyMemory(*Sd, Stored + 1, *SdLength);
    }

    ExFreePoolWithTag(Stored, TAG_NTFS);

    if (NT_SUCCESS(Status) && !RtlValidRelativeSecurityDescriptor(*Sd, *SdLength, 0))
    {
        ExFreePoolWithTag(*Sd, TAG_NTFS);
        *Sd = NULL;
        Status = STATUS_FILE_CORRUPT_ERROR;
    }

    return Status;
}

/**
* @name NtfsReadDescriptorById
*
* Reads the descriptor $SII maps SecurityId to.
*/
static
NTSTATUS
NtfsReadDescriptorById(PDEVICE_EXTENSION DeviceExt,
                       ULONG SecurityId,
                       PSECURITY_DESCRIPTOR *Sd,
                       PULONG SdLength)
{
    PINDEX_ENTRY_ATTRIBUTE SearchEntry, Found;
    NTSTATUS Status;

    SearchEntry = NtfsAllocateViewIndexEntry(&SecurityId, sizeof(SecurityId), NULL, 0);
    if (SearchEntry == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = NtfsFindIndexEntry(DeviceExt, DeviceExt->SecureMftIndex, L"$SII", 4, SearchEntry, &Found);
    ExFreePoolWithTag(SearchEntry, TAG_NTFS);
    if (!NT_SUCCESS(Status))
        return (Status == STATUS_OBJECT_NAME_NOT_FOUND) ? STATUS_FILE_CORRUPT_ERROR : Status;

    if (Found->Data.ViewIndex.DataLength < sizeof(NTFS_SECURITY_HEADER))
        Status = STATUS_FILE_CORRUPT_ERROR;
    else
        Status = NtfsReadStoredDescriptor(DeviceExt,
                                          (PNTFS_SECURITY_HEADER)((PUCHAR)Found + Found->Data.ViewIndex.DataOffset),
                                          Sd,
                                          SdLength);

    ExFreePoolWithTag(Found, TAG_NTFS);

    return Status;
}

/* What a $SDH walk looks for: a stored descriptor equal to this one */
typedef struct _NTFS_SDH_MATCH
{
    PDEVICE_EXTENSION DeviceExt;
    PSECURITY_DESCRIPTOR Sd;
    ULONG SdLength;
    ULONG SecurityId;           /* 0 until found */
    NTSTATUS Status;
} NTFS_SDH_MATCH, *PNTFS_SDH_MATCH;

static
BOOLEAN
NtfsMatchStoredDescriptor(PINDEX_ENTRY_ATTRIBUTE Entry,
                          PVOID Context)
{
    PNTFS_SDH_MATCH Match = Context;
    PNTFS_SECURITY_HEADER Header = (PNTFS_SECURITY_HEADER)((PUCHAR)Entry + Entry->Data.ViewIndex.DataOffset);
    PSECURITY_DESCRIPTOR Stored;
    ULONG StoredLength;

    if (Header->Length - sizeof(NTFS_SECURITY_HEADER) != Match->SdLength)
        return TRUE;

    Match->Status = NtfsReadStoredDescriptor(Match->DeviceExt, Header, &Stored, &StoredLength);
    if (!NT_SUCCESS(Match->Status))
        return FALSE;

    if (RtlEqualMemory(Stored, Match->Sd, StoredLength))
        Match->SecurityId = Header->SecurityId;

    ExFreePoolWithTag(Stored, TAG_NTFS);

    return Match->SecurityId == 0;
}

/**
* @name NtfsFindStoredDescriptor
*
* The SecurityId of a descriptor already in $Secure, 0 if there's none.
*/
static
NTSTATUS
NtfsFindStoredDescriptor(PDEVICE_EXTENSION DeviceExt,
                         PSECURITY_DESCRIPTOR Sd,
                         ULONG SdLength,
                         ULONG Hash,
                         PULONG SecurityId)
{
    NTFS_SDH_KEY LowKey = {Hash, 0}, HighKey = {Hash, MAXULONG};
    PINDEX_ENTRY_ATTRIBUTE Low, High;
    NTFS_SDH_MATCH Match = {DeviceExt, Sd, SdLength, 0, STATUS_SUCCESS};
    NTSTATUS Status = STATUS_INSUFFICIENT_RESOURCES;

    Low = NtfsAllocateViewIndexEntry(&LowKey, sizeof(LowKey), NULL, 0);
    High = NtfsAllocateViewIndexEntry(&HighKey, sizeof(HighKey), NULL, 0);
    if (Low != NULL && High != NULL)
    {
        Status = NtfsVisitIndexEntries(DeviceExt, DeviceExt->SecureMftIndex, L"$SDH", 4,
                                       Low, High, NtfsMatchStoredDescriptor, &Match);
        if (NT_SUCCESS(Status))
            Status = Match.Status;
    }

    if (Low != NULL)
        ExFreePoolWithTag(Low, TAG_NTFS);
    if (High != NULL)
        ExFreePoolWithTag(High, TAG_NTFS);

    *SecurityId = Match.SecurityId;

    return Status;
}

/**
* @name NtfsWriteSds
*
* Writes a descriptor with its header to $SDS, and its mirror copy.
*/
static
NTSTATUS
NtfsWriteSds(PDEVICE_EXTENSION DeviceExt,
             PNTFS_SECURITY_HEADER Entry)
{
    NTSTATUS Status;

    /* The mirror first: it grows the stream to its new end in one go */
    Status = NtfsWriteStreamAt(DeviceExt, DeviceExt->SecureMftIndex, L"$SDS", 4,
                               Entry->Offset + NTFS_SDS_BLOCK_SIZE, Entry, Entry->Length);
    if (!NT_SUCCESS(Status))
        return Status;

    return NtfsWriteStreamAt(DeviceExt, DeviceExt->SecureMftIndex, L"$SDS", 4,
                             Entry->Offset, Entry, Entry->Length);
}

/**
* @name NtfsIndexDescriptor
*
* Adds a stored descriptor to $SII and $SDH.
*/
static
NTSTATUS
NtfsIndexDescriptor(PDEVICE_EXTENSION DeviceExt,
                    PNTFS_SECURITY_HEADER Header)
{
    NTFS_SDH_KEY SdhKey = {Header->Hash, Header->SecurityId};
    PINDEX_ENTRY_ATTRIBUTE Entry;
    NTSTATUS Status;

    Entry = NtfsAllocateViewIndexEntry(&Header->SecurityId, sizeof(ULONG), Header, sizeof(*Header));
    if (Entry == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;
    Status = NtfsInsertIndexEntry(DeviceExt, DeviceExt->SecureMftIndex, L"$SII", 4, Entry, FALSE);
    ExFreePoolWithTag(Entry, TAG_NTFS);
    if (!NT_SUCCESS(Status))
        return Status;

    Entry = NtfsAllocateViewIndexEntry(&SdhKey, sizeof(SdhKey), Header, sizeof(*Header));
    if (Entry == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;
    Status = NtfsInsertIndexEntry(DeviceExt, DeviceExt->SecureMftIndex, L"$SDH", 4, Entry, FALSE);
    ExFreePoolWithTag(Entry, TAG_NTFS);

    return Status;
}

/**
* @name NtfsAppendDescriptor
*
* Stores a descriptor $Secure doesn't hold yet under the next SecurityId, after the last one.
*/
static
NTSTATUS
NtfsAppendDescriptor(PDEVICE_EXTENSION DeviceExt,
                     PSECURITY_DESCRIPTOR Sd,
                     ULONG SdLength,
                     ULONG Hash,
                     PULONG SecurityId)
{
    PNTFS_SECURITY_HEADER Entry;
    NTSTATUS Status;

    Entry = ExAllocatePoolWithTag(PagedPool, sizeof(*Entry) + SdLength, TAG_NTFS);
    if (Entry == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Entry->Hash = Hash;
    Entry->SecurityId = DeviceExt->NextSecurityId;
    Entry->Length = sizeof(*Entry) + SdLength;
    Entry->Offset = NtfsSdsOffsetFor(DeviceExt->SdsEnd, Entry->Length);
    RtlCopyMemory(Entry + 1, Sd, SdLength);

    Status = NtfsWriteSds(DeviceExt, Entry);
    if (NT_SUCCESS(Status))
        Status = NtfsIndexDescriptor(DeviceExt, Entry);

    if (NT_SUCCESS(Status))
    {
        *SecurityId = Entry->SecurityId;
        DeviceExt->NextSecurityId++;
        DeviceExt->SdsEnd = Entry->Offset + Entry->Length;
    }

    ExFreePoolWithTag(Entry, TAG_NTFS);

    return Status;
}

/**
* @name NtfsStoreDescriptor
*
* The SecurityId of a self-relative descriptor: the one it's stored under in $Secure, or a
* new one it gets stored under now.
*/
static
NTSTATUS
NtfsStoreDescriptor(PDEVICE_EXTENSION DeviceExt,
                    PSECURITY_DESCRIPTOR Sd,
                    PULONG SecurityId)
{
    ULONG SdLength = RtlLengthSecurityDescriptor(Sd);
    ULONG Hash = NtfsSecurityHash(Sd, SdLength);
    NTSTATUS Status;

    ASSERT(((PISECURITY_DESCRIPTOR)Sd)->Control & SE_SELF_RELATIVE);
    ASSERT(DeviceExt->SecureMftIndex != 0);

    /* Allocating SecurityIds and $SDS space, and the indexes, change together */
    KeEnterCriticalRegion();
    ExAcquireResourceExclusiveLite(&DeviceExt->IndexResource, TRUE);

    Status = NtfsFindStoredDescriptor(DeviceExt, Sd, SdLength, Hash, SecurityId);
    if (NT_SUCCESS(Status) && *SecurityId == 0)
        Status = NtfsAppendDescriptor(DeviceExt, Sd, SdLength, Hash, SecurityId);

    ExReleaseResourceLite(&DeviceExt->IndexResource);
    KeLeaveCriticalRegion();

    return Status;
}

/**
* @name NtfsGetSecurityId
*
* The SecurityId in a file's $STANDARD_INFORMATION, 0 if it has none.
*/
static
ULONG
NtfsGetSecurityId(PDEVICE_EXTENSION DeviceExt,
                  PFILE_RECORD_HEADER FileRecord)
{
    PSTANDARD_INFORMATION StdInfo;

    if (!NtfsHasNtfs3StandardInformation(DeviceExt, FileRecord))
        return 0;

    StdInfo = GetStandardInformationFromRecord(DeviceExt, FileRecord);
    return StdInfo != NULL ? StdInfo->SecurityId : 0;
}

/**
* @name NtfsReadOwnDescriptor
*
* Reads the $SECURITY_DESCRIPTOR attribute a file of NTFS before 3.0 carries.
*/
static
NTSTATUS
NtfsReadOwnDescriptor(PDEVICE_EXTENSION DeviceExt,
                      PFILE_RECORD_HEADER FileRecord,
                      PSECURITY_DESCRIPTOR *Sd)
{
    PNTFS_ATTR_CONTEXT Context;
    ULONGLONG Length;
    NTSTATUS Status;

    *Sd = NULL;

    Status = FindAttribute(DeviceExt, FileRecord, AttributeSecurityDescriptor, L"", 0, &Context, NULL);
    if (!NT_SUCCESS(Status))
        return STATUS_NO_SECURITY_ON_OBJECT;

    Length = AttributeDataLength(Context->pRecord);
    if (Length < sizeof(SECURITY_DESCRIPTOR_RELATIVE) || Length > NTFS_SDS_BLOCK_SIZE)
    {
        ReleaseAttributeContext(Context);
        return STATUS_FILE_CORRUPT_ERROR;
    }

    *Sd = ExAllocatePoolWithTag(PagedPool, (ULONG)Length, TAG_NTFS);
    if (*Sd == NULL)
        Status = STATUS_INSUFFICIENT_RESOURCES;
    else if (ReadAttribute(DeviceExt, Context, 0, *Sd, (ULONG)Length) != Length ||
             !RtlValidRelativeSecurityDescriptor(*Sd, (ULONG)Length, 0))
        Status = STATUS_FILE_CORRUPT_ERROR;

    ReleaseAttributeContext(Context);

    if (!NT_SUCCESS(Status) && *Sd != NULL)
    {
        ExFreePoolWithTag(*Sd, TAG_NTFS);
        *Sd = NULL;
    }

    return Status;
}

/**
* @name NtfsBuildOpenDescriptor
*
* The descriptor of a file that has none, which a volume formatted without security gives:
* owned by Administrators, everyone allowed everything.
*/
static
NTSTATUS
NtfsBuildOpenDescriptor(PSECURITY_DESCRIPTOR *Sd)
{
    UCHAR AclBuffer[sizeof(ACL) + FIELD_OFFSET(ACCESS_ALLOWED_ACE, SidStart) + SECURITY_MAX_SID_SIZE];
    SECURITY_DESCRIPTOR Absolute;
    PACL Acl = (PACL)AclBuffer;
    ULONG Length = 0;
    NTSTATUS Status;

    *Sd = NULL;

    RtlCreateSecurityDescriptor(&Absolute, SECURITY_DESCRIPTOR_REVISION);
    RtlCreateAcl(Acl, sizeof(AclBuffer), ACL_REVISION);
    RtlAddAccessAllowedAce(Acl, ACL_REVISION, FILE_ALL_ACCESS, SeExports->SeWorldSid);
    RtlSetDaclSecurityDescriptor(&Absolute, TRUE, Acl, FALSE);
    RtlSetOwnerSecurityDescriptor(&Absolute, SeExports->SeAliasAdminsSid, FALSE);
    RtlSetGroupSecurityDescriptor(&Absolute, SeExports->SeAliasAdminsSid, FALSE);

    RtlAbsoluteToSelfRelativeSD(&Absolute, NULL, &Length);
    *Sd = ExAllocatePoolWithTag(PagedPool, Length, TAG_NTFS);
    if (*Sd == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = RtlAbsoluteToSelfRelativeSD(&Absolute, *Sd, &Length);
    if (!NT_SUCCESS(Status))
    {
        ExFreePoolWithTag(*Sd, TAG_NTFS);
        *Sd = NULL;
    }

    return Status;
}

/**
* @name NtfsReadFileSecurity
* @implemented
*
* Reads the security descriptor of a file: the one in $Secure its SecurityId names, its own
* $SECURITY_DESCRIPTOR, or, with neither, one that lets everyone in. Free it with
* ExFreePoolWithTag(TAG_NTFS).
*/
NTSTATUS
NtfsReadFileSecurity(PDEVICE_EXTENSION DeviceExt,
                     ULONGLONG MftIndex,
                     PSECURITY_DESCRIPTOR *Sd)
{
    PFILE_RECORD_HEADER FileRecord;
    ULONG SecurityId, Length;
    NTSTATUS Status;

    *Sd = NULL;

    FileRecord = ExAllocateFromNPagedLookasideList(&DeviceExt->FileRecLookasideList);
    if (FileRecord == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = ReadFileRecord(DeviceExt, MftIndex, FileRecord);
    if (NT_SUCCESS(Status))
    {
        SecurityId = NtfsGetSecurityId(DeviceExt, FileRecord);
        if (SecurityId != 0 && DeviceExt->SecureMftIndex != 0)
            Status = NtfsReadDescriptorById(DeviceExt, SecurityId, Sd, &Length);
        else
            Status = NtfsReadOwnDescriptor(DeviceExt, FileRecord, Sd);
    }

    ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);

    if (Status == STATUS_NO_SECURITY_ON_OBJECT)
        Status = NtfsBuildOpenDescriptor(Sd);

    return Status;
}

/**
* @name NtfsSetSecurityIdOnRecord
*
* Makes a file's $STANDARD_INFORMATION name SecurityId, growing it to the NTFS 3.0 form if
* it is older, and drops a $SECURITY_DESCRIPTOR it carried. Writes the record.
*/
static
NTSTATUS
NtfsSetSecurityIdOnRecord(PDEVICE_EXTENSION DeviceExt,
                          PFILE_RECORD_HEADER FileRecord,
                          ULONG SecurityId)
{
    PNTFS_ATTR_CONTEXT Context;
    PSTANDARD_INFORMATION StdInfo;
    ULONG Offset, OldLength;
    NTSTATUS Status;

    Status = FindAttribute(DeviceExt, FileRecord, AttributeStandardInformation, L"", 0, &Context, &Offset);
    if (!NT_SUCCESS(Status))
        return STATUS_FILE_CORRUPT_ERROR;

    OldLength = (ULONG)AttributeDataLength(Context->pRecord);
    if (OldLength < sizeof(STANDARD_INFORMATION))
    {
        if (FileRecord->BytesInUse + ALIGN_UP_BY(sizeof(STANDARD_INFORMATION) - OldLength, ATTR_RECORD_ALIGNMENT) >
            DeviceExt->NtfsInfo.BytesPerFileRecord)
        {
            Status = STATUS_DISK_FULL;
        }
        else
        {
            Status = InternalSetResidentAttributeLength(DeviceExt, Context, FileRecord, Offset, sizeof(STANDARD_INFORMATION));
        }
    }
    ReleaseAttributeContext(Context);
    if (!NT_SUCCESS(Status))
        return Status;

    StdInfo = GetStandardInformationFromRecord(DeviceExt, FileRecord);
    ASSERT(StdInfo != NULL);
    if (OldLength < sizeof(STANDARD_INFORMATION))
        RtlZeroMemory((PUCHAR)StdInfo + OldLength, sizeof(STANDARD_INFORMATION) - OldLength);
    StdInfo->SecurityId = SecurityId;

    Status = NtfsDeleteAttributeFromFile(DeviceExt, FileRecord, AttributeSecurityDescriptor, L"", 0);
    if (!NT_SUCCESS(Status) && Status != STATUS_OBJECT_NAME_NOT_FOUND)
        return Status;

    return UpdateFileRecord(DeviceExt, FileRecord->MFTRecordNumber, FileRecord);
}

/**
* @name NtfsWriteFileSecurity
*
* Gives a file a new security descriptor, self-relative.
*/
static
NTSTATUS
NtfsWriteFileSecurity(PDEVICE_EXTENSION DeviceExt,
                      ULONGLONG MftIndex,
                      PSECURITY_DESCRIPTOR Sd)
{
    PFILE_RECORD_HEADER FileRecord;
    ULONG SecurityId;
    NTSTATUS Status;

    /* A volume before NTFS 3.0 would keep it in the file; this driver doesn't write those */
    if (DeviceExt->SecureMftIndex == 0)
        return STATUS_INVALID_DEVICE_REQUEST;

    Status = NtfsStoreDescriptor(DeviceExt, Sd, &SecurityId);
    if (!NT_SUCCESS(Status))
        return Status;

    FileRecord = ExAllocateFromNPagedLookasideList(&DeviceExt->FileRecLookasideList);
    if (FileRecord == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = ReadFileRecord(DeviceExt, MftIndex, FileRecord);
    if (NT_SUCCESS(Status))
        Status = NtfsSetSecurityIdOnRecord(DeviceExt, FileRecord, SecurityId);

    ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);

    return Status;
}

/**
* @name NtfsAssignSecurity
* @implemented
*
* The SecurityId of a file being created in the directory ParentMftIndex: the descriptor
* SeAssignSecurity() makes of the parent's (for what it passes on) and the one the creator
* gave, if any. 0 on a volume without $Secure.
*/
NTSTATUS
NtfsAssignSecurity(PDEVICE_EXTENSION DeviceExt,
                   ULONGLONG ParentMftIndex,
                   PACCESS_STATE AccessState,
                   BOOLEAN IsDirectory,
                   PULONG SecurityId)
{
    PSECURITY_DESCRIPTOR ParentSd, NewSd = NULL;
    NTSTATUS Status;

    *SecurityId = 0;

    if (DeviceExt->SecureMftIndex == 0)
        return STATUS_SUCCESS;

    Status = NtfsReadFileSecurity(DeviceExt, ParentMftIndex, &ParentSd);
    if (!NT_SUCCESS(Status))
        return Status;

    /* Like Windows, what a file inherits is marked inherited */
    Status = SeAssignSecurityEx(ParentSd,
                                AccessState->SecurityDescriptor,
                                &NewSd,
                                NULL,
                                IsDirectory,
                                SEF_DACL_AUTO_INHERIT | SEF_SACL_AUTO_INHERIT,
                                &AccessState->SubjectSecurityContext,
                                IoGetFileObjectGenericMapping(),
                                PagedPool);
    ExFreePoolWithTag(ParentSd, TAG_NTFS);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = NtfsStoreDescriptor(DeviceExt, NewSd, SecurityId);

    SeDeassignSecurity(&NewSd);

    return Status;
}

/**
* @name NtfsSecurityMftIndex
*
* The file record whose descriptor an open stands for: a volume open stands for $Volume.
*/
static
ULONGLONG
NtfsSecurityMftIndex(PNTFS_FCB Fcb)
{
    if (Fcb->Flags & (FCB_IS_VOLUME | FCB_IS_VOLUME_STREAM))
        return NTFS_FILE_VOLUME;

    return Fcb->MFTIndex;
}

/**
* @name NtfsQuerySecurity
* @implemented
*
* IRP_MJ_QUERY_SECURITY: the parts of the file's descriptor SecurityInformation asks for,
* self-relative, into the caller's buffer. A buffer too small gets STATUS_BUFFER_OVERFLOW
* with the length needed.
*/
NTSTATUS
NtfsQuerySecurity(PNTFS_IRP_CONTEXT IrpContext)
{
    PIRP Irp = IrpContext->Irp;
    PIO_STACK_LOCATION Stack = IrpContext->Stack;
    PDEVICE_EXTENSION DeviceExt = IrpContext->DeviceObject->DeviceExtension;
    PNTFS_FCB Fcb = Stack->FileObject->FsContext;
    PSECURITY_DESCRIPTOR Sd;
    ULONG Length = Stack->Parameters.QuerySecurity.Length;
    NTSTATUS Status;

    /* The file system device itself keeps no descriptor */
    if (Fcb == NULL || IrpContext->DeviceObject == NtfsGlobalData->DeviceObject)
        return STATUS_INVALID_DEVICE_REQUEST;

    ExAcquireResourceSharedLite(&Fcb->MainResource, TRUE);
    Status = NtfsReadFileSecurity(DeviceExt, NtfsSecurityMftIndex(Fcb), &Sd);
    ExReleaseResourceLite(&Fcb->MainResource);
    if (!NT_SUCCESS(Status))
        return Status;

    _SEH2_TRY
    {
        Status = SeQuerySecurityDescriptorInfo(&Stack->Parameters.QuerySecurity.SecurityInformation,
                                               Irp->UserBuffer,
                                               &Length,
                                               &Sd);
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = _SEH2_GetExceptionCode();
    }
    _SEH2_END;

    ExFreePoolWithTag(Sd, TAG_NTFS);

    if (Status == STATUS_BUFFER_TOO_SMALL)
        Status = STATUS_BUFFER_OVERFLOW;
    if (NT_SUCCESS(Status) || Status == STATUS_BUFFER_OVERFLOW)
        Irp->IoStatus.Information = Length;

    return Status;
}

/**
* @name NtfsSetSecurity
* @implemented
*
* IRP_MJ_SET_SECURITY: replaces the parts of the file's descriptor SecurityInformation names
* with those of the descriptor given, and keeps the rest.
*/
NTSTATUS
NtfsSetSecurity(PNTFS_IRP_CONTEXT IrpContext)
{
    PIO_STACK_LOCATION Stack = IrpContext->Stack;
    PDEVICE_EXTENSION DeviceExt = IrpContext->DeviceObject->DeviceExtension;
    PNTFS_FCB Fcb = Stack->FileObject->FsContext;
    PSECURITY_DESCRIPTOR OldSd, NewSd;
    ULONGLONG MftIndex;
    NTSTATUS Status;

    if (Fcb == NULL || IrpContext->DeviceObject == NtfsGlobalData->DeviceObject)
        return STATUS_INVALID_DEVICE_REQUEST;

    MftIndex = NtfsSecurityMftIndex(Fcb);

    if (!NtfsGlobalData->EnableWriteSupport)
        return STATUS_MEDIA_WRITE_PROTECTED;

    ExAcquireResourceExclusiveLite(&Fcb->MainResource, TRUE);

    Status = NtfsReadFileSecurity(DeviceExt, MftIndex, &OldSd);
    if (NT_SUCCESS(Status))
    {
        NewSd = OldSd;
        Status = SeSetSecurityDescriptorInfo(NULL,
                                             &Stack->Parameters.SetSecurity.SecurityInformation,
                                             Stack->Parameters.SetSecurity.SecurityDescriptor,
                                             &NewSd,
                                             PagedPool,
                                             IoGetFileObjectGenericMapping());
        if (NT_SUCCESS(Status))
        {
            Status = NtfsWriteFileSecurity(DeviceExt, MftIndex, NewSd);
            ExFreePool(NewSd);
        }

        if (NT_SUCCESS(Status))
            NtfsPostUsnChange(DeviceExt, Fcb, USN_REASON_SECURITY_CHANGE);

        ExFreePoolWithTag(OldSd, TAG_NTFS);
    }

    ExReleaseResourceLite(&Fcb->MainResource);

    if (NT_SUCCESS(Status))
        NtfsReportChange(Fcb, FILE_NOTIFY_CHANGE_SECURITY, FILE_ACTION_MODIFIED);

    return Status;
}

/* EOF */
