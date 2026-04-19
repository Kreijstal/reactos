/*
 * COPYRIGHT:        See COPYING in the top level directory
 * PROJECT:          ReactOS kernel
 * FILE:             drivers/filesystem/ntfs/quota.c
 * PURPOSE:          NTFS filesystem driver -- disk-quota first slice
 *                   (Kreijstal/reactos#37).
 *
 * On-disk layout (per libfsntfs + the well-known NTFS quota reference):
 *   \$Extend\$Quota  -- a file under the $Extend directory.
 *   $Q  index  -- keyed by 4-byte owner quota ID, values are QUOTA_CONTROL:
 *       version, flags, change_time, bytes_used, quota_threshold,
 *       quota_limit, exceeded_time, variable-length SID.
 *   $O  index  -- keyed by variable-length SID, values are owner quota IDs.
 *
 * Collation rules:
 *   $Q  COLLATION_NTOFS_ULONG  (owner quota ID, 4-byte LE)
 *   $O  COLLATION_NTOFS_SID    (variable-length SID bytes)
 *
 * Scope of this slice:
 *
 *   - Mount bootstrap: probe \$Extend\$Quota and set Vcb->QuotaPresent.
 *     Absent quota is the expected case on volumes formatted by ReactOS's
 *     mkntfs today, so a missing file is not a mount failure.
 *
 *   - Worker entry points (NtfsQuota{Set,Get,Enumerate,Delete}Entry).  These
 *     are the kernel-side primitives the dispatch.c IRP handlers delegate to.
 *     They now drive real $Q/$O B+trees via the generalised btree.c key API
 *     (NtfsBTree{Insert,Find,Remove}Blob + COLLATION_NTOFS_ULONG/SID).
 *
 * Explicitly DEFERRED (per issue #37 body):
 *   - per-file quota charging on write (bumping QUOTA_CONTROL.BytesUsed)
 *   - quota enforcement (STATUS_QUOTA_EXCEEDED)
 *   - default-quota entry (owner quota ID 0x100)
 *   - index allocation growth (the root-only case covers typical quotas)
 */

/* INCLUDES *****************************************************************/

#include "ntfs.h"

#define NDEBUG
#include <debug.h>

/* FUNCTIONS ****************************************************************/

/**
 * @name NtfsQuotaProbe
 * @implemented
 *
 * Called from NtfsMountVolume after the Vcb is otherwise wired up.  Walks
 * \$Extend\$Quota (if present) and caches a bool in Vcb->QuotaPresent.
 *
 * A missing \$Extend\$Quota is not a mount error -- the volume simply has
 * no quota metadata, which is the common case for ReactOS-formatted volumes.
 * Returns STATUS_SUCCESS either way; on genuine I/O failure it still
 * returns success with QuotaPresent=FALSE so mount succeeds.
 */
NTSTATUS
NtfsQuotaProbe(PDEVICE_EXTENSION Vcb)
{
    UNICODE_STRING Path;
    PFILE_RECORD_HEADER FileRecord = NULL;
    ULONGLONG MftIndex = 0;
    NTSTATUS Status;

    if (Vcb == NULL)
        return STATUS_INVALID_PARAMETER;

    Vcb->QuotaPresent = FALSE;
    Vcb->QuotaFileMft = 0;

    RtlInitUnicodeString(&Path, L"\\$Extend\\$Quota");
    Status = NtfsLookupFile(Vcb, &Path, FALSE, &FileRecord, &MftIndex);
    if (!NT_SUCCESS(Status))
    {
        /* Absent -- commonplace; not an error. */
        return STATUS_SUCCESS;
    }

    Vcb->QuotaPresent = TRUE;
    Vcb->QuotaFileMft = MftIndex;

    if (FileRecord != NULL)
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, FileRecord);

    return STATUS_SUCCESS;
}

/* ---------- internal: read a view index from \$Extend\$Quota ---------- */

typedef struct _QUOTA_INDEX_HANDLE
{
    PFILE_RECORD_HEADER FileRecord;         /* owned elsewhere if SharedFileRecord is TRUE */
    BOOLEAN SharedFileRecord;               /* don't free FileRecord on close */
    PNTFS_ATTR_CONTEXT IndexRootContext;
    ULONG IndexRootOffset;
    PINDEX_ROOT_ATTRIBUTE IndexRootData;
    ULONG IndexRootLen;
    PB_TREE Tree;
    const WCHAR *IndexName;
    ULONG IndexNameLen;
} QUOTA_INDEX_HANDLE, *PQUOTA_INDEX_HANDLE;

static NTSTATUS
NtfsQuotaOpenIndexWith(PDEVICE_EXTENSION Vcb,
                       PFILE_RECORD_HEADER SharedFileRecord,
                       PCWSTR IndexName,
                       ULONG IndexNameLen,
                       PQUOTA_INDEX_HANDLE Handle)
{
    NTSTATUS Status;

    RtlZeroMemory(Handle, sizeof(*Handle));
    Handle->IndexName = IndexName;
    Handle->IndexNameLen = IndexNameLen;

    if (SharedFileRecord)
    {
        Handle->FileRecord = SharedFileRecord;
        Handle->SharedFileRecord = TRUE;
    }
    else
    {
        Handle->FileRecord = ExAllocateFromNPagedLookasideList(&Vcb->FileRecLookasideList);
        if (!Handle->FileRecord)
            return STATUS_INSUFFICIENT_RESOURCES;

        Status = ReadFileRecord(Vcb, Vcb->QuotaFileMft, Handle->FileRecord);
        if (!NT_SUCCESS(Status))
            goto Error;
    }

    Status = FindAttribute(Vcb, Handle->FileRecord, AttributeIndexRoot,
                           IndexName, IndexNameLen,
                           &Handle->IndexRootContext, &Handle->IndexRootOffset);
    if (!NT_SUCCESS(Status))
        goto Error;

    Handle->IndexRootLen = (ULONG)AttributeDataLength(Handle->IndexRootContext->pRecord);
    Handle->IndexRootData = ExAllocatePoolWithTag(NonPagedPool, Handle->IndexRootLen, TAG_NTFS);
    if (!Handle->IndexRootData)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Error;
    }

    Status = ReadAttribute(Vcb, Handle->IndexRootContext, 0,
                           (PCHAR)Handle->IndexRootData, Handle->IndexRootLen);
    if (!NT_SUCCESS(Status))
        goto Error;

    Status = CreateBTreeFromIndexEx(Vcb, Handle->FileRecord,
                                    IndexName, IndexNameLen,
                                    Handle->IndexRootContext,
                                    Handle->IndexRootData,
                                    &Handle->Tree);
    if (!NT_SUCCESS(Status))
        goto Error;

    return STATUS_SUCCESS;

Error:
    if (Handle->IndexRootData)
        ExFreePoolWithTag(Handle->IndexRootData, TAG_NTFS);
    if (Handle->IndexRootContext)
        ReleaseAttributeContext(Handle->IndexRootContext);
    if (Handle->FileRecord && !Handle->SharedFileRecord)
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, Handle->FileRecord);
    RtlZeroMemory(Handle, sizeof(*Handle));
    return Status;
}

static NTSTATUS
NtfsQuotaOpenIndex(PDEVICE_EXTENSION Vcb,
                   PCWSTR IndexName,
                   ULONG IndexNameLen,
                   PQUOTA_INDEX_HANDLE Handle)
{
    return NtfsQuotaOpenIndexWith(Vcb, NULL, IndexName, IndexNameLen, Handle);
}

/* Persist an in-memory B+tree back into the file record's INDEX_ROOT
 * attribute and flush the MFT record to disk.  Only handles the root-only
 * case (no index allocation) — sufficient for the common quota sizes; see
 * the banner for deferred items. */
static NTSTATUS
NtfsQuotaFlushIndex(PDEVICE_EXTENSION Vcb, PQUOTA_INDEX_HANDLE Handle)
{
    PINDEX_ROOT_ATTRIBUTE NewIndexRoot = NULL;
    ULONG BtreeIndexLength = 0;
    ULONG AttributeLength;
    ULONG MaxIndexRootSize;
    ULONG LengthWritten = 0;
    NTSTATUS Status;
    PNTFS_ATTR_RECORD NextAttribute;

    /* Compute how much room we can give the index root. */
    MaxIndexRootSize = Vcb->NtfsInfo.BytesPerFileRecord
                       - Handle->IndexRootOffset
                       - Handle->IndexRootContext->pRecord->Resident.ValueOffset
                       - sizeof(INDEX_ROOT_ATTRIBUTE)
                       - (sizeof(ULONG) * 2);

    NextAttribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)Handle->FileRecord
                     + Handle->IndexRootOffset
                     + Handle->IndexRootContext->pRecord->Length);
    if (NextAttribute->Type != AttributeEnd)
    {
        ULONG LengthOfAttributes = 0;
        PNTFS_ATTR_RECORD CurrentAttribute = NextAttribute;
        while (CurrentAttribute->Type != AttributeEnd)
        {
            LengthOfAttributes += CurrentAttribute->Length;
            CurrentAttribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)CurrentAttribute + CurrentAttribute->Length);
        }
        if (MaxIndexRootSize > LengthOfAttributes)
            MaxIndexRootSize -= LengthOfAttributes;
        else
            return STATUS_INSUFFICIENT_RESOURCES;
    }

    Status = CreateIndexRootFromBTree(Vcb, Handle->Tree, MaxIndexRootSize,
                                      &NewIndexRoot, &BtreeIndexLength);
    if (!NT_SUCCESS(Status))
        return Status;

    AttributeLength = NewIndexRoot->Header.AllocatedSize + FIELD_OFFSET(INDEX_ROOT_ATTRIBUTE, Header);

    if (AttributeLength != Handle->IndexRootContext->pRecord->Resident.ValueLength)
    {
        Status = InternalSetResidentAttributeLength(Vcb, Handle->IndexRootContext,
                                                    Handle->FileRecord,
                                                    Handle->IndexRootOffset,
                                                    AttributeLength);
        if (!NT_SUCCESS(Status))
        {
            ExFreePoolWithTag(NewIndexRoot, TAG_NTFS);
            return Status;
        }
    }

    Status = WriteAttribute(Vcb, Handle->IndexRootContext, 0,
                            (PUCHAR)NewIndexRoot, AttributeLength,
                            &LengthWritten, Handle->FileRecord);
    ExFreePoolWithTag(NewIndexRoot, TAG_NTFS);
    if (!NT_SUCCESS(Status) || LengthWritten != AttributeLength)
        return NT_SUCCESS(Status) ? STATUS_UNSUCCESSFUL : Status;

    /* When the file record is shared with another index handle (e.g. $Q and
     * $O in the same transaction) the caller is responsible for the final
     * UpdateFileRecord once all handles have serialised their trees.  Calling
     * it here would clobber the other index's pending write. */
    if (!Handle->SharedFileRecord)
        Status = UpdateFileRecord(Vcb, Vcb->QuotaFileMft, Handle->FileRecord);
    return Status;
}

static VOID
NtfsQuotaCloseIndex(PDEVICE_EXTENSION Vcb, PQUOTA_INDEX_HANDLE Handle)
{
    if (Handle->Tree)
        DestroyBTree(Handle->Tree);
    if (Handle->IndexRootData)
        ExFreePoolWithTag(Handle->IndexRootData, TAG_NTFS);
    if (Handle->IndexRootContext)
        ReleaseAttributeContext(Handle->IndexRootContext);
    if (Handle->FileRecord && !Handle->SharedFileRecord)
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, Handle->FileRecord);
    RtlZeroMemory(Handle, sizeof(*Handle));
}

/* Walk $Q (owner-ID -> QUOTA_CONTROL) to determine the next free owner ID.
 * $O is a SID index pointing into $Q so we could also derive it from there,
 * but $Q is the authoritative source.  Windows allocates IDs monotonically
 * starting at 0x100, skipping over existing ones; we mirror that. */
static ULONG
NtfsQuotaPickNextOwnerId(PB_TREE QTree)
{
    PB_TREE_KEY Cur;
    ULONG Best = 0x100;  /* Windows convention */

    for (Cur = QTree->RootNode->FirstKey; Cur != NULL; Cur = Cur->NextKey)
    {
        if (!(Cur->IndexEntry->Flags & NTFS_INDEX_ENTRY_END) &&
            Cur->IndexEntry->KeyLength >= sizeof(ULONG))
        {
            ULONG OwnerId = 0;
            RtlCopyMemory(&OwnerId, &Cur->IndexEntry->FileName, sizeof(ULONG));
            if (OwnerId >= Best)
                Best = OwnerId + 1;
        }
    }
    return Best;
}

/**
 * @name NtfsQuotaSetEntry
 * @implemented
 *
 * Insert-or-update a quota entry keyed by SID.
 *   $O.Lookup(Sid) -> ownerQuotaId (allocating a fresh one on first insert)
 *   $Q.Update(ownerQuotaId, QUOTA_CONTROL(Limits, Sid))
 */
NTSTATUS
NtfsQuotaSetEntry(PDEVICE_EXTENSION Vcb,
                  const UCHAR *Sid,
                  ULONG SidLen,
                  const VOID *Limits,
                  ULONG LimitsLen)
{
    QUOTA_INDEX_HANDLE OHandle = {0};
    QUOTA_INDEX_HANDLE QHandle = {0};
    PFILE_RECORD_HEADER SharedFR = NULL;
    PINDEX_ENTRY_ATTRIBUTE Existing = NULL;
    ULONG OwnerId;
    NTSTATUS Status;

    if (Vcb == NULL || !Vcb->QuotaPresent || Sid == NULL || SidLen == 0 ||
        Limits == NULL || LimitsLen == 0)
        return STATUS_INVALID_PARAMETER;

    /* Single FileRecord shared across both index handles so each sees the
     * other's mutations before WriteAttribute/UpdateFileRecord serialises
     * to the MFT slot. */
    SharedFR = ExAllocateFromNPagedLookasideList(&Vcb->FileRecLookasideList);
    if (!SharedFR)
        return STATUS_INSUFFICIENT_RESOURCES;
    Status = ReadFileRecord(Vcb, Vcb->QuotaFileMft, SharedFR);
    if (!NT_SUCCESS(Status))
    {
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, SharedFR);
        return Status;
    }

    Status = NtfsQuotaOpenIndexWith(Vcb, SharedFR, L"$O", 2, &OHandle);
    if (!NT_SUCCESS(Status))
    {
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, SharedFR);
        return Status;
    }
    Status = NtfsQuotaOpenIndexWith(Vcb, SharedFR, L"$Q", 2, &QHandle);
    if (!NT_SUCCESS(Status))
        goto Done;

    /* Does this SID already have an entry? */
    Status = NtfsBTreeFindBlob(OHandle.Tree, Sid, SidLen, &Existing);
    if (NT_SUCCESS(Status))
    {
        /* ViewIndex value is the 4-byte OwnerId. */
        PUCHAR ValPtr = (PUCHAR)Existing + Existing->Data.ViewIndex.DataOffset;
        RtlCopyMemory(&OwnerId, ValPtr, sizeof(ULONG));
    }
    else if (Status == STATUS_OBJECT_NAME_NOT_FOUND)
    {
        OwnerId = NtfsQuotaPickNextOwnerId(QHandle.Tree);
        Status = NtfsBTreeInsertBlob(OHandle.Tree, Sid, SidLen,
                                     &OwnerId, sizeof(OwnerId),
                                     0, Vcb->NtfsInfo.BytesPerIndexRecord);
        if (!NT_SUCCESS(Status))
            goto Done;
    }
    else
    {
        goto Done;
    }

    /* Write (or overwrite) the $Q entry for OwnerId. */
    Status = NtfsBTreeInsertBlob(QHandle.Tree,
                                 &OwnerId, sizeof(OwnerId),
                                 Limits, LimitsLen,
                                 0, Vcb->NtfsInfo.BytesPerIndexRecord);
    if (!NT_SUCCESS(Status))
        goto Done;

    /* Persist both indexes into the shared FileRecord, then flush the
     * file record to disk once at the end. */
    Status = NtfsQuotaFlushIndex(Vcb, &OHandle);
    if (!NT_SUCCESS(Status))
        goto Done;
    Status = NtfsQuotaFlushIndex(Vcb, &QHandle);
    if (!NT_SUCCESS(Status))
        goto Done;

    Status = UpdateFileRecord(Vcb, Vcb->QuotaFileMft, SharedFR);

Done:
    NtfsQuotaCloseIndex(Vcb, &QHandle);
    NtfsQuotaCloseIndex(Vcb, &OHandle);
    if (SharedFR)
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, SharedFR);
    return Status;
}

/**
 * @name NtfsQuotaGetEntry
 * @implemented
 *
 * Look up a quota entry by SID.
 *   $O.Lookup(Sid) -> ownerQuotaId
 *   $Q.Lookup(ownerQuotaId) -> QUOTA_CONTROL
 */
NTSTATUS
NtfsQuotaGetEntry(PDEVICE_EXTENSION Vcb,
                  const UCHAR *Sid,
                  ULONG SidLen,
                  PVOID Limits,
                  ULONG LimitsCap,
                  PULONG LimitsOut)
{
    QUOTA_INDEX_HANDLE OHandle = {0};
    QUOTA_INDEX_HANDLE QHandle = {0};
    PINDEX_ENTRY_ATTRIBUTE Existing = NULL;
    ULONG OwnerId = 0;
    NTSTATUS Status;

    if (LimitsOut != NULL)
        *LimitsOut = 0;
    if (Vcb == NULL || !Vcb->QuotaPresent || Sid == NULL || SidLen == 0)
        return STATUS_INVALID_PARAMETER;

    Status = NtfsQuotaOpenIndex(Vcb, L"$O", 2, &OHandle);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = NtfsBTreeFindBlob(OHandle.Tree, Sid, SidLen, &Existing);
    if (!NT_SUCCESS(Status))
        goto Done;

    RtlCopyMemory(&OwnerId, (PUCHAR)Existing + Existing->Data.ViewIndex.DataOffset,
                  sizeof(ULONG));

    Status = NtfsQuotaOpenIndex(Vcb, L"$Q", 2, &QHandle);
    if (!NT_SUCCESS(Status))
        goto Done;

    Status = NtfsBTreeFindBlob(QHandle.Tree, &OwnerId, sizeof(OwnerId), &Existing);
    if (!NT_SUCCESS(Status))
        goto Done;

    {
        ULONG ValLen = Existing->Data.ViewIndex.DataLength;
        if (LimitsOut != NULL)
            *LimitsOut = ValLen;
        if (Limits != NULL && LimitsCap >= ValLen)
        {
            RtlCopyMemory(Limits, (PUCHAR)Existing + Existing->Data.ViewIndex.DataOffset, ValLen);
        }
        else if (Limits != NULL)
        {
            Status = STATUS_BUFFER_TOO_SMALL;
        }
    }

Done:
    NtfsQuotaCloseIndex(Vcb, &QHandle);
    NtfsQuotaCloseIndex(Vcb, &OHandle);
    return Status;
}

/**
 * @name NtfsQuotaEnumerate
 * @implemented
 *
 * Enumerate all entries in $Q starting at @p StartEntry (owner quota ID).
 * Returns concatenated QUOTA_CONTROL blobs, each prefixed by a 4-byte LE
 * OwnerQuotaId to make re-keying trivial for the caller.
 */
NTSTATUS
NtfsQuotaEnumerate(PDEVICE_EXTENSION Vcb,
                   ULONG StartEntry,
                   PVOID Buf,
                   ULONG BufLen,
                   PULONG BytesRet)
{
    QUOTA_INDEX_HANDLE QHandle = {0};
    PB_TREE_KEY Cur;
    ULONG Written = 0;
    NTSTATUS Status;

    if (BytesRet != NULL)
        *BytesRet = 0;
    if (Vcb == NULL || !Vcb->QuotaPresent)
        return STATUS_INVALID_PARAMETER;

    Status = NtfsQuotaOpenIndex(Vcb, L"$Q", 2, &QHandle);
    if (!NT_SUCCESS(Status))
        return Status;

    for (Cur = QHandle.Tree->RootNode->FirstKey; Cur != NULL; Cur = Cur->NextKey)
    {
        ULONG OwnerId = 0;
        ULONG ValLen;
        ULONG NeededLen;

        if (Cur->IndexEntry->Flags & NTFS_INDEX_ENTRY_END)
            continue;
        if (Cur->IndexEntry->KeyLength < sizeof(ULONG))
            continue;

        RtlCopyMemory(&OwnerId, &Cur->IndexEntry->FileName, sizeof(ULONG));
        if (OwnerId < StartEntry)
            continue;

        ValLen = Cur->IndexEntry->Data.ViewIndex.DataLength;
        NeededLen = sizeof(ULONG) + ValLen;
        if (Written + NeededLen > BufLen)
        {
            Status = STATUS_BUFFER_OVERFLOW;
            break;
        }

        if (Buf != NULL)
        {
            RtlCopyMemory((PUCHAR)Buf + Written, &OwnerId, sizeof(ULONG));
            RtlCopyMemory((PUCHAR)Buf + Written + sizeof(ULONG),
                          (PUCHAR)Cur->IndexEntry + Cur->IndexEntry->Data.ViewIndex.DataOffset,
                          ValLen);
        }
        Written += NeededLen;
    }

    if (BytesRet != NULL)
        *BytesRet = Written;

    NtfsQuotaCloseIndex(Vcb, &QHandle);
    return Status;
}

/**
 * @name NtfsQuotaDeleteEntry
 * @implemented
 *
 * Remove an entry from both $Q and $O.
 */
NTSTATUS
NtfsQuotaDeleteEntry(PDEVICE_EXTENSION Vcb,
                     const UCHAR *Sid,
                     ULONG SidLen)
{
    QUOTA_INDEX_HANDLE OHandle = {0};
    QUOTA_INDEX_HANDLE QHandle = {0};
    PFILE_RECORD_HEADER SharedFR = NULL;
    PINDEX_ENTRY_ATTRIBUTE Existing = NULL;
    ULONG OwnerId = 0;
    NTSTATUS Status;

    if (Vcb == NULL || !Vcb->QuotaPresent || Sid == NULL || SidLen == 0)
        return STATUS_INVALID_PARAMETER;

    SharedFR = ExAllocateFromNPagedLookasideList(&Vcb->FileRecLookasideList);
    if (!SharedFR)
        return STATUS_INSUFFICIENT_RESOURCES;
    Status = ReadFileRecord(Vcb, Vcb->QuotaFileMft, SharedFR);
    if (!NT_SUCCESS(Status))
    {
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, SharedFR);
        return Status;
    }

    Status = NtfsQuotaOpenIndexWith(Vcb, SharedFR, L"$O", 2, &OHandle);
    if (!NT_SUCCESS(Status))
    {
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, SharedFR);
        return Status;
    }

    Status = NtfsBTreeFindBlob(OHandle.Tree, Sid, SidLen, &Existing);
    if (!NT_SUCCESS(Status))
        goto Done;

    RtlCopyMemory(&OwnerId, (PUCHAR)Existing + Existing->Data.ViewIndex.DataOffset, sizeof(ULONG));

    Status = NtfsBTreeRemoveBlob(OHandle.Tree, Sid, SidLen);
    if (!NT_SUCCESS(Status))
        goto Done;

    Status = NtfsQuotaOpenIndexWith(Vcb, SharedFR, L"$Q", 2, &QHandle);
    if (!NT_SUCCESS(Status))
        goto Done;

    /* Best-effort — OK if not found. */
    (void)NtfsBTreeRemoveBlob(QHandle.Tree, &OwnerId, sizeof(OwnerId));

    Status = NtfsQuotaFlushIndex(Vcb, &OHandle);
    if (!NT_SUCCESS(Status))
        goto Done;
    Status = NtfsQuotaFlushIndex(Vcb, &QHandle);
    if (!NT_SUCCESS(Status))
        goto Done;

    Status = UpdateFileRecord(Vcb, Vcb->QuotaFileMft, SharedFR);

Done:
    NtfsQuotaCloseIndex(Vcb, &QHandle);
    NtfsQuotaCloseIndex(Vcb, &OHandle);
    if (SharedFR)
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, SharedFR);
    return Status;
}

/* EOF */
