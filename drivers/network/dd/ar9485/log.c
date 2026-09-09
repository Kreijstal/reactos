/*
 * PROJECT:     ReactOS AR9485 Wireless Driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     In-driver text log ring, readable from user mode via the lab
 *
 * On the ASUS X550DP the KDNET transport carries DbgPrint output only during
 * the boot flush; at runtime every DPRINT1 vanishes unless a KD stop is
 * spent, and a KD stop on that box costs the boot.  So every DPRINT1 in this
 * driver also lands here, and AR9485LAB_HW_LOG copies the ring into a lab
 * DMA buffer where `dmaread` can fetch it.  Live driver logging, no KD.
 */

#include "ar9485.h"
#undef DbgPrint     /* this file talks to the real one */

#include <stdarg.h>
#include <ntstrsafe.h>

#define AR9485_LOG_SIZE 16384

static CHAR gAr9485LogRing[AR9485_LOG_SIZE];
static ULONG gAr9485LogHead;        /* next byte to write */
static ULONG gAr9485LogTotal;       /* bytes ever written; > SIZE = wrapped */
static BOOLEAN gAr9485LogAtLineStart = TRUE;
static KSPIN_LOCK gAr9485LogLock;
static LONG gAr9485LogLockInit;

static VOID
AR9485LogAppend(_In_reads_(Length) const CHAR *Text, _In_ ULONG Length)
{
    ULONG i;

    for (i = 0; i < Length; ++i)
    {
        gAr9485LogRing[gAr9485LogHead] = Text[i];
        gAr9485LogHead = (gAr9485LogHead + 1) % AR9485_LOG_SIZE;
        gAr9485LogTotal++;
    }
}

ULONG
__cdecl
AR9485DbgPrint(_In_ PCSTR Format, ...)
{
    CHAR Line[256];
    va_list Args;
    KIRQL OldIrql;
    ULONG Length = 0;

    va_start(Args, Format);
    (VOID)vDbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL, Format, Args);
    va_end(Args);

    va_start(Args, Format);
    if (NT_SUCCESS(RtlStringCbVPrintfA(Line, sizeof(Line), Format, Args)))
        Length = (ULONG)strlen(Line);
    else
        Length = sizeof(Line) - 1;     /* truncated, keep what fits */
    va_end(Args);

    if (InterlockedCompareExchange(&gAr9485LogLockInit, 1, 0) == 0)
        KeInitializeSpinLock(&gAr9485LogLock);

    KeAcquireSpinLock(&gAr9485LogLock, &OldIrql);
    if (gAr9485LogAtLineStart)
    {
        CHAR Stamp[24];
        ULONGLONG Now = KeQueryInterruptTime() / 10000ULL;   /* ms */
        RtlStringCbPrintfA(Stamp, sizeof(Stamp), "[%7I64u.%03I64u] ",
                           Now / 1000, Now % 1000);
        AR9485LogAppend(Stamp, (ULONG)strlen(Stamp));
    }
    AR9485LogAppend(Line, Length);
    gAr9485LogAtLineStart = (Length != 0 && Line[Length - 1] == '\n');
    KeReleaseSpinLock(&gAr9485LogLock, OldIrql);

    return 0;
}

/* Copy the ring, oldest byte first, into Buffer.  Returns bytes copied. */
ULONG
AR9485LogCopy(_Out_writes_bytes_(Length) PUCHAR Buffer, _In_ ULONG Length)
{
    KIRQL OldIrql;
    ULONG Available, Start, i;

    if (gAr9485LogLockInit == 0 || Length == 0)
        return 0;

    KeAcquireSpinLock(&gAr9485LogLock, &OldIrql);
    Available = (gAr9485LogTotal >= AR9485_LOG_SIZE) ? AR9485_LOG_SIZE
                                                     : gAr9485LogHead;
    if (Available > Length)
        Available = Length;
    Start = (gAr9485LogHead + AR9485_LOG_SIZE - Available) % AR9485_LOG_SIZE;
    for (i = 0; i < Available; ++i)
        Buffer[i] = (UCHAR)gAr9485LogRing[(Start + i) % AR9485_LOG_SIZE];
    KeReleaseSpinLock(&gAr9485LogLock, OldIrql);

    return Available;
}
