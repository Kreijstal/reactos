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
BOOLEAN
NtfsWSubString(PWCHAR pTarget,
               size_t pTargetCount,
               PCWSTR pSource,
               size_t pLength)
{
    /* pLength is derived from the caller-supplied path, which is not bounded
     * by the size of the target buffer.  Refuse to copy rather than run off
     * the end of it: the targets are on-stack WCHAR[MAX_PATH] arrays, so an
     * overrun smashes the caller's spilled arguments. */
    if (pLength >= pTargetCount)
        return FALSE;

    wcsncpy(pTarget, pSource, pLength);
    pTarget[pLength] = L'\0';
    return TRUE;
}


/**
* @name NtfsParseStreamPath
* @implemented
*
* Splits an (absolute or relative) path into its file part, an optional
* alternate-data-stream name and the typed-stream class of the open,
* validating the stream syntax like Windows.  Per [MS-FSCC] 2.1.5 only the
* $DATA and $INDEX_ALLOCATION stream types are openable:
*
*   "file"                       -> Data, no stream
*   "file:stream"                -> Data, stream "stream"
*   "file:stream:$DATA"          -> Data, stream "stream" (suffix stripped)
*   "file::$DATA"                -> DefaultData - the default (unnamed)
*                                   stream; the "::$DATA" tail is stripped so
*                                   the name matches a plain open, but the
*                                   class lets the create path fail directory
*                                   targets (STATUS_FILE_IS_A_DIRECTORY)
*   "dir::$INDEX_ALLOCATION",
*   "dir:$I30:$INDEX_ALLOCATION" -> IndexAllocation - the directory stream
*                                   ($I30 is the one directory index NTFS
*                                   has); the whole tail is stripped, making
*                                   the open equivalent to a plain "dir"
*                                   open, and the class lets the create path
*                                   fail non-directory targets
*                                   (STATUS_NOT_A_DIRECTORY)
*   "dir:x:$INDEX_ALLOCATION"    -> STATUS_OBJECT_NAME_INVALID (only the
*                                   empty name and $I30 name the index)
*   "file:"                      -> STATUS_OBJECT_NAME_INVALID
*   "file:stream:$BAD"           -> STATUS_OBJECT_NAME_INVALID (any other
*                                   type - $BITMAP, $EA, $ATTRIBUTE_LIST,
*                                   $REPARSE_POINT, ... - is not openable)
*   "file:a:b:c"                 -> STATUS_OBJECT_NAME_INVALID
*   "di:r\\file"                 -> STATUS_OBJECT_NAME_INVALID (a colon may
*                                   only appear in the last path component)
*
* Normalizes FileName in place: any type suffix (and, for the default data
* stream and the directory stream, the entire ":...:$TYPE" tail) is removed
* so every downstream consumer - the FCB table key, the directory walk, the
* create path - sees at most one colon, separating the file part from the
* stream name.  The name only ever shrinks, and the buffer is
* re-NUL-terminated at the new end, preserving the driver-wide
* "FileName.Buffer is a C string" invariant.
*
* @param FileName
* The path to parse/normalize.  Buffer must be writable (both the I/O
* manager's FileObject->FileName and the local element buffers used by the
* directory walk are).
*
* @param StreamName
* Receives the stream name (pointing into FileName->Buffer, NUL-terminated).
* Length == 0 means "no stream / default stream / directory stream".
*
* @param StreamType
* Receives the typed-stream class (see NTFS_STREAM_TYPE in ntfs.h).
*
* @return
* STATUS_SUCCESS or STATUS_OBJECT_NAME_INVALID.
*/
NTSTATUS
NtfsParseStreamPath(PUNICODE_STRING FileName,
                    PUNICODE_STRING StreamName,
                    PNTFS_STREAM_TYPE StreamType)
{
    ULONG Chars = FileName->Length / sizeof(WCHAR);
    ULONG FirstColon = (ULONG)-1;
    ULONG SecondColon = (ULONG)-1;
    ULONG i;
    UNICODE_STRING Type;

    RtlInitEmptyUnicodeString(StreamName, NULL, 0);
    *StreamType = NtfsStreamTypeData;

    for (i = 0; i < Chars; i++)
    {
        if (FileName->Buffer[i] == L'\\')
        {
            /* A colon may only appear in the last path component. */
            if (FirstColon != (ULONG)-1)
                return STATUS_OBJECT_NAME_INVALID;
        }
        else if (FileName->Buffer[i] == L':')
        {
            if (FirstColon == (ULONG)-1)
                FirstColon = i;
            else if (SecondColon == (ULONG)-1)
                SecondColon = i;
            else
                return STATUS_OBJECT_NAME_INVALID;
        }
    }

    /* No stream suffix at all - the common case. */
    if (FirstColon == (ULONG)-1)
        return STATUS_SUCCESS;

    if (SecondColon != (ULONG)-1)
    {
        UNICODE_STRING DataTypeName = RTL_CONSTANT_STRING(L"$DATA");
        UNICODE_STRING IndexTypeName = RTL_CONSTANT_STRING(L"$INDEX_ALLOCATION");

        Type.Buffer = &FileName->Buffer[SecondColon + 1];
        Type.Length = (USHORT)((Chars - SecondColon - 1) * sizeof(WCHAR));
        Type.MaximumLength = Type.Length;

        if (RtlEqualUnicodeString(&Type, &IndexTypeName, TRUE))
        {
            UNICODE_STRING IndexName = RTL_CONSTANT_STRING(L"$I30");
            UNICODE_STRING Name;

            /* "$INDEX_ALLOCATION" opens the directory index stream; only
             * the empty name and "$I30" (the one index a directory has)
             * may name it - anything else is invalid. */
            Name.Buffer = &FileName->Buffer[FirstColon + 1];
            Name.Length = (USHORT)((SecondColon - FirstColon - 1) * sizeof(WCHAR));
            Name.MaximumLength = Name.Length;

            if (Name.Length != 0 &&
                !RtlEqualUnicodeString(&Name, &IndexName, TRUE))
            {
                return STATUS_OBJECT_NAME_INVALID;
            }

            /* Strip the whole tail: the open is equivalent to a plain open
             * of the directory (same FCB); the class tells the create path
             * to demand a directory target. */
            FileName->Length = (USHORT)(FirstColon * sizeof(WCHAR));
            FileName->Buffer[FirstColon] = UNICODE_NULL;
            *StreamType = NtfsStreamTypeIndexAllocation;
            return STATUS_SUCCESS;
        }

        /* Anything else must be $DATA; this also rejects the empty type of
         * "file:stream:" and the stream types that are never openable
         * ($BITMAP, $EA, $ATTRIBUTE_LIST, $REPARSE_POINT, ...). */
        if (!RtlEqualUnicodeString(&Type, &DataTypeName, TRUE))
            return STATUS_OBJECT_NAME_INVALID;

        /* Strip the type suffix: "file:stream:$DATA" -> "file:stream". */
        FileName->Length = (USHORT)(SecondColon * sizeof(WCHAR));
        FileName->Buffer[SecondColon] = UNICODE_NULL;
        Chars = SecondColon;
    }

    StreamName->Buffer = &FileName->Buffer[FirstColon + 1];
    StreamName->Length = (USHORT)((Chars - FirstColon - 1) * sizeof(WCHAR));
    StreamName->MaximumLength = StreamName->Length;

    if (StreamName->Length == 0)
    {
        /* "file:" (no type) is bad syntax. */
        if (SecondColon == (ULONG)-1)
            return STATUS_OBJECT_NAME_INVALID;

        /* "file::$DATA" is the default (unnamed) data stream - the very
         * same object as a plain open of "file".  Normalize the colon away
         * so both spellings share one FCB, but report the explicit class:
         * unlike a plain open, "dir::$DATA" must fail on a directory
         * (STATUS_FILE_IS_A_DIRECTORY - a directory has no unnamed $DATA). */
        FileName->Length = (USHORT)(FirstColon * sizeof(WCHAR));
        FileName->Buffer[FirstColon] = UNICODE_NULL;
        StreamName->Buffer = NULL;
        *StreamType = NtfsStreamTypeDefaultData;
        return STATUS_SUCCESS;
    }

    /* NTFS attribute names are at most 255 characters, and wildcards are
     * never valid in a stream name. */
    if (StreamName->Length > 255 * sizeof(WCHAR) ||
        FsRtlDoesNameContainWildCards(StreamName))
    {
        return STATUS_OBJECT_NAME_INVALID;
    }

    return STATUS_SUCCESS;
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
    Fcb->PathNameHash = NtfsComputePathNameHash(Fcb->PathName);

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
     * optional and we don't need them - FsRtlProcessFileLock and the
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
 * dies - see ntoskrnl/mm/section.c).  Called opportunistically from
 * NtfsCreateFCB and NtfsDestroyFCB so we don't need a dedicated worker
 * thread; the cost is one short critical-section walk per FCB churn,
 * which is negligible compared to the I/O the same code path is doing.
 *
 * Runs at PASSIVE/APC level - same constraints as the call sites. */
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

    /* Free outside the lock - ExDeleteResourceLite / ExFreePool can be
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
         * refuse on account of them.  Ignore the return value - it returns
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
            BOOLEAN FlushedClean = TRUE;

            if (tmpFileObject->SectionObjectPointer->SharedCacheMap)
            {
                IO_STATUS_BLOCK Iosb;
                ULONG FlushRetry = 0;

                /* CcFlushCache stops at the first VACB whose flush fails and
                 * CcPurgeCacheSection then DISCARDS every remaining dirty VACB
                 * without writing it (ntoskrnl/cc/fs.c).  Purging after a failed
                 * flush therefore drops this file's still-dirty data pages,
                 * leaving the on-disk file at its old/garbage content while its
                 * $MFT record (size / VDL / runlist) already describes the new
                 * data - exactly the "metadata perfect, content garbage" file
                 * corruption seen after an upgrade.  A transient flush failure
                 * must not become permanent data loss: retry the flush, and only
                 * purge once it has actually drained. */
                do
                {
                    Iosb.Status = STATUS_SUCCESS;
                    CcFlushCache(tmpFileObject->SectionObjectPointer, NULL, 0, &Iosb);
                    if (NT_SUCCESS(Iosb.Status))
                        break;

                    DPRINT1("NtfsReleaseFCB: teardown flush failed (0x%08lx) mft=%I64u, retry %lu\n",
                            Iosb.Status, Fcb->MFTIndex, FlushRetry + 1);
                }
                while (++FlushRetry < 100);

                FlushedClean = NT_SUCCESS(Iosb.Status);
            }

            /* Only purge when the data is durably on disk.  If the flush could
             * not be completed (a genuine device error), leave the dirty pages
             * in the cache rather than discarding them: dropping them here is
             * data loss / corruption, not a clean teardown. */
            if (FlushedClean)
                CcPurgeCacheSection(tmpFileObject->SectionObjectPointer, NULL, 0, FALSE);
            else
                DPRINT1("NtfsReleaseFCB: NOT purging dirty data for mft=%I64u after flush failure\n",
                        Fcb->MFTIndex);
        }

        CcUninitializeCacheMap(tmpFileObject, NULL, NULL);
        /* Do NOT ObDereferenceObject(tmpFileObject) here.
         * NtfsAttachFCBToFileObject already dropped its reference (line 395).
         * The remaining references belong to the cache manager and the MM
         * section segment - they will be released by CcUninitializeCacheMap
         * and MmDereferenceSegmentWithLock respectively. An extra deref here
         * causes a use-after-free: the FileObject is freed while the MM
         * segment still holds a pointer to it. */
    }
    else if (Fcb->RefCount <= 0 &&
             !BooleanFlagOn(Fcb->Flags, FCB_IS_VOLUME | FCB_IS_VOLUME_STREAM) &&
             (!NtfsFCBIsDirectory(Fcb) || Fcb->LinkCount == 0))
    {
        /* Non-directory FCBs are evicted as soon as the last reference goes
         * away. Live directory FCBs are intentionally kept cached in the
         * table, but a *deleted* directory (LinkCount == 0, set by
         * NtfsDeleteFileRecord) must be evicted too - otherwise its stale FCB
         * shadows the name and a later create at the same path wrongly sees
         * STATUS_OBJECT_NAME_COLLISION even though the on-disk entry is gone.
         *
         * The volume FCB and the internal volume-stream FCB are persistent
         * singletons (DeviceExt->VolumeFcb / Vcb->StreamFileObject's FCB) that
         * live for the whole mount and are never inserted into FcbListHead, so
         * they must never take this path: RemoveEntryList below would walk a
         * NULL Flink, and NtfsDestroyFCB would leave DeviceExt->VolumeFcb
         * dangling.  Their lifetime is owned by mount/dismount, not RefCount -
         * a bare volume open/close cycle legitimately drives RefCount 0->1->0.
         * NtfsCleanupFile already special-cases the same two flags. */
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


/* FNV-1a over PathName with the same case folding as the kernel's
 * _wcsicmp (ASCII A-Z only, see sdk/lib/crt/wstring/_wcsicmp_nt.c).
 * Any wider folding (e.g. RtlUpcaseUnicodeChar) would let two strings
 * hash differently although _wcsicmp calls them equal, and
 * NtfsGrabFCBFromTable would then miss an existing FCB and create a
 * duplicate. */
ULONG
NtfsComputePathNameHash(PCWSTR Path)
{
    ULONG Hash = 2166136261u;
    WCHAR c;

    while ((c = *Path++) != UNICODE_NULL)
    {
        if (c >= L'A' && c <= L'Z')
            c += (L'a' - L'A');
        Hash = (Hash ^ c) * 16777619u;
    }

    return Hash;
}


VOID
NtfsAddFCBToTable(PNTFS_VCB Vcb,
                  PNTFS_FCB Fcb)
{
    KIRQL oldIrql;

    /* Catch any PathName writer that forgot to refresh the hash */
    ASSERT(Fcb->PathNameHash == NtfsComputePathNameHash(Fcb->PathName));

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
    ULONG NameHash;

    if (FileName == NULL || *FileName == 0)
    {
        DPRINT("Return FCB for stream file object\n");
        KeAcquireSpinLock(&Vcb->FcbListLock, &oldIrql);
        Fcb = Vcb->StreamFileObject->FsContext;
        Fcb->RefCount++;
        KeReleaseSpinLock(&Vcb->FcbListLock, oldIrql);
        return Fcb;
    }

    NameHash = NtfsComputePathNameHash(FileName);

    KeAcquireSpinLock(&Vcb->FcbListLock, &oldIrql);

    current_entry = Vcb->FcbListHead.Flink;
    while (current_entry != &Vcb->FcbListHead)
    {
        Fcb = CONTAINING_RECORD(current_entry, NTFS_FCB, FcbListEntry);

        if (Fcb->PathNameHash == NameHash &&
            _wcsicmp(FileName, Fcb->PathName) == 0)
        {
            /* A fully deleted file (its last name removed and MFT record freed
             * by NtfsDeleteFileRecord, which zeroes LinkCount) may still linger
             * in the table while a cache/section reference keeps it from being
             * evicted.  It no longer has a name on disk, so it must not satisfy
             * a lookup by path: returning it would let a fresh open of the same
             * name (e.g. an installer re-creating a file it just unlinked) reuse
             * the freed record instead of creating a new one.  Keep scanning -
             * a live FCB for the same path, if any, appears elsewhere in the
             * list. */
            if (Fcb->LinkCount == 0)
            {
                current_entry = current_entry->Flink;
                continue;
            }

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


/* Drop the cached FCB that used to own a name which has just been unlinked by
 * a rename with ReplaceIfExists.
 *
 * NtfsRenameFileRecord deletes the replaced file through a *stack-local*
 * NTFS_FCB, so NtfsDeleteFileRecord zeroes the LinkCount of a throwaway copy.
 * The FCB that is actually cached for that path keeps LinkCount != 0 and still
 * describes the file that is now gone - its MFT index, its FileSize and its
 * timestamps.  Nothing evicts it either, because a non-directory FCB only
 * leaves the table once its RefCount reaches 0 and the cache-teardown branch
 * of NtfsReleaseFCB leaves it there at RefCount 1.  The next open of that path
 * therefore matches the zombie in NtfsGrabFCBFromTable and the caller sees the
 * REPLACED file's length: every read stops short of the new content, and once
 * the freed clusters get reused the tail of the old file shows up spliced onto
 * the new one.  That is how `patch` could rewrite a 4377-byte configure.ac to
 * 4453 bytes, report success, and leave every reader seeing 4377 bytes.
 *
 * Mark it unnamed instead.  LinkCount == 0 is the convention that
 * NtfsGrabFCBFromTable already skips for deleted files, so lookups fall
 * through to the on-disk entry while any handle still open on the old file
 * keeps working; the FCB is freed by the normal refcount teardown.
 *
 * MftIndex is matched as well as the path so that an FCB legitimately holding
 * this name - the source of the rename itself, once it has been repointed -
 * is left alone. */
VOID
NtfsInvalidateFCBForPath(PNTFS_VCB Vcb,
                         PCWSTR PathName,
                         ULONGLONG MftIndex)
{
    KIRQL oldIrql;
    PNTFS_FCB Fcb;
    PLIST_ENTRY current_entry;
    ULONG NameHash;

    if (PathName == NULL || *PathName == UNICODE_NULL)
        return;

    NameHash = NtfsComputePathNameHash(PathName);

    KeAcquireSpinLock(&Vcb->FcbListLock, &oldIrql);

    for (current_entry = Vcb->FcbListHead.Flink;
         current_entry != &Vcb->FcbListHead;
         current_entry = current_entry->Flink)
    {
        Fcb = CONTAINING_RECORD(current_entry, NTFS_FCB, FcbListEntry);

        if (Fcb->MFTIndex == MftIndex &&
            Fcb->PathNameHash == NameHash &&
            _wcsicmp(PathName, Fcb->PathName) == 0)
        {
            DPRINT("Invalidating replaced FCB %p (%S, mft=%I64u)\n",
                   Fcb, Fcb->PathName, Fcb->MFTIndex);
            Fcb->LinkCount = 0;
        }
    }

    KeReleaseSpinLock(&Vcb->FcbListLock, oldIrql);
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

    /* Pin the FCB for the lifetime of this stream FileObject.  The stream FO
     * carries the cache map, and once the memory manager creates a data
     * section for it, MM holds a reference on it via Segment->FileObject and
     * reaches this FCB's separately-allocated SectionObjectPointers through
     * FileObject->SectionObjectPointer.  Without a matching FCB reference the
     * FCB (with its PagingIoResource) and its SectionObjectPointers get freed
     * when the last *external* handle closes - while MM still references the
     * stream FO - producing a use-after-free (MiGrabDataSection dereferences a
     * recycled DataSectionObject; the freed SectionObjectPointers block gets a
     * stale write, corrupting the NonPagedPool free list).  The reference is
     * dropped in NtfsCloseFile when the stream FO is finally closed, which the
     * I/O manager defers until MM drops its ObReference (ntoskrnl/mm/section.c).
     * This mirrors how external FILE_OBJECTs pin the FCB via
     * NTFS_CCB_FLAG_COUNTED.  Directories keep their FCBs cached explicitly and
     * never carry a data section, so they are excluded to preserve deleted-
     * directory eviction in NtfsReleaseFCB. */
    if (!NtfsFCBIsDirectory(Fcb))
    {
        NtfsGrabFCB(Vcb, Fcb);
        newCCB->Flags |= NTFS_CCB_FLAG_COUNTED;
    }

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

    FileName = GetFileNameFromRecord(Vcb, MftRecord, NTFS_FILE_NAME_WIN32, NULL);
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
        /* $STANDARD_INFORMATION carries the authoritative timestamps; the
         * $FILE_NAME duplicates are only lazily maintained (as on Windows) */
        Fcb->Entry.CreationTime = StdInfo->CreationTime;
        Fcb->Entry.ChangeTime = StdInfo->ChangeTime;
        Fcb->Entry.LastWriteTime = StdInfo->LastWriteTime;
        Fcb->Entry.LastAccessTime = StdInfo->LastAccessTime;
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
    UCHAR SpillNameBuf[NTFS_FOUND_NAME_SIZE];
    PSTANDARD_INFORMATION StdInfo;
    PNTFS_FCB rcFCB;
    ULONGLONG Size, AllocatedSize;

    DPRINT("NtfsMakeFCBFromDirEntry(%p, %p, %wZ, %p, %p, %p)\n", Vcb, DirectoryFCB, Name, Stream, Record, fileFCB);

    FileName = GetBestFileNameFromRecord(Vcb, Record, (PFILENAME_ATTRIBUTE)SpillNameBuf);
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
        /* $STANDARD_INFORMATION carries the authoritative timestamps; the
         * $FILE_NAME duplicates are only lazily maintained (as on Windows) */
        rcFCB->Entry.CreationTime = StdInfo->CreationTime;
        rcFCB->Entry.ChangeTime = StdInfo->ChangeTime;
        rcFCB->Entry.LastWriteTime = StdInfo->LastWriteTime;
        rcFCB->Entry.LastAccessTime = StdInfo->LastAccessTime;
    }

    /* Initialise RefCount before priming the cache: NtfsFCBInitializeCache
     * takes a reference on behalf of the stream FileObject (see there), which
     * must not be clobbered by this assignment. */
    rcFCB->RefCount = 1;
    NtfsFCBInitializeCache(Vcb, rcFCB);
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

    if (!NtfsFCBIsDirectory(Fcb) &&
        !(Fcb->Flags & FCB_CACHE_INITIALIZED))
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
    UNICODE_STRING BaseName;
    UNICODE_STRING Stream;
    NTFS_STREAM_TYPE StreamType;
    PFILE_RECORD_HEADER FileRecord;
    ULONGLONG MFTIndex;
    PNTFS_ATTR_CONTEXT DataContext;

    DPRINT("NtfsDirFindFile(%p, %p, %S, %s, %p)\n",
           Vcb,
           DirectoryFcb,
           FileToFind,
           CaseSensitive ? "TRUE" : "FALSE",
           FoundFCB);

    *FoundFCB = NULL;
    RtlInitUnicodeString(&File, FileToFind);
    CurrentDir = DirectoryFcb->MFTIndex;

    /* Split off an alternate-data-stream suffix.  The create path already
     * validated and normalized the user name, but internal callers (rename
     * target lookups, etc.) come through here directly, so parse defensively.
     * Normalization is in place: any type suffix is stripped from
     * FileToFind, which also keeps the FCB PathName canonical below. */
    Status = NtfsParseStreamPath(&File, &Stream, &StreamType);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    BaseName = File;
    if (Stream.Length != 0)
    {
        BaseName.Length = (USHORT)(File.Length - Stream.Length - sizeof(WCHAR));
        DPRINT("Will now look for file '%wZ' with stream '%wZ'\n", &BaseName, &Stream);

        /* "\dir\:stream" - a named stream on the directory itself.  NTFS
         * allows those, but this driver's directory FCBs have no data cache
         * plumbing, so directory ADS is explicitly unsupported. */
        if (BaseName.Length == 0)
        {
            return STATUS_OBJECT_NAME_INVALID;
        }
    }

    Status = NtfsLookupFileAt(Vcb, &BaseName, CaseSensitive, &FileRecord, &MFTIndex, CurrentDir);
    NTFS_TRACE_IF(CurrentDir == 27 || MFTIndex == 144, "DRVIDX: dirfind lookup returned 0x%lx file=%wZ dir=%I64u mft=%I64u\n",
                Status,
                &BaseName,
                CurrentDir,
                MFTIndex);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    /* Typed opens ([MS-FSCC] 2.1.5): "::$INDEX_ALLOCATION" names the
     * directory index stream and requires a directory target, an explicit
     * default-stream "::$DATA" open requires a non-directory (a directory
     * has no unnamed $DATA attribute).  NtfsCreateFile enforces the same
     * on the user path; internal callers land here with the raw name. */
    if (StreamType == NtfsStreamTypeIndexAllocation &&
        !(FileRecord->Flags & FRH_DIRECTORY))
    {
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, FileRecord);
        return STATUS_NOT_A_DIRECTORY;
    }

    if (StreamType == NtfsStreamTypeDefaultData &&
        (FileRecord->Flags & FRH_DIRECTORY))
    {
        ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, FileRecord);
        return STATUS_FILE_IS_A_DIRECTORY;
    }

    if (Stream.Length != 0)
    {
        if (FileRecord->Flags & FRH_DIRECTORY)
        {
            /* Named $DATA streams on directories are legal in NTFS, but the
             * directory FCB/cache plumbing here doesn't support them - fail
             * the name rather than misopen the directory. */
            ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, FileRecord);
            return STATUS_OBJECT_NAME_INVALID;
        }

        /* The stream must exist for a plain open; the create dispositions
         * handle STATUS_OBJECT_NAME_NOT_FOUND by creating the attribute
         * (NtfsCreateStream in create.c). */
        Status = FindAttribute(Vcb, FileRecord, AttributeData,
                               Stream.Buffer, Stream.Length / sizeof(WCHAR),
                               &DataContext, NULL);
        if (!NT_SUCCESS(Status))
        {
            ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, FileRecord);
            return STATUS_OBJECT_NAME_NOT_FOUND;
        }
        ReleaseAttributeContext(DataContext);
    }

    NTFS_TRACE_IF(CurrentDir == 27 || MFTIndex == 144, "DRVIDX: make fcb begin file=%wZ mft=%I64u record=%p\n",
                &File,
                MFTIndex,
                FileRecord);
    Status = NtfsMakeFCBFromDirEntry(Vcb, DirectoryFcb, &File,
                                     (Stream.Length != 0) ? Stream.Buffer : NULL,
                                     FileRecord, MFTIndex, FoundFCB);
    NTFS_TRACE_IF(CurrentDir == 27 || MFTIndex == 144, "DRVIDX: make fcb returned 0x%lx file=%wZ mft=%I64u fcb=%p\n",
                Status,
                &File,
                MFTIndex,
                FoundFCB ? *FoundFCB : NULL);
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
        if (!NtfsWSubString(pathName,
                            RTL_NUMBER_OF(pathName),
                            pFileName,
                            NtfsGetNextPathElement(currentElement) - pFileName))
        {
            DPRINT1("Path element exceeds MAX_PATH\n");

            NtfsReleaseFCB(Vcb, parentFCB);
            *pParentFCB = NULL;
            *pFCB = NULL;

            return STATUS_OBJECT_NAME_INVALID;
        }
        DPRINT("  pathName:%S\n", pathName);

        FCB = NtfsGrabFCBFromTable(Vcb, pathName);
        if (FCB == NULL)
        {
            if (!NtfsWSubString(elementName,
                                RTL_NUMBER_OF(elementName),
                                currentElement,
                                NtfsGetNextPathElement(currentElement) - currentElement))
            {
                DPRINT1("Path element exceeds MAX_PATH\n");

                NtfsReleaseFCB(Vcb, parentFCB);
                *pParentFCB = NULL;
                *pFCB = NULL;

                return STATUS_OBJECT_NAME_INVALID;
            }
            DPRINT("  elementName:%S\n", elementName);

            Status = NtfsDirFindFile(Vcb, parentFCB, elementName, CaseSensitive, &FCB);
            if (!NT_SUCCESS(Status))
            {
                DPRINT("NtfsDirFindFile('%S' in MFT %I64u '%S') failed: 0x%lx\n",
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
