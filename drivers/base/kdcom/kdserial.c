/*
 * COPYRIGHT:       GPL, see COPYING in the top level directory
 * PROJECT:         ReactOS kernel
 * FILE:            drivers/base/kddll/kdserial.c
 * PURPOSE:         Serial communication functions for the kernel debugger.
 * PROGRAMMER:      Timo Kreuzer (timo.kreuzer@reactos.org)
 */

#include "kddll.h"

/*
 * Upper bound on the number of bytes KdpReceivePacketLeader() will consume
 * while hunting for a packet leader. See the comment in that function.
 */
#define KDP_LEADER_SCAN_LIMIT 65536

/* FUNCTIONS ******************************************************************/

/******************************************************************************
 * \name KdpSendBuffer
 * \brief Sends a buffer of data to the serial KD port.
 * \param Buffer Pointer to the data.
 * \param Size Size of data in bytes.
 */
VOID
NTAPI
KdpSendBuffer(
    IN PVOID Buffer,
    IN ULONG Size)
{
    PUCHAR ByteBuffer = Buffer;

    while (Size-- > 0)
    {
        KdpSendByte(*ByteBuffer++);
    }
}

/******************************************************************************
 * \name KdpReceiveBuffer
 * \brief Receives data from the KD port and fills a buffer.
 * \param Buffer Pointer to a buffer that receives the data.
 * \param Size Size of data to receive in bytes.
 * \return KDP_PACKET_RECEIVED if successful.
 *         KDP_PACKET_TIMEOUT if the receive timed out.
 */
KDP_STATUS
NTAPI
KdpReceiveBuffer(
    OUT PVOID Buffer,
    IN  ULONG Size)
{
    PUCHAR ByteBuffer = Buffer;
    UCHAR Byte;
    KDP_STATUS Status;

    while (Size-- > 0)
    {
        /* Try to get a byte from the port */
        Status = KdpReceiveByte(&Byte);
        if (Status != KDP_PACKET_RECEIVED)
            return Status;

        *ByteBuffer++ = Byte;
    }

    return KDP_PACKET_RECEIVED;
}


/******************************************************************************
 * \name KdpReceivePacketLeader
 * \brief Receives a packet leader from the KD port.
 * \param PacketLeader Pointer to an ULONG that receives the packet leader.
 * \return KDP_PACKET_RECEIVED if successful.
 *         KDP_PACKET_TIMEOUT if the receive timed out.
 *         KDP_PACKET_RESEND if a breakin byte was detected.
 */
KDP_STATUS
NTAPI
KdpReceivePacketLeader(
    OUT PULONG PacketLeader)
{
    UCHAR Index = 0, Byte, Buffer[4];
    KDP_STATUS KdStatus;
    ULONG ScanLimit = KDP_LEADER_SCAN_LIMIT;

    /* Set first character to 0 */
    Buffer[0] = 0;

    do
    {
        /*
         * Bound the scan. This loop only ever terminates on a receive timeout,
         * and KdpReceiveByte() only times out when the line is silent. A line
         * that keeps handing us bytes we cannot use - a host streaming garbage,
         * or a UART that reports a line error without draining the offending
         * byte, as happens under a permanent break condition - would otherwise
         * pin us here forever: no timeout, no return, and nothing transmitted.
         * Report a timeout instead, so that the caller's normal timeout and
         * retry handling gets a chance to run.
         */
        if (ScanLimit-- == 0)
            return KDP_PACKET_TIMEOUT;

        /* Receive a single byte */
        KdStatus = KdpReceiveByte(&Byte);

        /* Check for timeout */
        if (KdStatus == KDP_PACKET_TIMEOUT)
        {
            /* Check if we already got a breakin byte */
            if (Buffer[0] == BREAKIN_PACKET_BYTE)
            {
                return KDP_PACKET_RESEND;
            }

            /* Report timeout */
            return KDP_PACKET_TIMEOUT;
        }

        /* Check if we received a byte */
        if (KdStatus == KDP_PACKET_RECEIVED)
        {
            /* Check if this is a valid packet leader byte */
            if (Byte == PACKET_LEADER_BYTE ||
                Byte == CONTROL_PACKET_LEADER_BYTE)
            {
                /* Check if we match the first byte */
                if (Byte != Buffer[0])
                {
                    /* No, this is the new byte 0! */
                    Index = 0;
                }

                /* Store the byte in the buffer */
                Buffer[Index] = Byte;

                /* Continue with next byte */
                Index++;
                continue;
            }

            /* Check for breakin byte */
            if (Byte == BREAKIN_PACKET_BYTE)
            {
                KDDBGPRINT("BREAKIN_PACKET_BYTE\n");
                Index = 0;
                Buffer[0] = Byte;
                continue;
            }
        }

        /* Restart */
        Index = 0;
        Buffer[0] = 0;
    }
    while (Index < 4);

    /* Enable the debugger */
    KD_DEBUGGER_NOT_PRESENT = FALSE;
    SharedUserData->KdDebuggerEnabled |= 0x00000002;

    /* Return the received packet leader */
    *PacketLeader = *(PULONG)Buffer;

    return KDP_PACKET_RECEIVED;
}

/* EOF */
