/*
 * PROJECT:     ReactOS SD/SDIO/eMMC Driver Stack
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Public SD bus interface definitions
 */

#pragma once

#ifndef _NTDDSD_H_
#define _NTDDSD_H_

#ifdef __cplusplus
extern "C" {
#endif

#ifndef CTL_CODE
#include <winioctl.h>
#endif

#define IOCTL_SD_SUBMIT_REQUEST \
    CTL_CODE(FILE_DEVICE_CONTROLLER, 0x0700, METHOD_NEITHER, FILE_ANY_ACCESS)

#define SDBUS_INTERFACE_VERSION 1
#define SDBUS_DRIVER_VERSION_4  4

typedef enum _SD_COMMAND_CLASS
{
    SDCC_STANDARD = 0,
    SDCC_APP_CMD = 1
} SD_COMMAND_CLASS;

typedef enum _SD_RESPONSE_TYPE
{
    SDRT_NONE = 0,
    SDRT_1,
    SDRT_1B,
    SDRT_2,
    SDRT_3,
    SDRT_4,
    SDRT_5,
    SDRT_5B,
    SDRT_6
} SD_RESPONSE_TYPE;

typedef enum _SD_TRANSFER_DIRECTION
{
    SDTD_READ = 0,
    SDTD_WRITE = 1
} SD_TRANSFER_DIRECTION;

typedef enum _SD_TRANSFER_TYPE
{
    SDTT_CMD_ONLY = 0,
    SDTT_SINGLE_BLOCK,
    SDTT_MULTI_BLOCK,
    SDTT_MULTI_BLOCK_NO_CMD12
} SD_TRANSFER_TYPE;

typedef struct _SDCMD_DESCRIPTOR
{
    UCHAR Cmd;
    UCHAR CmdClass;
    UCHAR TransferDirection;
    UCHAR TransferType;
    SD_RESPONSE_TYPE ResponseType;
} SDCMD_DESCRIPTOR, *PSDCMD_DESCRIPTOR;

#define SDCMD_GO_IDLE_STATE         0
#define SDCMD_SEND_OP_COND          1
#define SDCMD_ALL_SEND_CID          2
#define SDCMD_SEND_RELATIVE_ADDR    3
#define SDCMD_IO_SEND_OP_COND       5
#define SDCMD_SWITCH_FUNC           6
#define SDCMD_SELECT_CARD           7
#define SDCMD_SEND_IF_COND          8
#define SDCMD_SEND_CSD              9
#define SDCMD_SEND_CID              10
#define SDCMD_SEND_STATUS           13
#define SDCMD_GO_INACTIVE_STATE     15
#define SDCMD_SET_BLOCKLEN          16
#define SDCMD_READ_SINGLE_BLOCK     17
#define SDCMD_READ_MULTIPLE_BLOCK   18
#define SDCMD_WRITE_BLOCK           24
#define SDCMD_WRITE_MULTIPLE_BLOCK  25
#define SDCMD_ERASE_WR_BLK_START    32
#define SDCMD_ERASE_WR_BLK_END      33
#define SDCMD_ERASE                 38
#define SDCMD_IO_RW_DIRECT          52
#define SDCMD_IO_RW_EXTENDED        53
#define SDCMD_APP_CMD               55
#define SDCMD_VOLTAGE_SWITCH        11

#define SDACMD_SET_BUS_WIDTH        6
#define SDACMD_SD_SEND_OP_COND      41
#define SDACMD_SEND_SCR             51

typedef enum _SDPROP_MEDIA_STATE
{
    SDPMS_NO_MEDIA = 0,
    SDPMS_MEDIA_INSERTED = 1
} SDPROP_MEDIA_STATE, *PSDPROP_MEDIA_STATE;

typedef enum _SDBUS_PROPERTY
{
    SDP_MEDIA_CHANGECOUNT = 0,
    SDP_MEDIA_STATE,
    SDP_WRITE_PROTECTED,
    SDP_FUNCTION_NUMBER,
    SDP_FUNCTION_TYPE,
    SDP_BUS_DRIVER_VERSION,
    SDP_BUS_WIDTH,
    SDP_BUS_CLOCK,
    SDP_HOST_BLOCK_LENGTH,
    SDP_HIGH_CAPACITY_SUPPORTED,
    SDP_TOTAL_SECTORS,
    SDP_SET_CARD_INTERRUPT_FORWARD,
    SDP_ROS_CARD_TYPE
} SDBUS_PROPERTY, *PSDBUS_PROPERTY;

typedef enum _SDBUS_FUNCTION_TYPE
{
    SDBUS_FUNCTION_TYPE_UNKNOWN = 0,
    SDBUS_FUNCTION_TYPE_SD_MEMORY,
    SDBUS_FUNCTION_TYPE_SDIO,
    SDBUS_FUNCTION_TYPE_MMC_MEMORY
} SDBUS_FUNCTION_TYPE, *PSDBUS_FUNCTION_TYPE;

typedef enum _SDBUS_REQUEST_FUNCTION
{
    SDRF_GET_PROPERTY = 0,
    SDRF_SET_PROPERTY,
    SDRF_DEVICE_COMMAND,
    SDRF_MMC_SOFT_RESET,
    SDRF_IO_RW_DIRECT,
    SDRF_IO_RW_EXTENDED,
    SDRF_EMMC_SWITCH,
    SDRF_EMMC_SELECT_PARTITION,
    SDRF_EMMC_SANITIZE
} SDBUS_REQUEST_FUNCTION;

typedef union _SDBUS_RESPONSE_DATA
{
    UCHAR AsUCHAR[16];
    ULONG AsULONG[4];
} SDBUS_RESPONSE_DATA, *PSDBUS_RESPONSE_DATA;

typedef struct _SDBUS_REQUEST_PACKET
{
    ULONG RequestFunction;
    ULONG Reserved;
    union
    {
        struct
        {
            SDBUS_PROPERTY Property;
            PVOID Buffer;
            ULONG Length;
        } GetSetProperty;

        struct
        {
            SDCMD_DESCRIPTOR CmdDesc;
            ULONG Argument;
            PMDL Mdl;
            ULONG Length;
        } DeviceCommand;

        struct
        {
            UCHAR Function;
            BOOLEAN Write;
            BOOLEAN RawMode;
            ULONG Address;
            UCHAR DataIn;
            UCHAR DataOut;
        } IoDirect;

        struct
        {
            UCHAR Function;
            BOOLEAN Write;
            BOOLEAN BlockMode;
            BOOLEAN Increment;
            ULONG Address;
            ULONG BlockCount;
            ULONG BlockSize;
            PMDL Mdl;
        } IoExtended;

        struct
        {
            UCHAR Access;
            UCHAR Index;
            UCHAR Value;
            UCHAR CmdSet;
        } EmmcSwitch;

        struct
        {
            UCHAR PartitionId;
        } EmmcSelectPartition;
    } Parameters;
    SDBUS_RESPONSE_DATA ResponseData;
    ULONG ResponseLength;
    ULONG_PTR Information;
} SDBUS_REQUEST_PACKET, *PSDBUS_REQUEST_PACKET;

typedef VOID
(NTAPI *PSDBUS_CALLBACK_ROUTINE)(
    _In_opt_ PVOID Context,
    _In_ ULONG InterruptType);

typedef struct _SDBUS_INTERFACE_PARAMETERS
{
    USHORT Size;
    USHORT Version;
    PSDBUS_CALLBACK_ROUTINE CallbackRoutine;
    PVOID CallbackRoutineContext;
    BOOLEAN DeviceGeneratesInterrupts;
} SDBUS_INTERFACE_PARAMETERS, *PSDBUS_INTERFACE_PARAMETERS;

typedef NTSTATUS
(NTAPI *PSDBUS_INITIALIZE_INTERFACE)(
    _In_ PVOID Context,
    _In_ PSDBUS_INTERFACE_PARAMETERS InterfaceParameters);

typedef NTSTATUS
(NTAPI *PSDBUS_ACKNOWLEDGE_INTERRUPT)(
    _In_ PVOID Context);

typedef struct _SDBUS_INTERFACE_STANDARD
{
    USHORT Size;
    USHORT Version;
    PVOID Context;
    PINTERFACE_REFERENCE InterfaceReference;
    PINTERFACE_DEREFERENCE InterfaceDereference;
    PSDBUS_INITIALIZE_INTERFACE InitializeInterface;
    PSDBUS_ACKNOWLEDGE_INTERRUPT AcknowledgeInterrupt;
} SDBUS_INTERFACE_STANDARD, *PSDBUS_INTERFACE_STANDARD;

#ifdef __cplusplus
}
#endif

#endif /* _NTDDSD_H_ */
