/*
 * COPYRIGHT:   See COPYING in the top level directory
 * PROJECT:     ReactOS Realtek 8168/8111 driver
 * FILE:        ndis.c
 * PURPOSE:     DriverEntry, MiniportInitialize, MiniportHalt, MiniportSend,
 *              MiniportReset.  NDIS5 legacy miniport surface.
 */

#include "nic.h"

#define NDEBUG
#include <debug.h>

ULONG DebugTraceLevel = MIN_TRACE;

#define TX_RING_BYTES   (TX_DESC_COUNT * sizeof(RTL_DESC))
#define RX_RING_BYTES   (RX_DESC_COUNT * sizeof(RTL_DESC))
#define TX_POOL_BYTES   (TX_DESC_COUNT * RX_BUF_SIZE)
#define RX_POOL_BYTES   (RX_DESC_COUNT * RX_BUF_SIZE)

NDIS_STATUS
NTAPI
MiniportReset (
    OUT PBOOLEAN AddressingReset,
    IN NDIS_HANDLE MiniportAdapterContext
    )
{
    PRTL_ADAPTER adapter = (PRTL_ADAPTER)MiniportAdapterContext;
    NDIS_STATUS status;

    NdisAcquireSpinLock(&adapter->Lock);
    NICDisableInterrupts(adapter);
    status = NICSoftReset(adapter);
    if (status == NDIS_STATUS_SUCCESS)
    {
        NICProgramRings(adapter);
        NICEnableTxRx(adapter);
        NICApplyInterruptMask(adapter);
    }
    NdisReleaseSpinLock(&adapter->Lock);

    *AddressingReset = FALSE;
    return status;
}

NDIS_STATUS
NTAPI
MiniportSend (
    IN NDIS_HANDLE MiniportAdapterContext,
    IN PNDIS_PACKET Packet,
    IN UINT Flags
    )
{
    PRTL_ADAPTER adapter = (PRTL_ADAPTER)MiniportAdapterContext;
    PNDIS_BUFFER firstBuffer;
    PVOID firstBufferVa;
    UINT firstBufferLength, totalBufferLength;
    PUCHAR slotVa;
    PHYSICAL_ADDRESS slotPa;
    ULONG idx;

    UNREFERENCED_PARAMETER(Flags);

    NdisGetFirstBufferFromPacketSafe(Packet,
                                     &firstBuffer,
                                     &firstBufferVa,
                                     &firstBufferLength,
                                     &totalBufferLength,
                                     NormalPagePriority);
    if (firstBufferVa == NULL || totalBufferLength == 0)
        return NDIS_STATUS_FAILURE;

    if (totalBufferLength > MAXIMUM_FRAME_SIZE)
    {
        NDIS_DbgPrint(MIN_TRACE,
            ("RTL8168: oversize packet (%u)\n", totalBufferLength));
        return NDIS_STATUS_FAILURE;
    }

    NdisAcquireSpinLock(&adapter->Lock);

    if (adapter->TxFull)
    {
        NdisReleaseSpinLock(&adapter->Lock);
        return NDIS_STATUS_RESOURCES;
    }

    idx = adapter->TxProducer;
    slotVa = adapter->TxBuffers + (idx * RX_BUF_SIZE);
    slotPa.QuadPart = adapter->TxBuffersPa.QuadPart + (ULONGLONG)idx * RX_BUF_SIZE;

    /* Per-packet TX checksum offload bits.  NDIS passes the request via the
     * TcpIpChecksumPacketInfo per-packet entry; bit layout is:
     *   V4.IpChecksum   -> TXD2_IPv4_CS
     *   V4.TcpChecksum  -> TXD2_TCP_CS
     *   V4.UdpChecksum  -> TXD2_UDP_CS
     * If the protocol didn't request offload (or we negotiated it off) the
     * field is zero and the chip emits the frame as-is. */
    {
        PNDIS_TCP_IP_CHECKSUM_PACKET_INFO csum =
            (PNDIS_TCP_IP_CHECKSUM_PACKET_INFO)
                &NDIS_PER_PACKET_INFO_FROM_PACKET(Packet, TcpIpChecksumPacketInfo);
        adapter->LastTxOpts2 = 0;
        if (csum->Value)
        {
            if ((adapter->TaskOffloadFlags & TASK_OFFLOAD_IPCS) &&
                csum->Transmit.NdisPacketIpChecksum)
                adapter->LastTxOpts2 |= TXD2_IPv4_CS;
            if ((adapter->TaskOffloadFlags & TASK_OFFLOAD_TCPCS) &&
                csum->Transmit.NdisPacketTcpChecksum)
                adapter->LastTxOpts2 |= TXD2_TCP_CS;
            if ((adapter->TaskOffloadFlags & TASK_OFFLOAD_UDPCS) &&
                csum->Transmit.NdisPacketUdpChecksum)
                adapter->LastTxOpts2 |= TXD2_UDP_CS;
        }
    }

    /* Copy each NDIS buffer fragment into our bounce slot -- avoids the SG path
     * which on NDIS5 hands us PHYSICAL_ADDRESS lists that may be 64-bit while
     * older 8168 chips only accept 32-bit DMA in non-DAC mode. */
    {
        PNDIS_BUFFER nb = firstBuffer;
        PUCHAR dst = slotVa;
        ULONG copied = 0;

        while (nb != NULL)
        {
            PVOID va;
            UINT len;
            PNDIS_BUFFER next;

            NdisQueryBufferSafe(nb, &va, &len, NormalPagePriority);
            if (va == NULL)
            {
                NdisReleaseSpinLock(&adapter->Lock);
                return NDIS_STATUS_FAILURE;
            }
            if (copied + len > RX_BUF_SIZE)
                len = RX_BUF_SIZE - copied;

            NdisMoveMemory(dst + copied, va, len);
            copied += len;

            if (copied >= totalBufferLength)
                break;

            NdisGetNextBuffer(nb, &next);
            nb = next;
        }

        /* Pad runts. */
        if (totalBufferLength < MINIMUM_FRAME_SIZE)
        {
            NdisZeroMemory(dst + totalBufferLength,
                           MINIMUM_FRAME_SIZE - totalBufferLength);
            totalBufferLength = MINIMUM_FRAME_SIZE;
        }
    }

    NICTransmitDescriptor(adapter, idx, slotPa, totalBufferLength,
                          adapter->LastTxOpts2);

    adapter->TxProducer = (idx + 1) % TX_DESC_COUNT;
    if (adapter->TxProducer == adapter->TxConsumer)
        adapter->TxFull = TRUE;

    NdisReleaseSpinLock(&adapter->Lock);
    return NDIS_STATUS_SUCCESS;
}

VOID
NTAPI
MiniportHalt (
    IN NDIS_HANDLE MiniportAdapterContext
    )
{
    PRTL_ADAPTER adapter = (PRTL_ADAPTER)MiniportAdapterContext;

    ASSERT(adapter != NULL);

    if (adapter->InterruptRegistered)
    {
        NdisMDeregisterInterrupt(&adapter->Interrupt);
        adapter->InterruptRegistered = FALSE;
    }

    if (adapter->IoBase)
    {
        NICDisableInterrupts(adapter);
        /* Stop RX/TX. */
        RtlWriteReg8(adapter, R_CMD, B_CMD_STOPREQ);

        /* Drop the PLL so D3 entry leaves the chip in its lowest-power state. */
        RtlPllPowerDown(adapter);

        if (adapter->IoBaseIsMmio)
        {
            NdisMUnmapIoSpace(adapter->MiniportAdapterHandle,
                              adapter->IoBase,
                              adapter->MmioLength);
        }
        else
        {
            NdisMDeregisterIoPortRange(adapter->MiniportAdapterHandle,
                                       adapter->IoPortStart,
                                       adapter->IoPortLength,
                                       adapter->IoBase);
        }
        adapter->IoBase = NULL;
    }

    if (adapter->TxRing)
    {
        NdisMFreeSharedMemory(adapter->MiniportAdapterHandle,
                              TX_RING_BYTES, FALSE,
                              adapter->TxRing, adapter->TxRingPa);
    }
    if (adapter->RxRing)
    {
        NdisMFreeSharedMemory(adapter->MiniportAdapterHandle,
                              RX_RING_BYTES, FALSE,
                              adapter->RxRing, adapter->RxRingPa);
    }
    if (adapter->TxBuffers)
    {
        NdisMFreeSharedMemory(adapter->MiniportAdapterHandle,
                              TX_POOL_BYTES, FALSE,
                              adapter->TxBuffers, adapter->TxBuffersPa);
    }
    if (adapter->RxBuffers)
    {
        NdisMFreeSharedMemory(adapter->MiniportAdapterHandle,
                              RX_POOL_BYTES, FALSE,
                              adapter->RxBuffers, adapter->RxBuffersPa);
    }

    NdisFreeMemory(adapter, sizeof(*adapter), 0);
}

static NDIS_STATUS
RtlAllocateSharedRing (
    IN PRTL_ADAPTER Adapter,
    IN ULONG Size,
    OUT PVOID *VaOut,
    OUT PNDIS_PHYSICAL_ADDRESS PaOut
    )
{
    NdisMAllocateSharedMemory(Adapter->MiniportAdapterHandle,
                              Size, FALSE, VaOut, PaOut);
    if (*VaOut == NULL)
    {
        NDIS_DbgPrint(MIN_TRACE,
            ("RTL8168: NdisMAllocateSharedMemory(%lu) failed\n", Size));
        return NDIS_STATUS_RESOURCES;
    }
    NdisZeroMemory(*VaOut, Size);
    return NDIS_STATUS_SUCCESS;
}

NDIS_STATUS
NTAPI
MiniportInitialize (
    OUT PNDIS_STATUS OpenErrorStatus,
    OUT PUINT SelectedMediumIndex,
    IN PNDIS_MEDIUM MediumArray,
    IN UINT MediumArraySize,
    IN NDIS_HANDLE MiniportAdapterHandle,
    IN NDIS_HANDLE WrapperConfigurationContext
    )
{
    PRTL_ADAPTER adapter = NULL;
    PNDIS_RESOURCE_LIST resourceList = NULL;
    UINT resourceListSize = 0;
    NDIS_STATUS status;
    UINT i;

    UNREFERENCED_PARAMETER(OpenErrorStatus);

    for (i = 0; i < MediumArraySize; i++)
    {
        if (MediumArray[i] == NdisMedium802_3)
        {
            *SelectedMediumIndex = i;
            break;
        }
    }
    if (i == MediumArraySize)
        return NDIS_STATUS_UNSUPPORTED_MEDIA;

    status = NdisAllocateMemoryWithTag((PVOID*)&adapter, sizeof(*adapter), ADAPTER_TAG);
    if (status != NDIS_STATUS_SUCCESS)
        return NDIS_STATUS_RESOURCES;

    NdisZeroMemory(adapter, sizeof(*adapter));
    adapter->MiniportAdapterHandle = MiniportAdapterHandle;
    NdisAllocateSpinLock(&adapter->Lock);

    NdisMSetAttributesEx(MiniportAdapterHandle,
                         adapter,
                         0,
                         NDIS_ATTRIBUTE_BUS_MASTER | NDIS_ATTRIBUTE_DESERIALIZE,
                         NdisInterfacePci);

    /* Resource list */
    NdisMQueryAdapterResources(&status,
                               WrapperConfigurationContext,
                               resourceList,
                               &resourceListSize);
    if (status != NDIS_STATUS_RESOURCES)
    {
        status = NDIS_STATUS_FAILURE;
        goto Cleanup;
    }

    status = NdisAllocateMemoryWithTag((PVOID*)&resourceList,
                                       resourceListSize,
                                       RESOURCE_LIST_TAG);
    if (status != NDIS_STATUS_SUCCESS)
        goto Cleanup;

    NdisMQueryAdapterResources(&status,
                               WrapperConfigurationContext,
                               resourceList,
                               &resourceListSize);
    if (status != NDIS_STATUS_SUCCESS)
        goto Cleanup;

    for (i = 0; i < resourceList->Count; i++)
    {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR desc = &resourceList->PartialDescriptors[i];

        switch (desc->Type)
        {
            case CmResourceTypeMemory:
                /* Prefer MMIO BAR (Linux r8169 uses BAR2 / mem). */
                if (adapter->MmioLength == 0)
                {
                    adapter->MmioPhysical = desc->u.Memory.Start;
                    adapter->MmioLength = desc->u.Memory.Length;
                }
                break;

            case CmResourceTypePort:
                if (adapter->IoPortStart == 0)
                {
                    adapter->IoPortStart = desc->u.Port.Start.LowPart;
                    adapter->IoPortLength = desc->u.Port.Length;
                }
                break;

            case CmResourceTypeInterrupt:
                adapter->InterruptVector = desc->u.Interrupt.Vector;
                adapter->InterruptLevel  = desc->u.Interrupt.Level;
                adapter->InterruptShared = (desc->ShareDisposition == CmResourceShareShared);
                adapter->InterruptFlags  = desc->Flags;
                break;

            default:
                break;
        }
    }

    NdisFreeMemory(resourceList, resourceListSize, 0);
    resourceList = NULL;

    if (adapter->InterruptVector == 0)
    {
        NDIS_DbgPrint(MIN_TRACE, ("RTL8168: no interrupt resource\n"));
        status = NDIS_STATUS_FAILURE;
        goto Cleanup;
    }

    /* Map register window: prefer memory BAR, fall back to I/O. */
    if (adapter->MmioLength > 0)
    {
        status = NdisMMapIoSpace((PVOID*)&adapter->IoBase,
                                 MiniportAdapterHandle,
                                 adapter->MmioPhysical,
                                 adapter->MmioLength);
        if (status != NDIS_STATUS_SUCCESS)
        {
            NDIS_DbgPrint(MIN_TRACE,
                ("RTL8168: NdisMMapIoSpace failed (0x%x), trying I/O\n", status));
        }
        else
        {
            adapter->IoBaseIsMmio = TRUE;
            NDIS_DbgPrint(MID_TRACE,
                ("RTL8168: mapped MMIO at %p (%lu bytes)\n",
                 adapter->IoBase, adapter->MmioLength));
        }
    }

    if (adapter->IoBase == NULL && adapter->IoPortLength > 0)
    {
        status = NdisMRegisterIoPortRange((PVOID*)&adapter->IoBase,
                                          MiniportAdapterHandle,
                                          adapter->IoPortStart,
                                          adapter->IoPortLength);
        if (status != NDIS_STATUS_SUCCESS)
            goto Cleanup;
        adapter->IoBaseIsMmio = FALSE;
    }

    if (adapter->IoBase == NULL)
    {
        NDIS_DbgPrint(MIN_TRACE, ("RTL8168: no register window mapped\n"));
        status = NDIS_STATUS_FAILURE;
        goto Cleanup;
    }

    /* DMA */
    status = NdisMInitializeScatterGatherDma(MiniportAdapterHandle,
                                             FALSE,     /* 32-bit DMA */
                                             MAXIMUM_FRAME_SIZE);
    if (status != NDIS_STATUS_SUCCESS)
        goto Cleanup;

    /* Rings + per-slot buffers.  NDIS shared-memory is contiguous and DMA-coherent. */
    status = RtlAllocateSharedRing(adapter, TX_RING_BYTES,
                                   (PVOID*)&adapter->TxRing, &adapter->TxRingPa);
    if (status != NDIS_STATUS_SUCCESS) goto Cleanup;

    status = RtlAllocateSharedRing(adapter, RX_RING_BYTES,
                                   (PVOID*)&adapter->RxRing, &adapter->RxRingPa);
    if (status != NDIS_STATUS_SUCCESS) goto Cleanup;

    status = RtlAllocateSharedRing(adapter, TX_POOL_BYTES,
                                   (PVOID*)&adapter->TxBuffers, &adapter->TxBuffersPa);
    if (status != NDIS_STATUS_SUCCESS) goto Cleanup;

    status = RtlAllocateSharedRing(adapter, RX_POOL_BYTES,
                                   (PVOID*)&adapter->RxBuffers, &adapter->RxBuffersPa);
    if (status != NDIS_STATUS_SUCCESS) goto Cleanup;

    /* Bring chip up.  Order mirrors Linux rtl_init_one/rtl_open: detect the
     * XID first (fails like Linux on unknown silicon), take the 8168G+ MCU
     * out of OOB mode (rtl_hw_initialize), then soft-reset. */
    status = NICDetectChipVersion(adapter);
    if (status != NDIS_STATUS_SUCCESS) goto Cleanup;

    NICInitializeHw(adapter);

    status = NICSoftReset(adapter);
    if (status != NDIS_STATUS_SUCCESS) goto Cleanup;

    /* Force PLL up so the PHY isn't power-gated when we touch MDIO during
     * RtlHwPhyConfig / NICProgramRings.  Safe no-op on chips without PMCH. */
    RtlPllPowerUp(adapter);

    status = NICGetPermanentMacAddress(adapter, adapter->PermanentMacAddress);
    if (status != NDIS_STATUS_SUCCESS) goto Cleanup;

    NdisMoveMemory(adapter->CurrentMacAddress, adapter->PermanentMacAddress,
                   IEEE_802_ADDR_LENGTH);

    NDIS_DbgPrint(MIN_TRACE,
        ("RTL8168: MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
         adapter->PermanentMacAddress[0], adapter->PermanentMacAddress[1],
         adapter->PermanentMacAddress[2], adapter->PermanentMacAddress[3],
         adapter->PermanentMacAddress[4], adapter->PermanentMacAddress[5]));

    /* PHY tuning passes -- Linux runs rtl8169_init_phy before rtl_hw_start. */
    RtlHwPhyConfig(adapter);

    status = NICProgramRings(adapter);
    if (status != NDIS_STATUS_SUCCESS) goto Cleanup;

    NICUpdateLinkStatus(adapter);

    status = NdisMRegisterInterrupt(&adapter->Interrupt,
                                    MiniportAdapterHandle,
                                    adapter->InterruptVector,
                                    adapter->InterruptLevel,
                                    TRUE,
                                    adapter->InterruptShared,
                                    (adapter->InterruptFlags & CM_RESOURCE_INTERRUPT_LATCHED) ?
                                        NdisInterruptLatched :
                                        NdisInterruptLevelSensitive);
    if (status != NDIS_STATUS_SUCCESS)
        goto Cleanup;

    adapter->InterruptRegistered = TRUE;

    /* Enable the engines, then unmask interrupts last, matching the tail of
     * Linux rtl_hw_start. */
    status = NICEnableTxRx(adapter);
    if (status != NDIS_STATUS_SUCCESS)
        goto Cleanup;

    adapter->InterruptMask = DEFAULT_INTERRUPT_MASK;
    NICApplyInterruptMask(adapter);

    NDIS_DbgPrint(MIN_TRACE,
        ("RTL8168: ready, link=%s, speed=%lu Mbps\n",
         adapter->MediaState == NdisMediaStateConnected ? "up" : "down",
         adapter->LinkSpeedMbps));
    return NDIS_STATUS_SUCCESS;

Cleanup:
    if (resourceList != NULL)
        NdisFreeMemory(resourceList, resourceListSize, 0);
    if (adapter != NULL)
        MiniportHalt(adapter);
    return status;
}

NTSTATUS
NTAPI
DriverEntry (
    IN PDRIVER_OBJECT DriverObject,
    IN PUNICODE_STRING RegistryPath
    )
{
    NDIS_HANDLE wrapperHandle;
    NDIS_MINIPORT_CHARACTERISTICS ch;
    NDIS_STATUS status;

    NdisZeroMemory(&ch, sizeof(ch));
    ch.MajorNdisVersion        = NDIS_MINIPORT_MAJOR_VERSION;
    ch.MinorNdisVersion        = NDIS_MINIPORT_MINOR_VERSION;
    ch.HaltHandler             = MiniportHalt;
    ch.HandleInterruptHandler  = MiniportHandleInterrupt;
    ch.InitializeHandler       = MiniportInitialize;
    ch.ISRHandler              = MiniportISR;
    ch.QueryInformationHandler = MiniportQueryInformation;
    ch.ResetHandler            = MiniportReset;
    ch.SendHandler             = MiniportSend;
    ch.SetInformationHandler   = MiniportSetInformation;

    NdisMInitializeWrapper(&wrapperHandle, DriverObject, RegistryPath, NULL);
    if (!wrapperHandle)
        return NDIS_STATUS_FAILURE;

    status = NdisMRegisterMiniport(wrapperHandle, &ch, sizeof(ch));
    if (status != NDIS_STATUS_SUCCESS)
    {
        NdisTerminateWrapper(wrapperHandle, 0);
        return NDIS_STATUS_FAILURE;
    }

    return NDIS_STATUS_SUCCESS;
}
