/*
 * PROJECT:         ReactOS xHCI Driver
 * LICENSE:         GPLv2+ - See COPYING in the top level directory
 * PURPOSE:         private xHCI functions and definitions
 * COPYRIGHT:       Copyright 2021 Justin Miller <justinmiller100@gmail.com>
 */

#pragma once

#include "usbxhci.h"

/* Private Functions ******************************************************************************/

/* Forward declarations */
typedef struct _XHCI_PENDING_COMMAND *PXHCI_PENDING_COMMAND;

/* Command type enumeration */
typedef enum {
    COMMAND_ENABLE_SLOT,
    COMMAND_ADDRESS_DEVICE,
    COMMAND_DISABLE_SLOT,
    COMMAND_CONFIGURE_ENDPOINT,
    COMMAND_DROP_ENDPOINT,
    COMMAND_SET_TR_DEQUEUE_POINTER,
    COMMAND_UNKNOWN
} XHCI_COMMAND_TYPE;

MPSTATUS
NTAPI
PXHCI_ControllerWorkTest(IN PXHCI_EXTENSION XhciExtension,
                         IN PXHCI_HC_RESOURCES HcResourcesVA,
                         IN PVOID resourcesStartPA);

VOID
NTAPI
PXHCI_PortStatusChange(IN PXHCI_EXTENSION XhciExtension, 
                       IN ULONG PortID);

VOID  
NTAPI
PXHCI_AssignSlot(IN PXHCI_EXTENSION XhciExtension, IN ULONG PortID);

VOID
NTAPI
PXHCI_InitSlot(IN PXHCI_EXTENSION XhciExtension, IN ULONG PortID, IN ULONG SlotID);

VOID
NTAPI
XHCI_Write64bitReg(IN PULONG BaseAddr,
                   IN ULONGLONG Data);


/* Transfer type functions ************************************************************************/

MPSTATUS
NTAPI
PXHCI_InitTransferBulk(PVOID xhciExtension);

MPSTATUS
NTAPI
PXHCI_InitTransferInterrupt(PVOID xhciExtension);

MPSTATUS
NTAPI
PXHCI_InitTransferIso(PVOID xhciExtension);

MPSTATUS
NTAPI
PXHCI_InitTransferControl(PVOID xhciExtension);

/* endpoint type functions ************************************************************************/

MPSTATUS
NTAPI
XHCI_OpenIsoEndpoint(IN PXHCI_EXTENSION XhciExtension,
                     IN PUSBPORT_ENDPOINT_PROPERTIES EndpointProperties,
                     IN PXHCI_ENDPOINT  XhciEndpoint);

MPSTATUS
NTAPI
XHCI_OpenControlEndpoint(IN PXHCI_EXTENSION XhciExtension,
                         IN PUSBPORT_ENDPOINT_PROPERTIES EndpointProperties,
                         IN PXHCI_ENDPOINT  XhciEndpoint);
MPSTATUS
NTAPI
XHCI_OpenBulkEndpoint(IN PXHCI_EXTENSION XhciExtension,
                      IN PUSBPORT_ENDPOINT_PROPERTIES EndpointProperties,
                      IN PXHCI_ENDPOINT  XhciEndpoint);
MPSTATUS
NTAPI
XHCI_OpenInterruptEndpoint(IN PXHCI_EXTENSION XhciExtension,
                           IN PUSBPORT_ENDPOINT_PROPERTIES EndpointProperties,
                           IN PXHCI_ENDPOINT  XhciEndpoint);

/* Transfer submission functions *************************************************************************/

MPSTATUS
NTAPI
XHCI_SubmitControlTransfer(IN PXHCI_EXTENSION XhciExtension,
                          IN PXHCI_ENDPOINT XhciEndpoint,
                          IN PXHCI_TRANSFER XhciTransfer,
                          IN PUSBPORT_SCATTER_GATHER_LIST SgList);

MPSTATUS
NTAPI
XHCI_SubmitBulkTransfer(IN PXHCI_EXTENSION XhciExtension,
                       IN PXHCI_ENDPOINT XhciEndpoint,
                       IN PXHCI_TRANSFER XhciTransfer,
                       IN PUSBPORT_SCATTER_GATHER_LIST SgList);

MPSTATUS
NTAPI
XHCI_SubmitInterruptTransfer(IN PXHCI_EXTENSION XhciExtension,
                            IN PXHCI_ENDPOINT XhciEndpoint,
                            IN PXHCI_TRANSFER XhciTransfer,
                            IN PUSBPORT_SCATTER_GATHER_LIST SgList);

MPSTATUS
NTAPI
XHCI_SubmitIsochronousTransfer(IN PXHCI_EXTENSION XhciExtension,
                              IN PXHCI_ENDPOINT XhciEndpoint,
                              IN PXHCI_TRANSFER XhciTransfer,
                              IN PUSBPORT_SCATTER_GATHER_LIST SgList);

/* Device context and TRB management functions *********************************************************/

#define XHCI_TRANSFER_RING_LINK_INDEX (XHCI_RING_TRB_COUNT - 1)
#define XHCI_MAX_BULK_NORMAL_TRBS XHCI_TRANSFER_RING_LINK_INDEX
#define XHCI_MAX_NORMAL_TRB_TRANSFER_LENGTH 0x10000

MPSTATUS
NTAPI
XHCI_BuildBulkNormalTrbs(IN PUSBPORT_SCATTER_GATHER_LIST SgList,
                         IN ULONG TransferLength,
                         OUT PXHCI_TRB Trbs,
                         IN ULONG MaxTrbs,
                         OUT PULONG TrbCount);

MPSTATUS
NTAPI
XHCI_CreateDataTRB(IN PXHCI_EXTENSION XhciExtension,
                   IN PXHCI_ENDPOINT XhciEndpoint,
                   IN PXHCI_TRANSFER XhciTransfer,
                   IN PHYSICAL_ADDRESS BufferPA,
                   IN ULONG Length,
                   IN BOOLEAN ChainBit,
                   IN BOOLEAN InterruptOnCompletion,
                   OUT PXHCI_TRB DataTrb);

MPSTATUS
NTAPI
XHCI_CreateControlTRBs(IN PXHCI_EXTENSION XhciExtension,
                       IN PXHCI_ENDPOINT XhciEndpoint,
                       IN PXHCI_TRANSFER XhciTransfer);

MPSTATUS
NTAPI
XHCI_RingDoorbell(IN PXHCI_EXTENSION XhciExtension,
                  IN ULONG SlotId,
                  IN ULONG EndpointIndex);

/* Event processing functions ***************************************************************************/

VOID
NTAPI
XHCI_ProcessTransferEvent(IN PXHCI_EXTENSION XhciExtension,
                         IN PXHCI_EVENT_TRB EventTrb);

VOID
NTAPI
XHCI_ProcessCommandCompletion(IN PXHCI_EXTENSION XhciExtension,
                             IN PXHCI_EVENT_TRB EventTrb);

VOID
NTAPI
XHCI_CompleteTransfer(IN PXHCI_EXTENSION XhciExtension,
                     IN ULONG SlotId,
                     IN ULONG EndpointId,
                     IN PHYSICAL_ADDRESS TrbPointer,
                     IN ULONG TransferLength,
                     IN ULONG USBDStatus);

/* Command and transfer tracking functions *************************************************************/

VOID
NTAPI
InitializeCommandTracking(VOID);

VOID
NTAPI
InitializeTransferTracking(VOID);

/* Device enumeration and management functions *************************************************/

MPSTATUS
NTAPI
WaitForCommandCompletion(IN PXHCI_EXTENSION XhciExtension,
                        IN PXHCI_PENDING_COMMAND PendingCommand,
                        IN ULONG TimeoutMs);

ULONG
NTAPI
GetDeviceSpeedFromPort(IN PXHCI_EXTENSION XhciExtension, IN ULONG PortNumber);

ULONG
NTAPI
GetMaxPacketSizeForSpeed(IN ULONG DeviceSpeed);

MPSTATUS
NTAPI
XHCI_EnumerateDevice(IN PXHCI_EXTENSION XhciExtension,
                    IN ULONG PortNumber,
                    OUT PULONG SlotId,
                    OUT PULONG DeviceAddress);

ULONG
NTAPI
XHCI_GetDeviceAddressFromOutputContext(IN PXHCI_EXTENSION XhciExtension,
                                      IN ULONG SlotId);

/* xHCI command functions *********************************************************************/

MPSTATUS
NTAPI
XHCI_EnableSlot(IN PXHCI_EXTENSION XhciExtension,
               IN ULONG PortNumber,
               OUT PULONG SlotId);

MPSTATUS
NTAPI
XHCI_DisableSlot(IN PXHCI_EXTENSION XhciExtension,
                IN ULONG SlotId);

VOID
NTAPI
CleanupSlotResources(IN PXHCI_EXTENSION XhciExtension,
                    IN ULONG SlotId);

MPSTATUS
NTAPI
XHCI_AddressDevice(IN PXHCI_EXTENSION XhciExtension,
                  IN ULONG SlotId,
                  IN PHYSICAL_ADDRESS InputContextPA,
                  IN BOOLEAN BlockSetRequest);

MPSTATUS
NTAPI
XHCI_SetupDeviceContext(IN PXHCI_EXTENSION XhciExtension,
                       IN ULONG SlotId,
                       IN ULONG MaxPacketSize,
                       IN ULONG DeviceSpeed,
                       IN PXHCI_ENDPOINT XhciEndpoint,
                       IN ULONG PortNumber);

MPSTATUS
NTAPI
XHCI_SetupDCBAAEntry(IN PXHCI_EXTENSION XhciExtension,
                     IN ULONG SlotId);

MPSTATUS
NTAPI
XHCI_SetTransferRingDequeuePointer(IN PXHCI_EXTENSION XhciExtension,
                                   IN ULONG SlotId,
                                   IN ULONG EndpointIndex,
                                   IN PHYSICAL_ADDRESS DequeuePointer,
                                   IN ULONG CycleState);

MPSTATUS
NTAPI
XHCI_ConfigureEndpoint(IN PXHCI_EXTENSION XhciExtension,
                       IN ULONG SlotId,
                       IN ULONG EndpointIndex,
                       IN ULONG EndpointType,
                       IN ULONG MaxPacketSize,
                       IN ULONG Interval,
                       IN PHYSICAL_ADDRESS TransferRingPA);

MPSTATUS
NTAPI
XHCI_DropEndpoint(IN PXHCI_EXTENSION XhciExtension,
                  IN ULONG SlotId,
                  IN ULONG EndpointIndex);

ULONG
NTAPI
XHCI_GetCurrentFrameNumber(IN PXHCI_EXTENSION XhciExtension);

/* Transfer ring management functions *********************************************************************/

MPSTATUS
NTAPI
XHCI_InitializeTransferRing(IN PXHCI_RING TransferRing);

/* Transfer submission functions *************************************************************************/
