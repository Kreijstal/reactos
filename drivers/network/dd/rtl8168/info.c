/*
 * COPYRIGHT:   See COPYING in the top level directory
 * PROJECT:     ReactOS Realtek 8168/8111 driver
 * FILE:        info.c
 * PURPOSE:     OID query/set handlers
 */

#include "nic.h"

#define NDEBUG
#include <debug.h>

static ULONG SupportedOidList[] =
{
    OID_GEN_SUPPORTED_LIST,
    OID_GEN_HARDWARE_STATUS,
    OID_GEN_MEDIA_SUPPORTED,
    OID_GEN_MEDIA_IN_USE,
    OID_GEN_MAXIMUM_LOOKAHEAD,
    OID_GEN_MAXIMUM_FRAME_SIZE,
    OID_GEN_LINK_SPEED,
    OID_GEN_TRANSMIT_BUFFER_SPACE,
    OID_GEN_RECEIVE_BUFFER_SPACE,
    OID_GEN_RECEIVE_BLOCK_SIZE,
    OID_GEN_TRANSMIT_BLOCK_SIZE,
    OID_GEN_VENDOR_ID,
    OID_GEN_VENDOR_DESCRIPTION,
    OID_GEN_VENDOR_DRIVER_VERSION,
    OID_GEN_CURRENT_PACKET_FILTER,
    OID_GEN_CURRENT_LOOKAHEAD,
    OID_GEN_DRIVER_VERSION,
    OID_GEN_MAXIMUM_TOTAL_SIZE,
    OID_GEN_PROTOCOL_OPTIONS,
    OID_GEN_MAC_OPTIONS,
    OID_GEN_MEDIA_CONNECT_STATUS,
    OID_GEN_MAXIMUM_SEND_PACKETS,
    OID_GEN_XMIT_OK,
    OID_GEN_RCV_OK,
    OID_GEN_XMIT_ERROR,
    OID_GEN_RCV_ERROR,
    OID_GEN_RCV_NO_BUFFER,
    OID_GEN_RCV_CRC_ERROR,
    OID_802_3_PERMANENT_ADDRESS,
    OID_802_3_CURRENT_ADDRESS,
    OID_802_3_MULTICAST_LIST,
    OID_802_3_MAXIMUM_LIST_SIZE,
    OID_802_3_MAC_OPTIONS,
    OID_802_3_RCV_ERROR_ALIGNMENT,
    OID_802_3_XMIT_ONE_COLLISION,
    OID_802_3_XMIT_MORE_COLLISIONS,
    OID_GEN_PHYSICAL_MEDIUM
};

NDIS_STATUS
NTAPI
MiniportQueryInformation (
    IN NDIS_HANDLE MiniportAdapterContext,
    IN NDIS_OID Oid,
    IN PVOID InformationBuffer,
    IN ULONG InformationBufferLength,
    OUT PULONG BytesWritten,
    OUT PULONG BytesNeeded
    )
{
    PRTL_ADAPTER adapter = (PRTL_ADAPTER)MiniportAdapterContext;
    ULONG genericUlong = 0;
    ULONG copyLength = sizeof(ULONG);
    PVOID copySource = &genericUlong;
    NDIS_STATUS status = NDIS_STATUS_SUCCESS;

    NdisAcquireSpinLock(&adapter->Lock);

    switch (Oid)
    {
        case OID_GEN_SUPPORTED_LIST:
            copySource = SupportedOidList;
            copyLength = sizeof(SupportedOidList);
            break;
        case OID_GEN_CURRENT_PACKET_FILTER:
            genericUlong = adapter->PacketFilter;
            break;
        case OID_GEN_HARDWARE_STATUS:
            genericUlong = (ULONG)NdisHardwareStatusReady;
            break;
        case OID_GEN_MEDIA_SUPPORTED:
        case OID_GEN_MEDIA_IN_USE:
        {
            static const NDIS_MEDIUM medium = NdisMedium802_3;
            copySource = (PVOID)&medium;
            copyLength = sizeof(medium);
            break;
        }
        case OID_GEN_RECEIVE_BLOCK_SIZE:
        case OID_GEN_TRANSMIT_BLOCK_SIZE:
        case OID_GEN_CURRENT_LOOKAHEAD:
        case OID_GEN_MAXIMUM_LOOKAHEAD:
        case OID_GEN_MAXIMUM_FRAME_SIZE:
            genericUlong = MAXIMUM_FRAME_SIZE - sizeof(ETH_HEADER);
            break;
        case OID_GEN_LINK_SPEED:
            genericUlong = adapter->LinkSpeedMbps * 10000;  /* OID unit = 100bps */
            break;
        case OID_GEN_TRANSMIT_BUFFER_SPACE:
            genericUlong = MAXIMUM_FRAME_SIZE * TX_DESC_COUNT;
            break;
        case OID_GEN_RECEIVE_BUFFER_SPACE:
            genericUlong = RX_BUF_SIZE * RX_DESC_COUNT;
            break;
        case OID_GEN_VENDOR_ID:
            genericUlong = (adapter->PermanentMacAddress[0] << 16) |
                           (adapter->PermanentMacAddress[1] <<  8) |
                            adapter->PermanentMacAddress[2];
            break;
        case OID_GEN_VENDOR_DESCRIPTION:
        {
            static UCHAR desc[] = "Realtek RTL8168/8111 PCI-E Gigabit Ethernet";
            copySource = desc;
            copyLength = sizeof(desc);
            break;
        }
        case OID_GEN_VENDOR_DRIVER_VERSION:
            genericUlong = DRIVER_VERSION;
            break;
        case OID_GEN_DRIVER_VERSION:
        {
            static const USHORT driverVersion =
                (NDIS_MINIPORT_MAJOR_VERSION << 8) + NDIS_MINIPORT_MINOR_VERSION;
            copySource = (PVOID)&driverVersion;
            copyLength = sizeof(driverVersion);
            break;
        }
        case OID_GEN_MAXIMUM_TOTAL_SIZE:
            genericUlong = MAXIMUM_FRAME_SIZE;
            break;
        case OID_GEN_PROTOCOL_OPTIONS:
            status = NDIS_STATUS_NOT_SUPPORTED;
            break;
        case OID_GEN_MAC_OPTIONS:
            genericUlong = NDIS_MAC_OPTION_RECEIVE_SERIALIZED |
                           NDIS_MAC_OPTION_COPY_LOOKAHEAD_DATA |
                           NDIS_MAC_OPTION_TRANSFERS_NOT_PEND |
                           NDIS_MAC_OPTION_NO_LOOPBACK;
            break;
        case OID_GEN_MEDIA_CONNECT_STATUS:
            genericUlong = adapter->MediaState;
            break;
        case OID_GEN_MAXIMUM_SEND_PACKETS:
            genericUlong = 1;
            break;
        case OID_802_3_CURRENT_ADDRESS:
            copySource = adapter->CurrentMacAddress;
            copyLength = IEEE_802_ADDR_LENGTH;
            break;
        case OID_802_3_PERMANENT_ADDRESS:
            copySource = adapter->PermanentMacAddress;
            copyLength = IEEE_802_ADDR_LENGTH;
            break;
        case OID_802_3_MAXIMUM_LIST_SIZE:
            genericUlong = MAXIMUM_MULTICAST_ADDRESSES;
            break;
        case OID_GEN_PHYSICAL_MEDIUM:
            genericUlong = NdisPhysicalMedium802_3;
            break;

        case OID_GEN_XMIT_OK:                   genericUlong = adapter->TransmitOk; break;
        case OID_GEN_RCV_OK:                    genericUlong = adapter->ReceiveOk; break;
        case OID_GEN_XMIT_ERROR:                genericUlong = adapter->TransmitError; break;
        case OID_GEN_RCV_ERROR:                 genericUlong = adapter->ReceiveError; break;
        case OID_GEN_RCV_NO_BUFFER:             genericUlong = adapter->ReceiveNoBufferSpace; break;
        case OID_GEN_RCV_CRC_ERROR:             genericUlong = adapter->ReceiveCrcError; break;
        /* The 8168 RX descriptor has no alignment-error status bit
         * (bit 27 is MAR, multicast received), so this is always 0. */
        case OID_802_3_RCV_ERROR_ALIGNMENT:     genericUlong = 0; break;
        case OID_802_3_XMIT_ONE_COLLISION:      genericUlong = adapter->TransmitOneCollision; break;
        case OID_802_3_XMIT_MORE_COLLISIONS:    genericUlong = adapter->TransmitMoreCollisions; break;
        default:
            status = NDIS_STATUS_NOT_SUPPORTED;
            break;
    }

    if (status == NDIS_STATUS_SUCCESS)
    {
        if (copyLength > InformationBufferLength)
        {
            *BytesNeeded = copyLength;
            *BytesWritten = 0;
            status = NDIS_STATUS_INVALID_LENGTH;
        }
        else
        {
            NdisMoveMemory(InformationBuffer, copySource, copyLength);
            *BytesWritten = copyLength;
            *BytesNeeded = copyLength;
        }
    }
    else
    {
        *BytesWritten = 0;
        *BytesNeeded = 0;
    }

    NdisReleaseSpinLock(&adapter->Lock);
    return status;
}

NDIS_STATUS
NTAPI
MiniportSetInformation (
    IN NDIS_HANDLE MiniportAdapterContext,
    IN NDIS_OID Oid,
    IN PVOID InformationBuffer,
    IN ULONG InformationBufferLength,
    OUT PULONG BytesRead,
    OUT PULONG BytesNeeded
    )
{
    PRTL_ADAPTER adapter = (PRTL_ADAPTER)MiniportAdapterContext;
    ULONG genericUlong;
    NDIS_STATUS status = NDIS_STATUS_SUCCESS;

    NdisAcquireSpinLock(&adapter->Lock);

    switch (Oid)
    {
        case OID_GEN_CURRENT_PACKET_FILTER:
            if (InformationBufferLength < sizeof(ULONG))
            {
                *BytesRead = 0;
                *BytesNeeded = sizeof(ULONG);
                status = NDIS_STATUS_INVALID_LENGTH;
                break;
            }
            NdisMoveMemory(&genericUlong, InformationBuffer, sizeof(ULONG));
            if (genericUlong &
                (NDIS_PACKET_TYPE_ALL_FUNCTIONAL |
                 NDIS_PACKET_TYPE_FUNCTIONAL |
                 NDIS_PACKET_TYPE_GROUP |
                 NDIS_PACKET_TYPE_MAC_FRAME |
                 NDIS_PACKET_TYPE_SMT |
                 NDIS_PACKET_TYPE_SOURCE_ROUTING))
            {
                *BytesRead = sizeof(ULONG);
                *BytesNeeded = sizeof(ULONG);
                status = NDIS_STATUS_NOT_SUPPORTED;
                break;
            }
            adapter->PacketFilter = genericUlong;
            status = NICApplyPacketFilter(adapter);
            break;

        case OID_GEN_CURRENT_LOOKAHEAD:
            if (InformationBufferLength < sizeof(ULONG))
            {
                *BytesRead = 0;
                *BytesNeeded = sizeof(ULONG);
                status = NDIS_STATUS_INVALID_LENGTH;
                break;
            }
            NdisMoveMemory(&genericUlong, InformationBuffer, sizeof(ULONG));
            if (genericUlong > MAXIMUM_FRAME_SIZE - sizeof(ETH_HEADER))
                status = NDIS_STATUS_INVALID_DATA;
            break;

        case OID_802_3_MULTICAST_LIST:
            if (InformationBufferLength % IEEE_802_ADDR_LENGTH)
            {
                *BytesRead = 0;
                *BytesNeeded = InformationBufferLength +
                               (InformationBufferLength % IEEE_802_ADDR_LENGTH);
                status = NDIS_STATUS_INVALID_LENGTH;
                break;
            }
            if (InformationBufferLength / 6 > MAXIMUM_MULTICAST_ADDRESSES)
            {
                *BytesNeeded = MAXIMUM_MULTICAST_ADDRESSES * IEEE_802_ADDR_LENGTH;
                *BytesRead = 0;
                status = NDIS_STATUS_INVALID_LENGTH;
                break;
            }
            NdisMoveMemory(adapter->MulticastList, InformationBuffer,
                           InformationBufferLength);
            /* TODO: program the actual multicast hash; for now accept-all stays set. */
            break;

        default:
            status = NDIS_STATUS_NOT_SUPPORTED;
            *BytesRead = 0;
            *BytesNeeded = 0;
            break;
    }

    if (status == NDIS_STATUS_SUCCESS)
    {
        *BytesRead = InformationBufferLength;
        *BytesNeeded = 0;
    }

    NdisReleaseSpinLock(&adapter->Lock);
    return status;
}
