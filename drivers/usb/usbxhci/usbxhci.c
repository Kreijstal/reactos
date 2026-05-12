/*
 * PROJECT:         ReactOS xHCI Driver
 * LICENSE:         GPLv2+ - See COPYING in the top level directory
 * PURPOSE:         main functions of xHCI
 * PROGRAMMER:      Rama Teja Gampa <ramateja.g@gmail.com>
 * 
 * STATUS:          Working implementation with the following features:
 *                  - Device enumeration and configuration
 *                  - Control transfers (fully functional)
 *                  - Interrupt transfers (fully functional)
 *                  - Transfer completion event handling
 *                  - Proper endpoint reopening after transfer completion
 *                  - Transfer ring dequeue pointer management
 *                  - Graceful handling of unimplemented kernel functions
 *                  
 *                  TODO:
 *                  - Bulk transfers (not yet implemented)
 *                  - Isochronous transfers (not yet implemented)
 *                  - Enhanced error handling and recovery
 *                  - Device context management improvements
 *                  - Endpoint cleanup and resource management
*/
#include "usbxhcip.h"
#define NDEBUG
#include <debug.h>
#define NDEBUG_XHCI_TRACE
#include "dbg_xhci.h"

/* Globals ****************************************************************************************/

typedef struct _XHCI_PENDING_COMMAND {
    XHCI_COMMAND_TYPE CommandType;
    PHYSICAL_ADDRESS TrbPointer;
    ULONG SlotId;
    ULONG CompletionCode;
    ULONG CompletionSlotId;  // Slot ID returned from completion event
    BOOLEAN Completed;
    BOOLEAN InUse;
} XHCI_PENDING_COMMAND, *PXHCI_PENDING_COMMAND;

USBPORT_REGISTRATION_PACKET RegPacket;
VOID RemovePendingCommand(PXHCI_PENDING_COMMAND PendingCommand);
PXHCI_PENDING_COMMAND
AddPendingCommand(XHCI_COMMAND_TYPE CommandType, PHYSICAL_ADDRESS TrbPointer, ULONG SlotId);
/* Public Functions *******************************************************************************/

VOID
NTAPI
XHCI_Write64bitReg(IN PULONG BaseAddr,
                   IN ULONGLONG Data)
{
    WRITE_REGISTER_ULONG(BaseAddr, Data);
    WRITE_REGISTER_ULONG(BaseAddr + 1, Data >> 32);
}

MPSTATUS
NTAPI
XHCI_OpenEndpoint(IN PVOID xhciExtension,
                  IN PUSBPORT_ENDPOINT_PROPERTIES  endpointParameters,
                  IN PVOID xhciEndpoint)
{
    PUSBPORT_ENDPOINT_PROPERTIES EndpointProperties = endpointParameters;
    PXHCI_EXTENSION XhciExtension = (PXHCI_EXTENSION)xhciExtension;
    PXHCI_ENDPOINT XhciEndpoint = xhciEndpoint;
    ULONG TransferType;
    MPSTATUS MPStatus;

    DPRINT("XHCI_OpenEndpoint: function initiated\n");
    DPRINT("XHCI_OpenEndpoint: EndpointProperties=%p, DeviceAddress=%d, EndpointAddress=%d, TransferType=%d, MaxPacketSize=%d\n",
            EndpointProperties, 
            EndpointProperties ? EndpointProperties->DeviceAddress : -1,
            EndpointProperties ? EndpointProperties->EndpointAddress : -1,
            EndpointProperties ? EndpointProperties->TransferType : -1,
            EndpointProperties ? EndpointProperties->MaxPacketSize : -1);
            
    if (!EndpointProperties || !XhciExtension || !XhciEndpoint) {
        DPRINT("XHCI_OpenEndpoint: Invalid parameters - EndpointProperties=%p, XhciExtension=%p, XhciEndpoint=%p\n",
                EndpointProperties, XhciExtension, XhciEndpoint);
        return MP_STATUS_FAILURE;
    }
    
    TransferType = EndpointProperties->TransferType;

    switch (TransferType)
    {
        case USBPORT_TRANSFER_TYPE_ISOCHRONOUS:
            MPStatus = XHCI_OpenIsoEndpoint(XhciExtension,
                                            EndpointProperties,
                                            XhciEndpoint);
            break;

        case USBPORT_TRANSFER_TYPE_CONTROL:
            MPStatus = XHCI_OpenControlEndpoint(XhciExtension,
                                                EndpointProperties,
                                                XhciEndpoint);
            break;

        case USBPORT_TRANSFER_TYPE_BULK:
            MPStatus = XHCI_OpenBulkEndpoint(XhciExtension,
                                             EndpointProperties,
                                             XhciEndpoint);
            break;

        case USBPORT_TRANSFER_TYPE_INTERRUPT:
            MPStatus = XHCI_OpenInterruptEndpoint(XhciExtension,
                                                  EndpointProperties,
                                                  XhciEndpoint);
            break;
        default:
            DPRINT("XHCI_OpenEndpoint: Unsupported transfer type %d\n", TransferType);
            return MP_STATUS_NOT_SUPPORTED;
            break;
    }

    DPRINT("XHCI_OpenEndpoint: Endpoint open completed with status 0x%x (MP_STATUS_SUCCESS=0x%x)\n", 
            MPStatus, MP_STATUS_SUCCESS);
    return MPStatus;
}

MPSTATUS
NTAPI
XHCI_ReopenEndpoint(IN PVOID xhciExtension,
                    IN PUSBPORT_ENDPOINT_PROPERTIES  endpointParameters,
                    IN PVOID  xhciEndpoint)
{
    PXHCI_EXTENSION XhciExtension = (PXHCI_EXTENSION)xhciExtension;
    PXHCI_ENDPOINT XhciEndpoint = (PXHCI_ENDPOINT)xhciEndpoint;
    MPSTATUS Status;

    DPRINT1("XHCI_ReopenEndpoint: function initiated\n");
    
    if (!XhciExtension || !XhciEndpoint || !endpointParameters)
    {
        DPRINT1("XHCI_ReopenEndpoint: Invalid parameters\n");
        return MP_STATUS_FAILURE;
    }
    
    DPRINT1("XHCI_ReopenEndpoint: Reopening endpoint for device %d, endpoint %d\n",
            endpointParameters->DeviceAddress, endpointParameters->EndpointAddress);
    
    // Validate endpoint address for control endpoint (should be 0)
    if (endpointParameters->EndpointAddress != 0)
    {
        DPRINT1("XHCI_ReopenEndpoint: WARNING - Non-control endpoint reopen (addr=%d), may need special handling\n",
                endpointParameters->EndpointAddress);
    }
    
    // Reinitialize the transfer ring to ensure clean state
    DPRINT1("XHCI_ReopenEndpoint: Reinitializing transfer ring for device %d\n", 
            endpointParameters->DeviceAddress);
    
    Status = XHCI_InitializeTransferRing(&XhciEndpoint->TransferRing);
    if (Status != MP_STATUS_SUCCESS)
    {
        DPRINT1("XHCI_ReopenEndpoint: Failed to reinitialize transfer ring\n");
        return Status;
    }
    
    // CRITICAL FIX: Update the xHCI controller's endpoint context with the new transfer ring state
    // This tells the controller where to find the new transfer ring after reinitialization
    // Retrieve the slot ID from FirstTD (stored during endpoint creation), NOT from DeviceAddress
    ULONG SlotId = *(PULONG)&XhciEndpoint->FirstTD;
    ULONG EndpointIndex = ((endpointParameters->EndpointAddress & 0x0F) * 2) + 
                          ((endpointParameters->EndpointAddress & 0x80) ? 1 : 0);  // Convert to endpoint index
    
    // Calculate physical address of the new transfer ring start
    PHYSICAL_ADDRESS RingDequeuePA = MmGetPhysicalAddress(XhciEndpoint->TransferRing.dequeue_pointer);
    
    DPRINT1("XHCI_ReopenEndpoint: Sending Set TR Dequeue Pointer command for slot %d, endpoint %d\n", 
            SlotId, EndpointIndex);
    DPRINT1("XHCI_ReopenEndpoint: New dequeue pointer PA=0x%I64x, cycle state=%d\n", 
            RingDequeuePA.QuadPart, XhciEndpoint->TransferRing.ConsumerCycleState);
    
    Status = XHCI_SetTransferRingDequeuePointer(XhciExtension,
                                               SlotId,
                                               EndpointIndex,
                                               RingDequeuePA,
                                               XhciEndpoint->TransferRing.ConsumerCycleState);
    if (Status != MP_STATUS_SUCCESS)
    {
        DPRINT1("XHCI_ReopenEndpoint: Failed to set transfer ring dequeue pointer, status=0x%x\n", Status);
        return Status;
    }
    
    // CRITICAL: After Set TR Dequeue Pointer command, the endpoint is in stopped state.
    // We need to restart it by ringing the doorbell to allow transfers to be processed.
    DPRINT1("XHCI_ReopenEndpoint: Restarting endpoint by ringing doorbell for slot %d, endpoint %d\n", 
            SlotId, EndpointIndex);
    
    // Ring the doorbell to restart the endpoint
    Status = XHCI_RingDoorbell(XhciExtension, SlotId, EndpointIndex);
    if (Status != MP_STATUS_SUCCESS)
    {
        DPRINT1("XHCI_ReopenEndpoint: Failed to ring doorbell for slot %d, endpoint %d\n", SlotId, EndpointIndex);
        // Don't fail the reopen for this, as the endpoint might still work
    }
    else
    {
        DPRINT1("XHCI_ReopenEndpoint: Doorbell rung successfully for slot %d, endpoint %d\n", SlotId, EndpointIndex);
    }
    
    // Update endpoint properties (in case they changed)
    XhciEndpoint->EndpointProperties = *endpointParameters;
    
    // Mark endpoint as active
    XhciEndpoint->EndpointState = USBPORT_ENDPOINT_ACTIVE;
    
    DPRINT1("XHCI_ReopenEndpoint: Endpoint reopened successfully with updated controller state and restarted\n");
    return MP_STATUS_SUCCESS;
}

VOID
NTAPI
XHCI_QueryEndpointRequirements(IN PVOID xhciExtension,
                               IN PUSBPORT_ENDPOINT_PROPERTIES  endpointParameters,
                               IN PUSBPORT_ENDPOINT_REQUIREMENTS EndpointRequirements)
{
    PUSBPORT_ENDPOINT_PROPERTIES EndpointProperties = endpointParameters;
    PXHCI_EXTENSION XhciExtension;
    ULONG TransferType;
    static ULONG QueryCount = 0;

    QueryCount++;
    if ((QueryCount % 100) == 1) // Reduced debug frequency
    {
        DPRINT1("XHCI_QueryEndpointRequirements: query #%d initiated\n", QueryCount);
    }
    
    TransferType = EndpointProperties->TransferType;
    XhciExtension = (PXHCI_EXTENSION)xhciExtension;
    
    // Only process events much less frequently to reduce overhead
    // and only for certain query counts to avoid constant processing
    if ((QueryCount % 50) == 1)
    {
        XHCI_ProcessEvent(XhciExtension);
    }
    
    switch (TransferType)
    {
        case USBPORT_TRANSFER_TYPE_ISOCHRONOUS:
            if ((QueryCount % 50) == 1)
            {
                DPRINT1("XHCI_QueryEndpointRequirements: IsoTransfer\n");
            }
            EndpointRequirements->MaxTransferSize = EHCI_MAX_HS_ISO_TRANSFER_SIZE;
            break;

        case USBPORT_TRANSFER_TYPE_CONTROL:
            EndpointRequirements->HeaderBufferSize = sizeof(XHCI_HCD_TD) +
                                                     sizeof(XHCI_TRANSFER_RING) +
                                                     EHCI_MAX_CONTROL_TD_COUNT * sizeof(XHCI_HCD_TD);
            EndpointRequirements->MaxTransferSize = EHCI_MAX_CONTROL_TRANSFER_SIZE;
            break;

        case USBPORT_TRANSFER_TYPE_BULK:
            EndpointRequirements->HeaderBufferSize = sizeof(XHCI_HCD_TD) +
                                                     sizeof(XHCI_TRANSFER_RING) +
                                                     EHCI_MAX_BULK_TD_COUNT * sizeof(XHCI_HCD_TD);

            EndpointRequirements->MaxTransferSize = EHCI_MAX_BULK_TRANSFER_SIZE;
            break;

        case USBPORT_TRANSFER_TYPE_INTERRUPT:
            EndpointRequirements->HeaderBufferSize = sizeof(XHCI_HCD_TD) +
                                                     sizeof(XHCI_TRANSFER_RING) +
                                                     EHCI_MAX_INTERRUPT_TD_COUNT * sizeof(XHCI_HCD_TD);

            EndpointRequirements->MaxTransferSize = EHCI_MAX_INTERRUPT_TRANSFER_SIZE;
            break;

        default:
            DPRINT1("XHCI_QueryEndpointRequirements: Unknown TransferType - %x\n",
                    TransferType);
            DbgBreakPoint();
            break;
    }
}

VOID
NTAPI
XHCI_CloseEndpoint(IN PVOID xhciExtension,
                   IN PVOID xhciEndpoint,
                   IN BOOLEAN IsDoDisablePeriodic)
{
    PXHCI_EXTENSION XhciExtension = (PXHCI_EXTENSION)xhciExtension;
    PXHCI_ENDPOINT XhciEndpoint = (PXHCI_ENDPOINT)xhciEndpoint;
    ULONG DeviceAddress;
    ULONG SlotId;

    DPRINT1("XHCI_CloseEndpoint: function initiated\n");

    if (!XhciExtension || !XhciEndpoint)
    {
        DPRINT1("XHCI_CloseEndpoint: Invalid parameters\n");
        return;
    }

    DeviceAddress = XhciEndpoint->EndpointProperties.DeviceAddress;
    // Retrieve the slot ID from FirstTD, where it was stored during endpoint creation
    SlotId = *(PULONG)&XhciEndpoint->FirstTD;

    DPRINT1("XHCI_CloseEndpoint: Closing endpoint for device %d, endpoint %d, slot %d\n",
            DeviceAddress, XhciEndpoint->EndpointProperties.EndpointAddress, SlotId);
    
    // Mark endpoint as inactive first
    XhciEndpoint->EndpointState = USBPORT_ENDPOINT_REMOVE;
    
    // Clear any pending transfers from the transfer ring
    DPRINT1("XHCI_CloseEndpoint: Clearing transfer ring for device %d\n", DeviceAddress);
    // Reset transfer ring to clean state - this will clear any pending TRBs
    
    // When closing EP0 of an addressed device (not during default-address
    // enumeration), disable the hardware slot so it can be reused.
    // DeviceAddress == 0 means the initial EP0 before SET_ADDRESS;
    // we must NOT disable the slot in that case.
    if (XhciEndpoint->EndpointProperties.EndpointAddress == 0 &&
        SlotId != 0 && DeviceAddress != 0)
    {
        DPRINT1("XHCI_CloseEndpoint: Disabling slot %d for device %d\n", SlotId, DeviceAddress);
        XHCI_DisableSlot(XhciExtension, SlotId);
        // Also clean up software slot tracking
        CleanupSlotResources(XhciExtension, SlotId);
    }
    
    // Clean up the endpoint structure
    if (!IsListEmpty(&XhciEndpoint->ListTDs))
    {
        DPRINT1("XHCI_CloseEndpoint: Warning - endpoint has pending transfers\n");
        // TODO: Complete or cancel pending transfers
    }
    
    DPRINT1("XHCI_CloseEndpoint: Endpoint closed\n");
}

MPSTATUS
NTAPI
XHCI_ProcessEvent (IN PXHCI_EXTENSION XhciExtension)
{
    PXHCI_HC_RESOURCES HcResourcesVA;
    PHYSICAL_ADDRESS HcResourcesPA;
    XHCI_EVENT_RING_DEQUEUE_POINTER erstdp;
    PULONG  RunTimeRegisterBase;
    PXHCI_TRB dequeue_pointer;
    ULONG TRBType;
    XHCI_EVENT_TRB eventTRB;
    ULONG EventsProcessed = 0;

    HcResourcesVA = XhciExtension -> HcResourcesVA;
    HcResourcesPA = XhciExtension -> HcResourcesPA;

    RunTimeRegisterBase = XhciExtension-> RunTimeRegisterBase;
    dequeue_pointer = HcResourcesVA-> EventRing.dequeue_pointer;

    while (TRUE)
    {
        if (EventsProcessed >= 256)
        {
            DPRINT1("XHCI_ProcessEvent: event ring did not terminate after %lu TRBs\n",
                    EventsProcessed);
            break;
        }
        
        eventTRB = (*dequeue_pointer).EventTRB;
        if (eventTRB.EventGenericTRB.CycleBit != HcResourcesVA->EventRing.ConsumerCycleState)
        {
            break;
        }
        TRBType = eventTRB.EventGenericTRB.TRBType;
        
        DPRINT("XHCI_ProcessEvent: Processing TRB Type %d (0x%x)\n",
               TRBType, TRBType);
        EventsProcessed++;
        
        switch (TRBType)
        {
            case TRANSFER_EVENT:
                DPRINT("XHCI_ProcessEvent: TRANSFER_EVENT\n");
                XHCI_ProcessTransferEvent(XhciExtension, &eventTRB);
                break;
            case COMMAND_COMPLETION_EVENT: 
                DPRINT("XHCI_ProcessEvent: COMMAND_COMPLETION_EVENT\n");
                // Always process completion events regardless of success/failure
                // The ProcessCommandCompletion function will handle the status appropriately
                DPRINT("XHCI_ProcessEvent: COMMAND_COMPLETION_EVENT, completion code %i\n",
                       eventTRB.CommandCompletionTRB.CompletionCode);
                XHCI_ProcessCommandCompletion(XhciExtension, &eventTRB);
                break;
            case PORT_STATUS_CHANGE_EVENT: 
                DPRINT("XHCI_ProcessEvent: Port Status change event\n");
                /* Call a private function to handle port status events */
                PXHCI_PortStatusChange(XhciExtension, eventTRB.PortStatusChangeTRB.PortID);
                break;
            case BANDWIDTH_RESET_REQUEST_EVENT:
                DPRINT1("XHCI_ProcessEvent: BANDWIDTH_RESET_REQUEST_EVENT\n");
                break;
            case DOORBELL_EVENT:
                DPRINT1("XHCI_ProcessEvent: DOORBELL_EVENT\n");
                break;
            case HOST_CONTROLLER_EVENT:
                DPRINT1("XHCI_ProcessEvent: HOST_CONTROLLER_EVENT\n");
                break;
            case DEVICE_NOTIFICATION_EVENT:
                DPRINT1("XHCI_ProcessEvent: DEVICE_NOTIFICATION_EVENT\n");
                break;
            case MF_INDEX_WARP_EVENT:
                DPRINT1("XHCI_ProcessEvent: MF_INDEX_WARP_EVENT\n");
                break;
            default:
                DPRINT1("XHCI_ProcessEvent: Unknown TRBType - %x\n",
                        TRBType);
                DbgBreakPoint(); 
                break;
        }
        
        // Advance dequeue pointer
        DPRINT("XHCI_ProcessEvent: Advancing dequeue pointer from %p\n", dequeue_pointer);
        dequeue_pointer = dequeue_pointer + 1;
        DPRINT("XHCI_ProcessEvent: Dequeue pointer advanced to %p\n", dequeue_pointer);
        
        // Check if we need to wrap around the event ring
        if (dequeue_pointer == &(HcResourcesVA->EventRing.firstSeg.XhciTrb[256]))
        {
            DPRINT("XHCI_ProcessEvent: Wrapping event ring, flipping cycle state from %d to %d\n",
                    HcResourcesVA->EventRing.ConsumerCycleState,
                    HcResourcesVA->EventRing.ConsumerCycleState ? 0 : 1);
                    
            HcResourcesVA->EventRing.ConsumerCycleState = HcResourcesVA->EventRing.ConsumerCycleState ? 0 : 1;
            HcResourcesVA->EventRing.ProducerCycleState = HcResourcesVA->EventRing.ProducerCycleState ? 0 : 1;
            dequeue_pointer = &(HcResourcesVA->EventRing.firstSeg.XhciTrb[0]);
        }
        
        // Update our stored dequeue pointer after each advancement to prevent stale loops
        HcResourcesVA->EventRing.dequeue_pointer = dequeue_pointer;
    }
    
    HcResourcesVA->EventRing.dequeue_pointer = dequeue_pointer;
    
    /*
     * Always update ERDP, even when no TRB was consumed.  The EHB bit is
     * write-1-to-clear; if an interrupt arrives while the next event TRB is
     * not yet visible, skipping this write leaves the interrupter busy and
     * QEMU can spin with IMAN.IP asserted without delivering the pending
     * transfer completion.
     */
    erstdp.AsULONGLONG = HcResourcesPA.QuadPart + ((ULONG_PTR)dequeue_pointer - (ULONG_PTR)HcResourcesVA);
    ASSERT(erstdp.AsULONGLONG >= HcResourcesPA.QuadPart && erstdp.AsULONGLONG < HcResourcesPA.QuadPart + sizeof(XHCI_HC_RESOURCES)) ;
    erstdp.DequeueERSTIndex = 0;
    erstdp.EventHandlerBusy = 1;

    XHCI_Write64bitReg(RunTimeRegisterBase + XHCI_ERSTDP, erstdp.AsULONGLONG);
    
    return MP_STATUS_SUCCESS;
}

MPSTATUS
NTAPI
XHCI_SendCommand (IN XHCI_TRB CommandTRB,
                  IN PXHCI_EXTENSION XhciExtension)
{
    PXHCI_HC_RESOURCES HcResourcesVA;
    PHYSICAL_ADDRESS HcResourcesPA;
    PULONG DoorBellRegisterBase;
    XHCI_DOORBELL Doorbell_0;
    PXHCI_TRB enqueue_pointer;
    PXHCI_TRB enqueue_pointer_prev;
    PXHCI_TRB dequeue_pointer;
    XHCI_TRB CheckLink;
    PHYSICAL_ADDRESS LinkPointer;
    BOOLEAN RingWrapped = FALSE;
    
    HcResourcesVA = XhciExtension->HcResourcesVA;
    HcResourcesPA = XhciExtension->HcResourcesPA;
    enqueue_pointer = HcResourcesVA->CommandRing.enqueue_pointer;
    dequeue_pointer = HcResourcesVA->CommandRing.dequeue_pointer;
    
    // Enhanced ring full check - consider wrap-around scenarios  
    PXHCI_TRB NextEnqueuePtr = enqueue_pointer + 1;
    
    // Check if next position would be a link TRB or past end of ring
    if (NextEnqueuePtr >= &(HcResourcesVA->CommandRing.firstSeg.XhciTrb[255]))
    {
        // We're approaching the link TRB or end of ring
        NextEnqueuePtr = &(HcResourcesVA->CommandRing.firstSeg.XhciTrb[0]); // Wrap to start
        RingWrapped = TRUE;
    }
    
    // check if ring is full (considering wrap-around)
    if ((NextEnqueuePtr == dequeue_pointer) || (enqueue_pointer + 1 == dequeue_pointer)) 
    {
        DPRINT1("XHCI_SendCommand: Command ring is full (enqueue=%p, dequeue=%p, wrapped=%d)\n",
                enqueue_pointer, dequeue_pointer, RingWrapped);
        return MP_STATUS_FAILURE;
    }
    
    // check if the current TRB is a link TRB
    CheckLink = *enqueue_pointer;
    if (CheckLink.LinkTRB.TRBType == LINK)
    {
        DPRINT1("XHCI_SendCommand: Hit link TRB at %p, processing wrap-around\n", enqueue_pointer);
        
        LinkPointer.QuadPart = CheckLink.GenericTRB.Word1;
        LinkPointer.QuadPart = LinkPointer.QuadPart << 32;
        LinkPointer.QuadPart = LinkPointer.QuadPart + CheckLink.GenericTRB.Word0;
        ASSERT(LinkPointer.QuadPart >= HcResourcesPA.QuadPart && LinkPointer.QuadPart < HcResourcesPA.QuadPart + sizeof(XHCI_HC_RESOURCES));
        enqueue_pointer_prev = enqueue_pointer;
        enqueue_pointer = (PXHCI_TRB)(HcResourcesVA + LinkPointer.QuadPart - HcResourcesPA.QuadPart);
        
        if ((enqueue_pointer == dequeue_pointer) || (enqueue_pointer == dequeue_pointer + 1))
        { // it can't move ahead break out of function
            DPRINT1("XHCI_SendCommand: Command ring is full after link processing\n");
            return MP_STATUS_FAILURE;
        }
        
        // now the link trb is valid. set its cycle state to Producer cycle state for the command ring to read
        CheckLink.LinkTRB.CycleBit = HcResourcesVA->CommandRing.ProducerCycleState;
        // write the link trb back. 
        *enqueue_pointer_prev = CheckLink;
        
        // now we can go ahead to the next pointer where we want to write the new trb. before that check and toggle if necessary.
        if (CheckLink.LinkTRB.ToggleCycle == 1)
        {
            HcResourcesVA->CommandRing.ProducerCycleState = ~(HcResourcesVA->CommandRing.ProducerCycleState); //update this when the xHC reaches link trb 
            DPRINT1("XHCI_SendCommand: Toggled producer cycle state to %d\n", HcResourcesVA->CommandRing.ProducerCycleState);
        }
        RingWrapped = TRUE;
    }
    
    // place trb on the command ring
    // CRITICAL: Set the cycle bit to match the producer cycle state
    CommandTRB.GenericTRB.Word3 = (CommandTRB.GenericTRB.Word3 & ~1) | HcResourcesVA->CommandRing.ProducerCycleState;
    
    // Calculate physical address for debug output and TRB tracking
    ULONG TrbIndex = (ULONG)((ULONG_PTR)enqueue_pointer - 
                            (ULONG_PTR)&HcResourcesVA->CommandRing.firstSeg.XhciTrb[0]) / sizeof(XHCI_TRB);
    PHYSICAL_ADDRESS TrbPA;
    TrbPA.QuadPart = HcResourcesPA.QuadPart + 
                     FIELD_OFFSET(XHCI_HC_RESOURCES, CommandRing.firstSeg.XhciTrb[0]) +
                     (TrbIndex * sizeof(XHCI_TRB));
    
    DPRINT1("XHCI_SendCommand: Placing TRB at VA %p, PA 0x%I64x, cycle bit %d, TRB type %d, index %d (wrapped=%d)\n", 
            enqueue_pointer, TrbPA.QuadPart, HcResourcesVA->CommandRing.ProducerCycleState, 
            (CommandTRB.GenericTRB.Word3 >> 10) & 0x3F, TrbIndex, RingWrapped);
    
    // Additional debug for TRB content before placing on ring
    DPRINT1("XHCI_SendCommand: Final TRB content - Word0=0x%08x, Word1=0x%08x, Word2=0x%08x, Word3=0x%08x\n",
            CommandTRB.GenericTRB.Word0, CommandTRB.GenericTRB.Word1, 
            CommandTRB.GenericTRB.Word2, CommandTRB.GenericTRB.Word3);
    
    // Verify TRB alignment and placement
    DPRINT1("XHCI_SendCommand: Command ring state - Producer cycle=%d, Consumer cycle=%d\n",
            HcResourcesVA->CommandRing.ProducerCycleState, HcResourcesVA->CommandRing.ConsumerCycleState);
    DPRINT1("XHCI_SendCommand: Enqueue ptr=%p, Dequeue ptr=%p\n",
            enqueue_pointer, HcResourcesVA->CommandRing.dequeue_pointer);
    
    // Store the actual TRB physical address for accurate command tracking
    // This ensures the caller gets the EXACT address where the TRB was placed
    XhciExtension->LastCommandTrbPA = TrbPA;
    
    // TODO: Add check for potential address reuse by scanning pending commands
    // This would help detect command ring wrap-around or tracking issues
    
    *enqueue_pointer = CommandTRB;
    enqueue_pointer = enqueue_pointer + 1;
    HcResourcesVA->CommandRing.enqueue_pointer = enqueue_pointer;
    
    // ring doorbell 
    DoorBellRegisterBase = XhciExtension->DoorBellRegisterBase;
    Doorbell_0.DoorBellTarget = 0;
    Doorbell_0.RsvdZ = 0;
    Doorbell_0.AsULONG = 0;
    WRITE_REGISTER_ULONG(DoorBellRegisterBase, Doorbell_0.AsULONG);

    DPRINT1("XHCI_SendCommand: Command TRB sent successfully, doorbell rung\n");
    return MP_STATUS_SUCCESS;
}

MPSTATUS
NTAPI
XHCI_InitializeResources(IN PXHCI_EXTENSION XhciExtension,
                         IN ULONG_PTR resourcesStartVA,
                         IN ULONG  resourcesStartPA)
{
    /* This function is fucked, rework later*/
    
    PXHCI_HC_RESOURCES HcResourcesVA;
    PHYSICAL_ADDRESS HcResourcesPA;
    PULONG OperationalRegs;
    USHORT PageSize;
    USHORT MaxScratchPadBuffers;
    
    PULONG  RunTimeRegisterBase;
    XHCI_EVENT_RING_TABLE_SIZE erstz;
    XHCI_EVENT_RING_TABLE_BASE_ADDR erstba;
    XHCI_EVENT_RING_DEQUEUE_POINTER erstdp;
    XHCI_EVENT_RING_SEGMENT_TABLE EventRingSegTable;
    
    XHCI_COMMAND_RING_CONTROL CommandRingControlRegister, CommandRingControlRegister_temp;
    XHCI_DEVICE_CONTEXT_BASE_ADD_ARRAY_POINTER DCBAAPointer;
    XHCI_TRB CommandLinkTRB;
    XHCI_LINK_ADDR RingStartAddr;
    
    PHYSICAL_ADDRESS Zero, Max;
    PMDL ScratchPadArrayMDL;
    PXHCI_SCRATCHPAD_BUFFER_ARRAY BufferArrayPointer;
    PMDL ScratchPadBufferMDL;
    int i = 0;
    
    DPRINT1("XHCI_InitializeResources: function initiated\n");
    DPRINT1_XHCI("XHCI_InitializeResources: BaseVA - %p, BasePA - %p\n",
                resourcesStartVA,
                resourcesStartPA);
                
    // Initialize command and transfer tracking systems
    InitializeCommandTracking();
    InitializeTransferTracking();
                
    HcResourcesVA = (PXHCI_HC_RESOURCES)resourcesStartVA;
    ASSERT((ULONG_PTR)HcResourcesVA % PAGE_SIZE == 0);
    XhciExtension->HcResourcesVA = HcResourcesVA;
    
    HcResourcesPA.QuadPart = (ULONG_PTR)resourcesStartPA;
    XhciExtension->HcResourcesPA = HcResourcesPA;
    OperationalRegs = XhciExtension->OperationalRegs;
    
    //DCBAA init
    // First, zero the entire DCBAA to ensure all device context pointers are NULL
    RtlZeroMemory(&HcResourcesVA->DCBAA, sizeof(XHCI_DEVICE_CONTEXT_BASE_ADD_ARRAY));
    
    DCBAAPointer.AsULONGLONG =  HcResourcesPA.QuadPart + FIELD_OFFSET(XHCI_HC_RESOURCES, DCBAA);
    DPRINT1("XHCI_InitializeResources  : DCBAAPointer   0x%I64x\n", DCBAAPointer.AsULONGLONG );
    DPRINT1("XHCI_InitializeResources  : DCBAA initialized with all NULL pointers\n");

    XHCI_Write64bitReg(OperationalRegs + XHCI_DCBAAP, DCBAAPointer.AsULONGLONG);

    // command ring intialisation.
    HcResourcesVA->CommandRing.enqueue_pointer = &(HcResourcesVA->CommandRing.firstSeg.XhciTrb[0]);
    HcResourcesVA->CommandRing.dequeue_pointer = &(HcResourcesVA->CommandRing.firstSeg.XhciTrb[0]);
    for (i=0; i<256; i++)
    {
        HcResourcesVA->CommandRing.firstSeg.XhciTrb[i].GenericTRB.Word0 = 0;
        HcResourcesVA->CommandRing.firstSeg.XhciTrb[i].GenericTRB.Word1 = 0;
        HcResourcesVA->CommandRing.firstSeg.XhciTrb[i].GenericTRB.Word2 = 0;
        HcResourcesVA->CommandRing.firstSeg.XhciTrb[i].GenericTRB.Word3 = 0;
    }
    CommandRingControlRegister.AsULONGLONG = HcResourcesPA.QuadPart + FIELD_OFFSET(XHCI_HC_RESOURCES, CommandRing.firstSeg);
    CommandRingControlRegister_temp.AsULONGLONG = READ_REGISTER_ULONG(OperationalRegs + XHCI_CRCR + 1) |  READ_REGISTER_ULONG(OperationalRegs + XHCI_CRCR);
    CommandRingControlRegister.RingCycleState = 1;
    HcResourcesVA->CommandRing.ProducerCycleState = 1;
    HcResourcesVA->CommandRing.ConsumerCycleState = 1;
    CommandRingControlRegister.RsvdP = CommandRingControlRegister_temp.RsvdP;  
    DPRINT1("XHCI_InitializeResources  : CommandRingControlRegister   %p\n", CommandRingControlRegister.AsULONGLONG );
    XHCI_Write64bitReg(OperationalRegs + XHCI_CRCR, CommandRingControlRegister.AsULONGLONG);
    
    // Place link trb with toggle cycle state in the last link trb.

    CommandLinkTRB.GenericTRB.Word0 = 0;
    CommandLinkTRB.GenericTRB.Word1 = 0;
    CommandLinkTRB.GenericTRB.Word2 = 0;
    CommandLinkTRB.GenericTRB.Word3 = 0;
    
    RingStartAddr.AsULONGLONG = HcResourcesPA.QuadPart + FIELD_OFFSET(XHCI_HC_RESOURCES, CommandRing.firstSeg);
    CommandLinkTRB.LinkTRB.RsvdZ1 = 0; 
    CommandLinkTRB.LinkTRB.RingSegmentPointerLo = RingStartAddr.RingSegmentPointerLo;
    CommandLinkTRB.LinkTRB.RingSegmentPointerHi = RingStartAddr.RingSegmentPointerHi;
    CommandLinkTRB.LinkTRB.InterrupterTarget = 0;
    CommandLinkTRB.LinkTRB.CycleBit = ~(HcResourcesVA->CommandRing.ProducerCycleState);
    CommandLinkTRB.LinkTRB.ToggleCycle = 1; //impt
    CommandLinkTRB.LinkTRB.ChainBit = 0;
    CommandLinkTRB.LinkTRB.InterruptOnCompletion = 1; //  NOT NECESSARY
    CommandLinkTRB.LinkTRB.TRBType = LINK;
    
    HcResourcesVA->CommandRing.firstSeg.XhciTrb[255] = CommandLinkTRB;
    // end of command ring init
    
    //Primary Interrupter init
    RunTimeRegisterBase =  XhciExtension -> RunTimeRegisterBase;

    erstz.AsULONG = READ_REGISTER_ULONG(RunTimeRegisterBase + XHCI_ERSTSZ) ;
    erstz.EventRingSegTableSize = 1;
    DPRINT1("XHCI_InitializeResources  : erstz.AsULONG   %p\n", erstz.AsULONG );
    WRITE_REGISTER_ULONG(RunTimeRegisterBase + XHCI_ERSTSZ, erstz.AsULONG);
    // event ring dequeue pointer.
    erstdp.AsULONGLONG = HcResourcesPA.QuadPart + FIELD_OFFSET(XHCI_HC_RESOURCES, EventRing.firstSeg.XhciTrb[0]);
    
    HcResourcesVA->EventRing.enqueue_pointer = &(HcResourcesVA->EventRing.firstSeg.XhciTrb[0]);
    HcResourcesVA->EventRing.dequeue_pointer = &(HcResourcesVA->EventRing.firstSeg.XhciTrb[0]);
    
    // Initialize Event Ring TRBs to zero (critical for cycle bit handling)
    DPRINT1("XHCI_InitializeResources: Clearing event ring TRBs\n");
    RtlZeroMemory(&HcResourcesVA->EventRing.firstSeg.XhciTrb[0], 
                  sizeof(XHCI_TRB) * 256);
    
    HcResourcesVA->EventRing.ProducerCycleState = 1;
    HcResourcesVA->EventRing.ConsumerCycleState = 1;
    
    erstdp.DequeueERSTIndex =0;
    DPRINT1("XHCI_InitializeResources  : erstdp.AsULONGLONG %p\n", erstdp.AsULONGLONG );
    XHCI_Write64bitReg(RunTimeRegisterBase + XHCI_ERSTDP, erstdp.AsULONGLONG);
    // event ring segment table base address array
    
    erstba.AsULONGLONG = HcResourcesPA.QuadPart + FIELD_OFFSET(XHCI_HC_RESOURCES, EventRingSegTable);
    EventRingSegTable.RingSegmentBaseAddr = (ULONGLONG)HcResourcesPA.QuadPart + FIELD_OFFSET(XHCI_HC_RESOURCES, EventRing.firstSeg.XhciTrb[0]);
    EventRingSegTable.RingSegmentSize = 256;
    EventRingSegTable.RsvdZ = 0;
    HcResourcesVA->EventRingSegTable = EventRingSegTable;
    DPRINT1("XHCI_InitializeResources  : erstba.AsULONGLONG   %p\n", erstba.AsULONGLONG );
    XHCI_Write64bitReg(RunTimeRegisterBase + XHCI_ERSTBA, erstba.AsULONGLONG);
    // intially enque and deque are equal. 

    
    for (i=0; i<256; i++)
    {
        HcResourcesVA->EventRing.firstSeg.XhciTrb[i].GenericTRB.Word0 = 0;
        HcResourcesVA->EventRing.firstSeg.XhciTrb[i].GenericTRB.Word1 = 0;
        HcResourcesVA->EventRing.firstSeg.XhciTrb[i].GenericTRB.Word2 = 0;
        HcResourcesVA->EventRing.firstSeg.XhciTrb[i].GenericTRB.Word3 = 0;
    }

    /* Initalize Transfer Ring */

    HcResourcesVA->TransferRing.enqueue_pointer = &(HcResourcesVA->TransferRing.firstSeg.XhciTrb[0]);
    HcResourcesVA->TransferRing.dequeue_pointer = &(HcResourcesVA->TransferRing.firstSeg.XhciTrb[0]);
    HcResourcesVA->TransferRing.UsedTrbs = 0;
    for (i=0; i<256; i++)
    {
        HcResourcesVA->TransferRing.firstSeg.XhciTrb[i].GenericTRB.Word0 = 0;
        HcResourcesVA->TransferRing.firstSeg.XhciTrb[i].GenericTRB.Word1 = 0;
        HcResourcesVA->TransferRing.firstSeg.XhciTrb[i].GenericTRB.Word2 = 0;
        HcResourcesVA->TransferRing.firstSeg.XhciTrb[i].GenericTRB.Word3 = 0;
    }
    HcResourcesVA->TransferRing.ProducerCycleState = 1;
    HcResourcesVA->TransferRing.ConsumerCycleState = 1;

    /* Initialize Per-Slot Transfer Rings for EP0 */
    DPRINT1("XHCI_InitializeResources: Initializing per-slot transfer rings\n");
    for (int SlotIndex = 0; SlotIndex < XHCI_MAX_SLOTS; SlotIndex++)
    {
        // Initialize each slot's transfer ring with proper Link TRB
        XHCI_InitializeTransferRing(&HcResourcesVA->SlotTransferRings[SlotIndex]);
        
        DPRINT1("XHCI_InitializeResources: Initialized transfer ring for slot %d at VA %p\n", 
                SlotIndex + 1, &HcResourcesVA->SlotTransferRings[SlotIndex]);
    }

    // check if the controller supports 4k page size or quit.
    PageSize = XhciExtension-> PageSize;
    MaxScratchPadBuffers = XhciExtension->MaxScratchPadBuffers;
    
    if (MaxScratchPadBuffers == 0)
    { // xHCI may declare 0 scratchpad arrays. if so there is no need for memory allocation.
        return MP_STATUS_SUCCESS;
    }
    if ((PageSize & (1 << 0)) == 0) 
    {
        DPRINT1("XHCI_InitializeResources  : fail. does not support 4k page size   %p\n",PageSize);
        return MP_STATUS_FAILURE;
    }
    // allocate scratchpad buffer array
     // Start of sus
    Zero.QuadPart = 0; 
    Max.QuadPart = -1;   
    
    BufferArrayPointer = MmAllocateContiguousMemory(MaxScratchPadBuffers * sizeof(XHCI_SCRATCHPAD_BUFFER_ARRAY), Max);
    if (BufferArrayPointer == NULL)
    {
        DPRINT1("XHCI_InitializeResources  : Scratch pad array ContiguousMemory allcoation fail NULL\n");
        return MP_STATUS_FAILURE;
    }
    ScratchPadArrayMDL = IoAllocateMdl(BufferArrayPointer, MaxScratchPadBuffers * sizeof(XHCI_SCRATCHPAD_BUFFER_ARRAY), FALSE, FALSE, NULL);
    if (ScratchPadArrayMDL == NULL) 
    {
        DPRINT1("XHCI_InitializeResources  : Scratch pad array could not be allocated. it is NULL\n");
        MmFreeContiguousMemory(BufferArrayPointer);
        return MP_STATUS_FAILURE;
    }
    MmBuildMdlForNonPagedPool(ScratchPadArrayMDL);

    HcResourcesVA->DCBAA.ContextBaseAddr[0].QuadPart = MmGetMdlPfnArray(ScratchPadArrayMDL)[0] << PAGE_SHIFT; 
    //allocate scratchpad buffers
    /////////////////////////////////////////////////////////// First File
    ScratchPadBufferMDL = MmAllocatePagesForMdlEx(Zero, Max, Zero, MaxScratchPadBuffers*PAGE_SIZE, MmNonCached, 0);
    if (ScratchPadBufferMDL == NULL) 
    { 
        IoFreeMdl(ScratchPadArrayMDL);
        MmFreeContiguousMemory(BufferArrayPointer);
        return MP_STATUS_FAILURE;
    }
    if (MmGetMdlByteCount(ScratchPadBufferMDL) < MaxScratchPadBuffers*PAGE_SIZE) 
    { 
        MmFreePagesFromMdl(ScratchPadBufferMDL); 
        ExFreePool(ScratchPadBufferMDL);
        IoFreeMdl(ScratchPadArrayMDL);
        MmFreeContiguousMemory(BufferArrayPointer);
        return MP_STATUS_FAILURE;
    }
    for (i = 0; i < MaxScratchPadBuffers ; i++)
    {
        BufferArrayPointer[i].AsULONGLONG = MmGetMdlPfnArray(ScratchPadBufferMDL)[i] << PAGE_SHIFT;
    }
    XhciExtension-> ScratchPadArrayMDL = ScratchPadArrayMDL;
    XhciExtension-> ScratchPadBufferMDL = ScratchPadBufferMDL;
    //end of sus
    DPRINT1("XHCI has been sucessfully setup... odd..");
    return MP_STATUS_SUCCESS;
}

MPSTATUS
NTAPI
XHCI_InitializeHardware(IN PXHCI_EXTENSION XhciExtension)
{
    PULONG BaseIoAdress;
    PULONG OperationalRegs;
    XHCI_USB_COMMAND Command;
    XHCI_USB_STATUS Status;
    LARGE_INTEGER CurrentTime = {{0, 0}};
    LARGE_INTEGER LastTime = {{0, 0}};
    XHCI_HC_STRUCTURAL_PARAMS_1 StructuralParams_1;
    XHCI_CONFIGURE Config;

    DPRINT1("XHCI_InitializeHardware: function initiated\n");
    DPRINT1("XHCI_InitializeHardware: ... \n");

    OperationalRegs = XhciExtension->OperationalRegs;
    BaseIoAdress = XhciExtension->BaseIoAdress;

    KeQuerySystemTime(&CurrentTime);
    CurrentTime.QuadPart += 100 * 10000;

    Status.AsULONG = READ_REGISTER_ULONG(OperationalRegs + XHCI_USBSTS);
    ASSERT(Status.ControllerNotReady != 1); // this is needed before writing anything to the operaational or doorbell registers

    Command.HCReset = 1;
    WRITE_REGISTER_ULONG(OperationalRegs + XHCI_USBCMD, Command.AsULONG);
    while (TRUE)
    {
        KeQuerySystemTime(&LastTime);

        Command.AsULONG = READ_REGISTER_ULONG(OperationalRegs + XHCI_USBCMD);

        if (Command.HCReset != 1)
        {
            break;
        }

        if (LastTime.QuadPart >= CurrentTime.QuadPart)
        {
            DPRINT1("XHCI_InitializeHardware: Software Reset failed!\n");
            return MP_STATUS_FAILURE;
        }
    }
    DPRINT1("XHCI_InitializeHardware: Reset - OK\n");

    StructuralParams_1.AsULONG = READ_REGISTER_ULONG(BaseIoAdress + XHCI_HCSP1); // HCSPARAMS1 register

    XhciExtension->NumberOfPorts = StructuralParams_1.NumberOfPorts;
    RtlZeroMemory(XhciExtension->PortConnectStatus, sizeof(XhciExtension->PortConnectStatus));
    RtlZeroMemory(XhciExtension->PortConnectChange, sizeof(XhciExtension->PortConnectChange));
    {
        ULONG Port;

        for (Port = 1; Port <= XhciExtension->NumberOfPorts && Port <= XHCI_MAX_PORTS; Port++)
        {
            PULONG PortReg = OperationalRegs + XHCI_PORTSC + ((Port - 1) * 4);
            XHCI_PORT_STATUS_CONTROL PortStatus;

            PortStatus.AsULONG = READ_REGISTER_ULONG(PortReg);
            XhciExtension->PortConnectStatus[Port] = PortStatus.CurrentConnectStatus ? 1 : 0;
        }
    }
    
    DPRINT1("XHCI_InitializeHardware: xHCI controller supports %d device slots\n", 
            StructuralParams_1.NumberOfDeviceSlots);

    Command.AsULONG = READ_REGISTER_ULONG(OperationalRegs + XHCI_USBCMD);
    Config.AsULONG = READ_REGISTER_ULONG(OperationalRegs + XHCI_CONFIG);
    ASSERT(Command.RunStop == 0); //required before setting max device slots enabled.
    
    // Enable more device slots - use up to XHCI_MAX_SLOTS or the maximum supported by the controller
    ULONG MaxSlots = min(StructuralParams_1.NumberOfDeviceSlots, XHCI_MAX_SLOTS);
    Config.MaxDeviceSlotsEnabled = MaxSlots;
    Config.U3EntryEnable = 0;
    Config.ConfigurationInfoEnable = 0;
    
    DPRINT1("XHCI_InitializeHardware: Enabling %d device slots (controller supports %d)\n",
            MaxSlots, StructuralParams_1.NumberOfDeviceSlots);
    
    WRITE_REGISTER_ULONG(OperationalRegs + XHCI_CONFIG, Config.AsULONG);

    return MP_STATUS_SUCCESS;
}

MPSTATUS
NTAPI
XHCI_StartController(IN PVOID xhciExtension,
                     IN PUSBPORT_RESOURCES Resources)
{
    PXHCI_EXTENSION XhciExtension;
    PULONG BaseIoAdress;
    PULONG OperationalRegs;
    PULONG RunTimeRegisterBase;
    PULONG DoorBellRegisterBase;
    XHCI_CAPLENGHT_INTERFACE_VERSION CapLenReg;
    XHCI_DOORBELL_OFFSET DoorBellOffsetRegister;
    MPSTATUS MPStatus;
    XHCI_USB_COMMAND Command;
    XHCI_RT_REGISTER_SPACE_OFFSET RTSOffsetRegister;
    UCHAR CapabilityRegLength;
    UCHAR Fladj;
    XHCI_PAGE_SIZE PageSizeReg;
    USHORT MaxScratchPadBuffers;
    XHCI_HC_STRUCTURAL_PARAMS_2 HCSPARAMS2;

    DPRINT1("XHCI_StartController: function initiated\n");
    if ((Resources->ResourcesTypes & (USBPORT_RESOURCES_MEMORY | USBPORT_RESOURCES_INTERRUPT)) !=
                                     (USBPORT_RESOURCES_MEMORY | USBPORT_RESOURCES_INTERRUPT))
    {
        DPRINT1("XHCI_StartController: Resources->ResourcesTypes - %x\n",
                Resources->ResourcesTypes);

        return MP_STATUS_ERROR;
    }

    XhciExtension = (PXHCI_EXTENSION)xhciExtension;
    BaseIoAdress = (PULONG)Resources->ResourceBase;
    XhciExtension->BaseIoAdress = BaseIoAdress;

    CapLenReg.AsULONG = READ_REGISTER_ULONG(BaseIoAdress);
    CapLenReg.Rsvd = 0;
    CapLenReg.HostControllerInterfaceVersion = 0;
    CapabilityRegLength = (UCHAR)CapLenReg.CapabilityRegistersLength;
    OperationalRegs = (PULONG)((ULONG_PTR)BaseIoAdress + CapabilityRegLength);
    XhciExtension->OperationalRegs = OperationalRegs;

    DoorBellOffsetRegister.AsULONG = READ_REGISTER_ULONG(BaseIoAdress + XHCI_DBOFF);
    DoorBellRegisterBase = (PULONG)((PBYTE)BaseIoAdress + DoorBellOffsetRegister.AsULONG);
    XhciExtension->DoorBellRegisterBase = DoorBellRegisterBase;

    RTSOffsetRegister.AsULONG = READ_REGISTER_ULONG(BaseIoAdress + XHCI_RTSOFF);
    RunTimeRegisterBase = (PULONG)((PBYTE)BaseIoAdress + RTSOffsetRegister.AsULONG);
    XhciExtension->RunTimeRegisterBase = RunTimeRegisterBase;

    PageSizeReg.AsULONG =  READ_REGISTER_ULONG(OperationalRegs + XHCI_PGSZ);
    XhciExtension->PageSize = PageSizeReg.PageSize;
    HCSPARAMS2.AsULONG = READ_REGISTER_ULONG(BaseIoAdress + XHCI_HCSP2);
    MaxScratchPadBuffers = 0;
    MaxScratchPadBuffers = HCSPARAMS2.MaxSPBuffersHi;
    MaxScratchPadBuffers = MaxScratchPadBuffers << 5;
    MaxScratchPadBuffers = MaxScratchPadBuffers + HCSPARAMS2.MaxSPBuffersLo;
    XhciExtension->MaxScratchPadBuffers = MaxScratchPadBuffers;

    DPRINT1("XHCI_StartController: BaseIoAdress    - %p\n", BaseIoAdress);
    DPRINT1("XHCI_StartController: OperationalRegs - %p\n", OperationalRegs);
    DPRINT1("XHCI_StartController: DoorBellRegisterBase - %p\n", DoorBellRegisterBase);
    DPRINT1("XHCI_StartController: RunTimeRegisterBase - %p\n", RunTimeRegisterBase);
    DPRINT1("XHCI_StartController: PageSize - %p\n", XhciExtension->PageSize);
    DPRINT1("XHCI_StartController: MaxScratchPadBuffers - %p\n", MaxScratchPadBuffers);

    RegPacket.UsbPortReadWriteConfigSpace(XhciExtension,
                                          1,
                                          &Fladj,
                                          0x61,
                                          1);

    XhciExtension->FrameLengthAdjustment = Fladj;
    
    // Initialize command tracking fields
    XhciExtension->LastCommandTrbPA.QuadPart = 0;

    MPStatus = XHCI_InitializeHardware(XhciExtension);

    if (MPStatus)
    {
        DPRINT1("XHCI_StartController: Unsuccessful InitializeHardware()\n");
        return MPStatus;
    }

    MPStatus = XHCI_InitializeResources(XhciExtension,
                                       Resources->StartVA,
                                       Resources->StartPA);

    if (MPStatus)
    {
        DPRINT1("XHCI_StartController: Unsuccessful InitializeSchedule()\n");
        return MPStatus;
    }

    // starting the controller
    Command.AsULONG = READ_REGISTER_ULONG(OperationalRegs + XHCI_USBCMD);
    Command.RunStop = 1;
    Command.InterrupterEnable = 1;  // Enable global interrupts
    DPRINT1("XHCI_StartController: Setting Command register: RunStop=1, InterrupterEnable=1\n");
    WRITE_REGISTER_ULONG(OperationalRegs + XHCI_USBCMD, Command.AsULONG);


    //below line should be uncommented if you want to use the controller work test function
    //MPStatus = PXHCI_ControllerWorkTest(XhciExtension, (PXHCI_HC_RESOURCES)Resources->StartVA, (PVOID)Resources->StartPA);
    return MP_STATUS_SUCCESS;
}


VOID
NTAPI
XHCI_StopController(IN PVOID xhciExtension,
                    IN BOOLEAN IsDoDisableInterrupts)
{

    PXHCI_EXTENSION XhciExtension;
    USHORT MaxScratchPadBuffers;
    PMDL ScratchPadArrayMDL;
    PMDL ScratchPadBufferMDL;
    PXHCI_SCRATCHPAD_BUFFER_ARRAY BufferArrayPointer;
    XHCI_USB_COMMAND Command, Command_temp;
    PULONG OperationalRegs;
    XHCI_USB_STATUS Status;
    LARGE_INTEGER CurrentTime = {{0, 0}};
    LARGE_INTEGER LastTime = {{0, 0}};

    DPRINT1("XHCI_StopController: Function initiated. \n");
    XhciExtension = (PXHCI_EXTENSION) xhciExtension;
    MaxScratchPadBuffers = XhciExtension->MaxScratchPadBuffers;
    // free memory allocated to scratchpad buffers.
    ScratchPadArrayMDL = XhciExtension-> ScratchPadArrayMDL;
    ScratchPadBufferMDL = XhciExtension-> ScratchPadBufferMDL;
    if (MaxScratchPadBuffers != 0)
    {
        // free the scratchpad buffers
        MmFreePagesFromMdl(ScratchPadBufferMDL);
        ExFreePool(ScratchPadBufferMDL);
        // free scratchpad array
        BufferArrayPointer = MmGetMdlVirtualAddress(ScratchPadArrayMDL);
        IoFreeMdl(ScratchPadArrayMDL);
        MmFreeContiguousMemory(BufferArrayPointer);
    }
    
    OperationalRegs = XhciExtension->OperationalRegs;
    Command_temp.AsULONG = READ_REGISTER_ULONG(OperationalRegs + XHCI_USBCMD);
    Command.AsULONG = 0;
    Command.RsvdP1 = Command_temp.RsvdP1;
    Command.RsvdP2 = Command_temp.RsvdP2;
    Command.RsvdP3 = Command_temp.RsvdP3;
    WRITE_REGISTER_ULONG(OperationalRegs + XHCI_USBCMD, Command.AsULONG);
    
    KeQuerySystemTime(&CurrentTime);
    CurrentTime.QuadPart += 100 * 10000;
    while (TRUE)
    {
        KeQuerySystemTime(&LastTime);
        
        Status.AsULONG = READ_REGISTER_ULONG(OperationalRegs + XHCI_USBSTS);
       
        if (Status.HCHalted == 1)
        {
            break;
        }

        if (LastTime.QuadPart >= CurrentTime.QuadPart)
        {
            DPRINT1("XHCI_StopController: controller stop  failed!\n");
        }
    }

}

VOID
NTAPI
XHCI_SuspendController(IN PVOID xhciExtension)
{
    
    PXHCI_EXTENSION XhciExtension;
    DPRINT1("XHCI_SuspendController: function initiated\n");
    XhciExtension = (PXHCI_EXTENSION)xhciExtension;
    
    XhciExtension->Flags |= XHCI_FLAGS_CONTROLLER_SUSPEND;
    
}

MPSTATUS
NTAPI
XHCI_ResumeController(IN PVOID xhciExtension)
{
    
    PXHCI_EXTENSION XhciExtension;
    DPRINT1("XHCI_ResumeController: function initiated\n");
    XhciExtension = (PXHCI_EXTENSION)xhciExtension;
    
    XhciExtension->Flags &= ~XHCI_FLAGS_CONTROLLER_SUSPEND;
    return MP_STATUS_SUCCESS;
}

BOOLEAN
NTAPI
XHCI_HardwarePresent(IN PXHCI_EXTENSION xhciExtension,
                     IN BOOLEAN IsInvalidateController)
{
    DPRINT1("XHCI_HardwarePresent: function initiated\n");
    return TRUE;
}

BOOLEAN
NTAPI
XHCI_InterruptService(IN PVOID xhciExtension)
{
    PULONG  RunTimeRegisterBase;
    PULONG  OperationalRegs;
    XHCI_INTERRUPTER_MANAGEMENT Iman;
    XHCI_USB_STATUS UsbStatus;
    PXHCI_EXTENSION XhciExtension;
    static ULONG ValidCount = 0;
    static ULONG SharedIrqCount = 0;

    XhciExtension = (PXHCI_EXTENSION)xhciExtension;
    RunTimeRegisterBase = XhciExtension->RunTimeRegisterBase;
    OperationalRegs = XhciExtension->OperationalRegs;
    
    // Read interrupt status and USB status
    Iman.AsULONG = READ_REGISTER_ULONG(RunTimeRegisterBase + XHCI_IMAN);
    UsbStatus.AsULONG = READ_REGISTER_ULONG(OperationalRegs + XHCI_USBSTS);
    
    // First check: Is this definitely an xHCI interrupt?
    if (Iman.InterruptPending == 1)
    {
        ValidCount++;

        /*
         * Acknowledge the interrupter in the ISR.  The event ring entries are
         * persistent until the DPC advances ERDP, but leaving IMAN.IP asserted
         * here causes an interrupt storm that can starve the DPC before the
         * transfer completion is drained.
         */
        Iman.InterruptPending = 1;
        WRITE_REGISTER_ULONG(RunTimeRegisterBase + XHCI_IMAN, Iman.AsULONG);
        return TRUE;
    }
    
    // Second check: Maybe USB Status register has events even without IMAN pending
    if (UsbStatus.EventInterrupt || UsbStatus.PortChangeDetect || UsbStatus.HostSystemError ||
        UsbStatus.SaveStateStatus || UsbStatus.RestoreStateStatus || UsbStatus.SaveRestoreError ||
        UsbStatus.HCError)
    {
        ValidCount++;
        if ((ValidCount % 10) == 1)
        {
            DPRINT1("XHCI_InterruptService: Valid USB Status interrupt #%d: EventInt=%d, PortChange=%d, Errors=%d\n",
                    ValidCount, UsbStatus.EventInterrupt, UsbStatus.PortChangeDetect, 
                    UsbStatus.HostSystemError || UsbStatus.HCError);
        }
                        
        // Create a copy to clear the status bits
        XHCI_USB_STATUS UsbStatusClear;
        UsbStatusClear.AsULONG = 0; // Clear all first
        
        // Set bits to 1 for the ones we want to clear (write-1-to-clear)
        if (UsbStatus.EventInterrupt) UsbStatusClear.EventInterrupt = 1;
        if (UsbStatus.PortChangeDetect) UsbStatusClear.PortChangeDetect = 1;
        if (UsbStatus.HostSystemError) UsbStatusClear.HostSystemError = 1;
        if (UsbStatus.SaveStateStatus) UsbStatusClear.SaveStateStatus = 1;
        if (UsbStatus.RestoreStateStatus) UsbStatusClear.RestoreStateStatus = 1;
        if (UsbStatus.SaveRestoreError) UsbStatusClear.SaveRestoreError = 1;
        if (UsbStatus.HCError) UsbStatusClear.HCError = 1;
        
        WRITE_REGISTER_ULONG(OperationalRegs + XHCI_USBSTS, UsbStatusClear.AsULONG);
        return TRUE;
    }
    
    // This is likely a shared IRQ from another device
    SharedIrqCount++;
    if ((SharedIrqCount % 100) == 1)
    {
        DPRINT1("XHCI_InterruptService: Shared IRQ #%d (not for xHCI), Iman=0x%08x, UsbStatus=0x%08x\n", 
                SharedIrqCount, Iman.AsULONG, UsbStatus.AsULONG);
    }
    
    // After many shared IRQ interrupts, warn about the situation
    if (SharedIrqCount == 1000)
    {
        DPRINT1("XHCI_InterruptService: WARNING - 1000 shared IRQ interrupts detected. This xHCI controller appears to be sharing an IRQ line with another device that is generating frequent interrupts.\n");
        DPRINT1("XHCI_InterruptService: This may impact USB performance. Consider checking IRQ assignments in device manager.\n");
    }
    
    // Don't disable our interrupts - just return FALSE to indicate this interrupt wasn't ours
    return FALSE;
}

VOID
NTAPI
XHCI_InterruptDpc(IN PVOID xhciExtension,
                  IN BOOLEAN IsDoEnableInterrupts)
{
    PXHCI_EXTENSION XhciExtension;
    PULONG RunTimeRegisterBase;
    PULONG OperationalRegs;
    XHCI_INTERRUPTER_MANAGEMENT Iman;
    XHCI_USB_STATUS UsbStatus;
    
    XhciExtension = (PXHCI_EXTENSION)xhciExtension;
    RunTimeRegisterBase = XhciExtension->RunTimeRegisterBase;
    OperationalRegs = XhciExtension->OperationalRegs;
    
    DPRINT("XHCI_InterruptDpc: Called with IsDoEnableInterrupts=%d\n", IsDoEnableInterrupts);
    
    // Read current interrupt status
    Iman.AsULONG = READ_REGISTER_ULONG(RunTimeRegisterBase + XHCI_IMAN);
    DPRINT("XHCI_InterruptDpc: Current IMAN=0x%08x, InterruptPending=%d\n", 
            Iman.AsULONG, Iman.InterruptPending);
    
    // Process any pending events
    XHCI_ProcessEvent(xhciExtension);
    
    // Clear the interrupt pending bit now that we've processed events
    if (Iman.InterruptPending == 1)
    {
        DPRINT("XHCI_InterruptDpc: Clearing interrupt pending bit\n");
        Iman.InterruptPending = 1; // Write 1 to clear
        WRITE_REGISTER_ULONG(RunTimeRegisterBase + XHCI_IMAN, Iman.AsULONG);
        
        // Verify it was cleared
        Iman.AsULONG = READ_REGISTER_ULONG(RunTimeRegisterBase + XHCI_IMAN);
        DPRINT("XHCI_InterruptDpc: After clearing, IMAN=0x%08x, InterruptPending=%d\n", 
                Iman.AsULONG, Iman.InterruptPending);
    }

    UsbStatus.AsULONG = READ_REGISTER_ULONG(OperationalRegs + XHCI_USBSTS);
    if (UsbStatus.EventInterrupt)
    {
        XHCI_USB_STATUS UsbStatusClear;

        UsbStatusClear.AsULONG = 0;
        UsbStatusClear.EventInterrupt = 1;
        WRITE_REGISTER_ULONG(OperationalRegs + XHCI_USBSTS, UsbStatusClear.AsULONG);
    }
    
    DPRINT("XHCI_InterruptDpc: Completed\n");
}

MPSTATUS
NTAPI
XHCI_SubmitTransfer(IN PVOID xhciExtension,
                    IN PVOID xhciEndpoint,
                    IN PUSBPORT_TRANSFER_PARAMETERS  transferParameters,
                    IN PVOID xhciTransfer,
                    IN PUSBPORT_SCATTER_GATHER_LIST  sgList)
{
    PXHCI_EXTENSION XhciExtension = (PXHCI_EXTENSION)xhciExtension;
    PXHCI_ENDPOINT XhciEndpoint = (PXHCI_ENDPOINT)xhciEndpoint;
    PXHCI_TRANSFER XhciTransfer = (PXHCI_TRANSFER)xhciTransfer;
    PUSBPORT_TRANSFER_PARAMETERS TransferParameters = transferParameters;
    ULONG TransferDirection;
    ULONG TransferType;
    ULONG TransferLength;
    MPSTATUS Status = MP_STATUS_SUCCESS;
    
    DPRINT("XHCI_SubmitTransfer: function initiated\n");

    if (!XhciExtension || !XhciEndpoint || !TransferParameters || !XhciTransfer)
    {
        DPRINT1("XHCI_SubmitTransfer: Invalid parameters\n");
        return MP_STATUS_FAILURE;
    }

    TransferDirection = TransferParameters->TransferFlags; //& USBPORT_TRANSFER_DIRECTION_FLAG;
    TransferType = XhciEndpoint->EndpointProperties.TransferType;
    TransferLength = TransferParameters->TransferBufferLength;

    DPRINT("XHCI_SubmitTransfer: TransferType=%d, TransferDirection=%d, TransferLength=%d\n",
            TransferType, TransferDirection, TransferLength);
    
    // Initialize transfer structure
    XhciTransfer->TransferParameters = TransferParameters;
    XhciTransfer->XhciEndpoint = XhciEndpoint;
    XhciTransfer->TransferLen = TransferLength;
    XhciTransfer->USBDStatus = USBD_STATUS_PENDING;
    
    // Handle different transfer types
    switch (TransferType)
    {
        case USBPORT_TRANSFER_TYPE_CONTROL:
            Status = XHCI_SubmitControlTransfer(XhciExtension, XhciEndpoint, XhciTransfer, sgList);
            break;
            
        case USBPORT_TRANSFER_TYPE_BULK:
            Status = XHCI_SubmitBulkTransfer(XhciExtension, XhciEndpoint, XhciTransfer, sgList);
            break;
            
        case USBPORT_TRANSFER_TYPE_INTERRUPT:
            Status = XHCI_SubmitInterruptTransfer(XhciExtension, XhciEndpoint, XhciTransfer, sgList);
            break;
            
        case USBPORT_TRANSFER_TYPE_ISOCHRONOUS:
            Status = XHCI_SubmitIsochronousTransfer(XhciExtension, XhciEndpoint, XhciTransfer, sgList);
            break;
            
        default:
            DPRINT1("XHCI_SubmitTransfer: Unknown transfer type %d\n", TransferType);
            Status = MP_STATUS_FAILURE;
            break;
    }
    
    if (Status == MP_STATUS_SUCCESS)
    {
        DPRINT("XHCI_SubmitTransfer: Transfer submitted successfully\n");
    }
    else
    {
        DPRINT1("XHCI_SubmitTransfer: Transfer submission failed with status %d\n", Status);
    }
    
    return Status;
}

MPSTATUS
NTAPI
XHCI_SubmitIsoTransfer(IN PVOID xhciExtension,
                       IN PVOID xhciEndpoint,
                       IN PUSBPORT_TRANSFER_PARAMETERS transferParameters,
                       IN PVOID xhciTransfer,
                       IN PVOID isoParameters)
{
    DPRINT1("XHCI_SubmitIsoTransfer: UNIMPLEMENTED. FIXME\n");
    return MP_STATUS_SUCCESS;
}

VOID
NTAPI
XHCI_AbortIsoTransfer(IN PXHCI_EXTENSION xhciExtension,
                      IN PXHCI_ENDPOINT xhciEndpoint,
                      IN PXHCI_TRANSFER xhciTransfer)
{
    DPRINT1("XHCI_AbortIsoTransfer: UNIMPLEMENTED. FIXME\n");
}

VOID
NTAPI
XHCI_AbortAsyncTransfer(IN PXHCI_EXTENSION xhciExtension,
                        IN PXHCI_ENDPOINT xhciEndpoint,
                        IN PXHCI_TRANSFER xhciTransfer)
{
    DPRINT1("XHCI_AbortAsyncTransfer: function initiated\n");
}

VOID
NTAPI
XHCI_AbortTransfer(IN PVOID xhciExtension,
                   IN PVOID xhciEndpoint,
                   IN PVOID xhciTransfer,
                   IN PULONG CompletedLength)
{
    DPRINT1("XHCI_AbortTransfer: function initiated\n");
}

ULONG
NTAPI
XHCI_GetEndpointState(IN PVOID xhciExtension,
                      IN PVOID xhciEndpoint)
{
    DPRINT1("XHCI_GetEndpointState: function initiated\n");
    PXHCI_ENDPOINT XhciEndpoint = xhciEndpoint;
    DPRINT1("XHCI_GetEndpointState: returning state %d\n", XhciEndpoint->EndpointState);
    return XhciEndpoint->EndpointState;
}

VOID
NTAPI
XHCI_SetEndpointState(IN PVOID xhciExtension,
                      IN PVOID xhciEndpoint,
                      IN ULONG EndpointState)
{
    PXHCI_EXTENSION XhciExtension = (PXHCI_EXTENSION)xhciExtension;
    PXHCI_ENDPOINT XhciEndpoint = (PXHCI_ENDPOINT)xhciEndpoint;
    
    DPRINT("XHCI_SetEndpointState: function initiated, setting state to %d\n", EndpointState);

    if (!XhciEndpoint) {
        DPRINT1("XHCI_SetEndpointState: Invalid endpoint pointer\n");
        return;
    }
    
    // Always update the software state
    XhciEndpoint->EndpointState = EndpointState;
    
    // Handle specific state transitions
    switch (EndpointState) {
        case 3: // USBPORT_ENDPOINT_ACTIVE
            DPRINT("XHCI_SetEndpointState: Activating endpoint (EP addr 0x%02x)\n",
                    XhciEndpoint->EndpointProperties.EndpointAddress);
            // For control endpoints (EP0), we've already configured them during OpenEndpoint
            // Just mark them as active
            if ((XhciEndpoint->EndpointProperties.EndpointAddress & 0x0F) == 0) {
                DPRINT("XHCI_SetEndpointState: Control endpoint already configured, marking as active\n");
            } else {
                /*
                 * USBPORT toggles SetEndpointState(ACTIVE) on every transfer
                 * completion's idle->active transition. On xHCI, an endpoint
                 * that's already in the Running state only needs a doorbell
                 * write to pick up newly-queued TRBs - issuing Set TR Dequeue
                 * Pointer floods the 256-entry command ring on sustained I/O
                 * (mass-storage walks file system metadata at ~one transfer
                 * per filesystem block) and stalls the controller.
                 *
                 * Only restart the EP via Set TR Dequeue after a real Halted
                 * or Stopped event (recorded in EndpointStatus). Otherwise,
                 * a doorbell ring is sufficient and idempotent.
                 */
                ULONG SlotId;
                ULONG DCI = XhciEndpoint->ContextIndex;

                SlotId = *(PULONG)&XhciEndpoint->FirstTD;
                if (SlotId == 0)
                    SlotId = XhciEndpoint->EndpointProperties.DeviceAddress;

                if (XhciEndpoint->EndpointStatus != 0)
                {
                    PHYSICAL_ADDRESS DequeuePA;
                    MPSTATUS Status;

                    DequeuePA = MmGetPhysicalAddress(
                        XhciEndpoint->TransferRing.dequeue_pointer);

                    DPRINT1("XHCI_SetEndpointState: Restarting halted EP slot=%d DCI=%d dequeue=0x%I64x cycle=%d status=0x%x\n",
                            SlotId, DCI, DequeuePA.QuadPart,
                            XhciEndpoint->TransferRing.ConsumerCycleState,
                            XhciEndpoint->EndpointStatus);

                    Status = XHCI_SetTransferRingDequeuePointer(
                        XhciExtension, SlotId, DCI,
                        DequeuePA,
                        XhciEndpoint->TransferRing.ConsumerCycleState);

                    if (Status == MP_STATUS_SUCCESS)
                    {
                        XhciEndpoint->EndpointStatus = 0;
                    }
                    else
                    {
                        DPRINT1("XHCI_SetEndpointState: SetTRDequeue failed (0x%x) slot=%d DCI=%d\n",
                                Status, SlotId, DCI);
                    }
                }

                /* Doorbell is idempotent: it tells HC to pick up new TRBs.
                 * Safe to ring whether or not we just issued SetTRDequeue. */
                XHCI_RingDoorbell(XhciExtension, SlotId, DCI);
            }
            break;
            
        case 4: // USBPORT_ENDPOINT_REMOVE
            DPRINT("XHCI_SetEndpointState: Removing endpoint\n");
            if ((XhciEndpoint->EndpointProperties.EndpointAddress & 0x0F) == 0)
            {
        }
        else
        {
            ULONG SlotId = XhciEndpoint->EndpointProperties.DeviceAddress;
            ULONG EndpointIndex = XhciEndpoint->ContextIndex;
            MPSTATUS DropStatus;

            DropStatus = XHCI_DropEndpoint(XhciExtension, SlotId, EndpointIndex);
            if (DropStatus != MP_STATUS_SUCCESS)
            {
                    DPRINT1("XHCI_SetEndpointState: XHCI_DropEndpoint failed for slot %d DCI %d with status 0x%x\n",
                            SlotId, EndpointIndex, DropStatus);
                }
            }
            break;
            
        case 5: // USBPORT_ENDPOINT_CLOSED
            DPRINT("XHCI_SetEndpointState: Closing endpoint\n");
            // TODO: Clean up endpoint resources
            break;

        default:
            DPRINT("XHCI_SetEndpointState: Setting endpoint to state %d\n", EndpointState);
            break;
    }

    DPRINT("XHCI_SetEndpointState: endpoint state set successfully to %d\n", EndpointState);
}

VOID
NTAPI
XHCI_PollEndpoint(IN PVOID xhciExtension,
                  IN PVOID xhciEndpoint)
{
    PXHCI_ENDPOINT XhciEndpoint = (PXHCI_ENDPOINT)xhciEndpoint;
    PXHCI_EXTENSION XhciExtension = (PXHCI_EXTENSION)xhciExtension;
    static ULONG PollCount = 0;
    
    PollCount++;
    if ((PollCount % 100) == 1)
    {
        DPRINT("XHCI_PollEndpoint: poll #%d initiated\n", PollCount);
        if (XhciEndpoint) {
            DPRINT("XHCI_PollEndpoint: DeviceAddress=%d, EndpointAddress=%d, EndpointState=%d\n",
                    XhciEndpoint->EndpointProperties.DeviceAddress,
                    XhciEndpoint->EndpointProperties.EndpointAddress,
                    XhciEndpoint->EndpointState);
        }
    }

    if (XhciExtension)
        XHCI_ProcessEvent(XhciExtension);
}

VOID
NTAPI
XHCI_CheckController(IN PVOID xhciExtension)
{
    PXHCI_EXTENSION XhciExtension = (PXHCI_EXTENSION)xhciExtension;

    DPRINT("XHCI_CheckController: function initiated\n");

    if (XhciExtension)
        XHCI_ProcessEvent(XhciExtension);
}

ULONG
NTAPI
XHCI_Get32BitFrameNumber(IN PVOID xhciExtension)
{
    PXHCI_EXTENSION XhciExtension = (PXHCI_EXTENSION)xhciExtension;
    PULONG RunTimeRegisterBase;
    ULONG Fn;
    ULONG Stored;
    ULONG Composed;

    /* xHCI MFINDEX is a 14-bit register (0..0x3FFF) that increments every
     * microframe (125us) and wraps every ~2s.  usbport.sys's scheduler
     * expects a monotonically increasing 32-bit frame number from
     * RegPacket.Get32BitFrameNumber and uses `FrameNumber > Endpoint->FrameNumber`
     * to decide when an endpoint is ready for state-change processing.
     *
     * A stalling / 14-bit-wrapping counter (as the previous 1 ms-cached
     * implementation produced) makes that compare fail forever and results
     * in a DPC storm from USBPORT_IsrDpcHandler via InterruptNextSOF.
     *
     * We read MFINDEX live on every call (no time-based cache) and keep
     * a software 32-bit value in XhciExtension->FrameHighPart: on each
     * call we detect wrap by comparing the live low 14 bits to the low
     * 14 bits we stored last time and, if it regressed, bump the upper
     * part by one full 14-bit range.
     *
     * Callers hold MiniportSpinLock at DISPATCH_LEVEL so the RMW on
     * FrameHighPart is serialised with the other miniport entry points. */
#define XHCI_MFINDEX_MASK       0x3FFFUL
#define XHCI_MFINDEX_RANGE      0x4000UL

    if (!XhciExtension)
    {
        return 0;
    }

    RunTimeRegisterBase = XhciExtension->RunTimeRegisterBase;
    if (!RunTimeRegisterBase)
    {
        return XhciExtension->FrameHighPart;
    }

    Fn = READ_REGISTER_ULONG(RunTimeRegisterBase + XHCI_MFINDEX) & XHCI_MFINDEX_MASK;
    Stored = XhciExtension->FrameHighPart;

    /* If the live low part is below the low part we stored last time, the
     * hardware counter wrapped at least once - advance the epoch by the
     * full 14-bit range.  We also advance on exact equality-from-above,
     * i.e. only when Fn strictly wrapped backwards. */
    if (Fn < (Stored & XHCI_MFINDEX_MASK))
    {
        Stored = (Stored & ~XHCI_MFINDEX_MASK) + XHCI_MFINDEX_RANGE;
    }

    Composed = (Stored & ~XHCI_MFINDEX_MASK) | Fn;
    XhciExtension->FrameHighPart = Composed;
    return Composed;
}

VOID
NTAPI
XHCI_InterruptNextSOF(IN PVOID xhciExtension)
{
    PXHCI_EXTENSION XhciExtension = (PXHCI_EXTENSION)xhciExtension;
    
    if (!XhciExtension)
    {
        return;
    }
    
    DPRINT("XHCI_InterruptNextSOF: Triggering soft interrupt via UsbPortInvalidateController\n");
    
    // Use the same approach as EHCI driver - trigger a soft interrupt through the USB port layer
    // This is much more reliable than manipulating hardware registers directly
    RegPacket.UsbPortInvalidateController(XhciExtension,
                                          USBPORT_INVALIDATE_CONTROLLER_SOFT_INTERRUPT);
}

VOID
NTAPI
XHCI_EnableInterrupts(IN PVOID xhciExtension)
{

    PXHCI_EXTENSION XhciExtension;
    PULONG  RunTimeRegisterBase;
    XHCI_INTERRUPTER_MANAGEMENT Iman;

    DPRINT1("XHCI_EnableInterrupts: function initiated\n");
    XhciExtension = (PXHCI_EXTENSION)xhciExtension;

    RunTimeRegisterBase =  XhciExtension->RunTimeRegisterBase;
    Iman.AsULONG = READ_REGISTER_ULONG(RunTimeRegisterBase + XHCI_IMAN);
    Iman.InterruptEnable = 1;
    WRITE_REGISTER_ULONG(RunTimeRegisterBase + XHCI_IMAN, Iman.AsULONG);
    DPRINT1("XHCI_EnableInterrupts: Interrupts enabled\n");
}

VOID
NTAPI
XHCI_DisableInterrupts(IN PVOID xhciExtension)
{

    PXHCI_EXTENSION XhciExtension;
    PULONG  RunTimeRegisterBase;
    XHCI_INTERRUPTER_MANAGEMENT Iman;
    DPRINT1("XHCI_DisableInterrupts: function initiated\n");
    XhciExtension = (PXHCI_EXTENSION)xhciExtension;

    RunTimeRegisterBase =  XhciExtension -> RunTimeRegisterBase;
    Iman.AsULONG = READ_REGISTER_ULONG(RunTimeRegisterBase + XHCI_IMAN);
    Iman.InterruptEnable = 0;
    WRITE_REGISTER_ULONG(RunTimeRegisterBase + XHCI_IMAN,Iman.AsULONG);

    DPRINT1("XHCI_EnableInterrupts: Interrupts enabled\n");
}

VOID
NTAPI
XHCI_PollController(IN PVOID xhciExtension)
{

    PXHCI_EXTENSION XhciExtension;
    DPRINT("XHCI_PollController: function initiated\n");
    XhciExtension = (PXHCI_EXTENSION)xhciExtension;

    if (!(XhciExtension->Flags & XHCI_FLAGS_CONTROLLER_SUSPEND))
    {
        RegPacket.UsbPortInvalidateRootHub(XhciExtension);
        return;
    }

     XHCI_ProcessEvent(xhciExtension);

}

VOID
NTAPI
XHCI_SetEndpointDataToggle(IN PVOID xhciExtension,
                           IN PVOID xhciEndpoint,
                           IN ULONG DataToggle)
{
    DPRINT1("XHCI_SetEndpointDataToggle: function initiated\n");
}

ULONG
NTAPI
XHCI_GetEndpointStatus(IN PVOID xhciExtension,
                       IN PVOID xhciEndpoint)
{
    DPRINT1("XHCI_GetEndpointStatus: function initiated\n");
    return 0;
}

VOID
NTAPI
XHCI_SetEndpointStatus(IN PVOID xhciExtension,
                       IN PVOID xhciEndpoint,
                       IN ULONG EndpointStatus)
{
    PXHCI_ENDPOINT XhciEndpoint = (PXHCI_ENDPOINT)xhciEndpoint;

    DPRINT1("XHCI_SetEndpointStatus: setting status=0x%x\n", EndpointStatus);
    if (XhciEndpoint != NULL)
    {
        /* Tracked so SetEndpointState(ACTIVE) can decide whether the EP
         * Context needs Set TR Dequeue Pointer (on real Halted/Stopped
         * state) or just a doorbell ring (normal idle->active). */
        XhciEndpoint->EndpointStatus = EndpointStatus;
    }
}

MPSTATUS
NTAPI
XHCI_SetTransferRingDequeuePointer(IN PXHCI_EXTENSION XhciExtension,
                                   IN ULONG SlotId,
                                   IN ULONG EndpointIndex,
                                   IN PHYSICAL_ADDRESS NewDequeuePointer,
                                   IN ULONG CycleState)
{
    XHCI_TRB SetDequeuePointerTrb;
    PXHCI_PENDING_COMMAND PendingCommand;
    PHYSICAL_ADDRESS TrbPA;
    MPSTATUS Status;
    
    DPRINT1("XHCI_SetTransferRingDequeuePointer: Setting dequeue pointer for slot %d, endpoint %d\n", 
            SlotId, EndpointIndex);
    DPRINT1("XHCI_SetTransferRingDequeuePointer: New dequeue PA=0x%I64x, cycle state=%d\n", 
            NewDequeuePointer.QuadPart, CycleState);
    
    if (!XhciExtension || SlotId == 0 || EndpointIndex == 0)
    {
        DPRINT1("XHCI_SetTransferRingDequeuePointer: Invalid parameters\n");
        return MP_STATUS_FAILURE;
    }
    
    // Create Set TR Dequeue Pointer command TRB
    RtlZeroMemory(&SetDequeuePointerTrb, sizeof(XHCI_TRB));
    
    // Set the new dequeue pointer address with DCS (Dequeue Cycle State) bit
    // The DCS bit is bit 0 of the dequeue pointer
    ULONGLONG DequeuePointerWithDCS = (NewDequeuePointer.QuadPart & ~0xF) | (CycleState ? 1 : 0);
    
    SetDequeuePointerTrb.GenericTRB.Word0 = (ULONG)(DequeuePointerWithDCS & 0xFFFFFFFF);
    SetDequeuePointerTrb.GenericTRB.Word1 = (ULONG)(DequeuePointerWithDCS >> 32);
    SetDequeuePointerTrb.GenericTRB.Word2 = 0; // Reserved
    SetDequeuePointerTrb.GenericTRB.Word3 = ((SET_TR_DEQUEUE_POINTER_COMMAND << 10) | // TRB Type
                                            (EndpointIndex << 16) |                    // Endpoint ID
                                            (SlotId << 24) |                          // Slot ID
                                            1);                                       // Cycle bit (will be set by XHCI_SendCommand)
    
    DPRINT1("XHCI_SetTransferRingDequeuePointer: Command TRB - Word0=0x%08x, Word1=0x%08x, Word2=0x%08x, Word3=0x%08x\n",
            SetDequeuePointerTrb.GenericTRB.Word0, SetDequeuePointerTrb.GenericTRB.Word1,
            SetDequeuePointerTrb.GenericTRB.Word2, SetDequeuePointerTrb.GenericTRB.Word3);
    
    // Pre-calculate the TRB physical address where the command will be placed
    PXHCI_HC_RESOURCES HcResourcesVA = XhciExtension->HcResourcesVA;
    PHYSICAL_ADDRESS HcResourcesPA = XhciExtension->HcResourcesPA;
    PXHCI_TRB enqueue_pointer = HcResourcesVA->CommandRing.enqueue_pointer;
    
    // Calculate the exact physical address where this TRB will be placed
    ULONG TrbIndex = (ULONG)((ULONG_PTR)enqueue_pointer - 
                            (ULONG_PTR)&HcResourcesVA->CommandRing.firstSeg.XhciTrb[0]) / sizeof(XHCI_TRB);
    TrbPA.QuadPart = HcResourcesPA.QuadPart + 
                     FIELD_OFFSET(XHCI_HC_RESOURCES, CommandRing.firstSeg.XhciTrb[0]) +
                     (TrbIndex * sizeof(XHCI_TRB));
    
    // Add to pending commands BEFORE sending the command to prevent race conditions
    PendingCommand = AddPendingCommand(COMMAND_SET_TR_DEQUEUE_POINTER, TrbPA, SlotId);
    if (!PendingCommand)
    {
        DPRINT1("XHCI_SetTransferRingDequeuePointer: Failed to add pending command\n");
        return MP_STATUS_FAILURE;
    }
    
    // Send command to the controller
    Status = XHCI_SendCommand(SetDequeuePointerTrb, XhciExtension);
    if (Status != MP_STATUS_SUCCESS)
    {
        DPRINT1("XHCI_SetTransferRingDequeuePointer: Failed to send Set TR Dequeue Pointer command\n");
        // Remove the pending command since we failed to send it
        RemovePendingCommand(PendingCommand);
        return Status;
    }
    
    // Wait for completion (with timeout)
    Status = WaitForCommandCompletion(XhciExtension, PendingCommand, 1000); // 1 second timeout
    if (Status == MP_STATUS_SUCCESS)
    {
        DPRINT1("XHCI_SetTransferRingDequeuePointer: Set TR Dequeue Pointer completed successfully\n");
    }
    else
    {
        DPRINT1("XHCI_SetTransferRingDequeuePointer: Set TR Dequeue Pointer failed or timed out\n");
    }
    
    // Remove from pending commands
    RemovePendingCommand(PendingCommand);
    
    return Status;
}

MPSTATUS
NTAPI
XHCI_StartSendOnePacket(IN PVOID xhciExtension,
                        IN PVOID PacketParameters,
                        IN PVOID Data,
                        IN PULONG pDataLength,
                        IN PVOID BufferVA,
                        IN PVOID BufferPA,
                        IN ULONG BufferLength,
                        IN USBD_STATUS * pUSBDStatus)
{
    DPRINT1("XHCI_StartSendOnePacket: UNIMPLEMENTED. FIXME\n");
    return MP_STATUS_SUCCESS;
}

MPSTATUS
NTAPI
XHCI_EndSendOnePacket(IN PVOID xhciExtension,
                      IN PVOID PacketParameters,
                      IN PVOID Data,
                      IN PULONG pDataLength,
                      IN PVOID BufferVA,
                      IN PVOID BufferPA,
                      IN ULONG BufferLength,
                      IN USBD_STATUS * pUSBDStatus)
{
    DPRINT1("XHCI_EndSendOnePacket: UNIMPLEMENTED. FIXME\n");
    return MP_STATUS_SUCCESS;
}

MPSTATUS
NTAPI
XHCI_PassThru(IN PVOID xhciExtension,
              IN PVOID passThruParameters,
              IN ULONG ParameterLength,
              IN PVOID pParameters)
{
    DPRINT1("XHCI_PassThru: UNIMPLEMENTED. FIXME\n");
    return MP_STATUS_SUCCESS;
}

VOID
NTAPI
XHCI_RebalanceEndpoint(IN PVOID ohciExtension,
                       IN PUSBPORT_ENDPOINT_PROPERTIES endpointParameters,
                       IN PVOID ohciEndpoint)
{
    DPRINT1("XHCI_RebalanceEndpoint: UNIMPLEMENTED. FIXME\n");
}

VOID
NTAPI
XHCI_FlushInterrupts(IN PVOID xhciExtension)
{
    DPRINT1("XHCI_FlushInterrupts: function initiated\n");
}

MPSTATUS
NTAPI
XHCI_RH_ChirpRootPort(IN PVOID xhciExtension,
                      IN USHORT Port)
{
    DPRINT1("XHCI_RH_ChirpRootPort: UNIMPLEMENTED. FIXME\n");
    return MP_STATUS_SUCCESS;
}

VOID
NTAPI
XHCI_TakePortControl(IN PVOID ohciExtension)
{
    DPRINT1("XHCI_TakePortControl: UNIMPLEMENTED. FIXME\n");
}

VOID
NTAPI
XHCI_Unload(PDRIVER_OBJECT DriverObject)
{
    DPRINT1("XHCI_Unload: UNIMPLEMENTED. FIXME\n");
}

NTSTATUS
NTAPI
DriverEntry(IN PDRIVER_OBJECT DriverObject,
            IN PUNICODE_STRING RegistryPath)
{
    DPRINT1("DriverEntry: DriverObject - %p, RegistryPath - %wZ\n",
           DriverObject,
           RegistryPath);
    if (USBPORT_GetHciMn() != USBPORT_HCI_MN) 
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
  //  __debugbreak();
    RtlZeroMemory(&RegPacket, sizeof(USBPORT_REGISTRATION_PACKET));
    
    RegPacket.MiniPortVersion = USB_MINIPORT_VERSION_XHCI;
    /* FIXE: USB_MINIPORT_FLAGS_USB2 on a USB3 driver? */
    RegPacket.MiniPortFlags = USB_MINIPORT_FLAGS_INTERRUPT |
                              USB_MINIPORT_FLAGS_MEMORY_IO |
                              USB_MINIPORT_FLAGS_USB2 |
                              USB_MINIPORT_FLAGS_POLLING |
                              USB_MINIPORT_FLAGS_WAKE_SUPPORT;
#if (NTDDI_VERSION >= NTDDI_WIN8)
    /*
     * Advertise SuperSpeed capability so usbport will honour the
     * Usb30PortStatus.NegotiatedDeviceSpeed field populated by
     * XHCI_RH_GetPortStatus. Without this, USBPORT_CreateDevice falls back
     * to the legacy USB 2.0 port-status bits and misclassifies SuperSpeed
     * connects as UsbHighSpeed, producing xHCI slot-context Speed=3 and a
     * Parameter Error on CONFIGURE_ENDPOINT for EP MaxPacketSize=1024.
     */
    RegPacket.MiniPortFlags |= USB_MINIPORT_FLAGS_USB3;
#endif

    RegPacket.MiniPortBusBandwidth = TOTAL_USB30_BUS_BANDWIDTH;

    RegPacket.MiniPortExtensionSize = sizeof(XHCI_EXTENSION);
    RegPacket.MiniPortEndpointSize = sizeof(XHCI_ENDPOINT);
    RegPacket.MiniPortTransferSize = sizeof(XHCI_TRANSFER);
    RegPacket.MiniPortResourcesSize = sizeof(XHCI_HC_RESOURCES);
    
    RegPacket.OpenEndpoint = XHCI_OpenEndpoint;
    RegPacket.ReopenEndpoint = XHCI_ReopenEndpoint;
    RegPacket.QueryEndpointRequirements = XHCI_QueryEndpointRequirements;
    RegPacket.CloseEndpoint = XHCI_CloseEndpoint;
    RegPacket.StartController = XHCI_StartController;
    RegPacket.StopController = XHCI_StopController;
    RegPacket.SuspendController = XHCI_SuspendController;
    RegPacket.ResumeController = XHCI_ResumeController;
    RegPacket.InterruptService = XHCI_InterruptService;
    RegPacket.InterruptDpc = XHCI_InterruptDpc;
    RegPacket.SubmitTransfer = XHCI_SubmitTransfer;
    RegPacket.SubmitIsoTransfer = XHCI_SubmitIsoTransfer;
    RegPacket.AbortTransfer = XHCI_AbortTransfer;
    RegPacket.GetEndpointState = XHCI_GetEndpointState;
    RegPacket.SetEndpointState = XHCI_SetEndpointState;
    RegPacket.PollEndpoint = XHCI_PollEndpoint;
    RegPacket.CheckController = XHCI_CheckController;
    RegPacket.Get32BitFrameNumber = XHCI_Get32BitFrameNumber;
    RegPacket.InterruptNextSOF = XHCI_InterruptNextSOF;
    RegPacket.EnableInterrupts = XHCI_EnableInterrupts;
    RegPacket.DisableInterrupts = XHCI_DisableInterrupts;
    RegPacket.PollController = XHCI_PollController;
    RegPacket.SetEndpointDataToggle = XHCI_SetEndpointDataToggle;
    RegPacket.GetEndpointStatus = XHCI_GetEndpointStatus;
    RegPacket.SetEndpointStatus = XHCI_SetEndpointStatus;
    RegPacket.RH_GetRootHubData = XHCI_RH_GetRootHubData;
    RegPacket.RH_GetStatus = XHCI_RH_GetStatus;
    RegPacket.RH_GetPortStatus = XHCI_RH_GetPortStatus;
    RegPacket.RH_GetHubStatus = XHCI_RH_GetHubStatus;
    RegPacket.RH_SetFeaturePortReset = XHCI_RH_SetFeaturePortReset;
    RegPacket.RH_SetFeaturePortPower = XHCI_RH_SetFeaturePortPower;
    RegPacket.RH_SetFeaturePortEnable = XHCI_RH_SetFeaturePortEnable;
    RegPacket.RH_SetFeaturePortSuspend = XHCI_RH_SetFeaturePortSuspend;
    RegPacket.RH_ClearFeaturePortEnable = XHCI_RH_ClearFeaturePortEnable;
    RegPacket.RH_ClearFeaturePortPower = XHCI_RH_ClearFeaturePortPower;
    RegPacket.RH_ClearFeaturePortSuspend = XHCI_RH_ClearFeaturePortSuspend;
    RegPacket.RH_ClearFeaturePortEnableChange = XHCI_RH_ClearFeaturePortEnableChange;
    RegPacket.RH_ClearFeaturePortConnectChange = XHCI_RH_ClearFeaturePortConnectChange;
    RegPacket.RH_ClearFeaturePortResetChange = XHCI_RH_ClearFeaturePortResetChange;
    RegPacket.RH_ClearFeaturePortSuspendChange = XHCI_RH_ClearFeaturePortSuspendChange;
    RegPacket.RH_ClearFeaturePortOvercurrentChange = XHCI_RH_ClearFeaturePortOvercurrentChange;
    RegPacket.RH_DisableIrq = XHCI_RH_DisableIrq;
    RegPacket.RH_EnableIrq = XHCI_RH_EnableIrq;
    RegPacket.StartSendOnePacket = XHCI_StartSendOnePacket;
    RegPacket.EndSendOnePacket = XHCI_EndSendOnePacket;
    RegPacket.PassThru = XHCI_PassThru;
    RegPacket.RebalanceEndpoint = XHCI_RebalanceEndpoint;
    RegPacket.FlushInterrupts = XHCI_FlushInterrupts;
    RegPacket.RH_ChirpRootPort = XHCI_RH_ChirpRootPort;
    RegPacket.TakePortControl = XHCI_TakePortControl;
    
    DPRINT1("XHCI_DriverEntry: before driver unload. FIXME\n");
    DriverObject->DriverUnload = XHCI_Unload;
    
    DPRINT1("XHCI_DriverEntry: after driver unload, before usbport_reg call. FIXME\n");

    return USBPORT_RegisterUSBPortDriver(DriverObject, USB30_MINIPORT_INTERFACE_VERSION, &RegPacket);

}
