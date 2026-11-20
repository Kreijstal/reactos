/*
 * PROJECT:         ReactOS Kernel
 * LICENSE:         GPL - See COPYING in the top level directory
 * FILE:            ntoskrnl/io/iomgr/irq.c
 * PURPOSE:         I/O Wrappers (called Completion Ports) for Kernel Queues
 * PROGRAMMERS:     Alex Ionescu (alex.ionescu@reactos.org)
 */

/* INCLUDES *****************************************************************/

#include <ntoskrnl.h>

#define NDEBUG
#include <debug.h>

/* FUNCTIONS *****************************************************************/

/*
 * @implemented
 */
NTSTATUS
NTAPI
IoConnectInterrupt(OUT PKINTERRUPT *InterruptObject,
                   IN PKSERVICE_ROUTINE ServiceRoutine,
                   IN PVOID ServiceContext,
                   IN PKSPIN_LOCK SpinLock,
                   IN ULONG Vector,
                   IN KIRQL Irql,
                   IN KIRQL SynchronizeIrql,
                   IN KINTERRUPT_MODE InterruptMode,
                   IN BOOLEAN ShareVector,
                   IN KAFFINITY ProcessorEnableMask,
                   IN BOOLEAN FloatingSave)
{
    PKINTERRUPT Interrupt;
    PKINTERRUPT InterruptUsed;
    PIO_INTERRUPT IoInterrupt;
    BOOLEAN FirstRun;
    CCHAR Count = 0;
    KAFFINITY Affinity;

    PAGED_CODE();

    /* Assume failure */
    *InterruptObject = NULL;

    /* Get the affinity */
    Affinity = ProcessorEnableMask & KeActiveProcessors;
    while (Affinity)
    {
        /* Increase count */
        if (Affinity & 1) Count++;
        Affinity >>= 1;
    }

    /* Make sure we have a valid CPU count */
    if (!Count) return STATUS_INVALID_PARAMETER;

    /* Allocate the array of I/O interrupts */
    IoInterrupt = ExAllocatePoolZero(NonPagedPool,
                                     (Count - 1) * sizeof(KINTERRUPT) +
                                     sizeof(IO_INTERRUPT),
                                     TAG_IO_INTERRUPT);
    if (!IoInterrupt) return STATUS_INSUFFICIENT_RESOURCES;

    /* Use the structure's spinlock, if none was provided */
    if (!SpinLock)
    {
        SpinLock = &IoInterrupt->SpinLock;
        KeInitializeSpinLock(SpinLock);
    }

    /* We first start with a built-in interrupt inside the I/O structure */
    Interrupt = (PKINTERRUPT)(IoInterrupt + 1);
    FirstRun = TRUE;

    /* Now create all the interrupts */
    Affinity = ProcessorEnableMask & KeActiveProcessors;
    for (Count = 0; Affinity; Count++, Affinity >>= 1)
    {
        /* Check if it's enabled for this CPU */
        if (!(Affinity & 1))
            continue;

        /* Check which one we will use */
        InterruptUsed = FirstRun ? &IoInterrupt->FirstInterrupt : Interrupt;

        /* Initialize it */
        KeInitializeInterrupt(InterruptUsed,
                              ServiceRoutine,
                              ServiceContext,
                              SpinLock,
                              Vector,
                              Irql,
                              SynchronizeIrql,
                              InterruptMode,
                              ShareVector,
                              Count,
                              FloatingSave);

        /* Connect it */
        if (!KeConnectInterrupt(InterruptUsed))
        {
            /* Check how far we got */
            if (FirstRun)
            {
                /* We failed early so just free this */
                ExFreePoolWithTag(IoInterrupt, TAG_IO_INTERRUPT);
            }
            else
            {
                /* Far enough, so disconnect everything */
                IoDisconnectInterrupt(&IoInterrupt->FirstInterrupt);
            }

            /* And fail */
            return STATUS_INVALID_PARAMETER;
        }

        /* Now we've used up our First Run */
        if (FirstRun)
        {
            FirstRun = FALSE;
        }
        else
        {
            /* Move on to the next one */
            IoInterrupt->Interrupt[(UCHAR)Count] = Interrupt++;
        }
    }

    /* Return success */
    *InterruptObject = &IoInterrupt->FirstInterrupt;
    return STATUS_SUCCESS;
}

/*
 * @implemented
 */
VOID
NTAPI
IoDisconnectInterrupt(PKINTERRUPT InterruptObject)
{
    PIO_INTERRUPT IoInterrupt;
    ULONG i;

    PAGED_CODE();

    /* Get the I/O interrupt */
    IoInterrupt = CONTAINING_RECORD(InterruptObject,
                                    IO_INTERRUPT,
                                    FirstInterrupt);

    /* Disconnect the first one */
    KeDisconnectInterrupt(&IoInterrupt->FirstInterrupt);

    /* Now disconnect the others */
    for (i = 0; i < KeNumberProcessors; i++)
    {
        /* Make sure one was registered */
        if (!IoInterrupt->Interrupt[i])
            continue;

        /* Disconnect it */
        KeDisconnectInterrupt(IoInterrupt->Interrupt[i]);
    }

    /* Free the I/O interrupt */
    ExFreePoolWithTag(IoInterrupt, TAG_IO_INTERRUPT);
}

NTSTATUS
IopConnectInterruptExFullySpecific(
    _Inout_ PIO_CONNECT_INTERRUPT_PARAMETERS Parameters)
{
    NTSTATUS Status;

    PAGED_CODE();

    /* Fallback to standard IoConnectInterrupt */
    Status = IoConnectInterrupt(Parameters->FullySpecified.InterruptObject,
                                Parameters->FullySpecified.ServiceRoutine,
                                Parameters->FullySpecified.ServiceContext,
                                Parameters->FullySpecified.SpinLock,
                                Parameters->FullySpecified.Vector,
                                Parameters->FullySpecified.Irql,
                                Parameters->FullySpecified.SynchronizeIrql,
                                Parameters->FullySpecified.InterruptMode,
                                Parameters->FullySpecified.ShareVector,
                                Parameters->FullySpecified.ProcessorEnableMask,
                                Parameters->FullySpecified.FloatingSave);
    if (!NT_SUCCESS(Status))
        DPRINT1("IopConnectInterruptExFullySpecific() failed: 0x%lx\n", Status);
    return Status;
}

/*
 * Connects ServiceRoutine to the line interrupt PnP assigned to PhysicalDeviceObject,
 * as found in its translated resources.
 */
static
NTSTATUS
IopConnectInterruptExLineBased(
    _In_ PDEVICE_OBJECT PhysicalDeviceObject,
    _Out_ PKINTERRUPT *InterruptObject,
    _In_ PKSERVICE_ROUTINE ServiceRoutine,
    _In_ PVOID ServiceContext,
    _In_opt_ PKSPIN_LOCK SpinLock,
    _In_opt_ KIRQL SynchronizeIrql,
    _In_ BOOLEAN FloatingSave)
{
    PDEVICE_NODE DeviceNode;
    PCM_RESOURCE_LIST ResourceList;
    PCM_FULL_RESOURCE_DESCRIPTOR FullDescriptor;
    PCM_PARTIAL_RESOURCE_DESCRIPTOR Descriptor;
    ULONG i, j;
    KIRQL Irql;

    PAGED_CODE();

    *InterruptObject = NULL;

    if (!PhysicalDeviceObject)
        return STATUS_INVALID_PARAMETER;

    DeviceNode = IopGetDeviceNode(PhysicalDeviceObject);
    if (!DeviceNode)
        return STATUS_INVALID_PARAMETER;

    ResourceList = DeviceNode->ResourceListTranslated;
    if (!ResourceList)
        return STATUS_NOT_FOUND;

    FullDescriptor = &ResourceList->List[0];
    for (i = 0; i < ResourceList->Count; i++)
    {
        for (j = 0; j < FullDescriptor->PartialResourceList.Count; j++)
        {
            Descriptor = &FullDescriptor->PartialResourceList.PartialDescriptors[j];
            if (Descriptor->Type != CmResourceTypeInterrupt ||
                (Descriptor->Flags & CM_RESOURCE_INTERRUPT_MESSAGE))
            {
                continue;
            }

            Irql = (KIRQL)Descriptor->u.Interrupt.Level;
            return IoConnectInterrupt(InterruptObject,
                                      ServiceRoutine,
                                      ServiceContext,
                                      SpinLock,
                                      Descriptor->u.Interrupt.Vector,
                                      Irql,
                                      SpinLock ? SynchronizeIrql : Irql,
                                      (Descriptor->Flags & CM_RESOURCE_INTERRUPT_LATCHED) ?
                                          Latched : LevelSensitive,
                                      Descriptor->ShareDisposition == CmResourceShareShared,
                                      Descriptor->u.Interrupt.Affinity,
                                      FloatingSave);
        }

        /* The next full descriptor follows the last partial one */
        FullDescriptor = (PCM_FULL_RESOURCE_DESCRIPTOR)
            &FullDescriptor->PartialResourceList.PartialDescriptors[FullDescriptor->PartialResourceList.Count];
    }

    return STATUS_NOT_FOUND;
}

NTSTATUS
NTAPI
IoConnectInterruptEx(
    _Inout_ PIO_CONNECT_INTERRUPT_PARAMETERS Parameters)
{
    NTSTATUS Status;

    PAGED_CODE();

    switch (Parameters->Version)
    {
        case CONNECT_FULLY_SPECIFIED:
            return IopConnectInterruptExFullySpecific(Parameters);
        case CONNECT_FULLY_SPECIFIED_GROUP:
            //TODO: We don't do anything for the group type
            return IopConnectInterruptExFullySpecific(Parameters);
        case CONNECT_MESSAGE_BASED:
            /*
             * No message signaled interrupt is ever assigned yet: like for any device
             * without messages, connect the fallback routine to the line interrupt
             * and tell the caller so through Version.
             */
            if (!Parameters->MessageBased.FallBackServiceRoutine)
                return STATUS_NOT_FOUND;

            Status = IopConnectInterruptExLineBased(Parameters->MessageBased.PhysicalDeviceObject,
                                                    &Parameters->MessageBased.ConnectionContext.InterruptObject,
                                                    Parameters->MessageBased.FallBackServiceRoutine,
                                                    Parameters->MessageBased.ServiceContext,
                                                    Parameters->MessageBased.SpinLock,
                                                    Parameters->MessageBased.SynchronizeIrql,
                                                    Parameters->MessageBased.FloatingSave);
            if (NT_SUCCESS(Status))
                Parameters->Version = CONNECT_LINE_BASED;
            return Status;
        case CONNECT_LINE_BASED:
            return IopConnectInterruptExLineBased(Parameters->LineBased.PhysicalDeviceObject,
                                                  Parameters->LineBased.InterruptObject,
                                                  Parameters->LineBased.ServiceRoutine,
                                                  Parameters->LineBased.ServiceContext,
                                                  Parameters->LineBased.SpinLock,
                                                  Parameters->LineBased.SynchronizeIrql,
                                                  Parameters->LineBased.FloatingSave);
    }

    return STATUS_INVALID_PARAMETER_1;
}

VOID
NTAPI
IoDisconnectInterruptEx(
    _In_ PIO_DISCONNECT_INTERRUPT_PARAMETERS Parameters)
{
    PAGED_CODE();

    //FIXME: This eventually will need to handle more cases
    if (Parameters->ConnectionContext.InterruptObject)
        IoDisconnectInterrupt(Parameters->ConnectionContext.InterruptObject);
}

/* EOF */
