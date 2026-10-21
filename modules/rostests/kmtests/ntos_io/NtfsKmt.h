/*
 * PROJECT:         ReactOS kernel-mode tests
 * LICENSE:         LGPLv2+ - See COPYING.LIB in the top level directory
 * PURPOSE:         Kernel-Mode Test Suite - shared helpers for the black-box
 *                  NTFS feature tests (NT API only, no driver internals)
 * PROGRAMMER:      ReactOS Team
 *
 * Every Ntfs* kmtest drives an NTFS volume through the documented NT file
 * system interface only (Zw* calls, FSCTLs, information classes), so the same
 * test binary can judge any NTFS implementation. Expected values are Windows
 * behaviour (MS-FSA / MS-FSCC / FSCTL reference / Wine and ReactOS winetests),
 * never what a particular driver happens to do.
 */

#pragma once

#define NTFS_KMT_TAG            'tKtN'

#if (NTDDI_VERSION < NTDDI_VISTA)
/* Windows' NTFS answers this class on every version; the headers only name it for Vista+. */
#define FileHardLinkInformation ((FILE_INFORMATION_CLASS)46)
#endif

/* Longest path the helpers build (characters, including the terminator). */
#define NTFS_KMT_MAX_PATH       2048

/* Find a writable (not read-only) NTFS volume on D:-Z:. RootPath receives
 * "\??\X:\" and DriveLetter (optional) the letter. */
BOOLEAN
NtfsKmtFindVolume(
    _Out_writes_(MAX_PATH) PWCHAR RootPath,
    _Out_opt_ PWCHAR DriveLetter);

/* Open the volume itself ("\??\X:") for FSCTLs that need a volume handle. */
NTSTATUS
NtfsKmtOpenVolume(
    _In_ WCHAR DriveLetter,
    _In_ ACCESS_MASK DesiredAccess,
    _Out_ PHANDLE Handle);

/* Full-control ZwCreateFile wrapper. FILE_SYNCHRONOUS_IO_NONALERT is always
 * added to CreateOptions. Information receives IoStatus.Information. */
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
    _Out_opt_ PULONG_PTR Information);

/* FILE_OPEN with full sharing. */
NTSTATUS
NtfsKmtOpen(
    _Out_ PHANDLE Handle,
    _In_ PCWSTR Path,
    _In_ ACCESS_MASK DesiredAccess,
    _In_ ULONG CreateOptions);

/* Create a new file (FILE_CREATE) holding Length bytes of Data, then close. */
NTSTATUS
NtfsKmtCreateFileWithData(
    _In_ PCWSTR Path,
    _In_reads_bytes_opt_(Length) const VOID *Data,
    _In_ ULONG Length);

/* Create a new directory (FILE_CREATE), then close. */
NTSTATUS
NtfsKmtCreateDirectory(
    _In_ PCWSTR Path);

/* Delete one name (file, empty directory or reparse point itself) through
 * FileDispositionInformation. Clears FILE_ATTRIBUTE_READONLY first. */
NTSTATUS
NtfsKmtDelete(
    _In_ PCWSTR Path);

/* Recursively delete Path and everything under it. Never follows reparse
 * points. Missing Path is not an error. */
NTSTATUS
NtfsKmtDeleteTree(
    _In_ PCWSTR Path);

/* Build Root + Name into DirPath, wipe any leftover of a previous (crashed)
 * run and create it as an empty directory. */
NTSTATUS
NtfsKmtPrepareDirectory(
    _In_ PCWSTR Root,
    _In_ PCWSTR Name,
    _Out_writes_(MAX_PATH) PWCHAR DirPath);

NTSTATUS
NtfsKmtWrite(
    _In_ HANDLE Handle,
    _In_ ULONGLONG Offset,
    _In_reads_bytes_(Length) const VOID *Buffer,
    _In_ ULONG Length,
    _Out_opt_ PULONG_PTR Written);

NTSTATUS
NtfsKmtRead(
    _In_ HANDLE Handle,
    _In_ ULONGLONG Offset,
    _Out_writes_bytes_(Length) PVOID Buffer,
    _In_ ULONG Length,
    _Out_opt_ PULONG_PTR Read);

NTSTATUS
NtfsKmtFsctl(
    _In_ HANDLE Handle,
    _In_ ULONG FsControlCode,
    _In_reads_bytes_opt_(InputLength) PVOID Input,
    _In_ ULONG InputLength,
    _Out_writes_bytes_opt_(OutputLength) PVOID Output,
    _In_ ULONG OutputLength,
    _Out_opt_ PULONG_PTR Information);

NTSTATUS
NtfsKmtQueryInfo(
    _In_ HANDLE Handle,
    _Out_writes_bytes_(Length) PVOID Buffer,
    _In_ ULONG Length,
    _In_ FILE_INFORMATION_CLASS InformationClass,
    _Out_opt_ PULONG_PTR Information);

NTSTATUS
NtfsKmtSetInfo(
    _In_ HANDLE Handle,
    _In_reads_bytes_(Length) PVOID Buffer,
    _In_ ULONG Length,
    _In_ FILE_INFORMATION_CLASS InformationClass);

/* FileLinkInformation / FileRenameInformation with an absolute target path. */
NTSTATUS
NtfsKmtLink(
    _In_ HANDLE Handle,
    _In_ PCWSTR NewPath,
    _In_ BOOLEAN ReplaceIfExists);

NTSTATUS
NtfsKmtRename(
    _In_ HANDLE Handle,
    _In_ PCWSTR NewPath,
    _In_ BOOLEAN ReplaceIfExists);

/* IRP_MJ_FLUSH_BUFFERS for the handle's file (ntoskrnl exports no
 * ZwFlushBuffersFile, so the IRP is built here). */
NTSTATUS
NtfsKmtFlush(
    _In_ HANDLE Handle);

/* FileStandardInformation / FileInternalInformation shortcuts. */
NTSTATUS
NtfsKmtQueryStandard(
    _In_ HANDLE Handle,
    _Out_ PFILE_STANDARD_INFORMATION Standard);

ULONGLONG
NtfsKmtQueryFileId(
    _In_ HANDLE Handle);

/* Statuses a driver returns for a request it does not implement at all.
 * Used ONLY on the first call that probes a feature, to turn a missing
 * feature into skip() instead of a cascade of failures. */
BOOLEAN
NtfsKmtIsUnimplemented(
    _In_ NTSTATUS Status);

/* Deterministic, offset-dependent byte pattern (so a misplaced block is
 * caught, not just a wrong byte value). */
VOID
NtfsKmtFillPattern(
    _Out_writes_bytes_(Length) PUCHAR Buffer,
    _In_ SIZE_T Length,
    _In_ ULONG Seed,
    _In_ ULONGLONG BaseOffset);

/* Returns the index of the first mismatching byte, or (SIZE_T)-1. */
SIZE_T
NtfsKmtCheckPattern(
    _In_reads_bytes_(Length) const UCHAR *Buffer,
    _In_ SIZE_T Length,
    _In_ ULONG Seed,
    _In_ ULONGLONG BaseOffset);

/* Returns TRUE when Length bytes at Buffer are all zero. */
BOOLEAN
NtfsKmtIsZero(
    _In_reads_bytes_(Length) const UCHAR *Buffer,
    _In_ SIZE_T Length);

PVOID
NtfsKmtAlloc(
    _In_ SIZE_T Size);

VOID
NtfsKmtFree(
    _In_opt_ PVOID Buffer);

/* Wrap a counted WCHAR buffer for "%wZ" (kmtest's printf has no "%.*ls"). */
FORCEINLINE
PUNICODE_STRING
NtfsKmtCounted(
    _Out_ PUNICODE_STRING String,
    _In_reads_bytes_(Bytes) const WCHAR *Buffer,
    _In_ ULONG Bytes)
{
    String->Buffer = (PWSTR)Buffer;
    String->Length = String->MaximumLength = (USHORT)Bytes;
    return String;
}

/* Path helper: Out = Dir + "\" + Name. */
VOID
NtfsKmtJoin(
    _Out_writes_(OutChars) PWCHAR Out,
    _In_ SIZE_T OutChars,
    _In_ PCWSTR Dir,
    _In_ PCWSTR Name);
