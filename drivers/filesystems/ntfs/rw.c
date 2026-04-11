/*
 *  ReactOS kernel
 *  Copyright (C) 2002, 2014 ReactOS Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * COPYRIGHT:        See COPYING in the top level directory
 * PROJECT:          ReactOS kernel
 * FILE:             drivers/filesystem/ntfs/rw.c
 * PURPOSE:          NTFS filesystem driver
 * PROGRAMMERS:      Art Yerkes
 *                   Pierre Schweitzer (pierre@reactos.org)
 *                   Trevor Thompson
 */

/* INCLUDES *****************************************************************/

#include <ntddk.h>
#include "ntfs.h"

#define NDEBUG
#include <debug.h>

/* FUNCTIONS ****************************************************************/

/*
 * FUNCTION: Reads data from a file
 */
static
NTSTATUS
NtfsReadFile(PDEVICE_EXTENSION DeviceExt,
             PFILE_OBJECT FileObject,
             PUCHAR Buffer,
             ULONG Length,
             ULONG ReadOffset,
             ULONG IrpFlags,
             PULONG LengthRead)
{
    NTSTATUS Status = STATUS_SUCCESS;
    PNTFS_FCB Fcb;
    PFILE_RECORD_HEADER FileRecord;
    PNTFS_ATTR_CONTEXT DataContext;
    ULONG RealLength;
    ULONG RealReadOffset;
    ULONG RealLengthRead;
    ULONG ToRead;
    BOOLEAN AllocatedBuffer = FALSE;
    PCHAR ReadBuffer = (PCHAR)Buffer;
    ULONGLONG StreamSize;

    DPRINT("NtfsReadFile(%p, %p, %p, %lu, %lu, %lx, %p)\n", DeviceExt, FileObject, Buffer, Length, ReadOffset, IrpFlags, LengthRead);

    *LengthRead = 0;

    if (Length == 0)
    {
        DPRINT1("Null read!\n");
        return STATUS_SUCCESS;
    }

    Fcb = (PNTFS_FCB)FileObject->FsContext;

    // Volume and volume stream reads go directly to the storage device —
    // don't route through MFT attributes (MFTIndex 0 is $MFT, and the
    // volume stream represents the raw volume for the cache manager).
    if (Fcb->Flags & (FCB_IS_VOLUME | FCB_IS_VOLUME_STREAM))
    {
        NTSTATUS VStatus;
        VStatus = NtfsReadDisk(DeviceExt->StorageDevice,
                               ReadOffset,
                               Length,
                               DeviceExt->NtfsInfo.BytesPerSector,
                               Buffer,
                               FALSE);
        if (NT_SUCCESS(VStatus))
            *LengthRead = Length;
        return VStatus;
    }

    if (NtfsFCBIsCompressed(Fcb))
    {
        DPRINT1("Compressed file!\n");
        UNIMPLEMENTED;
        return STATUS_NOT_IMPLEMENTED;
    }

    if (NtfsFCBIsEncrypted(Fcb))
    {
        DPRINT1("Encrypted file!\n");
        UNIMPLEMENTED;
        return STATUS_NOT_IMPLEMENTED;
    }

    FileRecord = ExAllocateFromNPagedLookasideList(&DeviceExt->FileRecLookasideList);
    if (FileRecord == NULL)
    {
        DPRINT1("Not enough memory!\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Status = ReadFileRecord(DeviceExt, Fcb->MFTIndex, FileRecord);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Can't find record!\n");
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
        return Status;
    }


    Status = FindAttribute(DeviceExt, FileRecord, AttributeData, Fcb->Stream, wcslen(Fcb->Stream), &DataContext, NULL);
    if (!NT_SUCCESS(Status))
    {
        NTSTATUS BrowseStatus;
        FIND_ATTR_CONTXT Context;
        PNTFS_ATTR_RECORD Attribute;

        DPRINT1("No '%S' data stream associated with file!\n", Fcb->Stream);

        BrowseStatus = FindFirstAttribute(&Context, DeviceExt, FileRecord, FALSE, &Attribute);
        while (NT_SUCCESS(BrowseStatus))
        {
            if (Attribute->Type == AttributeData)
            {
                UNICODE_STRING Name;

                Name.Length = Attribute->NameLength * sizeof(WCHAR);
                Name.MaximumLength = Name.Length;
                Name.Buffer = (PWCHAR)((ULONG_PTR)Attribute + Attribute->NameOffset);
                DPRINT1("Data stream: '%wZ' available\n", &Name);
            }

            BrowseStatus = FindNextAttribute(&Context, &Attribute);
        }
        FindCloseAttribute(&Context);

        ReleaseAttributeContext(DataContext);
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
        return Status;
    }

    StreamSize = AttributeDataLength(DataContext->pRecord);
    if (ReadOffset >= StreamSize)
    {
        DPRINT1("Reading beyond stream end!\n");
        ReleaseAttributeContext(DataContext);
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
        return STATUS_END_OF_FILE;
    }

    ToRead = Length;
    if (ReadOffset + Length > StreamSize)
        ToRead = StreamSize - ReadOffset;

    RealReadOffset = ReadOffset;
    RealLength = ToRead;

    if ((ReadOffset % DeviceExt->NtfsInfo.BytesPerSector) != 0 || (ToRead % DeviceExt->NtfsInfo.BytesPerSector) != 0)
    {
        RealReadOffset = ROUND_DOWN(ReadOffset, DeviceExt->NtfsInfo.BytesPerSector);
        RealLength = ROUND_UP(ToRead, DeviceExt->NtfsInfo.BytesPerSector);
        /* do we need to extend RealLength by one sector? */
        if (RealLength + RealReadOffset < ReadOffset + Length)
        {
            if (RealReadOffset + RealLength + DeviceExt->NtfsInfo.BytesPerSector <= AttributeAllocatedLength(DataContext->pRecord))
                RealLength += DeviceExt->NtfsInfo.BytesPerSector;
        }


        ReadBuffer = ExAllocatePoolWithTag(NonPagedPool, RealLength, TAG_NTFS);
        if (ReadBuffer == NULL)
        {
            DPRINT1("Not enough memory!\n");
            ReleaseAttributeContext(DataContext);
            ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        AllocatedBuffer = TRUE;
    }

    DPRINT("NtfsReadFile: calling ReadAttribute offset=%lu len=%lu\n", RealReadOffset, RealLength);
    RealLengthRead = ReadAttribute(DeviceExt, DataContext, RealReadOffset, (PCHAR)ReadBuffer, RealLength);
    DPRINT("NtfsReadFile: ReadAttribute returned %lu\n", RealLengthRead);
    if (RealLengthRead == 0)
    {
        DPRINT1("Read failure!\n");
        ReleaseAttributeContext(DataContext);
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
        if (AllocatedBuffer)
        {
            ExFreePoolWithTag(ReadBuffer, TAG_NTFS);
        }
        return Status;
    }

    ReleaseAttributeContext(DataContext);
    ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);

    *LengthRead = ToRead;

    DPRINT("%lu got read\n", *LengthRead);

    if (AllocatedBuffer)
    {
        RtlCopyMemory(Buffer, ReadBuffer + (ReadOffset - RealReadOffset), ToRead);
    }

    if (ToRead != Length)
    {
        RtlZeroMemory(Buffer + ToRead, Length - ToRead);
    }

    if (AllocatedBuffer)
    {
        ExFreePoolWithTag(ReadBuffer, TAG_NTFS);
    }

    return STATUS_SUCCESS;
}


NTSTATUS
NtfsRead(PNTFS_IRP_CONTEXT IrpContext)
{
    PDEVICE_EXTENSION DeviceExt;
    PIO_STACK_LOCATION Stack;
    PFILE_OBJECT FileObject;
    PVOID Buffer;
    ULONG ReadLength;
    LARGE_INTEGER ReadOffset;
    ULONG ReturnedReadLength = 0;
    NTSTATUS Status = STATUS_SUCCESS;
    PIRP Irp;
    PDEVICE_OBJECT DeviceObject;
    PNTFS_FCB Fcb;
    PERESOURCE Resource;
    BOOLEAN PagingIo, NonCachedIo;

    DPRINT("NtfsRead(IrpContext %p)\n", IrpContext);

    DeviceObject = IrpContext->DeviceObject;
    Irp = IrpContext->Irp;
    Stack = IrpContext->Stack;
    FileObject = IrpContext->FileObject;

    DeviceExt = DeviceObject->DeviceExtension;
    ReadLength = Stack->Parameters.Read.Length;
    ReadOffset = Stack->Parameters.Read.ByteOffset;

    Fcb = (PNTFS_FCB)FileObject->FsContext;
    ASSERT(Fcb);

    PagingIo = BooleanFlagOn(Irp->Flags, IRP_PAGING_IO);
    NonCachedIo = BooleanFlagOn(Irp->Flags, IRP_NOCACHE) ||
                  BooleanFlagOn(FileObject->Flags, FILE_NO_INTERMEDIATE_BUFFERING);

    DPRINT("NtfsRead: FCB=%p Flags=0x%lx IrpFlags=0x%lx Offset=%I64d Length=%lu\n",
           Fcb, Fcb->Flags, Irp->Flags, ReadOffset.QuadPart, ReadLength);

    if (ReadLength == 0)
    {
        Irp->IoStatus.Information = 0;
        return STATUS_SUCCESS;
    }

    /* Honor byte-range locks taken by other file objects on this file.
     * Paging I/O and volume reads aren't subject to lock checks (the
     * kernel initiates paging itself, and the volume stream isn't a
     * lockable file). */
    if (!PagingIo &&
        !(Fcb->Flags & FCB_IS_VOLUME) &&
        Fcb->Identifier.Type == NTFS_TYPE_FCB &&
        !NtfsFCBIsDirectory(Fcb) &&
        !FsRtlCheckLockForReadAccess(&Fcb->FileLock, Irp))
    {
        Irp->IoStatus.Information = 0;
        return STATUS_FILE_LOCK_CONFLICT;
    }

    if (Fcb->Flags & FCB_IS_VOLUME)
    {
        Resource = &DeviceExt->DirResource;
    }
    else if (PagingIo)
    {
        Resource = &Fcb->PagingIoResource;
    }
    else
    {
        Resource = &Fcb->MainResource;
    }

    if (!ExAcquireResourceSharedLite(Resource,
                                     BooleanFlagOn(IrpContext->Flags, IRPCONTEXT_CANWAIT)))
    {
        return NtfsMarkIrpContextForQueue(IrpContext);
    }

    /*
     * Cached read path: use CcCopyRead for non-paging, cached reads on
     * regular files. The cache manager will issue IRP_PAGING_IO back to us
     * on cache misses, which falls through to the non-cached path below.
     */
    if (!PagingIo && !NonCachedIo && !(Fcb->Flags & FCB_IS_VOLUME))
    {
        /* Ensure cache is initialized on this FileObject */
        if (FileObject->PrivateCacheMap == NULL)
        {
            CcInitializeCacheMap(FileObject,
                                 (PCC_FILE_SIZES)(&Fcb->RFCB.AllocationSize),
                                 FALSE,
                                 &(NtfsGlobalData->CacheMgrCallbacks),
                                 Fcb);
        }

        /* Clamp read to file size */
        if (ReadOffset.QuadPart >= Fcb->RFCB.FileSize.QuadPart)
        {
            ExReleaseResourceLite(Resource);
            Irp->IoStatus.Information = 0;
            return STATUS_END_OF_FILE;
        }
        if (ReadOffset.QuadPart + ReadLength > Fcb->RFCB.FileSize.QuadPart)
        {
            ReadLength = (ULONG)(Fcb->RFCB.FileSize.QuadPart - ReadOffset.QuadPart);
        }

        Buffer = NtfsGetUserBuffer(Irp, FALSE);

        if (!CcCopyRead(FileObject,
                        &ReadOffset,
                        ReadLength,
                        BooleanFlagOn(IrpContext->Flags, IRPCONTEXT_CANWAIT),
                        Buffer,
                        &Irp->IoStatus))
        {
            ExReleaseResourceLite(Resource);
            return NtfsMarkIrpContextForQueue(IrpContext);
        }

        Status = Irp->IoStatus.Status;
        ReturnedReadLength = (ULONG)Irp->IoStatus.Information;

        if (NT_SUCCESS(Status) && (FileObject->Flags & FO_SYNCHRONOUS_IO))
        {
            FileObject->CurrentByteOffset.QuadPart =
                ReadOffset.QuadPart + ReturnedReadLength;
        }

        ExReleaseResourceLite(Resource);
        return Status;
    }

    /*
     * Non-cached / paging I/O path: read directly from disk via ReadAttribute.
     * This is called by the cache manager for cache misses, and for
     * non-cached / paging I/O requests.
     */
    Buffer = NtfsGetUserBuffer(Irp, PagingIo);

    Status = NtfsReadFile(DeviceExt,
                          FileObject,
                          Buffer,
                          ReadLength,
                          ReadOffset.u.LowPart,
                          Irp->Flags,
                          &ReturnedReadLength);
    if (NT_SUCCESS(Status))
    {
        if (FileObject->Flags & FO_SYNCHRONOUS_IO)
        {
            FileObject->CurrentByteOffset.QuadPart =
                ReadOffset.QuadPart + ReturnedReadLength;
        }

        Irp->IoStatus.Information = ReturnedReadLength;
    }
    else
    {
        Irp->IoStatus.Information = 0;
    }

    ExReleaseResourceLite(Resource);

    return Status;
}

/**
* @name NtfsWriteFile
* @implemented
*
* Writes a file to the disk. It presently borrows a lot of code from NtfsReadFile() and
* VFatWriteFileData(). It needs some more work before it will be complete; it won't handle
* page files, asnyc io, cached writes, etc.
*
* @param DeviceExt
* Points to the target disk's DEVICE_EXTENSION
*
* @param FileObject
* Pointer to a FILE_OBJECT describing the target file
*
* @param Buffer
* The data that's being written to the file
*
* @Param Length
* The size of the data buffer being written, in bytes
*
* @param WriteOffset
* Offset, in bytes, from the beginning of the file. Indicates where to start
* writing data.
*
* @param IrpFlags
* TODO: flags are presently ignored in code.
*
* @param CaseSensitive
* Boolean indicating if the function should operate in case-sensitive mode. This will be TRUE
* if an application opened the file with the FILE_FLAG_POSIX_SEMANTICS flag.
*
* @param LengthWritten
* Pointer to a ULONG. This ULONG will be set to the number of bytes successfully written.
*
* @return
* STATUS_SUCCESS if successful, STATUS_NOT_IMPLEMENTED if a required feature isn't implemented,
* STATUS_INSUFFICIENT_RESOURCES if an allocation failed, STATUS_ACCESS_DENIED if the write itself fails,
* STATUS_PARTIAL_COPY or STATUS_UNSUCCESSFUL if ReadFileRecord() fails, or
* STATUS_OBJECT_NAME_NOT_FOUND if the file's data stream could not be found.
*
* @remarks Called by NtfsWrite(). It may perform a read-modify-write operation if the requested write is
* not sector-aligned. LengthWritten only refers to how much of the requested data has been written;
* extra data that needs to be written to make the write sector-aligned will not affect it.
*
*/
NTSTATUS NtfsWriteFile(PDEVICE_EXTENSION DeviceExt,
                       PFILE_OBJECT FileObject,
                       const PUCHAR Buffer,
                       ULONG Length,
                       ULONG WriteOffset,
                       ULONG IrpFlags,
                       BOOLEAN CaseSensitive,
                       PULONG LengthWritten)
{
    NTSTATUS Status = STATUS_NOT_IMPLEMENTED;
    PNTFS_FCB Fcb;
    PFILE_RECORD_HEADER FileRecord;
    PNTFS_ATTR_CONTEXT DataContext;
    ULONG AttributeOffset;
    ULONGLONG StreamSize;

    DPRINT("NtfsWriteFile(%p, %p, %p, %lu, %lu, %x, %s, %p)\n",
           DeviceExt,
           FileObject,
           Buffer,
           Length,
           WriteOffset,
           IrpFlags,
           (CaseSensitive ? "TRUE" : "FALSE"),
           LengthWritten);

    *LengthWritten = 0;

    ASSERT(DeviceExt);

    if (Length == 0)
    {
        if (Buffer == NULL)
            return STATUS_SUCCESS;
        else
            return STATUS_INVALID_PARAMETER;
    }

    // get the File control block
    Fcb = (PNTFS_FCB)FileObject->FsContext;
    ASSERT(Fcb);

    DPRINT("Fcb->PathName: %wS\n", Fcb->PathName);
    DPRINT("Fcb->ObjectName: %wS\n", Fcb->ObjectName);

    // Volume writes go directly to the storage device — don't route through
    // MFT attributes. The volume FCB has MFTIndex 0 ($MFT), so WriteAttribute
    // would write INTO the MFT data, corrupting it.
    if (Fcb->Flags & FCB_IS_VOLUME)
    {
        return NtfsWriteDiskCached(DeviceExt,
                                    WriteOffset,
                                    Length,
                                    Buffer);
    }

    // we don't yet handle compression
    if (NtfsFCBIsCompressed(Fcb))
    {
        DPRINT("Compressed file!\n");
        UNIMPLEMENTED;
        return STATUS_NOT_IMPLEMENTED;
    }

    // allocate non-paged memory for the FILE_RECORD_HEADER
    FileRecord = ExAllocateFromNPagedLookasideList(&DeviceExt->FileRecLookasideList);
    if (FileRecord == NULL)
    {
        DPRINT1("Not enough memory! Can't write %wS!\n", Fcb->PathName);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    // read the FILE_RECORD_HEADER from the drive (or cache)
    DPRINT("Reading file record...\n");
    Status = ReadFileRecord(DeviceExt, Fcb->MFTIndex, FileRecord);
    if (!NT_SUCCESS(Status))
    {
        // We couldn't get the file's record. Free the memory and return the error
        DPRINT1("Can't find record for %wS!\n", Fcb->ObjectName);
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
        return Status;
    }

    DPRINT("Found record for %wS\n", Fcb->ObjectName);

    // Find the attribute with the data stream for our file
    DPRINT("Finding Data Attribute...\n");
    Status = FindAttribute(DeviceExt, FileRecord, AttributeData, Fcb->Stream, wcslen(Fcb->Stream), &DataContext,
                           &AttributeOffset);

    // Did we fail to find the attribute?
    if (!NT_SUCCESS(Status))
    {
        NTSTATUS BrowseStatus;
        FIND_ATTR_CONTXT Context;
        PNTFS_ATTR_RECORD Attribute;

        DPRINT1("No '%S' data stream associated with file!\n", Fcb->Stream);

        // Couldn't find the requested data stream; print a list of streams available
        BrowseStatus = FindFirstAttribute(&Context, DeviceExt, FileRecord, FALSE, &Attribute);
        while (NT_SUCCESS(BrowseStatus))
        {
            if (Attribute->Type == AttributeData)
            {
                UNICODE_STRING Name;

                Name.Length = Attribute->NameLength * sizeof(WCHAR);
                Name.MaximumLength = Name.Length;
                Name.Buffer = (PWCHAR)((ULONG_PTR)Attribute + Attribute->NameOffset);
                DPRINT1("Data stream: '%wZ' available\n", &Name);
            }

            BrowseStatus = FindNextAttribute(&Context, &Attribute);
        }
        FindCloseAttribute(&Context);

        ReleaseAttributeContext(DataContext);
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
        return Status;
    }

    // Get the size of the stream on disk
    StreamSize = AttributeDataLength(DataContext->pRecord);

    DPRINT("WriteOffset: %lu\tStreamSize: %I64u\n", WriteOffset, StreamSize);

    // Are we trying to write beyond the end of the stream?
    if (WriteOffset + Length > StreamSize)
    {
        if (IrpFlags & IRP_PAGING_IO)
        {
            // The cache manager may flush full pages that extend beyond
            // the logical file size. Clamp the write to the stream size
            // so we don't try to extend the file from paging I/O.
            DPRINT1("NtfsWriteFile PAGING: %wS off=%lu len=%lu stream=%I64u\n",
                    Fcb->ObjectName, WriteOffset, Length, StreamSize);
            if (WriteOffset >= StreamSize)
            {
                // Nothing to write — the entire range is past EOF.
                DPRINT1("NtfsWriteFile PAGING: SKIP (past EOF)\n");
                ReleaseAttributeContext(DataContext);
                ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
                *LengthWritten = 0;
                return STATUS_SUCCESS;
            }
            Length = (ULONG)(StreamSize - WriteOffset);
        }
        else if (!(Fcb->Flags & FCB_IS_VOLUME))
        {
            LARGE_INTEGER DataSize;
            ULONGLONG AllocationSize;
            PFILENAME_ATTRIBUTE fileNameAttribute;
            ULONGLONG ParentMFTId;
            UNICODE_STRING filename;

            DataSize.QuadPart = WriteOffset + Length;

            // set the attribute data length
            Status = SetAttributeDataLength(FileObject, Fcb, DataContext, AttributeOffset, FileRecord, &DataSize);
            if (!NT_SUCCESS(Status))
            {
                ReleaseAttributeContext(DataContext);
                ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
                *LengthWritten = 0;
                return Status;
            }

            AllocationSize = AttributeAllocatedLength(DataContext->pRecord);

            // now we need to update this file's size in every directory index entry that references it
            // TODO: put this code in its own function and adapt it to work with every filename / hardlink
            // stored in the file record.
            fileNameAttribute = GetBestFileNameFromRecord(Fcb->Vcb, FileRecord);
            ASSERT(fileNameAttribute);

            ParentMFTId = fileNameAttribute->DirectoryFileReferenceNumber & NTFS_MFT_MASK;

            filename.Buffer = fileNameAttribute->Name;
            filename.Length = fileNameAttribute->NameLength * sizeof(WCHAR);
            filename.MaximumLength = filename.Length;

            Status = UpdateFileNameRecord(Fcb->Vcb,
                                          ParentMFTId,
                                          &filename,
                                          FALSE,
                                          DataSize.QuadPart,
                                          AllocationSize,
                                          CaseSensitive);

        }
        else
        {
            ReleaseAttributeContext(DataContext);
            ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
            *LengthWritten = 0;
            return STATUS_ACCESS_DENIED;
        }
    }

    DPRINT("Length: %lu\tWriteOffset: %lu\tStreamSize: %I64u\n", Length, WriteOffset, StreamSize);

    // After extending, re-read the file record and re-find the attribute to ensure
    // we have the latest on-disk state (the resident->non-resident conversion may
    // have changed the attribute layout).
    if (WriteOffset + Length > StreamSize && DataContext->pRecord->IsNonResident)
    {
        ReleaseAttributeContext(DataContext);

        Status = ReadFileRecord(DeviceExt, Fcb->MFTIndex, FileRecord);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("Re-read of file record failed after extend! Status: 0x%lx\n", Status);
            ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
            *LengthWritten = 0;
            return Status;
        }

        Status = FindAttribute(DeviceExt, FileRecord, AttributeData, Fcb->Stream, wcslen(Fcb->Stream),
                               &DataContext, &AttributeOffset);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("Re-find of $DATA after extend failed! Status: 0x%lx\n", Status);
            ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
            *LengthWritten = 0;
            return Status;
        }
    }

    // Write the data to the attribute
    if (WriteOffset == 0 && Length < 8192)
    {
        DPRINT1("NtfsWriteFile: write %wS: len=%lu stream=%I64u nr=%d fb=0x%02x\n",
                Fcb->ObjectName, Length, StreamSize,
                DataContext->pRecord->IsNonResident,
                Buffer ? Buffer[0] : 0xFF);
    }
    Status = WriteAttribute(DeviceExt, DataContext, WriteOffset, Buffer, Length, LengthWritten, FileRecord);
    if (!NT_SUCCESS(Status) || *LengthWritten != Length)
    {
        DPRINT1("NtfsWriteFile: WriteAttribute FAILED for %wS: status=0x%lx written=%lu requested=%lu\n",
                Fcb->ObjectName, Status, *LengthWritten, Length);
    }

    // Did the write fail?
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Write failure!\n");
        ReleaseAttributeContext(DataContext);
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);

        return Status;
    }

    // This should never happen:
    if (*LengthWritten != Length)
    {
        DPRINT1("\a\tNTFS DRIVER ERROR: length written (%lu) differs from requested (%lu), but no error was indicated!\n",
            *LengthWritten, Length);
        Status = STATUS_UNEXPECTED_IO_ERROR;
    }

    ReleaseAttributeContext(DataContext);
    ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);

    return Status;
}

/**
* @name NtfsWrite
* @implemented
*
* Handles IRP_MJ_WRITE I/O Request Packets for NTFS. This code borrows a lot from
* VfatWrite, and needs a lot of cleaning up. It also needs a lot more of the code
* from VfatWrite integrated.
*
* @param IrpContext
* Points to an NTFS_IRP_CONTEXT which describes the write
*
* @return
* STATUS_SUCCESS if successful,
* STATUS_INSUFFICIENT_RESOURCES if an allocation failed,
* STATUS_INVALID_DEVICE_REQUEST if called on the main device object,
* STATUS_NOT_IMPLEMENTED or STATUS_ACCESS_DENIED if a required feature isn't implemented.
* STATUS_PARTIAL_COPY, STATUS_UNSUCCESSFUL, or STATUS_OBJECT_NAME_NOT_FOUND if NtfsWriteFile() fails.
*
* @remarks Called by NtfsDispatch() in response to an IRP_MJ_WRITE request. Page files are not implemented.
* Support for large files (>4gb) is not implemented. Cached writes, file locks, transactions, etc - not implemented.
*
*/
NTSTATUS
NtfsWrite(PNTFS_IRP_CONTEXT IrpContext)
{
    PNTFS_FCB Fcb;
    PERESOURCE Resource = NULL;
    LARGE_INTEGER ByteOffset;
    PUCHAR Buffer;
    NTSTATUS Status = STATUS_SUCCESS;
    ULONG Length = 0;
    ULONG ReturnedWriteLength = 0;
    PDEVICE_OBJECT DeviceObject = NULL;
    PDEVICE_EXTENSION DeviceExt = NULL;
    PFILE_OBJECT FileObject = NULL;
    PIRP Irp = NULL;
    ULONG BytesPerSector;
    BOOLEAN PagingIo, NonCachedIo;

    DPRINT("NtfsWrite(IrpContext %p)\n", IrpContext);
    ASSERT(IrpContext);

    Irp = IrpContext->Irp;

    if (IrpContext->DeviceObject == NtfsGlobalData->DeviceObject)
    {
        Irp->IoStatus.Information = 0;
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    Fcb = (PNTFS_FCB)IrpContext->FileObject->FsContext;
    ASSERT(Fcb);

    FileObject = IrpContext->FileObject;
    DeviceObject = IrpContext->DeviceObject;
    DeviceExt = DeviceObject->DeviceExtension;
    BytesPerSector = DeviceExt->StorageDevice->SectorSize;
    Length = IrpContext->Stack->Parameters.Write.Length;

    ByteOffset = IrpContext->Stack->Parameters.Write.ByteOffset;
    if (ByteOffset.u.LowPart == FILE_WRITE_TO_END_OF_FILE &&
        ByteOffset.u.HighPart == -1)
    {
        ByteOffset.QuadPart = Fcb->RFCB.FileSize.QuadPart;
    }

    PagingIo = BooleanFlagOn(Irp->Flags, IRP_PAGING_IO);
    NonCachedIo = BooleanFlagOn(Irp->Flags, IRP_NOCACHE) ||
                  BooleanFlagOn(FileObject->Flags, FILE_NO_INTERMEDIATE_BUFFERING) ||
                  BooleanFlagOn(Fcb->Flags, FCB_IS_VOLUME);

    if (Length > 0 && Length <= 4096 && ByteOffset.QuadPart == 0 && !PagingIo)
    {
        DPRINT1("NtfsWrite ENTRY: %wS len=%lu nc=%d paging=%d fsize=%I64d\n",
                Fcb->ObjectName, Length, NonCachedIo, PagingIo, Fcb->RFCB.FileSize.QuadPart);
    }

    if (ByteOffset.u.HighPart && !(Fcb->Flags & FCB_IS_VOLUME))
    {
        DPRINT1("FIXME: Writing to large files is not yet supported at this time.\n");
        return STATUS_INVALID_PARAMETER;
    }

    if (PagingIo || NonCachedIo)
    {
        if (ByteOffset.u.LowPart % BytesPerSector != 0 || Length % BytesPerSector != 0)
        {
            DPRINT1("Non-cached writes and non-buffered writes must be sector aligned!\n");
            return STATUS_INVALID_PARAMETER;
        }
    }

    if (Length == 0)
    {
        IrpContext->Irp->IoStatus.Information = 0;

        if (Irp->UserBuffer == NULL && Irp->MdlAddress == NULL)
        {
            return STATUS_SUCCESS;
        }

        return STATUS_INVALID_PARAMETER;
    }

    /* Honor byte-range locks taken by other file objects on this file.
     * Skip the check for paging I/O, volume writes, and the volume
     * stream itself for the same reasons as in NtfsRead. */
    if (!PagingIo &&
        !(Fcb->Flags & FCB_IS_VOLUME) &&
        Fcb->Identifier.Type == NTFS_TYPE_FCB &&
        !NtfsFCBIsDirectory(Fcb) &&
        !FsRtlCheckLockForWriteAccess(&Fcb->FileLock, Irp))
    {
        Irp->IoStatus.Information = 0;
        return STATUS_FILE_LOCK_CONFLICT;
    }

    if (Fcb->Flags & FCB_IS_VOLUME)
    {
        Resource = &DeviceExt->DirResource;
    }
    else if (PagingIo)
    {
        Resource = &Fcb->PagingIoResource;
    }
    else
    {
        Resource = &Fcb->MainResource;
    }

    if (!ExAcquireResourceExclusiveLite(Resource, BooleanFlagOn(IrpContext->Flags, IRPCONTEXT_CANWAIT)))
    {
        return NtfsMarkIrpContextForQueue(IrpContext);
    }

    /*
     * Cached write path: use CcCopyWrite for non-paging, cached writes on
     * regular files. For extending writes, grow the file first via
     * NtfsWriteFile (which handles SetAttributeDataLength + CcSetFileSizes),
     * then write the data through the cache.
     */
    if (!PagingIo && !NonCachedIo && !(Fcb->Flags & FCB_IS_VOLUME))
    {
        /* Extend the file if needed — allocate clusters and update sizes */
        if (ByteOffset.QuadPart + Length > Fcb->RFCB.FileSize.QuadPart)
        {
            LARGE_INTEGER NewSize;
            NewSize.QuadPart = ByteOffset.QuadPart + Length;

            Status = NtfsSetEndOfFile(Fcb,
                                      FileObject,
                                      DeviceExt,
                                      Irp->Flags,
                                      BooleanFlagOn(IrpContext->Stack->Flags, SL_CASE_SENSITIVE),
                                      &NewSize);
            if (!NT_SUCCESS(Status))
            {
                ExReleaseResourceLite(Resource);
                Irp->IoStatus.Information = 0;
                return Status;
            }
        }

        if (FileObject->PrivateCacheMap == NULL)
        {
            CcInitializeCacheMap(FileObject,
                                 (PCC_FILE_SIZES)(&Fcb->RFCB.AllocationSize),
                                 FALSE,
                                 &(NtfsGlobalData->CacheMgrCallbacks),
                                 Fcb);
        }

        Buffer = NtfsGetUserBuffer(Irp, FALSE);

        if (!CcCopyWrite(FileObject,
                         &ByteOffset,
                         Length,
                         BooleanFlagOn(IrpContext->Flags, IRPCONTEXT_CANWAIT),
                         Buffer))
        {
            ExReleaseResourceLite(Resource);
            return NtfsMarkIrpContextForQueue(IrpContext);
        }

        /* Flush the written data to disk immediately.
         * The NTFS paging write path may reject cache manager writeback
         * in edge cases, so ensure data reaches disk now. */
        {
            IO_STATUS_BLOCK FlushIoStatus;
            CcFlushCache(FileObject->SectionObjectPointer,
                         &ByteOffset,
                         Length,
                         &FlushIoStatus);
        }

        Irp->IoStatus.Status = STATUS_SUCCESS;
        Irp->IoStatus.Information = Length;

        if (FileObject->Flags & FO_SYNCHRONOUS_IO)
        {
            FileObject->CurrentByteOffset.QuadPart = ByteOffset.QuadPart + Length;
        }

        IrpContext->PriorityBoost = IO_DISK_INCREMENT;
        ExReleaseResourceLite(Resource);

        return STATUS_SUCCESS;
    }

    /*
     * Non-cached / paging I/O path: write directly to disk via WriteAttribute.
     * This is called by the cache manager for dirty page writeback, and for
     * non-cached / paging I/O requests.
     */
    Buffer = NtfsGetUserBuffer(Irp, PagingIo);
    ASSERT(Buffer);

    Status = NtfsLockUserBuffer(Irp, Length, IoReadAccess);
    if (!NT_SUCCESS(Status))
    {
        ExReleaseResourceLite(Resource);
        return Status;
    }

    Status = NtfsWriteFile(DeviceExt,
                           FileObject,
                           Buffer,
                           Length,
                           ByteOffset.LowPart,
                           Irp->Flags,
                           BooleanFlagOn(IrpContext->Stack->Flags, SL_CASE_SENSITIVE),
                           &ReturnedWriteLength);

    IrpContext->Irp->IoStatus.Status = Status;

    if (NT_SUCCESS(Status))
    {
        if (FileObject->Flags & FO_SYNCHRONOUS_IO)
        {
            FileObject->CurrentByteOffset.QuadPart = ByteOffset.QuadPart + ReturnedWriteLength;
        }

        IrpContext->PriorityBoost = IO_DISK_INCREMENT;
    }

    Irp->IoStatus.Information = ReturnedWriteLength;

    ExReleaseResourceLite(Resource);

    return Status;
}

/* EOF */
