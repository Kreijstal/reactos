/*
 * PROJECT:     ReactOS Atheros AR9485 Wi-Fi Driver
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     The chip worker's one-request command slot.
 *
 * The OID handlers hand the chip worker its long-running work (a scan, a
 * connect) through a single slot, because the work resets the PHY and must
 * run on one thread.  A scan is only put into an empty slot, so it never
 * displaces anything; a connect is put in unconditionally, and if a scan
 * was still waiting there, that scan will never run.  Its OID was already
 * completed with success, and native 802.11 promises the caller a
 * NDIS_STATUS_DOT11_SCAN_CONFIRM for every scan request it accepted; without
 * one, nwifi stays in its scan and wlansvc waits its full scan timeout.  So
 * posting a connect reports the scan it displaced, and the caller confirms
 * that scan as failed (MEDIA_BUSY), as the worker already does for a scan
 * it can no longer run.
 *
 * No NDIS dependency: test_chipcmd.c builds this on the host.  The driver
 * gets the interlocked primitives below; the host test supplies its own.
 */

#ifndef _AR9485_CHIPCMD_H_
#define _AR9485_CHIPCMD_H_

/* Background chip-worker commands.  A disconnect is not one of them: it is
 * the pended OID in PendingDisconnect, so it can never be overwritten in
 * this one-request slot. */
#define AR9485_CMD_NONE             0
#define AR9485_CMD_SCAN             1
#define AR9485_CMD_CONNECT          2

#ifndef AR9485_CMD_EXCHANGE
#define AR9485_CMD_EXCHANGE(Slot, Value) \
    InterlockedExchange((Slot), (Value))
#define AR9485_CMD_COMPARE_EXCHANGE(Slot, Value, Comparand) \
    InterlockedCompareExchange((Slot), (Value), (Comparand))
#endif

/* Queue a scan.  Nonzero if it was queued; zero if the slot was taken. */
static __inline int
AR9485CmdPostScan(volatile long *Slot)
{
    return AR9485_CMD_COMPARE_EXCHANGE(Slot, AR9485_CMD_SCAN,
                                       AR9485_CMD_NONE) == AR9485_CMD_NONE;
}

/*
 * Queue Command over whatever the slot holds.  Nonzero if that was a scan
 * the caller must now confirm as not run.
 */
static __inline int
AR9485CmdPost(volatile long *Slot, long Command)
{
    long Displaced = AR9485_CMD_EXCHANGE(Slot, Command);

    return Displaced == AR9485_CMD_SCAN && Command != AR9485_CMD_SCAN;
}

/* Drop a connect still waiting in the slot (a disconnect overtook it). */
static __inline void
AR9485CmdCancelConnect(volatile long *Slot)
{
    AR9485_CMD_COMPARE_EXCHANGE(Slot, AR9485_CMD_NONE, AR9485_CMD_CONNECT);
}

/* The worker takes the pending command, leaving the slot empty. */
static __inline long
AR9485CmdTake(volatile long *Slot)
{
    return AR9485_CMD_EXCHANGE(Slot, AR9485_CMD_NONE);
}

#endif /* _AR9485_CHIPCMD_H_ */
