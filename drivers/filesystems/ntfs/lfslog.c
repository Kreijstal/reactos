/*
 * PROJECT:     ReactOS NTFS driver
 * LICENSE:     GPL-2.0-or-later (see COPYING)
 * PURPOSE:     Default-disabled LFS write-ahead emission (b4825bc14c1)
 *
 * This is experimental emission, not recovery. No user-facing switch
 * enables it. The caller must quiesce the volume before changing sessions.
 * Storage barriers are supplied by the mount layer; the codec has no
 * dependency on cache-manager or storage-device implementation details.
 */
#include "lfswrite.h"
#define NDEBUG
#include <debug.h>

typedef struct _NTFS_LOG_WRITER
{
    PDEVICE_EXTENSION Vcb;
    ERESOURCE Resource;
    BOOLEAN InIo;
    PNTFS_LOG_WRITE_CALLBACK Write;
    PNTFS_LOG_FLUSH_CALLBACK Flush;
    PNTFS_LOG_DIRTY_CALLBACK SetDirty;
    PNTFS_ATTR_CONTEXT Data;
    LFS_WRITE_CONTEXT Codec;
    BOOLEAN Enabled;
    BOOLEAN Paused;
    BOOLEAN PreviouslyDirty;
} NTFS_LOG_WRITER, *PNTFS_LOG_WRITER;

NTSTATUS
NtfsLfsCreateWriter(PDEVICE_EXTENSION Vcb,
                    PNTFS_LOG_WRITE_CALLBACK Write,
                    PNTFS_LOG_FLUSH_CALLBACK Flush,
                    PNTFS_LOG_DIRTY_CALLBACK SetDirty,
                    PVOID *WriterOut)
{
    PNTFS_LOG_WRITER Writer;
    NTSTATUS Status;

    *WriterOut = NULL;
    Writer = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Writer), TAG_NTFS);
    if (Writer == NULL) return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(Writer, sizeof(*Writer));
    Status = ExInitializeResourceLite(&Writer->Resource);
    if (!NT_SUCCESS(Status))
    {
        ExFreePoolWithTag(Writer, TAG_NTFS);
        return Status;
    }
    Writer->Vcb = Vcb;
    Writer->Write = Write;
    Writer->Flush = Flush;
    Writer->SetDirty = SetDirty;
    *WriterOut = Writer;
    return STATUS_SUCCESS;
}

/* $LogFile has a fixed, fully allocated mapping for this session. Writing
 * mapped runs cannot recurse through a read of its own MFT record. */
static NTSTATUS
NtfsLogWriteRange(PNTFS_LOG_WRITER Writer, ULONG Offset, ULONG Length)
{
    PDEVICE_EXTENSION Vcb = Writer->Vcb;
    ULONGLONG Position = Offset, Available;
    LONGLONG Lcn, RunLength;
    ULONG Within, Part;
    NTSTATUS Status;

    ASSERT(ExIsResourceAcquiredExclusiveLite(&Writer->Resource));
    ASSERT(Offset <= Writer->Codec.ImageLength && Length <= Writer->Codec.ImageLength - Offset);
    while (Length != 0)
    {
        if (!FsRtlLookupLargeMcbEntry(&Writer->Data->DataRunsMCB,
                                      Position / Vcb->NtfsInfo.BytesPerCluster,
                                      &Lcn, &RunLength, NULL, NULL, NULL) || Lcn < 0 ||
            (ULONGLONG)Lcn >= Vcb->NtfsInfo.ClusterCount || RunLength <= 0 ||
            (ULONGLONG)RunLength > Vcb->NtfsInfo.ClusterCount - Lcn)
            return STATUS_FILE_CORRUPT_ERROR;
        Within = Position % Vcb->NtfsInfo.BytesPerCluster;
        Available = (ULONGLONG)RunLength * Vcb->NtfsInfo.BytesPerCluster - Within;
        Part = (ULONG)min((ULONGLONG)Length, Available);
        Status = Writer->Write(Vcb, (ULONGLONG)Lcn * Vcb->NtfsInfo.BytesPerCluster + Within,
                                Part, Writer->Codec.Image + Position);
        if (!NT_SUCCESS(Status)) return Status;
        Position += Part;
        Length -= Part;
    }
    return STATUS_SUCCESS;
}

NTSTATUS
NtfsLfsEnableWriter(PVOID Context)
{
    PNTFS_LOG_WRITER Writer = Context;
    PDEVICE_EXTENSION Vcb = Writer->Vcb;
    PFILE_RECORD_HEADER Record;
    ULONGLONG Size;
    ULONG OffsetBits;
    NTSTATUS Status;

    ASSERT(ExIsResourceAcquiredExclusiveLite(&Vcb->IoResource));
    ASSERT(ExIsResourceAcquiredExclusiveLite(&Vcb->DirResource));
    ASSERT(!Writer->Enabled);
    if (Vcb->Flags & (VCB_DEVICE_REMOVED | VCB_VOLUME_READ_ONLY))
        return STATUS_MEDIA_WRITE_PROTECTED;
    Record = ExAllocateFromNPagedLookasideList(&Vcb->FileRecLookasideList);
    if (Record == NULL) return STATUS_INSUFFICIENT_RESOURCES;
    ExAcquireResourceExclusiveLite(&Writer->Resource, TRUE);
    if (Writer->Data != NULL) { ReleaseAttributeContext(Writer->Data); Writer->Data = NULL; }
    if (Writer->Codec.Image != NULL)
    {
        ExFreePoolWithTag(Writer->Codec.Image, TAG_NTFS);
        RtlZeroMemory(&Writer->Codec, sizeof(Writer->Codec));
    }
    Status = ReadFileRecord(Vcb, NTFS_FILE_LOGFILE, Record);
    if (NT_SUCCESS(Status))
        Status = FindAttribute(Vcb, Record, AttributeData, L"", 0, &Writer->Data, NULL);
    ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, Record);
    if (!NT_SUCCESS(Status)) goto Cleanup;
    Size = AttributeDataLength(Writer->Data->pRecord);
    if (!Writer->Data->pRecord->IsNonResident ||
        (Writer->Data->pRecord->Flags & (NTFS_ATTR_SPARSE | NTFS_ATTR_COMPRESSION_MASK)) ||
        Size > MAXULONG || Size < 6 * PAGE_SIZE || (Size & (PAGE_SIZE - 1)))
    {
        Status = STATUS_NOT_SUPPORTED;
        goto Cleanup;
    }
    Writer->Codec.Image = ExAllocatePoolWithTag(PagedPool, (ULONG)Size, TAG_NTFS);
    if (Writer->Codec.Image == NULL) { Status = STATUS_INSUFFICIENT_RESOURCES; goto Cleanup; }
    Status = NtfsLfsInitWriteContext(Vcb, &Writer->Codec, Writer->Codec.Image, (ULONG)Size, PAGE_SIZE);
    if (!NT_SUCCESS(Status)) goto Cleanup;
    OffsetBits = 64 - Writer->Codec.SeqNumberBits;
    Writer->Codec.CurrentSeqNumber = (Vcb->LogFileLsn >> OffsetBits) + 1;
    Writer->Codec.LastLsn = Vcb->LogFileLsn;
    Writer->PreviouslyDirty = BooleanFlagOn(Vcb->NtfsInfo.Flags, NTFS_VOLUME_FLAG_DIRTY);
    Writer->InIo = TRUE;
    Status = Writer->SetDirty(Vcb, TRUE);
    if (NT_SUCCESS(Status)) Status = Writer->Flush(Vcb);
    Writer->InIo = FALSE;
    if (NT_SUCCESS(Status)) Writer->Enabled = TRUE;
Cleanup:
    if (!NT_SUCCESS(Status))
    {
        if (Writer->Data != NULL) { ReleaseAttributeContext(Writer->Data); Writer->Data = NULL; }
        if (Writer->Codec.Image != NULL)
        {
            ExFreePoolWithTag(Writer->Codec.Image, TAG_NTFS);
            RtlZeroMemory(&Writer->Codec, sizeof(Writer->Codec));
        }
    }
    ExReleaseResourceLite(&Writer->Resource);
    return Status;
}

NTSTATUS
NtfsLfsLogFileRecord(PVOID Context, ULONGLONG MftIndex, PFILE_RECORD_HEADER Record)
{
    PNTFS_LOG_WRITER Writer = Context;
    PDEVICE_EXTENSION Vcb;
    PFILE_RECORD_HEADER Before;
    LFS_EMIT_PARAMS Params = {0};
    LONGLONG Lcn, RunLength, LastVcn;
    PLONGLONG Lcns;
    ULONGLONG ByteOffset, Lsn;
    ULONG Count, i, PageOffset;
    NTSTATUS Status;

    if (Writer == NULL || !Writer->Enabled || (Writer->InIo && ExIsResourceAcquiredExclusiveLite(&Writer->Resource)))
        return STATUS_SUCCESS;
    Vcb = Writer->Vcb;
    if (Vcb->NtfsInfo.BytesPerFileRecord > MAXUSHORT) return STATUS_LOG_FILE_FULL;
    Before = ExAllocateFromNPagedLookasideList(&Vcb->FileRecLookasideList);
    if (Before == NULL) return STATUS_INSUFFICIENT_RESOURCES;
    /* Capture before taking the writer resource: other metadata writers can
     * own MftContextResource while waiting for this writer. A slot the MFT
     * bitmap just handed out may never have been initialized: its undo image
     * is then its zeroes. */
    Status = NtfsReadFileRecordEx(Vcb, MftIndex, Before, TRUE);
    if (!NT_SUCCESS(Status)) goto FreeBefore;
    ByteOffset = MftIndex * Vcb->NtfsInfo.BytesPerFileRecord;
    Params.TargetVcn = ByteOffset / Vcb->NtfsInfo.BytesPerCluster;
    LastVcn = (ByteOffset + Vcb->NtfsInfo.BytesPerFileRecord - 1) / Vcb->NtfsInfo.BytesPerCluster;
    Count = (ULONG)(LastVcn - Params.TargetVcn + 1);
    Lcns = ExAllocatePoolWithTag(PagedPool, Count * sizeof(*Lcns), TAG_NTFS);
    if (Lcns == NULL) { Status = STATUS_INSUFFICIENT_RESOURCES; goto FreeBefore; }
    ExAcquireResourceSharedLite(&Vcb->MftContextResource, TRUE);
    for (i = 0; i < Count; i++)
    {
        if (!FsRtlLookupLargeMcbEntry(&Vcb->MFTContext->DataRunsMCB, Params.TargetVcn + i,
                                      &Lcn, &RunLength, NULL, NULL, NULL) || Lcn < 0)
        {
            Status = STATUS_FILE_CORRUPT_ERROR;
            break;
        }
        Lcns[i] = Lcn;
    }
    ExReleaseResourceLite(&Vcb->MftContextResource);
    if (!NT_SUCCESS(Status)) goto FreeLcns;
    Params.RedoOperation = NTFS_LFS_OP_INITIALIZE_FILE_RECORD_SEGMENT;
    Params.UndoOperation = NTFS_LFS_OP_INITIALIZE_FILE_RECORD_SEGMENT;
    Params.RedoData = Record;
    Params.UndoData = Before;
    Params.RedoLength = Params.UndoLength = (USHORT)Vcb->NtfsInfo.BytesPerFileRecord;
    Params.LcnsToFollow = (USHORT)Count;
    Params.Lcns = Lcns;
    Params.ClusterBlockOffset = (ByteOffset % Vcb->NtfsInfo.BytesPerCluster) / 512;
    Params.TargetBlockSize = Vcb->NtfsInfo.BytesPerFileRecord / 512;
    ExAcquireResourceExclusiveLite(&Writer->Resource, TRUE);
    Params.ClientPreviousLsn = Writer->Codec.LastLsn;
    Writer->InIo = TRUE;
    Status = NtfsLfsEmitRecord(&Writer->Codec, &Params, &Lsn);
    if (NT_SUCCESS(Status))
    {
        PageOffset = (NtfsLfsLsnToOffset(Writer->Codec.SeqNumberBits, Lsn) / PAGE_SIZE) * PAGE_SIZE;
        Status = NtfsLogWriteRange(Writer, PageOffset, PAGE_SIZE);
        if (NT_SUCCESS(Status)) Status = Writer->Flush(Vcb);
        if (NT_SUCCESS(Status)) Status = NtfsLogWriteRange(Writer, 0, 2 * PAGE_SIZE);
        if (NT_SUCCESS(Status)) Status = Writer->Flush(Vcb);
        if (NT_SUCCESS(Status)) Record->Ntfs.Lsn = Lsn;
    }
    Writer->InIo = FALSE;
    ExReleaseResourceLite(&Writer->Resource);
FreeLcns:
    ExFreePoolWithTag(Lcns, TAG_NTFS);
FreeBefore:
    ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, Before);
    return Status;
}

NTSTATUS
NtfsLfsFinishWriter(PVOID Context)
{
    PNTFS_LOG_WRITER Writer = Context;
    NTSTATUS Status;

    if (Writer == NULL || !Writer->Enabled) return STATUS_SUCCESS;
    ASSERT(ExIsResourceAcquiredExclusiveLite(&Writer->Vcb->IoResource) ||
           (Writer->Vcb->Flags & VCB_VOLUME_LOCKED));
    ASSERT(ExIsResourceAcquiredExclusiveLite(&Writer->Vcb->DirResource));
    ExAcquireResourceExclusiveLite(&Writer->Resource, TRUE);
    Writer->InIo = TRUE;
    /* Target metadata reaches storage before either restart is marked clean. */
    Status = Writer->Flush(Writer->Vcb);
    if (NT_SUCCESS(Status)) Status = NtfsLfsWriteRestart(&Writer->Codec, Writer->Vcb, TRUE);
    if (NT_SUCCESS(Status)) Status = NtfsLogWriteRange(Writer, 0, 2 * PAGE_SIZE);
    if (NT_SUCCESS(Status)) Status = Writer->Flush(Writer->Vcb);
    if (NT_SUCCESS(Status) && !Writer->PreviouslyDirty &&
        !(Writer->Vcb->Flags & VCB_VOLUME_CORRUPT))
    {
        Status = Writer->SetDirty(Writer->Vcb, FALSE);
        if (NT_SUCCESS(Status)) Status = Writer->Flush(Writer->Vcb);
    }
    if (NT_SUCCESS(Status))
    {
        Writer->Enabled = FALSE;
        Writer->Vcb->LogFileLsn = Writer->Codec.LastLsn;
    }
    Writer->InIo = FALSE;
    ExReleaseResourceLite(&Writer->Resource);
    return Status;
}

VOID
NtfsLfsDestroyWriter(PVOID Context)
{
    PNTFS_LOG_WRITER Writer = Context;

    if (Writer == NULL) return;
    if (Writer->Data != NULL) ReleaseAttributeContext(Writer->Data);
    if (Writer->Codec.Image != NULL) ExFreePoolWithTag(Writer->Codec.Image, TAG_NTFS);
    ExDeleteResourceLite(&Writer->Resource);
    ExFreePoolWithTag(Writer, TAG_NTFS);
}

BOOLEAN
NtfsLfsWriterEnabled(PVOID Context)
{
    PNTFS_LOG_WRITER Writer = Context;
    return Writer != NULL && Writer->Enabled;
}

NTSTATUS
NtfsLfsSuspendWriter(PVOID Context)
{
    PNTFS_LOG_WRITER Writer = Context;
    NTSTATUS Status;

    if (Writer == NULL || !Writer->Enabled) return STATUS_SUCCESS;
    Status = NtfsLfsFinishWriter(Writer);
    if (NT_SUCCESS(Status)) Writer->Paused = TRUE;
    return Status;
}

NTSTATUS
NtfsLfsResumeWriter(PVOID Context)
{
    PNTFS_LOG_WRITER Writer = Context;
    NTSTATUS Status;

    if (Writer == NULL || !Writer->Paused) return STATUS_SUCCESS;
    Status = NtfsLfsEnableWriter(Writer);
    if (NT_SUCCESS(Status)) Writer->Paused = FALSE;
    return Status;
}
