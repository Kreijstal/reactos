/*
 * PROJECT:     ReactOS hardware bring-up poke driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Script front end for rospoke.sys
 * COPYRIGHT:   Copyright 2026 the ReactOS contributors
 *
 * The unit of work is a batch, not a line: everything between two commands that
 * must talk to the driver on their own (map, dma, rblk, ...) accumulates into
 * one program and crosses into the kernel in a single ioctl.  That matters
 * because this is normally driven remotely -- push a script, spawn this, read
 * the output -- and a sequence that polls a register on a 25 us deadline cannot
 * afford a network round trip per access.
 *
 * The trace it prints is meant to be diffed.  Capture the same sequence from a
 * working driver on another OS (ath9k funnels every access through
 * ath9k_ioread32/ath9k_iowrite32, both kprobe-able) and the first line that
 * differs is the answer.
 */

#include "pokecli.h"

#define POKE_MAX_HANDLES    32
#define POKE_MAX_TOKENS     16
#define POKE_LINE_MAX       4096

typedef struct _POKE_HANDLE
{
    char Name[32];
    ULONG Target;
    ULONG Length;
    ULONGLONG PhysicalAddress;
} POKE_HANDLE;

static HANDLE PokeDevice = INVALID_HANDLE_VALUE;

static POKE_HANDLE PokeHandles[POKE_MAX_HANDLES];
static int PokeHandleCount = 0;

static ROSPOKE_OP *PokeOps = NULL;
static int *PokeOpHandle = NULL;        /* handle index per op, for the trace */
static ULONG PokeOpCount = 0;
static ULONG PokeOpCapacity = 0;

static int PokeTraceWrites = 1;
static int PokeLineNumber = 0;
static const char *PokeScriptName = "<stdin>";

static const char *const PokeExecStatusName[ROSPOKE_EXEC_MAX] =
{
    "ok",
    "bad opcode",
    "bad target (nothing mapped there)",
    "offset outside the mapped window",
    "misaligned access",
    "poll timed out",
    "more results than the output buffer holds",
    "batch exceeded its time budget",
    "no PCI device opened",
    "PCI config access failed"
};

/* -------------------------------------------------------------------------- */

static
int
PokeError(const char *Format, ...)
{
    va_list Args;

    fprintf(stderr, "%s:%d: ", PokeScriptName, PokeLineNumber);
    va_start(Args, Format);
    vfprintf(stderr, Format, Args);
    va_end(Args);
    fputc('\n', stderr);
    return -1;
}

static
int
PokeOpenDevice(void)
{
    if (PokeDevice != INVALID_HANDLE_VALUE)
        return 0;

    PokeDevice = CreateFileA(ROSPOKE_WIN32_NAME_A,
                             GENERIC_READ | GENERIC_WRITE,
                             0, NULL, OPEN_EXISTING, 0, NULL);
    if (PokeDevice == INVALID_HANDLE_VALUE)
    {
        PokePrintLastError("opening " ROSPOKE_WIN32_NAME_A);
        fprintf(stderr, "rospoke: is the driver started?  Try: rospoke --install && rospoke --start\n");
        return -1;
    }
    return 0;
}

static
int
PokeIoctl(DWORD Code,
          const void *In, DWORD InLength,
          void *Out, DWORD OutLength,
          DWORD *Returned)
{
    DWORD Bytes = 0;

    if (PokeOpenDevice() != 0)
        return -1;

    if (!DeviceIoControl(PokeDevice, Code,
                         (LPVOID)(ULONG_PTR)In, InLength,
                         Out, OutLength, &Bytes, NULL))
    {
        return -1;
    }

    if (Returned != NULL)
        *Returned = Bytes;
    return 0;
}

/* -------------------------------------------------------------------------- */

static
int
PokeHandleFind(const char *Name)
{
    int Index;

    for (Index = 0; Index < PokeHandleCount; Index++)
    {
        if (_stricmp(PokeHandles[Index].Name, Name) == 0)
            return Index;
    }
    return -1;
}

static
int
PokeHandleAdd(const char *Name, ULONG Target, ULONG Length, ULONGLONG Physical)
{
    int Index = PokeHandleFind(Name);

    if (Index < 0)
    {
        if (PokeHandleCount >= POKE_MAX_HANDLES)
            return PokeError("too many handles");
        Index = PokeHandleCount++;
    }

    _snprintf(PokeHandles[Index].Name, sizeof(PokeHandles[Index].Name), "%s", Name);
    PokeHandles[Index].Name[sizeof(PokeHandles[Index].Name) - 1] = '\0';
    PokeHandles[Index].Target = Target;
    PokeHandles[Index].Length = Length;
    PokeHandles[Index].PhysicalAddress = Physical;
    return Index;
}

static
const char *
PokeHandleName(int Index)
{
    if (Index < 0 || Index >= PokeHandleCount)
        return "?";
    return PokeHandles[Index].Name;
}

/* -------------------------------------------------------------------------- */

static
ULONG
PokeOpResults(ULONG Opcode)
{
    switch (Opcode)
    {
        case ROSPOKE_OP_READ32:
        case ROSPOKE_OP_READ16:
        case ROSPOKE_OP_READ8:
        case ROSPOKE_OP_RMW32:
        case ROSPOKE_OP_POLL32:
        case ROSPOKE_OP_MARK:
            return 1;
        default:
            return 0;
    }
}

static
int
PokeOpAppend(ULONG Opcode, int HandleIndex, ULONG Offset,
             ULONG Arg0, ULONG Arg1, ULONG Arg2)
{
    ULONG Target = (HandleIndex >= 0) ? PokeHandles[HandleIndex].Target : 0;

    if (PokeOpCount >= ROSPOKE_MAX_OPS)
        return PokeError("batch is full (%u ops); insert a `run` to flush it", ROSPOKE_MAX_OPS);

    if (PokeOpCount == PokeOpCapacity)
    {
        ULONG NewCapacity = PokeOpCapacity ? PokeOpCapacity * 2 : 1024;
        ROSPOKE_OP *NewOps = realloc(PokeOps, NewCapacity * sizeof(ROSPOKE_OP));
        int *NewHandles = realloc(PokeOpHandle, NewCapacity * sizeof(int));

        if (NewOps != NULL)
            PokeOps = NewOps;
        if (NewHandles != NULL)
            PokeOpHandle = NewHandles;
        if (NewOps == NULL || NewHandles == NULL)
            return PokeError("out of memory growing the batch");

        PokeOpCapacity = NewCapacity;
    }

    PokeOps[PokeOpCount].Opcode = Opcode;
    PokeOps[PokeOpCount].Target = Target;
    PokeOps[PokeOpCount].Offset = Offset;
    PokeOps[PokeOpCount].Arg0 = Arg0;
    PokeOps[PokeOpCount].Arg1 = Arg1;
    PokeOps[PokeOpCount].Arg2 = Arg2;
    PokeOpHandle[PokeOpCount] = HandleIndex;
    PokeOpCount++;
    return 0;
}

static
void
PokeTraceOp(const ROSPOKE_OP *Op, int HandleIndex, const ULONG *Result)
{
    const char *Name = PokeHandleName(HandleIndex);

    switch (Op->Opcode)
    {
        case ROSPOKE_OP_WRITE32:
            if (PokeTraceWrites)
                printf("wr   %s+0x%08lx 0x%08lx\n", Name, Op->Offset, Op->Arg0);
            break;
        case ROSPOKE_OP_WRITE16:
            if (PokeTraceWrites)
                printf("wr16 %s+0x%08lx 0x%04lx\n", Name, Op->Offset, Op->Arg0 & 0xFFFF);
            break;
        case ROSPOKE_OP_WRITE8:
            if (PokeTraceWrites)
                printf("wr8  %s+0x%08lx 0x%02lx\n", Name, Op->Offset, Op->Arg0 & 0xFF);
            break;
        case ROSPOKE_OP_READ32:
            printf("rd   %s+0x%08lx 0x%08lx\n", Name, Op->Offset, Result ? *Result : 0);
            break;
        case ROSPOKE_OP_READ16:
            printf("rd16 %s+0x%08lx 0x%04lx\n", Name, Op->Offset, (Result ? *Result : 0) & 0xFFFF);
            break;
        case ROSPOKE_OP_READ8:
            printf("rd8  %s+0x%08lx 0x%02lx\n", Name, Op->Offset, (Result ? *Result : 0) & 0xFF);
            break;
        case ROSPOKE_OP_RMW32:
            printf("rmw  %s+0x%08lx and=0x%08lx or=0x%08lx old=0x%08lx\n",
                   Name, Op->Offset, Op->Arg0, Op->Arg1, Result ? *Result : 0);
            break;
        case ROSPOKE_OP_POLL32:
            printf("poll %s+0x%08lx mask=0x%08lx want=0x%08lx got=0x%08lx\n",
                   Name, Op->Offset, Op->Arg0, Op->Arg1, Result ? *Result : 0);
            break;
        case ROSPOKE_OP_DELAY_US:
            printf("delay %lu\n", Op->Arg0);
            break;
        case ROSPOKE_OP_MARK:
            printf("mark 0x%08lx\n", Result ? *Result : Op->Arg0);
            break;
        default:
            break;
    }
}

/*
 * Hands the accumulated program to the driver and replays it as a trace.
 * Returns 0 on success, -1 if the program failed on the hardware or the ioctl
 * itself did.  Either way the batch is emptied, so a caller that decides to
 * keep going is not going to re-run it.
 */
static
int
PokeFlush(void)
{
    ROSPOKE_EXEC_IN *In;
    ROSPOKE_EXEC_OUT *Out;
    DWORD InLength, OutLength, Returned = 0;
    ULONG Index, Expected = 0, Consumed = 0;
    const ULONG *Results;
    int Failed = 0;

    if (PokeOpCount == 0)
        return 0;

    for (Index = 0; Index < PokeOpCount; Index++)
        Expected += PokeOpResults(PokeOps[Index].Opcode);

    InLength = (DWORD)(sizeof(ROSPOKE_EXEC_IN) + PokeOpCount * sizeof(ROSPOKE_OP));
    OutLength = (DWORD)(sizeof(ROSPOKE_EXEC_OUT) + Expected * sizeof(ULONG));

    In = calloc(1, InLength);
    Out = calloc(1, OutLength);
    if (In == NULL || Out == NULL)
    {
        free(In);
        free(Out);
        PokeOpCount = 0;
        return PokeError("out of memory building the batch");
    }

    In->OpCount = PokeOpCount;
    In->Flags = 0;
    memcpy((char *)In + sizeof(ROSPOKE_EXEC_IN), PokeOps, PokeOpCount * sizeof(ROSPOKE_OP));

    if (PokeIoctl(IOCTL_ROSPOKE_EXEC, In, InLength, Out, OutLength, &Returned) != 0)
    {
        PokePrintLastError("IOCTL_ROSPOKE_EXEC");
        free(In);
        free(Out);
        PokeOpCount = 0;
        return -1;
    }

    Results = (const ULONG *)((const char *)Out + sizeof(ROSPOKE_EXEC_OUT));

    for (Index = 0; Index < Out->OpsExecuted && Index < PokeOpCount; Index++)
    {
        const ROSPOKE_OP *Op = &PokeOps[Index];
        const ULONG *Result = NULL;

        if (PokeOpResults(Op->Opcode) != 0 && Consumed < Out->ResultCount)
            Result = &Results[Consumed++];

        PokeTraceOp(Op, PokeOpHandle[Index], Result);
    }

    if (Out->ExecStatus != ROSPOKE_EXEC_OK)
    {
        ULONG Failing = Out->OpsExecuted;
        const char *Why = (Out->ExecStatus < ROSPOKE_EXEC_MAX)
                        ? PokeExecStatusName[Out->ExecStatus]
                        : "unknown";

        /* The failing op did not get traced above (the loop stops before it),
         * so name it explicitly -- which op died is the whole diagnosis. */
        if (Failing < PokeOpCount)
        {
            const ROSPOKE_OP *Op = &PokeOps[Failing];
            const ULONG *Result = (PokeOpResults(Op->Opcode) != 0 && Consumed < Out->ResultCount)
                                ? &Results[Consumed] : NULL;

            PokeTraceOp(Op, PokeOpHandle[Failing], Result);
            fprintf(stderr, "rospoke: op %lu (%s+0x%08lx) failed: %s\n",
                    Failing, PokeHandleName(PokeOpHandle[Failing]), Op->Offset, Why);
        }
        else
        {
            fprintf(stderr, "rospoke: batch failed after %lu ops: %s\n", Failing, Why);
        }
        Failed = 1;
    }

    printf("# %lu ops, %lu results, %lu us\n",
           Out->OpsExecuted, Out->ResultCount, Out->ElapsedUs);

    free(In);
    free(Out);
    PokeOpCount = 0;
    return Failed ? -1 : 0;
}

/* -------------------------------------------------------------------------- */

static
int
PokeParseU32(const char *Text, ULONG *Value)
{
    char *End = NULL;
    unsigned long Parsed;

    if (Text == NULL || *Text == '\0')
        return -1;

    Parsed = strtoul(Text, &End, 0);
    if (End == Text || *End != '\0')
        return -1;

    *Value = (ULONG)Parsed;
    return 0;
}

static
int
PokeParseU64(const char *Text, ULONGLONG *Value)
{
    char *End = NULL;
    unsigned __int64 Parsed;

    if (Text == NULL || *Text == '\0')
        return -1;

    Parsed = _strtoui64(Text, &End, 0);
    if (End == Text || *End != '\0')
        return -1;

    *Value = Parsed;
    return 0;
}

static
int
PokeResolve(const char *Name)
{
    int Index = PokeHandleFind(Name);

    if (Index < 0)
        return PokeError("unknown handle '%s' (map it first)", Name);

    return Index;
}

/* -------------------------------------------------------------------------- */

static
void
PokeDumpOpen(const ROSPOKE_OPEN_PCI_OUT *Info)
{
    int Index;

    printf("# %04x:%04x subsys %04x:%04x rev %02x class %02x:%02x:%02x\n",
           Info->VendorId, Info->DeviceId, Info->SubVendorId, Info->SubDeviceId,
           Info->RevisionId, Info->BaseClass, Info->SubClass, Info->ProgIf);
    printf("# command 0x%04x status 0x%04x irq line %u pin %u\n",
           Info->Command, Info->Status, Info->InterruptLine, Info->InterruptPin);

    for (Index = 0; Index < 6; Index++)
    {
        if (Info->Bar[Index].Length == 0)
            continue;
        printf("# bar%d %s base 0x%I64x len 0x%I64x%s%s\n",
               Index,
               (Info->Bar[Index].Flags & ROSPOKE_BAR_IO) ? "io " : "mem",
               Info->Bar[Index].Base,
               Info->Bar[Index].Length,
               (Info->Bar[Index].Flags & ROSPOKE_BAR_MEM64) ? " 64bit" : "",
               (Info->Bar[Index].Flags & ROSPOKE_BAR_PREFETCHABLE) ? " prefetch" : "");
    }
}

static ROSPOKE_OPEN_PCI_OUT PokeLastOpen;
static int PokeHaveOpen = 0;

static
int
PokeCmdOpen(int Count, char **Token)
{
    ROSPOKE_OPEN_PCI_IN In;
    ROSPOKE_OPEN_PCI_OUT Out;
    unsigned Bus = 0, Device = 0, Function = 0;
    int Index;

    if (Count < 2)
        return PokeError("usage: open <bus>:<dev>.<fn> [vid=..] [did=..] [force] [nosize] [master]");

    if (sscanf(Token[1], "%x:%x.%x", &Bus, &Device, &Function) != 3)
        return PokeError("cannot parse '%s' as bus:device.function", Token[1]);

    memset(&In, 0, sizeof(In));
    In.BusNumber = Bus;
    In.DeviceNumber = Device;
    In.FunctionNumber = Function;
    In.ExpectedVendorId = 0xFFFF;
    In.ExpectedDeviceId = 0xFFFF;

    for (Index = 2; Index < Count; Index++)
    {
        ULONG Value;

        if (_strnicmp(Token[Index], "vid=", 4) == 0)
        {
            if (PokeParseU32(Token[Index] + 4, &Value) != 0)
                return PokeError("bad vid");
            In.ExpectedVendorId = (USHORT)Value;
        }
        else if (_strnicmp(Token[Index], "did=", 4) == 0)
        {
            if (PokeParseU32(Token[Index] + 4, &Value) != 0)
                return PokeError("bad did");
            In.ExpectedDeviceId = (USHORT)Value;
        }
        else if (_stricmp(Token[Index], "force") == 0)
            In.Flags |= ROSPOKE_OPEN_FORCE;
        else if (_stricmp(Token[Index], "nosize") == 0)
            In.Flags |= ROSPOKE_OPEN_NO_SIZE_BARS;
        else if (_stricmp(Token[Index], "master") == 0)
            In.Flags |= ROSPOKE_OPEN_SET_MASTER;
        else
            return PokeError("unknown option '%s'", Token[Index]);
    }

    if (In.ExpectedVendorId == 0xFFFF && !(In.Flags & ROSPOKE_OPEN_FORCE))
    {
        return PokeError("refusing to open %02x:%02x.%x without vid= -- a mistyped "
                         "address here writes registers on the wrong device.  Pass "
                         "'force' if you really mean it.", Bus, Device, Function);
    }

    if (PokeIoctl(IOCTL_ROSPOKE_OPEN_PCI, &In, sizeof(In), &Out, sizeof(Out), NULL) != 0)
    {
        PokePrintLastError("IOCTL_ROSPOKE_OPEN_PCI");
        return -1;
    }

    PokeLastOpen = Out;
    PokeHaveOpen = 1;
    PokeDumpOpen(&Out);
    return 0;
}

static
int
PokeCmdMap(int Count, char **Token)
{
    ROSPOKE_MAP_BAR_IN In;
    ROSPOKE_SLOT_OUT Out;
    char Name[32];
    unsigned BarIndex;
    int Index;

    if (Count < 2 || _strnicmp(Token[1], "bar", 3) != 0)
        return PokeError("usage: map bar<N> [as <name>] [len=<n>]");

    BarIndex = (unsigned)(Token[1][3] - '0');
    if (BarIndex > 5)
        return PokeError("bar index must be 0..5");

    memset(&In, 0, sizeof(In));
    In.BarIndex = BarIndex;
    _snprintf(Name, sizeof(Name), "bar%u", BarIndex);
    Name[sizeof(Name) - 1] = '\0';

    for (Index = 2; Index < Count; Index++)
    {
        if (_stricmp(Token[Index], "as") == 0 && Index + 1 < Count)
        {
            _snprintf(Name, sizeof(Name), "%s", Token[++Index]);
            Name[sizeof(Name) - 1] = '\0';
        }
        else if (_strnicmp(Token[Index], "len=", 4) == 0)
        {
            if (PokeParseU32(Token[Index] + 4, &In.Length) != 0)
                return PokeError("bad len");
        }
        else
        {
            return PokeError("unknown option '%s'", Token[Index]);
        }
    }

    if (PokeIoctl(IOCTL_ROSPOKE_MAP_BAR, &In, sizeof(In), &Out, sizeof(Out), NULL) != 0)
    {
        PokePrintLastError("IOCTL_ROSPOKE_MAP_BAR");
        return -1;
    }

    if (PokeHandleAdd(Name, Out.Target, Out.Length, Out.PhysicalAddress) < 0)
        return -1;

    printf("# %s = slot 0x%02lx pa 0x%I64x len 0x%lx\n",
           Name, Out.Target, Out.PhysicalAddress, Out.Length);
    return 0;
}

static
int
PokeCmdMapPhys(int Count, char **Token)
{
    ROSPOKE_MAP_PHYS_IN In;
    ROSPOKE_SLOT_OUT Out;
    char Name[32];
    int Index;
    static int PhysCounter = 0;

    if (Count < 3)
        return PokeError("usage: mapphys <physaddr> <len> [as <name>] [cache=nc|wb|wc]");

    memset(&In, 0, sizeof(In));
    if (PokeParseU64(Token[1], &In.PhysicalAddress) != 0)
        return PokeError("bad physical address");
    if (PokeParseU32(Token[2], &In.Length) != 0)
        return PokeError("bad length");

    _snprintf(Name, sizeof(Name), "p%d", PhysCounter);
    Name[sizeof(Name) - 1] = '\0';

    for (Index = 3; Index < Count; Index++)
    {
        if (_stricmp(Token[Index], "as") == 0 && Index + 1 < Count)
        {
            _snprintf(Name, sizeof(Name), "%s", Token[++Index]);
            Name[sizeof(Name) - 1] = '\0';
        }
        else if (_stricmp(Token[Index], "cache=nc") == 0)
            In.CacheType = 0;
        else if (_stricmp(Token[Index], "cache=wb") == 0)
            In.CacheType = 1;
        else if (_stricmp(Token[Index], "cache=wc") == 0)
            In.CacheType = 2;
        else
            return PokeError("unknown option '%s'", Token[Index]);
    }

    if (PokeIoctl(IOCTL_ROSPOKE_MAP_PHYS, &In, sizeof(In), &Out, sizeof(Out), NULL) != 0)
    {
        PokePrintLastError("IOCTL_ROSPOKE_MAP_PHYS");
        return -1;
    }

    if (PokeHandleAdd(Name, Out.Target, Out.Length, Out.PhysicalAddress) < 0)
        return -1;

    PhysCounter++;
    printf("# %s = slot 0x%02lx pa 0x%I64x len 0x%lx\n",
           Name, Out.Target, Out.PhysicalAddress, Out.Length);
    return 0;
}

static
int
PokeCmdDma(int Count, char **Token)
{
    ROSPOKE_ALLOC_DMA_IN In;
    ROSPOKE_SLOT_OUT Out;
    char Name[32];
    int Index;
    static int DmaCounter = 0;

    if (Count < 2)
        return PokeError("usage: dma <len> [as <name>] [above4g] [cache=nc|wb|wc]");

    memset(&In, 0, sizeof(In));
    if (PokeParseU32(Token[1], &In.Length) != 0)
        return PokeError("bad length");

    _snprintf(Name, sizeof(Name), "d%d", DmaCounter);
    Name[sizeof(Name) - 1] = '\0';

    for (Index = 2; Index < Count; Index++)
    {
        if (_stricmp(Token[Index], "as") == 0 && Index + 1 < Count)
        {
            _snprintf(Name, sizeof(Name), "%s", Token[++Index]);
            Name[sizeof(Name) - 1] = '\0';
        }
        else if (_stricmp(Token[Index], "above4g") == 0)
            In.Flags |= ROSPOKE_DMA_ABOVE_4G;
        else if (_stricmp(Token[Index], "cache=nc") == 0)
            In.CacheType = 0;
        else if (_stricmp(Token[Index], "cache=wb") == 0)
            In.CacheType = 1;
        else if (_stricmp(Token[Index], "cache=wc") == 0)
            In.CacheType = 2;
        else
            return PokeError("unknown option '%s'", Token[Index]);
    }

    if (PokeIoctl(IOCTL_ROSPOKE_ALLOC_DMA, &In, sizeof(In), &Out, sizeof(Out), NULL) != 0)
    {
        PokePrintLastError("IOCTL_ROSPOKE_ALLOC_DMA");
        return -1;
    }

    if (PokeHandleAdd(Name, Out.Target, Out.Length, Out.PhysicalAddress) < 0)
        return -1;

    DmaCounter++;
    printf("# %s = slot 0x%02lx pa 0x%I64x len 0x%lx\n",
           Name, Out.Target, Out.PhysicalAddress, Out.Length);
    return 0;
}

static
void
PokeHexDump(const unsigned char *Data, ULONG Length, ULONG BaseOffset)
{
    ULONG Row;

    for (Row = 0; Row < Length; Row += 16)
    {
        ULONG Column, Span = min(16, Length - Row);

        printf("%08lx ", BaseOffset + Row);
        for (Column = 0; Column < 16; Column++)
        {
            if (Column < Span)
                printf(" %02x", Data[Row + Column]);
            else
                printf("   ");
        }
        printf("  |");
        for (Column = 0; Column < Span; Column++)
        {
            unsigned char Char = Data[Row + Column];
            putchar((Char >= 0x20 && Char < 0x7F) ? Char : '.');
        }
        printf("|\n");
    }
}

static
int
PokeCmdReadBlock(int Count, char **Token)
{
    ROSPOKE_BLOCK_IN In;
    unsigned char *Buffer;
    int HandleIndex;
    DWORD Returned = 0;

    if (Count < 4)
        return PokeError("usage: rblk <handle> <off> <len> [<accesssize>]");

    HandleIndex = PokeResolve(Token[1]);
    if (HandleIndex < 0)
        return -1;

    memset(&In, 0, sizeof(In));
    In.Target = PokeHandles[HandleIndex].Target;
    In.AccessSize = 4;

    if (PokeParseU32(Token[2], &In.Offset) != 0)
        return PokeError("bad offset");
    if (PokeParseU32(Token[3], &In.Length) != 0)
        return PokeError("bad length");
    if (Count > 4 && PokeParseU32(Token[4], &In.AccessSize) != 0)
        return PokeError("bad access size");

    Buffer = calloc(1, In.Length ? In.Length : 1);
    if (Buffer == NULL)
        return PokeError("out of memory");

    if (PokeIoctl(IOCTL_ROSPOKE_READ_BLOCK, &In, sizeof(In), Buffer, In.Length, &Returned) != 0)
    {
        PokePrintLastError("IOCTL_ROSPOKE_READ_BLOCK");
        free(Buffer);
        return -1;
    }

    printf("# rblk %s+0x%08lx len 0x%lx\n", Token[1], In.Offset, In.Length);
    PokeHexDump(Buffer, Returned, In.Offset);
    free(Buffer);
    return 0;
}

static
int
PokeCmdWriteBlock(int HandleIndex, ULONG Offset, ULONG AccessSize,
                  const unsigned char *Data, ULONG Length)
{
    ROSPOKE_BLOCK_IN *In;
    DWORD InLength;
    int Result = 0;

    InLength = (DWORD)(sizeof(ROSPOKE_BLOCK_IN) + Length);
    In = calloc(1, InLength);
    if (In == NULL)
        return PokeError("out of memory");

    In->Target = PokeHandles[HandleIndex].Target;
    In->Offset = Offset;
    In->Length = Length;
    In->AccessSize = AccessSize;
    memcpy((char *)In + sizeof(ROSPOKE_BLOCK_IN), Data, Length);

    if (PokeIoctl(IOCTL_ROSPOKE_WRITE_BLOCK, In, InLength, NULL, 0, NULL) != 0)
    {
        PokePrintLastError("IOCTL_ROSPOKE_WRITE_BLOCK");
        Result = -1;
    }
    else
    {
        printf("# wblk %s+0x%08lx len 0x%lx\n",
               PokeHandleName(HandleIndex), Offset, Length);
    }

    free(In);
    return Result;
}

static
int
PokeCmdSlots(void)
{
    ROSPOKE_QUERY_SLOTS_OUT Out;
    ULONG Index;

    if (PokeIoctl(IOCTL_ROSPOKE_QUERY_SLOTS, NULL, 0, &Out, sizeof(Out), NULL) != 0)
    {
        PokePrintLastError("IOCTL_ROSPOKE_QUERY_SLOTS");
        return -1;
    }

    if (Out.DeviceOpen)
        printf("# device %02lx:%02lx.%lx open\n", Out.BusNumber, Out.DeviceNumber, Out.FunctionNumber);
    else
        printf("# no device open\n");

    for (Index = 0; Index < Out.SlotCount; Index++)
    {
        static const char *const Kind[] = { "free", "bar", "phys", "dma" };

        printf("# slot 0x%02lx %-4s pa 0x%I64x len 0x%lx\n",
               Out.Slot[Index].Target,
               (Out.Slot[Index].Kind < 4) ? Kind[Out.Slot[Index].Kind] : "?",
               Out.Slot[Index].PhysicalAddress,
               Out.Slot[Index].Length);
    }
    return 0;
}

/* -------------------------------------------------------------------------- */

static int PokeBlockMode = 0;       /* 0 = none, 1 = regs, 2 = wblk */
static int PokeBlockHandle = -1;
static ULONG PokeBlockOffset = 0;
static unsigned char *PokeBlockData = NULL;
static ULONG PokeBlockLength = 0;
static ULONG PokeBlockCapacity = 0;

static
int
PokeBlockAppendHex(const char *Text)
{
    int High = -1;

    for (; *Text != '\0'; Text++)
    {
        int Digit;
        char Char = *Text;

        if (Char == ' ' || Char == '\t' || Char == ',')
            continue;

        if (Char >= '0' && Char <= '9')
            Digit = Char - '0';
        else if (Char >= 'a' && Char <= 'f')
            Digit = Char - 'a' + 10;
        else if (Char >= 'A' && Char <= 'F')
            Digit = Char - 'A' + 10;
        else
            return PokeError("'%c' is not a hex digit", Char);

        if (High < 0)
        {
            High = Digit;
            continue;
        }

        if (PokeBlockLength == PokeBlockCapacity)
        {
            ULONG NewCapacity = PokeBlockCapacity ? PokeBlockCapacity * 2 : 256;
            unsigned char *NewData = realloc(PokeBlockData, NewCapacity);

            if (NewData == NULL)
                return PokeError("out of memory");
            PokeBlockData = NewData;
            PokeBlockCapacity = NewCapacity;
        }

        PokeBlockData[PokeBlockLength++] = (unsigned char)((High << 4) | Digit);
        High = -1;
    }

    if (High >= 0)
        return PokeError("odd number of hex digits");

    return 0;
}

static
int
PokeSplit(char *Line, char **Token, int Max)
{
    int Count = 0;

    while (*Line != '\0' && Count < Max)
    {
        while (*Line == ' ' || *Line == '\t')
            Line++;
        if (*Line == '\0' || *Line == '#')
            break;

        Token[Count++] = Line;
        while (*Line != '\0' && *Line != ' ' && *Line != '\t')
            Line++;
        if (*Line != '\0')
            *Line++ = '\0';
    }

    return Count;
}

/*
 * Commands that have to talk to the driver on their own terms end the batch
 * first, so the program the kernel runs always matches the order the script
 * reads in.  Without this, `w reg 0 1` followed by `rblk reg 0 4` would read
 * before it wrote.
 */
static
int
PokeNeedsFlush(const char *Command)
{
    static const char *const Names[] =
    {
        "open", "map", "mapphys", "dma", "release", "slots", "info",
        "rblk", "wblk", "sleep", "run", NULL
    };
    int Index;

    for (Index = 0; Names[Index] != NULL; Index++)
    {
        if (_stricmp(Command, Names[Index]) == 0)
            return 1;
    }
    return 0;
}

static
int
PokeAccessOp(ULONG Opcode, int Count, char **Token)
{
    int HandleIndex;
    ULONG Offset, Arg0 = 0, Arg1 = 0, Arg2 = 0, Repeat = 1, Index;
    ULONG Width = (Opcode == ROSPOKE_OP_WRITE8 || Opcode == ROSPOKE_OP_READ8) ? 1
                : (Opcode == ROSPOKE_OP_WRITE16 || Opcode == ROSPOKE_OP_READ16) ? 2 : 4;
    int IsWrite = (Opcode == ROSPOKE_OP_WRITE8 || Opcode == ROSPOKE_OP_WRITE16 ||
                   Opcode == ROSPOKE_OP_WRITE32);

    if (Count < 3)
        return PokeError("not enough arguments");

    HandleIndex = PokeResolve(Token[1]);
    if (HandleIndex < 0)
        return -1;
    if (PokeParseU32(Token[2], &Offset) != 0)
        return PokeError("bad offset '%s'", Token[2]);

    if (IsWrite)
    {
        if (Count < 4)
            return PokeError("usage: %s <handle> <off> <value>", Token[0]);
        if (PokeParseU32(Token[3], &Arg0) != 0)
            return PokeError("bad value '%s'", Token[3]);
    }
    else if (Opcode == ROSPOKE_OP_RMW32)
    {
        if (Count < 5)
            return PokeError("usage: rmw <handle> <off> <andmask> <orvalue>");
        if (PokeParseU32(Token[3], &Arg0) != 0 || PokeParseU32(Token[4], &Arg1) != 0)
            return PokeError("bad mask or value");
    }
    else if (Opcode == ROSPOKE_OP_POLL32)
    {
        if (Count < 6)
            return PokeError("usage: poll <handle> <off> <mask> <want> <timeout_us>");
        if (PokeParseU32(Token[3], &Arg0) != 0 ||
            PokeParseU32(Token[4], &Arg1) != 0 ||
            PokeParseU32(Token[5], &Arg2) != 0)
        {
            return PokeError("bad mask, want or timeout");
        }
    }
    else if (Count > 3)
    {
        /* A read may say how many consecutive registers to walk. */
        if (PokeParseU32(Token[3], &Repeat) != 0 || Repeat == 0)
            return PokeError("bad repeat count '%s'", Token[3]);
    }

    for (Index = 0; Index < Repeat; Index++)
    {
        if (PokeOpAppend(Opcode, HandleIndex, Offset + Index * Width, Arg0, Arg1, Arg2) != 0)
            return -1;
    }
    return 0;
}

static
int
PokeRunLine(char *Line)
{
    char *Token[POKE_MAX_TOKENS];
    int Count;

    /* Block bodies are read verbatim, before tokenisation gets a say. */
    if (PokeBlockMode != 0)
    {
        char *Trim = Line;

        while (*Trim == ' ' || *Trim == '\t')
            Trim++;

        if (_strnicmp(Trim, "end", 3) == 0 &&
            (Trim[3] == '\0' || Trim[3] == ' ' || Trim[3] == '\t' || Trim[3] == '#'))
        {
            int Mode = PokeBlockMode;
            int Result = 0;

            PokeBlockMode = 0;
            if (Mode == 2)
            {
                Result = PokeCmdWriteBlock(PokeBlockHandle, PokeBlockOffset, 1,
                                           PokeBlockData, PokeBlockLength);
            }
            PokeBlockLength = 0;
            return Result;
        }

        if (*Trim == '\0' || *Trim == '#')
            return 0;

        if (PokeBlockMode == 2)
            return PokeBlockAppendHex(Trim);

        /* regs body: "<offset> <value>" */
        Count = PokeSplit(Trim, Token, POKE_MAX_TOKENS);
        if (Count == 0)
            return 0;
        if (Count < 2)
            return PokeError("regs body wants '<offset> <value>'");
        {
            ULONG Offset, Value;

            if (PokeParseU32(Token[0], &Offset) != 0 || PokeParseU32(Token[1], &Value) != 0)
                return PokeError("bad register pair");
            return PokeOpAppend(ROSPOKE_OP_WRITE32, PokeBlockHandle, Offset, Value, 0, 0);
        }
    }

    Count = PokeSplit(Line, Token, POKE_MAX_TOKENS);
    if (Count == 0)
        return 0;

    if (PokeNeedsFlush(Token[0]) && PokeFlush() != 0)
        return -1;

    if (_stricmp(Token[0], "run") == 0)
        return 0;

    if (_stricmp(Token[0], "open") == 0)
        return PokeCmdOpen(Count, Token);

    if (_stricmp(Token[0], "map") == 0)
        return PokeCmdMap(Count, Token);

    if (_stricmp(Token[0], "mapphys") == 0)
        return PokeCmdMapPhys(Count, Token);

    if (_stricmp(Token[0], "dma") == 0)
        return PokeCmdDma(Count, Token);

    if (_stricmp(Token[0], "slots") == 0)
        return PokeCmdSlots();

    if (_stricmp(Token[0], "info") == 0)
    {
        if (!PokeHaveOpen)
            return PokeError("no device opened yet");
        PokeDumpOpen(&PokeLastOpen);
        return 0;
    }

    if (_stricmp(Token[0], "release") == 0)
    {
        if (PokeIoctl(IOCTL_ROSPOKE_RELEASE_ALL, NULL, 0, NULL, 0, NULL) != 0)
        {
            PokePrintLastError("IOCTL_ROSPOKE_RELEASE_ALL");
            return -1;
        }
        PokeHandleCount = 0;
        PokeHaveOpen = 0;
        PokeHandleAdd("cfg", ROSPOKE_TARGET_CFG, 0x100, 0);
        printf("# released all slots\n");
        return 0;
    }

    if (_stricmp(Token[0], "echo") == 0)
    {
        int Index;

        printf("#");
        for (Index = 1; Index < Count; Index++)
            printf(" %s", Token[Index]);
        printf("\n");
        return 0;
    }

    if (_stricmp(Token[0], "sleep") == 0)
    {
        ULONG Milliseconds;

        if (Count < 2 || PokeParseU32(Token[1], &Milliseconds) != 0)
            return PokeError("usage: sleep <ms>");
        Sleep(Milliseconds);
        return 0;
    }

    if (_stricmp(Token[0], "rblk") == 0)
        return PokeCmdReadBlock(Count, Token);

    if (_stricmp(Token[0], "wblk") == 0)
    {
        int HandleIndex;
        ULONG Offset;

        if (Count < 4)
            return PokeError("usage: wblk <handle> <off> <hexbytes>   or   wblk <handle> <off> <<");

        HandleIndex = PokeResolve(Token[1]);
        if (HandleIndex < 0)
            return -1;
        if (PokeParseU32(Token[2], &Offset) != 0)
            return PokeError("bad offset");

        PokeBlockHandle = HandleIndex;
        PokeBlockOffset = Offset;
        PokeBlockLength = 0;

        if (strcmp(Token[3], "<<") == 0)
        {
            PokeBlockMode = 2;
            return 0;
        }

        {
            int Index, Result = 0;

            for (Index = 3; Index < Count && Result == 0; Index++)
                Result = PokeBlockAppendHex(Token[Index]);
            if (Result != 0)
                return -1;
        }
        return PokeCmdWriteBlock(HandleIndex, Offset, 1, PokeBlockData, PokeBlockLength);
    }

    if (_stricmp(Token[0], "regs") == 0)
    {
        int HandleIndex;

        if (Count < 2)
            return PokeError("usage: regs <handle>  ... <off> <val> lines ...  end");

        HandleIndex = PokeResolve(Token[1]);
        if (HandleIndex < 0)
            return -1;

        PokeBlockHandle = HandleIndex;
        PokeBlockMode = 1;
        return 0;
    }

    if (_stricmp(Token[0], "delay") == 0)
    {
        ULONG Micros;

        if (Count < 2 || PokeParseU32(Token[1], &Micros) != 0)
            return PokeError("usage: delay <microseconds>");
        return PokeOpAppend(ROSPOKE_OP_DELAY_US, -1, 0, Micros, 0, 0);
    }

    if (_stricmp(Token[0], "mark") == 0)
    {
        ULONG Tag;

        if (Count < 2 || PokeParseU32(Token[1], &Tag) != 0)
            return PokeError("usage: mark <tag>");
        return PokeOpAppend(ROSPOKE_OP_MARK, -1, 0, Tag, 0, 0);
    }

    if (_stricmp(Token[0], "w") == 0)
        return PokeAccessOp(ROSPOKE_OP_WRITE32, Count, Token);
    if (_stricmp(Token[0], "w16") == 0)
        return PokeAccessOp(ROSPOKE_OP_WRITE16, Count, Token);
    if (_stricmp(Token[0], "w8") == 0)
        return PokeAccessOp(ROSPOKE_OP_WRITE8, Count, Token);
    if (_stricmp(Token[0], "r") == 0)
        return PokeAccessOp(ROSPOKE_OP_READ32, Count, Token);
    if (_stricmp(Token[0], "r16") == 0)
        return PokeAccessOp(ROSPOKE_OP_READ16, Count, Token);
    if (_stricmp(Token[0], "r8") == 0)
        return PokeAccessOp(ROSPOKE_OP_READ8, Count, Token);
    if (_stricmp(Token[0], "rmw") == 0)
        return PokeAccessOp(ROSPOKE_OP_RMW32, Count, Token);
    if (_stricmp(Token[0], "poll") == 0)
        return PokeAccessOp(ROSPOKE_OP_POLL32, Count, Token);

    return PokeError("unknown command '%s'", Token[0]);
}

static
int
PokeRunStream(FILE *Stream, const char *Name)
{
    char Line[POKE_LINE_MAX];
    int Result = 0;

    PokeScriptName = Name;
    PokeLineNumber = 0;

    while (fgets(Line, sizeof(Line), Stream) != NULL)
    {
        size_t Length = strlen(Line);

        PokeLineNumber++;
        while (Length > 0 && (Line[Length - 1] == '\n' || Line[Length - 1] == '\r'))
            Line[--Length] = '\0';

        if (PokeRunLine(Line) != 0)
        {
            Result = -1;
            break;
        }
    }

    if (PokeBlockMode != 0)
    {
        PokeError("unterminated %s block (missing 'end')",
                  (PokeBlockMode == 2) ? "wblk" : "regs");
        Result = -1;
    }

    /* Flush whatever the script left pending, even on the way out of a failure:
     * the ops already queued describe work the author asked for, and silently
     * dropping them would make the trace lie about what ran. */
    if (PokeFlush() != 0)
        Result = -1;

    return Result;
}

/* -------------------------------------------------------------------------- */

static
void
PokeUsage(void)
{
    printf(
    "rospoke - drive a PCI device from a script, without rebuilding a driver\n"
    "\n"
    "usage: rospoke [-q] <script>|-        run a script (- reads stdin)\n"
    "       rospoke [-q] -e '<command>'    run one command (repeatable)\n"
    "       rospoke --install [<path>]     register the driver service\n"
    "       rospoke --start | --stop       load / UNLOAD the driver image\n"
    "       rospoke --reload               stop, start: picks up a new .sys\n"
    "       rospoke --uninstall            delete the service\n"
    "       rospoke --status               is it installed and running?\n"
    "\n"
    "  -q   do not trace writes, only reads\n"
    "\n"
    "script commands\n"
    "  open <bus>:<dev>.<fn> vid=<id> [did=<id>] [force] [nosize] [master]\n"
    "  map bar<N> [as <name>] [len=<n>]     mapphys <pa> <len> [as <name>]\n"
    "  dma <len> [as <name>] [above4g]      release      slots      info\n"
    "  w|w16|w8 <handle> <off> <value>      r|r16|r8 <handle> <off> [<count>]\n"
    "  rmw <handle> <off> <and> <or>        poll <handle> <off> <mask> <want> <us>\n"
    "  delay <us>   mark <tag>   run   sleep <ms>   echo <text>\n"
    "  rblk <handle> <off> <len> [<accsize>]\n"
    "  wblk <handle> <off> <hex...>   or   wblk <handle> <off> <<  ... end\n"
    "  regs <handle>  ... <off> <val> lines ...  end\n"
    "\n"
    "Handles are named windows; 'cfg' is predefined and reaches the open\n"
    "device's first 256 config bytes.  Ops batch until a command needs the\n"
    "driver's attention, so a whole init table crosses into the kernel once.\n");
}

int
main(int argc, char **argv)
{
    int Index;
    int Result = POKE_EXIT_OK;
    int Ran = 0;

    PokeHandleAdd("cfg", ROSPOKE_TARGET_CFG, 0x100, 0);

    for (Index = 1; Index < argc; Index++)
    {
        const char *Argument = argv[Index];

        if (strcmp(Argument, "-q") == 0)
        {
            PokeTraceWrites = 0;
            continue;
        }

        if (strcmp(Argument, "-h") == 0 || strcmp(Argument, "--help") == 0)
        {
            PokeUsage();
            return POKE_EXIT_OK;
        }

        if (strcmp(Argument, "--install") == 0)
        {
            const char *Path = (Index + 1 < argc && argv[Index + 1][0] != '-')
                             ? argv[++Index] : NULL;
            Ran = 1;
            Result = PokeServiceInstall(Path);
            if (Result != POKE_EXIT_OK)
                return Result;
            continue;
        }

        if (strcmp(Argument, "--uninstall") == 0)
        {
            Ran = 1;
            Result = PokeServiceRemove();
            if (Result != POKE_EXIT_OK)
                return Result;
            continue;
        }

        if (strcmp(Argument, "--start") == 0)
        {
            Ran = 1;
            Result = PokeServiceStart();
            if (Result != POKE_EXIT_OK)
                return Result;
            continue;
        }

        if (strcmp(Argument, "--stop") == 0)
        {
            Ran = 1;
            Result = PokeServiceStop();
            if (Result != POKE_EXIT_OK)
                return Result;
            continue;
        }

        if (strcmp(Argument, "--reload") == 0)
        {
            Ran = 1;
            PokeServiceStop();      /* not running is fine */
            Result = PokeServiceStart();
            if (Result != POKE_EXIT_OK)
                return Result;
            continue;
        }

        if (strcmp(Argument, "--status") == 0)
        {
            Ran = 1;
            Result = PokeServiceStatus();
            if (Result != POKE_EXIT_OK)
                return Result;
            continue;
        }

        if (strcmp(Argument, "-e") == 0)
        {
            char Line[POKE_LINE_MAX];

            if (Index + 1 >= argc)
            {
                fprintf(stderr, "rospoke: -e wants a command\n");
                return POKE_EXIT_USAGE;
            }
            _snprintf(Line, sizeof(Line), "%s", argv[++Index]);
            Line[sizeof(Line) - 1] = '\0';

            PokeScriptName = "-e";
            PokeLineNumber = Index;
            Ran = 1;
            if (PokeRunLine(Line) != 0)
                return POKE_EXIT_SCRIPT;
            continue;
        }

        if (Argument[0] == '-' && Argument[1] != '\0' && strcmp(Argument, "-") != 0)
        {
            fprintf(stderr, "rospoke: unknown option '%s'\n", Argument);
            return POKE_EXIT_USAGE;
        }

        /* A script path, or "-" for stdin. */
        Ran = 1;
        if (strcmp(Argument, "-") == 0)
        {
            if (PokeRunStream(stdin, "<stdin>") != 0)
                Result = POKE_EXIT_SCRIPT;
        }
        else
        {
            FILE *Stream = fopen(Argument, "r");

            if (Stream == NULL)
            {
                fprintf(stderr, "rospoke: cannot open '%s'\n", Argument);
                return POKE_EXIT_USAGE;
            }
            if (PokeRunStream(Stream, Argument) != 0)
                Result = POKE_EXIT_SCRIPT;
            fclose(Stream);
        }
    }

    if (!Ran)
    {
        PokeUsage();
        return POKE_EXIT_USAGE;
    }

    /* -e commands leave a batch behind; nothing else does. */
    if (PokeOpCount != 0 && PokeFlush() != 0)
        Result = POKE_EXIT_SCRIPT;

    if (PokeDevice != INVALID_HANDLE_VALUE)
        CloseHandle(PokeDevice);

    free(PokeOps);
    free(PokeOpHandle);
    free(PokeBlockData);
    return Result;
}
