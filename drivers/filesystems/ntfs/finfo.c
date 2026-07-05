/*
 *  ReactOS kernel
 *  Copyright (C) 2002 ReactOS Team
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
 * FILE:             drivers/filesystem/ntfs/dirctl.c
 * PURPOSE:          NTFS filesystem driver
 * PROGRAMMERS:      Eric Kohl
 *                   Hervé Poussineau (hpoussin@reactos.org)
 *                   Pierre Schweitzer (pierre@reactos.org)
 */

/* INCLUDES *****************************************************************/

#include "ntfs.h"

#define NDEBUG
#include <debug.h>

/* FUNCTIONS ****************************************************************/

/*
 * FUNCTION: Retrieve the standard file information
 */
static
NTSTATUS
NtfsGetStandardInformation(PNTFS_FCB Fcb,
                           PDEVICE_OBJECT DeviceObject,
                           PFILE_STANDARD_INFORMATION StandardInfo,
                           PULONG BufferLength)
{
    UNREFERENCED_PARAMETER(DeviceObject);

    DPRINT("NtfsGetStandardInformation(%p, %p, %p, %p)\n", Fcb, DeviceObject, StandardInfo, BufferLength);

    if (*BufferLength < sizeof(FILE_STANDARD_INFORMATION))
        return STATUS_BUFFER_TOO_SMALL;

    /* PRECONDITION */
    ASSERT(StandardInfo != NULL);
    ASSERT(Fcb != NULL);

    RtlZeroMemory(StandardInfo,
                  sizeof(FILE_STANDARD_INFORMATION));

    StandardInfo->AllocationSize = Fcb->RFCB.AllocationSize;
    StandardInfo->EndOfFile = Fcb->RFCB.FileSize;
    StandardInfo->NumberOfLinks = Fcb->LinkCount;
    StandardInfo->DeletePending = BooleanFlagOn(Fcb->Flags, FCB_DELETE_PENDING);
    StandardInfo->Directory = NtfsFCBIsDirectory(Fcb);

    *BufferLength -= sizeof(FILE_STANDARD_INFORMATION);

    return STATUS_SUCCESS;
}


static
NTSTATUS
NtfsGetPositionInformation(PFILE_OBJECT FileObject,
                           PFILE_POSITION_INFORMATION PositionInfo,
                           PULONG BufferLength)
{
    DPRINT("NtfsGetPositionInformation(%p, %p, %p)\n", FileObject, PositionInfo, BufferLength);

    if (*BufferLength < sizeof(FILE_POSITION_INFORMATION))
        return STATUS_BUFFER_TOO_SMALL;

    PositionInfo->CurrentByteOffset.QuadPart = FileObject->CurrentByteOffset.QuadPart;

    DPRINT("Getting position %I64x\n",
           PositionInfo->CurrentByteOffset.QuadPart);

    *BufferLength -= sizeof(FILE_POSITION_INFORMATION);

    return STATUS_SUCCESS;
}


static
NTSTATUS
NtfsGetBasicInformation(PFILE_OBJECT FileObject,
                        PNTFS_FCB Fcb,
                        PDEVICE_OBJECT DeviceObject,
                        PFILE_BASIC_INFORMATION BasicInfo,
                        PULONG BufferLength)
{
    PFILENAME_ATTRIBUTE FileName = &Fcb->Entry;

    DPRINT("NtfsGetBasicInformation(%p, %p, %p, %p, %p)\n", FileObject, Fcb, DeviceObject, BasicInfo, BufferLength);

    if (*BufferLength < sizeof(FILE_BASIC_INFORMATION))
        return STATUS_BUFFER_TOO_SMALL;

    RtlZeroMemory(BasicInfo, sizeof(FILE_BASIC_INFORMATION));

    BasicInfo->CreationTime.QuadPart = FileName->CreationTime;
    BasicInfo->LastAccessTime.QuadPart = FileName->LastAccessTime;
    BasicInfo->LastWriteTime.QuadPart = FileName->LastWriteTime;
    BasicInfo->ChangeTime.QuadPart = FileName->ChangeTime;

    NtfsFileFlagsToAttributes(FileName->FileAttributes, &BasicInfo->FileAttributes);

    *BufferLength -= sizeof(FILE_BASIC_INFORMATION);

    return STATUS_SUCCESS;
}


/*
 * FUNCTION: Retrieve the file name information
 */
static
NTSTATUS
NtfsGetNameInformation(PFILE_OBJECT FileObject,
                       PNTFS_FCB Fcb,
                       PDEVICE_OBJECT DeviceObject,
                       PFILE_NAME_INFORMATION NameInfo,
                       PULONG BufferLength)
{
    ULONG BytesToCopy;

    UNREFERENCED_PARAMETER(FileObject);
    UNREFERENCED_PARAMETER(DeviceObject);

    DPRINT("NtfsGetNameInformation(%p, %p, %p, %p, %p)\n", FileObject, Fcb, DeviceObject, NameInfo, BufferLength);

    ASSERT(NameInfo != NULL);
    ASSERT(Fcb != NULL);

    /* If buffer can't hold at least the file name length, bail out */
    if (*BufferLength < (ULONG)FIELD_OFFSET(FILE_NAME_INFORMATION, FileName[0]))
        return STATUS_BUFFER_TOO_SMALL;

    /* Save file name length, and as much file len, as buffer length allows */
    NameInfo->FileNameLength = wcslen(Fcb->PathName) * sizeof(WCHAR);

    /* Calculate amount of bytes to copy not to overflow the buffer */
    BytesToCopy = min(NameInfo->FileNameLength,
                      *BufferLength - FIELD_OFFSET(FILE_NAME_INFORMATION, FileName[0]));

    /* Fill in the bytes */
    RtlCopyMemory(NameInfo->FileName, Fcb->PathName, BytesToCopy);

    /* Check if we could write more but are not able to */
    if (*BufferLength < NameInfo->FileNameLength + (ULONG)FIELD_OFFSET(FILE_NAME_INFORMATION, FileName[0]))
    {
        /* Return number of bytes written */
        *BufferLength -= FIELD_OFFSET(FILE_NAME_INFORMATION, FileName[0]) + BytesToCopy;
        return STATUS_BUFFER_OVERFLOW;
    }

    /* We filled up as many bytes, as needed */
    *BufferLength -= (FIELD_OFFSET(FILE_NAME_INFORMATION, FileName[0]) + NameInfo->FileNameLength);

    return STATUS_SUCCESS;
}


static
NTSTATUS
NtfsGetInternalInformation(PNTFS_FCB Fcb,
                           PFILE_INTERNAL_INFORMATION InternalInfo,
                           PULONG BufferLength)
{
    DPRINT("NtfsGetInternalInformation(%p, %p, %p)\n", Fcb, InternalInfo, BufferLength);

    ASSERT(InternalInfo);
    ASSERT(Fcb);

    if (*BufferLength < sizeof(FILE_INTERNAL_INFORMATION))
        return STATUS_BUFFER_TOO_SMALL;

    InternalInfo->IndexNumber.QuadPart = Fcb->MFTIndex;

    *BufferLength -= sizeof(FILE_INTERNAL_INFORMATION);

    return STATUS_SUCCESS;
}

static
NTSTATUS
NtfsGetAttributeTagInformation(PNTFS_FCB Fcb,
                               PDEVICE_EXTENSION DeviceExt,
                               PFILE_ATTRIBUTE_TAG_INFORMATION AttributeTagInfo,
                               PULONG BufferLength)
{
    PFILENAME_ATTRIBUTE FileName = &Fcb->Entry;

    DPRINT("NtfsGetAttributeTagInformation(%p, %p, %p, %p)\n", Fcb, DeviceExt, AttributeTagInfo, BufferLength);

    if (*BufferLength < sizeof(FILE_ATTRIBUTE_TAG_INFORMATION))
        return STATUS_BUFFER_TOO_SMALL;

    NtfsFileFlagsToAttributes(FileName->FileAttributes,
                              &AttributeTagInfo->FileAttributes);
    AttributeTagInfo->ReparseTag = 0;

    if (NtfsFCBIsReparsePoint(Fcb))
    {
        PFILE_RECORD_HEADER FileRecord;

        FileRecord = ExAllocateFromNPagedLookasideList(&DeviceExt->FileRecLookasideList);
        if (FileRecord == NULL)
            return STATUS_INSUFFICIENT_RESOURCES;

        if (NT_SUCCESS(ReadFileRecord(DeviceExt, Fcb->MFTIndex, FileRecord)))
        {
            ULONG ReparseTag;

            if (NT_SUCCESS(NtfsGetReparsePointFromRecord(DeviceExt,
                                                         FileRecord,
                                                         &ReparseTag,
                                                         NULL,
                                                         0,
                                                         NULL)))
            {
                AttributeTagInfo->ReparseTag = ReparseTag;
            }
        }

        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
    }

    *BufferLength -= sizeof(FILE_ATTRIBUTE_TAG_INFORMATION);
    return STATUS_SUCCESS;
}


static
NTSTATUS
NtfsGetNetworkOpenInformation(PNTFS_FCB Fcb,
                              PDEVICE_EXTENSION DeviceExt,
                              PFILE_NETWORK_OPEN_INFORMATION NetworkInfo,
                              PULONG BufferLength)
{
    PFILENAME_ATTRIBUTE FileName = &Fcb->Entry;

    DPRINT("NtfsGetNetworkOpenInformation(%p, %p, %p, %p)\n", Fcb, DeviceExt, NetworkInfo, BufferLength);

    if (*BufferLength < sizeof(FILE_NETWORK_OPEN_INFORMATION))
        return STATUS_BUFFER_TOO_SMALL;

    NetworkInfo->CreationTime.QuadPart = FileName->CreationTime;
    NetworkInfo->LastAccessTime.QuadPart = FileName->LastAccessTime;
    NetworkInfo->LastWriteTime.QuadPart = FileName->LastWriteTime;
    NetworkInfo->ChangeTime.QuadPart = FileName->ChangeTime;

    NetworkInfo->EndOfFile = Fcb->RFCB.FileSize;
    NetworkInfo->AllocationSize = Fcb->RFCB.AllocationSize;

    NtfsFileFlagsToAttributes(FileName->FileAttributes, &NetworkInfo->FileAttributes);

    *BufferLength -= sizeof(FILE_NETWORK_OPEN_INFORMATION);
    return STATUS_SUCCESS;
}

static
NTSTATUS
NtfsGetStreamInformation(PNTFS_FCB Fcb,
                         PDEVICE_EXTENSION DeviceExt,
                         PFILE_STREAM_INFORMATION StreamInfo,
                         PULONG BufferLength)
{
    ULONG CurrentSize;
    FIND_ATTR_CONTXT Context;
    PNTFS_ATTR_RECORD Attribute;
    NTSTATUS Status, BrowseStatus;
    PFILE_RECORD_HEADER FileRecord;
    PFILE_STREAM_INFORMATION CurrentInfo = StreamInfo, Previous = NULL;

    if (*BufferLength < sizeof(FILE_STREAM_INFORMATION))
        return STATUS_BUFFER_TOO_SMALL;

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

    BrowseStatus = FindFirstAttribute(&Context, DeviceExt, FileRecord, FALSE, &Attribute);
    while (NT_SUCCESS(BrowseStatus))
    {
        if (Attribute->Type == AttributeData)
        {
            CurrentSize = FIELD_OFFSET(FILE_STREAM_INFORMATION, StreamName) + Attribute->NameLength * sizeof(WCHAR) + wcslen(L"::$DATA") * sizeof(WCHAR);

            if (CurrentSize > *BufferLength)
            {
                Status = STATUS_BUFFER_OVERFLOW;
                break;
            }

            CurrentInfo->NextEntryOffset = 0;
            CurrentInfo->StreamNameLength = (Attribute->NameLength + wcslen(L"::$DATA")) * sizeof(WCHAR);
            CurrentInfo->StreamSize.QuadPart = AttributeDataLength(Attribute);
            CurrentInfo->StreamAllocationSize.QuadPart = AttributeAllocatedLength(Attribute);
            CurrentInfo->StreamName[0] = L':';
            RtlMoveMemory(&CurrentInfo->StreamName[1], (PWCHAR)((ULONG_PTR)Attribute + Attribute->NameOffset), CurrentInfo->StreamNameLength);
            RtlMoveMemory(&CurrentInfo->StreamName[Attribute->NameLength + 1], L":$DATA", sizeof(L":$DATA") - sizeof(UNICODE_NULL));

            if (Previous != NULL)
            {
                Previous->NextEntryOffset = (ULONG_PTR)CurrentInfo - (ULONG_PTR)Previous;
            }
            Previous = CurrentInfo;
            CurrentInfo = (PFILE_STREAM_INFORMATION)((ULONG_PTR)CurrentInfo + CurrentSize);
            *BufferLength -= CurrentSize;
        }

        BrowseStatus = FindNextAttribute(&Context, &Attribute);
    }

    FindCloseAttribute(&Context);
    ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
    return Status;
}

/*
 * Retrieve the 8.3 DOS short name for the file, if any.  Returns
 * STATUS_OBJECT_NAME_NOT_FOUND when the file has no DOS-namespace
 * $FILE_NAME attribute (true for most files created by ReactOS since we
 * don't auto-generate short names yet).  Returns STATUS_BUFFER_OVERFLOW
 * if the caller's buffer is large enough for the header but not the
 * full name.
 */
static
NTSTATUS
NtfsGetAlternateNameInformation(PNTFS_FCB Fcb,
                                PDEVICE_EXTENSION DeviceExt,
                                PFILE_NAME_INFORMATION NameInfo,
                                PULONG BufferLength)
{
    PFILE_RECORD_HEADER FileRecord;
    PFILENAME_ATTRIBUTE ShortName;
    NTSTATUS Status;
    ULONG NameBytes;
    ULONG BytesToCopy;

    ASSERT(Fcb != NULL);
    ASSERT(NameInfo != NULL);

    if (*BufferLength < (ULONG)FIELD_OFFSET(FILE_NAME_INFORMATION, FileName[0]))
        return STATUS_BUFFER_TOO_SMALL;

    FileRecord = ExAllocateFromNPagedLookasideList(&DeviceExt->FileRecLookasideList);
    if (FileRecord == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = ReadFileRecord(DeviceExt, Fcb->MFTIndex, FileRecord);
    if (!NT_SUCCESS(Status))
    {
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
        return Status;
    }

    ShortName = GetFileNameFromRecord(DeviceExt, FileRecord, NTFS_FILE_NAME_DOS);
    if (ShortName == NULL)
    {
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }

    NameBytes = (ULONG)ShortName->NameLength * sizeof(WCHAR);
    NameInfo->FileNameLength = NameBytes;

    BytesToCopy = min(NameBytes,
                      *BufferLength - FIELD_OFFSET(FILE_NAME_INFORMATION, FileName[0]));
    RtlCopyMemory(NameInfo->FileName, ShortName->Name, BytesToCopy);

    ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);

    if (BytesToCopy < NameBytes)
    {
        *BufferLength -= FIELD_OFFSET(FILE_NAME_INFORMATION, FileName[0]) + BytesToCopy;
        return STATUS_BUFFER_OVERFLOW;
    }

    *BufferLength -= FIELD_OFFSET(FILE_NAME_INFORMATION, FileName[0]) + NameBytes;
    return STATUS_SUCCESS;
}

/*
 * Retrieve FILE_ALL_INFORMATION by calling the per-class handlers in
 * order and stitching the output into the caller's buffer.  The layout
 * of FILE_ALL_INFORMATION is a fixed prefix (Basic..Alignment) followed
 * by a variable-length FILE_NAME_INFORMATION.  We return
 * STATUS_BUFFER_OVERFLOW (with a truncated name) if the buffer is too
 * small for the full name, matching Win32 behavior.
 */
static
NTSTATUS
NtfsGetAllInformation(PFILE_OBJECT FileObject,
                      PNTFS_FCB Fcb,
                      PDEVICE_OBJECT DeviceObject,
                      PFILE_ALL_INFORMATION AllInfo,
                      PULONG BufferLength)
{
    NTSTATUS Status;
    ULONG NameOffset;
    ULONG NameBufferLength;

    /* The fixed prefix runs from BasicInformation through
     * AlignmentInformation; the NameInformation is the last field. */
    NameOffset = FIELD_OFFSET(FILE_ALL_INFORMATION, NameInformation);

    if (*BufferLength < NameOffset)
        return STATUS_BUFFER_TOO_SMALL;

    /* Fill the fixed prefix.  Each helper subtracts its own struct size
     * from *BufferLength on success, so track that via local copies. */
    {
        ULONG Tmp = sizeof(FILE_BASIC_INFORMATION);
        Status = NtfsGetBasicInformation(FileObject, Fcb, DeviceObject,
                                         &AllInfo->BasicInformation, &Tmp);
        if (!NT_SUCCESS(Status))
            return Status;
    }
    {
        ULONG Tmp = sizeof(FILE_STANDARD_INFORMATION);
        Status = NtfsGetStandardInformation(Fcb, DeviceObject,
                                            &AllInfo->StandardInformation, &Tmp);
        if (!NT_SUCCESS(Status))
            return Status;
    }
    {
        ULONG Tmp = sizeof(FILE_INTERNAL_INFORMATION);
        Status = NtfsGetInternalInformation(Fcb,
                                            &AllInfo->InternalInformation, &Tmp);
        if (!NT_SUCCESS(Status))
            return Status;
    }

    /* EA, Access, Mode, Alignment - NTFS doesn't track these per-file
     * so report the safe defaults the other ReactOS file systems use. */
    AllInfo->EaInformation.EaSize = 0;
    AllInfo->AccessInformation.AccessFlags = 0;
    AllInfo->PositionInformation.CurrentByteOffset = FileObject->CurrentByteOffset;
    AllInfo->ModeInformation.Mode = 0;
    AllInfo->AlignmentInformation.AlignmentRequirement = 0;

    /* Fill NameInformation from the remaining buffer space. */
    NameBufferLength = *BufferLength - NameOffset;
    Status = NtfsGetNameInformation(FileObject, Fcb, DeviceObject,
                                    &AllInfo->NameInformation, &NameBufferLength);

    /* Report the remaining (unwritten) buffer space, matching the
     * convention of the other NtfsGetXxxInformation helpers, so the caller
     * computes Information = requested length - remaining = bytes written.
     * The fixed prefix always consumes exactly NameOffset bytes, so the
     * leftover is whatever NtfsGetNameInformation did not use.  This must
     * hold on the STATUS_BUFFER_OVERFLOW path too: Windows fills the entire
     * fixed prefix and reports the bytes written even when the trailing
     * name does not fit, and callers (e.g. the Cygwin/MSYS2 runtime, which
     * passes a buffer of exactly sizeof(FILE_ALL_INFORMATION)) rely on the
     * prefix being copied back on overflow. */
    *BufferLength = NameBufferLength;

    /* NtfsGetNameInformation returns STATUS_BUFFER_OVERFLOW if the name
     * didn't fully fit - propagate that up. */
    return Status;
}

// Convert enum value to friendly name
const PCSTR
GetInfoClassName(FILE_INFORMATION_CLASS infoClass)
{
    const PCSTR fileInfoClassNames[] = { "???????",
        "FileDirectoryInformation",
        "FileFullDirectoryInformation",
        "FileBothDirectoryInformation",
        "FileBasicInformation",
        "FileStandardInformation",
        "FileInternalInformation",
        "FileEaInformation",
        "FileAccessInformation",
        "FileNameInformation",
        "FileRenameInformation",
        "FileLinkInformation",
        "FileNamesInformation",
        "FileDispositionInformation",
        "FilePositionInformation",
        "FileFullEaInformation",
        "FileModeInformation",
        "FileAlignmentInformation",
        "FileAllInformation",
        "FileAllocationInformation",
        "FileEndOfFileInformation",
        "FileAlternateNameInformation",
        "FileStreamInformation",
        "FilePipeInformation",
        "FilePipeLocalInformation",
        "FilePipeRemoteInformation",
        "FileMailslotQueryInformation",
        "FileMailslotSetInformation",
        "FileCompressionInformation",
        "FileObjectIdInformation",
        "FileCompletionInformation",
        "FileMoveClusterInformation",
        "FileQuotaInformation",
        "FileReparsePointInformation",
        "FileNetworkOpenInformation",
        "FileAttributeTagInformation",
        "FileTrackingInformation",
        "FileIdBothDirectoryInformation",
        "FileIdFullDirectoryInformation",
        "FileValidDataLengthInformation",
        "FileShortNameInformation",
        "FileIoCompletionNotificationInformation",
        "FileIoStatusBlockRangeInformation",
        "FileIoPriorityHintInformation",
        "FileSfioReserveInformation",
        "FileSfioVolumeInformation",
        "FileHardLinkInformation",
        "FileProcessIdsUsingFileInformation",
        "FileNormalizedNameInformation",
        "FileNetworkPhysicalNameInformation",
        "FileIdGlobalTxDirectoryInformation",
        "FileIsRemoteDeviceInformation",
        "FileAttributeCacheInformation",
        "FileNumaNodeInformation",
        "FileStandardLinkInformation",
        "FileRemoteProtocolInformation",
        "FileReplaceCompletionInformation",
        "FileMaximumInformation",
        "FileDirectoryInformation",
        "FileFullDirectoryInformation",
        "FileBothDirectoryInformation",
        "FileBasicInformation",
        "FileStandardInformation",
        "FileInternalInformation",
        "FileEaInformation",
        "FileAccessInformation",
        "FileNameInformation",
        "FileRenameInformation",
        "FileLinkInformation",
        "FileNamesInformation",
        "FileDispositionInformation",
        "FilePositionInformation",
        "FileFullEaInformation",
        "FileModeInformation",
        "FileAlignmentInformation",
        "FileAllInformation",
        "FileAllocationInformation",
        "FileEndOfFileInformation",
        "FileAlternateNameInformation",
        "FileStreamInformation",
        "FilePipeInformation",
        "FilePipeLocalInformation",
        "FilePipeRemoteInformation",
        "FileMailslotQueryInformation",
        "FileMailslotSetInformation",
        "FileCompressionInformation",
        "FileObjectIdInformation",
        "FileCompletionInformation",
        "FileMoveClusterInformation",
        "FileQuotaInformation",
        "FileReparsePointInformation",
        "FileNetworkOpenInformation",
        "FileAttributeTagInformation",
        "FileTrackingInformation",
        "FileIdBothDirectoryInformation",
        "FileIdFullDirectoryInformation",
        "FileValidDataLengthInformation",
        "FileShortNameInformation",
        "FileIoCompletionNotificationInformation",
        "FileIoStatusBlockRangeInformation",
        "FileIoPriorityHintInformation",
        "FileSfioReserveInformation",
        "FileSfioVolumeInformation",
        "FileHardLinkInformation",
        "FileProcessIdsUsingFileInformation",
        "FileNormalizedNameInformation",
        "FileNetworkPhysicalNameInformation",
        "FileIdGlobalTxDirectoryInformation",
        "FileIsRemoteDeviceInformation",
        "FileAttributeCacheInformation",
        "FileNumaNodeInformation",
        "FileStandardLinkInformation",
        "FileRemoteProtocolInformation",
        "FileReplaceCompletionInformation",
        "FileMaximumInformation" };
    return fileInfoClassNames[infoClass];
}

/*
 * FUNCTION: Retrieve the specified file information
 */
NTSTATUS
NtfsQueryInformation(PNTFS_IRP_CONTEXT IrpContext)
{
    FILE_INFORMATION_CLASS FileInformationClass;
    PIO_STACK_LOCATION Stack;
    PFILE_OBJECT FileObject;
    PNTFS_FCB Fcb;
    PVOID SystemBuffer;
    ULONG BufferLength;
    PIRP Irp;
    PDEVICE_OBJECT DeviceObject;
    NTSTATUS Status = STATUS_SUCCESS;

    NTFS_TRACE("NtfsQueryInformation(%p)\n", IrpContext);

    Irp = IrpContext->Irp;
    Stack = IrpContext->Stack;
    DeviceObject = IrpContext->DeviceObject;
    FileInformationClass = Stack->Parameters.QueryFile.FileInformationClass;
    FileObject = IrpContext->FileObject;
    Fcb = FileObject->FsContext;

    SystemBuffer = Irp->AssociatedIrp.SystemBuffer;
    BufferLength = Stack->Parameters.QueryFile.Length;

    if (!ExAcquireResourceSharedLite(&Fcb->MainResource,
                                     BooleanFlagOn(IrpContext->Flags, IRPCONTEXT_CANWAIT)))
    {
        return NtfsMarkIrpContextForQueue(IrpContext);
    }

    switch (FileInformationClass)
    {
        case FileStandardInformation:
            Status = NtfsGetStandardInformation(Fcb,
                                                DeviceObject,
                                                SystemBuffer,
                                                &BufferLength);
            break;

        case FilePositionInformation:
            Status = NtfsGetPositionInformation(FileObject,
                                                SystemBuffer,
                                                &BufferLength);
            break;

        case FileBasicInformation:
            Status = NtfsGetBasicInformation(FileObject,
                                             Fcb,
                                             DeviceObject,
                                             SystemBuffer,
                                             &BufferLength);
            break;

        case FileNameInformation:
            Status = NtfsGetNameInformation(FileObject,
                                            Fcb,
                                            DeviceObject,
                                            SystemBuffer,
                                            &BufferLength);
            break;

        case FileInternalInformation:
            Status = NtfsGetInternalInformation(Fcb,
                                                SystemBuffer,
                                                &BufferLength);
            break;

        case FileNetworkOpenInformation:
            Status = NtfsGetNetworkOpenInformation(Fcb,
                                                   DeviceObject->DeviceExtension,
                                                   SystemBuffer,
                                                   &BufferLength);
            break;

        case FileAttributeTagInformation:
            Status = NtfsGetAttributeTagInformation(Fcb,
                                                    DeviceObject->DeviceExtension,
                                                    SystemBuffer,
                                                    &BufferLength);
            break;

        case FileStreamInformation:
            Status = NtfsGetStreamInformation(Fcb,
                                              DeviceObject->DeviceExtension,
                                              SystemBuffer,
                                              &BufferLength);
            break;

        case FileAlternateNameInformation:
            Status = NtfsGetAlternateNameInformation(Fcb,
                                                     DeviceObject->DeviceExtension,
                                                     SystemBuffer,
                                                     &BufferLength);
            break;

        case FileAllInformation:
            Status = NtfsGetAllInformation(FileObject,
                                           Fcb,
                                           DeviceObject,
                                           SystemBuffer,
                                           &BufferLength);
            break;

        case FileEaInformation:
            if (BufferLength < sizeof(FILE_EA_INFORMATION))
                Status = STATUS_BUFFER_TOO_SMALL;
            else
            {
                PNTFS_ATTR_CONTEXT EaContext;
                PFILE_RECORD_HEADER FileRecord;
                ULONG EaSize = 0;

                FileRecord = ExAllocateFromNPagedLookasideList(&((PDEVICE_EXTENSION)DeviceObject->DeviceExtension)->FileRecLookasideList);
                if (FileRecord &&
                    NT_SUCCESS(ReadFileRecord((PDEVICE_EXTENSION)DeviceObject->DeviceExtension,
                                              Fcb->MFTIndex, FileRecord)))
                {
                    if (NT_SUCCESS(FindAttribute((PDEVICE_EXTENSION)DeviceObject->DeviceExtension,
                                                  FileRecord, AttributeEA, L"", 0, &EaContext, NULL)))
                    {
                        EaSize = (ULONG)AttributeDataLength(EaContext->pRecord);
                        ReleaseAttributeContext(EaContext);
                    }
                }
                if (FileRecord)
                    ExFreeToNPagedLookasideList(&((PDEVICE_EXTENSION)DeviceObject->DeviceExtension)->FileRecLookasideList, FileRecord);

                ((PFILE_EA_INFORMATION)SystemBuffer)->EaSize = EaSize;
                BufferLength -= sizeof(FILE_EA_INFORMATION);
                Status = STATUS_SUCCESS;
            }
            break;

        default:
            DPRINT1("Unimplemented information class: %s\n", GetInfoClassName(FileInformationClass));
            Status = STATUS_INVALID_PARAMETER;
    }

    ExReleaseResourceLite(&Fcb->MainResource);

    /* On STATUS_BUFFER_OVERFLOW the variable-length information classes
     * (e.g. FileAllInformation, FileNameInformation) still wrote the part
     * that fit; report those bytes so the I/O manager copies them back, as
     * Windows does.  Returning Information == 0 here would leave the caller's
     * buffer untouched even though we populated the fixed prefix. */
    if (NT_SUCCESS(Status) || Status == STATUS_BUFFER_OVERFLOW)
        Irp->IoStatus.Information =
            Stack->Parameters.QueryFile.Length - BufferLength;
    else
        Irp->IoStatus.Information = 0;

    return Status;
}

/**
* @name NtfsSetEndOfFile
* @implemented
*
* Sets the end of file (file size) for a given file.
*
* @param Fcb
* Pointer to an NTFS_FCB which describes the target file. Fcb->MainResource should have been
* acquired with ExAcquireResourceSharedLite().
*
* @param FileObject
* Pointer to a FILE_OBJECT describing the target file.
*
* @param DeviceExt
* Points to the target disk's DEVICE_EXTENSION
*
* @param IrpFlags
* ULONG describing the flags of the original IRP request (Irp->Flags).
*
* @param CaseSensitive
* Boolean indicating if the function should operate in case-sensitive mode. This will be TRUE
* if an application opened the file with the FILE_FLAG_POSIX_SEMANTICS flag.
*
* @param NewFileSize
* Pointer to a LARGE_INTEGER which indicates the new end of file (file size).
*
* @return
* STATUS_SUCCESS if successful,
* STATUS_USER_MAPPED_FILE if trying to truncate a file but MmCanFileBeTruncated() returned false,
* STATUS_OBJECT_NAME_NOT_FOUND if there was no $DATA attribute associated with the target file,
* STATUS_INVALID_PARAMETER if there was no $FILENAME attribute associated with the target file,
* STATUS_INSUFFICIENT_RESOURCES if an allocation failed,
* STATUS_ACCESS_DENIED if target file is a volume or if paging is involved.
*
* @remarks As this function sets the size of a file at the file-level
* (and not at the attribute level) it's not recommended to use this
* function alongside functions that operate on the data attribute directly.
*
*/
NTSTATUS
NtfsSetEndOfFile(PNTFS_FCB Fcb,
                 PFILE_OBJECT FileObject,
                 PDEVICE_EXTENSION DeviceExt,
                 ULONG IrpFlags,
                 BOOLEAN CaseSensitive,
                 PLARGE_INTEGER NewFileSize)
{
    LARGE_INTEGER CurrentFileSize;
    PFILE_RECORD_HEADER FileRecord;
    PNTFS_ATTR_CONTEXT DataContext;
    ULONG AttributeOffset;
    NTSTATUS Status = STATUS_SUCCESS;
    ULONGLONG AllocationSize;
    PFILENAME_ATTRIBUTE FileNameAttribute;
    ULONGLONG ParentMFTId;
    UNICODE_STRING FileName;


    /* Invalidate FCB-level MFT record cache: SetAttributeDataLength /
     * UpdateFileNameRecord are about to mutate the on-disk record. */
    NtfsInvalidateCachedFileRecord(Fcb);

    // Allocate non-paged memory for the file record
    FileRecord = ExAllocateFromNPagedLookasideList(&DeviceExt->FileRecLookasideList);
    if (FileRecord == NULL)
    {
        DPRINT1("Couldn't allocate memory for file record!");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    // read the file record
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

    CurrentFileSize.QuadPart = NtfsGetFileSize(DeviceExt, FileRecord, L"", 0, NULL);

    // Are we trying to decrease the file size?
    if (NewFileSize->QuadPart < CurrentFileSize.QuadPart)
    {
        // Is the file mapped?
        if (!MmCanFileBeTruncated(FileObject->SectionObjectPointer,
                                  NewFileSize))
        {
            DPRINT1("Couldn't decrease file size!\n");
            ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
            return STATUS_USER_MAPPED_FILE;
        }
    }

    // Find the attribute with the data stream for our file
    DPRINT("Finding Data Attribute...\n");
    Status = FindAttribute(DeviceExt,
                           FileRecord,
                           AttributeData,
                           Fcb->Stream,
                           wcslen(Fcb->Stream),
                           &DataContext,
                           &AttributeOffset);

    // Did we fail to find the attribute?
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("No '%S' data stream associated with file!\n", Fcb->Stream);
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
        return Status;
    }

    // Get the size of the data attribute
    CurrentFileSize.QuadPart = AttributeDataLength(DataContext->pRecord);

    // Are we enlarging the attribute?
    if (NewFileSize->QuadPart > CurrentFileSize.QuadPart)
    {
        // is increasing the stream size not allowed?
        if ((Fcb->Flags & FCB_IS_VOLUME) ||
            (IrpFlags & IRP_PAGING_IO))
        {
            // TODO - just fail for now
            ReleaseAttributeContext(DataContext);
            ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
            return STATUS_ACCESS_DENIED;
        }
    }

    // set the attribute data length
    DPRINT("NtfsSetEndOfFile: calling SetAttributeDataLength for MFT %I64u size %I64u\n", Fcb->MFTIndex, NewFileSize->QuadPart);
    Status = SetAttributeDataLength(FileObject, Fcb, DataContext, AttributeOffset, FileRecord, NewFileSize);
    NTFS_TRACE_IF(Fcb->MFTIndex == 160, "REGSTALL: eof after set attribute 0x%lx\n", Status);
    DPRINT("NtfsSetEndOfFile: SetAttributeDataLength returned 0x%lx\n", Status);
    if (!NT_SUCCESS(Status))
    {
        ReleaseAttributeContext(DataContext);
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
        return Status;
    }

    // now we need to update this file's size in every directory index entry that references it
    // TODO: expand to work with every filename / hardlink stored in the file record.
    DPRINT("NtfsSetEndOfFile: calling GetBestFileNameFromRecord\n");
    NTFS_TRACE_IF(Fcb->MFTIndex == 160, "REGSTALL: eof get filename begin\n");
    FileNameAttribute = GetBestFileNameFromRecord(Fcb->Vcb, FileRecord);
    NTFS_TRACE_IF(Fcb->MFTIndex == 160, "REGSTALL: eof get filename returned %p\n", FileNameAttribute);
    if (FileNameAttribute == NULL)
    {
        DPRINT1("Unable to find FileName attribute associated with file!\n");
        ReleaseAttributeContext(DataContext);
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
        return STATUS_INVALID_PARAMETER;
    }

    ParentMFTId = FileNameAttribute->DirectoryFileReferenceNumber & NTFS_MFT_MASK;

    FileName.Buffer = FileNameAttribute->Name;
    FileName.Length = FileNameAttribute->NameLength * sizeof(WCHAR);
    FileName.MaximumLength = FileName.Length;

    AllocationSize = AttributeAllocatedLength(DataContext->pRecord);
    NTFS_TRACE_IF(Fcb->MFTIndex == 160, "REGSTALL: eof update filename begin parent=%I64u name=%wZ alloc=%I64u\n",
                ParentMFTId,
                &FileName,
                AllocationSize);

    DPRINT("NtfsSetEndOfFile: calling UpdateFileNameRecord parent=%I64u allocSize=%I64u\n", ParentMFTId, AllocationSize);
    Status = UpdateFileNameRecord(Fcb->Vcb,
                                  ParentMFTId,
                                  &FileName,
                                  FALSE,
                                  NewFileSize->QuadPart,
                                  AllocationSize,
                                  CaseSensitive);
    NTFS_TRACE_IF(Fcb->MFTIndex == 160, "REGSTALL: eof update filename returned 0x%lx\n", Status);
    DPRINT("NtfsSetEndOfFile: UpdateFileNameRecord returned 0x%lx\n", Status);

    NTFS_TRACE_IF(Fcb->MFTIndex == 160, "REGSTALL: eof release attr begin ctx=%p\n", DataContext);
    ReleaseAttributeContext(DataContext);
    NTFS_TRACE_IF(Fcb->MFTIndex == 160, "REGSTALL: eof release attr done\n");
    ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
    NTFS_TRACE_IF(Fcb->MFTIndex == 160, "REGSTALL: eof free file record done\n");

    DPRINT("NtfsSetEndOfFile: returning 0x%lx\n", Status);
    NTFS_TRACE_IF(Fcb->MFTIndex == 160, "REGSTALL: eof returning 0x%lx\n", Status);
    return Status;
}

static
NTSTATUS
NtfsSetAllocationInformation(PNTFS_FCB Fcb,
                             PFILE_OBJECT FileObject,
                             PDEVICE_EXTENSION DeviceExt,
                             ULONG IrpFlags,
                             BOOLEAN CaseSensitive,
                             PLARGE_INTEGER NewAllocationSize)
{
    LARGE_INTEGER CurrentFileSize = Fcb->RFCB.FileSize;

    if (NewAllocationSize->QuadPart < 0)
        return STATUS_INVALID_PARAMETER;

    /*
     * FileAllocationInformation reserves storage; it must not extend EOF.
     * ReactOS NTFS does not yet have a separate allocation-only grow path for
     * resident streams, so accept grow/preallocation requests and let ordinary
     * writes allocate real clusters when data is written.
     */
    if (NewAllocationSize->QuadPart >= CurrentFileSize.QuadPart)
        return STATUS_SUCCESS;

    return NtfsSetEndOfFile(Fcb,
                            FileObject,
                            DeviceExt,
                            IrpFlags,
                            CaseSensitive,
                            NewAllocationSize);
}

/**
* @name NtfsSetBasicInformation
* @implemented
*
* Sets the basic file information (timestamps and attributes) for a file.
*/
static
NTSTATUS
NtfsSetBasicInformation(PNTFS_FCB Fcb,
                        PDEVICE_EXTENSION DeviceExt,
                        PFILE_BASIC_INFORMATION BasicInfo)
{
    PFILE_RECORD_HEADER FileRecord;
    PSTANDARD_INFORMATION StdInfo;
    NTSTATUS Status;

    DPRINT("NtfsSetBasicInformation: FCB %p MFT %I64u\n", Fcb, Fcb->MFTIndex);

    /* Invalidate FCB-level MFT record cache: timestamps / attributes about
     * to change on disk. */
    NtfsInvalidateCachedFileRecord(Fcb);

    FileRecord = ExAllocateFromNPagedLookasideList(&DeviceExt->FileRecLookasideList);
    if (FileRecord == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = ReadFileRecord(DeviceExt, Fcb->MFTIndex, FileRecord);
    if (!NT_SUCCESS(Status))
    {
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
        return Status;
    }

    StdInfo = GetStandardInformationFromRecord(DeviceExt, FileRecord);
    if (StdInfo == NULL)
    {
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }

    /* Only update fields that the caller provided (non-zero means set) */
    if (BasicInfo->CreationTime.QuadPart != 0)
    {
        StdInfo->CreationTime = BasicInfo->CreationTime.QuadPart;
        Fcb->Entry.CreationTime = BasicInfo->CreationTime.QuadPart;
    }
    if (BasicInfo->LastAccessTime.QuadPart != 0)
    {
        StdInfo->LastAccessTime = BasicInfo->LastAccessTime.QuadPart;
        Fcb->Entry.LastAccessTime = BasicInfo->LastAccessTime.QuadPart;
    }
    if (BasicInfo->LastWriteTime.QuadPart != 0)
    {
        StdInfo->LastWriteTime = BasicInfo->LastWriteTime.QuadPart;
        Fcb->Entry.LastWriteTime = BasicInfo->LastWriteTime.QuadPart;
    }
    if (BasicInfo->ChangeTime.QuadPart != 0)
    {
        StdInfo->ChangeTime = BasicInfo->ChangeTime.QuadPart;
        Fcb->Entry.ChangeTime = BasicInfo->ChangeTime.QuadPart;
    }
    if (BasicInfo->FileAttributes != 0)
    {
        StdInfo->FileAttribute = (StdInfo->FileAttribute & ~0x3FB7) |
                                 (BasicInfo->FileAttributes & 0x3FB7);
    }

    Status = UpdateFileRecord(DeviceExt, Fcb->MFTIndex, FileRecord);
    DPRINT("NtfsSetBasicInformation: UpdateFileRecord returned 0x%lx\n", Status);

    ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, FileRecord);
    return Status;
}

static
BOOLEAN
NtfsIsDotName(PUNICODE_STRING Name)
{
    if (Name->Length == sizeof(WCHAR) &&
        Name->Buffer[0] == L'.')
    {
        return TRUE;
    }

    if (Name->Length == 2 * sizeof(WCHAR) &&
        Name->Buffer[0] == L'.' &&
        Name->Buffer[1] == L'.')
    {
        return TRUE;
    }

    return FALSE;
}

static
VOID
NtfsUpdateFcbPathName(PNTFS_FCB Fcb,
                      PUNICODE_STRING PathName)
{
    WCHAR *ObjectName;

    ASSERT(PathName->Length / sizeof(WCHAR) < MAX_PATH);

    RtlCopyMemory(Fcb->PathName, PathName->Buffer, PathName->Length);
    Fcb->PathName[PathName->Length / sizeof(WCHAR)] = UNICODE_NULL;

    ObjectName = wcsrchr(Fcb->PathName, L'\\');
    if (ObjectName != NULL && ObjectName[1] != UNICODE_NULL)
        Fcb->ObjectName = ObjectName + 1;
    else
        Fcb->ObjectName = Fcb->PathName;
}

static
NTSTATUS
NtfsBuildRenamePath(PNTFS_FCB Fcb,
                    PFILE_RENAME_INFORMATION RenameInfo,
                    PFILE_OBJECT TargetFileObject,
                    PUNICODE_STRING FullPath,
                    PUNICODE_STRING ParentPath,
                    PUNICODE_STRING LeafName)
{
    UNICODE_STRING RenameName;
    UNICODE_STRING Prefix;
    UNICODE_STRING LeafComponent;
    PWCHAR LastSeparator;
    USHORT TotalLength;
    USHORT Index;
    BOOLEAN AddSeparator = FALSE;
    PNTFS_FCB TargetFcb = NULL;

    RenameName.Buffer = RenameInfo->FileName;
    RenameName.Length = (USHORT)RenameInfo->FileNameLength;
    RenameName.MaximumLength = RenameName.Length;

    if (RenameName.Length == 0 || FsRtlDoesNameContainWildCards(&RenameName))
        return STATUS_OBJECT_NAME_INVALID;

    RtlInitEmptyUnicodeString(&Prefix, NULL, 0);
    LeafComponent = RenameName;

    if (RenameInfo->RootDirectory != NULL)
    {
        if (TargetFileObject == NULL || TargetFileObject->FsContext == NULL)
            return STATUS_INVALID_PARAMETER;

        TargetFcb = TargetFileObject->FsContext;
        if (TargetFcb->Vcb != Fcb->Vcb)
            return STATUS_NOT_SAME_DEVICE;

        Prefix.Buffer = TargetFcb->PathName;
        Prefix.Length = Prefix.MaximumLength = (USHORT)(wcslen(TargetFcb->PathName) * sizeof(WCHAR));
        AddSeparator = !NtfsFCBIsRoot(TargetFcb);
    }
    else if (RenameName.Buffer[0] == L'\\' &&
             TargetFileObject != NULL &&
             TargetFileObject->FsContext != NULL)
    {
        TargetFcb = TargetFileObject->FsContext;
        if (TargetFcb->Vcb != Fcb->Vcb)
            return STATUS_NOT_SAME_DEVICE;

        Prefix.Buffer = TargetFcb->PathName;
        Prefix.Length = Prefix.MaximumLength = (USHORT)(wcslen(TargetFcb->PathName) * sizeof(WCHAR));
        AddSeparator = !NtfsFCBIsRoot(TargetFcb);

        /* RenameInfo->FileName is a counted buffer and is not guaranteed to be
         * NUL-terminated, so bound the search to RenameName.Length instead of
         * using wcsrchr(), which would scan past the buffer into adjacent pool
         * and yield a negative leaf length (overflowing TotalLength below). */
        LastSeparator = NULL;
        for (Index = RenameName.Length / sizeof(WCHAR); Index > 0; Index--)
        {
            if (RenameName.Buffer[Index - 1] == L'\\')
            {
                LastSeparator = &RenameName.Buffer[Index - 1];
                break;
            }
        }
        if (LastSeparator == NULL ||
            (PUCHAR)(LastSeparator + 1) >= (PUCHAR)RenameName.Buffer + RenameName.Length)
        {
            return STATUS_OBJECT_NAME_INVALID;
        }

        LeafComponent.Buffer = LastSeparator + 1;
        LeafComponent.Length = (USHORT)(RenameName.Length - ((PUCHAR)LeafComponent.Buffer - (PUCHAR)RenameName.Buffer));
        LeafComponent.MaximumLength = LeafComponent.Length;
    }
    else if (RenameName.Buffer[0] != L'\\')
    {
        WCHAR *CurrentName = wcsrchr(Fcb->PathName, L'\\');
        if (CurrentName == NULL)
            return STATUS_OBJECT_NAME_INVALID;

        Prefix.Buffer = Fcb->PathName;
        if (CurrentName == Fcb->PathName)
            Prefix.Length = Prefix.MaximumLength = sizeof(WCHAR);
        else
            Prefix.Length = Prefix.MaximumLength = (USHORT)((CurrentName - Fcb->PathName) * sizeof(WCHAR));
        AddSeparator = (Prefix.Length != sizeof(WCHAR));
    }

    if (Prefix.Length != 0)
    {
        TotalLength = Prefix.Length + LeafComponent.Length + sizeof(WCHAR);
        if (AddSeparator)
            TotalLength += sizeof(WCHAR);

        if (TotalLength / sizeof(WCHAR) >= MAX_PATH)
            return STATUS_OBJECT_NAME_INVALID;

        FullPath->Buffer = ExAllocatePoolWithTag(NonPagedPool, TotalLength, TAG_NTFS);
        if (FullPath->Buffer == NULL)
            return STATUS_INSUFFICIENT_RESOURCES;

        FullPath->Length = 0;
        FullPath->MaximumLength = TotalLength;

        RtlCopyMemory(FullPath->Buffer, Prefix.Buffer, Prefix.Length);
        FullPath->Length = Prefix.Length;
        if (AddSeparator)
        {
            FullPath->Buffer[FullPath->Length / sizeof(WCHAR)] = L'\\';
            FullPath->Length += sizeof(WCHAR);
        }

        RtlCopyMemory((PUCHAR)FullPath->Buffer + FullPath->Length,
                      LeafComponent.Buffer,
                      LeafComponent.Length);
        FullPath->Length += LeafComponent.Length;
        FullPath->Buffer[FullPath->Length / sizeof(WCHAR)] = UNICODE_NULL;
    }
    else
    {
        if (RenameName.Length / sizeof(WCHAR) >= MAX_PATH)
            return STATUS_OBJECT_NAME_INVALID;

        FullPath->MaximumLength = RenameName.Length + sizeof(WCHAR);
        FullPath->Buffer = ExAllocatePoolWithTag(NonPagedPool, FullPath->MaximumLength, TAG_NTFS);
        if (FullPath->Buffer == NULL)
            return STATUS_INSUFFICIENT_RESOURCES;

        RtlCopyMemory(FullPath->Buffer, RenameName.Buffer, RenameName.Length);
        FullPath->Length = RenameName.Length;
        FullPath->Buffer[FullPath->Length / sizeof(WCHAR)] = UNICODE_NULL;
    }

    LastSeparator = wcsrchr(FullPath->Buffer, L'\\');
    if (LastSeparator == NULL || LastSeparator[1] == UNICODE_NULL)
        return STATUS_OBJECT_NAME_INVALID;

    ParentPath->Buffer = FullPath->Buffer;
    ParentPath->MaximumLength = FullPath->MaximumLength;
    if (LastSeparator == FullPath->Buffer)
        ParentPath->Length = sizeof(WCHAR);
    else
        ParentPath->Length = (USHORT)((LastSeparator - FullPath->Buffer) * sizeof(WCHAR));

    LeafName->Buffer = LastSeparator + 1;
    LeafName->Length = (USHORT)(FullPath->Length - ((PUCHAR)LeafName->Buffer - (PUCHAR)FullPath->Buffer));
    LeafName->MaximumLength = LeafName->Length;

    if (NtfsIsDotName(LeafName))
        return STATUS_OBJECT_NAME_INVALID;

    return STATUS_SUCCESS;
}

/*
 * Shared delete-disposition logic for both FileDispositionInformation and
 * FileDispositionInformationEx.  Setting DeleteFile clears any pending delete;
 * otherwise the file is deleted now (if this is the last handle) or marked
 * FCB_DELETE_PENDING for deletion at cleanup time.
 *
 * Like fastfat (and Windows), a still-mapped executable image cannot be
 * deleted out from under the running process: MmFlushImageSection with
 * MmFlushForDelete returns FALSE while any view of the image section is
 * mapped, in which case STATUS_CANNOT_DELETE is returned.  Callers such as
 * cygwin's unlink_nt rely on that status to fall back to rename-then-delete.
 */
static
NTSTATUS
NtfsSetDispositionInformation(PDEVICE_EXTENSION DeviceExt,
                              PNTFS_FCB Fcb,
                              BOOLEAN DeleteFile,
                              BOOLEAN CaseSensitive)
{
    NTSTATUS Status;

    if (!DeleteFile)
    {
        ClearFlag(Fcb->Flags, FCB_DELETE_PENDING);
        return STATUS_SUCCESS;
    }

    if (Fcb->SectionObjectPointers != NULL &&
        !MmFlushImageSection(Fcb->SectionObjectPointers, MmFlushForDelete))
    {
        return STATUS_CANNOT_DELETE;
    }

    if (NtfsFCBIsDirectory(Fcb))
    {
        BOOLEAN Empty = FALSE;

        /* The volume root can never be deleted. */
        if (NtfsFCBIsRoot(Fcb))
            return STATUS_CANNOT_DELETE;

        /* Windows reports a non-empty directory immediately when the delete
         * disposition is set, rather than deferring the error to close time.
         * NtfsDeleteFileRecord re-checks at the choke point in case the
         * directory is repopulated while a handle is still open. */
        Status = NtfsIsDirectoryEmpty(DeviceExt, Fcb->MFTIndex, CaseSensitive, &Empty);
        if (!NT_SUCCESS(Status))
            return Status;
        if (!Empty)
            return STATUS_DIRECTORY_NOT_EMPTY;
    }

    /* Like Windows/fastfat, setting the delete disposition never removes the
     * file here - it only records the intent.  The on-disk record is freed by
     * NtfsCleanupFile once the last handle is cleaned up (OpenHandleCount drops
     * to 0).  Deleting the record immediately while this (or any) handle is
     * still open frees the MFT entry out from under a live FCB: the FCB lingers
     * in the table with LinkCount == 0, and a subsequent open of the same name
     * (e.g. an installer re-extracting the file it just unlinked) reuses that
     * zombie FCB instead of creating a fresh record, after which the redundant
     * delete fails with STATUS_OBJECT_NAME_NOT_FOUND and the unlink is reported
     * as failed. */
    SetFlag(Fcb->Flags, FCB_DELETE_PENDING);

    return STATUS_SUCCESS;
}

/**
* @name NtfsSetInformation
* @implemented
*
* Sets the specified file information.
*
* @param IrpContext
* Points to an NTFS_IRP_CONTEXT which describes the set operation
*
* @return
* STATUS_SUCCESS if successful,
* STATUS_NOT_IMPLEMENTED if trying to set an unimplemented information class,
* STATUS_USER_MAPPED_FILE if trying to truncate a file but MmCanFileBeTruncated() returned false,
* STATUS_OBJECT_NAME_NOT_FOUND if there was no $DATA attribute associated with the target file,
* STATUS_INVALID_PARAMETER if there was no $FILENAME attribute associated with the target file,
* STATUS_INSUFFICIENT_RESOURCES if an allocation failed,
* STATUS_ACCESS_DENIED if target file is a volume or if paging is involved.
*
* @remarks Called by NtfsDispatch() in response to an IRP_MJ_SET_INFORMATION request.
* Only the FileEndOfFileInformation InformationClass is fully implemented. FileAllocationInformation
* is a hack and not a true implementation, but it's enough to make SetEndOfFile() work.
* All other information classes are TODO.
*
*/
NTSTATUS
NtfsSetInformation(PNTFS_IRP_CONTEXT IrpContext)
{
    FILE_INFORMATION_CLASS FileInformationClass;
    PIO_STACK_LOCATION Stack;
    PDEVICE_EXTENSION DeviceExt;
    PFILE_OBJECT FileObject;
    PNTFS_FCB Fcb;
    PVOID SystemBuffer;
    ULONG BufferLength;
    PIRP Irp;
    PDEVICE_OBJECT DeviceObject;
    NTSTATUS Status = STATUS_NOT_IMPLEMENTED;
    PFILE_RECORD_HEADER ParentFileRecord = NULL;
    ULONGLONG ParentMftIndex = NTFS_FILE_ROOT;
    UNICODE_STRING RenamePath;
    UNICODE_STRING RenameParentPath;
    UNICODE_STRING RenameLeafName;

    DPRINT("NtfsSetInformation(%p)\n", IrpContext);

    Irp = IrpContext->Irp;
    Stack = IrpContext->Stack;
    DeviceObject = IrpContext->DeviceObject;
    DeviceExt = DeviceObject->DeviceExtension;
    FileInformationClass = Stack->Parameters.SetFile.FileInformationClass;
    FileObject = IrpContext->FileObject;
    Fcb = FileObject->FsContext;

    DPRINT("NtfsSetInformation: class=%d FCB=%p\n", (int)FileInformationClass, Fcb);
    DPRINT("INSTRUMENT: NtfsSetInformation acquiring MainResource exclusive...\n");

    SystemBuffer = Irp->AssociatedIrp.SystemBuffer;
    BufferLength = Stack->Parameters.SetFile.Length;
    RtlInitEmptyUnicodeString(&RenamePath, NULL, 0);
    RtlInitEmptyUnicodeString(&RenameParentPath, NULL, 0);
    RtlInitEmptyUnicodeString(&RenameLeafName, NULL, 0);

    if (!ExAcquireResourceExclusiveLite(&Fcb->MainResource,
                                     BooleanFlagOn(IrpContext->Flags, IRPCONTEXT_CANWAIT)))
    {
        return NtfsMarkIrpContextForQueue(IrpContext);
    }

    switch (FileInformationClass)
    {
        PFILE_END_OF_FILE_INFORMATION EndOfFileInfo;
        PFILE_ALLOCATION_INFORMATION AllocationInfo;
        PFILE_DISPOSITION_INFORMATION DispositionInfo;
        PFILE_LINK_INFORMATION LinkInfo;
        PFILE_RENAME_INFORMATION RenameInfo;
        PFILE_POSITION_INFORMATION PositionInfo;

        case FileBasicInformation:
            Status = NtfsSetBasicInformation(Fcb,
                                             DeviceExt,
                                             (PFILE_BASIC_INFORMATION)SystemBuffer);
            break;

        case FilePositionInformation:
            if (BufferLength < sizeof(FILE_POSITION_INFORMATION))
            {
                Status = STATUS_INFO_LENGTH_MISMATCH;
                break;
            }
            PositionInfo = (PFILE_POSITION_INFORMATION)SystemBuffer;
            FileObject->CurrentByteOffset = PositionInfo->CurrentByteOffset;
            Status = STATUS_SUCCESS;
            break;

        case FileAllocationInformation:
            if (BufferLength < sizeof(FILE_ALLOCATION_INFORMATION))
            {
                Status = STATUS_INFO_LENGTH_MISMATCH;
                break;
            }
            AllocationInfo = (PFILE_ALLOCATION_INFORMATION)SystemBuffer;
            Status = NtfsSetAllocationInformation(Fcb,
                                                  FileObject,
                                                  DeviceExt,
                                                  Irp->Flags,
                                                  BooleanFlagOn(Stack->Flags, SL_CASE_SENSITIVE),
                                                  &AllocationInfo->AllocationSize);
            break;

        case FileEndOfFileInformation:
            if (BufferLength < sizeof(FILE_END_OF_FILE_INFORMATION))
            {
                Status = STATUS_INFO_LENGTH_MISMATCH;
                break;
            }
            EndOfFileInfo = (PFILE_END_OF_FILE_INFORMATION)SystemBuffer;
            Status = NtfsSetEndOfFile(Fcb,
                                      FileObject,
                                      DeviceExt,
                                      Irp->Flags,
                                      BooleanFlagOn(Stack->Flags, SL_CASE_SENSITIVE),
                                      &EndOfFileInfo->EndOfFile);
            NTFS_TRACE_IF(Fcb->MFTIndex == 160, "REGSTALL: setinfo eof returned 0x%lx\n", Status);
            break;

        case FileDispositionInformation:
            if (BufferLength < sizeof(FILE_DISPOSITION_INFORMATION))
            {
                Status = STATUS_INFO_LENGTH_MISMATCH;
                break;
            }

            DispositionInfo = (PFILE_DISPOSITION_INFORMATION)SystemBuffer;
            Status = NtfsSetDispositionInformation(DeviceExt,
                                                   Fcb,
                                                   DispositionInfo->DeleteFile,
                                                   BooleanFlagOn(Stack->Flags, SL_CASE_SENSITIVE));
            break;

#if (NTDDI_VERSION >= NTDDI_WIN10)
        case FileDispositionInformationEx:
        {
            PFILE_DISPOSITION_INFORMATION_EX DispositionInfoEx;

            if (BufferLength < sizeof(FILE_DISPOSITION_INFORMATION_EX))
            {
                Status = STATUS_INFO_LENGTH_MISMATCH;
                break;
            }

            /* Win10 RS1 POSIX/extended delete-disposition.  The interesting
             * flag for delete semantics is FILE_DISPOSITION_DELETE; the POSIX
             * and image-section-check refinements collapse to the same
             * "delete now or at last close, but never out from under a mapped
             * image" behaviour the shared helper implements. */
            DispositionInfoEx = (PFILE_DISPOSITION_INFORMATION_EX)SystemBuffer;
            Status = NtfsSetDispositionInformation(DeviceExt,
                                                   Fcb,
                                                   BooleanFlagOn(DispositionInfoEx->Flags, FILE_DISPOSITION_DELETE),
                                                   BooleanFlagOn(Stack->Flags, SL_CASE_SENSITIVE));
            break;
        }
#endif

        case FileLinkInformation:
        {
            PNTFS_FCB TargetFcb = NULL;

            if (BufferLength < FIELD_OFFSET(FILE_LINK_INFORMATION, FileName))
            {
                Status = STATUS_INFO_LENGTH_MISMATCH;
                break;
            }

            LinkInfo = (PFILE_LINK_INFORMATION)SystemBuffer;
            if (BufferLength < FIELD_OFFSET(FILE_LINK_INFORMATION, FileName) + LinkInfo->FileNameLength)
            {
                Status = STATUS_INFO_LENGTH_MISMATCH;
                break;
            }

            if (Fcb->Stream[0] != UNICODE_NULL)
            {
                Status = STATUS_INVALID_PARAMETER;
                break;
            }

            /*
             * FILE_LINK_INFORMATION and FILE_RENAME_INFORMATION have the same
             * leading layout used here: replace flag, root directory,
             * filename length, and filename buffer.
             */
            Status = NtfsBuildRenamePath(Fcb,
                                         (PFILE_RENAME_INFORMATION)LinkInfo,
                                         Stack->Parameters.SetFile.FileObject,
                                         &RenamePath,
                                         &RenameParentPath,
                                         &RenameLeafName);
            if (!NT_SUCCESS(Status))
                break;

            if (Stack->Parameters.SetFile.FileObject != NULL &&
                Stack->Parameters.SetFile.FileObject->FsContext != NULL)
            {
                TargetFcb = Stack->Parameters.SetFile.FileObject->FsContext;
                if (TargetFcb->Vcb == Fcb->Vcb)
                {
                    ParentMftIndex = TargetFcb->MFTIndex;
                }
            }

            if (ParentMftIndex == NTFS_FILE_ROOT &&
                (RenameParentPath.Length != sizeof(WCHAR) ||
                 RenameParentPath.Buffer[0] != L'\\'))
            {
                Status = NtfsLookupFile(DeviceExt,
                                        &RenameParentPath,
                                        BooleanFlagOn(Stack->Flags, SL_CASE_SENSITIVE),
                                        &ParentFileRecord,
                                        &ParentMftIndex);
                if (!NT_SUCCESS(Status))
                    break;

                if (!(ParentFileRecord->Flags & FRH_DIRECTORY))
                {
                    Status = STATUS_NOT_A_DIRECTORY;
                    break;
                }
            }

            Status = NtfsLinkFileRecord(DeviceExt,
                                        Fcb,
                                        ParentMftIndex,
                                        &RenameLeafName,
                                        LinkInfo->ReplaceIfExists,
                                        BooleanFlagOn(Stack->Flags, SL_CASE_SENSITIVE));
            break;
        }

        case FileRenameInformation:
        {
            PNTFS_FCB TargetFcb = NULL;

            if (BufferLength < FIELD_OFFSET(FILE_RENAME_INFORMATION, FileName))
            {
                Status = STATUS_INFO_LENGTH_MISMATCH;
                break;
            }

            RenameInfo = (PFILE_RENAME_INFORMATION)SystemBuffer;
            if (BufferLength < FIELD_OFFSET(FILE_RENAME_INFORMATION, FileName) + RenameInfo->FileNameLength)
            {
                Status = STATUS_INFO_LENGTH_MISMATCH;
                break;
            }

            if (Fcb->Stream[0] != UNICODE_NULL)
            {
                Status = STATUS_INVALID_PARAMETER;
                break;
            }

            Status = NtfsBuildRenamePath(Fcb,
                                         RenameInfo,
                                         Stack->Parameters.SetFile.FileObject,
                                         &RenamePath,
                                         &RenameParentPath,
                                         &RenameLeafName);
            if (!NT_SUCCESS(Status))
                break;

            if (Stack->Parameters.SetFile.FileObject != NULL &&
                Stack->Parameters.SetFile.FileObject->FsContext != NULL)
            {
                TargetFcb = Stack->Parameters.SetFile.FileObject->FsContext;
                if (TargetFcb->Vcb == Fcb->Vcb)
                {
                    ParentMftIndex = TargetFcb->MFTIndex;
                }
            }

            if (ParentMftIndex == NTFS_FILE_ROOT &&
                (RenameParentPath.Length != sizeof(WCHAR) ||
                 RenameParentPath.Buffer[0] != L'\\'))
            {
                Status = NtfsLookupFile(DeviceExt,
                                        &RenameParentPath,
                                        BooleanFlagOn(Stack->Flags, SL_CASE_SENSITIVE),
                                        &ParentFileRecord,
                                        &ParentMftIndex);
                if (!NT_SUCCESS(Status))
                    break;

                if (!(ParentFileRecord->Flags & FRH_DIRECTORY))
                {
                    Status = STATUS_NOT_A_DIRECTORY;
                    break;
                }
            }

            Status = NtfsRenameFileRecord(DeviceExt,
                                          Fcb,
                                          ParentMftIndex,
                                          &RenameLeafName,
                                          RenameInfo->ReplaceIfExists,
                                          BooleanFlagOn(Stack->Flags, SL_CASE_SENSITIVE));
            if (NT_SUCCESS(Status))
                NtfsUpdateFcbPathName(Fcb, &RenamePath);
            break;
        }

        // TODO: all other information classes

        default:
            DPRINT1("FIXME: Unimplemented information class: %s\n", GetInfoClassName(FileInformationClass));
            Status = STATUS_NOT_IMPLEMENTED;
    }

    if (ParentFileRecord != NULL)
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, ParentFileRecord);
    if (RenamePath.Buffer != NULL)
        ExFreePoolWithTag(RenamePath.Buffer, TAG_NTFS);

    NTFS_TRACE_IF(Fcb->MFTIndex == 160, "REGSTALL: setinfo release main begin\n");
    ExReleaseResourceLite(&Fcb->MainResource);
    NTFS_TRACE_IF(Fcb->MFTIndex == 160, "REGSTALL: setinfo release main done\n");

    if (NT_SUCCESS(Status))
        Irp->IoStatus.Information =
        Stack->Parameters.SetFile.Length - BufferLength;
    else
        Irp->IoStatus.Information = 0;

    DPRINT("NtfsSetInformation: returning 0x%lx\n", Status);
    NTFS_TRACE_IF(Fcb->MFTIndex == 160, "REGSTALL: setinfo returning 0x%lx info=%Iu\n",
                Status,
                Irp->IoStatus.Information);
    return Status;
}
/* EOF */
