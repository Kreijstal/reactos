/*
 * PROJECT:     ReactOS Intel Wireless (iwlwifi) Driver
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     NDIS 6.20 native-802.11 miniport for Intel Wireless PCIe
 *              devices.  Shared adapter extension and forward declarations.
 *
 * WHY THIS DRIVER EXISTS ALONGSIDE ar9485.  ath9k parts are soft-MAC: the
 * host driver programs the PHY directly (init-val tables, calibration, RX
 * DMA arming) and the MLME lives in software.  Intel parts from family
 * 7000 on push PHY bring-up, calibration, scanning and association into
 * signed device firmware, so the host side reduces to (a) power the
 * transport up, (b) load the ucode, (c) plumb TX/RX rings, (d) speak a
 * versioned host<->firmware command API.  That is a far larger fraction of
 * mechanical, diffable work and a far smaller fraction of register
 * archaeology - which is why it is the better vehicle for broad device
 * coverage.
 *
 * PHASE 1a SCOPE (this file set): register the miniport, bind the PCI IDs
 * in hw/devices.c, map BAR0, run the documented transport power-up
 * (prepare-card-hw -> APM init -> finish-nic-init), read and decode
 * CSR_HW_REV / CSR_HW_RF_ID, locate and parse the matching .ucode
 * container, then report identification OIDs.  No ucode is pushed to the
 * device, no rings are built, no scan or association.  The datapath and
 * the firmware command API land in later phases.
 */

#ifndef _IWLWIFI_H_
#define _IWLWIFI_H_

#include <ndis.h>
#include <windot11.h>
#include <wdmguid.h>
#include <ntstrsafe.h>

/* Same NDIS 6.20/6.30 symbol backfill the sibling e1000e/ar9485 miniports
 * use.  When that header is promoted to sdk/include/ddk/ndis6_compat.h this
 * include follows it. */
#include "../e1000e/ndis6_compat.h"

#include "hw/csr.h"
#include "hw/devices.h"
#include "fw/ucode_file.h"

#define IWL_TAG                     'lwiI'

#define IWL_FLAG_HW_RECOGNIZED      0x00000001
#define IWL_FLAG_HALTING            0x00000002
#define IWL_FLAG_APM_UP             0x00000004
#define IWL_FLAG_FW_LOADED          0x00000008
#define IWL_FLAG_PNVM_LOADED        0x00000010

/* NDIS carries an 802.3-form address even for a native-802.11 miniport. */
#define IWL_MAC_ADDRESS_LENGTH      6

/* Longest "iwlwifi-<pre>-<api>.ucode" we will ever build. */
#define IWL_MAX_FW_NAME             64

typedef struct _IWL_ADAPTER
{
    NDIS_HANDLE             MiniportAdapterHandle;
    NDIS_HANDLE             NdisMiniportDriverHandle;
    PDEVICE_OBJECT          PhysicalDeviceObject;

    /* PCI config-space access.  Not optional: the device ID selects the
     * bring-up path, so a driver that could not read it would be guessing.
     * There is deliberately no HalGetBusDataByOffset(bus 0, slot 0)
     * fallback here - that reads whatever device happens to sit at 0:0 and
     * would cheerfully identify the wrong part. */
    BUS_INTERFACE_STANDARD  BusInterface;
    BOOLEAN                 BusInterfaceValid;

    /* PCI identity */
    USHORT                  VendorId;
    USHORT                  DeviceId;
    USHORT                  SubsystemId;
    UCHAR                   RevisionId;

    /* Matched row of hw/devices.c's table.  NULL is impossible past
     * MiniportInitializeEx's identification step - the miniport refuses to
     * initialize on an unknown part rather than guessing a bring-up
     * sequence. */
    const IWL_DEVICE_CFG   *Cfg;

    /* BAR0 mapping.  The CSR block is the first 1 KiB; everything past it
     * needs the APM powered up first. */
    PHYSICAL_ADDRESS        IoAddress;
    ULONG                   IoLength;
    PVOID                   IoBase;

    /* Silicon revision, read from CSR_HW_REV once the APM is in D0A. */
    ULONG                   HwRev;
    ULONG                   HwRevStep;
    ULONG                   HwRevDash;
    ULONG                   HwRfId;

    /* Firmware image located and parsed at init.  FwImage is pool memory
     * owned by the adapter; FwParsed's sections point INTO it and must not
     * outlive it - IwlFreeFirmware() releases both together.  FwParsed is
     * a separate allocation because IWL_FW_PARSED carries a 128-entry
     * section array per image and has no business inflating every
     * adapter context by that much. */
    CHAR                    FwName[IWL_MAX_FW_NAME];
    PVOID                   FwImage;
    ULONG                   FwImageLength;
    IWL_FW_PARSED          *FwParsed;

    /* Platform NVM, AX210 and later only (IWL_CFG_NEEDS_PNVM).  Same
     * ownership rule as the firmware: PnvmParsed's sections point into
     * PnvmImage.  Which of its SKU blocks applies cannot be decided here -
     * the SKU ID comes from the firmware's ALIVE response - so this stage
     * only proves the blob is present and well formed and reports the
     * blocks it offers. */
    CHAR                    PnvmName[IWL_MAX_FW_NAME];
    PVOID                   PnvmImage;
    ULONG                   PnvmImageLength;
    IWL_PNVM_PARSED        *PnvmParsed;

    /* Interrupt resource */
    ULONG                   InterruptVector;
    KIRQL                   InterruptLevel;
    KAFFINITY               InterruptAffinity;
    KINTERRUPT_MODE         InterruptModeType;
    BOOLEAN                 InterruptShared;
    BOOLEAN                 HasMessageInterrupt;
    NDIS_HANDLE             InterruptHandle;

    /* Permanent address.  Family 9000+ reports it through the firmware's
     * NVM access command, which Phase 2 introduces; until then the
     * miniport has no address to report and says so rather than inventing
     * one. */
    UCHAR                   PermanentMacAddress[IWL_MAC_ADDRESS_LENGTH];
    UCHAR                   CurrentMacAddress[IWL_MAC_ADDRESS_LENGTH];
    BOOLEAN                 MacAddressValid;

    LONG                    Flags;
} IWL_ADAPTER, *PIWL_ADAPTER;

/* ------------------------------------------------------------------ */
/* Register access.  BAR0 is mapped non-cached; READ/WRITE_REGISTER_*  */
/* carry the ordering guarantees the device needs.                     */
/* ------------------------------------------------------------------ */

FORCEINLINE ULONG
IwlRead32(_In_ PIWL_ADAPTER Adapter, _In_ ULONG Offset)
{
    return READ_REGISTER_ULONG((PULONG)((PUCHAR)Adapter->IoBase + Offset));
}

FORCEINLINE VOID
IwlWrite32(_In_ PIWL_ADAPTER Adapter, _In_ ULONG Offset, _In_ ULONG Value)
{
    WRITE_REGISTER_ULONG((PULONG)((PUCHAR)Adapter->IoBase + Offset), Value);
}

FORCEINLINE VOID
IwlSetBit(_In_ PIWL_ADAPTER Adapter, _In_ ULONG Offset, _In_ ULONG Mask)
{
    IwlWrite32(Adapter, Offset, IwlRead32(Adapter, Offset) | Mask);
}

FORCEINLINE VOID
IwlClearBit(_In_ PIWL_ADAPTER Adapter, _In_ ULONG Offset, _In_ ULONG Mask)
{
    IwlWrite32(Adapter, Offset, IwlRead32(Adapter, Offset) & ~Mask);
}

/* ------------------------------------------------------------------ */
/* hw/trans.c - transport power-up                                     */
/* ------------------------------------------------------------------ */

/* Busy-wait or sleep for the requested microseconds.  PASSIVE_LEVEL only
 * for anything above IWL_STALL_MAX_US. */
VOID
IwlDelayUs(_In_ ULONG Microseconds);

/*
 * Poll Offset until (value & Mask) == Expected, or Timeout microseconds
 * elapse.  Returns TRUE on match.  Mirrors Linux's iwl_poll_bit().
 */
BOOLEAN
IwlPollBit(
    _In_ PIWL_ADAPTER Adapter,
    _In_ ULONG Offset,
    _In_ ULONG Expected,
    _In_ ULONG Mask,
    _In_ ULONG TimeoutUs);

/* Take ownership of the device from firmware/BIOS.  iwl_pcie_prepare_card_hw(). */
NDIS_STATUS
IwlPrepareCardHw(_In_ PIWL_ADAPTER Adapter);

/* Move the device out of low power into D0A.  iwl_pcie_apm_init(). */
NDIS_STATUS
IwlApmInit(_In_ PIWL_ADAPTER Adapter);

/* Stop the DMA master and put the APM back to sleep.  iwl_pcie_apm_stop(). */
VOID
IwlApmStop(_In_ PIWL_ADAPTER Adapter);

/* Assert the SW reset bit.  iwl_trans_pcie_sw_reset(). */
VOID
IwlSwReset(_In_ PIWL_ADAPTER Adapter);

/* Mask every CSR interrupt source and ack whatever was pending. */
VOID
IwlDisableInterrupts(_In_ PIWL_ADAPTER Adapter);

/* ------------------------------------------------------------------ */
/* fw/ucode_load.c - locating and reading the firmware container       */
/* ------------------------------------------------------------------ */

/*
 * Walk the device's API range from UcodeApiMax down to UcodeApiMin,
 * reading the first iwlwifi-<pre>-<api>.ucode that exists out of
 * \SystemRoot\System32\drivers\iwlwifi\ into pool memory, then parse it.
 * PASSIVE_LEVEL only.
 */
NDIS_STATUS
IwlLoadFirmware(_In_ PIWL_ADAPTER Adapter);

VOID
IwlFreeFirmware(_In_ PIWL_ADAPTER Adapter);

/*
 * Read and parse iwlwifi-<pre>.pnvm for parts that need one.  Not called
 * for parts without IWL_CFG_NEEDS_PNVM.  PASSIVE_LEVEL only.
 */
NDIS_STATUS
IwlLoadPnvm(_In_ PIWL_ADAPTER Adapter);

VOID
IwlFreePnvm(_In_ PIWL_ADAPTER Adapter);

/* ------------------------------------------------------------------ */
/* driver.c                                                            */
/* ------------------------------------------------------------------ */

extern NDIS_HANDLE g_NdisMiniportDriverHandle;

/* ------------------------------------------------------------------ */
/* init.c                                                              */
/* ------------------------------------------------------------------ */

NDIS_STATUS NTAPI
IwlMiniportInitializeEx(
    _In_ NDIS_HANDLE NdisMiniportHandle,
    _In_ NDIS_HANDLE MiniportDriverContext,
    _In_ PNDIS_MINIPORT_INIT_PARAMETERS MiniportInitParameters);

VOID NTAPI
IwlMiniportHaltEx(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ NDIS_HALT_ACTION HaltAction);

VOID NTAPI
IwlMiniportDriverUnload(
    _In_ PDRIVER_OBJECT DriverObject);

NDIS_STATUS NTAPI
IwlMiniportPause(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PNDIS_MINIPORT_PAUSE_PARAMETERS PauseParameters);

NDIS_STATUS NTAPI
IwlMiniportRestart(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PNDIS_MINIPORT_RESTART_PARAMETERS RestartParameters);

VOID NTAPI
IwlMiniportDevicePnPEventNotify(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PNET_DEVICE_PNP_EVENT NetDevicePnPEvent);

VOID NTAPI
IwlMiniportShutdownEx(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ NDIS_SHUTDOWN_ACTION ShutdownAction);

/* ------------------------------------------------------------------ */
/* interrupt.c                                                         */
/* ------------------------------------------------------------------ */

NDIS_STATUS
IwlRegisterInterrupt(_In_ PIWL_ADAPTER Adapter);

VOID
IwlUnregisterInterrupt(_In_ PIWL_ADAPTER Adapter);

BOOLEAN NTAPI
IwlIsr(
    _In_ NDIS_HANDLE MiniportInterruptContext,
    _Out_ PBOOLEAN QueueDefaultInterruptDpc,
    _Out_ PULONG TargetProcessors);

VOID NTAPI
IwlInterruptDpc(
    _In_ NDIS_HANDLE MiniportInterruptContext,
    _In_ PVOID MiniportDpcContext,
    _In_ PVOID ReceiveThrottleParameters,
    _In_ PVOID NdisReserved2);

VOID NTAPI
IwlDisableInterruptHandler(_In_ NDIS_HANDLE MiniportInterruptContext);

VOID NTAPI
IwlEnableInterruptHandler(_In_ NDIS_HANDLE MiniportInterruptContext);

/* ------------------------------------------------------------------ */
/* oid.c                                                               */
/* ------------------------------------------------------------------ */

NDIS_STATUS NTAPI
IwlOidRequest(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PNDIS_OID_REQUEST OidRequest);

VOID NTAPI
IwlCancelOidRequest(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PVOID RequestId);

/* ------------------------------------------------------------------ */
/* send.c / receive.c                                                  */
/* ------------------------------------------------------------------ */

VOID NTAPI
IwlSendNetBufferLists(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PNET_BUFFER_LIST NetBufferLists,
    _In_ NDIS_PORT_NUMBER PortNumber,
    _In_ ULONG SendFlags);

VOID NTAPI
IwlCancelSend(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PVOID CancelId);

VOID NTAPI
IwlReturnNetBufferLists(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PNET_BUFFER_LIST NetBufferLists,
    _In_ ULONG ReturnFlags);

#endif /* _IWLWIFI_H_ */
