/*
 * COPYRIGHT:        See COPYING in the top level directory
 * PROJECT:          ReactOS kernel
 * FILE:             drivers/filesystem/ntfs/compress.c
 * PURPOSE:          NTFS compressed $DATA read dispatch (slice 1a).
 *
 * Walks an attribute's DataRunsMCB one compression unit (CU) at a time,
 * classifies each CU, and feeds compressed CUs to the LZNT1 codec in
 * lznt1.c.  The write path stays STATUS_NOT_IMPLEMENTED - slice 1b
 * will add that in a separate issue.
 *
 * Classification per CU (CU_clusters = 1 << CompressionUnit):
 *
 *   1. All clusters mapped, VCN-count == CU_clusters  -> stored raw,
 *      copy the CU verbatim from disk.
 *   2. Some clusters mapped, VCN-count <  CU_clusters (MCB trailer is
 *      the implicit sparse tail inside the CU)         -> compressed,
 *      read the mapped prefix as the compressed payload and run the
 *      LZNT1 decoder over it.
 *   3. All clusters unmapped (MCB returns a hole for the start VCN)
 *                                                      -> sparse CU,
 *      zero-fill the logical range.
 *
 * Kernel / userspace both drive this via the same pure-byte interface:
 *
 *   NTSTATUS NtfsCompressedReadLogical(Context, Offset, Len, Out);
 *
 * A separate callback-free overload, NtfsCompressedReadLogicalEx, lets
 * the userspace harness inject a raw-cluster reader so it can drive
 * the walker against synthetic LCNs without a kernel VCB.  The kernel
 * wrapper (NtfsCompressedReadLogical) simply plumbs NtfsReadDiskCached.
 */

/* INCLUDES *****************************************************************/

#include "ntfs.h"

#define NDEBUG
#include <debug.h>

/* FUNCTIONS ****************************************************************/

/**
 * @name NtfsCompressedReadLogicalEx
 * @implemented
 *
 * Core compressed-$DATA read worker.  Reads the raw on-disk CUs via
 * the caller-supplied RawRead callback (so userspace can inject a
 * file-backed reader), decompresses compressed ones, copies raw ones
 * verbatim, and zero-fills sparse CUs.  The caller owns Out[0..Len).
 *
 * @param Context   Attribute context for a non-resident compressed $DATA.
 *                  Context->DataRunsMCB must be populated; Context->pRecord
 *                  must carry a non-zero CompressionUnit and the on-disk
 *                  DataSize.
 * @param Offset    Logical byte offset into the attribute.
 * @param Len       Number of logical bytes to read.
 * @param Out       Destination buffer, at least Len bytes.
 * @param BytesPerCluster   Cluster size in bytes (from volume boot sector).
 * @param RawRead   Callback: (Context, Lcn_start, Len_bytes, Buffer, Ctx) ->
 *                  NTSTATUS.  Reads raw on-disk bytes starting at cluster Lcn.
 * @param RawCtx    Opaque context threaded to RawRead.
 * @param BytesReadOut  Optional: receives number of bytes actually written
 *                      to Out (may be < Len near EOF).
 *
 * @return STATUS_SUCCESS on success, STATUS_INVALID_PARAMETER for
 *         bogus args, STATUS_INSUFFICIENT_RESOURCES on ENOMEM,
 *         STATUS_DATA_ERROR on malformed compressed payload, or
 *         anything RawRead returns.
 */
NTSTATUS
NtfsCompressedReadLogicalEx(PNTFS_ATTR_CONTEXT Context,
                            ULONGLONG Offset,
                            ULONG Len,
                            PUCHAR Out,
                            ULONG BytesPerCluster,
                            NtfsCompressedRawReadFn RawRead,
                            PVOID RawCtx,
                            PULONG BytesReadOut)
{
    NTSTATUS Status = STATUS_SUCCESS;
    ULONG CuClusters;
    ULONG CuBytes;
    ULONGLONG DataSize;
    ULONGLONG EndOffset;
    ULONGLONG CurOffset;
    PUCHAR CuBuffer = NULL;
    PUCHAR DecompBuffer = NULL;
    ULONG BytesCopied = 0;

    if (Context == NULL || Out == NULL || Len == 0 ||
        RawRead == NULL || BytesPerCluster == 0)
        return STATUS_INVALID_PARAMETER;
    if (!Context->pRecord->IsNonResident)
        return STATUS_INVALID_PARAMETER;
    if (Context->pRecord->NonResident.CompressionUnit == 0)
        return STATUS_INVALID_PARAMETER;
    if (Context->pRecord->NonResident.CompressionUnit >= 16)
        return STATUS_INVALID_PARAMETER; /* Guards the 1 << shift below. */

    CuClusters = 1u << Context->pRecord->NonResident.CompressionUnit;
    CuBytes    = CuClusters * BytesPerCluster;

    DataSize = (ULONGLONG)Context->pRecord->NonResident.DataSize;
    if (Offset >= DataSize)
    {
        if (BytesReadOut)
            *BytesReadOut = 0;
        return STATUS_SUCCESS;
    }

    /* Clip read tail to DataSize.  Reads past EOF shorten. */
    EndOffset = Offset + Len;
    if (EndOffset > DataSize)
        EndOffset = DataSize;

    CuBuffer = ExAllocatePoolWithTag(NonPagedPool, CuBytes, TAG_NTFS);
    if (CuBuffer == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    DecompBuffer = ExAllocatePoolWithTag(NonPagedPool, CuBytes, TAG_NTFS);
    if (DecompBuffer == NULL)
    {
        ExFreePoolWithTag(CuBuffer, TAG_NTFS);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    CurOffset = Offset;
    while (CurOffset < EndOffset)
    {
        ULONGLONG CuIndex     = CurOffset / CuBytes;
        ULONGLONG CuByteStart = CuIndex * CuBytes;
        ULONGLONG CuByteEnd   = CuByteStart + CuBytes;
        ULONGLONG CopyStart   = CurOffset;
        ULONGLONG CopyEnd     = (CuByteEnd < EndOffset) ? CuByteEnd : EndOffset;
        ULONG     CopyLen     = (ULONG)(CopyEnd - CopyStart);
        ULONG     InCuOffset  = (ULONG)(CopyStart - CuByteStart);
        LONGLONG  CuVcnStart  = (LONGLONG)(CuIndex * CuClusters);
        PUCHAR    Source      = NULL;

        LONGLONG  Lcn = 0;
        LONGLONG  MappedCount = 0;
        BOOLEAN   Found;

        Found = FsRtlLookupLargeMcbEntry(&Context->DataRunsMCB,
                                         CuVcnStart,
                                         &Lcn, &MappedCount,
                                         NULL, NULL, NULL);

        if (!Found || Lcn == -1)
        {
            /* Case 3: sparse CU - zero-fill the caller's slice of it.
             * Also covers Found==FALSE (VCN not in any run), which on a
             * compressed attribute is likewise the implicit sparse tail. */
            RtlZeroMemory(Out + BytesCopied, CopyLen);
            BytesCopied += CopyLen;
            CurOffset = CopyEnd;
            continue;
        }

        /* The MCB returned (Lcn, MappedCount) where MappedCount is the
         * number of clusters starting at Lcn that belong to the current
         * run and still cover CuVcnStart.  A "compressed" CU is
         * identified by a run that ends BEFORE the CU's final cluster:
         * the implicit sparse tail makes the logical CU exactly
         * CuClusters long, but the physical storage is MappedCount < CuClusters.
         *
         * A "raw" CU has the run covering all CuClusters clusters (possibly
         * as part of a longer run - MappedCount can be > CuClusters, in
         * which case we still have the full physical coverage). */

        if (MappedCount >= (LONGLONG)CuClusters)
        {
            /* Case 1: raw CU, read verbatim and copy the requested slice. */
            Status = RawRead(Context, Lcn, CuBytes, CuBuffer, RawCtx);
            if (!NT_SUCCESS(Status))
                goto Cleanup;

            Source = CuBuffer + InCuOffset;
            RtlCopyMemory(Out + BytesCopied, Source, CopyLen);
            BytesCopied += CopyLen;
            CurOffset = CopyEnd;
            continue;
        }

        /* Case 2: compressed CU.  The compressed payload occupies the
         * first MappedCount clusters; the decoder handles short inputs
         * via the end-of-CU header marker.  We read exactly
         * MappedCount*BytesPerCluster bytes - not the full CU - because
         * the tail clusters are the implicit sparse encoding marker and
         * are not physically present. */
        {
            ULONG CompressedBytes = (ULONG)(MappedCount) * BytesPerCluster;
            ULONG OutUsed = 0;

            Status = RawRead(Context, Lcn, CompressedBytes, CuBuffer, RawCtx);
            if (!NT_SUCCESS(Status))
                goto Cleanup;

            Status = NtfsLznt1Decompress(CuBuffer, CompressedBytes,
                                         DecompBuffer, CuBytes, &OutUsed);
            if (!NT_SUCCESS(Status))
                goto Cleanup;

            Source = DecompBuffer + InCuOffset;
            RtlCopyMemory(Out + BytesCopied, Source, CopyLen);
            BytesCopied += CopyLen;
            CurOffset = CopyEnd;
            continue;
        }
    }

    Status = STATUS_SUCCESS;

Cleanup:
    if (CuBuffer)     ExFreePoolWithTag(CuBuffer, TAG_NTFS);
    if (DecompBuffer) ExFreePoolWithTag(DecompBuffer, TAG_NTFS);

    if (BytesReadOut)
        *BytesReadOut = BytesCopied;
    return Status;
}

/* Kernel RawRead callback: NtfsReadDiskCached against the volume. */
static NTSTATUS
NtfsCompressedReadKernelRun(PNTFS_ATTR_CONTEXT Context,
                            LONGLONG Lcn,
                            ULONG Length,
                            PUCHAR Buffer,
                            PVOID Ctx)
{
    PDEVICE_EXTENSION Vcb = (PDEVICE_EXTENSION)Ctx;
    LONGLONG Offset;

    UNREFERENCED_PARAMETER(Context);

    Offset = Lcn * (LONGLONG)Vcb->NtfsInfo.BytesPerCluster;
    return NtfsReadDiskCached(Vcb, Offset, Length, Buffer);
}

/**
 * @name NtfsCompressedReadLogical
 * @implemented
 *
 * Kernel entry point: reads logical bytes from a compressed
 * non-resident $DATA attribute.  Thin wrapper that plugs the
 * volume-level NtfsReadDiskCached into NtfsCompressedReadLogicalEx.
 *
 * Returns the number of logical bytes read.  ReadAttribute's contract
 * is "returns bytes read as ULONG, 0 on failure"; we mirror that at
 * the caller (mft.c) where this branches.
 */
NTSTATUS
NtfsCompressedReadLogical(PDEVICE_EXTENSION Vcb,
                          PNTFS_ATTR_CONTEXT Context,
                          ULONGLONG Offset,
                          ULONG Len,
                          PUCHAR Out,
                          PULONG BytesReadOut)
{
    if (Vcb == NULL)
        return STATUS_INVALID_PARAMETER;

    return NtfsCompressedReadLogicalEx(Context, Offset, Len, Out,
                                       Vcb->NtfsInfo.BytesPerCluster,
                                       NtfsCompressedReadKernelRun,
                                       (PVOID)Vcb,
                                       BytesReadOut);
}

/* EOF */
