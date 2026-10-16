/*
 * PROJECT:     ReactOS NTFS driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Disk quota: $Extend\$Quota, IRP_MJ_QUERY/SET_QUOTA, FileFsControlInformation
 * COPYRIGHT:   Copyright 2026 ReactOS Team
 */

/*
 * $Extend\$Quota keeps the quota entries in two view indexes: $O maps an owner's SID to its
 * owner ID, $Q an owner ID to its QUOTA_CONTROL_ENTRY, which holds the limits, the bytes
 * charged and, for a real owner, the SID again. Owner ID 1 is not an owner: its entry holds
 * the defaults and the volume's quota flags. Owners start at 0x100.
 *
 * What files charge to their owners isn't counted yet: BytesUsed stays where it was.
 */

/* INCLUDES *****************************************************************/

#include "ntfs.h"

#define NDEBUG
#include <debug.h>

/* GLOBALS *****************************************************************/

#define NTFS_QUOTA_DEFAULTS_ID      1
#define NTFS_QUOTA_FIRST_OWNER_ID   0x100
#define NTFS_QUOTA_VERSION          2

/* QUOTA_CONTROL_ENTRY flags: per owner, and the volume's in the defaults entry */
#define NTFS_QUOTA_FLAG_DEFAULT_LIMITS      0x00000001
#define NTFS_QUOTA_FLAG_USER_MASK           0x00000007
#define NTFS_QUOTA_FLAG_TRACKING_ENABLED    0x00000010
#define NTFS_QUOTA_FLAG_ENFORCEMENT_ENABLED 0x00000020
#define NTFS_QUOTA_FLAG_LOG_THRESHOLD       0x00000080
#define NTFS_QUOTA_FLAG_LOG_LIMIT           0x00000100
#define NTFS_QUOTA_FLAG_OUT_OF_DATE         0x00000200

/* A QuotaLimit that asks for the entry to go */
#define NTFS_QUOTA_DELETE           (-2LL)

/* The data of a $Q entry, the SID of a real owner behind it */
typedef struct _NTFS_QUOTA_CONTROL
{
    ULONG Version;
    ULONG Flags;
    ULONGLONG BytesUsed;
    LONGLONG ChangeTime;
    LONGLONG WarningLimit;
    LONGLONG HardLimit;
    LONGLONG ExceededTime;
} NTFS_QUOTA_CONTROL, *PNTFS_QUOTA_CONTROL;

C_ASSERT(sizeof(NTFS_QUOTA_CONTROL) == 48);

/* FUNCTIONS ****************************************************************/

/**
* @name NtfsLoadQuota
* @implemented
*
* Finds $Extend\$Quota at mount, and the owner ID the next new owner gets.
*/
VOID
NtfsLoadQuota(PDEVICE_EXTENSION Vcb)
{
    UNICODE_STRING Name = RTL_CONSTANT_STRING(L"$Quota");
    PINDEX_ENTRY_ATTRIBUTE Last;
    ULONG FirstEntry = 0;

    Vcb->QuotaMftIndex = 0;
    Vcb->NextQuotaOwnerId = NTFS_QUOTA_FIRST_OWNER_ID;

    if (Vcb->NtfsInfo.MajorVersion < 3)
        return;

    if (!NT_SUCCESS(NtfsFindMftRecord(Vcb, NTFS_FILE_EXTEND, &Name, &FirstEntry, FALSE, FALSE, &Vcb->QuotaMftIndex, NULL)))
    {
        Vcb->QuotaMftIndex = 0;
        return;
    }

    if (NT_SUCCESS(NtfsFindLastIndexEntry(Vcb, Vcb->QuotaMftIndex, L"$Q", 2, &Last)))
    {
        ULONG OwnerId = *(PULONG)&Last->FileName;

        Vcb->NextQuotaOwnerId = max(OwnerId + 1, NTFS_QUOTA_FIRST_OWNER_ID);
        ExFreePoolWithTag(Last, TAG_NTFS);
    }
}

/**
* @name NtfsReadQuotaEntry
*
* Reads the $Q entry of an owner ID: its control data and, if Sid isn't NULL, a pool copy of
* its SID (NULL for the defaults), freed with ExFreePoolWithTag(TAG_NTFS).
*/
static
NTSTATUS
NtfsReadQuotaEntry(PDEVICE_EXTENSION Vcb,
                   ULONG OwnerId,
                   PNTFS_QUOTA_CONTROL Control,
                   PSID *Sid)
{
    PINDEX_ENTRY_ATTRIBUTE Search, Found;
    NTSTATUS Status;

    if (Sid != NULL)
        *Sid = NULL;

    Search = NtfsAllocateViewIndexEntry(&OwnerId, sizeof(OwnerId), NULL, 0);
    if (Search == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = NtfsFindIndexEntry(Vcb, Vcb->QuotaMftIndex, L"$Q", 2, Search, &Found);
    ExFreePoolWithTag(Search, TAG_NTFS);
    if (!NT_SUCCESS(Status))
        return Status;

    if (Found->Data.ViewIndex.DataLength < sizeof(*Control))
    {
        Status = STATUS_FILE_CORRUPT_ERROR;
    }
    else
    {
        RtlCopyMemory(Control, (PUCHAR)Found + Found->Data.ViewIndex.DataOffset, sizeof(*Control));

        if (Sid != NULL && Found->Data.ViewIndex.DataLength > sizeof(*Control))
        {
            ULONG SidLength = Found->Data.ViewIndex.DataLength - sizeof(*Control);
            PSID Stored = (PUCHAR)Found + Found->Data.ViewIndex.DataOffset + sizeof(*Control);

            if (!RtlValidSid(Stored) || RtlLengthSid(Stored) > SidLength)
            {
                Status = STATUS_FILE_CORRUPT_ERROR;
            }
            else
            {
                *Sid = ExAllocatePoolWithTag(PagedPool, RtlLengthSid(Stored), TAG_NTFS);
                if (*Sid == NULL)
                    Status = STATUS_INSUFFICIENT_RESOURCES;
                else
                    RtlCopySid(RtlLengthSid(Stored), *Sid, Stored);
            }
        }
    }

    ExFreePoolWithTag(Found, TAG_NTFS);

    return Status;
}

/**
* @name NtfsWriteQuotaEntry
*
* Replaces, or adds, the $Q entry of an owner ID.
*/
static
NTSTATUS
NtfsWriteQuotaEntry(PDEVICE_EXTENSION Vcb,
                    ULONG OwnerId,
                    PNTFS_QUOTA_CONTROL Control,
                    PSID Sid)
{
    ULONG SidLength = (Sid != NULL) ? RtlLengthSid(Sid) : 0;
    PINDEX_ENTRY_ATTRIBUTE Entry;
    PUCHAR Data;
    NTSTATUS Status;

    Data = ExAllocatePoolWithTag(PagedPool, sizeof(*Control) + SidLength, TAG_NTFS);
    if (Data == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    RtlCopyMemory(Data, Control, sizeof(*Control));
    if (Sid != NULL)
        RtlCopySid(SidLength, Data + sizeof(*Control), Sid);

    Entry = NtfsAllocateViewIndexEntry(&OwnerId, sizeof(OwnerId), Data, (USHORT)(sizeof(*Control) + SidLength));
    ExFreePoolWithTag(Data, TAG_NTFS);
    if (Entry == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = NtfsRemoveIndexEntry(Vcb, Vcb->QuotaMftIndex, L"$Q", 2, Entry, FALSE);
    if (NT_SUCCESS(Status) || Status == STATUS_OBJECT_NAME_NOT_FOUND)
        Status = NtfsInsertIndexEntry(Vcb, Vcb->QuotaMftIndex, L"$Q", 2, Entry, FALSE);

    ExFreePoolWithTag(Entry, TAG_NTFS);

    return Status;
}

/**
* @name NtfsFindQuotaOwner
*
* The owner ID $O gives a SID, STATUS_OBJECT_NAME_NOT_FOUND for a SID without an entry.
*/
static
NTSTATUS
NtfsFindQuotaOwner(PDEVICE_EXTENSION Vcb,
                   PSID Sid,
                   PULONG OwnerId)
{
    PINDEX_ENTRY_ATTRIBUTE Search, Found;
    NTSTATUS Status;

    Search = NtfsAllocateViewIndexEntry(Sid, (USHORT)RtlLengthSid(Sid), NULL, 0);
    if (Search == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = NtfsFindIndexEntry(Vcb, Vcb->QuotaMftIndex, L"$O", 2, Search, &Found);
    ExFreePoolWithTag(Search, TAG_NTFS);
    if (!NT_SUCCESS(Status))
        return Status;

    if (Found->Data.ViewIndex.DataLength < sizeof(ULONG))
        Status = STATUS_FILE_CORRUPT_ERROR;
    else
        *OwnerId = *(PULONG)((PUCHAR)Found + Found->Data.ViewIndex.DataOffset);

    ExFreePoolWithTag(Found, TAG_NTFS);

    return Status;
}

/**
* @name NtfsSetOwnerQuota
*
* Applies one FILE_QUOTA_INFORMATION: new limits for an owner, who gets an entry if it has
* none, or, with a QuotaLimit of -2, the end of its entry.
*/
static
NTSTATUS
NtfsSetOwnerQuota(PDEVICE_EXTENSION Vcb,
                  PFILE_QUOTA_INFORMATION Quota)
{
    NTFS_QUOTA_CONTROL Control;
    PINDEX_ENTRY_ATTRIBUTE Entry;
    LARGE_INTEGER Now;
    ULONG OwnerId;
    NTSTATUS Status;

    Status = NtfsFindQuotaOwner(Vcb, &Quota->Sid, &OwnerId);

    if (Quota->QuotaLimit.QuadPart == NTFS_QUOTA_DELETE)
    {
        if (Status == STATUS_OBJECT_NAME_NOT_FOUND)
            return STATUS_SUCCESS;
        if (!NT_SUCCESS(Status))
            return Status;

        Entry = NtfsAllocateViewIndexEntry(&OwnerId, sizeof(OwnerId), NULL, 0);
        if (Entry == NULL)
            return STATUS_INSUFFICIENT_RESOURCES;
        Status = NtfsRemoveIndexEntry(Vcb, Vcb->QuotaMftIndex, L"$Q", 2, Entry, FALSE);
        ExFreePoolWithTag(Entry, TAG_NTFS);
        if (!NT_SUCCESS(Status))
            return Status;

        Entry = NtfsAllocateViewIndexEntry(&Quota->Sid, (USHORT)Quota->SidLength, NULL, 0);
        if (Entry == NULL)
            return STATUS_INSUFFICIENT_RESOURCES;
        Status = NtfsRemoveIndexEntry(Vcb, Vcb->QuotaMftIndex, L"$O", 2, Entry, FALSE);
        ExFreePoolWithTag(Entry, TAG_NTFS);
        return Status;
    }

    KeQuerySystemTime(&Now);

    if (Status == STATUS_OBJECT_NAME_NOT_FOUND)
    {
        /* A new owner */
        OwnerId = Vcb->NextQuotaOwnerId;
        RtlZeroMemory(&Control, sizeof(Control));
        Control.Version = NTFS_QUOTA_VERSION;

        Entry = NtfsAllocateViewIndexEntry(&Quota->Sid, (USHORT)Quota->SidLength, &OwnerId, sizeof(OwnerId));
        if (Entry == NULL)
            return STATUS_INSUFFICIENT_RESOURCES;
        Status = NtfsInsertIndexEntry(Vcb, Vcb->QuotaMftIndex, L"$O", 2, Entry, FALSE);
        ExFreePoolWithTag(Entry, TAG_NTFS);
        if (!NT_SUCCESS(Status))
            return Status;

        Vcb->NextQuotaOwnerId++;
    }
    else if (NT_SUCCESS(Status))
    {
        Status = NtfsReadQuotaEntry(Vcb, OwnerId, &Control, NULL);
    }

    if (!NT_SUCCESS(Status))
        return Status;

    Control.Flags &= ~NTFS_QUOTA_FLAG_DEFAULT_LIMITS;
    Control.ChangeTime = Now.QuadPart;
    Control.WarningLimit = Quota->QuotaThreshold.QuadPart;
    Control.HardLimit = Quota->QuotaLimit.QuadPart;

    return NtfsWriteQuotaEntry(Vcb, OwnerId, &Control, &Quota->Sid);
}

/**
* @name NtfsCheckQuotaList
*
* Checks a FILE_QUOTA_INFORMATION list (MS-FSCC 2.4.36): every entry inside it, with a valid
* SID of the length it gives.
*/
static
NTSTATUS
NtfsCheckQuotaList(PFILE_QUOTA_INFORMATION List,
                   ULONG Length)
{
    PUCHAR End = (PUCHAR)List + Length;
    PFILE_QUOTA_INFORMATION Quota = List;

    for (;;)
    {
        if ((PUCHAR)Quota + FIELD_OFFSET(FILE_QUOTA_INFORMATION, Sid) > End ||
            (PUCHAR)&Quota->Sid + Quota->SidLength > End ||
            Quota->SidLength < FIELD_OFFSET(SID, SubAuthority) ||
            !RtlValidSid(&Quota->Sid) ||
            RtlLengthSid(&Quota->Sid) != Quota->SidLength)
        {
            return STATUS_INVALID_PARAMETER;
        }

        if (Quota->NextEntryOffset == 0)
            return STATUS_SUCCESS;
        Quota = (PFILE_QUOTA_INFORMATION)((PUCHAR)Quota + Quota->NextEntryOffset);
    }
}

/**
* @name NtfsSetQuota
* @implemented
*
* IRP_MJ_SET_QUOTA (MS-FSA 2.1.5.21): sets the limits of the owners the list names.
*/
NTSTATUS
NtfsSetQuota(PNTFS_IRP_CONTEXT IrpContext)
{
    PIRP Irp = IrpContext->Irp;
    PIO_STACK_LOCATION Stack = IrpContext->Stack;
    PDEVICE_EXTENSION Vcb = IrpContext->DeviceObject->DeviceExtension;
    PFILE_QUOTA_INFORMATION Quota = Irp->AssociatedIrp.SystemBuffer;
    NTSTATUS Status;

    if (IrpContext->DeviceObject == NtfsGlobalData->DeviceObject)
        return STATUS_INVALID_DEVICE_REQUEST;
    if (Vcb->QuotaMftIndex == 0)
        return STATUS_INVALID_DEVICE_REQUEST;
    if (!NtfsGlobalData->EnableWriteSupport)
        return STATUS_MEDIA_WRITE_PROTECTED;
    if (Quota == NULL || Stack->Parameters.SetQuota.Length == 0)
        return STATUS_INVALID_PARAMETER;

    Status = NtfsCheckQuotaList(Quota, Stack->Parameters.SetQuota.Length);
    if (!NT_SUCCESS(Status))
        return Status;

    /* The owner IDs and both indexes change together */
    KeEnterCriticalRegion();
    ExAcquireResourceExclusiveLite(&Vcb->IndexResource, TRUE);

    for (;;)
    {
        Status = NtfsSetOwnerQuota(Vcb, Quota);
        if (!NT_SUCCESS(Status) || Quota->NextEntryOffset == 0)
            break;
        Quota = (PFILE_QUOTA_INFORMATION)((PUCHAR)Quota + Quota->NextEntryOffset);
    }

    ExReleaseResourceLite(&Vcb->IndexResource);
    KeLeaveCriticalRegion();

    return Status;
}

/* Where a quota query's answer goes */
typedef struct _NTFS_QUOTA_OUTPUT
{
    PUCHAR Buffer;
    ULONG Length;
    ULONG Used;
    PFILE_QUOTA_INFORMATION Last;
    BOOLEAN Single;
    BOOLEAN Full;
    ULONG NextOwnerId;      /* where an enumeration goes on */
    NTSTATUS Status;
} NTFS_QUOTA_OUTPUT, *PNTFS_QUOTA_OUTPUT;

/**
* @name NtfsReturnQuota
*
* Appends a FILE_QUOTA_INFORMATION to the answer, FALSE when it doesn't fit.
*/
static
BOOLEAN
NtfsReturnQuota(PNTFS_QUOTA_OUTPUT Output,
                PSID Sid,
                PNTFS_QUOTA_CONTROL Control)
{
    ULONG Start = ALIGN_UP_BY(Output->Used, sizeof(LONGLONG));
    ULONG SidLength = RtlLengthSid(Sid);
    ULONG Size = FIELD_OFFSET(FILE_QUOTA_INFORMATION, Sid) + SidLength;
    PFILE_QUOTA_INFORMATION Quota;

    if (Start + Size > Output->Length || (Output->Single && Output->Last != NULL))
        return FALSE;

    Quota = (PFILE_QUOTA_INFORMATION)(Output->Buffer + Start);
    RtlZeroMemory(Quota, Size);
    Quota->SidLength = SidLength;
    Quota->ChangeTime.QuadPart = Control->ChangeTime;
    Quota->QuotaUsed.QuadPart = Control->BytesUsed;
    Quota->QuotaThreshold.QuadPart = Control->WarningLimit;
    Quota->QuotaLimit.QuadPart = Control->HardLimit;
    RtlCopySid(SidLength, &Quota->Sid, Sid);

    if (Output->Last != NULL)
        Output->Last->NextEntryOffset = (ULONG)((PUCHAR)Quota - (PUCHAR)Output->Last);
    Output->Last = Quota;
    Output->Used = Start + Size;

    return TRUE;
}

/**
* @name NtfsQuotaVisitor
*
* Returns each real owner's entry of $Q, in owner ID order, while they fit.
*/
static
BOOLEAN
NtfsQuotaVisitor(PINDEX_ENTRY_ATTRIBUTE Entry,
                 PVOID Context)
{
    PNTFS_QUOTA_OUTPUT Output = Context;
    ULONG OwnerId = *(PULONG)&Entry->FileName;
    NTFS_QUOTA_CONTROL Control;
    PSID Sid;

    if (Entry->Data.ViewIndex.DataLength < sizeof(Control) + FIELD_OFFSET(SID, SubAuthority))
    {
        Output->Status = STATUS_FILE_CORRUPT_ERROR;
        return FALSE;
    }

    RtlCopyMemory(&Control, (PUCHAR)Entry + Entry->Data.ViewIndex.DataOffset, sizeof(Control));
    Sid = (PUCHAR)Entry + Entry->Data.ViewIndex.DataOffset + sizeof(Control);

    if (!NtfsReturnQuota(Output, Sid, &Control))
    {
        Output->Full = TRUE;
        return FALSE;
    }

    Output->NextOwnerId = OwnerId + 1;
    return TRUE;
}

/**
* @name NtfsQueryQuotaBySid
*
* Answers a query that names its SIDs: their entries in the list's order, the defaults for
* an owner without one.
*/
static
NTSTATUS
NtfsQueryQuotaBySid(PDEVICE_EXTENSION Vcb,
                    PFILE_GET_QUOTA_INFORMATION List,
                    ULONG ListLength,
                    PNTFS_QUOTA_OUTPUT Output)
{
    PFILE_GET_QUOTA_INFORMATION Get = List;
    NTFS_QUOTA_CONTROL Control;
    ULONG OwnerId;
    NTSTATUS Status;

    for (;;)
    {
        if ((PUCHAR)&Get->Sid + Get->SidLength > (PUCHAR)List + ListLength ||
            Get->SidLength < FIELD_OFFSET(SID, SubAuthority) ||
            !RtlValidSid(&Get->Sid) || RtlLengthSid(&Get->Sid) != Get->SidLength)
        {
            return STATUS_INVALID_PARAMETER;
        }

        Status = NtfsFindQuotaOwner(Vcb, &Get->Sid, &OwnerId);
        if (Status == STATUS_OBJECT_NAME_NOT_FOUND)
            OwnerId = NTFS_QUOTA_DEFAULTS_ID;
        else if (!NT_SUCCESS(Status))
            return Status;

        Status = NtfsReadQuotaEntry(Vcb, OwnerId, &Control, NULL);
        if (!NT_SUCCESS(Status))
            return Status;
        if (OwnerId == NTFS_QUOTA_DEFAULTS_ID)
        {
            Control.BytesUsed = 0;
            Control.ChangeTime = 0;
        }

        if (!NtfsReturnQuota(Output, &Get->Sid, &Control))
            return (Output->Last == NULL) ? STATUS_BUFFER_TOO_SMALL : STATUS_BUFFER_OVERFLOW;

        if (Get->NextEntryOffset == 0 || Output->Single)
            return STATUS_SUCCESS;
        Get = (PFILE_GET_QUOTA_INFORMATION)((PUCHAR)Get + Get->NextEntryOffset);
    }
}

/**
* @name NtfsQueryQuota
* @implemented
*
* IRP_MJ_QUERY_QUOTA (MS-FSA 2.1.5.20): the entries a SID list names, or the volume's
* entries from the handle's cursor (SL_RESTART_SCAN starts over): STATUS_NO_MORE_ENTRIES
* once they are all returned.
*/
NTSTATUS
NtfsQueryQuota(PNTFS_IRP_CONTEXT IrpContext)
{
    PIRP Irp = IrpContext->Irp;
    PIO_STACK_LOCATION Stack = IrpContext->Stack;
    PDEVICE_EXTENSION Vcb = IrpContext->DeviceObject->DeviceExtension;
    PNTFS_CCB Ccb = Stack->FileObject->FsContext2;
    NTFS_QUOTA_OUTPUT Output;
    ULONG Low, High = MAXULONG;
    PINDEX_ENTRY_ATTRIBUTE LowEntry = NULL, HighEntry = NULL;
    PVOID UserBuffer;
    NTSTATUS Status;

    if (IrpContext->DeviceObject == NtfsGlobalData->DeviceObject || Ccb == NULL)
        return STATUS_INVALID_DEVICE_REQUEST;
    if (Vcb->QuotaMftIndex == 0)
        return STATUS_INVALID_DEVICE_REQUEST;

    RtlZeroMemory(&Output, sizeof(Output));
    Output.Length = Stack->Parameters.QueryQuota.Length;
    Output.Single = BooleanFlagOn(Stack->Flags, SL_RETURN_SINGLE_ENTRY);
    Output.Buffer = ExAllocatePoolWithTag(PagedPool, max(Output.Length, 1), TAG_NTFS);
    if (Output.Buffer == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    if (Stack->Parameters.QueryQuota.SidList != NULL)
    {
        Status = NtfsQueryQuotaBySid(Vcb, Stack->Parameters.QueryQuota.SidList,
                                     Stack->Parameters.QueryQuota.SidListLength, &Output);
    }
    else
    {
        if (Stack->Flags & SL_RESTART_SCAN)
            Ccb->QuotaOwnerId = NTFS_QUOTA_FIRST_OWNER_ID;
        Low = max(Ccb->QuotaOwnerId, NTFS_QUOTA_FIRST_OWNER_ID);
        Output.NextOwnerId = Low;

        LowEntry = NtfsAllocateViewIndexEntry(&Low, sizeof(Low), NULL, 0);
        HighEntry = NtfsAllocateViewIndexEntry(&High, sizeof(High), NULL, 0);
        if (LowEntry == NULL || HighEntry == NULL)
            Status = STATUS_INSUFFICIENT_RESOURCES;
        else
            Status = NtfsVisitIndexEntries(Vcb, Vcb->QuotaMftIndex, L"$Q", 2, LowEntry, HighEntry,
                                           NtfsQuotaVisitor, &Output);
        if (NT_SUCCESS(Status))
            Status = Output.Status;

        if (NT_SUCCESS(Status))
        {
            if (Output.Last == NULL)
                Status = Output.Full ? STATUS_BUFFER_TOO_SMALL : STATUS_NO_MORE_ENTRIES;
            Ccb->QuotaOwnerId = Output.NextOwnerId;
        }
    }

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

    if (LowEntry != NULL)
        ExFreePoolWithTag(LowEntry, TAG_NTFS);
    if (HighEntry != NULL)
        ExFreePoolWithTag(HighEntry, TAG_NTFS);
    ExFreePoolWithTag(Output.Buffer, TAG_NTFS);

    return Status;
}

/* FILE_VC_* flags and the defaults entry's flags they are kept in */
static const struct
{
    ULONG ControlFlag;
    ULONG QuotaFlag;
} NtfsQuotaFlagMap[] =
{
    { FILE_VC_QUOTA_TRACK, NTFS_QUOTA_FLAG_TRACKING_ENABLED },
    { FILE_VC_QUOTA_ENFORCE, NTFS_QUOTA_FLAG_ENFORCEMENT_ENABLED },
    { FILE_VC_LOG_QUOTA_THRESHOLD, NTFS_QUOTA_FLAG_LOG_THRESHOLD },
    { FILE_VC_LOG_QUOTA_LIMIT, NTFS_QUOTA_FLAG_LOG_LIMIT },
    { FILE_VC_QUOTAS_INCOMPLETE, NTFS_QUOTA_FLAG_OUT_OF_DATE },
};

/**
* @name NtfsQueryFsControl
* @implemented
*
* FileFsControlInformation: the volume's quota flags and default limits, from the defaults
* entry of $Q.
*/
NTSTATUS
NtfsQueryFsControl(PDEVICE_EXTENSION Vcb,
                   PFILE_FS_CONTROL_INFORMATION Info,
                   PULONG BufferLength)
{
    NTFS_QUOTA_CONTROL Control;
    NTSTATUS Status;
    ULONG i;

    if (*BufferLength < sizeof(FILE_FS_CONTROL_INFORMATION))
        return STATUS_BUFFER_TOO_SMALL;
    if (Vcb->QuotaMftIndex == 0)
        return STATUS_INVALID_PARAMETER;

    Status = NtfsReadQuotaEntry(Vcb, NTFS_QUOTA_DEFAULTS_ID, &Control, NULL);
    if (!NT_SUCCESS(Status))
        return Status;

    RtlZeroMemory(Info, sizeof(*Info));
    Info->DefaultQuotaThreshold.QuadPart = Control.WarningLimit;
    Info->DefaultQuotaLimit.QuadPart = Control.HardLimit;
    for (i = 0; i < RTL_NUMBER_OF(NtfsQuotaFlagMap); i++)
    {
        if (Control.Flags & NtfsQuotaFlagMap[i].QuotaFlag)
            Info->FileSystemControlFlags |= NtfsQuotaFlagMap[i].ControlFlag;
    }

    *BufferLength -= sizeof(FILE_FS_CONTROL_INFORMATION);

    return STATUS_SUCCESS;
}

/**
* @name NtfsSetFsControl
* @implemented
*
* FileFsControlInformation set: the volume's quota flags and default limits.
*/
NTSTATUS
NtfsSetFsControl(PDEVICE_EXTENSION Vcb,
                 PFILE_FS_CONTROL_INFORMATION Info,
                 ULONG BufferLength)
{
    NTFS_QUOTA_CONTROL Control;
    LARGE_INTEGER Now;
    NTSTATUS Status;
    ULONG i;

    if (BufferLength < sizeof(FILE_FS_CONTROL_INFORMATION))
        return STATUS_INVALID_PARAMETER;
    if (Vcb->QuotaMftIndex == 0)
        return STATUS_INVALID_PARAMETER;
    if (!NtfsGlobalData->EnableWriteSupport)
        return STATUS_MEDIA_WRITE_PROTECTED;

    KeEnterCriticalRegion();
    ExAcquireResourceExclusiveLite(&Vcb->IndexResource, TRUE);

    Status = NtfsReadQuotaEntry(Vcb, NTFS_QUOTA_DEFAULTS_ID, &Control, NULL);
    if (NT_SUCCESS(Status))
    {
        KeQuerySystemTime(&Now);
        for (i = 0; i < RTL_NUMBER_OF(NtfsQuotaFlagMap); i++)
        {
            if (Info->FileSystemControlFlags & NtfsQuotaFlagMap[i].ControlFlag)
                Control.Flags |= NtfsQuotaFlagMap[i].QuotaFlag;
            else
                Control.Flags &= ~NtfsQuotaFlagMap[i].QuotaFlag;
        }
        Control.WarningLimit = Info->DefaultQuotaThreshold.QuadPart;
        Control.HardLimit = Info->DefaultQuotaLimit.QuadPart;
        Control.ChangeTime = Now.QuadPart;
        Status = NtfsWriteQuotaEntry(Vcb, NTFS_QUOTA_DEFAULTS_ID, &Control, NULL);
    }

    ExReleaseResourceLite(&Vcb->IndexResource);
    KeLeaveCriticalRegion();

    return Status;
}

/* EOF */
