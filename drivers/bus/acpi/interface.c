#include "precomp.h"

#define NDEBUG
#include <debug.h>

VOID
NTAPI
AcpiInterfaceReference(PVOID Context)
{
  UNIMPLEMENTED;
}

VOID
NTAPI
AcpiInterfaceDereference(PVOID Context)
{
  UNIMPLEMENTED;
}

NTSTATUS
NTAPI
AcpiInterfaceConnectVector(PDEVICE_OBJECT Context,
                           ULONG GpeNumber,
                           KINTERRUPT_MODE Mode,
                           BOOLEAN Shareable,
                           PGPE_SERVICE_ROUTINE ServiceRoutine,
                           PVOID ServiceContext,
                           PVOID ObjectContext)
{
  UNIMPLEMENTED;

  return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
NTAPI
AcpiInterfaceDisconnectVector(PVOID ObjectContext)
{
  UNIMPLEMENTED;

  return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
NTAPI
AcpiInterfaceEnableEvent(PDEVICE_OBJECT Context,
                         PVOID ObjectContext)
{
  UNIMPLEMENTED;

  return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
NTAPI
AcpiInterfaceDisableEvent(PDEVICE_OBJECT Context,
                          PVOID ObjectContext)
{
  UNIMPLEMENTED;

  return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
NTAPI
AcpiInterfaceClearStatus(PDEVICE_OBJECT Context,
                         PVOID ObjectContext)
{
  UNIMPLEMENTED;

  return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
NTAPI
AcpiInterfaceNotificationsRegister(PDEVICE_OBJECT Context,
                                   PDEVICE_NOTIFY_CALLBACK NotificationHandler,
                                   PVOID NotificationContext)
{
  UNIMPLEMENTED;

  return STATUS_SUCCESS;
}

VOID
NTAPI
AcpiInterfaceNotificationsUnregister(PDEVICE_OBJECT Context,
                                     PDEVICE_NOTIFY_CALLBACK NotificationHandler)
{
  UNIMPLEMENTED;
}

/*
 * Lazy load and cache the _PRT (PCI Routing Table) for a PCI host bridge
 * (PNP0A03/PNP0A08).  AcpiGetIrqRoutingTable returns a buffer of variable
 * length packed ACPI_PCI_ROUTING_TABLE entries terminated by a Length=0
 * sentinel; we keep that buffer verbatim and walk it on every call.
 */
static
NTSTATUS
AcpiPciRoutingEnsureCached(_In_ PPDO_DEVICE_DATA DeviceData)
{
    ACPI_BUFFER Buffer;
    ACPI_STATUS AcpiStatus;
    PVOID Table;

    if (DeviceData->PciRoutingTable != NULL)
        return STATUS_SUCCESS;

    if (DeviceData->AcpiHandle == NULL)
        return STATUS_NOT_FOUND;

    /* Probe size first. */
    Buffer.Length = 0;
    Buffer.Pointer = NULL;
    AcpiStatus = AcpiGetIrqRoutingTable(DeviceData->AcpiHandle, &Buffer);
    if (AcpiStatus != AE_BUFFER_OVERFLOW || Buffer.Length == 0)
    {
        DPRINT1("AcpiGetIrqRoutingTable size probe failed (0x%x, len %lu)\n",
                AcpiStatus, (ULONG)Buffer.Length);
        return STATUS_NOT_FOUND;
    }

    Table = ExAllocatePoolWithTag(PagedPool, Buffer.Length, 'PpcA');
    if (Table == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Buffer.Pointer = Table;
    AcpiStatus = AcpiGetIrqRoutingTable(DeviceData->AcpiHandle, &Buffer);
    if (!ACPI_SUCCESS(AcpiStatus))
    {
        DPRINT1("AcpiGetIrqRoutingTable fetch failed (0x%x)\n", AcpiStatus);
        ExFreePoolWithTag(Table, 'PpcA');
        return STATUS_UNSUCCESSFUL;
    }

    DeviceData->PciRoutingTable = Table;
    DeviceData->PciRoutingTableSize = (ULONG)Buffer.Length;
    return STATUS_SUCCESS;
}

/*
 * Resolve a Source Link Device (LNKx) handle to a system IRQ by walking
 * its current resource settings.  The first IRQ resource produced by
 * _CRS is the IRQ that the link is currently programmed to.
 */
static
NTSTATUS
AcpiPciRoutingResolveLinkIrq(
    _In_ ACPI_HANDLE LinkHandle,
    _Out_ PULONG SystemIrq,
    _Out_ PBOOLEAN IsLevelTriggered,
    _Out_ PBOOLEAN IsActiveLow)
{
    ACPI_BUFFER Buffer;
    ACPI_STATUS AcpiStatus;
    ACPI_RESOURCE *Resource;
    NTSTATUS Status = STATUS_NOT_FOUND;

    Buffer.Length = ACPI_ALLOCATE_BUFFER;
    Buffer.Pointer = NULL;

    AcpiStatus = AcpiGetCurrentResources(LinkHandle, &Buffer);
    if (!ACPI_SUCCESS(AcpiStatus) || Buffer.Pointer == NULL)
    {
        DPRINT1("AcpiGetCurrentResources(LNK) failed: 0x%x\n", AcpiStatus);
        if (Buffer.Pointer)
            AcpiOsFree(Buffer.Pointer);
        return STATUS_UNSUCCESSFUL;
    }

    for (Resource = Buffer.Pointer;
         Resource->Type != ACPI_RESOURCE_TYPE_END_TAG;
         Resource = ACPI_NEXT_RESOURCE(Resource))
    {
        if (Resource->Type == ACPI_RESOURCE_TYPE_IRQ &&
            Resource->Data.Irq.InterruptCount > 0)
        {
            *SystemIrq = Resource->Data.Irq.Interrupts[0];
            *IsLevelTriggered = (Resource->Data.Irq.Triggering == ACPI_LEVEL_SENSITIVE);
            *IsActiveLow = (Resource->Data.Irq.Polarity == ACPI_ACTIVE_LOW);
            Status = STATUS_SUCCESS;
            break;
        }

        if (Resource->Type == ACPI_RESOURCE_TYPE_EXTENDED_IRQ &&
            Resource->Data.ExtendedIrq.InterruptCount > 0 &&
            Resource->Data.ExtendedIrq.ProducerConsumer == ACPI_CONSUMER)
        {
            *SystemIrq = Resource->Data.ExtendedIrq.Interrupts[0];
            *IsLevelTriggered = (Resource->Data.ExtendedIrq.Triggering == ACPI_LEVEL_SENSITIVE);
            *IsActiveLow = (Resource->Data.ExtendedIrq.Polarity == ACPI_ACTIVE_LOW);
            Status = STATUS_SUCCESS;
            break;
        }
    }

    AcpiOsFree(Buffer.Pointer);
    return Status;
}

static
NTSTATUS
NTAPI
AcpiInterfaceRoutePciInterrupt(
    _In_ PVOID Context,
    _In_ ULONG Bus,
    _In_ ULONG SlotDevice,
    _In_ ULONG SlotFunction,
    _In_ UCHAR Pin,
    _Out_ PULONG SystemIrq,
    _Out_ PBOOLEAN IsLevelTriggered,
    _Out_ PBOOLEAN IsActiveLow)
{
    PPDO_DEVICE_DATA DeviceData = (PPDO_DEVICE_DATA)Context;
    NTSTATUS Status;
    ACPI_PCI_ROUTING_TABLE *Entry;
    PUCHAR Cursor;
    PUCHAR End;
    ULONG TargetAddress;

    UNREFERENCED_PARAMETER(Bus);

    if (Pin == 0 || Pin > 4)
        return STATUS_INVALID_PARAMETER;

    /* The _PRT pin index is 0-based (0=INTA), but the PCI config-space
     * value stored in InterruptPin is 1-based (1=INTA).  Normalize. */
    Pin -= 1;

    *SystemIrq = 0;
    *IsLevelTriggered = TRUE;
    *IsActiveLow = TRUE;

    Status = AcpiPciRoutingEnsureCached(DeviceData);
    if (!NT_SUCCESS(Status))
        return Status;

    /* _PRT Address encodes (Device << 16) | Function, with Function == 0xFFFF
     * meaning "any function on this device". */
    TargetAddress = (SlotDevice << 16) | (SlotFunction & 0xFFFF);

    Cursor = (PUCHAR)DeviceData->PciRoutingTable;
    End = Cursor + DeviceData->PciRoutingTableSize;

    while (Cursor < End)
    {
        Entry = (ACPI_PCI_ROUTING_TABLE *)Cursor;
        if (Entry->Length == 0)
            break;

        if ((ULONG)(Entry->Address >> 16) == SlotDevice &&
            ((Entry->Address & 0xFFFF) == 0xFFFF ||
             (Entry->Address & 0xFFFF) == SlotFunction) &&
            Entry->Pin == Pin)
        {
            if (Entry->Source[0] == 0)
            {
                /* Direct GSI: SourceIndex is the system IRQ. PCI INTx is
                 * level-triggered active-low by spec. */
                *SystemIrq = Entry->SourceIndex;
                *IsLevelTriggered = TRUE;
                *IsActiveLow = TRUE;
                return STATUS_SUCCESS;
            }
            else
            {
                ACPI_HANDLE LinkHandle = NULL;
                ACPI_STATUS AcpiStatus;

                AcpiStatus = AcpiGetHandle(DeviceData->AcpiHandle,
                                           Entry->Source,
                                           &LinkHandle);
                if (!ACPI_SUCCESS(AcpiStatus) || LinkHandle == NULL)
                {
                    DPRINT1("AcpiGetHandle(%hs) failed: 0x%x\n",
                            Entry->Source, AcpiStatus);
                    return STATUS_NOT_FOUND;
                }

                return AcpiPciRoutingResolveLinkIrq(LinkHandle,
                                                   SystemIrq,
                                                   IsLevelTriggered,
                                                   IsActiveLow);
            }
        }

        Cursor += Entry->Length;
    }

    UNREFERENCED_PARAMETER(TargetAddress);
    return STATUS_NOT_FOUND;
}

NTSTATUS
Bus_PDO_QueryInterface(PPDO_DEVICE_DATA DeviceData,
                       PIRP Irp)
{
  PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
  PACPI_INTERFACE_STANDARD AcpiInterface;

  if (IrpSp->Parameters.QueryInterface.Version != 1)
  {
      DPRINT1("Invalid version number: %d\n",
              IrpSp->Parameters.QueryInterface.Version);
      return STATUS_INVALID_PARAMETER;
  }

  if (RtlCompareMemory(IrpSp->Parameters.QueryInterface.InterfaceType,
                        &GUID_ACPI_INTERFACE_STANDARD, sizeof(GUID)) == sizeof(GUID))
  {
      DPRINT("GUID_ACPI_INTERFACE_STANDARD\n");

      if (IrpSp->Parameters.QueryInterface.Size < sizeof(ACPI_INTERFACE_STANDARD))
      {
          DPRINT1("Buffer too small! (%d)\n", IrpSp->Parameters.QueryInterface.Size);
          return STATUS_BUFFER_TOO_SMALL;
      }

     AcpiInterface = (PACPI_INTERFACE_STANDARD)IrpSp->Parameters.QueryInterface.Interface;

     AcpiInterface->InterfaceReference = AcpiInterfaceReference;
     AcpiInterface->InterfaceDereference = AcpiInterfaceDereference;
     AcpiInterface->GpeConnectVector = AcpiInterfaceConnectVector;
     AcpiInterface->GpeDisconnectVector = AcpiInterfaceDisconnectVector;
     AcpiInterface->GpeEnableEvent = AcpiInterfaceEnableEvent;
     AcpiInterface->GpeDisableEvent = AcpiInterfaceDisableEvent;
     AcpiInterface->GpeClearStatus = AcpiInterfaceClearStatus;
     AcpiInterface->RegisterForDeviceNotifications = AcpiInterfaceNotificationsRegister;
     AcpiInterface->UnregisterForDeviceNotifications = AcpiInterfaceNotificationsUnregister;

     return STATUS_SUCCESS;
  }
  else if (RtlCompareMemory(IrpSp->Parameters.QueryInterface.InterfaceType,
                            &GUID_REACTOS_PCI_ROUTING_INTERFACE, sizeof(GUID)) == sizeof(GUID))
  {
      PREACTOS_PCI_ROUTING_INTERFACE PciRouting;

      DPRINT("GUID_REACTOS_PCI_ROUTING_INTERFACE\n");

      /* Only meaningful on PCI host bridges. */
      if (DeviceData->HardwareIDs == NULL ||
          (wcsstr(DeviceData->HardwareIDs, L"PNP0A03") == NULL &&
           wcsstr(DeviceData->HardwareIDs, L"PNP0A08") == NULL))
      {
          return STATUS_NOT_SUPPORTED;
      }

      if (IrpSp->Parameters.QueryInterface.Size < sizeof(REACTOS_PCI_ROUTING_INTERFACE))
      {
          DPRINT1("Buffer too small! (%d)\n", IrpSp->Parameters.QueryInterface.Size);
          return STATUS_BUFFER_TOO_SMALL;
      }

      PciRouting = (PREACTOS_PCI_ROUTING_INTERFACE)IrpSp->Parameters.QueryInterface.Interface;
      PciRouting->Size = sizeof(REACTOS_PCI_ROUTING_INTERFACE);
      PciRouting->Version = REACTOS_PCI_ROUTING_INTERFACE_VERSION;
      PciRouting->Context = DeviceData;
      PciRouting->InterfaceReference = AcpiInterfaceReference;
      PciRouting->InterfaceDereference = AcpiInterfaceDereference;
      PciRouting->RouteInterrupt = AcpiInterfaceRoutePciInterrupt;
      return STATUS_SUCCESS;
  }
  else
  {
      DPRINT1("Invalid GUID\n");
      return STATUS_NOT_SUPPORTED;
  }
}
