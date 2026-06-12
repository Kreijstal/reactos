/*
 * PROJECT:     ReactOS Run-Time Library
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     ARM64 interlocked S-List functions
 */

#include <asm.h>
#include <ksarm64.h>

/*
 * ARM64 always uses the 16-byte SLIST header:
 *   qword 0: Depth:16, Sequence:48
 *   qword 1: HeaderType:1, Init:1, Reserved:2, NextEntry:60
 * NextEntry holds bits 63:4 of the first entry's address. Entries are
 * 16-byte aligned, so (qword 1 & ~0xF) is the first entry pointer and
 * bits 0-1 carry HeaderType|Init.
 *
 * All three operations take a snapshot of the header with a plain LDP
 * (a torn read is corrected by the compare-exchange) and commit with an
 * LDAXP/STLXP 16-byte compare-exchange loop.
 */

    .text
    .align 4

/*
 * PSLIST_ENTRY
 * NTAPI
 * RtlInterlockedPopEntrySList(
 *     _Inout_ PSLIST_HEADER SListHead);
 *
 * The load of FirstEntry->Next reads memory that a concurrent pop may
 * already have freed. The page fault handler resolves a fault taken at
 * ExpInterlockedPopEntrySListFault by restarting the operation at
 * ExpInterlockedPopEntrySListResume.
 */
    .global RtlInterlockedPopEntrySList
    .global ExpInterlockedPopEntrySListResume
    .global ExpInterlockedPopEntrySListFault
    .global ExpInterlockedPopEntrySListEnd
    .seh_proc RtlInterlockedPopEntrySList
RtlInterlockedPopEntrySList:
    .seh_endprologue
ExpInterlockedPopEntrySListResume:
    ldp     x1, x2, [x0]
    ands    x3, x2, #0xFFFFFFFFFFFFFFF0
    b.eq    PopListEmpty
ExpInterlockedPopEntrySListFault:
    ldr     x4, [x3]                            // FirstEntry->Next, may fault
    /* New qword 0: Depth - 1, Sequence + 1 */
    and     x5, x1, #0xFFFF
    sub     x5, x5, #1
    and     x5, x5, #0xFFFF
    lsr     x6, x1, #16
    add     x6, x6, #1
    orr     x5, x5, x6, lsl #16
    /* New qword 1: next entry | HeaderType | Init */
    and     x6, x4, #0xFFFFFFFFFFFFFFF0
    orr     x6, x6, #3
1:  ldaxp   x7, x8, [x0]
    cmp     x7, x1
    ccmp    x8, x2, #0, eq
    b.ne    2f
    stlxp   w9, x5, x6, [x0]
    cbnz    w9, 1b
ExpInterlockedPopEntrySListEnd:
    mov     x0, x3
    ret
2:  clrex
    b       ExpInterlockedPopEntrySListResume
PopListEmpty:
    mov     x0, xzr
    ret
    .seh_endproc

/*
 * PSLIST_ENTRY
 * NTAPI
 * RtlInterlockedPushEntrySList(
 *     _Inout_ PSLIST_HEADER SListHead,
 *     _Inout_ PSLIST_ENTRY SListEntry);
 */
    .global RtlInterlockedPushEntrySList
    .seh_proc RtlInterlockedPushEntrySList
RtlInterlockedPushEntrySList:
    .seh_endprologue
1:  ldp     x2, x3, [x0]
    and     x4, x3, #0xFFFFFFFFFFFFFFF0
    str     x4, [x1]                            // SListEntry->Next = first
    /* New qword 0: Depth + 1, Sequence + 1 */
    and     x5, x2, #0xFFFF
    add     x5, x5, #1
    and     x5, x5, #0xFFFF
    lsr     x6, x2, #16
    add     x6, x6, #1
    orr     x5, x5, x6, lsl #16
    /* New qword 1: new entry | HeaderType | Init */
    orr     x6, x1, #3
2:  ldaxp   x7, x8, [x0]
    cmp     x7, x2
    ccmp    x8, x3, #0, eq
    b.ne    3f
    stlxp   w9, x5, x6, [x0]
    cbnz    w9, 2b
    mov     x0, x4
    ret
3:  clrex
    b       1b
    .seh_endproc

/*
 * PSLIST_ENTRY
 * NTAPI
 * RtlInterlockedFlushSList(
 *     _Inout_ PSLIST_HEADER SListHead);
 */
    .global RtlInterlockedFlushSList
    .seh_proc RtlInterlockedFlushSList
RtlInterlockedFlushSList:
    .seh_endprologue
1:  ldp     x2, x3, [x0]
    ands    x4, x3, #0xFFFFFFFFFFFFFFF0
    b.eq    FlushListEmpty
    /* New qword 0: Depth = 0, Sequence + 1 */
    lsr     x5, x2, #16
    add     x5, x5, #1
    lsl     x5, x5, #16
    /* New qword 1: empty | HeaderType | Init */
    mov     x6, #3
2:  ldaxp   x7, x8, [x0]
    cmp     x7, x2
    ccmp    x8, x3, #0, eq
    b.ne    3f
    stlxp   w9, x5, x6, [x0]
    cbnz    w9, 2b
    mov     x0, x4
    ret
3:  clrex
    b       1b
FlushListEmpty:
    mov     x0, xzr
    ret
    .seh_endproc
