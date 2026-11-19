/*
 * PROJECT:     ReactOS Win32 Base API
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     SetFileInformationByHandle
 * COPYRIGHT:   Based on Wine kernelbase/file.c (LGPL-2.1-or-later)
 */

#include "k32_vista.h"

#define NDEBUG
#include <debug.h>

/*
 * A FILE_RENAME_INFO from Win32 carries a DOS path. When it is a full or
 * drive path that is not relative to RootDirectory, give the I/O manager
 * the NT path; plain names (rename in place), names relative to
 * RootDirectory and NT paths are passed unchanged.
 */
static
NTSTATUS
SetFileRenameInformation(
    _In_ HANDLE FileHandle,
    _In_ PFILE_RENAME_INFO RenameInfo,
    _In_ DWORD Size,
    _In_ FILE_INFORMATION_CLASS InformationClass)
{
    IO_STATUS_BLOCK IoStatusBlock;
    PFILE_RENAME_INFORMATION NtRenameInfo;
    UNICODE_STRING NtName;
    PWSTR DosName;
    ULONG NtSize;
    RTL_PATH_TYPE PathType;
    NTSTATUS Status;

    /* Inconsistent sizes are for the I/O manager to reject */
    if (Size < sizeof(FILE_RENAME_INFO) ||
        RenameInfo->RootDirectory != NULL ||
        RenameInfo->FileNameLength == 0 ||
        (RenameInfo->FileNameLength & 1) ||
        RenameInfo->FileNameLength > Size - FIELD_OFFSET(FILE_RENAME_INFO, FileName) ||
        RenameInfo->FileNameLength > UNICODE_STRING_MAX_BYTES - sizeof(WCHAR))
    {
        return NtSetInformationFile(FileHandle, &IoStatusBlock, RenameInfo, Size, InformationClass);
    }

    /* The name need not be terminated */
    DosName = RtlAllocateHeap(RtlGetProcessHeap(), 0, RenameInfo->FileNameLength + sizeof(WCHAR));
    if (!DosName)
        return STATUS_NO_MEMORY;
    RtlCopyMemory(DosName, RenameInfo->FileName, RenameInfo->FileNameLength);
    DosName[RenameInfo->FileNameLength / sizeof(WCHAR)] = UNICODE_NULL;

    PathType = RtlDetermineDosPathNameType_U(DosName);
    if (PathType == RtlPathTypeRelative ||
        (PathType == RtlPathTypeRooted && RenameInfo->FileNameLength >= 4 * sizeof(WCHAR) &&
         DosName[1] == L'?' && DosName[2] == L'?' && DosName[3] == L'\\'))
    {
        RtlFreeHeap(RtlGetProcessHeap(), 0, DosName);
        return NtSetInformationFile(FileHandle, &IoStatusBlock, RenameInfo, Size, InformationClass);
    }

    if (!RtlDosPathNameToNtPathName_U(DosName, &NtName, NULL, NULL))
    {
        RtlFreeHeap(RtlGetProcessHeap(), 0, DosName);
        return STATUS_OBJECT_NAME_INVALID;
    }
    RtlFreeHeap(RtlGetProcessHeap(), 0, DosName);

    NtSize = FIELD_OFFSET(FILE_RENAME_INFORMATION, FileName) + NtName.Length + sizeof(WCHAR);
    NtRenameInfo = RtlAllocateHeap(RtlGetProcessHeap(), 0, NtSize);
    if (!NtRenameInfo)
    {
        RtlFreeUnicodeString(&NtName);
        return STATUS_NO_MEMORY;
    }

    /* Keeps ReplaceIfExists or, for FileRenameInfoEx, Flags */
    RtlCopyMemory(NtRenameInfo, RenameInfo, FIELD_OFFSET(FILE_RENAME_INFORMATION, FileName));
    NtRenameInfo->FileNameLength = NtName.Length;
    RtlCopyMemory(NtRenameInfo->FileName, NtName.Buffer, NtName.Length);
    NtRenameInfo->FileName[NtName.Length / sizeof(WCHAR)] = UNICODE_NULL;
    RtlFreeUnicodeString(&NtName);

    Status = NtSetInformationFile(FileHandle, &IoStatusBlock, NtRenameInfo, NtSize, InformationClass);
    RtlFreeHeap(RtlGetProcessHeap(), 0, NtRenameInfo);
    return Status;
}

/*
 * @implemented
 */
BOOL
WINAPI
SetFileInformationByHandle(
    _In_ HANDLE hFile,
    _In_ FILE_INFO_BY_HANDLE_CLASS FileInformationClass,
    _In_reads_bytes_(dwBufferSize) LPVOID lpFileInformation,
    _In_ DWORD dwBufferSize)
{
    IO_STATUS_BLOCK IoStatusBlock;
    FILE_INFORMATION_CLASS NtClass;
    NTSTATUS Status;

    switch (FileInformationClass)
    {
        case FileBasicInfo:
            NtClass = FileBasicInformation;
            break;

        case FileRenameInfo:
            NtClass = FileRenameInformation;
            break;

        case FileRenameInfoEx:
            NtClass = FileRenameInformationEx;
            break;

        case FileDispositionInfo:
            NtClass = FileDispositionInformation;
            break;

        case FileDispositionInfoEx:
            NtClass = FileDispositionInformationEx;
            break;

        case FileAllocationInfo:
            NtClass = FileAllocationInformation;
            break;

        case FileEndOfFileInfo:
            NtClass = FileEndOfFileInformation;
            break;

        case FileIoPriorityHintInfo:
            NtClass = FileIoPriorityHintInformation;
            break;

        case FileCaseSensitiveInfo:
            NtClass = FileCaseSensitiveInformation;
            break;

        /* The other classes can only be queried */
        default:
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
    }

    if (NtClass == FileRenameInformation || NtClass == FileRenameInformationEx)
    {
        Status = SetFileRenameInformation(hFile, lpFileInformation, dwBufferSize, NtClass);
    }
    else
    {
        Status = NtSetInformationFile(hFile,
                                      &IoStatusBlock,
                                      lpFileInformation,
                                      dwBufferSize,
                                      NtClass);
    }

    if (!NT_SUCCESS(Status))
    {
        BaseSetLastNTError(Status);
        return FALSE;
    }

    return TRUE;
}
