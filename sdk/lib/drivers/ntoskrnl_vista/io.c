/*
 * PROJECT:     ReactOS Kernel - Vista+ APIs
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Io functions of Vista+
 * COPYRIGHT:   2016 Pierre Schweitzer (pierre@reactos.org)
 *              2020 Victor Perevertkin (victor.perevertkin@reactos.org)
 */

#include <ntdef.h>
#include <ntifs.h>

typedef struct _EX_WORKITEM_CONTEXT
{
    PIO_WORKITEM WorkItem;
    PIO_WORKITEM_ROUTINE_EX WorkItemRoutineEx;
    PVOID Context;
} EX_WORKITEM_CONTEXT, *PEX_WORKITEM_CONTEXT;

#define TAG_IOWI 'IWOI'

NTKRNLVISTAAPI
NTSTATUS
NTAPI
IoGetIrpExtraCreateParameter(IN PIRP Irp,
                             OUT PECP_LIST *ExtraCreateParameter)
{
    /* Check we have a create operation */
    if (!BooleanFlagOn(Irp->Flags, IRP_CREATE_OPERATION))
    {
        return STATUS_INVALID_PARAMETER;
    }

    /* If so, return user buffer */
    *ExtraCreateParameter = Irp->UserBuffer;
    return STATUS_SUCCESS;
}

_Function_class_(IO_WORKITEM_ROUTINE)
static
VOID
NTAPI
IopWorkItemExCallback(
    PDEVICE_OBJECT DeviceObject,
    PVOID Ctx)
{
    PEX_WORKITEM_CONTEXT context = Ctx;

    context->WorkItemRoutineEx(DeviceObject, context->Context, context->WorkItem);
    ExFreePoolWithTag(context, TAG_IOWI);
}

NTKRNLVISTAAPI
VOID
NTAPI
IoQueueWorkItemEx(
    _Inout_ PIO_WORKITEM IoWorkItem,
    _In_ PIO_WORKITEM_ROUTINE_EX WorkerRoutine,
    _In_ WORK_QUEUE_TYPE QueueType,
    _In_opt_ __drv_aliasesMem PVOID Context)
{
    PEX_WORKITEM_CONTEXT newContext = ExAllocatePoolWithTag(NonPagedPoolMustSucceed, sizeof(*newContext), TAG_IOWI);
    newContext->WorkItem = IoWorkItem;
    newContext->WorkItemRoutineEx = WorkerRoutine;
    newContext->Context = Context;

    IoQueueWorkItem(IoWorkItem, IopWorkItemExCallback, QueueType, newContext);
}

NTKRNLVISTAAPI
IO_PRIORITY_HINT
NTAPI
IoGetIoPriorityHint(
    _In_ PIRP Irp)
{
    return IoPriorityNormal;
}

NTKRNLVISTAAPI
VOID
NTAPI
IoSetMasterIrpStatus(
    _Inout_ PIRP MasterIrp,
    _In_ NTSTATUS Status)
{
    NTSTATUS MasterStatus = MasterIrp->IoStatus.Status;

    if (Status == STATUS_FT_READ_FROM_COPY)
    {
        return;
    }

    if ((Status == STATUS_VERIFY_REQUIRED) ||
        (MasterStatus == STATUS_SUCCESS && !NT_SUCCESS(Status)) ||
        (!NT_SUCCESS(MasterStatus) && !NT_SUCCESS(Status) && Status > MasterStatus))
    {
        MasterIrp->IoStatus.Status = Status;
    }
}

/*
 * Opens (creating it if needed) a per-driver key below the driver's service
 * key: "Parameters" for DriverRegKeyParameters and "State" for
 * DriverRegKeyPersistentState.
 */
NTKRNLVISTAAPI
NTSTATUS
NTAPI
IoOpenDriverRegistryKey(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ DRIVER_REGKEY_TYPE RegKeyType,
    _In_ ACCESS_MASK DesiredAccess,
    _In_ ULONG Flags,
    _Out_ PHANDLE DriverRegKey)
{
    static const WCHAR ServicesPath[] =
        L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\";
    PCWSTR SubKeyName;
    PUNICODE_STRING ServiceName;
    UNICODE_STRING KeyPath;
    OBJECT_ATTRIBUTES ObjectAttributes;
    NTSTATUS Status;

    PAGED_CODE();

    *DriverRegKey = NULL;

    if (Flags != 0 || DriverObject == NULL ||
        DriverObject->DriverExtension == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    switch (RegKeyType)
    {
        case DriverRegKeyParameters:
            SubKeyName = L"\\Parameters";
            break;
        case DriverRegKeyPersistentState:
            SubKeyName = L"\\State";
            break;
        default:
            return STATUS_NOT_SUPPORTED;
    }

    ServiceName = &DriverObject->DriverExtension->ServiceKeyName;
    if (ServiceName->Length == 0)
        return STATUS_OBJECT_NAME_NOT_FOUND;

    KeyPath.Length = 0;
    KeyPath.MaximumLength = sizeof(ServicesPath) + ServiceName->Length +
                            (USHORT)(wcslen(SubKeyName) * sizeof(WCHAR));
    KeyPath.Buffer = ExAllocatePoolWithTag(PagedPool, KeyPath.MaximumLength, 'kRoI');
    if (KeyPath.Buffer == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    RtlAppendUnicodeToString(&KeyPath, ServicesPath);
    RtlAppendUnicodeStringToString(&KeyPath, ServiceName);
    RtlAppendUnicodeToString(&KeyPath, SubKeyName);

    InitializeObjectAttributes(&ObjectAttributes,
                               &KeyPath,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               NULL,
                               NULL);

    Status = ZwCreateKey(DriverRegKey,
                         DesiredAccess,
                         &ObjectAttributes,
                         0,
                         NULL,
                         REG_OPTION_NON_VOLATILE,
                         NULL);

    ExFreePoolWithTag(KeyPath.Buffer, 'kRoI');
    return Status;
}

/*
 * ReactOS does not record on whose behalf a file object was created, which
 * is what Windows reports when no initiator was specified.
 */
NTKRNLVISTAAPI
PEPROCESS
NTAPI
IoGetInitiatorProcess(
    _In_ PFILE_OBJECT FileObject)
{
    UNREFERENCED_PARAMETER(FileObject);
    return NULL;
}
