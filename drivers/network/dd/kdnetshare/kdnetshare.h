/*
 * PROJECT:     ReactOS KDNET Shared Adapter Miniport
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Private declarations
 */

#ifndef _KDNETSHARE_H_PRIVATE_
#define _KDNETSHARE_H_PRIVATE_

#include <ndis.h>
#include <reactos/kdnetshare.h>

#define KDNS_TAG                    'ShdK'

#define KDNS_ADDRESS_LENGTH         6
#define KDNS_FRAME_SIZE             1514    /* KDNET_FRAME_CAPACITY */
#define KDNS_MTU_SIZE               1500
#define KDNS_MAX_MULTICAST          32

/* The receive path is polled, so this interval is the floor on inbound latency
 * and, with KDNS_RX_DRAIN, the ceiling on inbound throughput.  Both are the
 * unavoidable price of not owning the interrupt: the debugger does, and it must
 * keep doing so to work from inside a bugcheck. */
#define KDNS_POLL_INTERVAL_MS       1
#define KDNS_RX_DRAIN               32
#define KDNS_DIAG_INTERVAL_100NS    (5ULL * 10 * 1000 * 1000)
#define KDNS_RX_BUFFERS             64

/* Receive slot ownership.  FREE and READY are handed back and forth between the
 * receive callback and the poll DPC; INDICATED means the frame is up in the
 * protocol stack and the slot is not ours again until it is returned. */
#define KDNS_SLOT_FREE              0
#define KDNS_SLOT_READY             1
#define KDNS_SLOT_INDICATED         2

typedef struct _KDNS_RX_SLOT
{
    volatile LONG State;
    ULONG Length;
    UCHAR Data[KDNS_FRAME_SIZE];
} KDNS_RX_SLOT, *PKDNS_RX_SLOT;

typedef struct _KDNS_ADAPTER
{
    NDIS_HANDLE MiniportAdapterHandle;
    NDIS_HANDLE RxNblPool;
    NDIS_HANDLE PollTimer;

    UCHAR PermanentAddress[KDNS_ADDRESS_LENGTH];
    UCHAR CurrentAddress[KDNS_ADDRESS_LENGTH];

    KDNET_SHARE_INFO ShareInfo;
    BOOLEAN Registered;
    BOOLEAN DataPathRunning;
    BOOLEAN Halting;
    volatile LONG PollActive;

    ULONG PacketFilter;
    ULONG Lookahead;
    ULONG MulticastCount;
    UCHAR MulticastList[KDNS_MAX_MULTICAST][KDNS_ADDRESS_LENGTH];

    /* Scratch for flattening a transmit NBL.  Only ever touched inside
     * SendNetBufferLists, which NDIS serializes per adapter. */
    UCHAR TxFrame[KDNS_FRAME_SIZE];

    ULONG RxNext;
    KDNS_RX_SLOT Rx[KDNS_RX_BUFFERS];

    ULONG64 TxOk;
    ULONG64 TxError;
    ULONG64 TxBytes;
    ULONG64 RxOk;
    ULONG64 RxBytes;
    ULONG64 RxNoBuffer;

    /* Slots filled by the callback but not yet indicated.  The poll timer used
     * to indicate only when its own KdNetSharePoll call returned frames, which
     * silently stranded everything the DEBUGGER's poll handed over. */
    volatile LONG RxPending;

    ULONG64 RxCallbacks;
    ULONG64 PollDelivered;
    ULONG PollTicks;
    /* Wall-clock base for the diagnostic line.  Counting DPC ticks was wrong:
     * the period is `5000 / KDNS_POLL_INTERVAL_MS` ticks, which is five seconds
     * only if the timer really runs at 1 kHz.  At a ~15 ms system tick it is
     * ~75 s, and the 32 KB KDBUFFERED ring wraps long before that, so the line
     * was effectively never in a harvest.  Interrupt time does not care how
     * often the DPC actually fires. */
    ULONG64 LastDiagTime;
} KDNS_ADAPTER, *PKDNS_ADAPTER;

#endif /* _KDNETSHARE_H_PRIVATE_ */
