/*
 * PROJECT:     ReactOS Atheros AR9485 Wi-Fi Driver
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     User-mode bring-up lab: the portable opcode interpreter.
 *
 * Reaches hardware only through LAB_TARGET (see lab_vm.h for why).  Two
 * invariants run through the whole file:
 *
 *   Reject, never clamp.  An out-of-range offset or length is an error the
 *   script author must see; silently narrowing it would return a plausible
 *   value from the wrong place.
 *
 *   Bound every wait, per-op AND per-program.  The machine this drives powers
 *   itself off during a prolonged stall.
 */

#include "lab_vm.h"

typedef struct _LAB_RUN
{
    const LAB_TARGET *Target;
    PUCHAR Out;
    ULONG OutLength;
    PAR9485LAB_RESULT_HEADER Header;
    PAR9485LAB_RESULT Records;
    ULONG RecordCapacity;
    ULONG DataStart;
    ULONG DataUsed;
    ULONG WaitedMicroseconds;
} LAB_RUN, *PLAB_RUN;

static BOOLEAN
LabOffsetValid(ULONG Offset, ULONG Bytes, ULONG Length)
{
    if ((Offset & (sizeof(ULONG) - 1)) != 0)
        return FALSE;
    if (Bytes == 0 || Bytes > Length)
        return FALSE;
    return Offset <= Length - Bytes;
}

static PAR9485LAB_RESULT
LabRecord(PLAB_RUN Run, ULONG Index, ULONG Op)
{
    PAR9485LAB_RESULT Record;

    if (Run->Header->RecordCount >= Run->RecordCapacity)
        return NULL;

    Record = &Run->Records[Run->Header->RecordCount++];
    RtlZeroMemory(Record, sizeof(*Record));
    Record->Index = Index;
    Record->Op = Op;
    Record->Status = AR9485LAB_ST_OK;
    return Record;
}

/* Reserve DataLength bytes in the result blob and return where to write. */
static PUCHAR
LabReserveData(PLAB_RUN Run, ULONG DataLength, ULONG *DataOffset)
{
    ULONG Offset = Run->DataStart + Run->DataUsed;

    if (DataLength > Run->OutLength || Offset > Run->OutLength - DataLength)
        return NULL;

    Run->DataUsed += DataLength;
    *DataOffset = Offset;
    return Run->Out + Offset;
}

/* Resolve a DMA buffer, and the sub-range [Offset, Offset+Length) inside it. */
static ULONG
LabDmaRange(
    PLAB_RUN Run,
    ULONG Index,
    ULONG Offset,
    ULONG Length,
    PUCHAR *Address)
{
    const LAB_TARGET *Target = Run->Target;
    PUCHAR Buffer;
    ULONG BufferLength, PhysicalLow, Flags;

    if (Target->DmaBuffer == NULL)
        return AR9485LAB_ST_UNSUPPORTED;
    if (!Target->DmaBuffer(Target->Context, Index, &Buffer, &BufferLength,
                           &PhysicalLow, &Flags))
    {
        return AR9485LAB_ST_BAD_INDEX;
    }
    if (Length == 0 || Length > BufferLength || Offset > BufferLength - Length)
        return AR9485LAB_ST_BAD_LENGTH;

    *Address = Buffer + Offset;
    return AR9485LAB_ST_OK;
}

/* The 32-bit bus address of a point inside a DMA buffer.  Every allocation
 * path refuses anything above 4 GiB, so a script can program the result
 * straight into a descriptor field. */
static ULONG
LabDmaAddress(PLAB_RUN Run, ULONG Index, ULONG Offset, ULONG *Address)
{
    const LAB_TARGET *Target = Run->Target;
    PUCHAR Buffer;
    ULONG BufferLength, PhysicalLow, Flags;

    if (Target->DmaBuffer == NULL)
        return AR9485LAB_ST_UNSUPPORTED;
    if (!Target->DmaBuffer(Target->Context, Index, &Buffer, &BufferLength,
                           &PhysicalLow, &Flags))
    {
        return AR9485LAB_ST_BAD_INDEX;
    }
    if (Offset >= BufferLength)
        return AR9485LAB_ST_BAD_OFFSET;

    *Address = PhysicalLow + Offset;
    return AR9485LAB_ST_OK;
}

static ULONG
LabWait(PLAB_RUN Run, ULONG Microseconds)
{
    if (Run->WaitedMicroseconds + Microseconds > AR9485LAB_MAX_TOTAL_WAIT_US)
        return AR9485LAB_ST_LIMIT;

    if (Run->Target->Stall != NULL)
        Run->Target->Stall(Run->Target->Context, Microseconds);
    Run->WaitedMicroseconds += Microseconds;
    return AR9485LAB_ST_OK;
}

static ULONG
LabExecuteOne(
    PLAB_RUN Run,
    ULONG Index,
    const AR9485LAB_OP *Op,
    const UCHAR *ProgramData,
    ULONG ProgramDataLength)
{
    const LAB_TARGET *Target = Run->Target;
    PAR9485LAB_RESULT Record;
    ULONG Length = Target->WindowLength;
    ULONG Status;

    switch (Op->Op)
    {
        case AR9485LAB_OP_END:
            return AR9485LAB_ST_OK;

        case AR9485LAB_OP_READ32:
        {
            if (!LabOffsetValid(Op->A, sizeof(ULONG), Length))
                return AR9485LAB_ST_BAD_OFFSET;
            Record = LabRecord(Run, Index, Op->Op);
            if (Record == NULL)
                return AR9485LAB_ST_RESULT_FULL;
            Record->Value = Target->Read32(Target->Context, Op->A);
            return AR9485LAB_ST_OK;
        }

        case AR9485LAB_OP_WRITE32:
        {
            if (!LabOffsetValid(Op->A, sizeof(ULONG), Length))
                return AR9485LAB_ST_BAD_OFFSET;
            Target->Write32(Target->Context, Op->A, Op->B);
            return AR9485LAB_ST_OK;
        }

        case AR9485LAB_OP_RMW32:
        {
            ULONG Value;

            if (!LabOffsetValid(Op->A, sizeof(ULONG), Length))
                return AR9485LAB_ST_BAD_OFFSET;
            Value = (Target->Read32(Target->Context, Op->A) & Op->B) | Op->C;
            Target->Write32(Target->Context, Op->A, Value);
            Record = LabRecord(Run, Index, Op->Op);
            if (Record == NULL)
                return AR9485LAB_ST_RESULT_FULL;
            Record->Value = Value;
            return AR9485LAB_ST_OK;
        }

        case AR9485LAB_OP_POLL:
        {
            ULONG Timeout = Op->D;
            ULONG Elapsed = 0;
            const ULONG Step = 10;
            ULONG Value;
            BOOLEAN Matched = FALSE;

            if (!LabOffsetValid(Op->A, sizeof(ULONG), Length))
                return AR9485LAB_ST_BAD_OFFSET;
            if (Timeout > AR9485LAB_MAX_POLL_US)
                return AR9485LAB_ST_LIMIT;
            if (Run->WaitedMicroseconds + Timeout > AR9485LAB_MAX_TOTAL_WAIT_US)
                return AR9485LAB_ST_LIMIT;

            for (;;)
            {
                Value = Target->Read32(Target->Context, Op->A);
                if ((Value & Op->B) == Op->C)
                {
                    Matched = TRUE;
                    break;
                }
                if (Elapsed >= Timeout)
                    break;
                if (Target->Stall != NULL)
                    Target->Stall(Target->Context, Step);
                Elapsed += Step;
                Run->WaitedMicroseconds += Step;
            }

            Record = LabRecord(Run, Index, Op->Op);
            if (Record == NULL)
                return AR9485LAB_ST_RESULT_FULL;
            Record->Value = Value;
            /* Elapsed is as interesting as the value: "already set" and
             * "settled after 800 us" are different findings. */
            Record->Extra = Elapsed;
            if (!Matched)
            {
                Record->Status = AR9485LAB_ST_TIMEOUT;
                return AR9485LAB_ST_TIMEOUT;
            }
            return AR9485LAB_ST_OK;
        }

        case AR9485LAB_OP_DELAY_US:
        {
            if (Op->A > AR9485LAB_MAX_DELAY_US)
                return AR9485LAB_ST_LIMIT;
            return LabWait(Run, Op->A);
        }

        case AR9485LAB_OP_DUMP:
        {
            ULONG Count = Op->B;
            ULONG Bytes;
            ULONG DataOffset;
            PUCHAR Data;
            ULONG i;

            if (Count == 0 || Count > AR9485LAB_MAX_DUMP_DWORDS)
                return AR9485LAB_ST_BAD_LENGTH;
            Bytes = Count * sizeof(ULONG);
            if (!LabOffsetValid(Op->A, Bytes, Length))
                return AR9485LAB_ST_BAD_OFFSET;

            Record = LabRecord(Run, Index, Op->Op);
            if (Record == NULL)
                return AR9485LAB_ST_RESULT_FULL;

            Data = LabReserveData(Run, Bytes, &DataOffset);
            if (Data == NULL)
            {
                Record->Status = AR9485LAB_ST_RESULT_FULL;
                return AR9485LAB_ST_RESULT_FULL;
            }

            for (i = 0; i < Count; ++i)
            {
                ULONG Value = Target->Read32(Target->Context,
                                             Op->A + i * sizeof(ULONG));

                RtlCopyMemory(Data + i * sizeof(ULONG), &Value, sizeof(ULONG));
            }

            Record->Value = Op->A;
            Record->DataOffset = DataOffset;
            Record->DataLength = Bytes;
            return AR9485LAB_ST_OK;
        }

        case AR9485LAB_OP_MARK:
        {
            Record = LabRecord(Run, Index, Op->Op);
            if (Record == NULL)
                return AR9485LAB_ST_RESULT_FULL;
            Record->Value = Op->A;
            return AR9485LAB_ST_OK;
        }

        case AR9485LAB_OP_BARRIER:
        {
            if (Target->Barrier != NULL)
                Target->Barrier(Target->Context);
            return AR9485LAB_ST_OK;
        }

        case AR9485LAB_OP_PCI_READ:
        case AR9485LAB_OP_PCI_WRITE:
        {
            BOOLEAN Write = (Op->Op == AR9485LAB_OP_PCI_WRITE);
            ULONG Width = Write ? Op->C : Op->B;
            ULONG Value = Write ? Op->B : 0;

            if (Target->PciAccess == NULL)
                return AR9485LAB_ST_UNSUPPORTED;
            if (Width != 1 && Width != 2 && Width != 4)
                return AR9485LAB_ST_BAD_LENGTH;
            if (Op->A > 0x1000 - Width)
                return AR9485LAB_ST_BAD_OFFSET;

            Status = Target->PciAccess(Target->Context, Write, Op->A, Width,
                                       &Value);
            if (Status != AR9485LAB_ST_OK)
                return Status;
            if (Write)
                return AR9485LAB_ST_OK;

            Record = LabRecord(Run, Index, Op->Op);
            if (Record == NULL)
                return AR9485LAB_ST_RESULT_FULL;
            Record->Value = Value;
            return AR9485LAB_ST_OK;
        }

        case AR9485LAB_OP_DMA_LIST:
        {
            ULONG Total = (Target->DmaCount != NULL)
                              ? Target->DmaCount(Target->Context) : 0;
            ULONG Bytes = Total * sizeof(AR9485LAB_DMA_ENTRY);
            ULONG DataOffset;
            PUCHAR Data;
            ULONG i;

            Record = LabRecord(Run, Index, Op->Op);
            if (Record == NULL)
                return AR9485LAB_ST_RESULT_FULL;

            Record->Value = Total;
            if (Total == 0)
                return AR9485LAB_ST_OK;

            Data = LabReserveData(Run, Bytes, &DataOffset);
            if (Data == NULL)
            {
                Record->Status = AR9485LAB_ST_RESULT_FULL;
                return AR9485LAB_ST_RESULT_FULL;
            }

            for (i = 0; i < Total; ++i)
            {
                AR9485LAB_DMA_ENTRY Entry;
                PUCHAR Buffer;
                ULONG BufferLength, PhysicalLow, Flags;

                RtlZeroMemory(&Entry, sizeof(Entry));
                Entry.Index = i;
                if (Target->DmaBuffer(Target->Context, i, &Buffer,
                                      &BufferLength, &PhysicalLow, &Flags))
                {
                    Entry.Length = BufferLength;
                    Entry.PhysicalLow = PhysicalLow;
                    Entry.Flags = Flags;
                }
                RtlCopyMemory(Data + i * sizeof(Entry), &Entry, sizeof(Entry));
            }

            Record->DataOffset = DataOffset;
            Record->DataLength = Bytes;
            return AR9485LAB_ST_OK;
        }

        case AR9485LAB_OP_DMA_ALLOC:
        {
            ULONG First;

            if (Target->DmaAlloc == NULL)
                return AR9485LAB_ST_UNSUPPORTED;
            if (Op->A == 0 || Op->A > AR9485LAB_MAX_DMA_ALLOC)
                return AR9485LAB_ST_LIMIT;
            if (Op->B == 0 || Op->B > AR9485LAB_MAX_DMA_ALLOC_SIZE)
                return AR9485LAB_ST_LIMIT;

            First = Target->DmaAlloc(Target->Context, Op->A, Op->B);
            if (First == MAXULONG)
                return AR9485LAB_ST_NO_RESOURCES;

            Record = LabRecord(Run, Index, Op->Op);
            if (Record == NULL)
                return AR9485LAB_ST_RESULT_FULL;
            Record->Value = First;
            return AR9485LAB_ST_OK;
        }

        case AR9485LAB_OP_DMA_FREE:
        {
            /* All-or-nothing: freeing one buffer out of the middle would
             * renumber the index space a running script is holding. */
            if (Op->A != AR9485LAB_DMA_ALL)
                return AR9485LAB_ST_UNSUPPORTED;
            if (Target->DmaFreeAll == NULL)
                return AR9485LAB_ST_UNSUPPORTED;
            Target->DmaFreeAll(Target->Context);
            return AR9485LAB_ST_OK;
        }

        case AR9485LAB_OP_DMA_READ:
        {
            PUCHAR Source;
            ULONG DataOffset;
            PUCHAR Data;

            Status = LabDmaRange(Run, Op->A, Op->B, Op->C, &Source);
            if (Status != AR9485LAB_ST_OK)
                return Status;

            Record = LabRecord(Run, Index, Op->Op);
            if (Record == NULL)
                return AR9485LAB_ST_RESULT_FULL;

            Data = LabReserveData(Run, Op->C, &DataOffset);
            if (Data == NULL)
            {
                Record->Status = AR9485LAB_ST_RESULT_FULL;
                return AR9485LAB_ST_RESULT_FULL;
            }

            if (Target->Barrier != NULL)
                Target->Barrier(Target->Context);
            RtlCopyMemory(Data, Source, Op->C);
            Record->Value = Op->A;
            Record->DataOffset = DataOffset;
            Record->DataLength = Op->C;
            return AR9485LAB_ST_OK;
        }

        case AR9485LAB_OP_DMA_WRITE:
        {
            PUCHAR Destination;

            Status = LabDmaRange(Run, Op->A, Op->B, Op->C, &Destination);
            if (Status != AR9485LAB_ST_OK)
                return Status;
            if (Op->C > ProgramDataLength || Op->D > ProgramDataLength - Op->C)
                return AR9485LAB_ST_BAD_LENGTH;

            RtlCopyMemory(Destination, ProgramData + Op->D, Op->C);
            if (Target->Barrier != NULL)
                Target->Barrier(Target->Context);
            return AR9485LAB_ST_OK;
        }

        case AR9485LAB_OP_DMA_ZERO:
        {
            PUCHAR Destination;

            Status = LabDmaRange(Run, Op->A, Op->B, Op->C, &Destination);
            if (Status != AR9485LAB_ST_OK)
                return Status;

            RtlZeroMemory(Destination, Op->C);
            if (Target->Barrier != NULL)
                Target->Barrier(Target->Context);
            return AR9485LAB_ST_OK;
        }

        case AR9485LAB_OP_WRITE_DMA_PA:
        {
            ULONG Address;

            if (!LabOffsetValid(Op->A, sizeof(ULONG), Length))
                return AR9485LAB_ST_BAD_OFFSET;
            Status = LabDmaAddress(Run, Op->B, Op->C, &Address);
            if (Status != AR9485LAB_ST_OK)
                return Status;

            Target->Write32(Target->Context, Op->A, Address);
            Record = LabRecord(Run, Index, Op->Op);
            if (Record == NULL)
                return AR9485LAB_ST_RESULT_FULL;
            Record->Value = Address;
            return AR9485LAB_ST_OK;
        }

        case AR9485LAB_OP_DMA_POKE_PA:
        {
            PUCHAR Field;
            ULONG Address;

            Status = LabDmaRange(Run, Op->A, Op->B, sizeof(ULONG), &Field);
            if (Status != AR9485LAB_ST_OK)
                return Status;
            if ((Op->B & (sizeof(ULONG) - 1)) != 0)
                return AR9485LAB_ST_BAD_OFFSET;
            Status = LabDmaAddress(Run, Op->C, Op->D, &Address);
            if (Status != AR9485LAB_ST_OK)
                return Status;

            RtlCopyMemory(Field, &Address, sizeof(Address));
            if (Target->Barrier != NULL)
                Target->Barrier(Target->Context);

            Record = LabRecord(Run, Index, Op->Op);
            if (Record == NULL)
                return AR9485LAB_ST_RESULT_FULL;
            Record->Value = Address;
            return AR9485LAB_ST_OK;
        }

        case AR9485LAB_OP_CALL_HW:
        {
            ULONG Value = 0;

            if (Target->CallHw == NULL)
                return AR9485LAB_ST_UNSUPPORTED;

            Status = Target->CallHw(Target->Context, Op->A, Op->B, &Value);
            if (Status != AR9485LAB_ST_OK)
                return Status;

            Record = LabRecord(Run, Index, Op->Op);
            if (Record == NULL)
                return AR9485LAB_ST_RESULT_FULL;
            Record->Value = Value;
            return AR9485LAB_ST_OK;
        }

        default:
            return AR9485LAB_ST_BAD_OPCODE;
    }
}

ULONG
LabVmRun(
    const LAB_TARGET *Target,
    const UCHAR *In,
    ULONG InLength,
    UCHAR *Out,
    ULONG OutLength,
    ULONG *Written)
{
    LAB_RUN Run;
    const AR9485LAB_PROGRAM *Program;
    const AR9485LAB_OP *Ops;
    const UCHAR *ProgramData = NULL;
    ULONG ProgramDataLength = 0;
    ULONG OpBytes;
    ULONG i;
    BOOLEAN ContinueOnError;

    *Written = 0;

    if (InLength < sizeof(AR9485LAB_PROGRAM))
        return AR9485LAB_VM_MALFORMED;

    Program = (const AR9485LAB_PROGRAM *)In;
    if (Program->AbiVersion != AR9485LAB_ABI_VERSION)
        return AR9485LAB_VM_ABI_MISMATCH;
    if (Program->OpCount == 0 || Program->OpCount > AR9485LAB_MAX_OPS)
        return AR9485LAB_VM_MALFORMED;

    OpBytes = Program->OpCount * sizeof(AR9485LAB_OP);
    if (OpBytes / sizeof(AR9485LAB_OP) != Program->OpCount)
        return AR9485LAB_VM_MALFORMED;
    if (InLength < sizeof(AR9485LAB_PROGRAM) + OpBytes)
        return AR9485LAB_VM_MALFORMED;

    if (Program->DataLength != 0)
    {
        if (Program->DataOffset < sizeof(AR9485LAB_PROGRAM) + OpBytes)
            return AR9485LAB_VM_MALFORMED;
        if (Program->DataLength > InLength ||
            Program->DataOffset > InLength - Program->DataLength)
        {
            return AR9485LAB_VM_MALFORMED;
        }
        ProgramData = In + Program->DataOffset;
        ProgramDataLength = Program->DataLength;
    }

    Ops = (const AR9485LAB_OP *)(In + sizeof(AR9485LAB_PROGRAM));
    ContinueOnError = (Program->Flags & AR9485LAB_PROGRAM_CONTINUE_ON_ERROR) != 0;

    RtlZeroMemory(&Run, sizeof(Run));
    Run.Target = Target;
    Run.Out = Out;
    Run.OutLength = OutLength;
    Run.RecordCapacity = Program->OpCount;
    Run.DataStart = sizeof(AR9485LAB_RESULT_HEADER) +
                    Run.RecordCapacity * sizeof(AR9485LAB_RESULT);

    if (OutLength < Run.DataStart)
        return AR9485LAB_VM_OUT_TOO_SMALL;

    RtlZeroMemory(Out, Run.DataStart);
    Run.Header = (PAR9485LAB_RESULT_HEADER)Out;
    Run.Records = (PAR9485LAB_RESULT)(Out + sizeof(AR9485LAB_RESULT_HEADER));
    Run.Header->AbiVersion = AR9485LAB_ABI_VERSION;
    Run.Header->FailedIndex = MAXULONG;

    for (i = 0; i < Program->OpCount; ++i)
    {
        ULONG Status;

        if (Ops[i].Op == AR9485LAB_OP_END)
            break;

        Status = LabExecuteOne(&Run, i, &Ops[i], ProgramData, ProgramDataLength);
        ++Run.Header->Executed;

        if (Status != AR9485LAB_ST_OK)
        {
            if (Run.Header->FailedIndex == MAXULONG)
            {
                Run.Header->FailedIndex = i;
                Run.Header->FailedStatus = Status;
            }

            /* Emit a record even for ops that normally produce none, so a
             * failure is never invisible in the result stream. */
            if (Run.Header->RecordCount == 0 ||
                Run.Records[Run.Header->RecordCount - 1].Index != i)
            {
                PAR9485LAB_RESULT Record = LabRecord(&Run, i, Ops[i].Op);

                if (Record != NULL)
                    Record->Status = Status;
            }
            else
            {
                Run.Records[Run.Header->RecordCount - 1].Status = Status;
            }

            if (!ContinueOnError)
                break;
        }
    }

    /* Compact: move the data blob down against the records actually used, so
     * a program of 4000 writes and one dump does not ship 96 KiB of zeros. */
    if (Run.DataUsed != 0)
    {
        ULONG PackedStart = sizeof(AR9485LAB_RESULT_HEADER) +
                            Run.Header->RecordCount * sizeof(AR9485LAB_RESULT);

        if (PackedStart < Run.DataStart)
        {
            ULONG Shift = Run.DataStart - PackedStart;

            RtlMoveMemory(Out + PackedStart, Out + Run.DataStart, Run.DataUsed);
            for (i = 0; i < Run.Header->RecordCount; ++i)
            {
                if (Run.Records[i].DataLength != 0)
                    Run.Records[i].DataOffset -= Shift;
            }
            Run.DataStart = PackedStart;
        }
    }

    Run.Header->DataOffset = Run.DataStart;
    Run.Header->DataLength = Run.DataUsed;
    Run.Header->WaitedMicroseconds = Run.WaitedMicroseconds;

    *Written = Run.DataStart + Run.DataUsed;
    return AR9485LAB_VM_OK;
}
