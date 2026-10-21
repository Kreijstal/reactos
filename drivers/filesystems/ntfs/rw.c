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
#include "lfswrite.h"

#define NDEBUG
#include <debug.h>

/* FUNCTIONS ****************************************************************/

/**
* @name NtfsReadWriteVolume
* @implemented
*
* Serves a read or write made through a handle on the volume itself.
*
* @param DeviceExt
* Points to the target disk's DEVICE_EXTENSION
*
* @param MajorFunction
* IRP_MJ_READ or IRP_MJ_WRITE
*
* @param Buffer
* System-space buffer to transfer to or from
*
* @param Length
* How much to transfer, in bytes
*
* @param Offset
* Byte offset from the start of the volume
*
* @param Irp
* The request being served, so that its buffer can be used directly
*
* @param LengthTransferred
* Receives how much was transferred
*
* @return
* STATUS_SUCCESS on success, otherwise the status of the transfer.
*
* @remarks A volume handle is raw access to the partition: it has no file
* record and no attributes. Running it through the attribute code would
* resolve the volume FCB's MFT index of zero and redirect the transfer into
* $MFT's own $DATA attribute.
*
*/
static
NTSTATUS
NtfsReadWriteVolume(PDEVICE_EXTENSION DeviceExt,
                    UCHAR MajorFunction,
                    PUCHAR Buffer,
                    ULONG Length,
                    LONGLONG Offset,
                    PIRP Irp,
                    PULONG LengthTransferred)
{
    NTFS_IO_RUN_LIST RunList;
    NTSTATUS Status;

    DPRINT("NtfsReadWriteVolume(%p, %u, %p, %lu, %I64u, %p, %p)\n",
           DeviceExt, MajorFunction, Buffer, Length, Offset, Irp, LengthTransferred);

    *LengthTransferred = 0;

    if (Length == 0)
        return STATUS_SUCCESS;

    /* Write-ahead logging: the records of the changes a page of the volume
     * stream holds reach the disk before it does. */
    if (MajorFunction == IRP_MJ_WRITE && (Irp->Flags & IRP_PAGING_IO) &&
        !(DeviceExt->Flags & VCB_VOLUME_DISMOUNTED))
    {
        Status = NtfsLfsForceRange(DeviceExt->LfsWriter, Offset, Length);
        if (!NT_SUCCESS(Status))
            return Status;
    }

    /* A raw handle must observe dirty metadata too. Paging I/O on the
     * internal volume stream is the cache's recursion terminator. */
    if (!(Irp->Flags & IRP_PAGING_IO) &&
        !(DeviceExt->Flags & VCB_VOLUME_DISMOUNTED))
    {
        if (MajorFunction == IRP_MJ_READ)
            Status = NtfsReadDiskCached(DeviceExt, Offset, Length, Buffer);
        else
        {
            Status = NtfsWriteDiskCached(DeviceExt, Offset, Length, Buffer);
            if (NT_SUCCESS(Status))
                Status = NtfsFlushVolume(DeviceExt);
        }
        if (NT_SUCCESS(Status))
            *LengthTransferred = Length;
        return Status;
    }

    NtfsInitIoRunList(&RunList);

    Status = NtfsAddIoRun(&RunList, Offset, Length);
    if (NT_SUCCESS(Status))
    {
        Status = NtfsPerformIrpIoRuns(DeviceExt->StorageDevice,
                                      MajorFunction,
                                      DeviceExt->NtfsInfo.BytesPerSector,
                                      Irp,
                                      Buffer,
                                      &RunList,
                                      FALSE,
                                      LengthTransferred);
    }

    NtfsFreeIoRunList(&RunList);

    return Status;
}

/*
 * FUNCTION: Reads data from a file
 */
static
NTSTATUS
NtfsReadFile(PDEVICE_EXTENSION DeviceExt,
             PFILE_OBJECT FileObject,
             PUCHAR Buffer,
             ULONG Length,
             ULONGLONG ReadOffset,
             ULONG IrpFlags,
             PIRP Irp,
             PULONG LengthRead)
{
    NTSTATUS Status = STATUS_SUCCESS;
    PNTFS_FCB Fcb;
    PFILE_RECORD_HEADER FileRecord;
    PNTFS_ATTR_CONTEXT DataContext = NULL;
    ULONG RealLength;
    ULONGLONG RealReadOffset;
    ULONG RealLengthRead;
    ULONG ToRead;
    BOOLEAN AllocatedBuffer = FALSE;
    PCHAR ReadBuffer = (PCHAR)Buffer;
    ULONGLONG StreamSize;

    DPRINT("NtfsReadFile(%p, %p, %p, %lu, %I64u, %lx, %p)\n", DeviceExt, FileObject, Buffer, Length, ReadOffset, IrpFlags, LengthRead);

    *LengthRead = 0;

    if (Length == 0)
    {
        DPRINT1("Null read!\n");
        return STATUS_SUCCESS;
    }

    Fcb = (PNTFS_FCB)FileObject->FsContext;

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

        DPRINT1("No '%S' data stream associated with file '%wS' (MFT record %I64u, record flags 0x%x)!\n",
                Fcb->Stream, Fcb->ObjectName, Fcb->MFTIndex, FileRecord->Flags);

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

        /* FindAttribute() failed, so DataContext was never set; there is
         * nothing to release here. */
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
        return Status;
    }

    DataContext->DirectIo = BooleanFlagOn(Fcb->Flags, FCB_IS_PAGING_FILE);

    StreamSize = AttributeDataLength(DataContext->pRecord);
    if (ReadOffset >= StreamSize)
    {
        DPRINT("Reading beyond stream end!\n");
        ReleaseAttributeContext(DataContext);
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
        return STATUS_END_OF_FILE;
    }

    ToRead = Length;
    if (ReadOffset + Length > StreamSize)
        ToRead = (ULONG)(StreamSize - ReadOffset);

    /* A compressed stream is read unit by unit */
    if (NtfsAttributeIsCompressed(DataContext))
    {
        Status = NtfsReadCompressed(DeviceExt, DataContext, ReadOffset, Buffer, ToRead);
        ReleaseAttributeContext(DataContext);
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
        if (!NT_SUCCESS(Status))
            return Status;

        if (ToRead != Length)
            RtlZeroMemory(Buffer + ToRead, Length - ToRead);
        *LengthRead = ToRead;
        return STATUS_SUCCESS;
    }

    RealReadOffset = ReadOffset;
    RealLength = ToRead;

    if ((ReadOffset % DeviceExt->NtfsInfo.BytesPerSector) != 0 || (ToRead % DeviceExt->NtfsInfo.BytesPerSector) != 0)
    {
        RealReadOffset = ROUND_DOWN(ReadOffset, DeviceExt->NtfsInfo.BytesPerSector);
        /* The aligned window must cover all of [ReadOffset, ReadOffset + ToRead),
         * including the sectors the unaligned start pulls in */
        RealLength = (ULONG)(ROUND_UP(ReadOffset + ToRead, DeviceExt->NtfsInfo.BytesPerSector) - RealReadOffset);

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

    DPRINT("Effective read: %lu at %I64u for stream '%S'\n", RealLength, RealReadOffset, Fcb->Stream);
    /* Only hand the IRP down when reading straight into its buffer: a
     * forwarded transfer lands at the start of the IRP's MDL, so a bounced
     * read would end up in the wrong place */
    RealLengthRead = ReadAttributeToIrp(DeviceExt,
                                        DataContext,
                                        RealReadOffset,
                                        (PCHAR)ReadBuffer,
                                        RealLength,
                                        AllocatedBuffer ? NULL : Irp);
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
        ASSERT(RealLengthRead >= (ReadOffset - RealReadOffset) + ToRead);
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


static
BOOLEAN
NtfsUseDataCache(PNTFS_FCB Fcb, PFILE_OBJECT FileObject, PIRP Irp)
{
    /* Separate path FCBs of hard links do not own a shared stream cache.
     * Their I/O stays coherent through the common volume cache instead,
     * including after an unlink leaves one name but several live path FCBs. */
    ASSERT(Fcb->FileLock != NULL);
    return !(Irp->Flags & (IRP_PAGING_IO | IRP_NOCACHE)) &&
           !(FileObject->Flags & FO_NO_INTERMEDIATE_BUFFERING) &&
           !NtfsFCBIsCompressed(Fcb) && !NtfsFCBIsEncrypted(Fcb) &&
           !(Fcb->Entry.FileAttributes & NTFS_FILE_TYPE_SPARSE) &&
           Fcb->LinkCount == 1 && !Fcb->FileLock->MultipleFcbs;
}

static
NTSTATUS
NtfsCachedReadFile(PFILE_OBJECT FileObject,
                   LARGE_INTEGER Offset,
                   PUCHAR Buffer,
                   ULONG Length,
                   PULONG Read)
{
    PNTFS_FCB Fcb = FileObject->FsContext;
    IO_STATUS_BLOCK IoStatus;
    NTSTATUS Status = STATUS_SUCCESS;

    *Read = 0;
    if (Length == 0)
        return STATUS_SUCCESS;
    if (Offset.QuadPart >= Fcb->RFCB.FileSize.QuadPart)
        return STATUS_END_OF_FILE;
    Length = (ULONG)min((LONGLONG)Length, Fcb->RFCB.FileSize.QuadPart - Offset.QuadPart);

    _SEH2_TRY
    {
        ASSERT(FileObject->PrivateCacheMap != NULL);
        if (!CcCopyRead(FileObject, &Offset, Length, TRUE, Buffer, &IoStatus))
            Status = STATUS_CANT_WAIT;
        else
        {
            Status = IoStatus.Status;
            *Read = (ULONG)IoStatus.Information;
        }
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = _SEH2_GetExceptionCode();
    }
    _SEH2_END;
    return Status;
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

    DPRINT("NtfsRead(IrpContext %p)\n", IrpContext);

    DeviceObject = IrpContext->DeviceObject;
    Irp = IrpContext->Irp;
    Stack = IrpContext->Stack;
    FileObject = IrpContext->FileObject;

    DeviceExt = DeviceObject->DeviceExtension;
    ReadLength = Stack->Parameters.Read.Length;
    ReadOffset = Stack->Parameters.Read.ByteOffset;
    Buffer = NtfsGetUserBuffer(Irp, BooleanFlagOn(Irp->Flags, IRP_PAGING_IO));

    /* Once dismounted the metadata can't be trusted; only raw volume access
     * is still served */
    if ((DeviceExt->Flags & VCB_VOLUME_DISMOUNTED) &&
        !(((PNTFS_FCB)FileObject->FsContext)->Flags & FCB_IS_VOLUME))
    {
        Irp->IoStatus.Information = 0;
        return STATUS_VOLUME_DISMOUNTED;
    }

    /* A handle on the volume itself is raw access to the partition. */
    if (((PNTFS_FCB)FileObject->FsContext)->Flags & (FCB_IS_VOLUME | FCB_IS_VOLUME_STREAM))
    {
        Status = NtfsReadWriteVolume(DeviceExt,
                                     IRP_MJ_READ,
                                     Buffer,
                                     ReadLength,
                                     ReadOffset.QuadPart,
                                     Irp,
                                     &ReturnedReadLength);
    }
    else if (NtfsFCBIsDirectory((PNTFS_FCB)FileObject->FsContext))
    {
        /* A directory has no data stream to read */
        Irp->IoStatus.Information = 0;
        return STATUS_INVALID_DEVICE_REQUEST;
    }
    else
    {
        PNTFS_FCB Fcb = FileObject->FsContext;
        PERESOURCE Resource = (Irp->Flags & IRP_PAGING_IO) ?
                              &Fcb->PagingIoResource : &Fcb->MainResource;
        IO_STATUS_BLOCK IoStatus;

        if (!NtfsCheckFileLock(Fcb, Irp, &ReadOffset, ReadLength, FALSE))
        {
            Irp->IoStatus.Information = 0;
            return STATUS_FILE_LOCK_CONFLICT;
        }

        /* Serialize private-map creation before taking main shared. Different
         * reads on the same file object may arrive concurrently. */
        if (NtfsUseDataCache(Fcb, FileObject, Irp) &&
            Fcb->DataCacheGeneration == Fcb->CacheGeneration &&
            FileObject->PrivateCacheMap == NULL)
        {
            ExAcquireResourceExclusiveLite(&Fcb->MainResource, TRUE);
            _SEH2_TRY
            {
                if (FileObject->PrivateCacheMap == NULL)
                {
                    CcInitializeCacheMap(FileObject, (PCC_FILE_SIZES)&Fcb->RFCB.AllocationSize,
                                         FALSE, &NtfsGlobalData->CacheMgrCallbacks, Fcb);
                }
            }
            _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
            {
                Status = _SEH2_GetExceptionCode();
            }
            _SEH2_END;
            ExReleaseResourceLite(&Fcb->MainResource);
            if (!NT_SUCCESS(Status))
            {
                Irp->IoStatus.Information = 0;
                return Status;
            }
        }
        ExAcquireResourceSharedLite(Resource, TRUE);
        if (!(Irp->Flags & IRP_PAGING_IO) &&
            !(NtfsUseDataCache(Fcb, FileObject, Irp) &&
              Fcb->DataCacheGeneration == Fcb->CacheGeneration))
        {
            CcFlushCache(FileObject->SectionObjectPointer, NULL, 0, &IoStatus);
            Status = IoStatus.Status;
        }
        if (NT_SUCCESS(Status))
        {
            if (!(Irp->Flags & IRP_PAGING_IO))
                ExAcquireResourceSharedLite(&Fcb->PagingIoResource, TRUE);
            if (NtfsUseDataCache(Fcb, FileObject, Irp) &&
                Fcb->DataCacheGeneration == Fcb->CacheGeneration)
            {
                Status = NtfsCachedReadFile(FileObject, ReadOffset, Buffer, ReadLength,
                                            &ReturnedReadLength);
            }
            else
            {
                Status = NtfsReadFile(DeviceExt, FileObject, Buffer, ReadLength,
                                      ReadOffset.QuadPart, Irp->Flags, Irp, &ReturnedReadLength);
            }
            if (!(Irp->Flags & IRP_PAGING_IO))
                ExReleaseResourceLite(&Fcb->PagingIoResource);
        }
        ExReleaseResourceLite(Resource);
    }
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
                       ULONGLONG WriteOffset,
                       ULONG IrpFlags,
                       BOOLEAN CaseSensitive,
                       PIRP Irp,
                       PULONG LengthWritten)
{
    NTSTATUS Status = STATUS_NOT_IMPLEMENTED;
    PNTFS_FCB Fcb;
    PFILE_RECORD_HEADER FileRecord;
    PNTFS_ATTR_CONTEXT DataContext = NULL;
    PFILE_RECORD_HEADER Holder;
    ULONG AttributeOffset;
    ULONGLONG StreamSize;
    ULONG RequestedLength = 0;

    DPRINT("NtfsWriteFile(%p, %p, %p, %lu, %I64u, %x, %s, %p)\n",
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

        DPRINT1("No '%S' data stream associated with file '%wS' (MFT record %I64u, record flags 0x%x)!\n",
                Fcb->Stream, Fcb->ObjectName, Fcb->MFTIndex, FileRecord->Flags);

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

        /* FindAttribute() failed, so DataContext was never set; there is
         * nothing to release here. */
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
        return Status;
    }

    /* Through an $ATTRIBUTE_LIST the stream can live in another record than the base */
    Status = NtfsReadAttributeHolder(DeviceExt, DataContext, FileRecord, &Holder);
    if (!NT_SUCCESS(Status))
    {
        ReleaseAttributeContext(DataContext);
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
        return Status;
    }

    DataContext->DirectIo = BooleanFlagOn(Fcb->Flags, FCB_IS_PAGING_FILE);

    // Get the size of the stream on disk
    StreamSize = AttributeDataLength(DataContext->pRecord);

    DPRINT("WriteOffset: %I64u\tStreamSize: %I64u\n", WriteOffset, StreamSize);

    // Are we trying to write beyond the end of the stream?
    if (WriteOffset + Length > StreamSize)
    {
        // is increasing the stream size allowed?
        if (!(Fcb->Flags & FCB_IS_VOLUME) &&
            !(IrpFlags & IRP_PAGING_IO))
        {
            LARGE_INTEGER DataSize;

            DataSize.QuadPart = WriteOffset + Length;

            /* Reserve room past the new end, so the appends that follow fit
             * without touching the bitmap or the run list. Cleanup gives back
             * what's left over. */
            if (DataContext->pRecord->IsNonResident &&
                !NtfsAttributeHasUnits(DataContext) &&
                (ULONGLONG)DataSize.QuadPart > DataContext->pRecord->NonResident.AllocatedSize)
            {
                ULONGLONG Ahead;

                Ahead = min(max(DataSize.QuadPart / 4, NTFS_GROW_AHEAD_MIN), NTFS_GROW_AHEAD_MAX);
                Status = NtfsExtendAllocation(DeviceExt,
                                              DataContext,
                                              AttributeOffset,
                                              Holder,
                                              ROUND_UP(DataSize.QuadPart + Ahead, DeviceExt->NtfsInfo.BytesPerCluster));
                if (!NT_SUCCESS(Status))
                {
                    /* Not fatal; SetAttributeDataLength() still allocates what's needed */
                    DPRINT("Couldn't reserve ahead of %wS (Status %lx)\n", Fcb->ObjectName, Status);
                }
            }

            // set the attribute data length
            Status = SetAttributeDataLength(FileObject, Fcb, DataContext, AttributeOffset, Holder, &DataSize);

            /* What lies between the old end and the write reads as zeros */
            if (NT_SUCCESS(Status) && WriteOffset > StreamSize)
                Status = NtfsZeroStreamRange(DeviceExt, DataContext, AttributeOffset, Holder, StreamSize, WriteOffset);

            if (!NT_SUCCESS(Status))
            {
                ReleaseAttributeContext(DataContext);
                NtfsReleaseAttributeHolder(DeviceExt, FileRecord, Holder);
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
                *LengthWritten = 0;
                return Status;
            }

            // The file grew, so every copy of its size has to follow.
            // TODO: adapt this to every filename / hardlink in the record.
            Status = NtfsUpdateDuplicatedInformation(Fcb->Vcb,
                                                     FileRecord,
                                                     Fcb->MFTIndex,
                                                     NTFS_FILENAME_UPDATE_SIZES,
                                                     CaseSensitive);
        }
        else if (IrpFlags & IRP_PAGING_IO)
        {
            /* Mm flushes whole pages, so a file's last page routinely reaches
             * past its end. A paging write must not change the file size, so
             * clamp instead of failing - refusing it would lose the valid
             * bytes before the end too. */
            if (WriteOffset >= StreamSize)
            {
                DPRINT("Paging write entirely past the end of the stream; nothing to do\n");

                ReleaseAttributeContext(DataContext);
                NtfsReleaseAttributeHolder(DeviceExt, FileRecord, Holder);
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
                *LengthWritten = Length;
                return STATUS_SUCCESS;
            }

            DPRINT("Clamping paging write of %lu bytes at %I64u to stream size %I64u\n",
                   Length, WriteOffset, StreamSize);

            RequestedLength = Length;
            Length = (ULONG)(StreamSize - WriteOffset);
        }
        else
        {
            /* The volume stream is the size of the volume and cannot grow */
            ReleaseAttributeContext(DataContext);
            NtfsReleaseAttributeHolder(DeviceExt, FileRecord, Holder);
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
            *LengthWritten = 0;
            return STATUS_ACCESS_DENIED;
        }
    }

    DPRINT("Length: %lu\tWriteOffset: %I64u\tStreamSize: %I64u\n", Length, WriteOffset, StreamSize);

    // Write the data to the attribute
    /* Buffer is the system mapping of this IRP's MDL, so hand the IRP down too
     * and let the run layer borrow it instead of locking these pages twice,
     * which the paging path forbids */
    if (NtfsAttributeIsCompressed(DataContext))
    {
        /* A compressed stream is written unit by unit */
        Status = NtfsWriteCompressed(DeviceExt, DataContext, AttributeOffset, Holder, WriteOffset, Buffer, Length);
        if (NT_SUCCESS(Status))
            *LengthWritten = Length;
    }
    else
    {
        Status = WriteAttributeFromIrp(DeviceExt, DataContext, WriteOffset, Buffer, Length, LengthWritten, Holder, Irp);
    }

    // Did the write fail?
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Write failure!\n");
        ReleaseAttributeContext(DataContext);
        NtfsReleaseAttributeHolder(DeviceExt, FileRecord, Holder);
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

    if (NT_SUCCESS(Status) && !(IrpFlags & IRP_PAGING_IO) &&
        WriteOffset + Length > (ULONGLONG)Fcb->RFCB.ValidDataLength.QuadPart)
    {
        Fcb->RFCB.ValidDataLength.QuadPart = WriteOffset + Length;
        CcSetFileSizes(FileObject, (PCC_FILE_SIZES)&Fcb->RFCB.AllocationSize);
    }

    // A clamped paging write stored all the stream had room for, so as far as
    // Mm is concerned the whole page is done
    if (RequestedLength != 0 && NT_SUCCESS(Status))
        *LengthWritten = RequestedLength;

    ReleaseAttributeContext(DataContext);
    NtfsReleaseAttributeHolder(DeviceExt, FileRecord, Holder);
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);

    return Status;
}

/* After the last other hard link is unlinked, sizes and clean private pages
 * may describe an older version written through that other path. Refresh both
 * before returning this FCB to the regular file cache path. */
static
NTSTATUS
NtfsRefreshDataCache(PDEVICE_EXTENSION Vcb, PFILE_OBJECT FileObject)
{
    PNTFS_FCB Fcb = FileObject->FsContext;
    PFILE_RECORD_HEADER Record;
    PNTFS_ATTR_CONTEXT Context = NULL;
    NTSTATUS Status;
    ULONG Generation = Fcb->CacheGeneration;

    Status = NtfsFlushFileCache(FileObject, TRUE);
    if (!NT_SUCCESS(Status))
        return Status;
    Record = ExAllocateFromNPagedLookasideList(&Vcb->FileRecLookasideList);
    if (Record == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;
    Status = ReadFileRecord(Vcb, Fcb->MFTIndex, Record);
    if (NT_SUCCESS(Status))
        Status = FindAttribute(Vcb, Record, AttributeData, Fcb->Stream, wcslen(Fcb->Stream), &Context, NULL);
    if (NT_SUCCESS(Status))
    {
        Fcb->RFCB.FileSize.QuadPart = AttributeDataLength(Context->pRecord);
        Fcb->RFCB.AllocationSize.QuadPart = Context->pRecord->IsNonResident ?
                                          Context->pRecord->NonResident.AllocatedSize :
                                          Context->pRecord->Resident.ValueLength;
        Fcb->RFCB.ValidDataLength = Fcb->RFCB.FileSize;
        CcSetFileSizes(FileObject, (PCC_FILE_SIZES)&Fcb->RFCB.AllocationSize);
        Fcb->DataCacheGeneration = Generation;
        ReleaseAttributeContext(Context);
    }
    ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, Record);
    return Status;
}

/* Preserve our cached-write path. Flush each copy before reporting completion, as on the source branch; the
 * volume cache still coalesces the resulting data and metadata writes. */
static
NTSTATUS
NtfsCachedWriteFile(PDEVICE_EXTENSION Vcb,
                    PFILE_OBJECT FileObject,
                    PUCHAR Buffer,
                    LARGE_INTEGER Offset,
                    ULONG Length,
                    BOOLEAN CaseSensitive,
                    PULONG Written)
{
    PNTFS_FCB Fcb = FileObject->FsContext;
    LARGE_INTEGER End;
    IO_STATUS_BLOCK IoStatus;
    NTSTATUS Status = STATUS_SUCCESS;

    *Written = 0;
    if (Fcb->DataCacheGeneration != Fcb->CacheGeneration)
    {
        Status = NtfsRefreshDataCache(Vcb, FileObject);
        if (!NT_SUCCESS(Status))
            return Status;
    }
    End.QuadPart = Offset.QuadPart + Length;
    if (End.QuadPart > Fcb->RFCB.FileSize.QuadPart)
    {
        Status = NtfsSetEndOfFile(Fcb, FileObject, Vcb, 0, CaseSensitive, &End);
        if (!NT_SUCCESS(Status))
            return Status;
    }

    _SEH2_TRY
    {
        if (FileObject->PrivateCacheMap == NULL)
        {
            CcInitializeCacheMap(FileObject, (PCC_FILE_SIZES)&Fcb->RFCB.AllocationSize,
                                 FALSE, &NtfsGlobalData->CacheMgrCallbacks, Fcb);
        }
        if (!CcCopyWrite(FileObject, &Offset, Length, TRUE, Buffer))
            Status = STATUS_CANT_WAIT;
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = _SEH2_GetExceptionCode();
    }
    _SEH2_END;
    if (!NT_SUCCESS(Status))
        return Status;

    CcFlushCache(FileObject->SectionObjectPointer, &Offset, Length, &IoStatus);
    if (!NT_SUCCESS(IoStatus.Status))
        return IoStatus.Status;

    if (End.QuadPart > Fcb->RFCB.ValidDataLength.QuadPart)
    {
        Fcb->RFCB.ValidDataLength = End;
        CcSetFileSizes(FileObject, (PCC_FILE_SIZES)&Fcb->RFCB.AllocationSize);
    }
    *Written = Length;
    return STATUS_SUCCESS;
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
    ULONG NotifyFilter = 0;
    PDEVICE_OBJECT DeviceObject = NULL;
    PDEVICE_EXTENSION DeviceExt = NULL;
    PFILE_OBJECT FileObject = NULL;
    PIRP Irp = NULL;
    ULONG BytesPerSector;
    BOOLEAN PagingResourceAcquired = FALSE;

    DPRINT("NtfsWrite(IrpContext %p)\n", IrpContext);
    ASSERT(IrpContext);

    // get the I/O request packet
    Irp = IrpContext->Irp;

    // This request is not allowed on the main device object
    if (IrpContext->DeviceObject == NtfsGlobalData->DeviceObject)
    {
        DPRINT1("\t\t\t\tNtfsWrite is called with the main device object.\n");

        Irp->IoStatus.Information = 0;
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    // get the File control block
    Fcb = (PNTFS_FCB)IrpContext->FileObject->FsContext;
    ASSERT(Fcb);

    DPRINT("About to write %wS\n", Fcb->ObjectName);
    DPRINT("NTFS Version: %d.%d\n", Fcb->Vcb->NtfsInfo.MajorVersion, Fcb->Vcb->NtfsInfo.MinorVersion);

    // setup some more locals
    FileObject = IrpContext->FileObject;
    DeviceObject = IrpContext->DeviceObject;
    DeviceExt = DeviceObject->DeviceExtension;
    BytesPerSector = DeviceExt->StorageDevice->SectorSize;
    Length = IrpContext->Stack->Parameters.Write.Length;

    // get the file offset we'll be writing to
    ByteOffset = IrpContext->Stack->Parameters.Write.ByteOffset;
    if (ByteOffset.u.LowPart == FILE_WRITE_TO_END_OF_FILE &&
        ByteOffset.u.HighPart == -1)
    {
        ByteOffset.QuadPart = Fcb->RFCB.FileSize.QuadPart;
    }

    DPRINT("ByteOffset: %I64u\tLength: %lu\tBytes per sector: %lu\n", ByteOffset.QuadPart,
        Length, BytesPerSector);

    // Is this a non-cached write? A non-buffered write?
    if (IrpContext->Irp->Flags & (IRP_PAGING_IO | IRP_NOCACHE) || (Fcb->Flags & FCB_IS_VOLUME) ||
        IrpContext->FileObject->Flags & FILE_NO_INTERMEDIATE_BUFFERING)
    {
        // non-cached and non-buffered writes must be sector aligned
        if (ByteOffset.QuadPart % BytesPerSector != 0 || Length % BytesPerSector != 0)
        {
            DPRINT1("Non-cached writes and non-buffered writes must be sector aligned!\n");
            return STATUS_INVALID_PARAMETER;
        }
    }

    /* A zero length write is a valid no-op */
    if (Length == 0)
    {
        // FIXME: Update last write time
        IrpContext->Irp->IoStatus.Information = 0;
        return STATUS_SUCCESS;
    }

    /* Is this an async request to a file? Serving a write means reading the
     * file record, walking its attributes and possibly growing the allocation,
     * all of it blocking metadata I/O, so post it to a worker thread which
     * runs the same path with IRPCONTEXT_CANWAIT set. Lock the user buffer
     * here, while we're still in the requesting thread's address space. */
    if (!(IrpContext->Flags & IRPCONTEXT_CANWAIT) && !(Fcb->Flags & FCB_IS_VOLUME))
    {
        Status = NtfsLockUserBuffer(Irp, Length, IoReadAccess);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("Unable to lock user buffer!\n");
            return Status;
        }

        return NtfsMarkIrpContextForQueue(IrpContext);
    }

    // get the Resource
    if (Fcb->Flags & FCB_IS_VOLUME_STREAM)
    {
        Resource = &Fcb->PagingIoResource;
    }
    else if (Fcb->Flags & FCB_IS_VOLUME)
    {
        Resource = &DeviceExt->DirResource;
    }
    else if (IrpContext->Irp->Flags & IRP_PAGING_IO)
    {
        Resource = &Fcb->PagingIoResource;
    }
    else
    {
        Resource = &Fcb->MainResource;
    }

    // acquire exclusive access to the Resource
    if (!ExAcquireResourceExclusiveLite(Resource, BooleanFlagOn(IrpContext->Flags, IRPCONTEXT_CANWAIT)))
    {
        return STATUS_CANT_WAIT;
    }

    if (Resource == &Fcb->MainResource)
    {
        ExAcquireResourceExclusiveLite(&Fcb->PagingIoResource, TRUE);
        PagingResourceAcquired = TRUE;
    }

    /* A directory has no data stream and no byte-range locks; it is
     * refused below. */
    if (!(Fcb->Flags & (FCB_IS_VOLUME | FCB_IS_VOLUME_STREAM)) &&
        !NtfsFCBIsDirectory(Fcb) &&
        !NtfsCheckFileLock(Fcb, Irp, &ByteOffset, Length, TRUE))
    {
        if (PagingResourceAcquired)
            ExReleaseResourceLite(&Fcb->PagingIoResource);
        ExReleaseResourceLite(Resource);
        Irp->IoStatus.Information = 0;
        return STATUS_FILE_LOCK_CONFLICT;
    }

    // get the buffer of data the user is trying to write
    Buffer = NtfsGetUserBuffer(Irp, BooleanFlagOn(Irp->Flags, IRP_PAGING_IO));
    ASSERT(Buffer);

    // lock the buffer
    Status = NtfsLockUserBuffer(Irp, Length, IoReadAccess);

    // were we unable to lock the buffer?
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Unable to lock user buffer!\n");

        if (PagingResourceAcquired)
            ExReleaseResourceLite(&Fcb->PagingIoResource);
        ExReleaseResourceLite(Resource);
        return Status;
    }

    DPRINT("Existing File Size(Fcb->RFCB.FileSize.QuadPart): %I64u\n", Fcb->RFCB.FileSize.QuadPart);
    DPRINT("About to write the data. Length: %lu\n", Length);

    /* See NtfsRead() */
    if ((DeviceExt->Flags & VCB_VOLUME_DISMOUNTED) &&
        !(Fcb->Flags & FCB_IS_VOLUME))
    {
        if (PagingResourceAcquired)
            ExReleaseResourceLite(&Fcb->PagingIoResource);
        ExReleaseResourceLite(Resource);
        Irp->IoStatus.Information = 0;
        return STATUS_VOLUME_DISMOUNTED;
    }

    // write the file
    if (Fcb->Flags & (FCB_IS_VOLUME | FCB_IS_VOLUME_STREAM))
    {
        /* Raw access to the partition, including after a dismount. */
        Status = NtfsReadWriteVolume(DeviceExt,
                                     IRP_MJ_WRITE,
                                     Buffer,
                                     Length,
                                     ByteOffset.QuadPart,
                                     Irp,
                                     &ReturnedWriteLength);
    }
    else if (NtfsFCBIsDirectory(Fcb))
    {
        /* A directory has no data stream to write */
        if (PagingResourceAcquired)
            ExReleaseResourceLite(&Fcb->PagingIoResource);
        ExReleaseResourceLite(Resource);
        Irp->IoStatus.Information = 0;
        return STATUS_INVALID_DEVICE_REQUEST;
    }
    else
    {
        LONGLONG OldSize = Fcb->RFCB.FileSize.QuadPart;

        Status = NtfsCheckQuotaGrowth(IrpContext);
        if (!NT_SUCCESS(Status))
            goto WriteDone;

        if (NtfsUseDataCache(Fcb, FileObject, Irp))
        {
            Status = NtfsCachedWriteFile(DeviceExt, FileObject, Buffer, ByteOffset, Length,
                                         BooleanFlagOn(IrpContext->Stack->Flags, SL_CASE_SENSITIVE),
                                         &ReturnedWriteLength);
        }
        else
        {
            /* Direct writes must invalidate the file cache after flushing it.
             * Paging writes are themselves that flush, and must never purge. */
            if (!(Irp->Flags & IRP_PAGING_IO))
            {
                IO_STATUS_BLOCK IoStatus;
                CcFlushCache(FileObject->SectionObjectPointer, NULL, 0, &IoStatus);
                Status = IoStatus.Status;
                if (NT_SUCCESS(Status))
                    CcPurgeCacheSection(FileObject->SectionObjectPointer, NULL, 0, FALSE);
            }
            if (NT_SUCCESS(Status))
            {
                Status = NtfsWriteFile(DeviceExt, FileObject, Buffer, Length,
                                       ByteOffset.QuadPart, Irp->Flags,
                                       BooleanFlagOn(IrpContext->Stack->Flags, SL_CASE_SENSITIVE),
                                       Irp, &ReturnedWriteLength);
            }
        }

        /* Paging I/O only writes back what a write already told the journal about */
        if (NT_SUCCESS(Status) && !(Irp->Flags & IRP_PAGING_IO) && ReturnedWriteLength != 0)
        {
            NotifyFilter = FILE_NOTIFY_CHANGE_LAST_WRITE;
            if (Fcb->RFCB.FileSize.QuadPart != OldSize)
                NotifyFilter |= FILE_NOTIFY_CHANGE_SIZE;
            NtfsPostUsnChange(DeviceExt,
                              Fcb,
                              NtfsUsnDataReason(Fcb,
                                                ByteOffset.QuadPart + ReturnedWriteLength > OldSize ?
                                                USN_REASON_DATA_EXTEND : USN_REASON_DATA_OVERWRITE));
        }
    }

WriteDone:
    IrpContext->Irp->IoStatus.Status = Status;

    // was the write successful?
    if (NT_SUCCESS(Status))
    {
        if (FileObject->Flags & FO_SYNCHRONOUS_IO)
        {
            // advance the file pointer
            FileObject->CurrentByteOffset.QuadPart = ByteOffset.QuadPart + ReturnedWriteLength;
        }

        IrpContext->PriorityBoost = IO_DISK_INCREMENT;
    }
    else
    {
        DPRINT1("Write not Succesful!\tReturned length: %lu\n", ReturnedWriteLength);
    }

    Irp->IoStatus.Information = ReturnedWriteLength;

    // Note: We leave the user buffer that we locked alone, it's up to the I/O manager to unlock and free it

    if (PagingResourceAcquired)
        ExReleaseResourceLite(&Fcb->PagingIoResource);
    ExReleaseResourceLite(Resource);

    if (NotifyFilter != 0)
        NtfsReportDataChange(Fcb, NotifyFilter);

    if (NT_SUCCESS(Status) && !(Irp->Flags & IRP_PAGING_IO) && ReturnedWriteLength != 0)
    {
        FileObject->Flags |= FO_FILE_MODIFIED;
        /* Write-through makes this write durable, not the whole volume. The
         * cached path has already flushed the written range of the file into
         * the volume stream, and the direct path wrote it there. */
        if (FileObject->Flags & FO_WRITE_THROUGH)
            Status = NtfsFlushVolumeStream(DeviceExt);
    }
    return Status;
}

/* EOF */
