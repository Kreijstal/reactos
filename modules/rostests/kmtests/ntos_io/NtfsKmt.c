/*
 * PROJECT:         ReactOS kernel-mode tests
 * LICENSE:         LGPLv2+ - See COPYING.LIB in the top level directory
 * PURPOSE:         Kernel-Mode Test Suite - shared helpers for the black-box
 *                  NTFS feature tests (NT API only, no driver internals)
 * PROGRAMMER:      ReactOS Team
 */

#include <kmt_test.h>
#include "NtfsKmt.h"

PVOID
NtfsKmtAlloc(
    _In_ SIZE_T Size)
{
    PVOID Buffer = ExAllocatePoolWithTag(NonPagedPool, Size, NTFS_KMT_TAG);
    if (Buffer != NULL)
        RtlZeroMemory(Buffer, Size);
    return Buffer;
}

VOID
NtfsKmtFree(
    _In_opt_ PVOID Buffer)
{
    if (Buffer != NULL)
        ExFreePoolWithTag(Buffer, NTFS_KMT_TAG);
}

VOID
NtfsKmtJoin(
    _Out_writes_(OutChars) PWCHAR Out,
    _In_ SIZE_T OutChars,
    _In_ PCWSTR Dir,
    _In_ PCWSTR Name)
{
    SIZE_T DirLen = wcslen(Dir);

    if (DirLen > 0 && Dir[DirLen - 1] == L'\\')
        RtlStringCchPrintfW(Out, OutChars, L"%ls%ls", Dir, Name);
    else
        RtlStringCchPrintfW(Out, OutChars, L"%ls\\%ls", Dir, Name);
}

BOOLEAN
NtfsKmtFindVolume(
    _Out_writes_(MAX_PATH) PWCHAR RootPath,
    _Out_opt_ PWCHAR DriveLetter)
{
    NTSTATUS Status;
    WCHAR Letter;
    WCHAR Path[16];
    UNICODE_STRING Name;
    OBJECT_ATTRIBUTES ObjectAttributes;
    IO_STATUS_BLOCK IoStatus;
    HANDLE Handle;
    UCHAR Buffer[sizeof(FILE_FS_ATTRIBUTE_INFORMATION) + 32 * sizeof(WCHAR)];
    PFILE_FS_ATTRIBUTE_INFORMATION FsAttr = (PFILE_FS_ATTRIBUTE_INFORMATION)Buffer;

    for (Letter = L'D'; Letter <= L'Z'; Letter++)
    {
        RtlStringCchPrintfW(Path, RTL_NUMBER_OF(Path), L"\\??\\%c:\\", Letter);
        RtlInitUnicodeString(&Name, Path);
        InitializeObjectAttributes(&ObjectAttributes, &Name,
                                   OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                                   NULL, NULL);
        Status = ZwOpenFile(&Handle,
                            FILE_LIST_DIRECTORY | SYNCHRONIZE,
                            &ObjectAttributes, &IoStatus,
                            FILE_SHARE_READ | FILE_SHARE_WRITE,
                            FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT);
        if (!NT_SUCCESS(Status))
            continue;

        RtlZeroMemory(Buffer, sizeof(Buffer));
        Status = ZwQueryVolumeInformationFile(Handle, &IoStatus, FsAttr,
                                              sizeof(Buffer),
                                              FileFsAttributeInformation);
        ZwClose(Handle);
        if (!NT_SUCCESS(Status))
            continue;

        if (FsAttr->FileSystemNameLength == 4 * sizeof(WCHAR) &&
            FsAttr->FileSystemName[0] == L'N' &&
            FsAttr->FileSystemName[1] == L'T' &&
            FsAttr->FileSystemName[2] == L'F' &&
            FsAttr->FileSystemName[3] == L'S' &&
            !(FsAttr->FileSystemAttributes & FILE_READ_ONLY_VOLUME))
        {
            RtlStringCchPrintfW(RootPath, MAX_PATH, L"\\??\\%c:\\", Letter);
            if (DriveLetter != NULL)
                *DriveLetter = Letter;
            trace("Found writable NTFS volume at %c:\n", (CHAR)Letter);
            return TRUE;
        }
    }

    return FALSE;
}

NTSTATUS
NtfsKmtOpenVolume(
    _In_ WCHAR DriveLetter,
    _In_ ACCESS_MASK DesiredAccess,
    _Out_ PHANDLE Handle)
{
    WCHAR Path[16];
    UNICODE_STRING Name;
    OBJECT_ATTRIBUTES ObjectAttributes;
    IO_STATUS_BLOCK IoStatus;

    RtlStringCchPrintfW(Path, RTL_NUMBER_OF(Path), L"\\??\\%c:", DriveLetter);
    RtlInitUnicodeString(&Name, Path);
    InitializeObjectAttributes(&ObjectAttributes, &Name,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               NULL, NULL);
    *Handle = NULL;
    return ZwCreateFile(Handle,
                        DesiredAccess | SYNCHRONIZE,
                        &ObjectAttributes, &IoStatus, NULL, 0,
                        FILE_SHARE_READ | FILE_SHARE_WRITE,
                        FILE_OPEN,
                        FILE_SYNCHRONOUS_IO_NONALERT,
                        NULL, 0);
}

NTSTATUS
NtfsKmtCreate(
    _Out_ PHANDLE Handle,
    _In_opt_ HANDLE RootDirectory,
    _In_ PCWSTR Path,
    _In_ ACCESS_MASK DesiredAccess,
    _In_ ULONG FileAttributes,
    _In_ ULONG ShareAccess,
    _In_ ULONG CreateDisposition,
    _In_ ULONG CreateOptions,
    _In_opt_ PVOID EaBuffer,
    _In_ ULONG EaLength,
    _Out_opt_ PULONG_PTR Information)
{
    NTSTATUS Status;
    UNICODE_STRING Name;
    OBJECT_ATTRIBUTES ObjectAttributes;
    IO_STATUS_BLOCK IoStatus;

    RtlInitUnicodeString(&Name, Path);
    InitializeObjectAttributes(&ObjectAttributes, &Name,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               RootDirectory, NULL);
    IoStatus.Status = 0xDEADBEEF;
    IoStatus.Information = (ULONG_PTR)0xDEADBEEF;
    *Handle = NULL;
    Status = ZwCreateFile(Handle,
                          DesiredAccess | SYNCHRONIZE,
                          &ObjectAttributes, &IoStatus, NULL,
                          FileAttributes,
                          ShareAccess,
                          CreateDisposition,
                          CreateOptions | FILE_SYNCHRONOUS_IO_NONALERT,
                          EaBuffer, EaLength);
    if (Information != NULL)
        *Information = NT_SUCCESS(Status) ? IoStatus.Information : 0;
    if (!NT_SUCCESS(Status))
        *Handle = NULL;
    return Status;
}

NTSTATUS
NtfsKmtOpen(
    _Out_ PHANDLE Handle,
    _In_ PCWSTR Path,
    _In_ ACCESS_MASK DesiredAccess,
    _In_ ULONG CreateOptions)
{
    return NtfsKmtCreate(Handle, NULL, Path, DesiredAccess, 0,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         FILE_OPEN, CreateOptions, NULL, 0, NULL);
}

NTSTATUS
NtfsKmtCreateFileWithData(
    _In_ PCWSTR Path,
    _In_reads_bytes_opt_(Length) const VOID *Data,
    _In_ ULONG Length)
{
    NTSTATUS Status;
    HANDLE Handle;

    Status = NtfsKmtCreate(&Handle, NULL, Path,
                           GENERIC_READ | GENERIC_WRITE,
                           FILE_ATTRIBUTE_NORMAL,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           FILE_CREATE, FILE_NON_DIRECTORY_FILE,
                           NULL, 0, NULL);
    if (!NT_SUCCESS(Status))
        return Status;

    if (Data != NULL && Length != 0)
        Status = NtfsKmtWrite(Handle, 0, Data, Length, NULL);

    ZwClose(Handle);
    return Status;
}

NTSTATUS
NtfsKmtCreateDirectory(
    _In_ PCWSTR Path)
{
    NTSTATUS Status;
    HANDLE Handle;

    Status = NtfsKmtCreate(&Handle, NULL, Path,
                           FILE_LIST_DIRECTORY,
                           FILE_ATTRIBUTE_NORMAL,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           FILE_CREATE, FILE_DIRECTORY_FILE,
                           NULL, 0, NULL);
    if (NT_SUCCESS(Status))
        ZwClose(Handle);
    return Status;
}

static
NTSTATUS
NtfsKmtDeleteHandle(
    _In_ HANDLE Handle)
{
    FILE_BASIC_INFORMATION Basic;
    FILE_DISPOSITION_INFORMATION Disposition;
    IO_STATUS_BLOCK IoStatus;
    NTSTATUS Status;

    Status = ZwQueryInformationFile(Handle, &IoStatus, &Basic, sizeof(Basic),
                                    FileBasicInformation);
    if (NT_SUCCESS(Status) && (Basic.FileAttributes & FILE_ATTRIBUTE_READONLY))
    {
        RtlZeroMemory(&Basic, sizeof(Basic));
        Basic.FileAttributes = FILE_ATTRIBUTE_NORMAL;
        ZwSetInformationFile(Handle, &IoStatus, &Basic, sizeof(Basic),
                             FileBasicInformation);
    }

    Disposition.DeleteFile = TRUE;
    return ZwSetInformationFile(Handle, &IoStatus, &Disposition,
                                sizeof(Disposition),
                                FileDispositionInformation);
}

NTSTATUS
NtfsKmtDelete(
    _In_ PCWSTR Path)
{
    NTSTATUS Status;
    HANDLE Handle;

    Status = NtfsKmtCreate(&Handle, NULL, Path,
                           DELETE | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES,
                           0,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           FILE_OPEN, FILE_OPEN_REPARSE_POINT,
                           NULL, 0, NULL);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = NtfsKmtDeleteHandle(Handle);
    ZwClose(Handle);
    return Status;
}

static
NTSTATUS
NtfsKmtDeleteTreeWorker(
    _In_ PCWSTR Path,
    _In_ ULONG Depth)
{
    NTSTATUS Status;
    HANDLE Handle;
    FILE_ATTRIBUTE_TAG_INFORMATION Tag;
    IO_STATUS_BLOCK IoStatus;
    PFILE_DIRECTORY_INFORMATION DirInfo;
    PUCHAR Buffer;
    PWCHAR Child;
    const ULONG BufferSize = 16 * 1024;
    BOOLEAN Restart;
    BOOLEAN DeletedAny;

    Status = NtfsKmtCreate(&Handle, NULL, Path,
                           DELETE | FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES |
                               FILE_WRITE_ATTRIBUTES,
                           0,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           FILE_OPEN, FILE_OPEN_REPARSE_POINT,
                           NULL, 0, NULL);
    if (Status == STATUS_OBJECT_NAME_NOT_FOUND ||
        Status == STATUS_OBJECT_PATH_NOT_FOUND)
        return STATUS_SUCCESS;
    if (!NT_SUCCESS(Status))
        return Status;

    Status = ZwQueryInformationFile(Handle, &IoStatus, &Tag, sizeof(Tag),
                                    FileAttributeTagInformation);
    if (!NT_SUCCESS(Status) ||
        !(Tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
        (Tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
        Depth > 32)
    {
        /* A file, a reparse point (never followed), or too deep. */
        Status = NtfsKmtDeleteHandle(Handle);
        ZwClose(Handle);
        return Status;
    }

    Buffer = NtfsKmtAlloc(BufferSize);
    Child = NtfsKmtAlloc(NTFS_KMT_MAX_PATH * sizeof(WCHAR));
    if (Buffer == NULL || Child == NULL)
    {
        NtfsKmtFree(Buffer);
        NtfsKmtFree(Child);
        ZwClose(Handle);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* Delete in batches, restarting the scan after each batch so removing
     * entries under an open enumeration cannot confuse the cursor. */
    do
    {
        DeletedAny = FALSE;
        Restart = TRUE;
        for (;;)
        {
            Status = ZwQueryDirectoryFile(Handle, NULL, NULL, NULL, &IoStatus,
                                          Buffer, BufferSize,
                                          FileDirectoryInformation,
                                          FALSE, NULL, Restart);
            Restart = FALSE;
            if (!NT_SUCCESS(Status))
                break;

            DirInfo = (PFILE_DIRECTORY_INFORMATION)Buffer;
            for (;;)
            {
                ULONG NameChars = DirInfo->FileNameLength / sizeof(WCHAR);
                BOOLEAN IsDot = (NameChars == 1 && DirInfo->FileName[0] == L'.') ||
                                (NameChars == 2 && DirInfo->FileName[0] == L'.' &&
                                 DirInfo->FileName[1] == L'.');
                if (!IsDot)
                {
                    SIZE_T PathLen = wcslen(Path);
                    if (PathLen + 1 + NameChars + 1 < NTFS_KMT_MAX_PATH)
                    {
                        RtlCopyMemory(Child, Path, PathLen * sizeof(WCHAR));
                        if (PathLen == 0 || Path[PathLen - 1] != L'\\')
                            Child[PathLen++] = L'\\';
                        RtlCopyMemory(Child + PathLen, DirInfo->FileName,
                                      DirInfo->FileNameLength);
                        Child[PathLen + NameChars] = UNICODE_NULL;
                        if (NT_SUCCESS(NtfsKmtDeleteTreeWorker(Child, Depth + 1)))
                            DeletedAny = TRUE;
                    }
                }
                if (DirInfo->NextEntryOffset == 0)
                    break;
                DirInfo = (PFILE_DIRECTORY_INFORMATION)((PUCHAR)DirInfo +
                                                        DirInfo->NextEntryOffset);
            }
        }
    } while (DeletedAny);

    NtfsKmtFree(Buffer);
    NtfsKmtFree(Child);

    Status = NtfsKmtDeleteHandle(Handle);
    ZwClose(Handle);
    return Status;
}

NTSTATUS
NtfsKmtDeleteTree(
    _In_ PCWSTR Path)
{
    return NtfsKmtDeleteTreeWorker(Path, 0);
}

NTSTATUS
NtfsKmtPrepareDirectory(
    _In_ PCWSTR Root,
    _In_ PCWSTR Name,
    _Out_writes_(MAX_PATH) PWCHAR DirPath)
{
    NTSTATUS Status;

    NtfsKmtJoin(DirPath, MAX_PATH, Root, Name);
    Status = NtfsKmtDeleteTree(DirPath);
    if (!NT_SUCCESS(Status))
        trace("Cleaning leftover %ls failed: 0x%lx\n", DirPath, Status);
    return NtfsKmtCreateDirectory(DirPath);
}

NTSTATUS
NtfsKmtWrite(
    _In_ HANDLE Handle,
    _In_ ULONGLONG Offset,
    _In_reads_bytes_(Length) const VOID *Buffer,
    _In_ ULONG Length,
    _Out_opt_ PULONG_PTR Written)
{
    NTSTATUS Status;
    IO_STATUS_BLOCK IoStatus;
    LARGE_INTEGER ByteOffset;

    ByteOffset.QuadPart = (LONGLONG)Offset;
    IoStatus.Information = 0;
    Status = ZwWriteFile(Handle, NULL, NULL, NULL, &IoStatus,
                         (PVOID)Buffer, Length, &ByteOffset, NULL);
    if (Written != NULL)
        *Written = NT_SUCCESS(Status) ? IoStatus.Information : 0;
    return Status;
}

NTSTATUS
NtfsKmtRead(
    _In_ HANDLE Handle,
    _In_ ULONGLONG Offset,
    _Out_writes_bytes_(Length) PVOID Buffer,
    _In_ ULONG Length,
    _Out_opt_ PULONG_PTR Read)
{
    NTSTATUS Status;
    IO_STATUS_BLOCK IoStatus;
    LARGE_INTEGER ByteOffset;

    ByteOffset.QuadPart = (LONGLONG)Offset;
    IoStatus.Information = 0;
    Status = ZwReadFile(Handle, NULL, NULL, NULL, &IoStatus,
                        Buffer, Length, &ByteOffset, NULL);
    if (Read != NULL)
        *Read = IoStatus.Information;
    return Status;
}

NTSTATUS
NtfsKmtFsctl(
    _In_ HANDLE Handle,
    _In_ ULONG FsControlCode,
    _In_reads_bytes_opt_(InputLength) PVOID Input,
    _In_ ULONG InputLength,
    _Out_writes_bytes_opt_(OutputLength) PVOID Output,
    _In_ ULONG OutputLength,
    _Out_opt_ PULONG_PTR Information)
{
    NTSTATUS Status;
    IO_STATUS_BLOCK IoStatus;

    IoStatus.Status = 0xDEADBEEF;
    IoStatus.Information = 0;
    Status = ZwFsControlFile(Handle, NULL, NULL, NULL, &IoStatus,
                             FsControlCode, Input, InputLength,
                             Output, OutputLength);
    if (Information != NULL)
        *Information = IoStatus.Information;
    return Status;
}

NTSTATUS
NtfsKmtQueryInfo(
    _In_ HANDLE Handle,
    _Out_writes_bytes_(Length) PVOID Buffer,
    _In_ ULONG Length,
    _In_ FILE_INFORMATION_CLASS InformationClass,
    _Out_opt_ PULONG_PTR Information)
{
    NTSTATUS Status;
    IO_STATUS_BLOCK IoStatus;

    IoStatus.Information = 0;
    Status = ZwQueryInformationFile(Handle, &IoStatus, Buffer, Length,
                                    InformationClass);
    if (Information != NULL)
        *Information = IoStatus.Information;
    return Status;
}

NTSTATUS
NtfsKmtSetInfo(
    _In_ HANDLE Handle,
    _In_reads_bytes_(Length) PVOID Buffer,
    _In_ ULONG Length,
    _In_ FILE_INFORMATION_CLASS InformationClass)
{
    IO_STATUS_BLOCK IoStatus;

    return ZwSetInformationFile(Handle, &IoStatus, Buffer, Length,
                                InformationClass);
}

static
NTSTATUS
NtfsKmtLinkOrRename(
    _In_ HANDLE Handle,
    _In_ PCWSTR NewPath,
    _In_ BOOLEAN ReplaceIfExists,
    _In_ FILE_INFORMATION_CLASS InformationClass)
{
    NTSTATUS Status;
    ULONG NameBytes = (ULONG)(wcslen(NewPath) * sizeof(WCHAR));
    ULONG Size;
    PFILE_LINK_INFORMATION Info;

    /* FILE_LINK_INFORMATION and FILE_RENAME_INFORMATION share this layout. */
    C_ASSERT(FIELD_OFFSET(FILE_LINK_INFORMATION, FileName) ==
             FIELD_OFFSET(FILE_RENAME_INFORMATION, FileName));
    Size = FIELD_OFFSET(FILE_LINK_INFORMATION, FileName) + NameBytes + sizeof(WCHAR);
    Info = NtfsKmtAlloc(Size);
    if (Info == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Info->ReplaceIfExists = ReplaceIfExists;
    Info->RootDirectory = NULL;
    Info->FileNameLength = NameBytes;
    RtlCopyMemory(Info->FileName, NewPath, NameBytes);
    Status = NtfsKmtSetInfo(Handle, Info,
                            FIELD_OFFSET(FILE_LINK_INFORMATION, FileName) + NameBytes,
                            InformationClass);
    NtfsKmtFree(Info);
    return Status;
}

NTSTATUS
NtfsKmtLink(
    _In_ HANDLE Handle,
    _In_ PCWSTR NewPath,
    _In_ BOOLEAN ReplaceIfExists)
{
    return NtfsKmtLinkOrRename(Handle, NewPath, ReplaceIfExists, FileLinkInformation);
}

NTSTATUS
NtfsKmtRename(
    _In_ HANDLE Handle,
    _In_ PCWSTR NewPath,
    _In_ BOOLEAN ReplaceIfExists)
{
    return NtfsKmtLinkOrRename(Handle, NewPath, ReplaceIfExists, FileRenameInformation);
}

NTSTATUS
NtfsKmtFlush(
    _In_ HANDLE Handle)
{
    NTSTATUS Status;
    PFILE_OBJECT FileObject;
    PDEVICE_OBJECT DeviceObject;
    PIRP Irp;
    PIO_STACK_LOCATION Stack;
    KEVENT Event;
    IO_STATUS_BLOCK IoStatus;

    Status = ObReferenceObjectByHandle(Handle, 0, *IoFileObjectType, KernelMode,
                                       (PVOID *)&FileObject, NULL);
    if (!NT_SUCCESS(Status))
        return Status;

    DeviceObject = IoGetRelatedDeviceObject(FileObject);
    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    Irp = IoBuildSynchronousFsdRequest(IRP_MJ_FLUSH_BUFFERS, DeviceObject, NULL, 0,
                                       NULL, &Event, &IoStatus);
    if (Irp == NULL)
    {
        ObDereferenceObject(FileObject);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    Stack = IoGetNextIrpStackLocation(Irp);
    Stack->FileObject = FileObject;

    Status = IoCallDriver(DeviceObject, Irp);
    if (Status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
        Status = IoStatus.Status;
    }
    ObDereferenceObject(FileObject);
    return Status;
}

NTSTATUS
NtfsKmtQueryStandard(
    _In_ HANDLE Handle,
    _Out_ PFILE_STANDARD_INFORMATION Standard)
{
    RtlZeroMemory(Standard, sizeof(*Standard));
    return NtfsKmtQueryInfo(Handle, Standard, sizeof(*Standard),
                            FileStandardInformation, NULL);
}

ULONGLONG
NtfsKmtQueryFileId(
    _In_ HANDLE Handle)
{
    FILE_INTERNAL_INFORMATION Internal;

    Internal.IndexNumber.QuadPart = 0;
    if (!NT_SUCCESS(NtfsKmtQueryInfo(Handle, &Internal, sizeof(Internal),
                                     FileInternalInformation, NULL)))
        return 0;
    return (ULONGLONG)Internal.IndexNumber.QuadPart;
}

BOOLEAN
NtfsKmtIsUnimplemented(
    _In_ NTSTATUS Status)
{
    return Status == STATUS_NOT_IMPLEMENTED ||
           Status == STATUS_NOT_SUPPORTED ||
           Status == STATUS_INVALID_DEVICE_REQUEST ||
           Status == STATUS_INVALID_INFO_CLASS;
}

static
UCHAR
NtfsKmtPatternByte(
    _In_ ULONG Seed,
    _In_ ULONGLONG Offset)
{
    ULONGLONG Mix = Offset * 0x9E3779B1ull + Seed * 0x85EBCA6Bull;
    return (UCHAR)((Mix >> 13) ^ (Mix >> 29) ^ Offset);
}

VOID
NtfsKmtFillPattern(
    _Out_writes_bytes_(Length) PUCHAR Buffer,
    _In_ SIZE_T Length,
    _In_ ULONG Seed,
    _In_ ULONGLONG BaseOffset)
{
    SIZE_T i;

    for (i = 0; i < Length; i++)
        Buffer[i] = NtfsKmtPatternByte(Seed, BaseOffset + i);
}

SIZE_T
NtfsKmtCheckPattern(
    _In_reads_bytes_(Length) const UCHAR *Buffer,
    _In_ SIZE_T Length,
    _In_ ULONG Seed,
    _In_ ULONGLONG BaseOffset)
{
    SIZE_T i;

    for (i = 0; i < Length; i++)
    {
        if (Buffer[i] != NtfsKmtPatternByte(Seed, BaseOffset + i))
            return i;
    }
    return (SIZE_T)-1;
}

BOOLEAN
NtfsKmtIsZero(
    _In_reads_bytes_(Length) const UCHAR *Buffer,
    _In_ SIZE_T Length)
{
    SIZE_T i;

    for (i = 0; i < Length; i++)
    {
        if (Buffer[i] != 0)
            return FALSE;
    }
    return TRUE;
}
