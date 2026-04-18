/*
 * PROJECT:         ReactOS xHCI Driver
 * LICENSE:         GPLv2+ - See COPYING in the top level directory
 * PURPOSE:         Private management functions of xHCI
 * COPYRIGHT:       Copyright 2021 Justin Miller <justinmiller100@gmail.com>
 * 
 * IMPLEMENTATION STATUS (2025-06-25):
 * ===================================
 * 
 * COMPLETED:
 * - Command completion tracking system with proper event handling
 * - Enable Slot and Address Device commands with command completion waiting
 * - Device speed detection from port status registers
 * - EP0 max packet size selection based on device speed
 * - Comprehensive device enumeration function (XHCI_EnumerateDevice)
 * - Event ring processing improvements for command completion events
 * - Debug output throughout the enumeration process
 * - Helper functions for command completion waiting
 * 
 * TODO:
 * - Dynamic allocation of input/output device contexts per device
 * - DCBAA (Device Context Base Address Array) setup and management
 * - Reading device address from output device context after Address Device
 * - Transfer ring management per endpoint
 * - Configure Endpoint command implementation
 * - Proper device removal and slot cleanup
 * - Integration with ReactOS USB stack for device operations
 * - Error recovery and timeout handling improvements
 */

#include "usbxhcip.h"
#include "hardware.h"
#include "../usbport/usbport.h"  // For USBPORT_ENDPOINT structure
#define NDEBUG
#include <debug.h>
#define NDEBUG_XHCI_TRACE
#include "dbg_xhci.h"

// Transfer tracking system - TODO: Replace with proper per-endpoint transfer queues
#define MAX_PENDING_TRANSFERS 32

// Work item structure for deferred device enumeration
typedef struct _XHCI_ENUMERATION_WORK_ITEM {
    WORK_QUEUE_ITEM WorkItem;
    PXHCI_EXTENSION XhciExtension;
    ULONG PortNumber;
} XHCI_ENUMERATION_WORK_ITEM, *PXHCI_ENUMERATION_WORK_ITEM;

typedef struct _XHCI_PENDING_TRANSFER {
    PXHCI_TRANSFER XhciTransfer;
    PXHCI_ENDPOINT XhciEndpoint;
    PXHCI_EXTENSION XhciExtension;
    PHYSICAL_ADDRESS TrbPointers[3];  // Setup, Data (optional), Status TRBs
    ULONG TrbCount;                   // Number of valid TRB pointers
    ULONG RequestedLength;            // Original request length for proper transfer length calculation
    BOOLEAN InUse;
} XHCI_PENDING_TRANSFER, *PXHCI_PENDING_TRANSFER;

static XHCI_PENDING_TRANSFER g_PendingTransfers[MAX_PENDING_TRANSFERS];
static BOOLEAN g_TransferTrackingInitialized = FALSE;

// Command completion tracking system
#define MAX_PENDING_COMMANDS 8

typedef struct _XHCI_PENDING_COMMAND {
    XHCI_COMMAND_TYPE CommandType;
    PHYSICAL_ADDRESS TrbPointer;
    ULONG SlotId;
    ULONG CompletionCode;
    ULONG CompletionSlotId;  // Slot ID returned from completion event
    BOOLEAN Completed;
    BOOLEAN InUse;
} XHCI_PENDING_COMMAND, *PXHCI_PENDING_COMMAND;

static XHCI_PENDING_COMMAND g_PendingCommands[MAX_PENDING_COMMANDS];
static KSPIN_LOCK g_PendingCommandsLock;
static BOOLEAN g_CommandTrackingInitialized = FALSE;

// Slot management tracking system
#define MAX_DEVICE_SLOTS 255
#define INVALID_SLOT_ID 0

typedef struct _XHCI_SLOT_INFO {
    ULONG SlotId;
    ULONG PortNumber;
    ULONG DeviceAddress;
    BOOLEAN InUse;
    BOOLEAN BeingEnumerated;  // Slot is in process of enumeration
    XHCI_INPUT_CONTEXT InputContext;  // Per-slot input context to prevent corruption
    PHYSICAL_ADDRESS InputContextPA;   // Physical address of this slot's input context
    PXHCI_RING EP0TransferRing;        // EP0 transfer ring for this slot
    PXHCI_INPUT_CONTEXT AllocatedInputContext;  // Pointer to the allocated DMA input context
    PVOID AllocatedMemory;             // Original allocated memory for cleanup
} XHCI_SLOT_INFO, *PXHCI_SLOT_INFO;

static XHCI_SLOT_INFO g_DeviceSlots[MAX_DEVICE_SLOTS];
static BOOLEAN g_SlotTrackingInitialized = FALSE;
VOID
UnregisterPendingTransfer(IN PXHCI_PENDING_TRANSFER PendingTransfer);
// Function prototypes
VOID InitializeSlotTracking(VOID);
ULONG ReserveSlotForEnumeration(ULONG PortNumber);
VOID ConfirmSlotAllocation(ULONG SlotId, ULONG DeviceAddress);
VOID ReleaseSlot(ULONG SlotId);
BOOLEAN IsSlotInUse(ULONG SlotId);
PXHCI_INPUT_CONTEXT GetSlotInputContext(ULONG SlotId, PPHYSICAL_ADDRESS InputContextPA);
VOID NTAPI CleanupSlotResources(IN PXHCI_EXTENSION XhciExtension, IN ULONG SlotId);
VOID RemovePendingCommand(PXHCI_PENDING_COMMAND PendingCommand);
PXHCI_PENDING_COMMAND FindPendingCommandInternal(PHYSICAL_ADDRESS TrbPointer);

/* Forward declaration for XHCI_OpenBulkEndpoint call site
 * (definition lives after XHCI_AddressDevice). */
MPSTATUS
NTAPI
XHCI_ConfigureEndpoint(IN PXHCI_EXTENSION XhciExtension,
                       IN ULONG SlotId,
                       IN ULONG EndpointIndex,
                       IN ULONG EndpointType,
                       IN ULONG MaxPacketSize,
                       IN PHYSICAL_ADDRESS TransferRingPA);

// Transfer tracking helper functions
VOID
NTAPI
InitializeTransferTracking(VOID)
{
    ULONG i;
    
    if (g_TransferTrackingInitialized)
        return;
        
    for (i = 0; i < MAX_PENDING_TRANSFERS; i++)
    {
        RtlZeroMemory(&g_PendingTransfers[i], sizeof(XHCI_PENDING_TRANSFER));
        g_PendingTransfers[i].InUse = FALSE;
    }
    
    g_TransferTrackingInitialized = TRUE;
    DPRINT1("InitializeTransferTracking: Transfer tracking system initialized\n");
}

MPSTATUS
RegisterPendingTransfer(IN PXHCI_EXTENSION XhciExtension,
                       IN PXHCI_ENDPOINT XhciEndpoint,
                       IN PXHCI_TRANSFER XhciTransfer,
                       IN PHYSICAL_ADDRESS TrbPointers[],
                       IN ULONG TrbCount,
                       IN ULONG RequestedLength)
{
    ULONG i, j;
    
    InitializeTransferTracking();
    
    // First, try to clean up any completed transfers to free up slots
    for (i = 0; i < MAX_PENDING_TRANSFERS; i++)
    {
        if (g_PendingTransfers[i].InUse && g_PendingTransfers[i].XhciTransfer)
        {
            // Check if this transfer has already been completed (simplified check)
            // This is a basic cleanup - a more sophisticated system would track completion status
            PUSBPORT_TRANSFER_PARAMETERS Params = g_PendingTransfers[i].XhciTransfer->TransferParameters;
            if (Params && g_PendingTransfers[i].XhciTransfer->USBDStatus != USBD_STATUS_PENDING)
            {
                DPRINT1("RegisterPendingTransfer: Cleaning up completed transfer at index %d (status=0x%x)\n", 
                        i, g_PendingTransfers[i].XhciTransfer->USBDStatus);
                UnregisterPendingTransfer(&g_PendingTransfers[i]);
            }
        }
    }
    
    // Now try to find a free slot
    for (i = 0; i < MAX_PENDING_TRANSFERS; i++)
    {
        if (!g_PendingTransfers[i].InUse)
        {
            g_PendingTransfers[i].XhciTransfer = XhciTransfer;
            g_PendingTransfers[i].XhciEndpoint = XhciEndpoint;
            g_PendingTransfers[i].XhciExtension = XhciExtension;
            g_PendingTransfers[i].TrbCount = TrbCount;
            for (j = 0; j < TrbCount && j < 3; j++)
            {
                g_PendingTransfers[i].TrbPointers[j] = TrbPointers[j];
            }
            g_PendingTransfers[i].RequestedLength = RequestedLength;
            g_PendingTransfers[i].InUse = TRUE;
            
            DPRINT1("RegisterPendingTransfer: Registered transfer at index %d with %d TRBs, requested length %d\n", 
                    i, TrbCount, RequestedLength);
            for (j = 0; j < TrbCount; j++)
            {
                DPRINT1("RegisterPendingTransfer: TRB[%d] = 0x%I64x\n", j, g_PendingTransfers[i].TrbPointers[j].QuadPart);
            }
            return MP_STATUS_SUCCESS;
        }
    }
    
    // If we still can't find a slot, force cleanup by removing the oldest entries
    DPRINT1("RegisterPendingTransfer: All slots full, forcing cleanup of first 4 entries\n");
    for (i = 0; i < 4; i++)
    {
        if (g_PendingTransfers[i].InUse)
        {
            DPRINT1("RegisterPendingTransfer: Force cleaning slot %d\n", i);
            UnregisterPendingTransfer(&g_PendingTransfers[i]);
        }
    }
    
    // Try again with the first cleaned slot
    if (!g_PendingTransfers[0].InUse)
    {
        g_PendingTransfers[0].XhciTransfer = XhciTransfer;
        g_PendingTransfers[0].XhciEndpoint = XhciEndpoint;
        g_PendingTransfers[0].XhciExtension = XhciExtension;
        g_PendingTransfers[0].TrbCount = TrbCount;
        for (j = 0; j < TrbCount && j < 3; j++)
        {
            g_PendingTransfers[0].TrbPointers[j] = TrbPointers[j];
        }
        g_PendingTransfers[0].RequestedLength = RequestedLength;
        g_PendingTransfers[0].InUse = TRUE;
        
        DPRINT1("RegisterPendingTransfer: Registered transfer at cleaned slot 0\n");
        return MP_STATUS_SUCCESS;
    }
    
    DPRINT1("RegisterPendingTransfer: Failed to find or create free slot for transfer tracking\n");
    return MP_STATUS_FAILURE;
}

PXHCI_PENDING_TRANSFER
FindPendingTransfer(IN PHYSICAL_ADDRESS TrbPointer)
{
    ULONG i, j;
    
    for (i = 0; i < MAX_PENDING_TRANSFERS; i++)
    {
        if (g_PendingTransfers[i].InUse)
        {
            // Check all TRB pointers in this transfer
            for (j = 0; j < g_PendingTransfers[i].TrbCount; j++)
            {
                if (g_PendingTransfers[i].TrbPointers[j].QuadPart == TrbPointer.QuadPart)
                {
                    DPRINT1("FindPendingTransfer: Found transfer at index %d for TRB pointer 0x%I64x (TRB %d of %d)\n",
                            i, TrbPointer.QuadPart, j, g_PendingTransfers[i].TrbCount);
                    return &g_PendingTransfers[i];
                }
            }
        }
    }
    
    DPRINT1("FindPendingTransfer: No transfer found for TRB pointer 0x%I64x\n", TrbPointer.QuadPart);
    return NULL;
}

VOID
UnregisterPendingTransfer(IN PXHCI_PENDING_TRANSFER PendingTransfer)
{
    if (PendingTransfer && PendingTransfer->InUse)
    {
        DPRINT1("UnregisterPendingTransfer: Unregistering transfer with %d TRBs\n",
                PendingTransfer->TrbCount);
        RtlZeroMemory(PendingTransfer, sizeof(XHCI_PENDING_TRANSFER));
        PendingTransfer->InUse = FALSE;
    }
}

// Command tracking helper functions
VOID
NTAPI
InitializeCommandTracking(VOID)
{
    ULONG i;
    
    if (g_CommandTrackingInitialized)
        return;
    
    // Initialize the spinlock for thread-safe access
    KeInitializeSpinLock(&g_PendingCommandsLock);
        
    for (i = 0; i < MAX_PENDING_COMMANDS; i++)
    {
        RtlZeroMemory(&g_PendingCommands[i], sizeof(XHCI_PENDING_COMMAND));
        g_PendingCommands[i].InUse = FALSE;
        g_PendingCommands[i].Completed = FALSE;
    }
    
    g_CommandTrackingInitialized = TRUE;
    DPRINT1("InitializeCommandTracking: Command tracking system initialized with spinlock protection\n");
}

// Internal helper function that assumes the spinlock is already held
PXHCI_PENDING_COMMAND
FindPendingCommandInternal(PHYSICAL_ADDRESS TrbPointer)
{
    ULONG i;
    PXHCI_PENDING_COMMAND FoundCommand = NULL;
    
    DPRINT1("FindPendingCommandInternal: Looking for TRB 0x%I64x (lock already held)\n", TrbPointer.QuadPart);
    
    for (i = 0; i < MAX_PENDING_COMMANDS; i++)
    {
        if (g_PendingCommands[i].InUse && !g_PendingCommands[i].Completed)
        {
            DPRINT1("FindPendingCommandInternal: Index %d - Command type %d, TRB 0x%I64x, InUse=%d, Completed=%d\n",
                    i, g_PendingCommands[i].CommandType, g_PendingCommands[i].TrbPointer.QuadPart, 
                    g_PendingCommands[i].InUse, g_PendingCommands[i].Completed);
            
            // Compare with 16-byte alignment (hardware only reports upper 60 bits)
            ULONGLONG StoredAddr = g_PendingCommands[i].TrbPointer.QuadPart & ~0xFULL;
            ULONGLONG SearchAddr = TrbPointer.QuadPart & ~0xFULL;
            
            if (StoredAddr == SearchAddr)
            {
                DPRINT1("FindPendingCommandInternal: MATCH FOUND at index %d (16-byte aligned comparison)\n", i);
                FoundCommand = &g_PendingCommands[i];
                break;
            }
            
            // Also try exact match for compatibility
            if (g_PendingCommands[i].TrbPointer.QuadPart == TrbPointer.QuadPart)
            {
                DPRINT1("FindPendingCommandInternal: EXACT MATCH FOUND at index %d\n", i);
                FoundCommand = &g_PendingCommands[i];
                break;
            }
        }
        else if (g_PendingCommands[i].InUse && g_PendingCommands[i].Completed)
        {
            // Skip completed commands to prevent duplicate processing
            DPRINT1("FindPendingCommandInternal: Index %d - Skipping completed command type %d\n",
                    i, g_PendingCommands[i].CommandType);
        }
    }
    
    if (!FoundCommand)
    {
        DPRINT1("FindPendingCommandInternal: NO MATCH FOUND for TRB 0x%I64x\n", TrbPointer.QuadPart);
    }
    
    return FoundCommand;
}

PXHCI_PENDING_COMMAND
FindPendingCommand(PHYSICAL_ADDRESS TrbPointer)
{
    PXHCI_PENDING_COMMAND FoundCommand;
    KIRQL OldIrql;
    
    KeAcquireSpinLock(&g_PendingCommandsLock, &OldIrql);
    FoundCommand = FindPendingCommandInternal(TrbPointer);
    KeReleaseSpinLock(&g_PendingCommandsLock, OldIrql);
    
    return FoundCommand;
}

PXHCI_PENDING_COMMAND
AddPendingCommand(XHCI_COMMAND_TYPE CommandType, PHYSICAL_ADDRESS TrbPointer, ULONG SlotId)
{
    ULONG i;
    PXHCI_PENDING_COMMAND NewCommand = NULL;
    KIRQL OldIrql;
    
    KeAcquireSpinLock(&g_PendingCommandsLock, &OldIrql);
    
    for (i = 0; i < MAX_PENDING_COMMANDS; i++)
    {
        if (!g_PendingCommands[i].InUse)
        {
            g_PendingCommands[i].CommandType = CommandType;
            g_PendingCommands[i].TrbPointer = TrbPointer;
            g_PendingCommands[i].SlotId = SlotId;
            g_PendingCommands[i].CompletionCode = 0;
            g_PendingCommands[i].CompletionSlotId = 0;
            g_PendingCommands[i].Completed = FALSE;
            g_PendingCommands[i].InUse = TRUE;
            
            NewCommand = &g_PendingCommands[i];
            
            DPRINT1("AddPendingCommand: Added command type %d, slot %d, TRB 0x%I64x at index %d\n",
                    CommandType, SlotId, TrbPointer.QuadPart, i);
            break;
        }
    }
    
    KeReleaseSpinLock(&g_PendingCommandsLock, OldIrql);
    
    if (!NewCommand)
    {
        DPRINT1("AddPendingCommand: No free command slots available!\n");
    }
    
    return NewCommand;
}

VOID
CompletePendingCommand(PHYSICAL_ADDRESS TrbPointer, ULONG CompletionCode, ULONG SlotIdFromEvent)
{
    PXHCI_PENDING_COMMAND PendingCommand;
    KIRQL OldIrql;
    
    // Use a lock to prevent race conditions during command completion
    KeAcquireSpinLock(&g_PendingCommandsLock, &OldIrql);
    
    // Use the internal version that doesn't acquire the spinlock since we already hold it
    PendingCommand = FindPendingCommandInternal(TrbPointer);
    if (PendingCommand)
    {
        // Check if this command was already completed to prevent duplicate processing
        if (PendingCommand->Completed)
        {
            DPRINT1("CompletePendingCommand: WARNING - Command type %d already completed, ignoring duplicate event\n",
                    PendingCommand->CommandType);
            KeReleaseSpinLock(&g_PendingCommandsLock, OldIrql);
            return;
        }
        
        // Mark as completed atomically
        PendingCommand->CompletionCode = CompletionCode;
        PendingCommand->CompletionSlotId = SlotIdFromEvent;
        PendingCommand->Completed = TRUE;
        
        DPRINT1("CompletePendingCommand: Completed command type %d, slot %d, completion code %d, event slot ID %d\n",
                PendingCommand->CommandType, PendingCommand->SlotId, CompletionCode, SlotIdFromEvent);
        
        // DO NOT remove the command immediately - let WaitForCommandCompletion handle it
        // This prevents race conditions where the waiter can't find the completed command
    }
    else
    {
        DPRINT1("CompletePendingCommand: Could not find pending command for TRB 0x%I64x\n", TrbPointer.QuadPart);
    }
    
    KeReleaseSpinLock(&g_PendingCommandsLock, OldIrql);
}

VOID
RemovePendingCommand(PXHCI_PENDING_COMMAND PendingCommand)
{
    if (PendingCommand && PendingCommand->InUse)
    {
        DPRINT1("RemovePendingCommand: Removing command type %d, slot %d\n",
                PendingCommand->CommandType, PendingCommand->SlotId);
        
        RtlZeroMemory(PendingCommand, sizeof(XHCI_PENDING_COMMAND));
        PendingCommand->InUse = FALSE;
        PendingCommand->Completed = FALSE;
    }
}

// Clean up completed commands to prevent array overflow
VOID
CleanupCompletedCommands(VOID)
{
    ULONG i;
    ULONG CleanedCount = 0;
    KIRQL OldIrql;
    
    KeAcquireSpinLock(&g_PendingCommandsLock, &OldIrql);
    
    for (i = 0; i < MAX_PENDING_COMMANDS; i++)
    {
        if (g_PendingCommands[i].InUse && g_PendingCommands[i].Completed)
        {
            // Command has been completed and processed, safe to remove
            RtlZeroMemory(&g_PendingCommands[i], sizeof(XHCI_PENDING_COMMAND));
            g_PendingCommands[i].InUse = FALSE;
            g_PendingCommands[i].Completed = FALSE;
            CleanedCount++;
        }
    }
    
    KeReleaseSpinLock(&g_PendingCommandsLock, OldIrql);
    
    if (CleanedCount > 0)
    {
        DPRINT1("CleanupCompletedCommands: Cleaned up %d completed commands\n", CleanedCount);
    }
}

// Work item routine for device enumeration (runs at PASSIVE_LEVEL)
VOID
NTAPI
XHCI_EnumerationWorkItem(IN PVOID Context)
{
    PXHCI_ENUMERATION_WORK_ITEM WorkItem = (PXHCI_ENUMERATION_WORK_ITEM)Context;
    PXHCI_EXTENSION XhciExtension = WorkItem->XhciExtension;
    ULONG PortNumber = WorkItem->PortNumber;
    
    DPRINT1("XHCI_EnumerationWorkItem: Starting deferred enumeration for port %d\n", PortNumber);
    
    // Verify we're actually at PASSIVE_LEVEL (not in a DPC or higher IRQL)
    if (KeGetCurrentIrql() != PASSIVE_LEVEL)
    {
        DPRINT1("XHCI_EnumerationWorkItem: ERROR - Work item running at IRQL %d, expected PASSIVE_LEVEL\n", 
                KeGetCurrentIrql());
        ExFreePool(WorkItem);
        return;
    }
    
    // Add a minimal delay to spread out enumeration attempts
    LARGE_INTEGER Delay;
    ULONG DelayMs = 5; // Small consistent delay
    Delay.QuadPart = -(DelayMs * 10000LL); // Convert to 100ns units (negative = relative time)
    DPRINT1("XHCI_EnumerationWorkItem: Adding %dms delay for port %d\n", DelayMs, PortNumber);
    KeDelayExecutionThread(KernelMode, FALSE, &Delay);
    
    // Now we can safely call the enumeration routine outside of DPC context
    PXHCI_AssignSlot(XhciExtension, PortNumber);
    
    DPRINT1("XHCI_EnumerationWorkItem: Completed deferred enumeration for port %d\n", PortNumber);
    
    // Free the work item
    ExFreePool(WorkItem);
}

VOID
NTAPI
PXHCI_PortStatusChange(IN PXHCI_EXTENSION XhciExtension, IN ULONG PortID)
{
    BOOLEAN DeviceInsertedEvent = FALSE;
    XHCI_PORT_STATUS_CONTROL PortStatus;
    PULONG PortReg;
    static ULONG LastProcessedPort = 0;
    static LARGE_INTEGER LastProcessedTime = {{0, 0}};
    LARGE_INTEGER CurrentTime;
    
    // Prevent processing the same port multiple times in quick succession
    KeQuerySystemTime(&CurrentTime);
    if (LastProcessedPort == PortID && 
        (CurrentTime.QuadPart - LastProcessedTime.QuadPart) < 10000000) // 1 second
    {
        DPRINT1("PXHCI_PortStatusChange: Ignoring repeated port %d change within 1 second\n", PortID);
        return;
    }
    
    LastProcessedPort = PortID;
    LastProcessedTime = CurrentTime;
    
    PortReg = XhciExtension->OperationalRegs + (0x400 / sizeof(ULONG)) + ((PortID - 1) * 4);
    PortStatus.AsULONG = READ_REGISTER_ULONG(PortReg);
    
    DPRINT1("PXHCI_PortStatusChange: Port %d status = 0x%x\n", PortID, PortStatus.AsULONG);
    DPRINT1("PXHCI_PortStatusChange: CCS=%d, CSC=%d, PED=%d, PEC=%d\n", 
            PortStatus.CurrentConnectStatus, PortStatus.ConnectStatusChange, 
            PortStatus.PortEnableDisable, PortStatus.PortEnableDisableChange);
    
    // Check if this is a connect status change
    if (PortStatus.ConnectStatusChange)
    {
        // Determine if device was inserted or removed
        DeviceInsertedEvent = PortStatus.CurrentConnectStatus ? TRUE : FALSE;
        
        DPRINT1("PXHCI_PortStatusChange: Connect status change on port %d, device %s\n", 
                PortID, DeviceInsertedEvent ? "connected" : "disconnected");
    }
    else
    {
        DPRINT1("PXHCI_PortStatusChange: No connect status change, ignoring\n");
        return;
    }
    
    if(DeviceInsertedEvent == TRUE)
    {
        extern USBPORT_REGISTRATION_PACKET RegPacket;

        /* Attached: */
        DPRINT1("PXHCI_PortStatusChange: USB device has been inserted from port: %X\n", PortID);

        /* Let usbport drive enumeration: it will poll XHCI_RH_GetPortStatus,
         * reset the port, W1C-clear CSC via XHCI_RH_ClearFeaturePortConnectChange,
         * then call XHCI_OpenEndpoint/XHCI_SubmitTransfer for Enable Slot,
         * Address Device and the standard descriptor requests. Do NOT clear
         * PORTSC here or enumerate locally; that pre-empts usbport and leaves
         * the device invisible to PnP. */
        RegPacket.UsbPortInvalidateRootHub(XhciExtension);
        DPRINT1("PXHCI_PortStatusChange: invalidated root hub for port %u, usbport will enumerate\n", PortID);
    }
    else
    {
        /* Detached: 
         *    - CCS -> 0 
         *    - CSC -> 1 
         */
        DPRINT1("PXHCI_PortStatusChange: USB device has been removed from port: %X\n", PortID);
        /* Run de-escalation code */
        /*
         * -> Submit a disable slot command
         * -> clear transfer rings of all TDs associated with device post deattach
         */
    }
}

VOID
NTAPI
PXHCI_AssignSlot(IN PXHCI_EXTENSION XhciExtension, IN ULONG PortID)
{
    ULONG SlotID, DeviceAddress;
    MPSTATUS Status;

    DPRINT1("PXHCI_AssignSlot: Assigning slot for port %d (IRQL=%d)\n", PortID, KeGetCurrentIrql());

    // Use the comprehensive enumeration function
    Status = XHCI_EnumerateDevice(XhciExtension, PortID, &SlotID, &DeviceAddress);
    if (Status != MP_STATUS_SUCCESS)
    {
        DPRINT1("PXHCI_AssignSlot: Device enumeration failed for port %d\n", PortID);
        return;
    }

    DPRINT1("PXHCI_AssignSlot: Device enumeration successful for port %d\n", PortID);
    DPRINT1("PXHCI_AssignSlot: Assigned slot %d, device address %d\n", SlotID, DeviceAddress);
    
    // Process any remaining events to clear the queue
    XHCI_ProcessEvent(XhciExtension);
    
    // TODO: Store slot information for future reference
    // TODO: Notify USB stack about new device
}

VOID
NTAPI
PXHCI_InitSlot(IN PXHCI_EXTENSION xhciExtension, ULONG PortID, ULONG SlotID)
{
    /* 4.3.3 of the Intel xHCI spec */
    PXHCI_OUTPUT_DEVICE_CONTEXT HcOutputDeviceContext;
    PXHCI_TRANSFER_RING HcTransferControlRing;
    PXHCI_INPUT_CONTEXT HcInputContext;
    PXHCI_SLOT_CONTEXT HcSlotContext;
    PXHCI_ENDPOINT_CONTEXT HcDefaultEndpoint;

    PXHCI_EXTENSION XhciExtension;
    PXHCI_EVENT_TRB eventTRB;
    ULONG CheckCompletion;
    ULONG_PTR TrDeqPtr;
    XHCI_TRB Trb;
    PXHCI_HC_RESOURCES HcResourcesVA;
    XhciExtension = (PXHCI_EXTENSION)xhciExtension;
    HcResourcesVA = XhciExtension -> HcResourcesVA;
    eventTRB = (PXHCI_EVENT_TRB)HcResourcesVA->EventRing.dequeue_pointer;

    CheckCompletion = INVALID;

    HcOutputDeviceContext = ExAllocatePoolZero(NonPagedPool, sizeof(XHCI_OUTPUT_DEVICE_CONTEXT), 'TVED');
    HcTransferControlRing = ExAllocatePoolZero(NonPagedPool, sizeof(XHCI_TRANSFER_RING), 'TVED');
    HcDefaultEndpoint = ExAllocatePoolZero(NonPagedPool, sizeof(XHCI_ENDPOINT_CONTEXT), 'TVED');
    HcInputContext = ExAllocatePoolZero(NonPagedPool, sizeof(XHCI_INPUT_CONTEXT), 'TVED');
    HcSlotContext = ExAllocatePoolZero(NonPagedPool, sizeof(XHCI_SLOT_CONTEXT), 'TVED');

    RtlZeroMemory((PVOID)HcOutputDeviceContext, sizeof(XHCI_OUTPUT_DEVICE_CONTEXT));
    RtlZeroMemory((PVOID)HcTransferControlRing, sizeof(XHCI_TRANSFER_RING));
    RtlZeroMemory((PVOID)HcDefaultEndpoint, sizeof(XHCI_ENDPOINT_CONTEXT));
    RtlZeroMemory((PVOID)HcInputContext, sizeof(XHCI_INPUT_CONTEXT));
    RtlZeroMemory((PVOID)HcSlotContext, sizeof(XHCI_SLOT_CONTEXT));


    TrDeqPtr = (ULONG_PTR)HcTransferControlRing->firstSeg.XhciTrb;

    HcSlotContext->RouteString = 0;
    HcSlotContext->ParentPortNumber = PortID;
    HcSlotContext->ContextEntries = 1;
    HcSlotContext->ParentHubSlotID = SlotID;
    HcInputContext->InputContext.A0 = 1;
    HcInputContext->InputContext.A1 = 1;

    HcDefaultEndpoint->EPType = 4;
    HcDefaultEndpoint->MaxBurstSize = 0;
    HcDefaultEndpoint->TRDeqPtr = TrDeqPtr | 0x1;  // Include DCS bit = 1
    HcDefaultEndpoint->Interval = 0;
    HcDefaultEndpoint->MaxPStreams = 0;
    HcDefaultEndpoint->Mult = 0;
    HcDefaultEndpoint->CErr = 3;

    HcOutputDeviceContext->SlotContext = HcSlotContext;

    // Store the output device context physical address in the DCBAA for this slot
    HcResourcesVA->DCBAA.ContextBaseAddr[SlotID] = MmGetPhysicalAddress(HcOutputDeviceContext);
    DPRINT1("PXHCI_InitSlot: Stored device context for slot %d at PA 0x%I64x\n", 
            SlotID, HcResourcesVA->DCBAA.ContextBaseAddr[SlotID].QuadPart);

    // Get the physical address of the input context
    PHYSICAL_ADDRESS InputContextPA = MmGetPhysicalAddress(HcInputContext);
    
    Trb.CommandTRB.AddressDevice.InputContextPtrLow = (ULONG)(InputContextPA.QuadPart & 0xFFFFFFFF);
    Trb.CommandTRB.AddressDevice.InputContextPtrHigh = (ULONG)(InputContextPA.QuadPart >> 32);
    Trb.CommandTRB.AddressDevice.RsvdZ2 = 0;
    Trb.CommandTRB.AddressDevice.RsvdZ3 = 0;
    Trb.CommandTRB.AddressDevice.CycleBit = 0;
    Trb.CommandTRB.AddressDevice.RsvdZ4 = 0;
    Trb.CommandTRB.AddressDevice.TRBType = ADDRESS_DEVICE_COMMAND;

    XHCI_SendCommand(Trb,XhciExtension);

    while (!CheckCompletion)
    {
        SlotID = eventTRB->CommandCompletionTRB.SlotID;
        CheckCompletion = eventTRB->CommandCompletionTRB.CompletionCode;
        if(CheckCompletion == SUCCESS)
        {
            KeStallExecutionProcessor(10);
            break;
        }
    }
}


/* Transfer type functions ************************************************************************/

MPSTATUS
NTAPI
PXHCI_InitTransferBulk(PVOID xhciExtension)
{
    __debugbreak();
    return MP_STATUS_FAILURE;
}

MPSTATUS
NTAPI
PXHCI_InitTransferInterrupt(PVOID xhciExtension)
{
    return MP_STATUS_SUCCESS;
}

MPSTATUS
NTAPI
PXHCI_InitTransferIso(PVOID xhciExtension)
{
    DPRINT1("PXHCI_InitTransferIso: Isochronous transfers not yet implemented\n");
    return MP_STATUS_FAILURE;
}

MPSTATUS
NTAPI
PXHCI_InitTransferControl(PVOID xhciExtension)
{
    __debugbreak();
    return MP_STATUS_FAILURE;
} 

MPSTATUS
NTAPI
XHCI_OpenIsoEndpoint(IN PXHCI_EXTENSION XhciExtension,
                     IN PUSBPORT_ENDPOINT_PROPERTIES EndpointProperties,
                     IN PXHCI_ENDPOINT  XhciEndpoint)
{
    ULONG DeviceAddress;
    ULONG EndpointAddress;
    ULONG MaxPacketSize;
    ULONG DeviceSpeed;
    ULONG ContextIndex;
    ULONG Interval;
    
    DPRINT1("XHCI_OpenIsoEndpoint: function initiated\n");
    
    DeviceAddress = EndpointProperties->DeviceAddress;
    EndpointAddress = EndpointProperties->EndpointAddress;
    MaxPacketSize = EndpointProperties->MaxPacketSize;
    DeviceSpeed = EndpointProperties->DeviceSpeed;
    Interval = EndpointProperties->Period;
    
    DPRINT1("XHCI_OpenIsoEndpoint: DeviceAddress=%d, EndpointAddress=%d, MaxPacketSize=%d, DeviceSpeed=%d, Interval=%d\n",
            DeviceAddress, EndpointAddress, MaxPacketSize, DeviceSpeed, Interval);
    
    // Initialize endpoint structure
    InitializeListHead(&XhciEndpoint->ListTDs);
    XhciEndpoint->DmaBufferVA = (PVOID)EndpointProperties->BufferVA;
    XhciEndpoint->DmaBufferPA = EndpointProperties->BufferPA;
    XhciEndpoint->EndpointProperties = *EndpointProperties;
    XhciEndpoint->EndpointState = USBPORT_ENDPOINT_ACTIVE;
    XhciEndpoint->Interval = Interval;
    
    // Calculate context index: 2 * EP number + direction (0=OUT, 1=IN)
    ContextIndex = 2 * (EndpointAddress & 0x0F) + ((EndpointAddress & 0x80) ? 1 : 0);
    XhciEndpoint->ContextIndex = ContextIndex;
    
    // Initialize transfer ring for this endpoint with proper Link TRB
    XHCI_InitializeTransferRing(&XhciEndpoint->TransferRing);
    
    // Store the slot ID in the endpoint for future reference
    // For non-control endpoints, the slot ID is typically the device address
    *(PULONG)&XhciEndpoint->FirstTD = DeviceAddress;
    
    DPRINT1("XHCI_OpenIsoEndpoint: Stored slot ID %d for isochronous endpoint\n", DeviceAddress);
    DPRINT1("XHCI_OpenIsoEndpoint: Isochronous endpoint initialized successfully\n");
    return MP_STATUS_SUCCESS;
}

MPSTATUS
NTAPI
XHCI_OpenControlEndpoint(IN PXHCI_EXTENSION XhciExtension,
                         IN PUSBPORT_ENDPOINT_PROPERTIES EndpointProperties,
                         IN PXHCI_ENDPOINT  XhciEndpoint)
{
    ULONG DeviceAddress;
    ULONG EndpointAddress;
    ULONG MaxPacketSize;
    ULONG DeviceSpeed;
   /// PXHCI_HC_RESOURCES HcResourcesVA;
    ULONG SlotId;
    MPSTATUS Status;
    UNREFERENCED_PARAMETER(SlotId);
    UNREFERENCED_PARAMETER(Status);
    DPRINT1("XHCI_OpenControlEndpoint: function initiated\n");
    
    DeviceAddress = EndpointProperties->DeviceAddress;
    EndpointAddress = EndpointProperties->EndpointAddress;
    MaxPacketSize = EndpointProperties->MaxPacketSize;
    DeviceSpeed = EndpointProperties->DeviceSpeed;
    
    DPRINT1("XHCI_OpenControlEndpoint: DeviceAddress=%d, EndpointAddress=%d, MaxPacketSize=%d, DeviceSpeed=%d\n",
            DeviceAddress, EndpointAddress, MaxPacketSize, DeviceSpeed);
    
    // Validate parameters
    if (!XhciExtension || !EndpointProperties || !XhciEndpoint) {
        DPRINT1("XHCI_OpenControlEndpoint: Invalid parameters\n");
        return MP_STATUS_FAILURE;
    }
    
    if (EndpointAddress != 0) {
        DPRINT1("XHCI_OpenControlEndpoint: This function only handles endpoint 0, got endpoint %d\n", EndpointAddress);
        return MP_STATUS_FAILURE;
    }
    
    // Initialize endpoint structure
    InitializeListHead(&XhciEndpoint->ListTDs);
    XhciEndpoint->DmaBufferVA = (PVOID)EndpointProperties->BufferVA;
    XhciEndpoint->DmaBufferPA = EndpointProperties->BufferPA;
    XhciEndpoint->EndpointProperties = *EndpointProperties;
    XhciEndpoint->EndpointState = USBPORT_ENDPOINT_ACTIVE;
    XhciEndpoint->EndpointStatus = 0;  // Initialize status
    XhciEndpoint->PendingTDs = 0;
    XhciEndpoint->RemainTDs = 0;
    XhciEndpoint->MaxTDs = 16;  // Reasonable default for control endpoint
    //XhciEndpoint->EndpointType.Type = USBPORT_ENDPOINT_CONTROL;
    
    // Initialize transfer ring for this endpoint with proper Link TRB
    XHCI_InitializeTransferRing(&XhciEndpoint->TransferRing);
    
    // Calculate context index for EP0 (always 1 for control endpoints)
    XhciEndpoint->ContextIndex = 1;
    
    // For a new device (DeviceAddress == 0), we need to ensure slot is enabled first
    if (DeviceAddress == 0 && EndpointAddress == 0) {
        DPRINT1("XHCI_OpenControlEndpoint: This is a new device (address 0), need to enable slot first\n");
        
        // Get the actual port number from endpoint properties
        ULONG PortNumber = EndpointProperties->PortNumber;
        DPRINT1("XHCI_OpenControlEndpoint: Device is on port %d\n", PortNumber);
        
        // Step 1: Enable Slot first (this must be done before Address Device)
        Status = XHCI_EnableSlot(XhciExtension, PortNumber, &SlotId);
        if (Status != MP_STATUS_SUCCESS) {
            DPRINT1("XHCI_OpenControlEndpoint: Failed to enable slot\n");
            return MP_STATUS_FAILURE;
        }
        
        DPRINT1("XHCI_OpenControlEndpoint: Enable Slot successful, using slot ID %d\n", SlotId);
        
        // Step 2: Set up Input Context (device context and endpoint context for EP0)
        Status = XHCI_SetupDeviceContext(XhciExtension, SlotId, MaxPacketSize, DeviceSpeed, XhciEndpoint, PortNumber);
        if (Status != MP_STATUS_SUCCESS) {
            DPRINT1("XHCI_OpenControlEndpoint: Failed to setup device context\n");
            return MP_STATUS_FAILURE;
        }
        
        // Step 3: Setup DCBAA entry
        Status = XHCI_SetupDCBAAEntry(XhciExtension, SlotId);
        if (Status != MP_STATUS_SUCCESS) {
            DPRINT1("XHCI_OpenControlEndpoint: Failed to setup DCBAA entry\n");
            return MP_STATUS_FAILURE;
        }
        
        // Step 4: Issue Address Device command to configure the device
        PHYSICAL_ADDRESS InputContextPA = MmGetPhysicalAddress(&XhciExtension->HcResourcesVA->InputContext);
        Status = XHCI_AddressDevice(XhciExtension, SlotId, InputContextPA, FALSE);
        if (Status != MP_STATUS_SUCCESS) {
            DPRINT1("XHCI_OpenControlEndpoint: Failed to issue Address Device command\n");
            return MP_STATUS_FAILURE;
        }
        
        // Store the slot ID in the endpoint for future reference
        *(PULONG)&XhciEndpoint->FirstTD = SlotId;
        
        DPRINT1("XHCI_OpenControlEndpoint: Device context setup completed for address 0 device\n");
    } else {
        DPRINT1("XHCI_OpenControlEndpoint: Opening endpoint for existing device (addr=%d)\n", DeviceAddress);
        // For an existing device, we would need to look up the slot ID
        // For now, assume slot 1
        *(PULONG)&XhciEndpoint->FirstTD = 1;
    }
    
    DPRINT1("XHCI_OpenControlEndpoint: Control endpoint initialized successfully\n");
    DPRINT1("XHCI_OpenControlEndpoint: Returning MP_STATUS_SUCCESS (0x%x)\n", MP_STATUS_SUCCESS);
    return MP_STATUS_SUCCESS;
}

MPSTATUS
NTAPI
XHCI_OpenBulkEndpoint(IN PXHCI_EXTENSION XhciExtension,
                      IN PUSBPORT_ENDPOINT_PROPERTIES EndpointProperties,
                      IN PXHCI_ENDPOINT  XhciEndpoint)
{
    ULONG DeviceAddress;
    ULONG EndpointAddress;
    ULONG MaxPacketSize;
    ULONG DeviceSpeed;
    ULONG ContextIndex;
    
    DPRINT1("XHCI_OpenBulkEndpoint: function initiated\n");
    
    DeviceAddress = EndpointProperties->DeviceAddress;
    EndpointAddress = EndpointProperties->EndpointAddress;
    MaxPacketSize = EndpointProperties->MaxPacketSize;
    DeviceSpeed = EndpointProperties->DeviceSpeed;
    
    DPRINT1("XHCI_OpenBulkEndpoint: DeviceAddress=%d, EndpointAddress=%d, MaxPacketSize=%d, DeviceSpeed=%d\n",
            DeviceAddress, EndpointAddress, MaxPacketSize, DeviceSpeed);
    
    // Initialize endpoint structure
    InitializeListHead(&XhciEndpoint->ListTDs);
    XhciEndpoint->DmaBufferVA = (PVOID)EndpointProperties->BufferVA;
    XhciEndpoint->DmaBufferPA = EndpointProperties->BufferPA;
    XhciEndpoint->EndpointProperties = *EndpointProperties;
    XhciEndpoint->EndpointState = USBPORT_ENDPOINT_ACTIVE;
   // XhciEndpoint->EndpointType.Type = USBPORT_ENDPOINT_BULK;

    // Calculate context index: 2 * EP number + direction (0=OUT, 1=IN)
    ContextIndex = 2 * (EndpointAddress & 0x0F) + ((EndpointAddress & 0x80) ? 1 : 0);
    XhciEndpoint->ContextIndex = ContextIndex;
    
    // Initialize transfer ring for this endpoint with proper Link TRB
    XHCI_InitializeTransferRing(&XhciEndpoint->TransferRing);
    
    // Store the slot ID in the endpoint for future reference
    // For non-control endpoints, the slot ID is typically the device address
    *(PULONG)&XhciEndpoint->FirstTD = DeviceAddress;

    DPRINT1("XHCI_OpenBulkEndpoint: Stored slot ID %d for bulk endpoint\n", DeviceAddress);

    /*
     * Tell the controller about this endpoint: issue CONFIGURE_ENDPOINT so the
     * EP Context at DCI (ContextIndex) transitions from Disabled to Running.
     * Without this, doorbell rings on this DCI are silently dropped and no
     * TRANSFER_EVENT is ever posted for our Bulk CBW/CSW traffic.
     *
     * Slot ID convention matches XHCI_SubmitBulkTransfer: stored via
     * *(PULONG)&XhciEndpoint->FirstTD = DeviceAddress.
     *
     * EpType encoding per xHCI spec section 6.2.3 Table 6-9:
     *   Bulk OUT = 2, Bulk IN = 6.  IN direction == (EndpointAddress & 0x80).
     */
    {
        ULONG SlotId = DeviceAddress;
        ULONG EndpointNumber = EndpointAddress & 0x0F;
        BOOLEAN IsIn = (EndpointAddress & 0x80) != 0;
        ULONG DCI = (EndpointNumber * 2) + (IsIn ? 1 : 0);
        ULONG EpType = IsIn ? 6 : 2;
        PHYSICAL_ADDRESS RingPA;
        MPSTATUS CfgStatus;

        /* Sanity: the DCI we derive here must match what the submission
         * path already cached (computed above identically). */
        ASSERT(DCI == ContextIndex);

        RingPA = MmGetPhysicalAddress(&XhciEndpoint->TransferRing.firstSeg.XhciTrb[0]);

        DPRINT1("XHCI_OpenBulkEndpoint: Configuring EP on slot %d, DCI=%d, EpType=%d, RingPA=0x%I64x\n",
                SlotId, DCI, EpType, RingPA.QuadPart);

        CfgStatus = XHCI_ConfigureEndpoint(XhciExtension,
                                           SlotId,
                                           DCI,
                                           EpType,
                                           MaxPacketSize,
                                           RingPA);
        if (CfgStatus != MP_STATUS_SUCCESS)
        {
            DPRINT1("XHCI_OpenBulkEndpoint: XHCI_ConfigureEndpoint failed (0x%x) for slot %d DCI %d\n",
                    CfgStatus, SlotId, DCI);
            return CfgStatus;
        }
    }

    DPRINT1("XHCI_OpenBulkEndpoint: Bulk endpoint initialized successfully\n");
    return MP_STATUS_SUCCESS;
}

MPSTATUS
NTAPI
XHCI_OpenInterruptEndpoint(IN PXHCI_EXTENSION XhciExtension,
                           IN PUSBPORT_ENDPOINT_PROPERTIES EndpointProperties,
                           IN PXHCI_ENDPOINT  XhciEndpoint)
{
    ULONG DeviceAddress;
    ULONG EndpointAddress;
    ULONG MaxPacketSize;
    ULONG DeviceSpeed;
    ULONG ContextIndex;
    ULONG Interval;
    
    DPRINT1("XHCI_OpenInterruptEndpoint: function initiated\n");
    
    DeviceAddress = EndpointProperties->DeviceAddress;
    EndpointAddress = EndpointProperties->EndpointAddress;
    MaxPacketSize = EndpointProperties->MaxPacketSize;
    DeviceSpeed = EndpointProperties->DeviceSpeed;
    Interval = EndpointProperties->Period;
    
    DPRINT1("XHCI_OpenInterruptEndpoint: DeviceAddress=%d, EndpointAddress=%d, MaxPacketSize=%d, DeviceSpeed=%d, Interval=%d\n",
            DeviceAddress, EndpointAddress, MaxPacketSize, DeviceSpeed, Interval);
    
    // Initialize endpoint structure
    InitializeListHead(&XhciEndpoint->ListTDs);
    XhciEndpoint->DmaBufferVA = (PVOID)EndpointProperties->BufferVA;
    XhciEndpoint->DmaBufferPA = EndpointProperties->BufferPA;
    XhciEndpoint->EndpointProperties = *EndpointProperties;
    XhciEndpoint->EndpointState = USBPORT_ENDPOINT_ACTIVE;
  //  XhciEndpoint->EndpointType.Type = USBPORT_ENDPOINT_INTERRUPT;
    XhciEndpoint->Interval = Interval;
    
    // Calculate context index: 2 * EP number + direction (0=OUT, 1=IN)
    ContextIndex = 2 * (EndpointAddress & 0x0F) + ((EndpointAddress & 0x80) ? 1 : 0);
    XhciEndpoint->ContextIndex = ContextIndex;
    
    // Initialize transfer ring for this endpoint with proper Link TRB
    XHCI_InitializeTransferRing(&XhciEndpoint->TransferRing);
    
    // Store the slot ID in the endpoint for future reference
    // For non-control endpoints, the slot ID is typically the device address
    // (assuming a simple 1:1 mapping of device address to slot ID)
    *(PULONG)&XhciEndpoint->FirstTD = DeviceAddress;
    
    DPRINT1("XHCI_OpenInterruptEndpoint: Stored slot ID %d for interrupt endpoint\n", DeviceAddress);
    DPRINT1("XHCI_OpenInterruptEndpoint: Interrupt endpoint initialized successfully\n");
    return MP_STATUS_SUCCESS;
}

// Helper function to enqueue a TRB onto a transfer ring
MPSTATUS
NTAPI
XHCI_EnqueueTRBOnTransferRing(IN PXHCI_RING TransferRing,
                              IN PXHCI_TRB Trb,
                              OUT PPHYSICAL_ADDRESS TrbPhysicalAddress OPTIONAL)
{
    PXHCI_TRB enqueue_pointer;
    PXHCI_TRB dequeue_pointer;
    
    enqueue_pointer = TransferRing->enqueue_pointer;
    dequeue_pointer = TransferRing->dequeue_pointer;
    
    // Check if we're at the Link TRB (index 255) and need to wrap around
    LONG TrbIndex = enqueue_pointer - &(TransferRing->firstSeg.XhciTrb[0]);
    if (TrbIndex == 255)
    {
        // We're at the Link TRB - toggle cycle state and wrap to beginning
        TransferRing->ProducerCycleState = TransferRing->ProducerCycleState ? 0 : 1;
        enqueue_pointer = &(TransferRing->firstSeg.XhciTrb[0]);
        TransferRing->enqueue_pointer = enqueue_pointer;
        TrbIndex = 0; // Update index after wrapping
        DPRINT1("XHCI_EnqueueTRBOnTransferRing: Wrapped to beginning, new cycle state %d\n", TransferRing->ProducerCycleState);
    }
    
    // Calculate next position for ring full check
    PXHCI_TRB NextEnqueuePtr = enqueue_pointer + 1;
    if (NextEnqueuePtr >= &(TransferRing->firstSeg.XhciTrb[255]))
    {
        NextEnqueuePtr = &(TransferRing->firstSeg.XhciTrb[0]); // Would wrap to start
    }
    
    // Check if ring is full
    if (NextEnqueuePtr == dequeue_pointer) 
    {
        DPRINT1("XHCI_EnqueueTRBOnTransferRing: Transfer ring is full\n");
        return MP_STATUS_FAILURE;
    }
    
    // Debug: Log ring state for troubleshooting
    DPRINT1("XHCI_EnqueueTRBOnTransferRing: Ring state - enqueue=%p, dequeue=%p, producer_cycle=%d, consumer_cycle=%d\n",
            TransferRing->enqueue_pointer, TransferRing->dequeue_pointer, 
            TransferRing->ProducerCycleState, TransferRing->ConsumerCycleState);
    
    // Set the cycle bit to match the producer cycle state
    Trb->GenericTRB.Word3 = (Trb->GenericTRB.Word3 & ~1) | TransferRing->ProducerCycleState;
    
    DPRINT1("XHCI_EnqueueTRBOnTransferRing: Placing TRB at %p (index %d), cycle bit %d, TRB type %d\n", 
            enqueue_pointer, TrbIndex, TransferRing->ProducerCycleState, 
            (Trb->GenericTRB.Word3 >> 10) & 0x3F);
    
    DPRINT1("XHCI_EnqueueTRBOnTransferRing: TRB content - Word0=0x%08x, Word1=0x%08x, Word2=0x%08x, Word3=0x%08x\n",
            Trb->GenericTRB.Word0, Trb->GenericTRB.Word1, Trb->GenericTRB.Word2, Trb->GenericTRB.Word3);
    
    // Calculate physical address of where we're placing this TRB
    PHYSICAL_ADDRESS TrbPA = MmGetPhysicalAddress(enqueue_pointer);
    DPRINT1("XHCI_EnqueueTRBOnTransferRing: Placing TRB at PA 0x%I64x\n", TrbPA.QuadPart);
    
    // Return the physical address if requested
    if (TrbPhysicalAddress)
    {
        *TrbPhysicalAddress = TrbPA;
    }
    
    // Place TRB on the transfer ring
    *enqueue_pointer = *Trb;
    
    // Advance enqueue pointer
    enqueue_pointer = enqueue_pointer + 1;
    TransferRing->enqueue_pointer = enqueue_pointer;
    
    DPRINT1("XHCI_EnqueueTRBOnTransferRing: TRB enqueued successfully, new enqueue pointer at %p\n", enqueue_pointer);
    
    return MP_STATUS_SUCCESS;
}

// Helper function to initialize a transfer ring with proper Link TRB
MPSTATUS
NTAPI
XHCI_InitializeTransferRing(IN PXHCI_RING TransferRing)
{
    PXHCI_TRB LinkTrb;
    PHYSICAL_ADDRESS RingStartPA;
    
    if (!TransferRing) {
        return MP_STATUS_FAILURE;
    }
    
    // Initialize ring pointers
    TransferRing->enqueue_pointer = &TransferRing->firstSeg.XhciTrb[0];
    TransferRing->dequeue_pointer = &TransferRing->firstSeg.XhciTrb[0];
    TransferRing->ProducerCycleState = 1;
    TransferRing->ConsumerCycleState = 1;
    
    // Zero out all TRBs (indices 0-255)
    RtlZeroMemory(&TransferRing->firstSeg.XhciTrb[0], sizeof(XHCI_TRB) * 256);
    
    // Set up Link TRB at the last position (index 255)
    // This makes indices 0-254 usable for actual transfer TRBs
    LinkTrb = &TransferRing->firstSeg.XhciTrb[255];
    RingStartPA = MmGetPhysicalAddress(&TransferRing->firstSeg.XhciTrb[0]);
    
    // Configure Link TRB to point back to start of ring
    LinkTrb->GenericTRB.Word0 = (ULONG)(RingStartPA.QuadPart & 0xFFFFFFFF);
    LinkTrb->GenericTRB.Word1 = (ULONG)(RingStartPA.QuadPart >> 32);
    LinkTrb->GenericTRB.Word2 = 0;
    LinkTrb->GenericTRB.Word3 = (LINK_TRB << 10) | // TRB Type = Link TRB
                                (1 << 1) |          // Toggle Cycle bit
                                1;                  // Cycle bit (matches initial producer cycle state)
    
    DPRINT1("XHCI_InitializeTransferRing: Transfer ring initialized with Link TRB at index 255 pointing to PA 0x%I64x\n", RingStartPA.QuadPart);
    return MP_STATUS_SUCCESS;
}

MPSTATUS
NTAPI
XHCI_SubmitControlTransfer(IN PXHCI_EXTENSION XhciExtension,
                          IN PXHCI_ENDPOINT XhciEndpoint,
                          IN PXHCI_TRANSFER XhciTransfer,
                          IN PUSBPORT_SCATTER_GATHER_LIST SgList)
{
    PUSBPORT_TRANSFER_PARAMETERS TransferParameters;
    PUSB_DEFAULT_PIPE_SETUP_PACKET SetupPacket;
    XHCI_TRB SetupTrb, DataTrb, StatusTrb;
    PHYSICAL_ADDRESS BufferPA;
    ULONG TransferLength;
    ULONG Direction;
    ULONG SlotId;
    MPSTATUS Status;
    PHYSICAL_ADDRESS SetupTrbPA, DataTrbPA, StatusTrbPA;
    PXHCI_HC_RESOURCES HcResourcesVA;
    PXHCI_RING SlotTransferRing;
    
    DPRINT1("XHCI_SubmitControlTransfer: function initiated\n");
    
    TransferParameters = XhciTransfer->TransferParameters;
    SetupPacket = &TransferParameters->SetupPacket;
    TransferLength = TransferParameters->TransferBufferLength;
    Direction = TransferParameters->TransferFlags; // Direction flags - TODO: Use proper mask
    
    DPRINT1("XHCI_SubmitControlTransfer: SetupPacket->bmRequestType=0x%02x, bRequest=0x%02x, wValue=0x%04x\n",
            SetupPacket->bmRequestType.B, SetupPacket->bRequest, SetupPacket->wValue.W);
    
    // Get the slot ID for this endpoint
    SlotId = *(PULONG)&XhciEndpoint->FirstTD;  // Stored in FirstTD field during endpoint creation
    DPRINT1("XHCI_SubmitControlTransfer: Retrieved slot ID %d from endpoint\n", SlotId);
    
    // Get the correct transfer ring - use endpoint's ring if available, otherwise fall back to slot ring
    HcResourcesVA = XhciExtension->HcResourcesVA;
    if (XhciEndpoint->TransferRing.enqueue_pointer != NULL) {
        // Use the endpoint's transfer ring (matches what's configured in device context)
        SlotTransferRing = &XhciEndpoint->TransferRing;
        DPRINT1("XHCI_SubmitControlTransfer: Using endpoint transfer ring at %p\n", SlotTransferRing);
    } else {
        // Fall back to slot-specific ring for enumeration
        SlotTransferRing = &HcResourcesVA->SlotTransferRings[SlotId - 1];
        DPRINT1("XHCI_SubmitControlTransfer: Using slot transfer ring at %p (SlotTransferRings[%d])\n", 
                SlotTransferRing, SlotId - 1);
    }
    
    // Verify the transfer ring PA matches what's in the device context
    PHYSICAL_ADDRESS ExpectedRingPA = MmGetPhysicalAddress(&SlotTransferRing->firstSeg.XhciTrb[0]);
    DPRINT1("XHCI_SubmitControlTransfer: Transfer ring PA = 0x%I64x\n", ExpectedRingPA.QuadPart);
    
    // Also log the DCBAA entry for this slot
    if (SlotId > 0 && SlotId <= MAX_DEVICE_SLOTS) {
        PHYSICAL_ADDRESS DeviceContextPA = HcResourcesVA->DCBAA.ContextBaseAddr[SlotId];
        DPRINT1("XHCI_SubmitControlTransfer: DCBAA[%d] points to device context at PA 0x%I64x\n", 
                SlotId, DeviceContextPA.QuadPart);
    }
    
    // Get buffer physical address if data transfer is needed
    if (TransferLength > 0 && SgList && SgList->SgElementCount > 0)
    {
        // Get physical address from scatter-gather list
        BufferPA = SgList->SgElement[0].SgPhysicalAddress;
        DPRINT1("XHCI_SubmitControlTransfer: Using buffer PA from SG list: 0x%I64x (SgElementCount=%d)\n", 
                BufferPA.QuadPart, SgList->SgElementCount);
    }
    else
    {
        BufferPA.QuadPart = 0; // No data transfer needed
        DPRINT1("XHCI_SubmitControlTransfer: No data transfer or SG list - TransferLength=%d, SgList=%p\n", 
                TransferLength, SgList);
        if (SgList)
        {
            DPRINT1("XHCI_SubmitControlTransfer: SgList->SgElementCount=%d\n", SgList->SgElementCount);
        }
    }
    
    if (!SetupPacket)
    {
        DPRINT1("XHCI_SubmitControlTransfer: No setup packet provided\n");
        return MP_STATUS_FAILURE;
    }
    
    // Create Setup TRB
    RtlZeroMemory(&SetupTrb, sizeof(XHCI_TRB));
    SetupTrb.GenericTRB.Word0 = SetupPacket->bmRequestType.B | 
                               (SetupPacket->bRequest << 8) |
                               (SetupPacket->wValue.W << 16);
    SetupTrb.GenericTRB.Word1 = SetupPacket->wIndex.W | (SetupPacket->wLength << 16);
    SetupTrb.GenericTRB.Word2 = 8; // Setup packet is always 8 bytes
    SetupTrb.GenericTRB.Word3 = (SETUP_STAGE << 10) | // TRB Type = Setup Stage
                               (1 << 6) | // IDT (Immediate Data)
                               1; // Cycle bit (will be overridden by enqueue function)
    
    // Enqueue Setup TRB onto the transfer ring
    // CRITICAL: Use the slot-specific transfer ring that matches the device context
    Status = XHCI_EnqueueTRBOnTransferRing(SlotTransferRing, &SetupTrb, &SetupTrbPA);
    if (Status != MP_STATUS_SUCCESS)
    {
        DPRINT1("XHCI_SubmitControlTransfer: Failed to enqueue Setup TRB\n");
        return MP_STATUS_FAILURE;
    }
    
    // Create and enqueue Data TRB if there's data to transfer
    if (TransferLength > 0)
    {
        RtlZeroMemory(&DataTrb, sizeof(XHCI_TRB));
        DataTrb.GenericTRB.Word0 = (ULONG)(BufferPA.QuadPart & 0xFFFFFFFF);
        DataTrb.GenericTRB.Word1 = (ULONG)(BufferPA.QuadPart >> 32);
        DataTrb.GenericTRB.Word2 = TransferLength;
        DataTrb.GenericTRB.Word3 = (DATA_STAGE << 10) | // TRB Type = Data Stage
                                  (Direction ? (1 << 16) : 0) | // DIR bit (1=IN, 0=OUT)
                                  1; // Cycle bit (will be overridden by enqueue function)
        
        Status = XHCI_EnqueueTRBOnTransferRing(SlotTransferRing, &DataTrb, &DataTrbPA);
        if (Status != MP_STATUS_SUCCESS)
        {
            DPRINT1("XHCI_SubmitControlTransfer: Failed to enqueue Data TRB\n");
            return MP_STATUS_FAILURE;
        }
    }
    
    // Create Status TRB (with IOC bit for completion interrupt)
    RtlZeroMemory(&StatusTrb, sizeof(XHCI_TRB));
    StatusTrb.GenericTRB.Word0 = 0;
    StatusTrb.GenericTRB.Word1 = 0;
    StatusTrb.GenericTRB.Word2 = 0;
    StatusTrb.GenericTRB.Word3 = (STATUS_STAGE << 10) | // TRB Type = Status Stage
                                (TransferLength > 0 ? (Direction ? 0 : (1 << 16)) : (1 << 16)) | // DIR opposite of data or IN for no-data
                                (1 << 5) | // IOC (Interrupt on Completion)
                                1; // Cycle bit (will be overridden by enqueue function)
    
    Status = XHCI_EnqueueTRBOnTransferRing(SlotTransferRing, &StatusTrb, &StatusTrbPA);
    if (Status != MP_STATUS_SUCCESS)
    {
        DPRINT1("XHCI_SubmitControlTransfer: Failed to enqueue Status TRB\n");
        return MP_STATUS_FAILURE;
    }
    
    // Register the transfer for completion tracking BEFORE ringing the doorbell
    // Pass all TRB physical addresses for proper tracking
    PHYSICAL_ADDRESS TrbPointers[3];
    ULONG TrbCount = 0;
    
    TrbPointers[TrbCount++] = SetupTrbPA;
    if (TransferLength > 0)
    {
        TrbPointers[TrbCount++] = DataTrbPA;
    }
    TrbPointers[TrbCount++] = StatusTrbPA;
    
    Status = RegisterPendingTransfer(XhciExtension, XhciEndpoint, XhciTransfer, TrbPointers, TrbCount, TransferLength);
    if (Status != MP_STATUS_SUCCESS)
    {
        DPRINT1("XHCI_SubmitControlTransfer: Failed to register transfer for tracking\n");
        // Don't fail the transfer submission just because tracking failed
    }
    else
    {
        DPRINT1("XHCI_SubmitControlTransfer: Transfer registered in tracking system with %d TRBs\n", TrbCount);
    }
    
    // Ring doorbell to notify the controller (after registration to avoid race condition)
    DPRINT1("XHCI_SubmitControlTransfer: About to ring doorbell for slot %d, endpoint 1\n", SlotId);
    Status = XHCI_RingDoorbell(XhciExtension, SlotId, 1); // EP0 is always DCI 1
    if (Status != MP_STATUS_SUCCESS)
    {
        DPRINT1("XHCI_SubmitControlTransfer: Failed to ring doorbell\n");
        return MP_STATUS_FAILURE;
    }
    
    DPRINT1("XHCI_SubmitControlTransfer: Control transfer submitted successfully\n");
    
    // Process events after submission to catch any completions
    XHCI_ProcessEvent(XhciExtension);
    
    return MP_STATUS_SUCCESS;
}

MPSTATUS
NTAPI
XHCI_RingDoorbell(IN PXHCI_EXTENSION XhciExtension,
                  IN ULONG SlotId,
                  IN ULONG EndpointIndex)
{
    PULONG DoorBellRegisterBase;
    XHCI_DOORBELL DoorbellValue;
    
    DPRINT1("XHCI_RingDoorbell: SlotId=%d, EndpointIndex=%d\n", SlotId, EndpointIndex);
    
    if (!XhciExtension || !XhciExtension->DoorBellRegisterBase)
    {
        DPRINT1("XHCI_RingDoorbell: Invalid XhciExtension or doorbell register base\n");
        return MP_STATUS_FAILURE;
    }
    
    if (SlotId == 0 || SlotId > 255) // Basic validation
    {
        DPRINT1("XHCI_RingDoorbell: Invalid SlotId %d\n", SlotId);
        return MP_STATUS_FAILURE;
    }
    
    // Calculate doorbell register address for this slot
    DoorBellRegisterBase = XhciExtension->DoorBellRegisterBase + SlotId;
    
    // Set up doorbell value
    DoorbellValue.AsULONG = 0;
    DoorbellValue.DoorBellTarget = EndpointIndex;
    DoorbellValue.RsvdZ = 0;
    
    // Ring the doorbell
    WRITE_REGISTER_ULONG(DoorBellRegisterBase, DoorbellValue.AsULONG);
    
    DPRINT1("XHCI_RingDoorbell: Doorbell rung successfully\n");
    return MP_STATUS_SUCCESS;
}

MPSTATUS
NTAPI
XHCI_SubmitBulkTransfer(IN PXHCI_EXTENSION XhciExtension,
                       IN PXHCI_ENDPOINT XhciEndpoint,
                       IN PXHCI_TRANSFER XhciTransfer,
                       IN PUSBPORT_SCATTER_GATHER_LIST SgList)
{
    PUSBPORT_TRANSFER_PARAMETERS TransferParameters;
    XHCI_TRB NormalTrb;
    PHYSICAL_ADDRESS BufferPA;
    ULONG TransferLength;
    ULONG SlotId;
    ULONG ContextIndex;
    MPSTATUS Status;
    PHYSICAL_ADDRESS NormalTrbPA;
    PXHCI_RING EndpointTransferRing;

    DPRINT1("XHCI_SubmitBulkTransfer: function initiated\n");

    TransferParameters = XhciTransfer->TransferParameters;
    TransferLength = TransferParameters->TransferBufferLength;

    // Get the slot ID for this endpoint
    SlotId = *(PULONG)&XhciEndpoint->FirstTD;  // Stored in FirstTD field during endpoint creation
    DPRINT1("XHCI_SubmitBulkTransfer: Retrieved slot ID %d from endpoint\n", SlotId);

    // Calculate the context index (DCI) for this endpoint
    ContextIndex = XhciEndpoint->ContextIndex;
    DPRINT1("XHCI_SubmitBulkTransfer: Using context index %d for endpoint (length=%d)\n",
            ContextIndex, TransferLength);

    // Use the endpoint's transfer ring
    EndpointTransferRing = &XhciEndpoint->TransferRing;
    if (EndpointTransferRing->enqueue_pointer == NULL) {
        DPRINT1("XHCI_SubmitBulkTransfer: Endpoint transfer ring not initialized\n");
        return MP_STATUS_FAILURE;
    }

    DPRINT1("XHCI_SubmitBulkTransfer: Using endpoint transfer ring at %p\n", EndpointTransferRing);

    // Resolve buffer physical address.
    // Bulk endpoints legitimately carry zero-length transfers (BOT status
    // phase, ZLPs) -- in that case we still build a Normal TRB with
    // Word0/Word1/Word2 all zero and IOC=1. Only reject when TransferLength
    // is non-zero but the SgList is missing/empty.
    BufferPA.QuadPart = 0;
    if (TransferLength > 0)
    {
        if (SgList == NULL || SgList->SgElementCount == 0)
        {
            DPRINT1("XHCI_SubmitBulkTransfer: missing SgList for length=%d\n",
                    TransferLength);
            return MP_STATUS_FAILURE;
        }
        BufferPA = SgList->SgElement[0].SgPhysicalAddress;
        DPRINT1("XHCI_SubmitBulkTransfer: Using buffer PA from SG list: 0x%I64x (length=%d)\n",
                BufferPA.QuadPart, TransferLength);
    }
    else
    {
        DPRINT1("XHCI_SubmitBulkTransfer: Zero-length bulk transfer (ZLP / status phase)\n");
    }

    // Build a single Normal TRB for this bulk transfer.
    // NOTE: USB Mass Storage BOT CBW/CSW are 31/13 bytes and typical SCSI
    // data phases fit in a single TRB, so one Normal TRB per submission is
    // sufficient for current usbstor needs. Multi-TRB chaining for transfers
    // exceeding a single TRB's max length is not implemented here.
    RtlZeroMemory(&NormalTrb, sizeof(XHCI_TRB));
    NormalTrb.GenericTRB.Word0 = (ULONG)(BufferPA.QuadPart & 0xFFFFFFFF);
    NormalTrb.GenericTRB.Word1 = (ULONG)(BufferPA.QuadPart >> 32);
    NormalTrb.GenericTRB.Word2 = TransferLength;
    NormalTrb.GenericTRB.Word3 = (NORMAL_TRB << 10) | // TRB Type = Normal
                                (1 << 5) | // IOC (Interrupt on Completion)
                                1; // Cycle bit (will be overridden by enqueue function)

    DPRINT1("XHCI_SubmitBulkTransfer: Created Normal TRB with length %d, IOC=1\n", TransferLength);

    // Enqueue Normal TRB onto the endpoint's transfer ring
    Status = XHCI_EnqueueTRBOnTransferRing(EndpointTransferRing, &NormalTrb, &NormalTrbPA);
    if (Status != MP_STATUS_SUCCESS)
    {
        DPRINT1("XHCI_SubmitBulkTransfer: Failed to enqueue Normal TRB\n");
        return MP_STATUS_FAILURE;
    }

    // Register the transfer for completion tracking
    PHYSICAL_ADDRESS TrbPointers[1];
    TrbPointers[0] = NormalTrbPA;

    Status = RegisterPendingTransfer(XhciExtension, XhciEndpoint, XhciTransfer, TrbPointers, 1, TransferLength);
    if (Status != MP_STATUS_SUCCESS)
    {
        DPRINT1("XHCI_SubmitBulkTransfer: Failed to register transfer for tracking\n");
        // Don't fail the transfer submission just because tracking failed
    }
    else
    {
        DPRINT1("XHCI_SubmitBulkTransfer: Transfer registered in tracking system\n");
    }

    // Ring doorbell to notify the controller (using the endpoint's context index)
    DPRINT1("XHCI_SubmitBulkTransfer: About to ring doorbell for slot %d, endpoint %d\n", SlotId, ContextIndex);
    Status = XHCI_RingDoorbell(XhciExtension, SlotId, ContextIndex);
    if (Status != MP_STATUS_SUCCESS)
    {
        DPRINT1("XHCI_SubmitBulkTransfer: Failed to ring doorbell\n");
        return MP_STATUS_FAILURE;
    }

    DPRINT1("XHCI_SubmitBulkTransfer: Bulk transfer submitted successfully\n");

    // Process events after submission to catch any completions
    XHCI_ProcessEvent(XhciExtension);

    return MP_STATUS_SUCCESS;
}

MPSTATUS
NTAPI
XHCI_SubmitInterruptTransfer(IN PXHCI_EXTENSION XhciExtension,
                            IN PXHCI_ENDPOINT XhciEndpoint,
                            IN PXHCI_TRANSFER XhciTransfer,
                            IN PUSBPORT_SCATTER_GATHER_LIST SgList)
{
    PUSBPORT_TRANSFER_PARAMETERS TransferParameters;
    XHCI_TRB NormalTrb;
    PHYSICAL_ADDRESS BufferPA;
    ULONG TransferLength;
    ULONG SlotId;
    ULONG ContextIndex;
    MPSTATUS Status;
    PHYSICAL_ADDRESS NormalTrbPA;
    PXHCI_RING EndpointTransferRing;
    
    DPRINT1("XHCI_SubmitInterruptTransfer: function initiated\n");
    
    TransferParameters = XhciTransfer->TransferParameters;
    TransferLength = TransferParameters->TransferBufferLength;
    
    // Get the slot ID for this endpoint
    SlotId = *(PULONG)&XhciEndpoint->FirstTD;  // Stored in FirstTD field during endpoint creation
    DPRINT1("XHCI_SubmitInterruptTransfer: Retrieved slot ID %d from endpoint\n", SlotId);
    
    // Calculate the context index (DCI) for this endpoint
    ContextIndex = XhciEndpoint->ContextIndex;
    DPRINT1("XHCI_SubmitInterruptTransfer: Using context index %d for endpoint\n", ContextIndex);
    
    // Use the endpoint's transfer ring
    EndpointTransferRing = &XhciEndpoint->TransferRing;
    if (EndpointTransferRing->enqueue_pointer == NULL) {
        DPRINT1("XHCI_SubmitInterruptTransfer: Endpoint transfer ring not initialized\n");
        return MP_STATUS_FAILURE;
    }
    
    DPRINT1("XHCI_SubmitInterruptTransfer: Using endpoint transfer ring at %p\n", EndpointTransferRing);
    
    // Get buffer physical address if data transfer is needed
    if (TransferLength > 0 && SgList && SgList->SgElementCount > 0)
    {
        // Get physical address from scatter-gather list
        BufferPA = SgList->SgElement[0].SgPhysicalAddress;
        DPRINT1("XHCI_SubmitInterruptTransfer: Using buffer PA from SG list: 0x%I64x (length=%d)\n", 
                BufferPA.QuadPart, TransferLength);
    }
    else
    {
        DPRINT1("XHCI_SubmitInterruptTransfer: No data transfer - TransferLength=%d, SgList=%p\n", 
                TransferLength, SgList);
        return MP_STATUS_FAILURE; // Interrupt transfers should always have data
    }
    
    // Create Normal TRB for interrupt transfer
    RtlZeroMemory(&NormalTrb, sizeof(XHCI_TRB));
    NormalTrb.GenericTRB.Word0 = (ULONG)(BufferPA.QuadPart & 0xFFFFFFFF);
    NormalTrb.GenericTRB.Word1 = (ULONG)(BufferPA.QuadPart >> 32);
    NormalTrb.GenericTRB.Word2 = TransferLength;
    NormalTrb.GenericTRB.Word3 = (NORMAL_TRB << 10) | // TRB Type = Normal
                                (1 << 5) | // IOC (Interrupt on Completion)
                                1; // Cycle bit (will be overridden by enqueue function)
    
    DPRINT1("XHCI_SubmitInterruptTransfer: Created Normal TRB with length %d, IOC=1\n", TransferLength);
    
    // Enqueue Normal TRB onto the endpoint's transfer ring
    Status = XHCI_EnqueueTRBOnTransferRing(EndpointTransferRing, &NormalTrb, &NormalTrbPA);
    if (Status != MP_STATUS_SUCCESS)
    {
        DPRINT1("XHCI_SubmitInterruptTransfer: Failed to enqueue Normal TRB\n");
        return MP_STATUS_FAILURE;
    }
    
    // Register the transfer for completion tracking
    PHYSICAL_ADDRESS TrbPointers[1];
    TrbPointers[0] = NormalTrbPA;
    
    Status = RegisterPendingTransfer(XhciExtension, XhciEndpoint, XhciTransfer, TrbPointers, 1, TransferLength);
    if (Status != MP_STATUS_SUCCESS)
    {
        DPRINT1("XHCI_SubmitInterruptTransfer: Failed to register transfer for tracking\n");
        // Don't fail the transfer submission just because tracking failed
    }
    else
    {
        DPRINT1("XHCI_SubmitInterruptTransfer: Transfer registered in tracking system\n");
    }
    
    // Ring doorbell to notify the controller (using the endpoint's context index)
    DPRINT1("XHCI_SubmitInterruptTransfer: About to ring doorbell for slot %d, endpoint %d\n", SlotId, ContextIndex);
    Status = XHCI_RingDoorbell(XhciExtension, SlotId, ContextIndex);
    if (Status != MP_STATUS_SUCCESS)
    {
        DPRINT1("XHCI_SubmitInterruptTransfer: Failed to ring doorbell\n");
        return MP_STATUS_FAILURE;
    }
    
    DPRINT1("XHCI_SubmitInterruptTransfer: Interrupt transfer submitted successfully\n");
    
    // Process events after submission to catch any completions
    XHCI_ProcessEvent(XhciExtension);
    
    return MP_STATUS_SUCCESS;
}

MPSTATUS
NTAPI
XHCI_SubmitIsochronousTransfer(IN PXHCI_EXTENSION XhciExtension,
                              IN PXHCI_ENDPOINT XhciEndpoint,
                              IN PXHCI_TRANSFER XhciTransfer,
                              IN PUSBPORT_SCATTER_GATHER_LIST SgList)
{
    DPRINT1("XHCI_SubmitIsochronousTransfer: Isochronous transfers not yet implemented\n");
    return MP_STATUS_FAILURE;
}

// Event processing functions
VOID
NTAPI
XHCI_ProcessTransferEvent(IN PXHCI_EXTENSION XhciExtension,
                         IN PXHCI_EVENT_TRB EventTrb)
{
    PHYSICAL_ADDRESS TrbPointer;
    PXHCI_PENDING_TRANSFER PendingTransfer;
    ULONG CompletionCode, TransferLength, SlotId, EndpointId;
    
    DPRINT1("XHCI_ProcessTransferEvent: Processing transfer completion event\n");
    
    // Extract event data
    TrbPointer.LowPart = EventTrb->TransferEventTRB.TRBPtrLo;
    TrbPointer.HighPart = EventTrb->TransferEventTRB.TRBPtrHi;
    CompletionCode = EventTrb->TransferEventTRB.CompleteionCode;
    TransferLength = EventTrb->TransferEventTRB.TrbTransferLen;
    SlotId = EventTrb->TransferEventTRB.SlotID;
    EndpointId = EventTrb->TransferEventTRB.EndpointID;
    
    DPRINT1("XHCI_ProcessTransferEvent: TRB=0x%I64x, Slot=%d, EP=%d, Code=%d, Length=%d\n",
            TrbPointer.QuadPart, SlotId, EndpointId, CompletionCode, TransferLength);
    
    // Check for slot ID mismatches - this may indicate a problem with device context setup
    if (SlotId == 0) {
        DPRINT1("XHCI_ProcessTransferEvent: WARNING - Transfer event reports slot ID 0, this may indicate a controller issue\n");
    }
    
    // Find the pending transfer
    PendingTransfer = FindPendingTransfer(TrbPointer);
    if (PendingTransfer)
    {
        // Complete the transfer
        DPRINT1("XHCI_ProcessTransferEvent: Found pending transfer, completing\n");
        
        // Update transfer status based on completion code
        switch (CompletionCode)
        {
            case SUCCESS:
                PendingTransfer->XhciTransfer->USBDStatus = USBD_STATUS_SUCCESS;
                break;
            case SHORT_PACKET:
                PendingTransfer->XhciTransfer->USBDStatus = USBD_STATUS_SUCCESS; // Short packet is usually OK
                break;
            case STALL_ERROR:
                PendingTransfer->XhciTransfer->USBDStatus = USBD_STATUS_STALL_PID;
                break;
            case TRB_ERROR:
                PendingTransfer->XhciTransfer->USBDStatus = USBD_STATUS_XACT_ERROR;
                DPRINT1("XHCI_ProcessTransferEvent: TRB_ERROR detected (code=5)\n");
                break;
            case DATA_BUFFER_ERROR:
                PendingTransfer->XhciTransfer->USBDStatus = USBD_STATUS_DATA_BUFFER_ERROR;
                break;
            default:
                PendingTransfer->XhciTransfer->USBDStatus = USBD_STATUS_XACT_ERROR;
                DPRINT1("XHCI_ProcessTransferEvent: Unknown completion code %d\n", CompletionCode);
                break;
        }
        
        // Calculate actual transferred length based on completion code and transfer type
        ULONG ActualTransferred;
        
        if (CompletionCode == SUCCESS)
        {
            // For successful transfers, actual transferred = requested
            ActualTransferred = PendingTransfer->RequestedLength;
            DPRINT1("XHCI_ProcessTransferEvent: SUCCESS - using requested length %d\n", ActualTransferred);
        }
        else if (CompletionCode == SHORT_PACKET)
        {
            // For short packet transfers, calculate from event data
            ActualTransferred = PendingTransfer->RequestedLength - TransferLength;
            DPRINT1("XHCI_ProcessTransferEvent: SHORT_PACKET - calculated %d (req=%d, not_xfer=%d)\n", 
                    ActualTransferred, PendingTransfer->RequestedLength, TransferLength);
        }
        else
        {
            // For error conditions, no data was transferred
            ActualTransferred = 0;
            DPRINT1("XHCI_ProcessTransferEvent: ERROR (code=%d) - setting transferred to 0\n", CompletionCode);
        }
        
        // Ensure we don't report negative transfer lengths
        if ((LONG)ActualTransferred < 0)
        {
            DPRINT1("XHCI_ProcessTransferEvent: WARNING - ActualTransferred was negative (%d), setting to 0\n", 
                    (LONG)ActualTransferred);
            ActualTransferred = 0;
        }
        
        PendingTransfer->XhciTransfer->TransferLen = ActualTransferred;
        
        DPRINT1("XHCI_ProcessTransferEvent: Final - Requested=%d, NotTransferred=%d, ActualTransferred=%d, Status=0x%x\n",
                PendingTransfer->RequestedLength, TransferLength, ActualTransferred, 
                PendingTransfer->XhciTransfer->USBDStatus);
        
        DPRINT1("XHCI_ProcessTransferEvent: About to notify USBPORT with length %d\n", ActualTransferred);
        
        // Mark transfer as completed and notify USB port driver
        XHCI_CompleteTransfer(XhciExtension, SlotId, EndpointId, TrbPointer, ActualTransferred, 
                             PendingTransfer->XhciTransfer->USBDStatus);
        
        // Remove from tracking after completion
        UnregisterPendingTransfer(PendingTransfer);
    }
    else
    {
        DPRINT1("XHCI_ProcessTransferEvent: No pending transfer found for TRB 0x%I64x\n", TrbPointer.QuadPart);
    }
}

VOID
NTAPI
XHCI_ProcessCommandCompletion(IN PXHCI_EXTENSION XhciExtension,
                             IN PXHCI_EVENT_TRB EventTrb)
{
    PHYSICAL_ADDRESS TrbPointer;
    ULONG CompletionCode, SlotId;
    
    DPRINT1("XHCI_ProcessCommandCompletion: Processing command completion event\n");
    
    // Extract event data
    TrbPointer.LowPart = EventTrb->CommandCompletionTRB.CommandTRBPointerLo << 4; // Shift back to get full address
    TrbPointer.HighPart = EventTrb->CommandCompletionTRB.CommandTRBPointerHi;
    CompletionCode = EventTrb->CommandCompletionTRB.CompletionCode;
    SlotId = EventTrb->CommandCompletionTRB.SlotID;
    
    DPRINT1("XHCI_ProcessCommandCompletion: TRB=0x%I64x, Slot=%d, Code=%d\n",
            TrbPointer.QuadPart, SlotId, CompletionCode);
    
    // Complete the pending command
    CompletePendingCommand(TrbPointer, CompletionCode, SlotId);
}

VOID
NTAPI
XHCI_CompleteTransfer(IN PXHCI_EXTENSION XhciExtension,
                     IN ULONG SlotId,
                     IN ULONG EndpointId,
                     IN PHYSICAL_ADDRESS TrbPointer,
                     IN ULONG TransferLength,
                     IN ULONG USBDStatus)
{
    extern USBPORT_REGISTRATION_PACKET RegPacket;
    
    DPRINT1("XHCI_CompleteTransfer: Slot=%d, EP=%d, TRB=0x%I64x, Length=%d, Status=0x%x\n",
            SlotId, EndpointId, TrbPointer.QuadPart, TransferLength, USBDStatus);
    
    // Find the pending transfer to get the required parameters
    PXHCI_PENDING_TRANSFER PendingTransfer = FindPendingTransfer(TrbPointer);
    if (PendingTransfer && PendingTransfer->XhciTransfer && PendingTransfer->XhciEndpoint)
    {
        DPRINT1("XHCI_CompleteTransfer: Notifying USB port driver about transfer completion\n");
        
        // CRITICAL FIX: Update transfer ring dequeue pointer
        // This is essential to prevent the ring from appearing "full" to the controller
        PXHCI_ENDPOINT XhciEndpoint = (PXHCI_ENDPOINT)PendingTransfer->XhciEndpoint;
        if (XhciEndpoint)
        {
            PXHCI_RING TransferRing = &XhciEndpoint->TransferRing;
            
            // Since MmGetVirtualForPhysical is unimplemented in ReactOS, we'll advance the 
            // dequeue pointer based on the number of TRBs that were part of this transfer
            ULONG TrbsToAdvance = PendingTransfer->TrbCount;
            
            DPRINT1("XHCI_CompleteTransfer: Advancing dequeue pointer by %d TRBs (current dequeue=%p)\n",
                    TrbsToAdvance, TransferRing->dequeue_pointer);
            
            // Advance dequeue pointer by the number of TRBs in this transfer
            for (ULONG i = 0; i < TrbsToAdvance; i++)
            {
                PXHCI_TRB CurrentDequeue = TransferRing->dequeue_pointer;
                PXHCI_TRB NewDequeue = CurrentDequeue + 1;
                
                // Handle ring wrap-around if we reach the Link TRB
                if (NewDequeue >= &(TransferRing->firstSeg.XhciTrb[255]))
                {
                    // Skip over Link TRB and wrap to beginning
                    NewDequeue = &(TransferRing->firstSeg.XhciTrb[0]);
                    // Update consumer cycle state when wrapping
                    TransferRing->ConsumerCycleState = TransferRing->ConsumerCycleState ? 0 : 1;
                    DPRINT1("XHCI_CompleteTransfer: Transfer ring wrapped at TRB %d, new consumer cycle state %d\n", 
                            i, TransferRing->ConsumerCycleState);
                }
                
                TransferRing->dequeue_pointer = NewDequeue;
                
                DPRINT1("XHCI_CompleteTransfer: Advanced dequeue pointer %d/%d from %p to %p\n",
                        i + 1, TrbsToAdvance, CurrentDequeue, NewDequeue);
            }
            
            // Log final ring state after update for debugging
            DPRINT1("XHCI_CompleteTransfer: Final ring state - enqueue=%p, dequeue=%p, producer_cycle=%d, consumer_cycle=%d\n",
                    TransferRing->enqueue_pointer, TransferRing->dequeue_pointer, 
                    TransferRing->ProducerCycleState, TransferRing->ConsumerCycleState);
        }
        
        // Update endpoint frame number after successful transfer to prevent re-queuing
        if (USBD_SUCCESS(USBDStatus))
        {
            // Get current frame number from XHCI controller
            ULONG CurrentFrameNumber = XHCI_GetCurrentFrameNumber(XhciExtension);
            
            // The XhciEndpoint should be a USBPORT_ENDPOINT structure
            PUSBPORT_ENDPOINT UsbPortEndpoint = (PUSBPORT_ENDPOINT)PendingTransfer->XhciEndpoint;
            
            DPRINT1("XHCI_CompleteTransfer: Updating endpoint frame number from %d to %d\n", 
                    UsbPortEndpoint->FrameNumber, CurrentFrameNumber);
                    
            // Update the endpoint's frame number to signal that it has advanced
            UsbPortEndpoint->FrameNumber = CurrentFrameNumber;
        }
        
        // Notify USB port driver about transfer completion
        RegPacket.UsbPortCompleteTransfer(XhciExtension,
                                          PendingTransfer->XhciEndpoint,
                                          PendingTransfer->XhciTransfer->TransferParameters,
                                          USBDStatus,
                                          TransferLength);
    }
    else
    {
        DPRINT1("XHCI_CompleteTransfer: ERROR - Could not find transfer context for completion\n");
    }
}

// Device enumeration functions
MPSTATUS
NTAPI
WaitForCommandCompletion(IN PXHCI_EXTENSION XhciExtension,
                        IN PXHCI_PENDING_COMMAND PendingCommand,
                        IN ULONG TimeoutMs)
{
    ULONG WaitIterations = TimeoutMs / 10; // Check every 10ms
    ULONG i;
    
    DPRINT1("WaitForCommandCompletion: Waiting for command type %d, timeout %dms\n", 
            PendingCommand->CommandType, TimeoutMs);
    
    for (i = 0; i < WaitIterations; i++)
    {
        // Process events to handle command completion
        XHCI_ProcessEvent(XhciExtension);
        
        // Check if command completed
        if (PendingCommand->Completed)
        {
            DPRINT1("WaitForCommandCompletion: Command completed with code %d after %dms\n",
                    PendingCommand->CompletionCode, i * 10);
            
            if (PendingCommand->CompletionCode == SUCCESS)
            {
                return MP_STATUS_SUCCESS;
            }
            else
            {
                DPRINT1("WaitForCommandCompletion: Command failed with completion code %d\n",
                        PendingCommand->CompletionCode);
                return MP_STATUS_FAILURE;
            }
        }
        
        // Wait 10ms before checking again
        KeStallExecutionProcessor(10000); // 10ms = 10000 microseconds
    }
    
    DPRINT1("WaitForCommandCompletion: Command timed out after %dms\n", TimeoutMs);
    return MP_STATUS_FAILURE;
}

ULONG
NTAPI
GetDeviceSpeedFromPort(IN PXHCI_EXTENSION XhciExtension, IN ULONG PortNumber)
{
    PULONG PortReg;
    XHCI_PORT_STATUS_CONTROL PortStatus;
    ULONG DeviceSpeed;
    
    // Calculate port register address
    PortReg = XhciExtension->OperationalRegs + (0x400 / sizeof(ULONG)) + ((PortNumber - 1) * 4);
    PortStatus.AsULONG = READ_REGISTER_ULONG(PortReg);
    
    // Extract port speed from bits 13:10
    DeviceSpeed = (PortStatus.AsULONG >> 10) & 0xF;
    
    DPRINT1("GetDeviceSpeedFromPort: Port %d speed = %d\n", PortNumber, DeviceSpeed);
    
    // Map xHCI speed values to USB port speeds
    switch (DeviceSpeed)
    {
        case 1: return UsbFullSpeed;    // Full-speed
        case 2: return UsbLowSpeed;     // Low-speed  
        case 3: return UsbHighSpeed;    // High-speed
        case 4: return 3;               // SuperSpeed (define as 3 since UsbSuperSpeed not defined)
        default: return UsbFullSpeed;   // Default fallback
    }
}

ULONG
NTAPI
GetMaxPacketSizeForSpeed(IN ULONG DeviceSpeed)
{
    switch (DeviceSpeed)
    {
        case UsbLowSpeed:    return 8;   // Low-speed: 8 bytes
        case UsbFullSpeed:   return 64;  // Full-speed: 64 bytes
        case UsbHighSpeed:   return 64;  // High-speed: 64 bytes
        case 3:              return 512; // SuperSpeed: 512 bytes (3 = our SuperSpeed value)
        default:             return 64;  // Default fallback
    }
}

MPSTATUS
NTAPI
XHCI_EnumerateDevice(IN PXHCI_EXTENSION XhciExtension,
                    IN ULONG PortNumber,
                    OUT PULONG SlotId,
                    OUT PULONG DeviceAddress)
{
    MPSTATUS Status;
  //  PXHCI_PENDING_COMMAND PendingCommand;
    ULONG DeviceSpeed, MaxPacketSize;
    
    DPRINT1("XHCI_EnumerateDevice: Starting enumeration for port %d\n", PortNumber);
    
    // Initialize command tracking
    InitializeCommandTracking();
    
    // Step 1: Enable Slot
    Status = XHCI_EnableSlot(XhciExtension, PortNumber, SlotId);
    if (Status != MP_STATUS_SUCCESS)
    {
        DPRINT1("XHCI_EnumerateDevice: Enable Slot failed\n");
        return Status;
    }
    
    DPRINT1("XHCI_EnumerateDevice: Enable Slot successful, slot ID = %d\n", *SlotId);
    
    // Get device speed from port
    DeviceSpeed = GetDeviceSpeedFromPort(XhciExtension, PortNumber);
    MaxPacketSize = GetMaxPacketSizeForSpeed(DeviceSpeed);
    
    DPRINT1("XHCI_EnumerateDevice: Device speed = %d, max packet size = %d\n", DeviceSpeed, MaxPacketSize);
    
    // Step 1.5: Set up Input Context (slot context + EP0 context)
    // For enumeration, we'll pass NULL for XhciEndpoint and use slot-specific ring
    Status = XHCI_SetupDeviceContext(XhciExtension, *SlotId, MaxPacketSize, DeviceSpeed, NULL, PortNumber);
    if (Status != MP_STATUS_SUCCESS)
    {
        DPRINT1("XHCI_EnumerateDevice: Setup Device Context failed\n");
        return Status;
    }
    
    // Step 2: Setup DCBAA entry so controller knows where device context is located
    Status = XHCI_SetupDCBAAEntry(XhciExtension, *SlotId);
    if (Status != MP_STATUS_SUCCESS)
    {
        DPRINT1("XHCI_EnumerateDevice: Setup DCBAA entry failed\n");
        return Status;
    }
    
    // Step 3: Issue Address Device command to configure the device
    PHYSICAL_ADDRESS InputContextPA = MmGetPhysicalAddress(&XhciExtension->HcResourcesVA->InputContext);
    Status = XHCI_AddressDevice(XhciExtension, *SlotId, InputContextPA, FALSE);
    if (Status != MP_STATUS_SUCCESS)
    {
        DPRINT1("XHCI_EnumerateDevice: Address Device failed\n");
        return Status;
    }
    
    // Get assigned device address
    *DeviceAddress = XHCI_GetDeviceAddressFromOutputContext(XhciExtension, *SlotId);
    
    DPRINT1("XHCI_EnumerateDevice: Device enumeration completed successfully\n");
    DPRINT1("XHCI_EnumerateDevice: Slot ID = %d, Device Address = %d\n", *SlotId, *DeviceAddress);
    
    return MP_STATUS_SUCCESS;
}

ULONG
NTAPI
XHCI_GetDeviceAddressFromOutputContext(IN PXHCI_EXTENSION XhciExtension,
                                      IN ULONG SlotId)
{
    PXHCI_HC_RESOURCES HcResourcesVA = (PXHCI_HC_RESOURCES)XhciExtension->HcResourcesVA;
    PXHCI_INPUT_CONTEXT OutputContext;
    PXHCI_SLOT_CONTEXT SlotContext;
    ULONG DeviceAddress;
    
    if (SlotId == 0 || SlotId > XHCI_MAX_SLOTS) {
        DPRINT1("XHCI_GetDeviceAddressFromOutputContext: Invalid slot ID %d\n", SlotId);
        return 0;
    }
    
    // Get the device-specific output context
    OutputContext = &HcResourcesVA->OutputContexts[SlotId - 1];
    SlotContext = &OutputContext->SlotContext;
    
    // Read the device address from the slot context
    // Device address is in bits 7:0 of the USBDeviceAddress field
    DeviceAddress = SlotContext->USBDeviceAddress & 0xFF;  // Extract device address field
    
    DPRINT1("XHCI_GetDeviceAddressFromOutputContext: Slot %d has device address %d\n", 
            SlotId, DeviceAddress);
    
    return DeviceAddress;
}

MPSTATUS
NTAPI
XHCI_EnableSlot(IN PXHCI_EXTENSION XhciExtension,
               IN ULONG PortNumber,
               OUT PULONG SlotId)
{
    XHCI_TRB EnableSlotTrb;
    PXHCI_PENDING_COMMAND PendingCommand;
    PHYSICAL_ADDRESS TrbPA;
    MPSTATUS Status;
    
    DPRINT1("XHCI_EnableSlot: Enabling slot for port %d\n", PortNumber);
    
    // Check controller operational state before issuing Enable Slot command
    PULONG OperationalRegs = XhciExtension->OperationalRegs;
    ULONG UsbSts = READ_REGISTER_ULONG(OperationalRegs + 1); // USBSTS register
    ULONG UsbCmd = READ_REGISTER_ULONG(OperationalRegs + 0); // USBCMD register
    
    DPRINT1("XHCI_EnableSlot: Controller state - USBCMD=0x%x, USBSTS=0x%x\n", UsbCmd, UsbSts);
    DPRINT1("XHCI_EnableSlot: HC Halted=%s, Run/Stop=%s\n", 
            (UsbSts & 0x1) ? "YES" : "NO",
            (UsbCmd & 0x1) ? "RUN" : "STOP");
    
    // Create Enable Slot command TRB
    RtlZeroMemory(&EnableSlotTrb, sizeof(XHCI_TRB));
    EnableSlotTrb.GenericTRB.Word0 = 0;
    EnableSlotTrb.GenericTRB.Word1 = 0;
    EnableSlotTrb.GenericTRB.Word2 = 0;
    EnableSlotTrb.GenericTRB.Word3 = ((ENABLE_SLOT_COMMAND << 10) |  // TRB Type
                                     (0 << 16) |                     // Slot Type = 0 (no specific type)
                                     1);                             // Cycle bit will be set by XHCI_SendCommand
    
    // IMPORTANT: Pre-calculate the TRB physical address where the command will be placed
    // We need to add the pending command BEFORE sending it to avoid race conditions
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
    PendingCommand = AddPendingCommand(COMMAND_ENABLE_SLOT, TrbPA, 0);
    if (!PendingCommand)
    {
        DPRINT1("XHCI_EnableSlot: Failed to add pending command\n");
        return MP_STATUS_FAILURE;
    }
    
    // Send command and get TRB physical address
    Status = XHCI_SendCommand(EnableSlotTrb, XhciExtension);
    if (Status != MP_STATUS_SUCCESS)
    {
        DPRINT1("XHCI_EnableSlot: Failed to send Enable Slot command\n");
        // Remove the pending command since we failed to send it
        RemovePendingCommand(PendingCommand);
        return Status;
    }
    
    // Wait for completion
    Status = WaitForCommandCompletion(XhciExtension, PendingCommand, 1000); // 1 second timeout
    if (Status == MP_STATUS_SUCCESS)
    {
        *SlotId = PendingCommand->CompletionSlotId;
        DPRINT1("XHCI_EnableSlot: Enable Slot completed successfully, slot ID = %d\n", *SlotId);
    }
    else
    {
        DPRINT1("XHCI_EnableSlot: Enable Slot failed or timed out\n");
    }
    
    // Remove from pending commands
    RemovePendingCommand(PendingCommand);
    
    return Status;
}

MPSTATUS
NTAPI
XHCI_SetupDeviceContext(IN PXHCI_EXTENSION XhciExtension,
                       IN ULONG SlotId,
                       IN ULONG MaxPacketSize,
                       IN ULONG DeviceSpeed,
                       IN PXHCI_ENDPOINT XhciEndpoint,
                       IN ULONG PortNumber)
{
    PXHCI_HC_RESOURCES HcResourcesVA = (PXHCI_HC_RESOURCES)XhciExtension->HcResourcesVA;
    PXHCI_INPUT_CONTEXT InputContext;
    PXHCI_SLOT_CONTEXT SlotContext;
    PXHCI_ENDPOINT_CONTEXT EndpointContext;
    ULONG RouteString = 0;  // For root hub device
    ULONG RootHubPortNumber = PortNumber;  // Use the actual port number
    
    DPRINT1("XHCI_SetupDeviceContext: Setting up device context for slot %d, MaxPacketSize=%d, Speed=%d, Port=%d\n",
            SlotId, MaxPacketSize, DeviceSpeed, PortNumber);
    
    // Get pointer to input context (reuse command ring memory for simplicity)
    InputContext = &HcResourcesVA->InputContext;

    // Clear input context
    RtlZeroMemory(InputContext, sizeof(XHCI_INPUT_CONTEXT));
    
    // Set input control context - enable slot context and EP0 context
    InputContext->InputContext.AddContextFlags = 0x3;  // Enable Slot Context (bit 0) and EP0 Context (bit 1)
    InputContext->InputContext.DropContextFlags = 0x0;
    
    // Get pointers to slot and endpoint contexts within input context
    SlotContext = &InputContext->SlotContext;
    EndpointContext = &InputContext->EndpointContextList[0];  // EP0 context
    
    // Set up slot context with proper xHCI speed encoding
    SlotContext->SlotState = 0;  // Default state
    SlotContext->RouteString = RouteString;
    
    // Convert USB speed to xHCI speed encoding
    // USB enum: 0=Low, 1=Full, 2=High; xHCI: 1=Full, 2=Low, 3=High, 4=Super
    ULONG XhciSpeed;
    switch (DeviceSpeed) {
        case 0: XhciSpeed = 2; break;  // Low Speed -> 2
        case 1: XhciSpeed = 1; break;  // Full Speed ->  1  
        case 2: XhciSpeed = 3; break;  // High Speed -> 3
        default: XhciSpeed = 1; break; // Default to Full Speed
    }
    SlotContext->Speed = XhciSpeed;
    SlotContext->ContextEntries = 1;  // Only EP0 for now
    SlotContext->RootHubPortNumber = RootHubPortNumber;
    
    // Set up endpoint 0 context with all required fields
    EndpointContext->EPState = 0;  // Disabled initially
    EndpointContext->EPType = 4;   // Control endpoint (bidirectional)
    EndpointContext->MaxPacketSize = MaxPacketSize;
    EndpointContext->MaxBurstSize = 0;   // No burst for control endpoints
    EndpointContext->CErr = 3;    // CErr = 3 (retry 3 times on error)
    EndpointContext->Interval = 0;  // No periodic scheduling for control endpoints
    EndpointContext->Mult = 0;     // Not used for control endpoints
    EndpointContext->LSA = 0;      // Linear Stream Array = 0 (not using streams)
    EndpointContext->MaxPStreams = 0;  // Not using streams
    EndpointContext->MaxESITPayload = 0;    // Not applicable for control endpoints
    EndpointContext->MaxESITHigh = 0;       // High bits of ESIT payload
    EndpointContext->AverageTRBLength = 8;  // Default average TRB length for control transfers
    EndpointContext->HID = 0;       // Host Initiate Disable = 0
    
    // Step 5: Set up Transfer Ring Dequeue Pointer for EP0
    // CRITICAL: This must point to the SAME transfer ring used for transfer submissions
    PHYSICAL_ADDRESS TransferRingPA;
    if (XhciEndpoint != NULL) {
        // Use the endpoint's own transfer ring (preferred method)
        TransferRingPA = MmGetPhysicalAddress(&XhciEndpoint->TransferRing.firstSeg.XhciTrb[0]);
        DPRINT1("XHCI_SetupDeviceContext: Using endpoint transfer ring at PA 0x%I64x\n", TransferRingPA.QuadPart);
    } else {
        // Fallback to slot-specific ring for enumeration
        TransferRingPA = MmGetPhysicalAddress(&HcResourcesVA->SlotTransferRings[SlotId - 1].firstSeg.XhciTrb[0]);
        DPRINT1("XHCI_SetupDeviceContext: Using slot transfer ring at PA 0x%I64x\n", TransferRingPA.QuadPart);
    }
    EndpointContext->TRDeqPtr = (TransferRingPA.QuadPart & ~0xF) | 1;  // Set DCS=1, clear reserved bits
    
    DPRINT1("XHCI_SetupDeviceContext: Device context setup completed successfully\n");
    return MP_STATUS_SUCCESS;
}

MPSTATUS
NTAPI
XHCI_AddressDevice(IN PXHCI_EXTENSION XhciExtension,
                  IN ULONG SlotId,
                  IN PHYSICAL_ADDRESS InputContextPA,
                  IN BOOLEAN BlockSetRequest)
{
    XHCI_TRB CommandTrb;
    PXHCI_PENDING_COMMAND PendingCommand;
    PHYSICAL_ADDRESS TrbPA;
    MPSTATUS Status;
    
    DPRINT1("XHCI_AddressDevice: Issuing Address Device command for slot %d\n", SlotId);
    
    // Prepare Address Device command TRB
    RtlZeroMemory(&CommandTrb, sizeof(XHCI_TRB));
    CommandTrb.GenericTRB.Word0 = (ULONG)(InputContextPA.QuadPart & 0xFFFFFFFF);
    CommandTrb.GenericTRB.Word1 = (ULONG)(InputContextPA.QuadPart >> 32);
    CommandTrb.GenericTRB.Word2 = 0;
    CommandTrb.GenericTRB.Word3 = ((ADDRESS_DEVICE_COMMAND << 10) |  // TRB Type
                                   (SlotId << 24) |                   // Slot ID
                                   (BlockSetRequest ? 0 : (1 << 9)) | // BSR bit (0 = assign address)
                                   1);                                // Cycle bit will be set by XHCI_SendCommand
    
    // IMPORTANT: Pre-calculate the TRB physical address where the command will be placed
    // We need to add the pending command BEFORE sending it to avoid race conditions
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
    PendingCommand = AddPendingCommand(COMMAND_ADDRESS_DEVICE, TrbPA, SlotId);
    if (!PendingCommand)
    {
        DPRINT1("XHCI_AddressDevice: Failed to add pending command\n");
        return MP_STATUS_FAILURE;
    }
    
    // Submit command to command ring
    Status = XHCI_SendCommand(CommandTrb, XhciExtension);
    if (Status != MP_STATUS_SUCCESS) {
        DPRINT1("XHCI_AddressDevice: Failed to submit command\n");
        // Remove the pending command since we failed to send it
        RemovePendingCommand(PendingCommand);
        return Status;
    }
    
    // Wait for completion
    Status = WaitForCommandCompletion(XhciExtension, PendingCommand, 2000); // 2 second timeout
    if (Status == MP_STATUS_SUCCESS)
    {
        DPRINT1("XHCI_AddressDevice: Address Device completed successfully\n");
    }
    else
    {
        DPRINT1("XHCI_AddressDevice: Address Device failed or timed out\n");
    }
    
    // Remove from pending commands
    RemovePendingCommand(PendingCommand);

    return Status;
}

/*
 * XHCI_ConfigureEndpoint
 *
 * Issue a CONFIGURE_ENDPOINT_COMMAND for a single non-EP0 endpoint on an
 * already-addressed slot.  Tells the controller about one new DCI so it
 * transitions the EP Context from Disabled to Running; without this the
 * controller silently drops doorbells rung on that DCI.
 *
 * EndpointIndex is the xHCI Device Context Index (DCI): 2..31.
 * EndpointType is the xHCI EP Type encoding:
 *   1 = Isoch OUT, 2 = Bulk OUT, 3 = Interrupt OUT,
 *   4 = Control, 5 = Isoch IN, 6 = Bulk IN, 7 = Interrupt IN.
 * TransferRingPA must be the PA of the first TRB on the endpoint's
 * transfer ring; the DCS (bit 0) is forced to 1 by this helper.
 *
 * This first pass supports single-DCI add only (no sibling ExpandAddFlags
 * re-add).  usbport opens bulk IN and bulk OUT via separate OpenEndpoint
 * calls, so one DCI per Configure Endpoint command is sufficient.
 */
MPSTATUS
NTAPI
XHCI_ConfigureEndpoint(IN PXHCI_EXTENSION XhciExtension,
                       IN ULONG SlotId,
                       IN ULONG EndpointIndex,
                       IN ULONG EndpointType,
                       IN ULONG MaxPacketSize,
                       IN PHYSICAL_ADDRESS TransferRingPA)
{
    PXHCI_HC_RESOURCES HcResourcesVA;
    PHYSICAL_ADDRESS HcResourcesPA;
    PXHCI_INPUT_CONTEXT InputContext;
    PXHCI_INPUT_CONTEXT OutputContext;
    PXHCI_SLOT_CONTEXT InputSlotContext;
    PXHCI_SLOT_CONTEXT OutputSlotContext;
    PXHCI_ENDPOINT_CONTEXT EpCtx;
    PHYSICAL_ADDRESS InputContextPA;
    PHYSICAL_ADDRESS TrbPA;
    PXHCI_TRB enqueue_pointer;
    ULONG TrbIndex;
    ULONG ContextEntries;
    XHCI_TRB CommandTrb;
    PXHCI_PENDING_COMMAND PendingCommand;
    MPSTATUS Status;

    DPRINT1("XHCI_ConfigureEndpoint: slot=%d DCI=%d EPType=%d MaxPacket=%d RingPA=0x%I64x\n",
            SlotId, EndpointIndex, EndpointType, MaxPacketSize, TransferRingPA.QuadPart);

    if (SlotId == 0 || SlotId > XHCI_MAX_SLOTS)
    {
        DPRINT1("XHCI_ConfigureEndpoint: Invalid slot ID %d\n", SlotId);
        return MP_STATUS_FAILURE;
    }
    if (EndpointIndex < 2 || EndpointIndex > 31)
    {
        DPRINT1("XHCI_ConfigureEndpoint: Invalid DCI %d (must be 2..31)\n", EndpointIndex);
        return MP_STATUS_FAILURE;
    }

    HcResourcesVA = (PXHCI_HC_RESOURCES)XhciExtension->HcResourcesVA;
    HcResourcesPA = XhciExtension->HcResourcesPA;

    /* 1. Zero and populate the shared Input Context. */
    InputContext = &HcResourcesVA->InputContext;
    RtlZeroMemory(InputContext, sizeof(XHCI_INPUT_CONTEXT));

    /* 2. Input Control Context: add the slot context (A0) and the new DCI. */
    InputContext->InputContext.AddContextFlags = (1u << 0) | (1u << EndpointIndex);
    InputContext->InputContext.DropContextFlags = 0;

    /*
     * 3. Copy the controller's current output slot context into the input
     *    slot context.  CONFIGURE_ENDPOINT is a read-modify-write: the
     *    controller re-reads the slot context, so it must reflect the
     *    addressed device's current state (speed, route string, root hub
     *    port, device address, slot state).
     */
    OutputContext = &HcResourcesVA->OutputContexts[SlotId - 1];
    OutputSlotContext = &OutputContext->SlotContext;
    InputSlotContext = &InputContext->SlotContext;
    RtlCopyMemory(InputSlotContext, OutputSlotContext, sizeof(XHCI_SLOT_CONTEXT));

    /* 4. Bump ContextEntries to at least the new DCI so the controller
     *    treats it as valid. */
    ContextEntries = InputSlotContext->ContextEntries;
    if (ContextEntries < EndpointIndex)
        ContextEntries = EndpointIndex;
    InputSlotContext->ContextEntries = ContextEntries;

    /* 5. Fill the endpoint context at (DCI - 1).  EndpointContextList[0]
     *    is EP0/DCI 1. */
    EpCtx = &InputContext->EndpointContextList[EndpointIndex - 1];
    EpCtx->EPState          = 0;   /* Disabled -> controller will move to Running */
    EpCtx->EPType           = EndpointType;
    EpCtx->MaxPacketSize    = MaxPacketSize;
    EpCtx->MaxBurstSize     = 0;   /* SS burst not supported in first pass */
    EpCtx->CErr             = 3;   /* Spec-recommended retry count for bulk/int */
    EpCtx->Mult             = 0;
    EpCtx->LSA              = 0;
    EpCtx->MaxPStreams      = 0;
    EpCtx->Interval         = 0;
    EpCtx->MaxESITPayload   = 0;
    EpCtx->MaxESITHigh      = 0;
    EpCtx->HID              = 0;
    EpCtx->AverageTRBLength = (EndpointType == 2 || EndpointType == 6) ? 1024 : 8;
    EpCtx->TRDeqPtr         = (TransferRingPA.QuadPart & ~((ULONGLONG)0xFULL)) | 1ULL; /* DCS = 1 */

    InputContextPA = MmGetPhysicalAddress(InputContext);
    DPRINT1("XHCI_ConfigureEndpoint: InputContext PA=0x%I64x, AddFlags=0x%x, ContextEntries=%d\n",
            InputContextPA.QuadPart,
            InputContext->InputContext.AddContextFlags,
            InputSlotContext->ContextEntries);

    /* 6. Build and issue the Configure Endpoint command.  Same command-ring
     *    + pending-command pattern as XHCI_AddressDevice. */
    RtlZeroMemory(&CommandTrb, sizeof(XHCI_TRB));
    CommandTrb.GenericTRB.Word0 = (ULONG)(InputContextPA.QuadPart & 0xFFFFFFFF);
    CommandTrb.GenericTRB.Word1 = (ULONG)(InputContextPA.QuadPart >> 32);
    CommandTrb.GenericTRB.Word2 = 0;
    CommandTrb.GenericTRB.Word3 = ((CONFIGURE_ENDPOINT_COMMAND << 10) |
                                   (SlotId << 24) |
                                   1);  /* Cycle bit fixed up inside XHCI_SendCommand */

    /* Pre-compute the PA where the TRB will land (matches AddressDevice). */
    enqueue_pointer = HcResourcesVA->CommandRing.enqueue_pointer;
    TrbIndex = (ULONG)((ULONG_PTR)enqueue_pointer -
                       (ULONG_PTR)&HcResourcesVA->CommandRing.firstSeg.XhciTrb[0]) /
               sizeof(XHCI_TRB);
    TrbPA.QuadPart = HcResourcesPA.QuadPart +
                     FIELD_OFFSET(XHCI_HC_RESOURCES, CommandRing.firstSeg.XhciTrb[0]) +
                     (TrbIndex * sizeof(XHCI_TRB));

    PendingCommand = AddPendingCommand(COMMAND_CONFIGURE_ENDPOINT, TrbPA, SlotId);
    if (!PendingCommand)
    {
        DPRINT1("XHCI_ConfigureEndpoint: Failed to add pending command\n");
        return MP_STATUS_FAILURE;
    }

    Status = XHCI_SendCommand(CommandTrb, XhciExtension);
    if (Status != MP_STATUS_SUCCESS)
    {
        DPRINT1("XHCI_ConfigureEndpoint: Failed to submit command\n");
        RemovePendingCommand(PendingCommand);
        return Status;
    }

    /* 7. Wait for the matching COMMAND_COMPLETION_EVENT. */
    Status = WaitForCommandCompletion(XhciExtension, PendingCommand, 2000);
    if (Status == MP_STATUS_SUCCESS)
    {
        DPRINT1("XHCI_ConfigureEndpoint: slot %d DCI %d configured OK\n",
                SlotId, EndpointIndex);
    }
    else
    {
        DPRINT1("XHCI_ConfigureEndpoint: slot %d DCI %d FAILED (completion=%d)\n",
                SlotId, EndpointIndex, PendingCommand->CompletionCode);
    }

    RemovePendingCommand(PendingCommand);
    return Status;
}

// Slot management functions
VOID
InitializeSlotTracking(VOID)
{
    ULONG i;
    
    if (g_SlotTrackingInitialized)
        return;
        
    for (i = 0; i < MAX_DEVICE_SLOTS; i++)
    {
        RtlZeroMemory(&g_DeviceSlots[i], sizeof(XHCI_SLOT_INFO));
        g_DeviceSlots[i].InUse = FALSE;
        g_DeviceSlots[i].BeingEnumerated = FALSE;
    }
    
    g_SlotTrackingInitialized = TRUE;
    DPRINT1("InitializeSlotTracking: Slot tracking system initialized\n");
}

ULONG
ReserveSlotForEnumeration(ULONG PortNumber)
{
    ULONG i;
    
    InitializeSlotTracking();
    
    for (i = 1; i < MAX_DEVICE_SLOTS; i++) // Start from 1, slot 0 is invalid
    {
        if (!g_DeviceSlots[i].InUse && !g_DeviceSlots[i].BeingEnumerated)
        {
            g_DeviceSlots[i].SlotId = i;
            g_DeviceSlots[i].PortNumber = PortNumber;
            g_DeviceSlots[i].BeingEnumerated = TRUE;
            g_DeviceSlots[i].InUse = TRUE;
            
            DPRINT1("ReserveSlotForEnumeration: Reserved slot %d for port %d\n", i, PortNumber);
            return i;
        }
    }
    
    DPRINT1("ReserveSlotForEnumeration: No free slots available\n");
    return INVALID_SLOT_ID;
}

VOID
ConfirmSlotAllocation(ULONG SlotId, ULONG DeviceAddress)
{
    if (SlotId > 0 && SlotId < MAX_DEVICE_SLOTS && g_DeviceSlots[SlotId].InUse)
    {
        g_DeviceSlots[SlotId].DeviceAddress = DeviceAddress;
        g_DeviceSlots[SlotId].BeingEnumerated = FALSE;
        DPRINT1("ConfirmSlotAllocation: Confirmed slot %d with device address %d\n", SlotId, DeviceAddress);
    }
}

VOID
ReleaseSlot(ULONG SlotId)
{
    if (SlotId > 0 && SlotId < MAX_DEVICE_SLOTS && g_DeviceSlots[SlotId].InUse)
    {
        DPRINT1("ReleaseSlot: Releasing slot %d\n", SlotId);
        
        // Free allocated memory if any
        if (g_DeviceSlots[SlotId].AllocatedMemory)
        {
            ExFreePool(g_DeviceSlots[SlotId].AllocatedMemory);
        }
        
        RtlZeroMemory(&g_DeviceSlots[SlotId], sizeof(XHCI_SLOT_INFO));
        g_DeviceSlots[SlotId].InUse = FALSE;
        g_DeviceSlots[SlotId].BeingEnumerated = FALSE;
    }
}

BOOLEAN
IsSlotInUse(ULONG SlotId)
{
    if (SlotId > 0 && SlotId < MAX_DEVICE_SLOTS)
    {
        return g_DeviceSlots[SlotId].InUse;
    }
    return FALSE;
}

PXHCI_INPUT_CONTEXT
GetSlotInputContext(ULONG SlotId, PPHYSICAL_ADDRESS InputContextPA)
{
    DPRINT1("GetSlotInputContext: Getting input context for slot %d\n", SlotId);
    
    if (SlotId == 0 || SlotId >= MAX_DEVICE_SLOTS)
    {
        DPRINT1("GetSlotInputContext: Invalid slot ID %d\n", SlotId);
        return NULL;
    }
    
    InitializeSlotTracking();
    
    // If we don't have an allocated input context for this slot, allocate one
    if (!g_DeviceSlots[SlotId].AllocatedInputContext)
    {
        // Allocate DMA-coherent memory for input context
        PVOID AllocatedMemory = ExAllocatePoolZero(NonPagedPool, 
                                                   sizeof(XHCI_INPUT_CONTEXT) + 16, // Add 16 bytes for alignment
                                                   'XINP');
        if (!AllocatedMemory)
        {
            DPRINT1("GetSlotInputContext: Failed to allocate input context for slot %d\n", SlotId);
            return NULL;
        }
        
        // Align to 16-byte boundary (required by xHCI spec)
        PXHCI_INPUT_CONTEXT InputContext = (PXHCI_INPUT_CONTEXT)(((ULONG_PTR)AllocatedMemory + 15) & ~15);
        
        // Zero out the context
        RtlZeroMemory(InputContext, sizeof(XHCI_INPUT_CONTEXT));
        
        // Store in slot tracking
        g_DeviceSlots[SlotId].AllocatedInputContext = InputContext;
        g_DeviceSlots[SlotId].AllocatedMemory = AllocatedMemory;
        g_DeviceSlots[SlotId].InputContextPA = MmGetPhysicalAddress(InputContext);
        
        DPRINT1("GetSlotInputContext: Allocated new input context for slot %d at VA %p, PA 0x%I64x\n",
                SlotId, InputContext, g_DeviceSlots[SlotId].InputContextPA.QuadPart);
    }
    
    // Return the physical address
    if (InputContextPA)
    {
        *InputContextPA = g_DeviceSlots[SlotId].InputContextPA;
    }
    
    return g_DeviceSlots[SlotId].AllocatedInputContext;
}

VOID
NTAPI
CleanupSlotResources(IN PXHCI_EXTENSION XhciExtension, IN ULONG SlotId)
{
    UNREFERENCED_PARAMETER(XhciExtension);
    
    DPRINT1("CleanupSlotResources: Cleaning up resources for slot %d\n", SlotId);
    
    if (SlotId > 0 && SlotId < MAX_DEVICE_SLOTS)
    {
        ReleaseSlot(SlotId);
    }
}

MPSTATUS
NTAPI
XHCI_SetupDCBAAEntry(IN PXHCI_EXTENSION XhciExtension,
                     IN ULONG SlotId)
{
    PXHCI_HC_RESOURCES HcResourcesVA = (PXHCI_HC_RESOURCES)XhciExtension->HcResourcesVA;
    PHYSICAL_ADDRESS OutputContextPA;
    PXHCI_INPUT_CONTEXT OutputContext;
    
    DPRINT1("XHCI_SetupDCBAAEntry: Setting up DCBAA entry for slot %d\n", SlotId);
    
    if (SlotId == 0 || SlotId > XHCI_MAX_SLOTS) {
        DPRINT1("XHCI_SetupDCBAAEntry: Invalid slot ID %d\n", SlotId);
        return MP_STATUS_FAILURE;
    }
    
    // Get the device-specific output context
    OutputContext = &HcResourcesVA->OutputContexts[SlotId - 1];
    
    // Clear the output context
    RtlZeroMemory(OutputContext, sizeof(XHCI_INPUT_CONTEXT));
    
    // Get physical address of this device's output context
    OutputContextPA = MmGetPhysicalAddress(OutputContext);
    
    // Set the DCBAA entry to point to the device's output context
    HcResourcesVA->DCBAA.ContextBaseAddr[SlotId].QuadPart = OutputContextPA.QuadPart;
    
    DPRINT1("XHCI_SetupDCBAAEntry: DCBAA[%d] = 0x%I64x (device-specific context)\n", 
            SlotId, HcResourcesVA->DCBAA.ContextBaseAddr[SlotId].QuadPart);
    
    return MP_STATUS_SUCCESS;
}

ULONG
NTAPI
XHCI_GetCurrentFrameNumber(IN PXHCI_EXTENSION XhciExtension)
{
    PULONG RunTimeRegisterBase;
    ULONG MFIndex;
    
    DPRINT1("XHCI_GetCurrentFrameNumber: Reading current frame number\n");
    
    RunTimeRegisterBase = XhciExtension->RunTimeRegisterBase;
    if (!RunTimeRegisterBase)
    {
        DPRINT1("XHCI_GetCurrentFrameNumber: RunTimeRegisterBase is NULL\n");
        return 0;
    }
    
    // Read the MFINDEX register which contains the current microframe index
    // The MFINDEX register is 14 bits wide and increments every 125 microseconds
    MFIndex = READ_REGISTER_ULONG(RunTimeRegisterBase + XHCI_MFINDEX);
    
    // Extract the 14-bit microframe index (bits 0-13)
    MFIndex &= 0x3FFF;
    
    DPRINT1("XHCI_GetCurrentFrameNumber: Current MFINDEX = %d\n", MFIndex);
    
    // Convert microframe index to frame number (divide by 8 since there are 8 microframes per frame)
    return MFIndex / 8;
}