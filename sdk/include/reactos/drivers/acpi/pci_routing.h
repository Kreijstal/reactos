#pragma once

DEFINE_GUID(GUID_REACTOS_PCI_ROUTING_INTERFACE,
  0x21d2a3db, 0xf7a4, 0x4d93, 0x94, 0x8c, 0x7f, 0x12, 0xd0, 0x7b, 0x11, 0x8a);

#ifndef REACTOS_PCI_ROUTING_GUID_ONLY

#include <ntddk.h>

#define REACTOS_PCI_ROUTING_INTERFACE_VERSION 1

typedef
NTSTATUS
(NTAPI *PREACTOS_PCI_ROUTE_INTERRUPT)(
    _In_ PVOID Context,
    _In_ ULONG Bus,
    _In_ ULONG SlotDevice,
    _In_ ULONG SlotFunction,
    _In_ UCHAR Pin,
    _Out_ PULONG SystemIrq,
    _Out_ PBOOLEAN IsLevelTriggered,
    _Out_ PBOOLEAN IsActiveLow);

typedef struct _REACTOS_PCI_ROUTING_INTERFACE
{
    USHORT Size;
    USHORT Version;
    PVOID Context;
    PINTERFACE_REFERENCE InterfaceReference;
    PINTERFACE_DEREFERENCE InterfaceDereference;
    PREACTOS_PCI_ROUTE_INTERRUPT RouteInterrupt;
} REACTOS_PCI_ROUTING_INTERFACE, *PREACTOS_PCI_ROUTING_INTERFACE;

#endif /* REACTOS_PCI_ROUTING_GUID_ONLY */
