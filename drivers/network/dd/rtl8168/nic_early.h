/*
 * PROJECT:     ReactOS RTL8168/8111 driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     NDIS-free compatibility surface for the early KDNET transport
 */

#pragma once

#include <ntifs.h>

#include "rtlhw.h"

typedef NTSTATUS NDIS_STATUS;
typedef unsigned int UINT;

#define MAX_RESET_ATTEMPTS              100

#define NDIS_STATUS_SUCCESS             STATUS_SUCCESS
#define NDIS_STATUS_FAILURE             STATUS_UNSUCCESSFUL
#define NDIS_STATUS_ADAPTER_NOT_FOUND   STATUS_DEVICE_DOES_NOT_EXIST

#define NdisMediaStateConnected         0
#define NdisMediaStateDisconnected      1

#define NDIS_PACKET_TYPE_DIRECTED       0x00000001
#define NDIS_PACKET_TYPE_MULTICAST      0x00000002
#define NDIS_PACKET_TYPE_ALL_MULTICAST  0x00000004
#define NDIS_PACKET_TYPE_BROADCAST      0x00000008
#define NDIS_PACKET_TYPE_PROMISCUOUS    0x00000020

#define MIN_TRACE 0
#define MID_TRACE 0
#define MAX_TRACE 0
#define NDIS_DbgPrint(Level, Arguments) ((void)0)
#define NdisStallExecution(Microseconds) KeStallExecutionProcessor(Microseconds)
#define NdisMSleep(Microseconds) KeStallExecutionProcessor(Microseconds)
#define NdisZeroMemory(Destination, Length) RtlZeroMemory((Destination), (Length))

typedef struct _RTL_ADAPTER
{
    PUCHAR IoBase;
    BOOLEAN IoBaseIsMmio;

    USHORT InterruptMask;

    UCHAR PermanentMacAddress[IEEE_802_ADDR_LENGTH];
    UCHAR CurrentMacAddress[IEEE_802_ADDR_LENGTH];
    struct
    {
        UCHAR MacAddress[IEEE_802_ADDR_LENGTH];
    } MulticastList[MAXIMUM_MULTICAST_ADDRESSES];

    ULONG TxConfigRaw;
    RTL_MAC_VER MacVersion;
    BOOLEAN PhyIsGigabit;
    BOOLEAN OcpMdioRedirect;
    USHORT OcpBase;

    PRTL_DESC TxRing;
    PHYSICAL_ADDRESS TxRingPa;
    PUCHAR TxBuffers;
    PHYSICAL_ADDRESS TxBuffersPa;
    ULONG TxProducer;
    ULONG TxConsumer;
    BOOLEAN TxFull;

    PRTL_DESC RxRing;
    PHYSICAL_ADDRESS RxRingPa;
    PUCHAR RxBuffers;
    PHYSICAL_ADDRESS RxBuffersPa;
    ULONG RxConsumer;

    ULONG LinkSpeedMbps;
    ULONG MediaState;
    BOOLEAN LinkChange;
    UCHAR LastPhyStatus;

    ULONG PacketFilter;

    BOOLEAN HwHang;
    BOOLEAN CheckForHangTxPending;
    ULONG CheckForHangTxOk;

    ULONG ReceiveOk;
    ULONG TransmitOk;
    ULONG ReceiveError;
    ULONG TransmitError;
    ULONG ReceiveNoBufferSpace;
    ULONG ReceiveCrcError;
    ULONG TransmitOneCollision;
    ULONG TransmitMoreCollisions;
} RTL_ADAPTER, *PRTL_ADAPTER;

FORCEINLINE
UCHAR
RtlReadReg8(_In_ PRTL_ADAPTER Adapter, _In_ ULONG Offset)
{
    if (Adapter->IoBaseIsMmio)
        return READ_REGISTER_UCHAR(Adapter->IoBase + Offset);
    return READ_PORT_UCHAR(Adapter->IoBase + Offset);
}

FORCEINLINE
USHORT
RtlReadReg16(_In_ PRTL_ADAPTER Adapter, _In_ ULONG Offset)
{
    if (Adapter->IoBaseIsMmio)
        return READ_REGISTER_USHORT((PUSHORT)(Adapter->IoBase + Offset));
    return READ_PORT_USHORT((PUSHORT)(Adapter->IoBase + Offset));
}

FORCEINLINE
ULONG
RtlReadReg32(_In_ PRTL_ADAPTER Adapter, _In_ ULONG Offset)
{
    if (Adapter->IoBaseIsMmio)
        return READ_REGISTER_ULONG((PULONG)(Adapter->IoBase + Offset));
    return READ_PORT_ULONG((PULONG)(Adapter->IoBase + Offset));
}

FORCEINLINE
VOID
RtlWriteReg8(_In_ PRTL_ADAPTER Adapter, _In_ ULONG Offset, _In_ UCHAR Value)
{
    if (Adapter->IoBaseIsMmio)
        WRITE_REGISTER_UCHAR(Adapter->IoBase + Offset, Value);
    else
        WRITE_PORT_UCHAR(Adapter->IoBase + Offset, Value);
}

FORCEINLINE
VOID
RtlWriteReg16(_In_ PRTL_ADAPTER Adapter, _In_ ULONG Offset, _In_ USHORT Value)
{
    if (Adapter->IoBaseIsMmio)
        WRITE_REGISTER_USHORT((PUSHORT)(Adapter->IoBase + Offset), Value);
    else
        WRITE_PORT_USHORT((PUSHORT)(Adapter->IoBase + Offset), Value);
}

FORCEINLINE
VOID
RtlWriteReg32(_In_ PRTL_ADAPTER Adapter, _In_ ULONG Offset, _In_ ULONG Value)
{
    if (Adapter->IoBaseIsMmio)
        WRITE_REGISTER_ULONG((PULONG)(Adapter->IoBase + Offset), Value);
    else
        WRITE_PORT_ULONG((PULONG)(Adapter->IoBase + Offset), Value);
}

VOID NTAPI RtlEriWrite(PRTL_ADAPTER Adapter, ULONG Address, ULONG Mask, ULONG Value);
ULONG NTAPI RtlEriRead(PRTL_ADAPTER Adapter, ULONG Address);
VOID NTAPI RtlEriSetBits(PRTL_ADAPTER Adapter, ULONG Address, ULONG Bits);
VOID NTAPI RtlEriClearBits(PRTL_ADAPTER Adapter, ULONG Address, ULONG Bits);
VOID NTAPI RtlW0w1Eri(PRTL_ADAPTER Adapter, ULONG Address, ULONG Set, ULONG Clear);
VOID NTAPI RtlEphyWrite(PRTL_ADAPTER Adapter, ULONG Register, USHORT Value);
USHORT NTAPI RtlEphyRead(PRTL_ADAPTER Adapter, ULONG Register);

typedef struct _EPHY_INFO
{
    ULONG Offset;
    USHORT Mask;
    USHORT Bits;
} EPHY_INFO, *PEPHY_INFO;

VOID NTAPI RtlEphyInit(PRTL_ADAPTER Adapter, const EPHY_INFO *Table, ULONG Count);
VOID NTAPI RtlPhyOcpWrite(PRTL_ADAPTER Adapter, ULONG Register, ULONG Data);
USHORT NTAPI RtlPhyOcpRead(PRTL_ADAPTER Adapter, ULONG Register);
VOID NTAPI RtlMacOcpWrite(PRTL_ADAPTER Adapter, ULONG Register, ULONG Data);
USHORT NTAPI RtlMacOcpRead(PRTL_ADAPTER Adapter, ULONG Register);
VOID NTAPI RtlMacOcpModify(PRTL_ADAPTER Adapter, ULONG Register, USHORT Mask, USHORT Set);
VOID NTAPI RtlMdioWrite(PRTL_ADAPTER Adapter, ULONG Register, USHORT Value);
USHORT NTAPI RtlMdioRead(PRTL_ADAPTER Adapter, ULONG Register);
VOID NTAPI RtlPhyWritePaged(PRTL_ADAPTER Adapter, USHORT Page, ULONG Register, USHORT Value);
USHORT NTAPI RtlPhyReadPaged(PRTL_ADAPTER Adapter, USHORT Page, ULONG Register);
VOID NTAPI RtlPhyModifyPaged(PRTL_ADAPTER Adapter, USHORT Page, ULONG Register,
                             USHORT Mask, USHORT Set);

VOID NTAPI RtlHwStartChipSpecific(PRTL_ADAPTER Adapter);
VOID NTAPI RtlHwAspmClkReqDisable(PRTL_ADAPTER Adapter);
VOID NTAPI RtlEnableExitL1(PRTL_ADAPTER Adapter);
VOID NTAPI RtlHwPhyConfig(PRTL_ADAPTER Adapter);
VOID NTAPI RtlPllPowerUp(PRTL_ADAPTER Adapter);
VOID NTAPI RtlPllPowerDown(PRTL_ADAPTER Adapter);
NDIS_STATUS NTAPI NICSoftReset(PRTL_ADAPTER Adapter);
VOID NTAPI NICInitializeHw(PRTL_ADAPTER Adapter);
VOID NTAPI Rtl8168EpStopCmac(PRTL_ADAPTER Adapter);
NDIS_STATUS NTAPI NICDetectChipVersion(PRTL_ADAPTER Adapter);
NDIS_STATUS NTAPI NICGetPermanentMacAddress(PRTL_ADAPTER Adapter, PUCHAR MacAddress);
NDIS_STATUS NTAPI NICProgramRings(PRTL_ADAPTER Adapter);
NDIS_STATUS NTAPI NICEnableTxRx(PRTL_ADAPTER Adapter);
NDIS_STATUS NTAPI NICApplyPacketFilter(PRTL_ADAPTER Adapter);
NDIS_STATUS NTAPI NICApplyInterruptMask(PRTL_ADAPTER Adapter);
NDIS_STATUS NTAPI NICDisableInterrupts(PRTL_ADAPTER Adapter);
USHORT NTAPI NICInterruptRecognized(PRTL_ADAPTER Adapter, PBOOLEAN Recognized);
VOID NTAPI NICAcknowledgeInterrupts(PRTL_ADAPTER Adapter, USHORT Status);
VOID NTAPI NICUpdateLinkStatus(PRTL_ADAPTER Adapter);
NDIS_STATUS NTAPI NICTransmitDescriptor(PRTL_ADAPTER Adapter, ULONG Index,
                                        PHYSICAL_ADDRESS BufferPa, ULONG Length);
NDIS_STATUS NTAPI NICRefillRxDescriptor(PRTL_ADAPTER Adapter, ULONG Index);
