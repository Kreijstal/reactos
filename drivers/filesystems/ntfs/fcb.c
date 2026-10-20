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
 * FILE:             drivers/filesystem/ntfs/fcb.c
 * PURPOSE:          NTFS filesystem driver
 * PROGRAMMERS:      Eric Kohl
 *                   Pierre Schweitzer (pierre@reactos.org)
 *                   Hervé Poussineau (hpoussin@reactos.org)
 */

/* INCLUDES *****************************************************************/

#include "ntfs.h"

#define NDEBUG
#include <debug.h>

/* FUNCTIONS ****************************************************************/

static
PCWSTR
NtfsGetNextPathElement(PCWSTR FileName)
{
    if (*FileName == L'\0')
    {
        return NULL;
    }

    while (*FileName != L'\0' && *FileName != L'\\')
    {
        FileName++;
    }

    return FileName;
}


/* Match _wcsicmp's folding: a name equal in the table must hash equally. */
static
ULONG
NtfsComputePathNameHash(PCWSTR Path)
{
    ULONG Hash = 2166136261u;
    WCHAR Character;

    while ((Character = *Path++) != UNICODE_NULL)
    {
        if (Character >= L'A' && Character <= L'Z')
            Character += L'a' - L'A';
        Hash = (Hash ^ Character) * 16777619u;
    }
    return Hash;
}

NTSTATUS
NtfsInitializeFcbHash(PNTFS_VCB Vcb)
{
    ULONG Index;

    Vcb->FcbHashBuckets = ExAllocatePoolWithTag(NonPagedPool,
                                               NTFS_FCB_HASH_BUCKETS * sizeof(LIST_ENTRY),
                                               TAG_NTFS);
    if (Vcb->FcbHashBuckets == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;
    for (Index = 0; Index < NTFS_FCB_HASH_BUCKETS; Index++)
        InitializeListHead(&Vcb->FcbHashBuckets[Index]);
    return STATUS_SUCCESS;
}

VOID
NtfsUninitializeFcbHash(PNTFS_VCB Vcb)
{
    if (Vcb->FcbHashBuckets != NULL)
    {
        ExFreePoolWithTag(Vcb->FcbHashBuckets, TAG_NTFS);
        Vcb->FcbHashBuckets = NULL;
    }
}

/* FcbListLock protects both indexes. Fresh and unlinked entries are self-linked. */
static
VOID
NtfsRemoveFCBFromHash(PNTFS_FCB Fcb)
{
    /* Deletion already unlinks the entry before the final reference goes. */
    if (!IsListEmpty(&Fcb->FcbHashEntry))
        RemoveEntryList(&Fcb->FcbHashEntry);
    InitializeListHead(&Fcb->FcbHashEntry);
}

static
BOOLEAN
NtfsWSubString(PWCHAR pTarget,
               size_t pTargetCount,
               PCWSTR pSource,
               size_t pLength)
{
    /* pLength comes from the caller's path, not from the target's size */
    if (pLength >= pTargetCount)
        return FALSE;

    wcsncpy(pTarget, pSource, pLength);
    pTarget[pLength] = L'\0';
    return TRUE;
}


/**
* @name NtfsSetFCBPath
*
* Gives Fcb a pool copy of Path, of any length, and frees the one it had.
*
* @return
* STATUS_SUCCESS, or STATUS_INSUFFICIENT_RESOURCES with the old path kept.
*/
NTSTATUS
NtfsSetFCBPath(
    _Inout_ PNTFS_FCB Fcb,
    _In_ PCWSTR Path)
{
    SIZE_T Size = (wcslen(Path) + 1) * sizeof(WCHAR);
    PWCHAR Buffer;

    Buffer = ExAllocatePoolWithTag(NonPagedPool, Size, TAG_NTFS);
    if (Buffer == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    RtlCopyMemory(Buffer, Path, Size);
    NtfsReplaceFCBPath(Fcb, Buffer);

    return STATUS_SUCCESS;
}

/**
* @name NtfsReplaceFCBPath
*
* Makes Path, a NUL-terminated pool buffer, the path of Fcb, and frees the one it had.
*/
VOID
NtfsReplaceFCBPath(
    _Inout_ PNTFS_FCB Fcb,
    _In_ PWCHAR Path)
{
    PWCHAR OldPath = Fcb->PathName;

    /* Rename holds FcbListLock; creation has not published the FCB yet. */
    if (!IsListEmpty(&Fcb->FcbHashEntry))
    {
        NtfsRemoveFCBFromHash(Fcb);
        Fcb->PathNameHash = NtfsComputePathNameHash(Path);
        InsertHeadList(&Fcb->Vcb->FcbHashBuckets[Fcb->PathNameHash & (NTFS_FCB_HASH_BUCKETS - 1)],
                       &Fcb->FcbHashEntry);
    }
    else
    {
        Fcb->PathNameHash = NtfsComputePathNameHash(Path);
    }
    Fcb->PathName = Path;
    RtlInitUnicodeString(&Fcb->NotifyName, Path);
    Fcb->ObjectName = wcsrchr(Path, L'\\');
    if (Fcb->ObjectName == NULL)
        Fcb->ObjectName = Path;

    if (OldPath != NULL)
        ExFreePoolWithTag(OldPath, TAG_NTFS);
}

/**
* @name NtfsBuildChildPath
*
* Returns a pool copy (TAG_NTFS) of the path of Name in the directory at DirectoryPath.
*/
PWCHAR
NtfsBuildChildPath(
    _In_ PCWSTR DirectoryPath,
    _In_ PCWSTR Name)
{
    SIZE_T DirectoryLength = wcslen(DirectoryPath);
    SIZE_T NameLength = wcslen(Name);
    PWCHAR Path;

    /* The root directory already ends with the separator */
    if (DirectoryLength == 1 && DirectoryPath[0] == L'\\')
        DirectoryLength = 0;

    Path = ExAllocatePoolWithTag(NonPagedPool, (DirectoryLength + 1 + NameLength + 1) * sizeof(WCHAR), TAG_NTFS);
    if (Path == NULL)
        return NULL;

    RtlCopyMemory(Path, DirectoryPath, DirectoryLength * sizeof(WCHAR));
    Path[DirectoryLength] = L'\\';
    RtlCopyMemory(Path + DirectoryLength + 1, Name, (NameLength + 1) * sizeof(WCHAR));

    return Path;
}

NTSTATUS
NtfsInitializeFcbLock(PNTFS_FCB Fcb)
{
    PNTFS_VCB Vcb = Fcb->Vcb;
    PNTFS_FILE_LOCK NewLock, FileLock;
    PLIST_ENTRY Entry;
    KIRQL OldIrql;

    ASSERT(Fcb->FileLock == NULL);
    if (NtfsFCBIsDirectory(Fcb))
        return STATUS_SUCCESS;

    NewLock = ExAllocatePoolWithTag(NonPagedPool, sizeof(*NewLock), TAG_NTFS);
    if (NewLock == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(NewLock, sizeof(*NewLock));
    NewLock->Vcb = Vcb;
    NewLock->FileReference = NtfsFileReference(Fcb);
    wcscpy(NewLock->Stream, Fcb->Stream);
    ExInitializeResourceLite(&NewLock->Resource);
    FsRtlInitializeFileLock(&NewLock->Lock, NtfsCompleteLockIrp, NULL);

    KeAcquireSpinLock(&Vcb->FcbListLock, &OldIrql);
    for (Entry = Vcb->FileLockList.Flink; Entry != &Vcb->FileLockList; Entry = Entry->Flink)
    {
        FileLock = CONTAINING_RECORD(Entry, NTFS_FILE_LOCK, ListEntry);
        if (FileLock->FileReference == NewLock->FileReference &&
            _wcsicmp(FileLock->Stream, NewLock->Stream) == 0)
        {
            FileLock->References++;
            FileLock->MultipleFcbs = TRUE;
            Fcb->FileLock = FileLock;
            KeReleaseSpinLock(&Vcb->FcbListLock, OldIrql);
            FsRtlUninitializeFileLock(&NewLock->Lock);
            ExDeleteResourceLite(&NewLock->Resource);
            ExFreePoolWithTag(NewLock, TAG_NTFS);
            return STATUS_SUCCESS;
        }
    }
    NewLock->References = 1;
    InsertTailList(&Vcb->FileLockList, &NewLock->ListEntry);
    Fcb->FileLock = NewLock;
    KeReleaseSpinLock(&Vcb->FcbListLock, OldIrql);
    return STATUS_SUCCESS;
}

VOID
NtfsReferenceFileLock(PNTFS_FILE_LOCK FileLock)
{
    KIRQL OldIrql;

    KeAcquireSpinLock(&FileLock->Vcb->FcbListLock, &OldIrql);
    ASSERT(FileLock->References > 0);
    FileLock->References++;
    KeReleaseSpinLock(&FileLock->Vcb->FcbListLock, OldIrql);
}

VOID
NtfsReleaseFileLock(PNTFS_FILE_LOCK FileLock)
{
    KIRQL OldIrql;
    BOOLEAN Last;

    KeAcquireSpinLock(&FileLock->Vcb->FcbListLock, &OldIrql);
    ASSERT(FileLock->References > 0);
    Last = (--FileLock->References == 0);
    if (Last)
        RemoveEntryList(&FileLock->ListEntry);
    KeReleaseSpinLock(&FileLock->Vcb->FcbListLock, OldIrql);
    if (Last)
    {
        FsRtlUninitializeFileLock(&FileLock->Lock);
        ExDeleteResourceLite(&FileLock->Resource);
        ExFreePoolWithTag(FileLock, TAG_NTFS);
    }
}

PNTFS_FCB
NtfsCreateFCB(PCWSTR FileName,
              PCWSTR Stream,
              PNTFS_VCB Vcb)
{
    PNTFS_FCB Fcb;

    ASSERT(Vcb);
    ASSERT(Vcb->Identifier.Type == NTFS_TYPE_VCB);

    Fcb = ExAllocateFromNPagedLookasideList(&NtfsGlobalData->FcbLookasideList);
    if (Fcb == NULL)
    {
        return NULL;
    }

    RtlZeroMemory(Fcb, sizeof(NTFS_FCB));

    Fcb->Identifier.Type = NTFS_TYPE_FCB;
    Fcb->Identifier.Size = sizeof(NTFS_TYPE_FCB);

    Fcb->Vcb = Vcb;
    InitializeListHead(&Fcb->FcbHashEntry);

    /* An FCB without a name (the volume's stream) still has a path, an empty one */
    if (!NT_SUCCESS(NtfsSetFCBPath(Fcb, FileName ? FileName : L"")))
    {
        ExFreeToNPagedLookasideList(&NtfsGlobalData->FcbLookasideList, Fcb);
        return NULL;
    }

    if (Stream)
    {
        wcscpy(Fcb->Stream, Stream);
    }
    else
    {
        Fcb->Stream[0] = UNICODE_NULL;
    }

    ExInitializeResourceLite(&Fcb->MainResource);
    ExInitializeResourceLite(&Fcb->PagingIoResource);

    Fcb->RFCB.NodeTypeCode = (USHORT)NTFS_TYPE_FCB;
    Fcb->RFCB.NodeByteSize = sizeof(NTFS_FCB);
    Fcb->RFCB.Resource = &(Fcb->MainResource);
    Fcb->RFCB.PagingIoResource = &Fcb->PagingIoResource;
    InitializeListHead(&Fcb->FcbListEntry);

    return Fcb;
}


VOID
NtfsDestroyFCB(PNTFS_FCB Fcb)
{
    ASSERT(Fcb);
    ASSERT(Fcb->Identifier.Type == NTFS_TYPE_FCB);

    MmFlushImageSection(&Fcb->SectionObjectPointers, MmFlushForWrite);
    MmForceSectionClosed(&Fcb->SectionObjectPointers, TRUE);
    ASSERT(Fcb->SectionObjectPointers.DataSectionObject == NULL);
    ASSERT(Fcb->SectionObjectPointers.ImageSectionObject == NULL);
    ASSERT(Fcb->SectionObjectPointers.SharedCacheMap == NULL);

    if (Fcb->FileLock != NULL)
        NtfsReleaseFileLock(Fcb->FileLock);
    ExDeleteResourceLite(&Fcb->PagingIoResource);
    ExDeleteResourceLite(&Fcb->MainResource);

    ExFreePoolWithTag(Fcb->PathName, TAG_NTFS);
    ExFreeToNPagedLookasideList(&NtfsGlobalData->FcbLookasideList, Fcb);
}


BOOLEAN
NtfsFCBIsDirectory(PNTFS_FCB Fcb)
{
    return ((Fcb->Entry.FileAttributes & NTFS_FILE_TYPE_DIRECTORY) == NTFS_FILE_TYPE_DIRECTORY);
}


BOOLEAN
NtfsFCBIsReparsePoint(PNTFS_FCB Fcb)
{
    return ((Fcb->Entry.FileAttributes & NTFS_FILE_TYPE_REPARSE) == NTFS_FILE_TYPE_REPARSE);
}


BOOLEAN
NtfsFCBIsCompressed(PNTFS_FCB Fcb)
{
    return ((Fcb->Entry.FileAttributes & NTFS_FILE_TYPE_COMPRESSED) == NTFS_FILE_TYPE_COMPRESSED);
}

BOOLEAN
NtfsFCBIsEncrypted(PNTFS_FCB Fcb)
{
    return ((Fcb->Entry.FileAttributes & NTFS_FILE_TYPE_ENCRYPTED) == NTFS_FILE_TYPE_ENCRYPTED);
}

BOOLEAN
NtfsFCBIsRoot(PNTFS_FCB Fcb)
{
    return (wcscmp(Fcb->PathName, L"\\") == 0);
}


VOID
NtfsGrabFCB(PNTFS_VCB Vcb,
            PNTFS_FCB Fcb)
{
    KIRQL oldIrql;

    DPRINT("grabbing FCB at %p: %S, refCount:%d\n",
           Fcb,
           Fcb->PathName,
           Fcb->RefCount);

    KeAcquireSpinLock(&Vcb->FcbListLock, &oldIrql);
    Fcb->RefCount++;
    KeReleaseSpinLock(&Vcb->FcbListLock, oldIrql);
}


VOID
NtfsReleaseFCB(PNTFS_VCB Vcb,
               PNTFS_FCB Fcb)
{
    KIRQL OldIrql;
    PFILE_OBJECT CacheFile;

    KeAcquireSpinLock(&Vcb->FcbListLock, &OldIrql);
    ASSERT(Fcb->RefCount > 0);
    Fcb->RefCount--;

    /* Volume objects live for the mount, even with no open handles. */
    if (Fcb->Flags & (FCB_IS_VOLUME | FCB_IS_VOLUME_STREAM))
    {
        KeReleaseSpinLock(&Vcb->FcbListLock, OldIrql);
        return;
    }

    /* The internal stream pins the FCB until MM has dropped its file object.
     * With only that reference left, detach its private cache map. Close may
     * run synchronously inside CcUninitializeCacheMap and free the FCB. */
    if (Fcb->RefCount == 1 && Fcb->FileObject != NULL)
    {
        NTSTATUS Status;

        CacheFile = Fcb->FileObject;
        KeReleaseSpinLock(&Vcb->FcbListLock, OldIrql);
        if (Vcb->Flags & VCB_DEVICE_REMOVED)
        {
            /* Surprise removal makes dirty data unrecoverable. Do not issue
             * writeback to a device that has already left the stack. */
            CcPurgeCacheSection(&Fcb->SectionObjectPointers, NULL, 0, FALSE);
            Status = STATUS_SUCCESS;
        }
        else
        {
            Status = NtfsFlushFileCache(CacheFile, TRUE);
        }
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("Cache teardown failed for %wS: %lx\n", Fcb->PathName, Status);
            return;
        }

        KeAcquireSpinLock(&Vcb->FcbListLock, &OldIrql);
        Fcb->FileObject = NULL;
        Fcb->Flags &= ~FCB_CACHE_INITIALIZED;
        KeReleaseSpinLock(&Vcb->FcbListLock, OldIrql);
        CcUninitializeCacheMap(CacheFile, NULL, NULL);
        return;
    }

    if (Fcb->RefCount == 0 &&
        (!NtfsFCBIsDirectory(Fcb) || (Fcb->Flags & FCB_DELETED)))
    {
        if (!(Fcb->Flags & FCB_DELETED))
            RemoveEntryList(&Fcb->FcbListEntry);
        InitializeListHead(&Fcb->FcbListEntry);
        NtfsRemoveFCBFromHash(Fcb);
        KeReleaseSpinLock(&Vcb->FcbListLock, OldIrql);
        NtfsDestroyFCB(Fcb);
        return;
    }
    KeReleaseSpinLock(&Vcb->FcbListLock, OldIrql);
}


VOID
NtfsAddFCBToTable(PNTFS_VCB Vcb,
                  PNTFS_FCB Fcb)
{
    KIRQL oldIrql;

    KeAcquireSpinLock(&Vcb->FcbListLock, &oldIrql);
    Fcb->Vcb = Vcb;
    ASSERT(Fcb->PathNameHash == NtfsComputePathNameHash(Fcb->PathName));
    InsertTailList(&Vcb->FcbListHead, &Fcb->FcbListEntry);
    /* The list remains authoritative before the optional index exists. */
    if (Vcb->FcbHashBuckets != NULL)
    {
        InsertHeadList(&Vcb->FcbHashBuckets[Fcb->PathNameHash & (NTFS_FCB_HASH_BUCKETS - 1)],
                       &Fcb->FcbHashEntry);
    }
    KeReleaseSpinLock(&Vcb->FcbListLock, oldIrql);
}


/**
* @name NtfsRemoveFCBFromTable
*
* Takes the FCB of a deleted file or directory out of the table, so a later
* open of the same path goes to the disk instead of finding it. The FCB lives
* on until its last reference is released.
*/
VOID
NtfsRemoveFCBFromTable(
    _In_ PNTFS_VCB Vcb,
    _In_ PNTFS_FCB Fcb)
{
    KIRQL OldIrql;

    PLIST_ENTRY Entry;
    SIZE_T PathLength = wcslen(Fcb->PathName);

    KeAcquireSpinLock(&Vcb->FcbListLock, &OldIrql);

    /* FCB_DELETED tells NtfsReleaseFCB() the entry is already unlinked */
    if (!(Fcb->Flags & FCB_DELETED))
    {
        RemoveEntryList(&Fcb->FcbListEntry);
        InitializeListHead(&Fcb->FcbListEntry);
        NtfsRemoveFCBFromHash(Fcb);
        Fcb->Flags |= FCB_DELETED;
    }

    /* The named streams of a deleted file went with it */
    Entry = Vcb->FcbListHead.Flink;
    while (Fcb->Stream[0] == UNICODE_NULL && Entry != &Vcb->FcbListHead)
    {
        PNTFS_FCB Other = CONTAINING_RECORD(Entry, NTFS_FCB, FcbListEntry);

        Entry = Entry->Flink;

        if (Other->MFTIndex == Fcb->MFTIndex &&
            Other->Stream[0] != UNICODE_NULL &&
            _wcsnicmp(Other->PathName, Fcb->PathName, PathLength) == 0 &&
            Other->PathName[PathLength] == L':')
        {
            RemoveEntryList(&Other->FcbListEntry);
            InitializeListHead(&Other->FcbListEntry);
            NtfsRemoveFCBFromHash(Other);
            Other->Flags |= FCB_DELETED;
        }
    }

    KeReleaseSpinLock(&Vcb->FcbListLock, OldIrql);
}


PNTFS_FCB
NtfsGrabFCBFromTable(PNTFS_VCB Vcb,
                     PCWSTR FileName)
{
    KIRQL oldIrql;
    PNTFS_FCB Fcb;
    PLIST_ENTRY current_entry;
    PLIST_ENTRY Head;
    ULONG Hash = FileName ? NtfsComputePathNameHash(FileName) : 0;

    KeAcquireSpinLock(&Vcb->FcbListLock, &oldIrql);

    if (FileName == NULL || *FileName == 0)
    {
        DPRINT("Return FCB for stream file object\n");
        Fcb = Vcb->StreamFileObject->FsContext;
        Fcb->RefCount++;
        KeReleaseSpinLock(&Vcb->FcbListLock, oldIrql);
        return Fcb;
    }

    Head = Vcb->FcbHashBuckets ?
           &Vcb->FcbHashBuckets[Hash & (NTFS_FCB_HASH_BUCKETS - 1)] : &Vcb->FcbListHead;
    current_entry = Head->Flink;
    while (current_entry != Head)
    {
        Fcb = Vcb->FcbHashBuckets ?
              CONTAINING_RECORD(current_entry, NTFS_FCB, FcbHashEntry) :
              CONTAINING_RECORD(current_entry, NTFS_FCB, FcbListEntry);

        DPRINT("Comparing '%S' and '%S'\n", FileName, Fcb->PathName);
        if (Fcb->PathNameHash == Hash && _wcsicmp(FileName, Fcb->PathName) == 0)
        {
            Fcb->RefCount++;
            KeReleaseSpinLock(&Vcb->FcbListLock, oldIrql);
            return Fcb;
        }

        //FIXME: need to compare against short name in FCB here

        current_entry = current_entry->Flink;
    }

    KeReleaseSpinLock(&Vcb->FcbListLock, oldIrql);

    return NULL;
}


NTSTATUS
NtfsFCBInitializeCache(PNTFS_VCB Vcb,
                       PNTFS_FCB Fcb)
{
    PFILE_OBJECT FileObject;
    NTSTATUS Status;
    PNTFS_CCB newCCB;

    if (NtfsFCBIsDirectory(Fcb))
        return STATUS_SUCCESS;

    FileObject = IoCreateStreamFileObject(NULL, Vcb->StorageDevice);
    /* The stream owns the VPB reference taken by IoCreateStreamFileObject.
     * Keep the original filesystem reachable after storage removal or a VPB
     * replacement, so its eventual close releases the FCB pin here. */
    FileObject->Vpb = Vcb->Vpb;

    newCCB = ExAllocatePoolWithTag(NonPagedPool, sizeof(NTFS_CCB), TAG_CCB);
    if (newCCB == NULL)
    {
        ObDereferenceObject(FileObject);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(newCCB, sizeof(NTFS_CCB));

    newCCB->Identifier.Type = NTFS_TYPE_CCB;
    newCCB->Identifier.Size = sizeof(NTFS_TYPE_CCB);

    FileObject->SectionObjectPointer = &Fcb->SectionObjectPointers;
    FileObject->FsContext = Fcb;
    FileObject->FsContext2 = newCCB;
    newCCB->PtrFileObject = FileObject;
    Fcb->FileObject = FileObject;
    Fcb->Vcb = Vcb;

    Status = STATUS_SUCCESS;
    _SEH2_TRY
    {
        CcInitializeCacheMap(FileObject,
                             (PCC_FILE_SIZES)(&Fcb->RFCB.AllocationSize),
                             FALSE,
                             &(NtfsGlobalData->CacheMgrCallbacks),
                             Fcb);
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        FileObject->FsContext2 = NULL;
        ExFreePoolWithTag(newCCB, TAG_CCB);
        ObDereferenceObject(FileObject);
        Fcb->FileObject = NULL;
        return _SEH2_GetExceptionCode();
    }
    _SEH2_END;

    NtfsGrabFCB(Vcb, Fcb);
    newCCB->Flags |= CCB_FCB_REFERENCE;
    Fcb->Flags |= FCB_CACHE_INITIALIZED;
    ObDereferenceObject(FileObject);

    return Status;
}


PNTFS_FCB
NtfsMakeRootFCB(PNTFS_VCB Vcb)
{
    PNTFS_FCB Fcb;
    PFILE_RECORD_HEADER MftRecord;
    PFILENAME_ATTRIBUTE FileName;

    MftRecord = ExAllocateFromNPagedLookasideList(&Vcb->FileRecLookasideList);
    if (MftRecord == NULL)
    {
        return NULL;
    }

    if (!NT_SUCCESS(ReadFileRecord(Vcb, NTFS_FILE_ROOT, MftRecord)))
    {
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, MftRecord);
        return NULL;
    }

    FileName = GetFileNameFromRecord(Vcb, MftRecord, NTFS_FILE_NAME_WIN32);
    if (!FileName)
    {
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, MftRecord);
        return NULL;
    }

    Fcb = NtfsCreateFCB(L"\\", NULL, Vcb);
    if (!Fcb)
    {
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, MftRecord);
        return NULL;
    }

    memcpy(&Fcb->Entry, FileName, FIELD_OFFSET(FILENAME_ATTRIBUTE, NameLength));
    Fcb->Entry.NameType = FileName->NameType;
    Fcb->Entry.NameLength = 0;
    Fcb->Entry.Name[0] = UNICODE_NULL;
    Fcb->RefCount = 1;
    Fcb->DirIndex = 0;
    Fcb->RFCB.FileSize.QuadPart = FileName->DataSize;
    Fcb->RFCB.ValidDataLength.QuadPart = FileName->DataSize;
    Fcb->RFCB.AllocationSize.QuadPart = FileName->AllocatedSize;
    Fcb->MFTIndex = NTFS_FILE_ROOT;
    Fcb->ParentMFTIndex = NTFS_FILE_ROOT;
    Fcb->SequenceNumber = MftRecord->SequenceNumber;
    Fcb->LinkCount = MftRecord->LinkCount;

    NtfsFCBInitializeCache(Vcb, Fcb);
    NtfsAddFCBToTable(Vcb, Fcb);
    /* RefCount already includes the caller. Directories remain in the table
     * at zero; another reference here would permanently veto query-remove. */

    ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, MftRecord);

    return Fcb;
}


PNTFS_FCB
NtfsOpenRootFCB(PNTFS_VCB Vcb)
{
    PNTFS_FCB Fcb;

    Fcb = NtfsGrabFCBFromTable(Vcb, L"\\");
    if (Fcb == NULL)
    {
        Fcb = NtfsMakeRootFCB(Vcb);
    }

    return Fcb;
}


NTSTATUS
NtfsMakeFCBFromDirEntry(PNTFS_VCB Vcb,
                        PNTFS_FCB DirectoryFCB,
                        PUNICODE_STRING Name,
                        PCWSTR Stream,
                        PFILE_RECORD_HEADER Record,
                        ULONGLONG MFTIndex,
                        PNTFS_FCB * fileFCB)
{
    NTSTATUS Status;
    PWCHAR pathName;
    PFILENAME_ATTRIBUTE FileName;
    PSTANDARD_INFORMATION StdInfo;
    PNTFS_FCB rcFCB;
    ULONGLONG Size, AllocatedSize;

    DPRINT("NtfsMakeFCBFromDirEntry(%p, %p, %wZ, %p, %p, %p)\n", Vcb, DirectoryFCB, Name, Stream, Record, fileFCB);

    FileName = GetBestFileNameFromRecord(Vcb, Record);
    if (!FileName)
    {
        return STATUS_OBJECT_NAME_NOT_FOUND; // Not sure that's the best here
    }

    /* Name->Buffer is NUL-terminated and goes on past Name->Length with the
     * stream part, if any, which is part of the FCB's path */
    if (DirectoryFCB && Name)
    {
        pathName = NtfsBuildChildPath(DirectoryFCB->PathName, Name->Buffer);
    }
    else
    {
        pathName = ExAllocatePoolWithTag(NonPagedPool, (FileName->NameLength + 1) * sizeof(WCHAR), TAG_NTFS);
        if (pathName != NULL)
        {
            RtlCopyMemory(pathName, FileName->Name, FileName->NameLength * sizeof(WCHAR));
            pathName[FileName->NameLength] = UNICODE_NULL;
        }
    }

    if (pathName == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Size = NtfsGetFileSize(Vcb, Record, (Stream ? Stream : L""), (Stream ? wcslen(Stream) : 0), &AllocatedSize);

    rcFCB = NtfsCreateFCB(pathName, Stream, Vcb);
    ExFreePoolWithTag(pathName, TAG_NTFS);
    if (!rcFCB)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    memcpy(&rcFCB->Entry, FileName, FIELD_OFFSET(FILENAME_ATTRIBUTE, NameLength));
    rcFCB->Entry.NameType = FileName->NameType;
    rcFCB->RFCB.FileSize.QuadPart = Size;
    rcFCB->RFCB.ValidDataLength.QuadPart = Size;
    rcFCB->RFCB.AllocationSize.QuadPart = AllocatedSize;

    StdInfo = GetStandardInformationFromRecord(Vcb, Record);
    if (StdInfo != NULL)
    {
        rcFCB->Entry.FileAttributes = StdInfo->FileAttribute |
                                     (FileName->FileAttributes & NTFS_FILE_TYPE_DIRECTORY);
        rcFCB->Entry.CreationTime = StdInfo->CreationTime;
        rcFCB->Entry.LastAccessTime = StdInfo->LastAccessTime;
        rcFCB->Entry.LastWriteTime = StdInfo->LastWriteTime;
        rcFCB->Entry.ChangeTime = StdInfo->ChangeTime;
    }

    rcFCB->RefCount = 1;
    rcFCB->MFTIndex = MFTIndex;
    rcFCB->ParentMFTIndex = DirectoryFCB ? DirectoryFCB->MFTIndex : (FileName->DirectoryFileReferenceNumber & NTFS_MFT_MASK);
    rcFCB->SequenceNumber = Record->SequenceNumber;
    rcFCB->LinkCount = Record->LinkCount;
    Status = NtfsInitializeFcbLock(rcFCB);
    if (!NT_SUCCESS(Status))
    {
        NtfsDestroyFCB(rcFCB);
        return Status;
    }
    NtfsFCBInitializeCache(Vcb, rcFCB);
    NtfsAddFCBToTable(Vcb, rcFCB);
    *fileFCB = rcFCB;

    return STATUS_SUCCESS;
}


/**
* @name NtfsSetLinkCount
*
* Gives every cached FCB of a file - one per name it was opened by, and its streams - the
* link count its record has now.
*/
VOID
NtfsSetLinkCount(
    _In_ PNTFS_VCB Vcb,
    _In_ ULONGLONG MftIndex,
    _In_ USHORT LinkCount)
{
    PLIST_ENTRY Entry;
    KIRQL OldIrql;

    KeAcquireSpinLock(&Vcb->FcbListLock, &OldIrql);

    for (Entry = Vcb->FcbListHead.Flink; Entry != &Vcb->FcbListHead; Entry = Entry->Flink)
    {
        PNTFS_FCB Fcb = CONTAINING_RECORD(Entry, NTFS_FCB, FcbListEntry);

        if (Fcb->MFTIndex == MftIndex && Fcb->LinkCount != LinkCount)
        {
            Fcb->LinkCount = LinkCount;
            Fcb->CacheGeneration++;
        }
    }

    KeReleaseSpinLock(&Vcb->FcbListLock, OldIrql);
}

/**
* @name NtfsMakeStreamFCB
*
* The FCB of a named data stream of the file BaseFcb is open on: the one in the FCB table
* under "path:stream" if the stream is open, else a new one put there.
*
* @param StreamName
* The stream's name, at most 255 characters.
*
* @param StreamFcb
* Receives the FCB, referenced.
*
* @return
* STATUS_SUCCESS, STATUS_OBJECT_NAME_NOT_FOUND if the file has no such stream, or an
* allocation or I/O error.
*/
NTSTATUS
NtfsMakeStreamFCB(
    _In_ PNTFS_VCB Vcb,
    _In_ PNTFS_FCB BaseFcb,
    _In_ PCUNICODE_STRING StreamName,
    _Out_ PNTFS_FCB *StreamFcb)
{
    WCHAR Stream[NTFS_MAX_NAME_LENGTH + 1];
    PFILE_RECORD_HEADER Record;
    PNTFS_ATTR_CONTEXT DataContext;
    ULONGLONG AllocatedSize;
    SIZE_T BaseLength;
    PWCHAR Path;
    PNTFS_FCB Fcb;
    NTSTATUS Status;

    ASSERT(StreamName->Length != 0 && StreamName->Length <= NTFS_MAX_NAME_LENGTH * sizeof(WCHAR));
    ASSERT(BaseFcb->Stream[0] == UNICODE_NULL);

    *StreamFcb = NULL;

    RtlCopyMemory(Stream, StreamName->Buffer, StreamName->Length);
    Stream[StreamName->Length / sizeof(WCHAR)] = UNICODE_NULL;

    Record = ExAllocateFromNPagedLookasideList(&Vcb->FileRecLookasideList);
    if (Record == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = ReadFileRecord(Vcb, BaseFcb->MFTIndex, Record);
    if (NT_SUCCESS(Status))
    {
        Status = FindAttribute(Vcb, Record, AttributeData, Stream, StreamName->Length / sizeof(WCHAR), &DataContext, NULL);
        if (NT_SUCCESS(Status))
        {
            /* Keep the stream's name as the disk has it */
            RtlCopyMemory(Stream,
                          (PUCHAR)DataContext->pRecord + DataContext->pRecord->NameOffset,
                          StreamName->Length);
            ReleaseAttributeContext(DataContext);
        }
        else if (Status == STATUS_NOT_FOUND || Status == STATUS_END_OF_FILE)
        {
            Status = STATUS_OBJECT_NAME_NOT_FOUND;
        }
    }

    if (!NT_SUCCESS(Status))
    {
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, Record);
        return Status;
    }

    BaseLength = wcslen(BaseFcb->PathName);
    Path = ExAllocatePoolWithTag(NonPagedPool, (BaseLength + 1 + wcslen(Stream) + 1) * sizeof(WCHAR), TAG_NTFS);
    if (Path == NULL)
    {
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, Record);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlCopyMemory(Path, BaseFcb->PathName, BaseLength * sizeof(WCHAR));
    Path[BaseLength] = L':';
    wcscpy(Path + BaseLength + 1, Stream);

    /* A stream already open has its FCB: share access, byte-range locks and
     * sizes are the stream's, however the name was spelled. */
    Fcb = NtfsGrabFCBFromTable(Vcb, Path);
    if (Fcb != NULL)
    {
        ExFreePoolWithTag(Path, TAG_NTFS);
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, Record);
        *StreamFcb = Fcb;
        return STATUS_SUCCESS;
    }

    Fcb = NtfsCreateFCB(Path, Stream, Vcb);
    ExFreePoolWithTag(Path, TAG_NTFS);
    if (Fcb == NULL)
    {
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, Record);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* A stream is a file's data, even when the file is a directory */
    RtlCopyMemory(&Fcb->Entry, &BaseFcb->Entry, FIELD_OFFSET(FILENAME_ATTRIBUTE, NameLength));
    Fcb->Entry.FileAttributes &= ~NTFS_FILE_TYPE_DIRECTORY;
    Fcb->Entry.NameType = BaseFcb->Entry.NameType;

    Fcb->RFCB.FileSize.QuadPart = NtfsGetFileSize(Vcb, Record, Stream, wcslen(Stream), &AllocatedSize);
    Fcb->RFCB.ValidDataLength = Fcb->RFCB.FileSize;
    Fcb->RFCB.AllocationSize.QuadPart = AllocatedSize;

    Fcb->RefCount = 1;
    Fcb->MFTIndex = BaseFcb->MFTIndex;
    Fcb->ParentMFTIndex = BaseFcb->ParentMFTIndex;
    Fcb->SequenceNumber = BaseFcb->SequenceNumber;
    Fcb->LinkCount = BaseFcb->LinkCount;

    ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, Record);

    Status = NtfsInitializeFcbLock(Fcb);
    if (!NT_SUCCESS(Status))
    {
        NtfsDestroyFCB(Fcb);
        return Status;
    }
    NtfsFCBInitializeCache(Vcb, Fcb);
    NtfsAddFCBToTable(Vcb, Fcb);

    *StreamFcb = Fcb;

    return STATUS_SUCCESS;
}


NTSTATUS
NtfsAttachFCBToFileObject(PNTFS_VCB Vcb,
                          PNTFS_FCB Fcb,
                          PFILE_OBJECT FileObject)
{
    PNTFS_CCB newCCB;

    newCCB = ExAllocatePoolWithTag(NonPagedPool, sizeof(NTFS_CCB), TAG_CCB);
    if (newCCB == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(newCCB, sizeof(NTFS_CCB));

    newCCB->Identifier.Type = NTFS_TYPE_CCB;
    newCCB->Identifier.Size = sizeof(NTFS_TYPE_CCB);

    FileObject->SectionObjectPointer = &Fcb->SectionObjectPointers;
    FileObject->FsContext = Fcb;
    FileObject->FsContext2 = newCCB;
    newCCB->PtrFileObject = FileObject;
    newCCB->Flags |= CCB_FCB_REFERENCE | CCB_EXTERNAL_FILE_OBJECT;
    Vcb->FileObjectCount++;
    Fcb->Vcb = Vcb;

    /* Cached reads initialize this file object's private map under main
     * exclusive. Directories and raw volume handles never need a data map. */

    //DPRINT("file open: fcb:%x file size: %d\n", Fcb, Fcb->Entry.DataLengthL);

    return STATUS_SUCCESS;
}


static NTSTATUS
NtfsDirFindFile(PNTFS_VCB Vcb,
                PNTFS_FCB DirectoryFcb,
                PWSTR FileToFind,
                BOOLEAN CaseSensitive,
                PNTFS_FCB *FoundFCB)
{
    NTSTATUS Status;
    ULONGLONG CurrentDir;
    UNICODE_STRING File;
    PFILE_RECORD_HEADER FileRecord;
    ULONGLONG MFTIndex;
    PWSTR Colon, OldColon;
    PNTFS_ATTR_CONTEXT DataContext;
    USHORT Length = 0;

    DPRINT("NtfsDirFindFile(%p, %p, %S, %s, %p)\n",
           Vcb,
           DirectoryFcb,
           FileToFind,
           CaseSensitive ? "TRUE" : "FALSE",
           FoundFCB);

    *FoundFCB = NULL;
    RtlInitUnicodeString(&File, FileToFind);
    CurrentDir = DirectoryFcb->MFTIndex;

    Colon = wcsrchr(FileToFind, L':');
    if (Colon != NULL)
    {
        Length = File.Length;
        File.Length = (Colon - FileToFind) * sizeof(WCHAR);

        if (_wcsicmp(Colon + 1, L"$DATA") == 0)
        {
            OldColon = Colon;
            Colon[0] = UNICODE_NULL;
            Colon = wcsrchr(FileToFind, L':');
            if (Colon != NULL)
            {
                Length = File.Length;
                File.Length = (Colon - FileToFind) * sizeof(WCHAR);
            }
            else
            {
                Colon = OldColon;
                Colon[0] = L':';
            }
        }

        /* Skip colon */
        ++Colon;
        DPRINT("Will now look for file '%wZ' with stream '%S'\n", &File, Colon);
    }

    Status = NtfsLookupFileAt(Vcb, &File, CaseSensitive, &FileRecord, &MFTIndex, CurrentDir);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    if (Length != 0)
    {
        File.Length = Length;
    }

    if ((FileRecord->Flags & FRH_DIRECTORY) && Colon != 0)
    {
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, FileRecord);
        return STATUS_INVALID_PARAMETER;
    }
    else if (Colon != 0)
    {
        Status = FindAttribute(Vcb, FileRecord, AttributeData, Colon, wcslen(Colon), &DataContext, NULL);
        if (!NT_SUCCESS(Status))
        {
            return STATUS_OBJECT_NAME_NOT_FOUND;
        }
        ReleaseAttributeContext(DataContext);
    }

    Status = NtfsMakeFCBFromDirEntry(Vcb, DirectoryFcb, &File, Colon, FileRecord, MFTIndex, FoundFCB);
    ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, FileRecord);

    return Status;
}


NTSTATUS
NtfsGetFCBForFile(PNTFS_VCB Vcb,
                  PNTFS_FCB *pParentFCB,
                  PNTFS_FCB *pFCB,
                  PCWSTR pFileName,
                  BOOLEAN CaseSensitive,
                  PULONG ReparseOffset)
{
    NTSTATUS Status;
    PWCHAR pathName;
    PWCHAR elementName;
    SIZE_T pathCount;
    PCWSTR currentElement;
    PNTFS_FCB FCB;
    PNTFS_FCB parentFCB;

    DPRINT("NtfsGetFCBForFile(%p, %p, %p, '%S', %s)\n",
           Vcb,
           pParentFCB,
           pFCB,
           pFileName,
           CaseSensitive ? "TRUE" : "FALSE");

    /* Dummy code */
//  FCB = NtfsOpenRootFCB(Vcb);
//  *pFCB = FCB;
//  *pParentFCB = NULL;

#if 1
    /* Trivial case, open of the root directory on volume */
    if (pFileName[0] == L'\0' || wcscmp(pFileName, L"\\") == 0)
    {
        DPRINT("returning root FCB\n");

        FCB = NtfsOpenRootFCB(Vcb);
        *pFCB = FCB;
        *pParentFCB = NULL;

        return (FCB != NULL) ? STATUS_SUCCESS : STATUS_OBJECT_PATH_NOT_FOUND;
    }
    else
    {
        currentElement = pFileName + 1;
        FCB = NtfsOpenRootFCB (Vcb);
    }

    if (FCB == NULL)
    {
        *pFCB = NULL;
        *pParentFCB = NULL;
        return STATUS_OBJECT_PATH_NOT_FOUND;
    }

    /* Each holds at most the whole path */
    pathCount = wcslen(pFileName) + 1;
    pathName = ExAllocatePoolWithTag(NonPagedPool, 2 * pathCount * sizeof(WCHAR), TAG_NTFS);
    if (pathName == NULL)
    {
        NtfsReleaseFCB(Vcb, FCB);
        *pFCB = NULL;
        *pParentFCB = NULL;
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    elementName = pathName + pathCount;

    parentFCB = NULL;
    Status = STATUS_SUCCESS;

    /* Parse filename and check each path element for existence and access */
    while (NtfsGetNextPathElement(currentElement) != 0)
    {
        PCWSTR nextElement = NtfsGetNextPathElement(currentElement);

        /* Skip blank directory levels */
        if (nextElement == currentElement)
        {
            currentElement++;
            continue;
        }

        DPRINT("Parsing, currentElement:%S\n", currentElement);
        DPRINT("  parentFCB:%p FCB:%p\n", parentFCB, FCB);

        /* Descend to next directory level */
        if (parentFCB)
        {
            NtfsReleaseFCB(Vcb, parentFCB);
            parentFCB = NULL;
        }

        /* fail if element in FCB is not a directory */
        if (!NtfsFCBIsDirectory(FCB))
        {
            DPRINT("Element in requested path is not a directory\n");

            NtfsReleaseFCB(Vcb, FCB);
            FCB = NULL;
            Status = STATUS_OBJECT_PATH_NOT_FOUND;
            break;
        }

        /* A reparse point on the way (a mount point) sends the rest of the path
         * elsewhere: hand it back with where the unparsed rest starts */
        if (NtfsFCBIsReparsePoint(FCB))
        {
            *ReparseOffset = (ULONG)(currentElement - 1 - pFileName);
            Status = STATUS_REPARSE;
            break;
        }

        parentFCB = FCB;

        /* Extract next directory level into pathName */
        NtfsWSubString(pathName, pathCount, pFileName, nextElement - pFileName);
        DPRINT("  pathName:%S\n", pathName);

        FCB = NtfsGrabFCBFromTable(Vcb, pathName);
        if (FCB == NULL)
        {
            NtfsWSubString(elementName, pathCount, currentElement, nextElement - currentElement);
            DPRINT("  elementName:%S\n", elementName);

            Status = NtfsDirFindFile(Vcb, parentFCB, elementName, CaseSensitive, &FCB);
            if (Status == STATUS_OBJECT_NAME_NOT_FOUND)
            {
                /* Only the last element missing is a missing name; the parent is handed back
                 * for a create */
                if (*nextElement != L'\0' && NtfsGetNextPathElement(nextElement + 1) != 0)
                    Status = STATUS_OBJECT_PATH_NOT_FOUND;

                FCB = NULL;
                break;
            }
            else if (!NT_SUCCESS(Status))
            {
                NtfsReleaseFCB(Vcb, parentFCB);
                parentFCB = NULL;
                FCB = NULL;
                break;
            }
        }

        currentElement = nextElement;
    }

    ExFreePoolWithTag(pathName, TAG_NTFS);

    *pParentFCB = parentFCB;
    *pFCB = FCB;
#endif

    return Status;
}


NTSTATUS
NtfsReadFCBAttribute(PNTFS_VCB Vcb,
                     PNTFS_FCB pFCB,
                     ULONG Type,
                     PCWSTR Name,
                     ULONG NameLength,
                     PVOID * Data)
{
    NTSTATUS Status;
    PFILE_RECORD_HEADER FileRecord;
    PNTFS_ATTR_CONTEXT AttrCtxt;
    ULONGLONG AttrLength;

    FileRecord = ExAllocateFromNPagedLookasideList(&Vcb->FileRecLookasideList);
    if (FileRecord == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Status = ReadFileRecord(Vcb, pFCB->MFTIndex, FileRecord);
    if (!NT_SUCCESS(Status))
    {
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, FileRecord);
        return Status;
    }

    Status = FindAttribute(Vcb, FileRecord, Type, Name, NameLength, &AttrCtxt, NULL);
    if (!NT_SUCCESS(Status))
    {
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, FileRecord);
        return Status;
    }

    AttrLength = AttributeDataLength(AttrCtxt->pRecord);
    *Data = ExAllocatePoolWithTag(NonPagedPool, AttrLength, TAG_NTFS);
    if (*Data == NULL)
    {
        ReleaseAttributeContext(AttrCtxt);
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, FileRecord);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    ReadAttribute(Vcb, AttrCtxt, 0, *Data, AttrLength);

    ReleaseAttributeContext(AttrCtxt);
    ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, FileRecord);

    return STATUS_SUCCESS;
}

/* EOF */
