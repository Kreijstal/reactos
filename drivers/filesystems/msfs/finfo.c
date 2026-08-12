/*
 * COPYRIGHT:  See COPYING in the top level directory
 * PROJECT:    ReactOS kernel
 * FILE:       drivers/filesystems/msfs/finfo.c
 * PURPOSE:    Mailslot filesystem
 * PROGRAMMER: Eric Kohl
 */

/* INCLUDES ******************************************************************/

#include "msfs.h"

#define NDEBUG
#include <debug.h>

#undef MAILSLOT_NO_MESSAGE
#undef MAILSLOT_WAIT_FOREVER
#define MAILSLOT_NO_MESSAGE   MAXULONG
#define MAILSLOT_WAIT_FOREVER MAXULONG

/* FUNCTIONS *****************************************************************/

static NTSTATUS
MsfsQueryMailslotInformation(PMSFS_FCB Fcb,
                             PFILE_MAILSLOT_QUERY_INFORMATION Buffer,
                             PULONG BufferLength)
{
    KIRQL oldIrql;

    if (*BufferLength < sizeof(FILE_MAILSLOT_QUERY_INFORMATION))
        return STATUS_BUFFER_OVERFLOW;

    Buffer->MaximumMessageSize = Fcb->MaxMessageSize;
    Buffer->ReadTimeout = Fcb->TimeOut;

    KeAcquireSpinLock(&Fcb->MessageListLock, &oldIrql);
    Buffer->MessagesAvailable = Fcb->MessageCount;
    if (Fcb->MessageCount == 0)
    {
        Buffer->NextMessageSize = MAILSLOT_NO_MESSAGE;
    }
    else
    {
        PMSFS_MESSAGE Message = CONTAINING_RECORD(Fcb->MessageListHead.Flink,
                                                  MSFS_MESSAGE,
                                                  MessageListEntry);
        Buffer->NextMessageSize = Message->Size;
    }
    KeReleaseSpinLock(&Fcb->MessageListLock, oldIrql);

    *BufferLength -= sizeof(FILE_MAILSLOT_QUERY_INFORMATION);

    return STATUS_SUCCESS;
}

static NTSTATUS
MsfsQueryNameInformation(PMSFS_FCB Fcb,
                         PFILE_NAME_INFORMATION Buffer,
                         PULONG BufferLength)
{
    ULONG HeaderLength = FIELD_OFFSET(FILE_NAME_INFORMATION, FileName);
    ULONG NameLength = Fcb ? Fcb->Name.Length : 0;
    ULONG CopyLength;

    if (*BufferLength < HeaderLength)
        return STATUS_BUFFER_OVERFLOW;

    Buffer->FileNameLength = NameLength;
    CopyLength = min(NameLength, *BufferLength - HeaderLength);
    if (CopyLength)
    {
        RtlCopyMemory(Buffer->FileName, Fcb->Name.Buffer, CopyLength);
    }
    else if (*BufferLength >= HeaderLength + sizeof(WCHAR))
    {
        /* I/O manager expects an absolute file name even for the root. */
        Buffer->FileName[0] = L'\\';
    }

    *BufferLength -= HeaderLength + CopyLength;
    return (CopyLength == NameLength) ? STATUS_SUCCESS : STATUS_BUFFER_OVERFLOW;
}


static NTSTATUS
MsfsSetMailslotInformation(PMSFS_FCB Fcb,
                           PFILE_MAILSLOT_SET_INFORMATION Buffer,
                           PULONG BufferLength)
{
    if (*BufferLength < sizeof(FILE_MAILSLOT_SET_INFORMATION))
        return STATUS_BUFFER_OVERFLOW;

    Fcb->TimeOut = *Buffer->ReadTimeout;

    return STATUS_SUCCESS;
}


NTSTATUS DEFAULTAPI
MsfsQueryInformation(PDEVICE_OBJECT DeviceObject,
                     PIRP Irp)
{
    PIO_STACK_LOCATION IoStack;
    FILE_INFORMATION_CLASS FileInformationClass;
    PFILE_OBJECT FileObject;
    PMSFS_FCB Fcb;
    PMSFS_CCB Ccb;
    PVOID SystemBuffer;
    ULONG BufferLength;
    NTSTATUS Status;

    DPRINT("MsfsQueryInformation(DeviceObject %p Irp %p)\n",
           DeviceObject, Irp);

    IoStack = IoGetCurrentIrpStackLocation (Irp);
    FileInformationClass = IoStack->Parameters.QueryFile.FileInformationClass;
    FileObject = IoStack->FileObject;
    Fcb = (PMSFS_FCB)FileObject->FsContext;
    Ccb = (PMSFS_CCB)FileObject->FsContext2;

    if (Fcb) DPRINT("Mailslot name: %wZ\n", &Fcb->Name);

    SystemBuffer = Irp->AssociatedIrp.SystemBuffer;
    BufferLength = IoStack->Parameters.QueryFile.Length;

    /* Names are available for the server, clients, and the filesystem root. */
    if (FileInformationClass == FileNameInformation)
    {
        if (Fcb)
        {
            Status = MsfsQueryNameInformation(Fcb,
                                              SystemBuffer,
                                              &BufferLength);
        }
        else
        {
            Status = STATUS_INVALID_PARAMETER;
        }
        goto Complete;
    }

    /* querying information is not permitted on client side */
    if (!Fcb || Fcb->ServerCcb != Ccb)
    {
        Status = STATUS_ACCESS_DENIED;
        goto Complete;
    }

    switch (FileInformationClass)
    {
    case FileMailslotQueryInformation:
        Status = MsfsQueryMailslotInformation(Fcb,
                                              SystemBuffer,
                                              &BufferLength);
        break;

    default:
        Status = STATUS_NOT_IMPLEMENTED;
    }

Complete:
    Irp->IoStatus.Status = Status;
    if (NT_SUCCESS(Status) || (Status == STATUS_BUFFER_OVERFLOW))
        Irp->IoStatus.Information =
             IoStack->Parameters.QueryFile.Length - BufferLength;
    else
        Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return Status;
}


NTSTATUS DEFAULTAPI
MsfsSetInformation(PDEVICE_OBJECT DeviceObject,
                   PIRP Irp)
{
    PIO_STACK_LOCATION IoStack;
    FILE_INFORMATION_CLASS FileInformationClass;
    PFILE_OBJECT FileObject;
    PMSFS_FCB Fcb;
    PMSFS_CCB Ccb;
    PVOID SystemBuffer;
    ULONG BufferLength;
    NTSTATUS Status;

    DPRINT("MsfsSetInformation(DeviceObject %p Irp %p)\n", DeviceObject, Irp);

    IoStack = IoGetCurrentIrpStackLocation (Irp);
    FileInformationClass = IoStack->Parameters.QueryFile.FileInformationClass;
    FileObject = IoStack->FileObject;
    Fcb = (PMSFS_FCB)FileObject->FsContext;
    Ccb = (PMSFS_CCB)FileObject->FsContext2;

    DPRINT("Mailslot name: %wZ\n", &Fcb->Name);

    /* setting information is not permitted on client side */
    if (Fcb->ServerCcb != Ccb)
    {
        Status = STATUS_ACCESS_DENIED;

        Irp->IoStatus.Status = Status;
        Irp->IoStatus.Information = 0;

        IoCompleteRequest(Irp, IO_NO_INCREMENT);

        return Status;
    }

    SystemBuffer = Irp->AssociatedIrp.SystemBuffer;
    BufferLength = IoStack->Parameters.QueryFile.Length;

    DPRINT("FileInformationClass %d\n", FileInformationClass);
    DPRINT("SystemBuffer %p\n", SystemBuffer);

    switch (FileInformationClass)
    {
    case FileMailslotSetInformation:
        Status = MsfsSetMailslotInformation(Fcb,
                                            SystemBuffer,
                                            &BufferLength);
        break;

     default:
        Status = STATUS_NOT_IMPLEMENTED;
    }

    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return Status;
}

/* EOF */
