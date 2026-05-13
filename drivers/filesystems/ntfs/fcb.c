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


static
VOID
NtfsWSubString(PWCHAR pTarget,
               PCWSTR pSource,
               size_t pLength)
{
    wcsncpy(pTarget, pSource, pLength);
    pTarget[pLength] = L'\0';
}


PNTFS_FCB
NtfsCreateFCB(PCWSTR FileName,
              PCWSTR Stream,
              PNTFS_VCB Vcb)
{
    PNTFS_FCB Fcb;

    ASSERT(Vcb);
    ASSERT(Vcb->Identifier.Type == NTFS_TYPE_VCB);

    /* Try to free any zombie FCBs whose MM references have finally
     * dropped, so we don't grow the zombie list unboundedly. */
    NtfsReapZombieFcbs();

    Fcb = ExAllocateFromNPagedLookasideList(&NtfsGlobalData->FcbLookasideList);
    if (Fcb == NULL)
    {
        return NULL;
    }

    RtlZeroMemory(Fcb, sizeof(NTFS_FCB));

    /* Allocate the SECTION_OBJECT_POINTERS struct out-of-band so it can
     * outlive the FCB if MM still holds a reference at destruction time.
     * See the comment on NTFS_FCB::SectionObjectPointers in ntfs.h. */
    Fcb->SectionObjectPointers = ExAllocatePoolWithTag(NonPagedPool,
                                                       sizeof(SECTION_OBJECT_POINTERS),
                                                       TAG_SOP);
    if (Fcb->SectionObjectPointers == NULL)
    {
        ExFreeToNPagedLookasideList(&NtfsGlobalData->FcbLookasideList, Fcb);
        return NULL;
    }
    RtlZeroMemory(Fcb->SectionObjectPointers, sizeof(SECTION_OBJECT_POINTERS));

    Fcb->Identifier.Type = NTFS_TYPE_FCB;
    Fcb->Identifier.Size = sizeof(NTFS_TYPE_FCB);

    Fcb->Vcb = Vcb;

    if (FileName)
    {
        wcscpy(Fcb->PathName, FileName);
        if (wcsrchr(Fcb->PathName, '\\') != 0)
        {
            Fcb->ObjectName = wcsrchr(Fcb->PathName, '\\');
        }
        else
        {
            Fcb->ObjectName = Fcb->PathName;
        }
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

    Fcb->RFCB.Resource = &(Fcb->MainResource);
    Fcb->RFCB.PagingIoResource = &(Fcb->PagingIoResource);

    /* Byte-range file locks. The completion / unlock callbacks are
     * optional and we don't need them — FsRtlProcessFileLock and the
     * default unlock path do everything we want. Used by lock.c
     * (NtfsLockControl) and consulted by NtfsRead / NtfsWrite via
     * FsRtlCheckLockForReadAccess / FsRtlCheckLockForWriteAccess. */
    FsRtlInitializeFileLock(&Fcb->FileLock, NULL, NULL);

    return Fcb;
}


/* Drop the cached MFT record (if any) and clear the slot.  Called from
 * the FCB teardown path and from FCB-aware write paths that mutate the
 * on-disk file record so the next reader picks up fresh data.  Uses an
 * atomic exchange so a concurrent NtfsReadFile install loses cleanly:
 * either the installer's CAS sees NULL and stores its buffer (which
 * this exchange then claims and frees), or it sees the old pointer
 * we just nulled out and frees its own buffer.  Invocation happens
 * under the FCB MainResource taken exclusive by writers, so racing
 * readers never observe a half-replaced cache. */
VOID
NtfsInvalidateCachedFileRecord(PNTFS_FCB Fcb)
{
    PFILE_RECORD_HEADER Old;

    Old = InterlockedExchangePointer((PVOID *)&Fcb->CachedFileRecord, NULL);
    if (Old != NULL)
    {
        ExFreePoolWithTag(Old, TAG_NTFS);
    }
}

/* Free an FCB whose SectionObjectPointers slot is fully clean: tear down
 * the resources, FILE_LOCK, the SOP struct itself, and return the FCB
 * pool block to the lookaside list.  Caller must guarantee no other
 * thread can still hold a pointer to this FCB. */
static VOID
NtfsFreeFcbStorage(PNTFS_FCB Fcb)
{
    if (Fcb->SectionObjectPointers != NULL)
    {
        ExFreePoolWithTag(Fcb->SectionObjectPointers, TAG_SOP);
        Fcb->SectionObjectPointers = NULL;
    }

    NtfsInvalidateCachedFileRecord(Fcb);

    FsRtlUninitializeFileLock(&Fcb->FileLock);
    ExDeleteResourceLite(&Fcb->PagingIoResource);
    ExDeleteResourceLite(&Fcb->MainResource);

    ExFreeToNPagedLookasideList(&NtfsGlobalData->FcbLookasideList, Fcb);
}

/* Walk the global zombie list and free any FCB whose SOP slots have all
 * been cleared by MM (MmDereferenceSegmentWithLock writes
 * SectionObjectPointer->DataSectionObject = NULL when the segment finally
 * dies — see ntoskrnl/mm/section.c).  Called opportunistically from
 * NtfsCreateFCB and NtfsDestroyFCB so we don't need a dedicated worker
 * thread; the cost is one short critical-section walk per FCB churn,
 * which is negligible compared to the I/O the same code path is doing.
 *
 * Runs at PASSIVE/APC level — same constraints as the call sites. */
VOID
NtfsReapZombieFcbs(VOID)
{
    KIRQL OldIrql;
    LIST_ENTRY ToFree;
    PLIST_ENTRY entry, next;

    InitializeListHead(&ToFree);

    KeAcquireSpinLock(&NtfsGlobalData->ZombieLock, &OldIrql);
    for (entry = NtfsGlobalData->ZombieFcbList.Flink;
         entry != &NtfsGlobalData->ZombieFcbList;
         entry = next)
    {
        PNTFS_FCB zombie = CONTAINING_RECORD(entry, NTFS_FCB, ZombieListEntry);
        next = entry->Flink;

        if (zombie->SectionObjectPointers == NULL ||
            (zombie->SectionObjectPointers->DataSectionObject == NULL &&
             zombie->SectionObjectPointers->ImageSectionObject == NULL &&
             zombie->SectionObjectPointers->SharedCacheMap == NULL))
        {
            RemoveEntryList(entry);
            InsertTailList(&ToFree, entry);
        }
    }
    KeReleaseSpinLock(&NtfsGlobalData->ZombieLock, OldIrql);

    /* Free outside the lock — ExDeleteResourceLite / ExFreePool can be
     * slow and may acquire other locks. */
    while (!IsListEmpty(&ToFree))
    {
        PLIST_ENTRY e = RemoveHeadList(&ToFree);
        PNTFS_FCB zombie = CONTAINING_RECORD(e, NTFS_FCB, ZombieListEntry);
        DPRINT("NtfsReapZombieFcbs: reaping FCB %p\n", zombie);
        NtfsFreeFcbStorage(zombie);
    }
}

VOID
NtfsDestroyFCB(PNTFS_FCB Fcb)
{
    ASSERT(Fcb);
    ASSERT(Fcb->Identifier.Type == NTFS_TYPE_FCB);

    /* Opportunistic zombie reap on the way through. */
    NtfsReapZombieFcbs();

    /* If the memory manager still has section objects referencing the FCB's
     * SectionObjectPointers, freeing the FCB pool block would leave a
     * dangling FileObject->FsContext: MM's Segment->FileObject is still
     * usable for paging I/O, and NtfsRead would dereference the freed FCB
     * (and the freed PagingIoResource that lives inside it).  That race
     * surfaces as ASSERT(Owner != NULL) at ntoskrnl/ex/resource.c:1939
     * because the freed FCB pool gets reused for a new FCB whose
     * resources have a different owner thread.
     *
     * Strategy:
     *   1. Try to tear the sections down ourselves (MmFlushImageSection +
     *      MmForceSectionClosed with DelayClose=TRUE).
     *   2. If that succeeds (slots all NULL), free the FCB now.
     *   3. Otherwise put the FCB on the global zombie list and let
     *      NtfsReapZombieFcbs free it later, when MM has cleared the
     *      SOP slots from MmDereferenceSegmentWithLock.
     *
     * Both NtfsDestroyFCB call sites (NtfsReleaseFCB after dropping
     * FcbListLock, and the NtfsMountVolume error path which uses a fresh
     * FCB) call this at IRQL <= APC_LEVEL without holding the FCB
     * resource, which is what MmFlushImageSection / MmForceSectionClosed
     * require. */
    if (Fcb->SectionObjectPointers != NULL &&
        (Fcb->SectionObjectPointers->DataSectionObject != NULL ||
         Fcb->SectionObjectPointers->ImageSectionObject != NULL ||
         Fcb->SectionObjectPointers->SharedCacheMap != NULL))
    {
        DPRINT("NtfsDestroyFCB(%p): tearing down lingering sections (Data=%p Image=%p Cache=%p)\n",
                Fcb, Fcb->SectionObjectPointers->DataSectionObject,
                Fcb->SectionObjectPointers->ImageSectionObject,
                Fcb->SectionObjectPointers->SharedCacheMap);

        /* Flush dirty image-section pages so MmForceSectionClosed doesn't
         * refuse on account of them.  Ignore the return value — it returns
         * FALSE only when there's no image section to flush, which is fine. */
        if (Fcb->SectionObjectPointers->ImageSectionObject != NULL)
        {
            MmFlushImageSection(Fcb->SectionObjectPointers, MmFlushForWrite);
        }

        /* Force the section(s) closed.  DelayClose=TRUE lets MM defer the
         * actual teardown if it can't drop the segment immediately, which
         * matches the contract Windows file systems rely on. */
        MmForceSectionClosed(Fcb->SectionObjectPointers, TRUE);

        /* If MM still references the SOP, the FCB becomes a zombie:
         * stays alive until NtfsReapZombieFcbs notices the SOP is clean. */
        if (Fcb->SectionObjectPointers->DataSectionObject != NULL ||
            Fcb->SectionObjectPointers->ImageSectionObject != NULL ||
            Fcb->SectionObjectPointers->SharedCacheMap != NULL)
        {
            KIRQL OldIrql;
            DPRINT("NtfsDestroyFCB(%p): zombifying (SOP=%p Data=%p Image=%p Cache=%p)\n",
                    Fcb, Fcb->SectionObjectPointers,
                    Fcb->SectionObjectPointers->DataSectionObject,
                    Fcb->SectionObjectPointers->ImageSectionObject,
                    Fcb->SectionObjectPointers->SharedCacheMap);
            KeAcquireSpinLock(&NtfsGlobalData->ZombieLock, &OldIrql);
            InsertTailList(&NtfsGlobalData->ZombieFcbList, &Fcb->ZombieListEntry);
            KeReleaseSpinLock(&NtfsGlobalData->ZombieLock, OldIrql);
            return;
        }
    }

    NtfsFreeFcbStorage(Fcb);
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
    KIRQL oldIrql;

    DPRINT("releasing FCB at %p: %S, refCount:%d\n",
           Fcb,
           Fcb->PathName,
           Fcb->RefCount);

    KeAcquireSpinLock(&Vcb->FcbListLock, &oldIrql);
    Fcb->RefCount--;

    /* When the last external reference is released but the internal stream
     * file object still holds a cache map, tear down the cache and release
     * that file object.  This lets the memory manager fully clean up any
     * data section segments before the FCB (and its embedded
     * SectionObjectPointers) is freed.  Mirrors the two-step pattern used
     * by vfatfs. */
    if (Fcb->RefCount == 1 &&
        !NtfsFCBIsDirectory(Fcb) &&
        BooleanFlagOn(Fcb->Flags, FCB_CACHE_INITIALIZED) &&
        Fcb->FileObject != NULL)
    {
        PFILE_OBJECT tmpFileObject = Fcb->FileObject;
        Fcb->FileObject = NULL;
        ClearFlag(Fcb->Flags, FCB_CACHE_INITIALIZED);
        KeReleaseSpinLock(&Vcb->FcbListLock, oldIrql);

        /* Flush and purge all dirty data BEFORE uninitializing the cache map.
         * CcUninitializeCacheMap can drop the last reference on the data section
         * segment, triggering MmDereferenceSegmentWithLock → FreeSegmentPage.
         * If dirty pages remain at that point, FreeSegmentPage calls MiWritePage
         * which does synchronous I/O that can re-enter the page-out path under
         * memory pressure, corrupting pool.  Flushing here writes dirty pages
         * under controlled conditions (proper IRQL, no re-entrancy risk). */
        if (tmpFileObject->SectionObjectPointer)
        {
            if (tmpFileObject->SectionObjectPointer->SharedCacheMap)
            {
                IO_STATUS_BLOCK Iosb;
                CcFlushCache(tmpFileObject->SectionObjectPointer, NULL, 0, &Iosb);
            }
            CcPurgeCacheSection(tmpFileObject->SectionObjectPointer, NULL, 0, FALSE);
        }

        CcUninitializeCacheMap(tmpFileObject, NULL, NULL);
        /* Do NOT ObDereferenceObject(tmpFileObject) here.
         * NtfsAttachFCBToFileObject already dropped its reference (line 395).
         * The remaining references belong to the cache manager and the MM
         * section segment — they will be released by CcUninitializeCacheMap
         * and MmDereferenceSegmentWithLock respectively. An extra deref here
         * causes a use-after-free: the FileObject is freed while the MM
         * segment still holds a pointer to it. */
    }
    else if (Fcb->RefCount <= 0 && !NtfsFCBIsDirectory(Fcb))
    {
        PFILE_OBJECT tmpFileObject = NULL;

        /* If cache is still initialized, tear it down before freeing the FCB.
         * This can happen if the RefCount == 1 check above was skipped because
         * FileObject was NULL (stale stream file object), or if RefCount
         * went from >1 to 0 in one step. */
        if (BooleanFlagOn(Fcb->Flags, FCB_CACHE_INITIALIZED) &&
            Fcb->FileObject != NULL)
        {
            tmpFileObject = Fcb->FileObject;
            Fcb->FileObject = NULL;
            ClearFlag(Fcb->Flags, FCB_CACHE_INITIALIZED);
        }

        RemoveEntryList(&Fcb->FcbListEntry);
        KeReleaseSpinLock(&Vcb->FcbListLock, oldIrql);

        if (tmpFileObject)
        {
            CcUninitializeCacheMap(tmpFileObject, NULL, NULL);
            /* Same as above: do not ObDereferenceObject here. */
        }

        NtfsDestroyFCB(Fcb);
    }
    else
    {
        KeReleaseSpinLock(&Vcb->FcbListLock, oldIrql);
    }

    /* Debug: verify IRQL is restored after FCB release */
    ASSERT(KeGetCurrentIrql() <= APC_LEVEL);
}


VOID
NtfsAddFCBToTable(PNTFS_VCB Vcb,
                  PNTFS_FCB Fcb)
{
    KIRQL oldIrql;

    KeAcquireSpinLock(&Vcb->FcbListLock, &oldIrql);
    Fcb->Vcb = Vcb;
    InsertTailList(&Vcb->FcbListHead, &Fcb->FcbListEntry);
    KeReleaseSpinLock(&Vcb->FcbListLock, oldIrql);
}


PNTFS_FCB
NtfsGrabFCBFromTable(PNTFS_VCB Vcb,
                     PCWSTR FileName)
{
    KIRQL oldIrql;
    PNTFS_FCB Fcb;
    PLIST_ENTRY current_entry;

    KeAcquireSpinLock(&Vcb->FcbListLock, &oldIrql);

    if (FileName == NULL || *FileName == 0)
    {
        DPRINT("Return FCB for stream file object\n");
        Fcb = Vcb->StreamFileObject->FsContext;
        Fcb->RefCount++;
        KeReleaseSpinLock(&Vcb->FcbListLock, oldIrql);
        return Fcb;
    }

    current_entry = Vcb->FcbListHead.Flink;
    while (current_entry != &Vcb->FcbListHead)
    {
        Fcb = CONTAINING_RECORD(current_entry, NTFS_FCB, FcbListEntry);

        DPRINT("Comparing '%S' and '%S'\n", FileName, Fcb->PathName);
        if (_wcsicmp(FileName, Fcb->PathName) == 0)
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

    FileObject = IoCreateStreamFileObject(NULL, Vcb->StorageDevice);

    newCCB = ExAllocatePoolWithTag(NonPagedPool, sizeof(NTFS_CCB), TAG_CCB);
    if (newCCB == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(newCCB, sizeof(NTFS_CCB));

    newCCB->Identifier.Type = NTFS_TYPE_CCB;
    newCCB->Identifier.Size = sizeof(NTFS_TYPE_CCB);

    FileObject->SectionObjectPointer = Fcb->SectionObjectPointers;
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

    ObDereferenceObject(FileObject);
    Fcb->Flags |= FCB_CACHE_INITIALIZED;

    return Status;
}


PNTFS_FCB
NtfsMakeRootFCB(PNTFS_VCB Vcb)
{
    PNTFS_FCB Fcb;
    PFILE_RECORD_HEADER MftRecord;
    PFILENAME_ATTRIBUTE FileName;
    PSTANDARD_INFORMATION StdInfo;

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
    Fcb->LinkCount = MftRecord->LinkCount;

    StdInfo = GetStandardInformationFromRecord(Vcb, MftRecord);
    if (StdInfo != NULL)
    {
        Fcb->Entry.FileAttributes |= StdInfo->FileAttribute;
    }

    NtfsFCBInitializeCache(Vcb, Fcb);
    NtfsAddFCBToTable(Vcb, Fcb);
    NtfsGrabFCB(Vcb, Fcb);

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
    WCHAR pathName[MAX_PATH];
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

    if (DirectoryFCB && Name)
    {
        if (Name->Buffer[0] != 0 && wcslen(DirectoryFCB->PathName) +
            sizeof(WCHAR) + Name->Length / sizeof(WCHAR) > MAX_PATH)
        {
            return STATUS_OBJECT_NAME_INVALID;
        }

        wcscpy(pathName, DirectoryFCB->PathName);
        if (!NtfsFCBIsRoot(DirectoryFCB))
        {
            wcscat(pathName, L"\\");
        }
        wcscat(pathName, Name->Buffer);
    }
    else
    {
        RtlCopyMemory(pathName, FileName->Name, FileName->NameLength * sizeof (WCHAR));
        pathName[FileName->NameLength] = UNICODE_NULL;
    }

    Size = NtfsGetFileSize(Vcb, Record, (Stream ? Stream : L""), (Stream ? wcslen(Stream) : 0), &AllocatedSize);

    rcFCB = NtfsCreateFCB(pathName, Stream, Vcb);
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
        rcFCB->Entry.FileAttributes |= StdInfo->FileAttribute;
    }

    NtfsFCBInitializeCache(Vcb, rcFCB);
    rcFCB->RefCount = 1;
    rcFCB->MFTIndex = MFTIndex;
    rcFCB->LinkCount = Record->LinkCount;
    NtfsAddFCBToTable(Vcb, rcFCB);
    *fileFCB = rcFCB;

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

    FileObject->SectionObjectPointer = Fcb->SectionObjectPointers;
    FileObject->FsContext = Fcb;
    FileObject->FsContext2 = newCCB;
    newCCB->PtrFileObject = FileObject;
    Fcb->Vcb = Vcb;

    if (!(Fcb->Flags & FCB_CACHE_INITIALIZED))
    {
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
            return _SEH2_GetExceptionCode();
        }
        _SEH2_END;

        Fcb->Flags |= FCB_CACHE_INITIALIZED;
    }

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
        DPRINT1("Will now look for file '%wZ' with stream '%S'\n", &File, Colon);
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
                  BOOLEAN CaseSensitive)
{
    NTSTATUS Status;
    WCHAR pathName [MAX_PATH];
    WCHAR elementName [MAX_PATH];
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
        wcscpy (pathName, L"\\");
        FCB = NtfsOpenRootFCB (Vcb);
    }

    parentFCB = NULL;

    /* Parse filename and check each path element for existence and access */
    while (NtfsGetNextPathElement(currentElement) != 0)
    {
        /* Skip blank directory levels */
        if ((NtfsGetNextPathElement(currentElement) - currentElement) == 0)
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
            FCB = 0;
            *pParentFCB = NULL;
            *pFCB = NULL;

            return STATUS_OBJECT_PATH_NOT_FOUND;
        }

        parentFCB = FCB;

        /* Extract next directory level into dirName */
        NtfsWSubString(pathName,
                       pFileName,
                       NtfsGetNextPathElement(currentElement) - pFileName);
        DPRINT("  pathName:%S\n", pathName);

        FCB = NtfsGrabFCBFromTable(Vcb, pathName);
        if (FCB == NULL)
        {
            NtfsWSubString(elementName,
                           currentElement,
                           NtfsGetNextPathElement(currentElement) - currentElement);
            DPRINT("  elementName:%S\n", elementName);

            Status = NtfsDirFindFile(Vcb, parentFCB, elementName, CaseSensitive, &FCB);
            if (!NT_SUCCESS(Status))
            {
                DPRINT1("NtfsDirFindFile('%S' in MFT %I64u '%S') failed: 0x%lx\n",
                        elementName, parentFCB->MFTIndex, parentFCB->ObjectName, Status);
            }
            if (Status == STATUS_OBJECT_NAME_NOT_FOUND)
            {
                *pParentFCB = parentFCB;
                *pFCB = NULL;
                currentElement = NtfsGetNextPathElement(currentElement);
                if (*currentElement == L'\0' || NtfsGetNextPathElement(currentElement + 1) == 0)
                {
                    return STATUS_OBJECT_NAME_NOT_FOUND;
                }
                else
                {
                    return STATUS_OBJECT_PATH_NOT_FOUND;
                }
            }
            else if (!NT_SUCCESS(Status))
            {
                NtfsReleaseFCB(Vcb, parentFCB);
                *pParentFCB = NULL;
                *pFCB = NULL;

                return Status;
            }
        }

        currentElement = NtfsGetNextPathElement(currentElement);
    }

    *pParentFCB = parentFCB;
    *pFCB = FCB;
#endif

    return STATUS_SUCCESS;
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
