/*
 * PROJECT:     ReactOS Atheros AR9485 Wi-Fi Driver
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     Host unit test for the chip worker's command slot (chipcmd.h).
 *
 * Not built by CMake.  Build and run on the host:
 *     gcc -std=c99 -Wall -Wextra -o test_chipcmd test_chipcmd.c && ./test_chipcmd
 *
 * The property under test is the native 802.11 one: every scan request the
 * miniport accepted gets exactly one SCAN_CONFIRM.  A model of the driver
 * (StartScan, the connect OID, the disconnect OID and the chip worker) runs
 * the orders in which those can interleave through the very slot functions
 * the driver calls, and counts the confirms.  A connect that overwrote a
 * queued scan used to leave that scan with none (#431).
 */

#include <stdio.h>

/* Single-threaded stand-ins for the interlocked primitives. */
static long HostExchange(volatile long *Slot, long Value)
{
    long Old = *Slot;
    *Slot = Value;
    return Old;
}

static long HostCompareExchange(volatile long *Slot, long Value, long Comparand)
{
    long Old = *Slot;
    if (Old == Comparand)
        *Slot = Value;
    return Old;
}

#define AR9485_CMD_EXCHANGE(Slot, Value) HostExchange((Slot), (Value))
#define AR9485_CMD_COMPARE_EXCHANGE(Slot, Value, Comparand) \
    HostCompareExchange((Slot), (Value), (Comparand))
#define __inline inline

#include "chipcmd.h"

static int g_pass = 0;
static int g_fail = 0;

static void Check(const char *name, int ok)
{
    if (ok) { ++g_pass; printf("  PASS  %s\n", name); }
    else    { ++g_fail; printf("  FAIL  %s\n", name); }
}

/* The driver, reduced to what touches the slot. */
typedef struct
{
    volatile long Slot;
    int ScansAccepted;      /* OID_DOT11_SCAN_REQUEST returned success */
    int ScanConfirms;       /* NDIS_STATUS_DOT11_SCAN_CONFIRM indicated */
    int ScansRun;
    int ConnectsRun;
} MODEL;

/* AR9485StartScan */
static void OidScan(MODEL *M)
{
    if (AR9485CmdPostScan(&M->Slot))
        M->ScansAccepted++;
}

/* OID_DOT11_CONNECT_REQUEST -> AR9485QueueCommand */
static void OidConnect(MODEL *M)
{
    if (AR9485CmdPost(&M->Slot, AR9485_CMD_CONNECT))
        M->ScanConfirms++;          /* MEDIA_BUSY */
}

/* OID_DOT11_DISCONNECT_REQUEST */
static void OidDisconnect(MODEL *M)
{
    AR9485CmdCancelConnect(&M->Slot);
}

/* One pass of AR9485ChipWorker */
static void Worker(MODEL *M)
{
    switch (AR9485CmdTake(&M->Slot))
    {
        case AR9485_CMD_SCAN:
            M->ScansRun++;
            M->ScanConfirms++;      /* AR9485RunScan, or MEDIA_BUSY */
            break;
        case AR9485_CMD_CONNECT:
            M->ConnectsRun++;
            break;
        default:
            break;
    }
}

static void Reset(MODEL *M)
{
    M->Slot = AR9485_CMD_NONE;
    M->ScansAccepted = M->ScanConfirms = M->ScansRun = M->ConnectsRun = 0;
}

int main(void)
{
    MODEL M;

    printf("command slot: one SCAN_CONFIRM per accepted scan\n");

    Reset(&M);
    OidScan(&M); Worker(&M);
    Check("a scan the worker picks up is confirmed once",
          M.ScansAccepted == 1 && M.ScanConfirms == 1 && M.ScansRun == 1);

    /* #431: the connect lands before the worker woke for the scan. */
    Reset(&M);
    OidScan(&M); OidConnect(&M); Worker(&M);
    Check("a scan displaced by a connect is still confirmed",
          M.ScansAccepted == 1 && M.ScanConfirms == 1);
    Check("... and the connect runs", M.ConnectsRun == 1 && M.ScansRun == 0);

    Reset(&M);
    OidScan(&M); OidConnect(&M); OidConnect(&M); Worker(&M);
    Check("a second connect does not confirm the scan again",
          M.ScanConfirms == 1 && M.ConnectsRun == 1);

    Reset(&M);
    OidConnect(&M); OidScan(&M); Worker(&M);
    Check("a scan is refused while a connect waits, and not confirmed",
          M.ScansAccepted == 0 && M.ScanConfirms == 0 && M.ConnectsRun == 1);

    Reset(&M);
    OidScan(&M); OidDisconnect(&M); Worker(&M);
    Check("a disconnect leaves a queued scan to run",
          M.ScansRun == 1 && M.ScanConfirms == 1);

    Reset(&M);
    OidConnect(&M); OidDisconnect(&M); Worker(&M);
    Check("a disconnect drops a queued connect",
          M.ConnectsRun == 0 && M.Slot == AR9485_CMD_NONE);

    Reset(&M);
    OidConnect(&M); Worker(&M);
    Check("a connect into an empty slot confirms nothing",
          M.ScanConfirms == 0 && M.ConnectsRun == 1);

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
