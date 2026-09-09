/*
 * PROJECT:     ReactOS Atheros AR9485 Wi-Fi Driver
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     User-mode bring-up lab: host harness for the opcode interpreter.
 *
 * Runs the REAL interpreter (lab_vm.c) and the REAL script encoder
 * (lab_script.c) against a simulated target, on the build host, in about a
 * second.  Nothing here reimplements either -- a harness that agreed with the
 * driver only by coincidence would prove nothing about what runs on the
 * machine.
 *
 * This exists because of the two-reboot budget: an interpreter bug found here
 * costs a rebuild, and the same bug found on the ASUS costs a boot.
 *
 *   labtest            run the assertion suite
 *   labtest x.lab      run one script and print its results
 */

#include "lab_vm.h"
#include "lab_script.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WINDOW_LENGTH   (64 * 1024)
#define FIXED_BUFFERS   4
#define BUFFER_SIZE     4096
#define MAX_BUFFERS     (FIXED_BUFFERS + AR9485LAB_MAX_DMA_ALLOC)
/* Synthetic bus addresses.  Deliberately not equal to the host pointers, so a
 * confusion between virtual and bus address cannot pass unnoticed. */
#define FAKE_BUS_BASE   0x10000000u

typedef struct _SIM
{
    unsigned char Window[WINDOW_LENGTH];
    unsigned char Config[4096];
    unsigned char *Buffer[MAX_BUFFERS];
    ULONG BufferLength[MAX_BUFFERS];
    ULONG BufferBus[MAX_BUFFERS];
    ULONG BufferFlags[MAX_BUFFERS];
    ULONG BufferCount;
    ULONG RuntimeCount;
    ULONG RuntimeBytes;
    ULONG StalledMicroseconds;
    ULONG HwCalls;
    /* Set to make the next N reads of PollOffset return PollValue, so a
     * settle-then-match can be exercised without real time passing. */
    ULONG PollOffset;
    ULONG PollValue;
    ULONG PollAfterReads;
    ULONG PollReads;
} SIM;

static SIM gSim;

static ULONG
SimRead32(void *Context, ULONG Offset)
{
    SIM *Sim = (SIM *)Context;
    ULONG Value;

    if (Sim->PollAfterReads != 0 && Offset == Sim->PollOffset)
    {
        ++Sim->PollReads;
        if (Sim->PollReads >= Sim->PollAfterReads)
            memcpy(Sim->Window + Offset, &Sim->PollValue, sizeof(ULONG));
    }

    memcpy(&Value, Sim->Window + Offset, sizeof(Value));
    return Value;
}

static void
SimWrite32(void *Context, ULONG Offset, ULONG Value)
{
    SIM *Sim = (SIM *)Context;

    memcpy(Sim->Window + Offset, &Value, sizeof(Value));
}

static void
SimStall(void *Context, ULONG Microseconds)
{
    /* Simulated: the interpreter's wait accounting is what is under test, not
     * the host's clock.  Sleeping here would make the suite take 5 s. */
    ((SIM *)Context)->StalledMicroseconds += Microseconds;
}

static void
SimBarrier(void *Context)
{
    (void)Context;
}

static ULONG
SimDmaCount(void *Context)
{
    return ((SIM *)Context)->BufferCount;
}

static BOOLEAN
SimDmaBuffer(void *Context, ULONG Index, PUCHAR *VirtualAddress, ULONG *Length,
             ULONG *PhysicalLow, ULONG *Flags)
{
    SIM *Sim = (SIM *)Context;

    if (Index >= Sim->BufferCount || Sim->Buffer[Index] == NULL)
        return FALSE;

    *VirtualAddress = Sim->Buffer[Index];
    *Length = Sim->BufferLength[Index];
    *PhysicalLow = Sim->BufferBus[Index];
    *Flags = Sim->BufferFlags[Index];
    return TRUE;
}

static ULONG
SimDmaAlloc(void *Context, ULONG Count, ULONG Size)
{
    SIM *Sim = (SIM *)Context;
    ULONG First = Sim->BufferCount;
    ULONG i;

    if (Sim->RuntimeCount + Count > AR9485LAB_MAX_DMA_ALLOC)
        return MAXULONG;
    if (Sim->RuntimeBytes + Count * Size > AR9485LAB_MAX_DMA_ALLOC_TOTAL)
        return MAXULONG;

    for (i = 0; i < Count; ++i)
    {
        ULONG Index = Sim->BufferCount;

        Sim->Buffer[Index] = (unsigned char *)calloc(1, Size);
        if (Sim->Buffer[Index] == NULL)
            return MAXULONG;
        Sim->BufferLength[Index] = Size;
        Sim->BufferBus[Index] = FAKE_BUS_BASE + Index * 0x10000u;
        Sim->BufferFlags[Index] = AR9485LAB_DMA_RUNTIME;
        ++Sim->BufferCount;
        ++Sim->RuntimeCount;
        Sim->RuntimeBytes += Size;
    }

    return First;
}

static void
SimDmaFreeAll(void *Context)
{
    SIM *Sim = (SIM *)Context;

    while (Sim->BufferCount > FIXED_BUFFERS)
    {
        --Sim->BufferCount;
        free(Sim->Buffer[Sim->BufferCount]);
        Sim->Buffer[Sim->BufferCount] = NULL;
    }
    Sim->RuntimeCount = 0;
    Sim->RuntimeBytes = 0;
}

static ULONG
SimPciAccess(void *Context, BOOLEAN Write, ULONG Offset, ULONG Width,
             ULONG *Value)
{
    SIM *Sim = (SIM *)Context;

    if (Write)
        memcpy(Sim->Config + Offset, Value, Width);
    else
    {
        *Value = 0;
        memcpy(Value, Sim->Config + Offset, Width);
    }
    return AR9485LAB_ST_OK;
}

static ULONG
SimCallHw(void *Context, ULONG Function, ULONG Argument, ULONG *Value)
{
    SIM *Sim = (SIM *)Context;

    if (Function >= AR9485LAB_HW_MAX)
        return AR9485LAB_ST_UNSUPPORTED;

    ++Sim->HwCalls;
    *Value = Argument ? Argument : 1;
    return AR9485LAB_ST_OK;
}

static void
SimReset(LAB_TARGET *Target)
{
    ULONG i;

    SimDmaFreeAll(&gSim);
    for (i = 0; i < FIXED_BUFFERS; ++i)
    {
        free(gSim.Buffer[i]);
        gSim.Buffer[i] = NULL;
    }

    memset(&gSim, 0, sizeof(gSim));
    for (i = 0; i < FIXED_BUFFERS; ++i)
    {
        gSim.Buffer[i] = (unsigned char *)calloc(1, BUFFER_SIZE);
        gSim.BufferLength[i] = BUFFER_SIZE;
        gSim.BufferBus[i] = FAKE_BUS_BASE + i * 0x1000u;
        gSim.BufferFlags[i] = AR9485LAB_DMA_FIXED;
    }
    gSim.BufferCount = FIXED_BUFFERS;

    memset(Target, 0, sizeof(*Target));
    Target->Context = &gSim;
    Target->WindowLength = WINDOW_LENGTH;
    Target->Read32 = SimRead32;
    Target->Write32 = SimWrite32;
    Target->Stall = SimStall;
    Target->Barrier = SimBarrier;
    Target->DmaCount = SimDmaCount;
    Target->DmaBuffer = SimDmaBuffer;
    Target->DmaAlloc = SimDmaAlloc;
    Target->DmaFreeAll = SimDmaFreeAll;
    Target->PciAccess = SimPciAccess;
    Target->CallHw = SimCallHw;
}

/* ===========================================================================
 *  Assertions
 * ===========================================================================
 */

static int gChecks = 0;
static int gFailures = 0;

static void
Check(int Condition, const char *What)
{
    ++gChecks;
    if (!Condition)
    {
        ++gFailures;
        printf("  FAIL %s\n", What);
    }
}

static void
CheckEqual(unsigned long Actual, unsigned long Expected, const char *What)
{
    ++gChecks;
    if (Actual != Expected)
    {
        ++gFailures;
        printf("  FAIL %s: got 0x%lx, expected 0x%lx\n", What, Actual, Expected);
    }
}

/* Build a program out of ops given inline, run it, hand back the results. */
static unsigned char gIn[AR9485LAB_MAX_PROGRAM_BYTES];
static unsigned char gOut[AR9485LAB_MAX_RESULT_BYTES];

static AR9485LAB_RESULT_HEADER *
RunOps(LAB_TARGET *Target, const AR9485LAB_OP *Ops, ULONG Count, ULONG Flags,
       const unsigned char *Data, ULONG DataLength, ULONG *VmStatus)
{
    AR9485LAB_PROGRAM *Program = (AR9485LAB_PROGRAM *)gIn;
    ULONG Written = 0;
    ULONG InLength;

    memset(gIn, 0, sizeof(gIn));
    memset(gOut, 0, sizeof(gOut));

    Program->AbiVersion = AR9485LAB_ABI_VERSION;
    Program->OpCount = Count;
    Program->DataOffset = (ULONG)(sizeof(*Program) + Count * sizeof(AR9485LAB_OP));
    Program->DataLength = DataLength;
    Program->Flags = Flags;
    memcpy(gIn + sizeof(*Program), Ops, Count * sizeof(AR9485LAB_OP));
    if (DataLength != 0)
        memcpy(gIn + Program->DataOffset, Data, DataLength);
    InLength = Program->DataOffset + DataLength;

    *VmStatus = LabVmRun(Target, gIn, InLength, gOut, sizeof(gOut), &Written);
    return (AR9485LAB_RESULT_HEADER *)gOut;
}

static AR9485LAB_RESULT *
RecordAt(AR9485LAB_RESULT_HEADER *Header, ULONG Index)
{
    AR9485LAB_RESULT *Records =
        (AR9485LAB_RESULT *)((unsigned char *)Header + sizeof(*Header));

    return (Index < Header->RecordCount) ? &Records[Index] : NULL;
}

#define OP(o, a, b, c, d) { (o), (a), (b), (c), (d) }

static void
TestRegisterOps(LAB_TARGET *Target)
{
    static const AR9485LAB_OP Ops[] = {
        OP(AR9485LAB_OP_WRITE32, 0x100, 0xdeadbeef, 0, 0),
        OP(AR9485LAB_OP_READ32,  0x100, 0, 0, 0),
        OP(AR9485LAB_OP_RMW32,   0x100, 0xffff0000, 0x5555, 0),
        OP(AR9485LAB_OP_READ32,  0x100, 0, 0, 0),
        OP(AR9485LAB_OP_MARK,    0x1234, 0, 0, 0),
    };
    ULONG VmStatus;
    AR9485LAB_RESULT_HEADER *Header;

    printf("register ops\n");
    SimReset(Target);
    Header = RunOps(Target, Ops, 5, 0, NULL, 0, &VmStatus);

    CheckEqual(VmStatus, AR9485LAB_VM_OK, "vm status");
    CheckEqual(Header->FailedIndex, MAXULONG, "no failure");
    CheckEqual(Header->Executed, 5, "all ops executed");
    CheckEqual(Header->RecordCount, 4, "read/rmw/read/mark recorded");
    CheckEqual(RecordAt(Header, 0)->Value, 0xdeadbeef, "read back the write");
    CheckEqual(RecordAt(Header, 1)->Value, 0xdead5555, "rmw result");
    CheckEqual(RecordAt(Header, 2)->Value, 0xdead5555, "rmw landed in the window");
    CheckEqual(RecordAt(Header, 3)->Value, 0x1234, "mark tag");
}

static void
TestBoundsAreRejected(LAB_TARGET *Target)
{
    static const AR9485LAB_OP Ops[] = {
        OP(AR9485LAB_OP_READ32, 0x102, 0, 0, 0),            /* unaligned */
        OP(AR9485LAB_OP_READ32, WINDOW_LENGTH, 0, 0, 0),    /* just past the end */
        OP(AR9485LAB_OP_READ32, 0xffff0000, 0, 0, 0),       /* far past, no wrap */
        OP(AR9485LAB_OP_DUMP,   0x100, 999999, 0, 0),       /* absurd count */
        OP(AR9485LAB_OP_DUMP,   WINDOW_LENGTH - 4, 2, 0, 0),/* runs off the end */
        OP(AR9485LAB_OP_DMA_READ, 200, 0, 16, 0),           /* no such buffer */
        OP(AR9485LAB_OP_DMA_READ, 0, 4090, 16, 0),          /* runs off the buffer */
        OP(AR9485LAB_OP_DELAY_US, AR9485LAB_MAX_DELAY_US + 1, 0, 0, 0),
        OP(AR9485LAB_OP_POLL, 0x100, 0, 0, AR9485LAB_MAX_POLL_US + 1),
        OP(AR9485LAB_OP_CALL_HW, 999, 0, 0, 0),             /* no such function */
        OP(AR9485LAB_OP_MAX + 7, 0, 0, 0, 0),               /* no such opcode */
    };
    static const ULONG Expected[] = {
        AR9485LAB_ST_BAD_OFFSET, AR9485LAB_ST_BAD_OFFSET, AR9485LAB_ST_BAD_OFFSET,
        AR9485LAB_ST_BAD_LENGTH, AR9485LAB_ST_BAD_OFFSET, AR9485LAB_ST_BAD_INDEX,
        AR9485LAB_ST_BAD_LENGTH, AR9485LAB_ST_LIMIT, AR9485LAB_ST_LIMIT,
        AR9485LAB_ST_UNSUPPORTED, AR9485LAB_ST_BAD_OPCODE,
    };
    const ULONG Count = (ULONG)(sizeof(Ops) / sizeof(Ops[0]));
    ULONG VmStatus, i;
    AR9485LAB_RESULT_HEADER *Header;

    printf("bounds are rejected, not clamped\n");
    SimReset(Target);
    Header = RunOps(Target, Ops, Count, AR9485LAB_PROGRAM_CONTINUE_ON_ERROR,
                    NULL, 0, &VmStatus);

    CheckEqual(VmStatus, AR9485LAB_VM_OK, "vm status");
    CheckEqual(Header->Executed, Count, "every op attempted");
    CheckEqual(Header->RecordCount, Count, "every failure recorded");
    CheckEqual(Header->FailedIndex, 0, "first failure is op 0");

    for (i = 0; i < Count && i < Header->RecordCount; ++i)
    {
        AR9485LAB_RESULT *Record = RecordAt(Header, i);

        CheckEqual(Record->Index, i, "record index");
        CheckEqual(Record->Status, Expected[i], "rejection reason");
    }
}

static void
TestStopsAtFirstError(LAB_TARGET *Target)
{
    static const AR9485LAB_OP Ops[] = {
        OP(AR9485LAB_OP_WRITE32, 0x200, 0x11111111, 0, 0),
        OP(AR9485LAB_OP_READ32,  0x201, 0, 0, 0),           /* unaligned: stops here */
        OP(AR9485LAB_OP_WRITE32, 0x204, 0x22222222, 0, 0),  /* must NOT run */
    };
    ULONG VmStatus, Value;
    AR9485LAB_RESULT_HEADER *Header;

    printf("a failing op stops the program by default\n");
    SimReset(Target);
    Header = RunOps(Target, Ops, 3, 0, NULL, 0, &VmStatus);

    CheckEqual(Header->Executed, 2, "stopped after the failure");
    CheckEqual(Header->FailedIndex, 1, "failure index");
    CheckEqual(Header->FailedStatus, AR9485LAB_ST_BAD_OFFSET, "failure status");
    memcpy(&Value, gSim.Window + 0x204, sizeof(Value));
    CheckEqual(Value, 0, "the op after the failure never ran");
}

static void
TestPoll(LAB_TARGET *Target)
{
    static const AR9485LAB_OP Immediate[] = {
        OP(AR9485LAB_OP_WRITE32, 0x300, 0x0000000c, 0, 0),
        OP(AR9485LAB_OP_POLL,    0x300, 0x0000000f, 0x0000000c, 1000),
    };
    static const AR9485LAB_OP Settles[] = {
        OP(AR9485LAB_OP_POLL, 0x300, 0xffffffff, 0x600d600d, 1000),
    };
    static const AR9485LAB_OP NeverMatches[] = {
        OP(AR9485LAB_OP_POLL, 0x300, 0xffffffff, 0xdeadc0de, 500),
    };
    ULONG VmStatus;
    AR9485LAB_RESULT_HEADER *Header;

    printf("poll reports how long it waited\n");

    SimReset(Target);
    Header = RunOps(Target, Immediate, 2, 0, NULL, 0, &VmStatus);
    CheckEqual(Header->FailedIndex, MAXULONG, "already-set poll succeeds");
    CheckEqual(RecordAt(Header, 0)->Extra, 0, "already-set poll waited 0 us");
    CheckEqual(RecordAt(Header, 0)->Value, 0x0000000c, "poll value");

    /* Make the register change after a few reads: the poll must succeed and
     * report a non-zero wait. */
    SimReset(Target);
    gSim.PollOffset = 0x300;
    gSim.PollValue = 0x600d600d;
    gSim.PollAfterReads = 5;
    Header = RunOps(Target, Settles, 1, 0, NULL, 0, &VmStatus);
    CheckEqual(Header->FailedIndex, MAXULONG, "settling poll succeeds");
    CheckEqual(RecordAt(Header, 0)->Value, 0x600d600d, "settled value");
    Check(RecordAt(Header, 0)->Extra > 0, "settling poll reports a wait");
    Check(gSim.StalledMicroseconds > 0, "settling poll actually stalled");

    SimReset(Target);
    Header = RunOps(Target, NeverMatches, 1, 0, NULL, 0, &VmStatus);
    CheckEqual(Header->FailedStatus, AR9485LAB_ST_TIMEOUT, "unmatched poll times out");
    CheckEqual(RecordAt(Header, 0)->Extra, 500, "timed-out poll waited the timeout");
}

static void
TestWaitBudget(LAB_TARGET *Target)
{
    AR9485LAB_OP Ops[64];
    ULONG i, VmStatus;
    AR9485LAB_RESULT_HEADER *Header;

    printf("the whole-program wait budget holds\n");
    /* Each delay is legal on its own; together they exceed the ceiling. */
    for (i = 0; i < 64; ++i)
    {
        Ops[i].Op = AR9485LAB_OP_DELAY_US;
        Ops[i].A = AR9485LAB_MAX_DELAY_US;
        Ops[i].B = Ops[i].C = Ops[i].D = 0;
    }

    SimReset(Target);
    Header = RunOps(Target, Ops, 64, 0, NULL, 0, &VmStatus);
    CheckEqual(Header->FailedStatus, AR9485LAB_ST_LIMIT, "budget refuses the excess");
    Check(Header->WaitedMicroseconds <= AR9485LAB_MAX_TOTAL_WAIT_US,
          "never waits past the ceiling");
    CheckEqual(Header->WaitedMicroseconds, gSim.StalledMicroseconds,
               "reported wait matches what the target was asked for");
}

static void
TestDma(LAB_TARGET *Target)
{
    static const unsigned char Payload[16] = {
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
        0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff
    };
    static const AR9485LAB_OP Ops[] = {
        OP(AR9485LAB_OP_DMA_LIST,  0, 0, 0, 0),
        OP(AR9485LAB_OP_DMA_WRITE, 0, 0, 16, 0),
        OP(AR9485LAB_OP_DMA_READ,  0, 0, 16, 0),
        OP(AR9485LAB_OP_WRITE_DMA_PA, 0x78, 2, 0, 0),   /* AR_LP_RXDP <- buf 2 */
        OP(AR9485LAB_OP_DMA_POKE_PA, 0, 0x20, 3, 0x40), /* buf0[0x20] <- &buf3+0x40 */
        OP(AR9485LAB_OP_DMA_READ,  0, 0x20, 4, 0),
        OP(AR9485LAB_OP_DMA_ZERO,  0, 0, 16, 0),
        OP(AR9485LAB_OP_DMA_READ,  0, 0, 16, 0),
        OP(AR9485LAB_OP_DMA_ALLOC, 2, 4096, 0, 0),
        OP(AR9485LAB_OP_DMA_LIST,  0, 0, 0, 0),
    };
    ULONG VmStatus;
    AR9485LAB_RESULT_HEADER *Header;
    AR9485LAB_RESULT *Record;
    AR9485LAB_DMA_ENTRY *Entries;
    ULONG Written, Poked;

    printf("dma buffers and bus addresses\n");
    SimReset(Target);
    Header = RunOps(Target, Ops, 10, 0, Payload, sizeof(Payload), &VmStatus);

    CheckEqual(VmStatus, AR9485LAB_VM_OK, "vm status");
    CheckEqual(Header->FailedIndex, MAXULONG, "no failure");

    /* [0] DMA_LIST over the fixed pool */
    Record = RecordAt(Header, 0);
    CheckEqual(Record->Value, FIXED_BUFFERS, "fixed pool listed");
    Entries = (AR9485LAB_DMA_ENTRY *)(gOut + Record->DataOffset);
    CheckEqual(Entries[0].PhysicalLow, FAKE_BUS_BASE, "buffer 0 bus address");
    CheckEqual(Entries[0].Length, BUFFER_SIZE, "buffer 0 length");
    CheckEqual(Entries[0].Flags, AR9485LAB_DMA_FIXED, "buffer 0 is fixed");

    /* [1] the DMA_READ after DMA_WRITE must return the payload byte for byte */
    Record = RecordAt(Header, 1);
    CheckEqual(Record->DataLength, 16, "read back 16 bytes");
    Check(memcmp(gOut + Record->DataOffset, Payload, 16) == 0,
          "dma write/read round-trips");

    /* [2] WRITE_DMA_PA must have programmed buffer 2's BUS address */
    Record = RecordAt(Header, 2);
    CheckEqual(Record->Value, FAKE_BUS_BASE + 2 * 0x1000u, "wdmapa value");
    memcpy(&Written, gSim.Window + 0x78, sizeof(Written));
    CheckEqual(Written, FAKE_BUS_BASE + 2 * 0x1000u, "wdmapa reached the register");

    /* [3] DMA_POKE_PA must have written &buf3+0x40 into buf0[0x20] */
    Record = RecordAt(Header, 3);
    CheckEqual(Record->Value, FAKE_BUS_BASE + 3 * 0x1000u + 0x40, "dmapokepa value");
    memcpy(&Poked, gSim.Buffer[0] + 0x20, sizeof(Poked));
    CheckEqual(Poked, FAKE_BUS_BASE + 3 * 0x1000u + 0x40, "dmapokepa reached the buffer");

    /* [4] the read-back of that field */
    Record = RecordAt(Header, 4);
    CheckEqual(Record->DataLength, 4, "poked field read back");

    /* [5] after DMA_ZERO the buffer must read as zeros */
    Record = RecordAt(Header, 5);
    {
        static const unsigned char Zeros[16] = { 0 };

        Check(memcmp(gOut + Record->DataOffset, Zeros, 16) == 0,
              "dmazero cleared the buffer");
    }

    /* [6] alloc, [7] list again */
    Record = RecordAt(Header, 6);
    CheckEqual(Record->Value, FIXED_BUFFERS, "runtime buffers start after the pool");
    Record = RecordAt(Header, 7);
    CheckEqual(Record->Value, FIXED_BUFFERS + 2, "list sees the new buffers");
    Entries = (AR9485LAB_DMA_ENTRY *)(gOut + Record->DataOffset);
    CheckEqual(Entries[FIXED_BUFFERS].Flags, AR9485LAB_DMA_RUNTIME,
               "new buffers are marked runtime");
}

static void
TestResultLayout(LAB_TARGET *Target)
{
    /* Many silent ops and one dump: the data blob must be compacted down
     * against the records actually emitted, and its offset must still be
     * correct afterwards. */
    AR9485LAB_OP Ops[130];
    ULONG i, VmStatus;
    AR9485LAB_RESULT_HEADER *Header;
    AR9485LAB_RESULT *Record;
    ULONG First;

    printf("result compaction keeps offsets correct\n");
    for (i = 0; i < 128; ++i)
    {
        Ops[i].Op = AR9485LAB_OP_WRITE32;
        Ops[i].A = 0x400 + i * 4;
        Ops[i].B = 0xa5a50000u + i;
        Ops[i].C = Ops[i].D = 0;
    }
    Ops[128].Op = AR9485LAB_OP_DUMP;
    Ops[128].A = 0x400;
    Ops[128].B = 8;
    Ops[128].C = Ops[128].D = 0;
    Ops[129].Op = AR9485LAB_OP_MARK;
    Ops[129].A = 0xfeed;
    Ops[129].B = Ops[129].C = Ops[129].D = 0;

    SimReset(Target);
    Header = RunOps(Target, Ops, 130, 0, NULL, 0, &VmStatus);

    CheckEqual(Header->RecordCount, 2, "only dump and mark recorded");
    Record = RecordAt(Header, 0);
    CheckEqual(Record->Op, AR9485LAB_OP_DUMP, "first record is the dump");
    CheckEqual(Header->DataOffset,
               sizeof(AR9485LAB_RESULT_HEADER) + 2 * sizeof(AR9485LAB_RESULT),
               "data packed against the records used");
    CheckEqual(Record->DataOffset, Header->DataOffset, "dump data at the start");
    CheckEqual(Record->DataLength, 32, "dump length");
    memcpy(&First, gOut + Record->DataOffset, sizeof(First));
    CheckEqual(First, 0xa5a50000u, "dump content survived compaction");
    CheckEqual(RecordAt(Header, 1)->Value, 0xfeed, "mark after the dump");
}

static void
TestMalformedPrograms(LAB_TARGET *Target)
{
    AR9485LAB_PROGRAM *Program = (AR9485LAB_PROGRAM *)gIn;
    ULONG Written;
    ULONG Status;

    printf("malformed programs are refused\n");
    SimReset(Target);

    memset(gIn, 0, sizeof(gIn));
    Program->AbiVersion = AR9485LAB_ABI_VERSION + 1;
    Program->OpCount = 1;
    Status = LabVmRun(Target, gIn, sizeof(*Program) + sizeof(AR9485LAB_OP),
                      gOut, sizeof(gOut), &Written);
    CheckEqual(Status, AR9485LAB_VM_ABI_MISMATCH, "wrong ABI refused");

    memset(gIn, 0, sizeof(gIn));
    Program->AbiVersion = AR9485LAB_ABI_VERSION;
    Program->OpCount = AR9485LAB_MAX_OPS + 1;
    Status = LabVmRun(Target, gIn, sizeof(gIn), gOut, sizeof(gOut), &Written);
    CheckEqual(Status, AR9485LAB_VM_MALFORMED, "too many ops refused");

    memset(gIn, 0, sizeof(gIn));
    Program->AbiVersion = AR9485LAB_ABI_VERSION;
    Program->OpCount = 4;
    /* Data claiming to start inside the op array would let a DMA_WRITE read
     * the ops as payload. */
    Program->DataOffset = sizeof(*Program);
    Program->DataLength = 8;
    Status = LabVmRun(Target, gIn, sizeof(*Program) + 4 * sizeof(AR9485LAB_OP) + 8,
                      gOut, sizeof(gOut), &Written);
    CheckEqual(Status, AR9485LAB_VM_MALFORMED, "data overlapping the ops refused");

    memset(gIn, 0, sizeof(gIn));
    Program->AbiVersion = AR9485LAB_ABI_VERSION;
    Program->OpCount = 4;
    Program->DataOffset = sizeof(*Program) + 4 * sizeof(AR9485LAB_OP);
    Program->DataLength = 0x7fffffff;   /* runs off the end of the buffer */
    Status = LabVmRun(Target, gIn, sizeof(*Program) + 4 * sizeof(AR9485LAB_OP),
                      gOut, sizeof(gOut), &Written);
    CheckEqual(Status, AR9485LAB_VM_MALFORMED, "data past the buffer refused");

    memset(gIn, 0, sizeof(gIn));
    Program->AbiVersion = AR9485LAB_ABI_VERSION;
    Program->OpCount = 8;
    Status = LabVmRun(Target, gIn, sizeof(*Program) + 8 * sizeof(AR9485LAB_OP),
                      gOut, 16, &Written);
    CheckEqual(Status, AR9485LAB_VM_OUT_TOO_SMALL, "undersized result refused");
}

/* ===========================================================================
 *  Running a real .lab script through the real parser
 * ===========================================================================
 */

static AR9485LAB_OP gScriptOps[AR9485LAB_MAX_OPS];
static LAB_SCRIPT gScript;

static int
RunScriptFile(LAB_TARGET *Target, const char *Path, int Quiet)
{
    FILE *Stream = fopen(Path, "r");
    AR9485LAB_RESULT_HEADER *Header;
    ULONG InLength, Written = 0, Status, i;

    if (Stream == NULL)
    {
        printf("labtest: cannot open %s\n", Path);
        return 0;
    }

    memset(&gScript, 0, sizeof(gScript));
    gScript.Ops = gScriptOps;
    gScript.OpCapacity = AR9485LAB_MAX_OPS;

    if (!LabScriptParse(Stream, &gScript))
    {
        fclose(Stream);
        return 0;
    }
    fclose(Stream);

    InLength = LabScriptEncode(&gScript, gIn, sizeof(gIn));
    if (InLength == 0)
    {
        printf("labtest: %s failed to encode\n", Path);
        return 0;
    }

    memset(gOut, 0, sizeof(gOut));
    Status = LabVmRun(Target, gIn, InLength, gOut, sizeof(gOut), &Written);
    if (Status != AR9485LAB_VM_OK)
    {
        printf("labtest: %s rejected, vm status %lu\n", Path, (unsigned long)Status);
        return 0;
    }

    Header = (AR9485LAB_RESULT_HEADER *)gOut;
    if (Quiet)
        return 1;

    printf("RUN %s: ops=%lu executed=%lu records=%lu waited=%luus\n",
           Path, (unsigned long)gScript.OpCount,
           (unsigned long)Header->Executed,
           (unsigned long)Header->RecordCount,
           (unsigned long)Header->WaitedMicroseconds);

    for (i = 0; i < Header->RecordCount; ++i)
    {
        AR9485LAB_RESULT *Record = RecordAt(Header, i);

        printf("[%lu] %s value=0x%08lx",
               (unsigned long)Record->Index,
               LabScriptOpName(Record->Op),
               (unsigned long)Record->Value);
        if (Record->Op == AR9485LAB_OP_POLL)
            printf(" elapsed=%luus", (unsigned long)Record->Extra);
        if (Record->Status != AR9485LAB_ST_OK)
            printf(" status=%s", LabScriptStatusName(Record->Status));
        printf("\n");
    }

    if (Header->FailedIndex != MAXULONG)
    {
        printf("FAIL index=%lu status=%s\n", (unsigned long)Header->FailedIndex,
               LabScriptStatusName(Header->FailedStatus));
    }
    else
    {
        printf("DONE\n");
    }

    return 1;
}

static void
TestShippedScripts(LAB_TARGET *Target, const char *Directory)
{
    /* The scripts that will actually be run on the machine must at minimum
     * parse, encode and execute against the simulator without the interpreter
     * rejecting them as malformed. */
    static const char *Names[] = {
        "opcode-selftest.lab", "rx-state.lab", "rx-arm-crrxe.lab"
    };
    char Path[1024];
    ULONG i;

    printf("shipped scripts parse, encode and run\n");
    for (i = 0; i < sizeof(Names) / sizeof(Names[0]); ++i)
    {
        snprintf(Path, sizeof(Path), "%s/%s", Directory, Names[i]);
        SimReset(Target);
        Check(RunScriptFile(Target, Path, 1) == 1, Names[i]);
    }
}

int
main(int argc, char **argv)
{
    LAB_TARGET Target;
    int i;

    memset(&gSim, 0, sizeof(gSim));

    if (argc > 2 && strcmp(argv[1], "--scripts") == 0)
    {
        SimReset(&Target);
        TestShippedScripts(&Target, argv[2]);
        printf("\n%d checks, %d failures\n", gChecks, gFailures);
        return gFailures ? 1 : 0;
    }

    if (argc > 1)
    {
        SimReset(&Target);
        for (i = 1; i < argc; ++i)
        {
            if (!RunScriptFile(&Target, argv[i], 0))
                return 1;
        }
        return 0;
    }

    TestRegisterOps(&Target);
    TestBoundsAreRejected(&Target);
    TestStopsAtFirstError(&Target);
    TestPoll(&Target);
    TestWaitBudget(&Target);
    TestDma(&Target);
    TestResultLayout(&Target);
    TestMalformedPrograms(&Target);

    printf("\n%d checks, %d failures\n", gChecks, gFailures);
    return gFailures ? 1 : 0;
}
