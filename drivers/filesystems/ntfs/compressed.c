/*
 * PROJECT:     ReactOS NTFS driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Compressed streams: LZNT1 units, FSCTL_GET/SET_COMPRESSION
 * COPYRIGHT:   Copyright 2026 ReactOS Team
 */

/*
 * A compressed stream is a non-resident $DATA flagged LZNT1-compressed, cut into units of
 * 16 clusters. Each unit is stored one of three ways, which its runs tell apart:
 *   - all 16 clusters allocated: the data as it is;
 *   - fewer, followed by a hole: the LZNT1-compressed data;
 *   - no cluster at all: zeros.
 * A unit is always read and written whole: a write decompresses the unit, changes it,
 * compresses it again and gives it the clusters that takes.
 */

/* INCLUDES *****************************************************************/

#include "ntfs.h"

#define NDEBUG
#include <debug.h>

/* GLOBALS *****************************************************************/

#define NTFS_LZNT1_CHUNK_SIZE   0x1000

/* FUNCTIONS ****************************************************************/

/* Size in bytes of a unit of the stream */
static
ULONG
NtfsUnitBytes(PDEVICE_EXTENSION Vcb,
              PNTFS_ATTR_CONTEXT Context)
{
    return NtfsSparseUnitClusters(Context) * Vcb->NtfsInfo.BytesPerCluster;
}

/**
* @name NtfsUnitClusters
*
* How many clusters the unit starting at UnitVcn has; they are its first ones.
*/
static
ULONG
NtfsUnitClusters(PNTFS_ATTR_CONTEXT Context,
                 ULONGLONG UnitVcn)
{
    ULONG Unit = NtfsSparseUnitClusters(Context);
    ULONGLONG Vcn = UnitVcn;
    ULONG Allocated = 0;

    while (Vcn < UnitVcn + Unit)
    {
        LONGLONG Lbn, Count;

        if (!FsRtlLookupLargeMcbEntry(&Context->DataRunsMCB, Vcn, &Lbn, &Count, NULL, NULL, NULL))
            break;
        Count = min(Count, (LONGLONG)(UnitVcn + Unit - Vcn));
        if (Lbn != -1)
            Allocated += (ULONG)Count;
        Vcn += Count;
    }

    return Allocated;
}

/**
* @name NtfsReadUnit
*
* Reads the unit starting at UnitVcn into Unit, NtfsUnitBytes() long: as it is, from LZNT1,
* or zeros.
*/
static
NTSTATUS
NtfsReadUnit(PDEVICE_EXTENSION Vcb,
             PNTFS_ATTR_CONTEXT Context,
             ULONGLONG UnitVcn,
             PUCHAR Unit)
{
    ULONG BytesPerCluster = Vcb->NtfsInfo.BytesPerCluster;
    ULONG UnitBytes = NtfsUnitBytes(Vcb, Context);
    ULONG Clusters = NtfsUnitClusters(Context, UnitVcn);
    PUCHAR Compressed;
    ULONG Final = 0;
    NTSTATUS Status;

    if (Clusters == 0)
    {
        RtlZeroMemory(Unit, UnitBytes);
        return STATUS_SUCCESS;
    }

    if (Clusters == NtfsSparseUnitClusters(Context))
    {
        if (ReadAttribute(Vcb, Context, UnitVcn * BytesPerCluster, (PCHAR)Unit, UnitBytes) != UnitBytes)
            return STATUS_FILE_CORRUPT_ERROR;
        return STATUS_SUCCESS;
    }

    Compressed = ExAllocatePoolWithTag(NonPagedPool, Clusters * BytesPerCluster, TAG_NTFS);
    if (Compressed == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    if (ReadAttribute(Vcb, Context, UnitVcn * BytesPerCluster, (PCHAR)Compressed, Clusters * BytesPerCluster) !=
        Clusters * BytesPerCluster)
    {
        Status = STATUS_FILE_CORRUPT_ERROR;
    }
    else
    {
        Status = RtlDecompressBuffer(COMPRESSION_FORMAT_LZNT1, Unit, UnitBytes,
                                     Compressed, Clusters * BytesPerCluster, &Final);
        if (!NT_SUCCESS(Status))
            Status = STATUS_FILE_CORRUPT_ERROR;
    }

    /* A unit whose end was all zeros stores less than a unit */
    if (NT_SUCCESS(Status) && Final < UnitBytes)
        RtlZeroMemory(Unit + Final, UnitBytes - Final);

    ExFreePoolWithTag(Compressed, TAG_NTFS);

    return Status;
}

/**
* @name NtfsReadCompressed
* @implemented
*
* Reads Length bytes at Offset of a compressed stream, inside its data size.
*/
NTSTATUS
NtfsReadCompressed(PDEVICE_EXTENSION Vcb,
                   PNTFS_ATTR_CONTEXT Context,
                   ULONGLONG Offset,
                   PUCHAR Buffer,
                   ULONG Length)
{
    ULONG UnitBytes = NtfsUnitBytes(Vcb, Context);
    ULONG BytesPerCluster = Vcb->NtfsInfo.BytesPerCluster;
    PUCHAR Unit;
    NTSTATUS Status = STATUS_SUCCESS;

    Unit = ExAllocatePoolWithTag(NonPagedPool, UnitBytes, TAG_NTFS);
    if (Unit == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    while (Length != 0 && NT_SUCCESS(Status))
    {
        ULONGLONG UnitStart = Offset - Offset % UnitBytes;
        ULONG InUnit = (ULONG)(Offset - UnitStart);
        ULONG Piece = min(Length, UnitBytes - InUnit);

        Status = NtfsReadUnit(Vcb, Context, UnitStart / BytesPerCluster, Unit);
        if (NT_SUCCESS(Status))
            RtlCopyMemory(Buffer, Unit + InUnit, Piece);

        Buffer += Piece;
        Offset += Piece;
        Length -= Piece;
    }

    ExFreePoolWithTag(Unit, TAG_NTFS);

    return Status;
}

/**
* @name NtfsStoreUnit
*
* Stores a unit, its first Valid bytes being what the stream holds there: nothing for a
* unit of zeros, the LZNT1 form in as few clusters as that takes when that saves one, the
* data as it is otherwise. The caller writes Holder back.
*/
static
NTSTATUS
NtfsStoreUnit(PDEVICE_EXTENSION Vcb,
              PNTFS_ATTR_CONTEXT Context,
              ULONG AttrOffset,
              PFILE_RECORD_HEADER Holder,
              ULONGLONG UnitVcn,
              PUCHAR Unit,
              ULONG Valid,
              PUCHAR Compressed,
              PVOID Workspace)
{
    ULONG BytesPerCluster = Vcb->NtfsInfo.BytesPerCluster;
    ULONG UnitClusters = NtfsSparseUnitClusters(Context);
    ULONG UnitBytes = UnitClusters * BytesPerCluster;
    ULONG CompressedSize = 0, Clusters, Written, i;
    PUCHAR Data = Unit;
    BOOLEAN Zero = TRUE;
    NTSTATUS Status;

    /* What lies past the end of the stream is no part of it */
    RtlZeroMemory(Unit + Valid, UnitBytes - Valid);

    for (i = 0; i < Valid && Zero; i++)
        Zero = (Unit[i] == 0);

    if (Zero)
    {
        Clusters = 0;
    }
    else if (NT_SUCCESS(RtlCompressBuffer(COMPRESSION_FORMAT_LZNT1 | COMPRESSION_ENGINE_STANDARD,
                                          Unit, ALIGN_UP_BY(Valid, NTFS_LZNT1_CHUNK_SIZE),
                                          Compressed, UnitBytes, NTFS_LZNT1_CHUNK_SIZE,
                                          &CompressedSize, Workspace)) &&
             ALIGN_UP_BY(CompressedSize, BytesPerCluster) < UnitBytes)
    {
        Clusters = ALIGN_UP_BY(CompressedSize, BytesPerCluster) / BytesPerCluster;
        RtlZeroMemory(Compressed + CompressedSize, Clusters * BytesPerCluster - CompressedSize);
        Data = Compressed;
    }
    else
    {
        Clusters = UnitClusters;
    }

    /* The unit gets exactly the clusters it needs, at its start */
    Status = NtfsDeallocateSparseRange(Vcb, Context, AttrOffset, Holder, UnitVcn, UnitClusters);
    if (NT_SUCCESS(Status) && Clusters != 0)
        Status = NtfsAllocateSparseRange(Vcb, Context, AttrOffset, Holder, UnitVcn, Clusters);
    if (NT_SUCCESS(Status) && Clusters != 0)
        Status = WriteAttribute(Vcb, Context, UnitVcn * BytesPerCluster, Data, Clusters * BytesPerCluster, &Written, Holder);

    return Status;
}

/**
* @name NtfsWriteCompressed
* @implemented
*
* Writes Length bytes at Offset of a compressed stream, inside its data size, or zeros when
* Buffer is NULL, unit by unit. Writes the record holding the stream.
*/
NTSTATUS
NtfsWriteCompressed(PDEVICE_EXTENSION Vcb,
                    PNTFS_ATTR_CONTEXT Context,
                    ULONG AttrOffset,
                    PFILE_RECORD_HEADER Holder,
                    ULONGLONG Offset,
                    const UCHAR *Buffer,
                    ULONG Length)
{
    ULONG BytesPerCluster = Vcb->NtfsInfo.BytesPerCluster;
    ULONG UnitBytes = NtfsUnitBytes(Vcb, Context);
    ULONGLONG DataSize = AttributeDataLength(Context->pRecord);
    ULONG WorkspaceSize, FragmentSize;
    PUCHAR Unit = NULL, Compressed = NULL;
    PVOID Workspace = NULL;
    NTSTATUS Status;

    ASSERT(Offset + Length <= DataSize);

    Status = RtlGetCompressionWorkSpaceSize(COMPRESSION_FORMAT_LZNT1 | COMPRESSION_ENGINE_STANDARD,
                                            &WorkspaceSize, &FragmentSize);
    if (NT_SUCCESS(Status))
    {
        Unit = ExAllocatePoolWithTag(NonPagedPool, UnitBytes, TAG_NTFS);
        Compressed = ExAllocatePoolWithTag(NonPagedPool, UnitBytes, TAG_NTFS);
        Workspace = ExAllocatePoolWithTag(NonPagedPool, WorkspaceSize, TAG_NTFS);
        if (Unit == NULL || Compressed == NULL || Workspace == NULL)
            Status = STATUS_INSUFFICIENT_RESOURCES;
    }

    while (Length != 0 && NT_SUCCESS(Status))
    {
        ULONGLONG UnitStart = Offset - Offset % UnitBytes;
        ULONG InUnit = (ULONG)(Offset - UnitStart);
        ULONG Piece = min(Length, UnitBytes - InUnit);

        /* Only a unit the write doesn't cover whole is read first */
        if (Piece != UnitBytes)
            Status = NtfsReadUnit(Vcb, Context, UnitStart / BytesPerCluster, Unit);

        if (NT_SUCCESS(Status))
        {
            if (Buffer != NULL)
                RtlCopyMemory(Unit + InUnit, Buffer, Piece);
            else
                RtlZeroMemory(Unit + InUnit, Piece);

            Status = NtfsStoreUnit(Vcb, Context, AttrOffset, Holder, UnitStart / BytesPerCluster, Unit,
                                   (ULONG)min(UnitBytes, DataSize - UnitStart), Compressed, Workspace);
        }

        if (Buffer != NULL)
            Buffer += Piece;
        Offset += Piece;
        Length -= Piece;
    }

    if (NT_SUCCESS(Status))
        Status = UpdateFileRecord(Vcb, Context->FileMFTIndex & NTFS_MFT_MASK, Holder);

    if (Workspace != NULL)
        ExFreePoolWithTag(Workspace, TAG_NTFS);
    if (Compressed != NULL)
        ExFreePoolWithTag(Compressed, TAG_NTFS);
    if (Unit != NULL)
        ExFreePoolWithTag(Unit, TAG_NTFS);

    return Status;
}

/**
* @name NtfsZeroStreamRange
* @implemented
*
* Makes [Start, End) of a stream read as zeros: what a file grows by.
*/
NTSTATUS
NtfsZeroStreamRange(PDEVICE_EXTENSION Vcb,
                    PNTFS_ATTR_CONTEXT Context,
                    ULONG AttrOffset,
                    PFILE_RECORD_HEADER Holder,
                    ULONGLONG Start,
                    ULONGLONG End)
{
    if (Start >= End)
        return STATUS_SUCCESS;

    if (NtfsAttributeIsCompressed(Context))
        return NtfsWriteCompressed(Vcb, Context, AttrOffset, Holder, Start, NULL, (ULONG)(End - Start));

    return NtfsZeroAttributeRange(Vcb, Context, Holder, Start, End);
}

/* A file's stream, read for a change */
typedef struct _NTFS_COMPRESSION_EDIT
{
    PFILE_RECORD_HEADER FileRecord;
    PFILE_RECORD_HEADER Holder;
    PNTFS_ATTR_CONTEXT Context;
    ULONG Offset;
} NTFS_COMPRESSION_EDIT, *PNTFS_COMPRESSION_EDIT;

static
NTSTATUS
NtfsOpenCompressionEdit(PDEVICE_EXTENSION Vcb,
                        PNTFS_FCB Fcb,
                        PNTFS_COMPRESSION_EDIT Edit)
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
NtfsCloseCompressionEdit(PDEVICE_EXTENSION Vcb,
                         PNTFS_COMPRESSION_EDIT Edit)
{
    if (Edit->Holder != NULL)
        NtfsReleaseAttributeHolder(Vcb, Edit->FileRecord, Edit->Holder);
    if (Edit->Context != NULL)
        ReleaseAttributeContext(Edit->Context);
    if (Edit->FileRecord != NULL)
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, Edit->FileRecord);
}

/**
* @name NtfsMarkCompressed
*
* Sets or clears FILE_ATTRIBUTE_COMPRESSED wherever the file keeps its attributes.
*/
static
NTSTATUS
NtfsMarkCompressed(PDEVICE_EXTENSION Vcb,
                   PNTFS_FCB Fcb,
                   BOOLEAN Compressed)
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
            if (Compressed)
            {
                StdInfo->FileAttribute |= NTFS_FILE_TYPE_COMPRESSED;
                FileName->FileAttributes |= NTFS_FILE_TYPE_COMPRESSED;
                Fcb->Entry.FileAttributes |= NTFS_FILE_TYPE_COMPRESSED;
            }
            else
            {
                StdInfo->FileAttribute &= ~NTFS_FILE_TYPE_COMPRESSED;
                FileName->FileAttributes &= ~NTFS_FILE_TYPE_COMPRESSED;
                Fcb->Entry.FileAttributes &= ~NTFS_FILE_TYPE_COMPRESSED;
            }
            Status = NtfsUpdateDuplicatedInformation(Vcb, FileRecord, Fcb->MFTIndex, NTFS_FILENAME_UPDATE_ATTRS, FALSE);
        }
    }

    ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, FileRecord);

    return Status;
}

/**
* @name NtfsReadWholeStream
*
* Reads every byte of a stream, compressed or not, before its layout changes. The caller
* frees the copy with ExFreePoolWithTag(TAG_NTFS).
*/
static
NTSTATUS
NtfsReadWholeStream(PDEVICE_EXTENSION Vcb,
                    PNTFS_ATTR_CONTEXT Context,
                    PUCHAR *Data,
                    PULONG Size)
{
    ULONGLONG DataSize = AttributeDataLength(Context->pRecord);
    NTSTATUS Status;

    *Data = NULL;
    *Size = 0;

    if (DataSize == 0)
        return STATUS_SUCCESS;
    if (DataSize > MAXULONG / 2)
        return STATUS_INSUFFICIENT_RESOURCES;

    *Data = ExAllocatePoolWithTag(NonPagedPool, (ULONG)DataSize, TAG_NTFS);
    if (*Data == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    if (NtfsAttributeIsCompressed(Context))
        Status = NtfsReadCompressed(Vcb, Context, 0, *Data, (ULONG)DataSize);
    else
        Status = (ReadAttribute(Vcb, Context, 0, (PCHAR)*Data, (ULONG)DataSize) == DataSize) ?
                 STATUS_SUCCESS : STATUS_FILE_CORRUPT_ERROR;

    if (!NT_SUCCESS(Status))
    {
        ExFreePoolWithTag(*Data, TAG_NTFS);
        *Data = NULL;
        return Status;
    }

    *Size = (ULONG)DataSize;

    return STATUS_SUCCESS;
}

/**
* @name NtfsRelayoutStream
*
* Changes how a stream stores its data, compressed or not, keeping the data: gives back all
* its clusters, flips the flag and writes the data again under the new layout.
*/
static
NTSTATUS
NtfsRelayoutStream(PDEVICE_EXTENSION Vcb,
                   PNTFS_FCB Fcb,
                   BOOLEAN Compress)
{
    NTFS_COMPRESSION_EDIT Edit;
    PUCHAR Data = NULL;
    ULONG Size = 0, Written;
    LARGE_INTEGER Zero;
    NTSTATUS Status;

    Status = NtfsOpenCompressionEdit(Vcb, Fcb, &Edit);

    /* Units need runs, so a resident stream moves to clusters first */
    if (NT_SUCCESS(Status) && !Edit.Context->pRecord->IsNonResident)
    {
        LARGE_INTEGER Current;

        Current.QuadPart = Edit.Context->pRecord->Resident.ValueLength;
        Status = ConvertResidentAttributeToNonResident(Vcb, Edit.Context, Edit.Offset, Edit.Holder, &Current);
    }

    if (NT_SUCCESS(Status))
        Status = NtfsReadWholeStream(Vcb, Edit.Context, &Data, &Size);

    /* Empty the stream, change its layout, and put the data back */
    if (NT_SUCCESS(Status))
    {
        Zero.QuadPart = 0;
        Status = SetNonResidentAttributeDataLength(Vcb, Edit.Context, Edit.Offset, Edit.Holder, &Zero);
    }
    if (NT_SUCCESS(Status))
    {
        if (Compress)
            Status = NtfsSetAttributeUnits(Vcb, Edit.Context, Edit.Offset, Edit.Holder, NTFS_ATTR_COMPRESSED_LZNT1);
        else
            Status = NtfsClearAttributeUnits(Vcb, Edit.Context, Edit.Offset, Edit.Holder, NTFS_ATTR_COMPRESSED_LZNT1);
    }
    if (NT_SUCCESS(Status) && Size != 0)
    {
        LARGE_INTEGER NewSize;

        NewSize.QuadPart = Size;
        Status = SetNonResidentAttributeDataLength(Vcb, Edit.Context, Edit.Offset, Edit.Holder, &NewSize);
        if (NT_SUCCESS(Status))
        {
            if (Compress)
                Status = NtfsWriteCompressed(Vcb, Edit.Context, Edit.Offset, Edit.Holder, 0, Data, Size);
            else
                Status = WriteAttribute(Vcb, Edit.Context, 0, Data, Size, &Written, Edit.Holder);
        }
    }
    if (NT_SUCCESS(Status))
        Status = UpdateFileRecord(Vcb, Edit.Context->FileMFTIndex & NTFS_MFT_MASK, Edit.Holder);

    if (NT_SUCCESS(Status))
    {
        Fcb->RFCB.AllocationSize.QuadPart = Edit.Context->pRecord->NonResident.AllocatedSize;
        Fcb->RFCB.FileSize.QuadPart = Size;
        Fcb->RFCB.ValidDataLength.QuadPart = Size;
    }

    if (Data != NULL)
        ExFreePoolWithTag(Data, TAG_NTFS);
    NtfsCloseCompressionEdit(Vcb, &Edit);

    return Status;
}

/**
* @name NtfsGetCompression
* @implemented
*
* FSCTL_GET_COMPRESSION: COMPRESSION_FORMAT_LZNT1 for a compressed stream or directory,
* COMPRESSION_FORMAT_NONE otherwise.
*/
NTSTATUS
NtfsGetCompression(PDEVICE_EXTENSION Vcb,
                   PNTFS_FCB Fcb,
                   PUSHORT Format)
{
    NTFS_COMPRESSION_EDIT Edit;
    NTSTATUS Status;

    if (NtfsFCBIsDirectory(Fcb))
    {
        *Format = NtfsFCBIsCompressed(Fcb) ? COMPRESSION_FORMAT_LZNT1 : COMPRESSION_FORMAT_NONE;
        return STATUS_SUCCESS;
    }

    Status = NtfsOpenCompressionEdit(Vcb, Fcb, &Edit);
    if (NT_SUCCESS(Status))
        *Format = (Edit.Context->pRecord->Flags & NTFS_ATTR_COMPRESSION_MASK) ? COMPRESSION_FORMAT_LZNT1 : COMPRESSION_FORMAT_NONE;
    NtfsCloseCompressionEdit(Vcb, &Edit);

    return Status;
}

/**
* @name NtfsSetCompression
* @implemented
*
* FSCTL_SET_COMPRESSION (MS-FSA 2.1.5.10.30): COMPRESSION_FORMAT_DEFAULT or LZNT1 compresses
* the stream, COMPRESSION_FORMAT_NONE decompresses it, its data kept either way. On a
* directory it only marks what new files in it start with.
*/
NTSTATUS
NtfsSetCompression(PDEVICE_EXTENSION Vcb,
                   PNTFS_FCB Fcb,
                   USHORT Format)
{
    BOOLEAN Compress;
    USHORT Current;
    NTSTATUS Status;

    switch (Format)
    {
        case COMPRESSION_FORMAT_NONE:
            Compress = FALSE;
            break;

        case COMPRESSION_FORMAT_DEFAULT:
        case COMPRESSION_FORMAT_LZNT1:
            Compress = TRUE;
            break;

        default:
            return STATUS_INVALID_PARAMETER;
    }

    if (!NtfsFCBIsDirectory(Fcb))
    {
        Status = NtfsGetCompression(Vcb, Fcb, &Current);
        if (!NT_SUCCESS(Status))
            return Status;

        /* A sparse stream can't also be compressed here */
        if (Compress && (Fcb->Entry.FileAttributes & NTFS_FILE_TYPE_SPARSE))
            return STATUS_NOT_SUPPORTED;

        if ((Current != COMPRESSION_FORMAT_NONE) != Compress)
        {
            Status = NtfsRelayoutStream(Vcb, Fcb, Compress);
            if (!NT_SUCCESS(Status))
                return Status;
        }
    }

    return NtfsMarkCompressed(Vcb, Fcb, Compress);
}

/* EOF */
