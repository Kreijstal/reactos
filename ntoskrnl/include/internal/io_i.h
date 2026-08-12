/*
 * PROJECT:         ReactOS Kernel
 * LICENSE:         GPL - See COPYING in the top level directory
 * FILE:            ntoskrnl/include/internal/io_i.h
 * PURPOSE:         Info Classes for the I/O Manager
 * PROGRAMMERS:     Alex Ionescu (alex.ionescu@reactos.org)
 */

//
// File Information Classes
//
// Every table is sized by the compile-time FileMaximumInformation so that
// any class value admitted by the "Class >= FileMaximumInformation" range
// check in NtQuery/SetInformationFile indexes inside the array.  (The old
// positional tables ended below FileMaximumInformation on NT6+ builds, so
// validating a high class read past the end of the array.)
//
// A zero length means the class is rejected with STATUS_INVALID_INFO_CLASS;
// classes the kernel does not implement yet deliberately stay zero.
//
UCHAR IopQueryOperationLength[FileMaximumInformation] =
{
    [FileBasicInformation] = sizeof(FILE_BASIC_INFORMATION),
    [FileStandardInformation] = sizeof(FILE_STANDARD_INFORMATION),
    [FileInternalInformation] = sizeof(FILE_INTERNAL_INFORMATION),
    [FileEaInformation] = sizeof(FILE_EA_INFORMATION),
    [FileAccessInformation] = sizeof(FILE_ACCESS_INFORMATION),
    [FileNameInformation] = sizeof(FILE_NAME_INFORMATION),
    [FilePositionInformation] = sizeof(FILE_POSITION_INFORMATION),
    [FileModeInformation] = sizeof(FILE_MODE_INFORMATION),
    [FileAlignmentInformation] = sizeof(FILE_ALIGNMENT_INFORMATION),
    [FileAllInformation] = sizeof(FILE_ALL_INFORMATION),
    [FileAlternateNameInformation] = sizeof(FILE_NAME_INFORMATION),
    [FileStreamInformation] = sizeof(FILE_STREAM_INFORMATION),
    [FilePipeInformation] = sizeof(FILE_PIPE_INFORMATION),
    [FilePipeLocalInformation] = sizeof(FILE_PIPE_LOCAL_INFORMATION),
    [FilePipeRemoteInformation] = sizeof(FILE_PIPE_REMOTE_INFORMATION),
    [FileMailslotQueryInformation] = sizeof(FILE_MAILSLOT_QUERY_INFORMATION),
    [FileCompressionInformation] = sizeof(FILE_COMPRESSION_INFORMATION),
    [FileObjectIdInformation] = sizeof(FILE_OBJECTID_INFORMATION),
    [FileQuotaInformation] = sizeof(FILE_QUOTA_INFORMATION),
    [FileReparsePointInformation] = sizeof(FILE_REPARSE_POINT_INFORMATION),
    [FileNetworkOpenInformation] = sizeof(FILE_NETWORK_OPEN_INFORMATION),
    [FileAttributeTagInformation] = sizeof(FILE_ATTRIBUTE_TAG_INFORMATION),
#if (NTDDI_VERSION >= NTDDI_VISTA)
    /* Vista+ query classes. */
    [FileHardLinkInformation] = sizeof(FILE_LINKS_INFORMATION),
    [FileIoCompletionNotificationInformation] = sizeof(FILE_IO_COMPLETION_NOTIFICATION_INFORMATION),
#endif
};

UCHAR IopSetOperationLength[FileMaximumInformation] =
{
    [FileBasicInformation] = sizeof(FILE_BASIC_INFORMATION),
    [FileRenameInformation] = sizeof(FILE_RENAME_INFORMATION),
    [FileLinkInformation] = sizeof(FILE_LINK_INFORMATION),
    [FileDispositionInformation] = sizeof(FILE_DISPOSITION_INFORMATION),
    [FilePositionInformation] = sizeof(FILE_POSITION_INFORMATION),
    [FileModeInformation] = sizeof(FILE_MODE_INFORMATION),
    [FileAllocationInformation] = sizeof(FILE_ALLOCATION_INFORMATION),
    [FileEndOfFileInformation] = sizeof(FILE_END_OF_FILE_INFORMATION),
    [FilePipeInformation] = sizeof(FILE_PIPE_INFORMATION),
    [FileMailslotSetInformation] = sizeof(FILE_MAILSLOT_SET_INFORMATION),
    [FileObjectIdInformation] = sizeof(FILE_OBJECTID_INFORMATION),
    [FileCompletionInformation] = sizeof(FILE_COMPLETION_INFORMATION),
    [FileMoveClusterInformation] = sizeof(FILE_MOVE_CLUSTER_INFORMATION),
    [FileQuotaInformation] = sizeof(FILE_QUOTA_INFORMATION),
    [FileTrackingInformation] = sizeof(FILE_TRACKING_INFORMATION),
    [FileValidDataLengthInformation] = sizeof(FILE_VALID_DATA_LENGTH_INFORMATION),
    [FileShortNameInformation] = sizeof(UNICODE_STRING),
#if (NTDDI_VERSION >= NTDDI_VISTA)
    /* Vista+ set classes. */
    [FileIoCompletionNotificationInformation] = sizeof(FILE_IO_COMPLETION_NOTIFICATION_INFORMATION),
    [FileIoStatusBlockRangeInformation] = sizeof(FILE_IOSTATUSBLOCK_RANGE_INFORMATION),
    [FileIoPriorityHintInformation] = sizeof(FILE_IO_PRIORITY_HINT_INFORMATION),
    [FileSfioReserveInformation] = sizeof(FILE_SFIO_RESERVE_INFORMATION),
    [FileSfioVolumeInformation] = sizeof(FILE_SFIO_VOLUME_INFORMATION),
#endif
#if (NTDDI_VERSION >= NTDDI_WIN10)
    /* FileDispositionInformationEx is the Win10 (RS1) POSIX/extended
     * delete-disposition class.  The Win7/Win8/8.1 set classes are not
     * handled and stay zero. */
    [FileDispositionInformationEx] = sizeof(FILE_DISPOSITION_INFORMATION_EX),
#endif
};

ACCESS_MASK IopQueryOperationAccess[FileMaximumInformation] =
{
    [FileBasicInformation] = FILE_READ_ATTRIBUTES,
    [FileFullEaInformation] = FILE_READ_EA,
    [FileAllInformation] = FILE_READ_ATTRIBUTES,
    [FilePipeInformation] = FILE_READ_ATTRIBUTES,
    [FilePipeLocalInformation] = FILE_READ_ATTRIBUTES,
    [FilePipeRemoteInformation] = FILE_READ_ATTRIBUTES,
    [FileNetworkOpenInformation] = FILE_READ_ATTRIBUTES,
    [FileAttributeTagInformation] = FILE_READ_ATTRIBUTES,
    /* FileHardLinkInformation requires no particular access, like on
     * Windows: any handle to the file can enumerate its names. */
};

ACCESS_MASK IopSetOperationAccess[FileMaximumInformation] =
{
    [FileBasicInformation] = FILE_WRITE_ATTRIBUTES,
    [FileRenameInformation] = DELETE,
    [FileDispositionInformation] = DELETE,
    [FileFullEaInformation] = FILE_WRITE_EA,
    [FileAllocationInformation] = FILE_WRITE_DATA,
    [FileEndOfFileInformation] = FILE_WRITE_DATA,
    [FilePipeInformation] = FILE_WRITE_ATTRIBUTES,
    [FilePipeRemoteInformation] = FILE_WRITE_ATTRIBUTES,
    [FileMoveClusterInformation] = FILE_WRITE_DATA,
    [FileTrackingInformation] = FILE_WRITE_DATA,
    [FileValidDataLengthInformation] = FILE_WRITE_DATA,
    [FileShortNameInformation] = DELETE,
#if (NTDDI_VERSION >= NTDDI_WIN10)
    /* FileDispositionInformationEx deletes the file - requires DELETE. */
    [FileDispositionInformationEx] = DELETE,
#endif
};

//
// Volume Information Classes
//
UCHAR IopQueryFsOperationLength[FileFsMaximumInformation] =
{
    [FileFsVolumeInformation] = sizeof(FILE_FS_VOLUME_INFORMATION),
    [FileFsSizeInformation] = sizeof(FILE_FS_SIZE_INFORMATION),
    [FileFsDeviceInformation] = sizeof(FILE_FS_DEVICE_INFORMATION),
    [FileFsAttributeInformation] = sizeof(FILE_FS_ATTRIBUTE_INFORMATION),
    [FileFsControlInformation] = sizeof(FILE_FS_CONTROL_INFORMATION),
    [FileFsFullSizeInformation] = sizeof(FILE_FS_FULL_SIZE_INFORMATION),
    [FileFsObjectIdInformation] = sizeof(FILE_FS_OBJECTID_INFORMATION),
    [FileFsDriverPathInformation] = sizeof(FILE_FS_DRIVER_PATH_INFORMATION),
};

UCHAR IopSetFsOperationLength[FileFsMaximumInformation] =
{
    [FileFsLabelInformation] = sizeof(FILE_FS_LABEL_INFORMATION),
    [FileFsControlInformation] = sizeof(FILE_FS_CONTROL_INFORMATION),
    [FileFsObjectIdInformation] = sizeof(FILE_FS_OBJECTID_INFORMATION),
};

ULONG IopQueryFsOperationAccess[FileFsMaximumInformation] =
{
    [FileFsControlInformation] = FILE_READ_DATA,
};

ULONG IopSetFsOperationAccess[FileFsMaximumInformation] =
{
    [FileFsLabelInformation] = FILE_WRITE_DATA,
    [FileFsControlInformation] = FILE_WRITE_DATA,
    [FileFsObjectIdInformation] = FILE_WRITE_DATA,
};
