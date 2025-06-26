/*
 * PROJECT:         ReactOS xHCI Driver
 * LICENSE:         GPLv2+ - See COPYING in the top level directory
 * PURPOSE:         hardware register definitions
 * COPYRIGHT:       Rama Teja Gampa <ramateja.g@gmail.com>
 */

#pragma once

/* Register Definitions ***************************************************************************/

// base io addr register offsets
#define XHCI_HCSP1            1
#define XHCI_HCSP2            2
#define XHCI_HCSP3            3
#define XHCI_HCCP1            4
#define XHCI_DBOFF            5
#define XHCI_RTSOFF           6
#define XHCI_HCCP2            7
// operational register offsets
#define XHCI_USBCMD           0
#define XHCI_USBSTS           1
#define XHCI_PGSZ             2
#define XHCI_DNCTRL           5
#define XHCI_CRCR             6
#define XHCI_DCBAAP           12
#define XHCI_CONFIG           14
#define XHCI_PORTSC           256
// runtime register offsets
#define XHCI_MFINDEX          0
#define XHCI_IMAN             8
#define XHCI_IMOD             9
#define XHCI_ERSTSZ           10
#define XHCI_ERSTBA           12
#define XHCI_ERSTDP           14

/* Hardware Structs *******************************************************************************/

/* 5.4.8 */
typedef struct _XHCI_PORT_STATUS_REGISTER
{
    struct 
    {
        ULONG WPR                            : 1;
        ULONG DR                             : 1;
        ULONG RsvdZ1                         : 2;
        ULONG WOE                            : 1;
        ULONG WDE                            : 1;
        ULONG WCE                            : 1;
        ULONG CAS                            : 1;
        ULONG CEC                            : 1;
        ULONG PLC                            : 1;
        ULONG PRC                            : 1;
        ULONG OCC                            : 1;
        ULONG WRC                            : 1;
        ULONG PEC                            : 1;
        ULONG CSC                            : 1;
        ULONG LWS                            : 1;
        ULONG PIC                            : 2;
        ULONG PortSpeed                      : 4;
        ULONG PP                             : 1;
        ULONG PLS                            : 4;
        ULONG PR                             : 1;
        ULONG OCA                            : 1;
        ULONG RsvdZ2                         : 1;
        ULONG PED                            : 1;
        ULONG CCS                            : 1;
        
    };
} XHCI_PORT_STATUS_REGISTER, *PXHCI_PORT_STATUS_REGISTER;

typedef volatile union _XHCI_CAPLENGHT_INTERFACE_VERSION 
{
    struct 
    {
        ULONG CapabilityRegistersLength                         : 8; 
        ULONG Rsvd                                              : 8; 
        ULONG HostControllerInterfaceVersion                    : 16;
    };
    ULONG AsULONG;
} XHCI_CAPLENGHT_INTERFACE_VERSION;

typedef volatile union _XHCI_HC_STRUCTURAL_PARAMS_1 
{
    struct 
    {
        ULONG NumberOfDeviceSlots     : 8; // MAXSLOTS
        ULONG NumberOfInterrupters    : 11; // MAXINTRS
        ULONG Rsvd                    : 5;
        ULONG NumberOfPorts           : 8; //MAXPORTS
    };
    ULONG AsULONG;
} XHCI_HC_STRUCTURAL_PARAMS_1;

typedef volatile union _XHCI_HC_STRUCTURAL_PARAMS_2 
{
    struct 
    {
        ULONG Ist               : 4; // Isochronous Scheduling Treshold 
        ULONG ERSTMax           : 4; //Even ring segment table max
        ULONG Rsvd              : 13;
        ULONG MaxSPBuffersHi    : 5; //Max Scratchpad buffers high
        ULONG SPR               : 1; //Scratchpad Restore
        ULONG MaxSPBuffersLo    : 5; //Max Scratchpad buffers Low
    };
    ULONG AsULONG;
} XHCI_HC_STRUCTURAL_PARAMS_2;

typedef volatile union _XHCI_HC_STRUCTURAL_PARAMS_3 
{
    struct 
    {
        ULONG U1DeviceExitLatecy : 8;
        ULONG Rsvd               : 8;
        ULONG U2DeviceExitLatecy : 16;
    };
    ULONG AsULONG;
} XHCI_HC_STRUCTURAL_PARAMS_3;

typedef volatile union _XHCI_HC_CAPABILITY_PARAMS_1 
{  // need to comment full forms, pg 291 in xHCI documentation 
    struct
    {
        ULONG AC64               : 1;
        ULONG BNC                : 1;
        ULONG CSZ                : 1;
        ULONG PPC                : 1;
        ULONG PIND               : 1;
        ULONG LHRC               : 1;
        ULONG LTC                : 1;
        ULONG NSS                : 1;
        ULONG PAE                : 1;
        ULONG SPC                : 1;
        ULONG SEC                : 1;
        ULONG CFC                : 1;
        ULONG MaxPSASize         : 4;
        ULONG xECP               : 16;
    };
    ULONG AsULONG;
} XHCI_HC_CAPABILITY_PARAMS_1;

typedef volatile union _XHCI_DOORBELL_OFFSET 
{
    struct 
    {
        ULONG Rsvd               : 2;
        ULONG DBArrayOffset      : 30;
    };
    ULONG AsULONG;
} XHCI_DOORBELL_OFFSET;

typedef volatile union _XHCI_RT_REGISTER_SPACE_OFFSET 
{ //RUNTIME REGISTER SPACE OFFSET
    struct {
        ULONG Rsvd               : 5;
        ULONG RTSOffset          : 27;
    };
    ULONG AsULONG;
} XHCI_RT_REGISTER_SPACE_OFFSET;

typedef volatile union _XHCI_HC_CAPABILITY_PARAMS_2 
{
    struct 
    {
        ULONG U3C                : 1;
        ULONG CMC                : 1;
        ULONG FSC                : 1;
        ULONG CTC                : 1;
        ULONG LEC                : 1;
        ULONG CIC                : 1;
        ULONG Rsvd               : 26;
    };
    ULONG AsULONG;
} XHCI_HC_CAPABILITY_PARAMS_2;

typedef volatile union _XHCI_USB_COMMAND 
{
    struct 
        {
        ULONG RunStop                    : 1;
        ULONG HCReset                    : 1;
        ULONG InterrupterEnable          : 1;
        ULONG HostSystemErrorEnable      : 1;
        ULONG RsvdP1                     : 3;
        ULONG LightHCReset               : 1;
        ULONG ControllerSaveState        : 1; 
        ULONG ControllerRestoreState     : 1; 
        ULONG EnableWrapEvent            : 1;
        ULONG EnableU3Stop               : 1; 
        ULONG RsvdP2                     : 1;
        ULONG CEMEnable                  : 1;
        ULONG RsvdP3                     : 18;
    };
    ULONG AsULONG;
} XHCI_USB_COMMAND;
C_ASSERT(sizeof(XHCI_USB_COMMAND) == sizeof(ULONG));

typedef volatile union _XHCI_USB_STATUS 
{
    struct 
    {
        ULONG HCHalted               : 1;
        ULONG RsvdZ1                 : 1;
        ULONG HostSystemError        : 1;
        ULONG EventInterrupt         : 1;
        ULONG PortChangeDetect       : 1;
        ULONG RsvdZ2                 : 3;
        ULONG SaveStateStatus        : 1; 
        ULONG RestoreStateStatus     : 1; 
        ULONG SaveRestoreError       : 1;
        ULONG ControllerNotReady     : 1; 
        ULONG HCError                : 1;
        ULONG RsvdZ3                 : 19;
    };
    ULONG AsULONG;
} XHCI_USB_STATUS;
C_ASSERT(sizeof(XHCI_USB_STATUS) == sizeof(ULONG));

typedef volatile union _XHCI_PAGE_SIZE 
{ 
    struct 
    {
        ULONG PageSize           : 16;
        ULONG Rsvd               : 16;
    };
    ULONG AsULONG;
} XHCI_PAGE_SIZE;

typedef volatile union _XHCI_DEVICE_NOTIFICATION_CONTROL 
{ 
    struct 
    {
        ULONG NotificationEnable : 16;
        ULONG Rsvd               : 16;
    };
    ULONG AsULONG;
} XHCI_DEVICE_NOTIFICATION_CONTROL;

typedef volatile union _XHCI_COMMAND_RING_CONTROL 
{ 
    struct 
    {
        ULONGLONG RingCycleState           : 1;
        ULONGLONG CommandStop              : 1;
        ULONGLONG CommandAbort             : 1;
        ULONGLONG CommandRingRunning       : 1;
        ULONGLONG RsvdP                    : 2;
        ULONGLONG CommandRingPointerLo     : 26;
        ULONGLONG CommandRingPointerHi     : 32;
    };
    ULONGLONG AsULONGLONG;
} XHCI_COMMAND_RING_CONTROL;

typedef volatile union _XHCI_DEVICE_CONTEXT_BASE_ADD_ARRAY_POINTER 
{ 
    struct 
    {
        ULONGLONG RsvdZ                       : 6;
        ULONGLONG DCBAAPointerLo              : 26;
        ULONGLONG DCBAAPointerHi              : 32;
    };
    ULONGLONG AsULONGLONG;
} XHCI_DEVICE_CONTEXT_BASE_ADD_ARRAY_POINTER;

typedef volatile union _XHCI_CONFIGURE 
{ 
    struct 
    {
        ULONG MaxDeviceSlotsEnabled        : 8;
        ULONG U3EntryEnable                : 1;
        ULONG ConfigurationInfoEnable      : 1;
        ULONG Rsvd                         : 22;
    };
    ULONG AsULONG;
} XHCI_CONFIGURE;
C_ASSERT(sizeof(XHCI_CONFIGURE) == sizeof(ULONG));

#define PORT_STATUS_MASK    0x4F01FFE9  // 0100 1111 0000 0001 1111 1111 1110 1001 // RW 1, RW1C/RW1S 0, RO 1
typedef volatile union _XHCI_PORT_STATUS_CONTROL 
{
    struct 
    {
        ULONG CurrentConnectStatus                  : 1;
        ULONG PortEnableDisable                     : 1;
        ULONG RsvdZ1                                : 1;
        ULONG OverCurrentActive                     : 1;
        ULONG PortReset                             : 1;
        ULONG PortLinkState                         : 4;
        ULONG PortPower                             : 1;
        ULONG PortSpeed                             : 4;
        ULONG PortIndicatorControl                  : 2;
        ULONG LinkWriteStrobe                       : 1;
        ULONG ConnectStatusChange                   : 1;
        ULONG PortEnableDisableChange               : 1;
        ULONG WarmResetChange                       : 1;
        ULONG OverCurrentChange                     : 1;
        ULONG PortResetChange                       : 1;
        ULONG PortLinkStateChange                   : 1;
        ULONG ConfigErrorChange                     : 1;
        ULONG ColdAttachStatus                      : 1;
        ULONG WakeONConnectEnable                   : 1;
        ULONG WakeONDisconnectEnable                : 1;
        ULONG WakeONOverCurrentEnable               : 1;
        ULONG RsvdZ2                                : 2;
        ULONG DeviceRemovable                       : 1;
        ULONG WarmPortReset                         : 1;
    };
    ULONG AsULONG;
} XHCI_PORT_STATUS_CONTROL;

// Interrupt Register Set
typedef volatile union _XHCI_INTERRUPTER_MANAGEMENT 
{
    struct 
    {
        ULONG InterruptPending  : 1;
        ULONG InterruptEnable   : 1;
        ULONG RsvdP             : 30;
    };
    ULONG AsULONG;
} XHCI_INTERRUPTER_MANAGEMENT;

typedef volatile union _XHCI_INTERRUPTER_MODERATION 
{
    struct 
    {
        ULONG InterruptModIterval  : 16;
        ULONG InterruptModCounter  : 16;
    };
    ULONG AsULONG;
} XHCI_INTERRUPTER_MODERATION;

typedef volatile union _XHCI_EVENT_RING_TABLE_SIZE 
{
    struct 
    {
        ULONG EventRingSegTableSize  : 16;
        ULONG RsvdP                  : 16;
    };
    ULONG AsULONG;
} XHCI_EVENT_RING_TABLE_SIZE;

typedef volatile union _XHCI_EVENT_RING_TABLE_BASE_ADDR 
{ 
    struct
    {
        ULONGLONG RsvdP                       : 6;
        ULONGLONG EventRingSegTableBaseAddr   : 58;
    };
    ULONGLONG AsULONGLONG;
} XHCI_EVENT_RING_TABLE_BASE_ADDR;

typedef volatile union _XHCI_EVENT_RING_DEQUEUE_POINTER 
{ 
    struct 
    {
        ULONGLONG DequeueERSTIndex            : 3;
        ULONGLONG EventHandlerBusy            : 1;
        ULONGLONG EventRingSegDequeuePointer  : 60;
    };
    ULONGLONG AsULONGLONG;
} XHCI_EVENT_RING_DEQUEUE_POINTER;

// Doorbell register
typedef volatile union _XHCI_DOORBELL 
{
    struct 
    {
        ULONG DoorBellTarget        : 8;
        ULONG RsvdZ                 : 8;
        ULONG DoorbellStreamID      : 16;
    };
    ULONG AsULONG;
} XHCI_DOORBELL;

typedef struct _XHCI_HCD_TD {
  /* Hardware*/
  //EHCI_QUEUE_TD HwTD;
  /* Software */
  ULONG PhysicalAddress;
  ULONG TdFlags;
  struct _XHCI_ENDPOINT * XhciEndpoint;
  struct _XHCI_TRANSFER * XhciTransfer;
  struct _XHCI_HCD_TD * NextHcdTD;
  struct _XHCI_HCD_TD * AltNextHcdTD;
  USB_DEFAULT_PIPE_SETUP_PACKET SetupPacket;
  ULONG LengthThisTD;
  LIST_ENTRY DoneLink;
#ifdef _WIN64
  ULONG Pad[31];
#else
  ULONG Pad[40];
#endif
} XHCI_HCD_TD, *PXHCI_HCD_TD;

/* TRB Types (Section 6.4.6) */
#define NORMAL_TRB                      1
#define SETUP_STAGE_TRB                 2
#define DATA_STAGE_TRB                  3
#define STATUS_STAGE_TRB                4
#define ISOCH_TRB                       5
#define LINK_TRB                        6
#define EVENT_DATA_TRB                  7
#define NO_OP_TRB                       8
#define ENABLE_SLOT_COMMAND             9
#define DISABLE_SLOT_COMMAND            10
#define ADDRESS_DEVICE_COMMAND          11
#define CONFIGURE_ENDPOINT_COMMAND      12
#define EVALUATE_CONTEXT_COMMAND        13
#define RESET_ENDPOINT_COMMAND          14
#define STOP_ENDPOINT_COMMAND           15
#define SET_TR_DEQUEUE_POINTER_COMMAND  16
#define RESET_DEVICE_COMMAND            17
#define FORCE_EVENT_COMMAND             18
#define NEGOTIATE_BANDWIDTH_COMMAND     19
#define SET_LATENCY_TOLERANCE_COMMAND   20
#define GET_PORT_BANDWIDTH_COMMAND      21
#define FORCE_HEADER_COMMAND            22
#define NO_OP_COMMAND                   23

/* Event TRB Types */
#define TRANSFER_EVENT                  32
#define COMMAND_COMPLETION_EVENT        33
#define PORT_STATUS_CHANGE_EVENT        34
#define BANDWIDTH_REQUEST_EVENT         35
#define DOORBELL_EVENT                  36
#define HOST_CONTROLLER_EVENT           37
#define DEVICE_NOTIFICATION_EVENT       38
#define MFINDEX_WRAP_EVENT              39

/* Completion Codes (Section 6.4.5) */
#define COMPLETION_INVALID              0
#define COMPLETION_SUCCESS              1
#define COMPLETION_DATA_BUFFER_ERROR    2
#define COMPLETION_BABBLE_DETECTED      3
#define COMPLETION_USB_TRANSACTION_ERROR 4
#define COMPLETION_TRB_ERROR            5
#define COMPLETION_STALL_ERROR          6
#define COMPLETION_RESOURCE_ERROR       7
#define COMPLETION_BANDWIDTH_ERROR      8
#define COMPLETION_NO_SLOTS_ERROR       9
#define COMPLETION_INVALID_STREAM_TYPE  10
#define COMPLETION_SLOT_NOT_ENABLED     11
#define COMPLETION_ENDPOINT_NOT_ENABLED 12
#define COMPLETION_SHORT_PACKET         13
#define COMPLETION_RING_UNDERRUN        14
#define COMPLETION_RING_OVERRUN         15
#define COMPLETION_VF_EVENT_RING_FULL   16
#define COMPLETION_PARAMETER_ERROR      17
#define COMPLETION_BANDWIDTH_OVERRUN    18
#define COMPLETION_CONTEXT_STATE_ERROR  19
#define COMPLETION_NO_PING_RESPONSE     20
#define COMPLETION_EVENT_RING_FULL      21
#define COMPLETION_INCOMPATIBLE_DEVICE  22
#define COMPLETION_MISSED_SERVICE       23
#define COMPLETION_COMMAND_RING_STOPPED 24
#define COMPLETION_COMMAND_ABORTED      25
#define COMPLETION_STOPPED              26
#define COMPLETION_STOPPED_LENGTH_INVALID 27
#define COMPLETION_STOPPED_SHORT_PACKET 28
#define COMPLETION_MAX_EXIT_LATENCY_TOO_LARGE 29
#define COMPLETION_ISOCH_BUFFER_OVERRUN 31
#define COMPLETION_EVENT_LOST           32
#define COMPLETION_UNDEFINED_ERROR      33
#define COMPLETION_INVALID_STREAM_ID    34
#define COMPLETION_SECONDARY_BANDWIDTH_ERROR 35
#define COMPLETION_SPLIT_TRANSACTION_ERROR 36

// Device context and address extraction
#define XHCI_MAX_DEVICE_SLOTS     255
#define XHCI_DEVICE_ADDRESS_MASK  0xFF

// Device context state values
#define XHCI_SLOT_STATE_DISABLED      0
#define XHCI_SLOT_STATE_DEFAULT       1
#define XHCI_SLOT_STATE_ADDRESSED     2
#define XHCI_SLOT_STATE_CONFIGURED    3

