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
 * FILE:             drivers/filesystem/ntfs/ntfs.c
 * PURPOSE:          NTFS filesystem driver
 * PROGRAMMER:       Eric Kohl
 *                   Pierre Schweitzer
 */

/* INCLUDES *****************************************************************/

#include "ntfs.h"

#define NDEBUG
#include <debug.h>

/* GLOBALS *****************************************************************/

PNTFS_GLOBAL_DATA NtfsGlobalData = NULL;

/* FUNCTIONS ****************************************************************/

/*
 * Filter callback used by the memory manager when it needs to acquire a
 * file before creating a section, or to query whether the file currently
 * has any writers.  Mirrors FatFilterCallbackAcquireForCreateSection so
 * NTFS-backed images can be mapped under the same locking discipline as
 * FAT-backed images.
 *
 * For SyncTypeCreateSection: returns STATUS_FILE_LOCKED_WITH_WRITERS if
 * any handle currently has write access, otherwise
 * STATUS_FILE_LOCKED_WITH_ONLY_READERS.  For other sync types it returns
 * STATUS_FSFILTER_OP_COMPLETED_SUCCESSFULLY.
 *
 * The default FsRtl release routine releases Fcb->RFCB.Resource so we
 * acquire that resource here (matches FAT).
 *
 * Note for issue #11: this callback must be paired with the NTFS
 * FastIoDispatch.AcquireFileForNtCreateSection / ReleaseFileForNtCreateSection
 * handlers below.  Without them, FsRtlReleaseFile's default release path
 * would call ExReleaseResourceLite(FcbHeader->Resource) **unconditionally**
 * — even for FileObjects whose FsContext is an NTFS volume/global-data
 * structure (not a real FCB), where we *didn't* acquire here.  On such
 * objects the FSRTL header offset points at uninitialized or unrelated
 * memory, so the default release reads garbage at Resource and eventually
 * leaks / bugchecks.  Providing typed Acquire/Release handlers keeps all
 * releases bounded to real NTFS_TYPE_FCB objects and fixes the smss boot
 * deadlock documented in issue #11. */
NTSTATUS
NTAPI
NtfsFilterCallbackAcquireForCreateSection(IN PFS_FILTER_CALLBACK_DATA CallbackData,
                                          OUT PVOID *CompletionContext)
{
    PNTFS_FCB Fcb;

    PAGED_CODE();

    ASSERT(CallbackData->Operation == FS_FILTER_ACQUIRE_FOR_SECTION_SYNCHRONIZATION);
    ASSERT(CallbackData->SizeOfFsFilterCallbackData == sizeof(FS_FILTER_CALLBACK_DATA));

    UNREFERENCED_PARAMETER(CompletionContext);

    Fcb = CallbackData->FileObject->FsContext;
    if (Fcb == NULL)
    {
        return STATUS_FSFILTER_OP_COMPLETED_SUCCESSFULLY;
    }

    /* Volume opens have no per-file resource — let MM proceed.  The paired
     * FastIoDispatch.ReleaseFileForNtCreateSection handler below will also
     * skip releasing in this case, so the acquire/release stays balanced. */
    if (Fcb->Identifier.Type != NTFS_TYPE_FCB)
    {
        return STATUS_FSFILTER_OP_COMPLETED_SUCCESSFULLY;
    }

    /* Acquire the FCB's main resource exclusively, matching FAT.
     * The default FsRtl release routine drops Fcb->RFCB.Resource. */
    if (Fcb->RFCB.Resource != NULL)
    {
        ExAcquireResourceExclusiveLite(Fcb->RFCB.Resource, TRUE);
    }

    if (CallbackData->Parameters.AcquireForSectionSynchronization.SyncType != SyncTypeCreateSection)
    {
        return STATUS_FSFILTER_OP_COMPLETED_SUCCESSFULLY;
    }

    if (Fcb->ShareAccess.Writers == 0)
    {
        return STATUS_FILE_LOCKED_WITH_ONLY_READERS;
    }

    return STATUS_FILE_LOCKED_WITH_WRITERS;
}

/* FastIoDispatch.AcquireFileForNtCreateSection handler.
 *
 * Called by FsRtlAcquireFileExclusiveCommon when the filter callback
 * returns an unrecognized status code (the fall-through path).  For the
 * normal section-creation path this is not reached — the filter callback
 * returns STATUS_FILE_LOCKED_WITH_*. We still provide a typed handler
 * so any fallback acquire is bounded to real NTFS_TYPE_FCB objects and
 * stays symmetric with NtfsReleaseFileForNtCreateSection.
 *
 * See issue #11 for why a typed handler is required — without it the
 * default release path touches FcbHeader->Resource for non-FCB FileObjects
 * and corrupts accounting. */
VOID
NTAPI
NtfsAcquireFileForNtCreateSection(IN PFILE_OBJECT FileObject)
{
    PNTFS_FCB Fcb = FileObject->FsContext;

    if (Fcb == NULL || Fcb->Identifier.Type != NTFS_TYPE_FCB)
        return;

    if (Fcb->RFCB.Resource != NULL)
    {
        ExAcquireResourceExclusiveLite(Fcb->RFCB.Resource, TRUE);
    }
}

/* FastIoDispatch.ReleaseFileForNtCreateSection handler.
 *
 * FsRtlReleaseFile always calls this handler when registered, regardless
 * of how the matching acquire went (filter callback, default, or
 * AcquireFileForNtCreateSection).  Pairing this with the filter callback
 * and the acquire handler above guarantees that releases are only
 * performed on NTFS_TYPE_FCB objects whose RFCB.Resource is initialized
 * — which is what the filter callback check above guards on the acquire
 * side.  Without this typed handler, FsRtlReleaseFile's default path
 * would release on every FileObject FsContext (including NTFS volumes
 * and the driver's global-data device extension), reading Resource from
 * an offset in a differently-typed struct.  Issue #11 smss boot deadlock. */
VOID
NTAPI
NtfsReleaseFileForNtCreateSection(IN PFILE_OBJECT FileObject)
{
    PNTFS_FCB Fcb = FileObject->FsContext;

    if (Fcb == NULL || Fcb->Identifier.Type != NTFS_TYPE_FCB)
        return;

    if (Fcb->RFCB.Resource != NULL)
    {
        ExReleaseResourceLite(Fcb->RFCB.Resource);
    }
}


/*
 * FUNCTION: Called by the system to initialize the driver
 * ARGUMENTS:
 *           DriverObject = object describing this driver
 *           RegistryPath = path to our configuration entries
 * RETURNS: Success or failure
 */
CODE_SEG("INIT")
NTSTATUS
NTAPI
DriverEntry(PDRIVER_OBJECT DriverObject,
            PUNICODE_STRING RegistryPath)
{
    UNICODE_STRING DeviceName = RTL_CONSTANT_STRING(DEVICE_NAME);
    NTSTATUS Status;
    PDEVICE_OBJECT DeviceObject;
    OBJECT_ATTRIBUTES Attributes;
    HANDLE DriverKey = NULL;

    TRACE_(NTFS, "DriverEntry(%p, '%wZ')\n", DriverObject, RegistryPath);

    Status = IoCreateDevice(DriverObject,
                            sizeof(NTFS_GLOBAL_DATA),
                            &DeviceName,
                            FILE_DEVICE_DISK_FILE_SYSTEM,
                            0,
                            FALSE,
                            &DeviceObject);
    if (!NT_SUCCESS(Status))
    {
        WARN_(NTFS, "IoCreateDevice failed with status: %lx\n", Status);
        return Status;
    }

    /* Initialize global data */
    NtfsGlobalData = DeviceObject->DeviceExtension;
    RtlZeroMemory(NtfsGlobalData, sizeof(NTFS_GLOBAL_DATA));

    NtfsGlobalData->DeviceObject = DeviceObject;
    NtfsGlobalData->Identifier.Type = NTFS_TYPE_GLOBAL_DATA;
    NtfsGlobalData->Identifier.Size = sizeof(NTFS_GLOBAL_DATA);

    ExInitializeResourceLite(&NtfsGlobalData->Resource);

    NtfsGlobalData->EnableWriteSupport = TRUE;

    // Read registry to determine if write support should be enabled
    InitializeObjectAttributes(&Attributes,
                               RegistryPath,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               NULL,
                               NULL);

    Status = ZwOpenKey(&DriverKey, KEY_READ, &Attributes);
    if (NT_SUCCESS(Status))
    {
        UNICODE_STRING ValueName;
        UCHAR Buffer[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(ULONG)];
        PKEY_VALUE_PARTIAL_INFORMATION Value = (PKEY_VALUE_PARTIAL_INFORMATION)Buffer;
        ULONG ValueLength = sizeof(Buffer);
        ULONG ResultLength;

        RtlInitUnicodeString(&ValueName, L"MyDataDoesNotMatterSoEnableExperimentalWriteSupportForEveryNTFSVolume");

        Status = ZwQueryValueKey(DriverKey,
                                 &ValueName,
                                 KeyValuePartialInformation,
                                 Value,
                                 ValueLength,
                                 &ResultLength);

        if (NT_SUCCESS(Status) && Value->Data[0] == TRUE)
        {
            DPRINT1("\tEnabling write support on ALL NTFS volumes!\n");
            NtfsGlobalData->EnableWriteSupport = TRUE;
        }

        ZwClose(DriverKey);
    }

    /* Keep trace of Driver Object */
    NtfsGlobalData->DriverObject = DriverObject;

    /* Initialize IRP functions array */
    NtfsInitializeFunctionPointers(DriverObject);

    /* Initialize CC functions array */
    NtfsGlobalData->CacheMgrCallbacks.AcquireForLazyWrite = NtfsAcqLazyWrite;
    NtfsGlobalData->CacheMgrCallbacks.ReleaseFromLazyWrite = NtfsRelLazyWrite;
    NtfsGlobalData->CacheMgrCallbacks.AcquireForReadAhead = NtfsAcqReadAhead;
    NtfsGlobalData->CacheMgrCallbacks.ReleaseFromReadAhead = NtfsRelReadAhead;

    NtfsGlobalData->FastIoDispatch.SizeOfFastIoDispatch = sizeof(FAST_IO_DISPATCH);
    NtfsGlobalData->FastIoDispatch.FastIoCheckIfPossible = NtfsFastIoCheckIfPossible;
    NtfsGlobalData->FastIoDispatch.FastIoRead = NtfsFastIoRead;
    NtfsGlobalData->FastIoDispatch.FastIoWrite = NtfsFastIoWrite;
    /* Issue #11: typed Acquire/Release handlers for NtCreateSection.
     * Required to pair with the PreAcquireForSectionSynchronization filter
     * callback below.  Without them, FsRtlReleaseFile falls through to a
     * generic ExReleaseResourceLite(FcbHeader->Resource) that doesn't
     * check FsContext type, which reads the Resource slot out of NTFS
     * volume/global-data structures where it isn't valid.  See comments
     * on Ntfs{Acquire,Release}FileForNtCreateSection above. */
    NtfsGlobalData->FastIoDispatch.AcquireFileForNtCreateSection = NtfsAcquireFileForNtCreateSection;
    NtfsGlobalData->FastIoDispatch.ReleaseFileForNtCreateSection = NtfsReleaseFileForNtCreateSection;
    DriverObject->FastIoDispatch = &NtfsGlobalData->FastIoDispatch;

    /* Register the FS filter callback (PreAcquireForSectionSynchronization)
     * so MM can coordinate section creation with open writers.  Mirrors
     * FAT.  Must be paired with the typed FastIoDispatch handlers above:
     * re-enabling this block without them reproduces the issue #11 smss
     * boot deadlock. */
    {
        FS_FILTER_CALLBACKS FilterCallbacks;

        RtlZeroMemory(&FilterCallbacks, sizeof(FilterCallbacks));
        FilterCallbacks.SizeOfFsFilterCallbacks = sizeof(FilterCallbacks);
        FilterCallbacks.PreAcquireForSectionSynchronization = NtfsFilterCallbackAcquireForCreateSection;

        Status = FsRtlRegisterFileSystemFilterCallbacks(DriverObject, &FilterCallbacks);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("NTFS: FsRtlRegisterFileSystemFilterCallbacks failed: 0x%lx\n", Status);
            /* Non-fatal; continue without MM section sync. */
            Status = STATUS_SUCCESS;
        }
    }

    /* Initialize lookaside list for IRP contexts */
    ExInitializeNPagedLookasideList(&NtfsGlobalData->IrpContextLookasideList,
                                    NULL, NULL, 0, sizeof(NTFS_IRP_CONTEXT), TAG_IRP_CTXT, 0);
    /* Initialize lookaside list for FCBs */
    ExInitializeNPagedLookasideList(&NtfsGlobalData->FcbLookasideList,
                                    NULL, NULL, 0, sizeof(NTFS_FCB), TAG_FCB, 0);
    /* Initialize lookaside list for attributes contexts */
    ExInitializeNPagedLookasideList(&NtfsGlobalData->AttrCtxtLookasideList,
                                    NULL, NULL, 0, sizeof(NTFS_ATTR_CONTEXT), TAG_ATT_CTXT, 0);

    /* Zombie FCB list (see fcb.c NtfsDestroyFCB / NtfsReapZombieFcbs). */
    InitializeListHead(&NtfsGlobalData->ZombieFcbList);
    KeInitializeSpinLock(&NtfsGlobalData->ZombieLock);

    /* Driver can't be unloaded */
    DriverObject->DriverUnload = NULL;

    NtfsGlobalData->DeviceObject->Flags |= DO_DIRECT_IO;

    /* Register file system */
    IoRegisterFileSystem(NtfsGlobalData->DeviceObject);
    ObReferenceObject(NtfsGlobalData->DeviceObject);

    return STATUS_SUCCESS;
}


/*
 * FUNCTION: Called within the driver entry to initialize the IRP functions array
 * ARGUMENTS:
 *           DriverObject = object describing this driver
 * RETURNS: Nothing
 */
CODE_SEG("INIT")
VOID
NTAPI
NtfsInitializeFunctionPointers(PDRIVER_OBJECT DriverObject)
{
    DriverObject->MajorFunction[IRP_MJ_CREATE]                   = NtfsFsdDispatch;
    DriverObject->MajorFunction[IRP_MJ_CLOSE]                    = NtfsFsdDispatch;
    DriverObject->MajorFunction[IRP_MJ_CLEANUP]                  = NtfsFsdDispatch;
    DriverObject->MajorFunction[IRP_MJ_READ]                     = NtfsFsdDispatch;
    DriverObject->MajorFunction[IRP_MJ_WRITE]                    = NtfsFsdDispatch;
    DriverObject->MajorFunction[IRP_MJ_QUERY_INFORMATION]        = NtfsFsdDispatch;
    DriverObject->MajorFunction[IRP_MJ_SET_INFORMATION]          = NtfsFsdDispatch;
    DriverObject->MajorFunction[IRP_MJ_QUERY_VOLUME_INFORMATION] = NtfsFsdDispatch;
    DriverObject->MajorFunction[IRP_MJ_SET_VOLUME_INFORMATION]   = NtfsFsdDispatch;
    DriverObject->MajorFunction[IRP_MJ_DIRECTORY_CONTROL]        = NtfsFsdDispatch;
    DriverObject->MajorFunction[IRP_MJ_FLUSH_BUFFERS]             = NtfsFsdDispatch;
    DriverObject->MajorFunction[IRP_MJ_FILE_SYSTEM_CONTROL]      = NtfsFsdDispatch;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL]           = NtfsFsdDispatch;
    DriverObject->MajorFunction[IRP_MJ_QUERY_EA]                 = NtfsFsdDispatch;
    DriverObject->MajorFunction[IRP_MJ_SET_EA]                   = NtfsFsdDispatch;
    DriverObject->MajorFunction[IRP_MJ_LOCK_CONTROL]             = NtfsFsdDispatch;
    DriverObject->MajorFunction[IRP_MJ_SHUTDOWN]                 = NtfsFsdDispatch;
    DriverObject->MajorFunction[IRP_MJ_PNP]                      = NtfsFsdDispatch;
    DriverObject->MajorFunction[IRP_MJ_QUERY_SECURITY]           = NtfsFsdDispatch;
    DriverObject->MajorFunction[IRP_MJ_SET_SECURITY]             = NtfsFsdDispatch;

    return;
}

/* EOF */
