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

/* Section creation and the matching release must acquire the same resource.
 * Report existing writers to MM, as required by the section synchronization
 * callback contract. Control-device file objects have no FCB. */
static
VOID
NTAPI
NtfsAcquireFileForCreateSection(PFILE_OBJECT FileObject)
{
    PNTFS_FCB Fcb = FileObject->FsContext;

    if (Fcb == NULL)
        return;
    ASSERT(Fcb->Identifier.Type == NTFS_TYPE_FCB);
    ExAcquireResourceExclusiveLite(&Fcb->MainResource, TRUE);
}

static
VOID
NTAPI
NtfsReleaseFileForCreateSection(PFILE_OBJECT FileObject)
{
    PNTFS_FCB Fcb = FileObject->FsContext;

    if (Fcb == NULL)
        return;
    ASSERT(Fcb->Identifier.Type == NTFS_TYPE_FCB);
    ExReleaseResourceLite(&Fcb->MainResource);
}

static
NTSTATUS
NTAPI
NtfsAcquireForSectionSynchronization(PFS_FILTER_CALLBACK_DATA Data,
                                     PVOID *CompletionContext)
{
    PNTFS_FCB Fcb = Data->FileObject->FsContext;

    UNREFERENCED_PARAMETER(CompletionContext);
    ASSERT(Data->Operation == FS_FILTER_ACQUIRE_FOR_SECTION_SYNCHRONIZATION);
    NtfsAcquireFileForCreateSection(Data->FileObject);
    if (Fcb == NULL ||
        Data->Parameters.AcquireForSectionSynchronization.SyncType != SyncTypeCreateSection)
    {
        return STATUS_FSFILTER_OP_COMPLETED_SUCCESSFULLY;
    }
    return Fcb->ShareAccess.Writers ? STATUS_FILE_LOCKED_WITH_WRITERS :
                                     STATUS_FILE_LOCKED_WITH_ONLY_READERS;
}

/* Paging writes take the paging resource exclusively. Acquiring it shared
 * in the default FsRtl callbacks would require an unsupported lock upgrade. */
static
NTSTATUS
NTAPI
NtfsAcquireForCcFlush(PFILE_OBJECT FileObject, PDEVICE_OBJECT DeviceObject)
{
    PNTFS_FCB Fcb = FileObject->FsContext;

    UNREFERENCED_PARAMETER(DeviceObject);
    ASSERT(Fcb->Identifier.Type == NTFS_TYPE_FCB);
    if (ExIsResourceAcquiredSharedLite(&Fcb->MainResource))
        ExAcquireResourceSharedLite(&Fcb->MainResource, TRUE);
    else
        ExAcquireResourceExclusiveLite(&Fcb->MainResource, TRUE);
    ExAcquireResourceExclusiveLite(&Fcb->PagingIoResource, TRUE);
    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
NtfsReleaseForCcFlush(PFILE_OBJECT FileObject, PDEVICE_OBJECT DeviceObject)
{
    PNTFS_FCB Fcb = FileObject->FsContext;

    UNREFERENCED_PARAMETER(DeviceObject);
    ExReleaseResourceLite(&Fcb->PagingIoResource);
    ExReleaseResourceLite(&Fcb->MainResource);
    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
NtfsAcquireForModWrite(PFILE_OBJECT FileObject,
                       PLARGE_INTEGER EndingOffset,
                       PERESOURCE *ResourceToRelease,
                       PDEVICE_OBJECT DeviceObject)
{
    PNTFS_FCB Fcb = FileObject->FsContext;

    UNREFERENCED_PARAMETER(EndingOffset);
    UNREFERENCED_PARAMETER(DeviceObject);
    ASSERT(Fcb->Identifier.Type == NTFS_TYPE_FCB);
    *ResourceToRelease = NULL;
    /* An explicit flush can be called by a reader already holding main
     * shared. Recurse in that mode instead of attempting a lock upgrade. */
    if (ExIsResourceAcquiredSharedLite(&Fcb->MainResource))
    {
        if (!ExAcquireResourceSharedLite(&Fcb->MainResource, FALSE))
            return STATUS_CANT_WAIT;
    }
    else if (!ExAcquireResourceExclusiveLite(&Fcb->MainResource, FALSE))
    {
        return STATUS_CANT_WAIT;
    }
    *ResourceToRelease = &Fcb->MainResource;
    return STATUS_SUCCESS;
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

    NtfsGlobalData->EnableWriteSupport = FALSE;

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

    {
        FS_FILTER_CALLBACKS Callbacks;

        RtlZeroMemory(&Callbacks, sizeof(Callbacks));
        Callbacks.SizeOfFsFilterCallbacks = sizeof(Callbacks);
        Callbacks.PreAcquireForSectionSynchronization = NtfsAcquireForSectionSynchronization;
        Status = FsRtlRegisterFileSystemFilterCallbacks(DriverObject, &Callbacks);
        if (!NT_SUCCESS(Status))
        {
            ExDeleteResourceLite(&NtfsGlobalData->Resource);
            IoDeleteDevice(DeviceObject);
            NtfsGlobalData = NULL;
            return Status;
        }
    }

    /* Initialize CC functions array */
    NtfsGlobalData->CacheMgrCallbacks.AcquireForLazyWrite = NtfsAcqLazyWrite;
    NtfsGlobalData->CacheMgrCallbacks.ReleaseFromLazyWrite = NtfsRelLazyWrite;
    NtfsGlobalData->CacheMgrCallbacks.AcquireForReadAhead = NtfsAcqReadAhead;
    NtfsGlobalData->CacheMgrCallbacks.ReleaseFromReadAhead = NtfsRelReadAhead;

    NtfsGlobalData->FastIoDispatch.SizeOfFastIoDispatch = sizeof(FAST_IO_DISPATCH);
    NtfsGlobalData->FastIoDispatch.FastIoCheckIfPossible = NtfsFastIoCheckIfPossible;
    NtfsGlobalData->FastIoDispatch.FastIoRead = NtfsFastIoRead;
    NtfsGlobalData->FastIoDispatch.FastIoWrite = NtfsFastIoWrite;
    NtfsGlobalData->FastIoDispatch.AcquireFileForNtCreateSection = NtfsAcquireFileForCreateSection;
    NtfsGlobalData->FastIoDispatch.ReleaseFileForNtCreateSection = NtfsReleaseFileForCreateSection;
    NtfsGlobalData->FastIoDispatch.AcquireForCcFlush = NtfsAcquireForCcFlush;
    NtfsGlobalData->FastIoDispatch.ReleaseForCcFlush = NtfsReleaseForCcFlush;
    NtfsGlobalData->FastIoDispatch.AcquireForModWrite = NtfsAcquireForModWrite;
    DriverObject->FastIoDispatch = &NtfsGlobalData->FastIoDispatch;

    /* Initialize lookaside list for IRP contexts */
    ExInitializeNPagedLookasideList(&NtfsGlobalData->IrpContextLookasideList,
                                    NULL, NULL, 0, sizeof(NTFS_IRP_CONTEXT), TAG_IRP_CTXT, 0);
    /* Initialize lookaside list for FCBs */
    ExInitializeNPagedLookasideList(&NtfsGlobalData->FcbLookasideList,
                                    NULL, NULL, 0, sizeof(NTFS_FCB), TAG_FCB, 0);
    /* Initialize lookaside list for attributes contexts */
    ExInitializeNPagedLookasideList(&NtfsGlobalData->AttrCtxtLookasideList,
                                    NULL, NULL, 0, sizeof(NTFS_ATTR_CONTEXT), TAG_ATT_CTXT, 0);

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
    DriverObject->MajorFunction[IRP_MJ_FILE_SYSTEM_CONTROL]      = NtfsFsdDispatch;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL]           = NtfsFsdDispatch;
    DriverObject->MajorFunction[IRP_MJ_LOCK_CONTROL]            = NtfsFsdDispatch;
    DriverObject->MajorFunction[IRP_MJ_PNP]                     = NtfsFsdDispatch;
    DriverObject->MajorFunction[IRP_MJ_SHUTDOWN]                = NtfsFsdDispatch;
    DriverObject->MajorFunction[IRP_MJ_FLUSH_BUFFERS]            = NtfsFsdDispatch;
    DriverObject->MajorFunction[IRP_MJ_QUERY_SECURITY]           = NtfsFsdDispatch;
    DriverObject->MajorFunction[IRP_MJ_QUERY_EA]                 = NtfsFsdDispatch;
    DriverObject->MajorFunction[IRP_MJ_QUERY_QUOTA]              = NtfsFsdDispatch;
    DriverObject->MajorFunction[IRP_MJ_SET_QUOTA]                = NtfsFsdDispatch;
    DriverObject->MajorFunction[IRP_MJ_SET_EA]                   = NtfsFsdDispatch;
    DriverObject->MajorFunction[IRP_MJ_SET_SECURITY]             = NtfsFsdDispatch;

    return;
}

/* EOF */
