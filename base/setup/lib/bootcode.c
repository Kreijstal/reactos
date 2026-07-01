/*
 * PROJECT:     ReactOS Setup Library
 * LICENSE:     GPL-2.0+ (https://spdx.org/licenses/GPL-2.0+)
 * PURPOSE:     BootCode support functions.
 * COPYRIGHT:   Copyright 2020 Hermes Belusca-Maito
 */

/* INCLUDES *****************************************************************/

#include "precomp.h"

#include "bootcode.h"

#define NDEBUG
#include <debug.h>


/* FUNCTIONS ****************************************************************/

NTSTATUS
ReadBootCodeByHandle(
    IN OUT PBOOTCODE BootCodeInfo,
    IN HANDLE FileHandle,
    IN ULONG Length OPTIONAL)
{
    NTSTATUS Status;
    PVOID BootCode;
    IO_STATUS_BLOCK IoStatusBlock;
    LARGE_INTEGER FileOffset;

    ASSERT(BootCodeInfo);

    /* Normalize the bootcode length */
    if (Length == 0 || Length == (ULONG)-1)
        Length = SECTORSIZE;

    /* Allocate a buffer for the bootcode */
    BootCode = RtlAllocateHeap(ProcessHeap, HEAP_ZERO_MEMORY, Length);
    if (BootCode == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    /* Read the bootcode from the file into the buffer */
    FileOffset.QuadPart = 0ULL;
    Status = NtReadFile(FileHandle,
                        NULL,
                        NULL,
                        NULL,
                        &IoStatusBlock,
                        BootCode,
                        Length,
                        &FileOffset,
                        NULL);
    /*
     * The source bootcode file can be shorter than the requested Length: e.g.
     * ntfs.bin spans only ~2 sectors, while NTFS_BOOTSECTOR_SIZE covers the
     * whole 16-sector $Boot region.  Reading past the end of such a file yields
     * STATUS_END_OF_FILE, which is not a failure as long as some bytes were
     * actually read -- the buffer was zero-initialized, so the unread tail is
     * simply zeros.  Previously this caused InstallNtfsBootCode() to bail out
     * before writing the VBR, leaving a bootstrap-less boot sector that hangs
     * the machine (the failure depended on the on-medium layout past the file,
     * which is why it was intermittent).  Only a read that produced no data at
     * all is a real error.
     */
    if (!NT_SUCCESS(Status) && Status != STATUS_END_OF_FILE)
    {
        RtlFreeHeap(ProcessHeap, 0, BootCode);
        return Status;
    }
    if (IoStatusBlock.Information == 0)
    {
        RtlFreeHeap(ProcessHeap, 0, BootCode);
        return STATUS_END_OF_FILE;
    }

    /* Update the bootcode information */
    if (BootCodeInfo->BootCode)
        RtlFreeHeap(ProcessHeap, 0, BootCodeInfo->BootCode);
    BootCodeInfo->BootCode = BootCode;
    /**/ BootCodeInfo->Length = Length; /**/

    return STATUS_SUCCESS;
}

NTSTATUS
ReadBootCodeFromFile(
    IN OUT PBOOTCODE BootCodeInfo,
    IN PUNICODE_STRING FilePath,
    IN ULONG Length OPTIONAL)
{
    NTSTATUS Status;
    OBJECT_ATTRIBUTES ObjectAttributes;
    IO_STATUS_BLOCK IoStatusBlock;
    HANDLE FileHandle;

    ASSERT(BootCodeInfo);

    /* Open the file */
    InitializeObjectAttributes(&ObjectAttributes,
                               FilePath,
                               OBJ_CASE_INSENSITIVE,
                               NULL,
                               NULL);
    Status = NtOpenFile(&FileHandle,
                        GENERIC_READ | SYNCHRONIZE,
                        &ObjectAttributes,
                        &IoStatusBlock,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, // Is FILE_SHARE_WRITE necessary?
                        FILE_SYNCHRONOUS_IO_NONALERT);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = ReadBootCodeByHandle(BootCodeInfo, FileHandle, Length);

    /* Close the file and return */
    NtClose(FileHandle);
    return Status;
}

VOID
FreeBootCode(
    IN OUT PBOOTCODE BootCodeInfo)
{
    ASSERT(BootCodeInfo);

    /* Update the bootcode information */
    if (BootCodeInfo->BootCode)
        RtlFreeHeap(ProcessHeap, 0, BootCodeInfo->BootCode);
    BootCodeInfo->BootCode = NULL;
    /**/ BootCodeInfo->Length = 0; /**/
}

/* EOF */
