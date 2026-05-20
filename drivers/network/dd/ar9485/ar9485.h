/*
 * PROJECT:     ReactOS Atheros AR9485 Wi-Fi Driver
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     NDIS 6.20 native-802.11 miniport for the AR9485 PCIe chip
 *              (PCI 168C:0032).  This header collects the shared adapter
 *              extension and forward declarations consumed by every
 *              ar9485 .c file.
 *
 * Phase 1a scope: load, bind to PCI\VEN_168C&DEV_0032, map BAR0, read the
 * AR_SREV register at offset 0x4020, and log the chip-revision.  No data
 * path, no PHY/RF, no scan/connect.  Subsequent phases drop the verbatim
 * Linux ath9k_hw layer under drivers/network/dd/ar9485/ath9k/ and wire it
 * to MiniportInitializeEx.
 */

#ifndef _AR9485_H_
#define _AR9485_H_

#include <ndis.h>
#include <wdmguid.h>

/* Pulled in from the sibling e1000e driver for now; this file backfills the
 * NDIS 6.20/6.30 symbol gaps in ROS' SDK ndis.h.  Promote to a real
 * sdk/include/ddk/ndis6_compat.h in a follow-up cleanup so each NDIS 6
 * miniport doesn't have to reach into a sibling tree. */
#include "../e1000e/ndis6_compat.h"

/* Register offsets (subset; full set arrives with ath9k/reg.h in Phase 2a). */
#define AR_SREV                     0x4020
#define AR_SREV_VERSION_MASK        0x0FFF0000
#define AR_SREV_VERSION_SHIFT       16
#define AR_SREV_REVISION_MASK       0xF0000000
#define AR_SREV_REVISION_SHIFT      28

/* Chip-version constants from ath9k/reg.h.  AR9485 reads back 0x240 in the
 * macVersion field after AR_SREV is masked and shifted as above. */
#define AR_SREV_VERSION_9485        0x240

#define AR9485_TAG                  'A584'

#define AR9485_FLAG_HW_RECOGNIZED   0x00000001
#define AR9485_FLAG_HALTING         0x00000002

typedef struct _AR9485_ADAPTER
{
    NDIS_HANDLE             MiniportAdapterHandle;
    NDIS_HANDLE             NdisMiniportDriverHandle;
    PDEVICE_OBJECT          PhysicalDeviceObject;

    /* PCI identity */
    USHORT                  VendorId;
    USHORT                  DeviceId;
    UCHAR                   RevisionId;

    /* BAR0 mapping */
    PHYSICAL_ADDRESS        IoAddress;
    ULONG                   IoLength;
    PVOID                   IoBase;

    /* Chip revision read from AR_SREV at init */
    ULONG                   SregRaw;
    ULONG                   MacVersion;
    ULONG                   MacRevision;

    /* Interrupt resource */
    ULONG                   InterruptVector;
    KIRQL                   InterruptLevel;
    KAFFINITY               InterruptAffinity;
    KINTERRUPT_MODE         InterruptModeType;
    BOOLEAN                 InterruptShared;
    BOOLEAN                 HasMessageInterrupt;
    NDIS_HANDLE             InterruptHandle;

    LONG                    Flags;
} AR9485_ADAPTER, *PAR9485_ADAPTER;

/* Read/write the memory-mapped register file (BAR0). */
FORCEINLINE ULONG
AR9485_READ_REG(_In_ PAR9485_ADAPTER Adapter, _In_ ULONG Offset)
{
    return READ_REGISTER_ULONG((PULONG)((PUCHAR)Adapter->IoBase + Offset));
}

FORCEINLINE VOID
AR9485_WRITE_REG(_In_ PAR9485_ADAPTER Adapter, _In_ ULONG Offset, _In_ ULONG Value)
{
    WRITE_REGISTER_ULONG((PULONG)((PUCHAR)Adapter->IoBase + Offset), Value);
}

/* driver.c */
extern NDIS_HANDLE g_NdisMiniportDriverHandle;

/* init.c */
NDIS_STATUS NTAPI
AR9485MiniportInitializeEx(
    _In_ NDIS_HANDLE NdisMiniportHandle,
    _In_ NDIS_HANDLE MiniportDriverContext,
    _In_ PNDIS_MINIPORT_INIT_PARAMETERS MiniportInitParameters);

VOID NTAPI
AR9485MiniportHaltEx(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ NDIS_HALT_ACTION HaltAction);

VOID NTAPI
AR9485MiniportDriverUnload(
    _In_ PDRIVER_OBJECT DriverObject);

NDIS_STATUS NTAPI
AR9485MiniportPause(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PNDIS_MINIPORT_PAUSE_PARAMETERS PauseParameters);

NDIS_STATUS NTAPI
AR9485MiniportRestart(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PNDIS_MINIPORT_RESTART_PARAMETERS RestartParameters);

VOID NTAPI
AR9485MiniportDevicePnPEventNotify(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PNET_DEVICE_PNP_EVENT NetDevicePnPEvent);

VOID NTAPI
AR9485MiniportShutdownEx(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ NDIS_SHUTDOWN_ACTION ShutdownAction);

/* interrupt.c */
NDIS_STATUS
AR9485RegisterInterrupt(_In_ PAR9485_ADAPTER Adapter);

VOID
AR9485UnregisterInterrupt(_In_ PAR9485_ADAPTER Adapter);

BOOLEAN NTAPI
AR9485Isr(
    _In_ NDIS_HANDLE MiniportInterruptContext,
    _Out_ PBOOLEAN QueueDefaultInterruptDpc,
    _Out_ PULONG TargetProcessors);

VOID NTAPI
AR9485InterruptDpc(
    _In_ NDIS_HANDLE MiniportInterruptContext,
    _In_ PVOID MiniportDpcContext,
    _In_ PVOID ReceiveThrottleParameters,
    _In_ PVOID NdisReserved2);

VOID NTAPI
AR9485DisableInterrupt(
    _In_ NDIS_HANDLE MiniportInterruptContext);

VOID NTAPI
AR9485EnableInterrupt(
    _In_ NDIS_HANDLE MiniportInterruptContext);

/* oid.c */
NDIS_STATUS NTAPI
AR9485OidRequest(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PNDIS_OID_REQUEST OidRequest);

VOID NTAPI
AR9485CancelOidRequest(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PVOID RequestId);

/* send.c */
VOID NTAPI
AR9485SendNetBufferLists(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PNET_BUFFER_LIST NetBufferLists,
    _In_ NDIS_PORT_NUMBER PortNumber,
    _In_ ULONG SendFlags);

VOID NTAPI
AR9485CancelSend(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PVOID CancelId);

/* receive.c */
VOID NTAPI
AR9485ReturnNetBufferLists(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PNET_BUFFER_LIST NetBufferLists,
    _In_ ULONG ReturnFlags);

#endif /* _AR9485_H_ */
