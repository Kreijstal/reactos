/*
 * PROJECT:     ReactOS Intel Wireless (iwlwifi) Driver
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     Firmware RX completion to Native Wi-Fi indication path.
 */

#include "iwlwifi.h"

#define NDEBUG
#include <debug.h>

#pragma pack(push, 1)
typedef struct _IWL_RX_COMPLETION_DESC_LOCAL
{
    ULONG Reserved;
    USHORT Rbid;
    UCHAR Flags;
    UCHAR Reserved2[25];
} IWL_RX_COMPLETION_DESC_LOCAL;

typedef struct _IWL_RX_PACKET_LOCAL
{
    ULONG LengthAndFlags;
    UCHAR Command, Group;
    USHORT Sequence;
    UCHAR Data[1];
} IWL_RX_PACKET_LOCAL;
#pragma pack(pop)

VOID NTAPI
IwlRxPollTimerDpc(_In_ PVOID SystemSpecific1, _In_ PVOID FunctionContext,
                  _In_ PVOID SystemSpecific2, _In_ PVOID SystemSpecific3)
{
    PIWL_ADAPTER Adapter = FunctionContext;
    UNREFERENCED_PARAMETER(SystemSpecific1);
    UNREFERENCED_PARAMETER(SystemSpecific2);
    UNREFERENCED_PARAMETER(SystemSpecific3);
    if (Adapter != NULL &&
        InterlockedCompareExchange(&Adapter->DataPathReady, 0, 0))
        IwlProcessReceiveCompletions(Adapter, TRUE);
}

VOID
IwlProcessReceiveCompletions(_In_ PIWL_ADAPTER Adapter,
                             _In_ BOOLEAN AtDispatchLevel)
{
    IWL_RX_COMPLETION_DESC_LOCAL *Completion;
    USHORT Producer, Cursor;

    if (Adapter->RxNblPool == NULL || Adapter->RxStatus.VirtualAddress == NULL)
        return;
    Producer = *(volatile USHORT *)Adapter->RxStatus.VirtualAddress;
    Completion = (IWL_RX_COMPLETION_DESC_LOCAL *)
        Adapter->RxCompletionRing.VirtualAddress;
    for (Cursor = Adapter->RxReadIndex; Cursor != Producer; Cursor++)
    {
        USHORT Rbid = Completion[Cursor &
            (IWL_GEN3_RX_QUEUE_SIZE - 1)].Rbid;
        IWL_RX_PACKET_LOCAL *Packet;
        ULONG PacketLength, MpduLength;

        if (Rbid == 0 || Rbid > Adapter->RxBufferCount)
        {
            Adapter->RxReadIndex = Cursor + 1;
            continue;
        }
        /* Publish consumption before indicating: the supplicant may issue a
         * synchronous key OID from inside this receive callback, and that
         * command must begin at the following firmware completion. */
        Adapter->RxReadIndex = Cursor + 1;
        Packet = (IWL_RX_PACKET_LOCAL *)
            Adapter->RxBuffers[Rbid - 1].VirtualAddress;
        PacketLength = Packet->LengthAndFlags & 0x3fff;
        if (Packet->Command == 0xc1 && Packet->Group == 0 &&
            PacketLength >= 4 + 64)
        {
            PUCHAR Frame = Packet->Data + 64;
            ULONG RxStatus = *(UNALIGNED ULONG *)(Packet->Data + 12);
            UCHAR MacFlags1 = Packet->Data[2];
            UCHAR MacFlags2 = Packet->Data[3];
            MpduLength = *(UNALIGNED USHORT *)Packet->Data;
            if (MpduLength >= 24 &&
                MpduLength <= IWL_GEN3_RX_BUFFER_SIZE - 8 - 64 &&
                PacketLength >= 4 + 64 + MpduLength)
            {
                /* AX210 RX_MPDU v5 leaves the CCMP header and MIC in the
                 * returned MPDU even after hardware decryption.  Native Wi-Fi
                 * expects a normal plaintext 802.11 frame here, so remove the
                 * crypto framing and clear Protected after validating the
                 * firmware's key/MIC/decrypted status bits. */
                if ((Frame[1] & 0x40) &&
                    ((((RxStatus & 0x700) == 0x200) &&
                      (RxStatus & (0x08 | 0x40 | 0x800)) ==
                          (0x08 | 0x40 | 0x800)) ||
                     (((RxStatus & 0x700) == 0x300) &&
                      (RxStatus & (0x08 | 0x20 | 0x800)) ==
                          (0x08 | 0x20 | 0x800))))
                {
                    /* mac_flags2 header length includes the 8-byte CCMP/TKIP
                     * IV on protected frames (32 rather than the plaintext
                     * 24-byte non-QoS MAC header). */
                    ULONG HeaderLength = (MacFlags2 & 0x1f) * 2 - 8;
                    ULONG PaddingLength = (MacFlags2 & 0x20) ? 2 : 0;
                    ULONG MicLength = (MacFlags1 & 0xf0) >> 3;
                    ULONG CryptoLength = 8 + PaddingLength;

                    if (HeaderLength >= 24 &&
                        HeaderLength + CryptoLength + MicLength <= MpduLength)
                    {
                        RtlMoveMemory(Frame + HeaderLength,
                                      Frame + HeaderLength + CryptoLength,
                                      MpduLength - HeaderLength -
                                          CryptoLength - MicLength);
                        MpduLength -= CryptoLength + MicLength;
                        Frame[1] &= ~0x40;
                    }
                }
                PMDL Mdl = NdisAllocateMdl(Adapter->MiniportAdapterHandle,
                                            Frame, MpduLength);
                if (Mdl != NULL)
                {
                    PNET_BUFFER_LIST Nbl =
                        NdisAllocateNetBufferAndNetBufferList(
                            Adapter->RxNblPool, 0, 0, Mdl, 0, MpduLength);
                    if (Nbl != NULL)
                    {
                        DOT11_EXTSTA_RECV_CONTEXT Context;
                        NdisZeroMemory(&Context, sizeof(Context));
                        Context.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
                        Context.Header.Revision =
                            DOT11_EXTSTA_RECV_CONTEXT_REVISION_1;
                        Context.Header.Size = sizeof(Context);
                        Context.uReceiveFlags = DOT11_RECV_FLAG_RAW_PACKET;
                        Context.usNumberOfMPDUsReceived = 1;
                        NET_BUFFER_LIST_INFO(Nbl, MediaSpecificInformation) =
                            &Context;
                        Nbl->SourceHandle = Adapter->MiniportAdapterHandle;
                        NdisMIndicateReceiveNetBufferLists(
                            Adapter->MiniportAdapterHandle, Nbl,
                            NDIS_DEFAULT_PORT_NUMBER, 1,
                            NDIS_RECEIVE_FLAGS_RESOURCES |
                            (AtDispatchLevel ?
                             NDIS_RECEIVE_FLAGS_DISPATCH_LEVEL : 0));
                        NdisFreeNetBufferList(Nbl);
                    }
                    NdisFreeMdl(Mdl);
                }
            }
        }
        IwlRecycleRxBuffer(Adapter, Rbid);
    }
}

VOID NTAPI
IwlReturnNetBufferLists(
    _In_ NDIS_HANDLE MiniportAdapterContext,
    _In_ PNET_BUFFER_LIST NetBufferLists,
    _In_ ULONG ReturnFlags)
{
    UNREFERENCED_PARAMETER(MiniportAdapterContext);
    UNREFERENCED_PARAMETER(ReturnFlags);

    /* Every indication carries RESOURCES and is reclaimed by the indication
     * site after NdisMIndicateReceiveNetBufferLists returns.  ReactOS NDIS
     * still calls this handler on that synchronous path, so there is no
     * ownership transfer or work to perform here. */
    UNREFERENCED_PARAMETER(NetBufferLists);
}
