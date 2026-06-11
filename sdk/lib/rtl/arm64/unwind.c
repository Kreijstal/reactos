/*
 * PROJECT:     ReactOS Run-Time Library
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     ARM64 user-mode PE/COFF unwinding support
 * NOTES:       The unwind-code interpreter is derived from Wine's
 *              dlls/ntdll/unwind.c (ARM64 support), which implements the
 *              Microsoft ARM64 exception handling specification.
 *              Copyright (C) 1999, 2005 Alexandre Julliard
 *              Copyright (C) 2009, 2019 Martin Storsjo
 */

#include <rtl.h>

#define NDEBUG
#include <debug.h>

#ifndef UNW_FLAG_NHANDLER
#define UNW_FLAG_NHANDLER 0x0
#define UNW_FLAG_EHANDLER 0x1
#define UNW_FLAG_UHANDLER 0x2
#endif

#ifndef CONTEXT_UNWOUND_TO_CALL
#define CONTEXT_UNWOUND_TO_CALL CONTEXT_ARM64_UNWOUND_TO_CALL
#endif

/* Funclet/handler call helpers (unwind_asm.S) */
EXCEPTION_DISPOSITION
RtlpCallUnwindHandler(
    _In_ PEXCEPTION_RECORD ExceptionRecord,
    _In_ ULONG_PTR EstablisherFrame,
    _Inout_ PCONTEXT ContextRecord,
    _In_ PVOID DispatcherContext,
    _In_ PEXCEPTION_ROUTINE ExceptionRoutine);

DECLSPEC_NORETURN
VOID
RtlpRestoreContextInternal(
    _In_ PCONTEXT Context);

static ULONG
RtlpArm64FunctionLength(
    _In_ ULONG_PTR ImageBase,
    _In_ PRUNTIME_FUNCTION FunctionEntry)
{
    if (FunctionEntry->Flag != 0)
    {
        return FunctionEntry->FunctionLength * sizeof(ULONG);
    }

    return (((PIMAGE_ARM64_RUNTIME_FUNCTION_ENTRY_XDATA)
             (ImageBase + FunctionEntry->UnwindData))->FunctionLength) * sizeof(ULONG);
}

PRUNTIME_FUNCTION
NTAPI
RtlLookupFunctionTable(
    _In_ DWORD64 ControlPc,
    _Out_ PDWORD64 ImageBase,
    _Out_ PULONG Length)
{
    PVOID Table;
    ULONG Size;

    /* Find the corresponding file header from the code address. This works
       in both modes: user mode walks the PEB loader lists, kernel mode walks
       PsLoadedModuleList (ntoskrnl/rtl/libsupp.c). */
    if (!RtlPcToFileHeader((PVOID)(ULONG_PTR)ControlPc, (PVOID*)ImageBase))
    {
        *Length = 0;
        return NULL;
    }

    /* Locate the exception directory */
    Table = RtlImageDirectoryEntryToData((PVOID)(ULONG_PTR)*ImageBase,
                                         TRUE,
                                         IMAGE_DIRECTORY_ENTRY_EXCEPTION,
                                         &Size);

    *Length = Size / sizeof(RUNTIME_FUNCTION);
    return Table;
}

PRUNTIME_FUNCTION
NTAPI
RtlLookupFunctionEntry(
    _In_ DWORD64 ControlPc,
    _Out_ PDWORD64 ImageBase,
    _Inout_opt_ PVOID HistoryTable)
{
    PRUNTIME_FUNCTION FunctionTable, FunctionEntry;
    ULONG TableLength;
    ULONG_PTR ControlRva;
    ULONG IndexLow, IndexHigh, IndexMid;

    (VOID)HistoryTable;

    FunctionTable = RtlLookupFunctionTable(ControlPc, ImageBase, &TableLength);
    if (FunctionTable == NULL)
        return NULL;

    ControlRva = (ULONG_PTR)(ControlPc - *ImageBase);

    IndexLow = 0;
    IndexHigh = TableLength;
    while (IndexHigh > IndexLow)
    {
        IndexMid = (IndexLow + IndexHigh) / 2;
        FunctionEntry = &FunctionTable[IndexMid];

        if (ControlRva < FunctionEntry->BeginAddress)
        {
            IndexHigh = IndexMid;
            continue;
        }

        if (ControlRva >= FunctionEntry->BeginAddress +
                          RtlpArm64FunctionLength((ULONG_PTR)*ImageBase, FunctionEntry))
        {
            IndexLow = IndexMid + 1;
            continue;
        }

        return FunctionEntry;
    }

    return NULL;
}

/*
 * ARM64 unwind code interpreter, derived from Wine.
 */

static const UCHAR RtlpArm64CodeLength[256] =
{
/* 00 */ 1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
/* 20 */ 1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
/* 40 */ 1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
/* 60 */ 1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
/* 80 */ 1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
/* a0 */ 1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
/* c0 */ 2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,
/* e0 */ 4,1,2,1,1,1,1,3,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1
};

static ULONG
RtlpArm64GetSequenceLength(
    _In_ PUCHAR Ptr,
    _In_ PUCHAR End)
{
    ULONG Count = 0;

    while (Ptr < End)
    {
        if (*Ptr == 0xE4 || *Ptr == 0xE5)
            break;
        /* Custom stack frame ops (0xE8-0xEF) don't map to instructions */
        if ((*Ptr & 0xF8) != 0xE8)
            Count++;
        Ptr += RtlpArm64CodeLength[*Ptr];
    }
    return Count;
}

static VOID
RtlpArm64RestoreRegs(
    _In_ ULONG Reg,
    _In_ ULONG Count,
    _In_ LONG Pos,
    _Inout_ PCONTEXT Context,
    _Inout_opt_ PKNONVOLATILE_CONTEXT_POINTERS Ptrs)
{
    ULONG i;
    ULONG Offset = (Pos > 0) ? (ULONG)Pos : 0;

    for (i = 0; i < Count; i++)
    {
        if (Ptrs && (Reg + i) >= 19 && (Reg + i) <= 28)
            (&Ptrs->X19)[Reg + i - 19] = (PDWORD64)Context->Sp + i + Offset;
        Context->X[Reg + i] = ((PDWORD64)Context->Sp)[i + Offset];
    }
    if (Pos < 0)
        Context->Sp += -8 * Pos;
}

static VOID
RtlpArm64RestoreFpRegs(
    _In_ ULONG Reg,
    _In_ ULONG Count,
    _In_ LONG Pos,
    _Inout_ PCONTEXT Context,
    _Inout_opt_ PKNONVOLATILE_CONTEXT_POINTERS Ptrs)
{
    ULONG i;
    ULONG Offset = (Pos > 0) ? (ULONG)Pos : 0;

    for (i = 0; i < Count; i++)
    {
        if (Ptrs && (Reg + i) >= 8 && (Reg + i) <= 15)
            (&Ptrs->D8)[Reg + i - 8] = (PDWORD64)Context->Sp + i + Offset;
        Context->V[Reg + i].D[0] = ((double *)Context->Sp)[i + Offset];
    }
    if (Pos < 0)
        Context->Sp += -8 * Pos;
}

static VOID
RtlpArm64RestoreQRegs(
    _In_ ULONG Reg,
    _In_ ULONG Count,
    _In_ LONG Pos,
    _Inout_ PCONTEXT Context,
    _Inout_opt_ PKNONVOLATILE_CONTEXT_POINTERS Ptrs)
{
    ULONG i;
    ULONG Offset = (Pos > 0) ? (ULONG)Pos : 0;

    for (i = 0; i < Count; i++)
    {
        if (Ptrs && (Reg + i) >= 8 && (Reg + i) <= 15)
            (&Ptrs->D8)[Reg + i - 8] = (PDWORD64)Context->Sp + 2 * (i + Offset);
        Context->V[Reg + i].Low  = ((PDWORD64)Context->Sp)[2 * (i + Offset)];
        Context->V[Reg + i].High = ((PDWORD64)Context->Sp)[2 * (i + Offset) + 1];
    }
    if (Pos < 0)
        Context->Sp += -16 * Pos;
}

static VOID
RtlpArm64RestoreAnyReg(
    _In_ ULONG Reg,
    _In_ ULONG Count,
    _In_ ULONG Type,
    _In_ LONG Pos,
    _Inout_ PCONTEXT Context,
    _Inout_opt_ PKNONVOLATILE_CONTEXT_POINTERS Ptrs)
{
    if (Reg & 0x20)
        Pos = -Pos - 1;

    switch (Type)
    {
        case 0:
            if (Count > 1 || Pos < 0)
                Pos *= 2;
            RtlpArm64RestoreRegs(Reg & 0x1F, Count, Pos, Context, Ptrs);
            break;
        case 1:
            if (Count > 1 || Pos < 0)
                Pos *= 2;
            RtlpArm64RestoreFpRegs(Reg & 0x1F, Count, Pos, Context, Ptrs);
            break;
        case 2:
            RtlpArm64RestoreQRegs(Reg & 0x1F, Count, Pos, Context, Ptrs);
            break;
    }
}

static VOID
RtlpArm64PacAuth(
    _Inout_ PCONTEXT Context)
{
    register DWORD64 x17 __asm__("x17") = Context->Lr;
    register DWORD64 x16 __asm__("x16") = Context->Sp;

    /* autib1716; hint encoding so pre-armv8.3a assemblers accept it */
    __asm__("hint 0xe" : "+r"(x17) : "r"(x16));

    Context->Lr = x17;
}

static VOID
RtlpArm64ProcessUnwindCodes(
    _In_ PUCHAR Ptr,
    _In_ PUCHAR End,
    _Inout_ PCONTEXT Context,
    _Inout_opt_ PKNONVOLATILE_CONTEXT_POINTERS Ptrs,
    _In_ ULONG Skip,
    _Inout_ PBOOLEAN FinalPcFromLr)
{
    ULONG i, Val, Len, SaveNext = 2;

    /* Skip the codes for instructions not yet executed */
    while (Ptr < End && Skip)
    {
        if (*Ptr == 0xE4)
            break;
        Ptr += RtlpArm64CodeLength[*Ptr];
        Skip--;
    }

    while (Ptr < End)
    {
        if ((Len = RtlpArm64CodeLength[*Ptr]) > 1)
        {
            if (Ptr + Len > End)
                break;
            Val = Ptr[0] * 0x100 + Ptr[1];
        }
        else
        {
            Val = *Ptr;
        }

        if (*Ptr < 0x20)        /* alloc_s */
            Context->Sp += 16 * (Val & 0x1F);
        else if (*Ptr < 0x40)   /* save_r19r20_x */
            RtlpArm64RestoreRegs(19, SaveNext, -(LONG)(Val & 0x1F), Context, Ptrs);
        else if (*Ptr < 0x80)   /* save_fplr */
            RtlpArm64RestoreRegs(29, 2, Val & 0x3F, Context, Ptrs);
        else if (*Ptr < 0xC0)   /* save_fplr_x */
            RtlpArm64RestoreRegs(29, 2, -(LONG)(Val & 0x3F) - 1, Context, Ptrs);
        else if (*Ptr < 0xC8)   /* alloc_m */
            Context->Sp += 16 * (Val & 0x7FF);
        else if (*Ptr < 0xCC)   /* save_regp */
            RtlpArm64RestoreRegs(19 + ((Val >> 6) & 0xF), SaveNext, Val & 0x3F, Context, Ptrs);
        else if (*Ptr < 0xD0)   /* save_regp_x */
            RtlpArm64RestoreRegs(19 + ((Val >> 6) & 0xF), SaveNext, -(LONG)(Val & 0x3F) - 1, Context, Ptrs);
        else if (*Ptr < 0xD4)   /* save_reg */
            RtlpArm64RestoreRegs(19 + ((Val >> 6) & 0xF), 1, Val & 0x3F, Context, Ptrs);
        else if (*Ptr < 0xD6)   /* save_reg_x */
            RtlpArm64RestoreRegs(19 + ((Val >> 5) & 0xF), 1, -(LONG)(Val & 0x1F) - 1, Context, Ptrs);
        else if (*Ptr < 0xD8)   /* save_lrpair */
        {
            RtlpArm64RestoreRegs(19 + 2 * ((Val >> 6) & 0x7), 1, Val & 0x3F, Context, Ptrs);
            RtlpArm64RestoreRegs(30, 1, (Val & 0x3F) + 1, Context, Ptrs);
        }
        else if (*Ptr < 0xDA)   /* save_fregp */
            RtlpArm64RestoreFpRegs(8 + ((Val >> 6) & 0x7), SaveNext, Val & 0x3F, Context, Ptrs);
        else if (*Ptr < 0xDC)   /* save_fregp_x */
            RtlpArm64RestoreFpRegs(8 + ((Val >> 6) & 0x7), SaveNext, -(LONG)(Val & 0x3F) - 1, Context, Ptrs);
        else if (*Ptr < 0xDE)   /* save_freg */
            RtlpArm64RestoreFpRegs(8 + ((Val >> 6) & 0x7), 1, Val & 0x3F, Context, Ptrs);
        else if (*Ptr == 0xDE)  /* save_freg_x */
            RtlpArm64RestoreFpRegs(8 + ((Val >> 5) & 0x7), 1, -(LONG)(Val & 0x3F) - 1, Context, Ptrs);
        else if (*Ptr == 0xE0)  /* alloc_l */
            Context->Sp += 16 * (((ULONG)Ptr[1] << 16) + ((ULONG)Ptr[2] << 8) + Ptr[3]);
        else if (*Ptr == 0xE1)  /* set_fp */
            Context->Sp = Context->Fp;
        else if (*Ptr == 0xE2)  /* add_fp */
            Context->Sp = Context->Fp - 8 * (Val & 0xFF);
        else if (*Ptr == 0xE3)  /* nop */
            ;
        else if (*Ptr == 0xE4)  /* end */
            break;
        else if (*Ptr == 0xE5)  /* end_c */
            ;
        else if (*Ptr == 0xE6)  /* save_next */
        {
            SaveNext += 2;
            Ptr += Len;
            continue;
        }
        else if (*Ptr == 0xE7)  /* save_any_reg */
        {
            RtlpArm64RestoreAnyReg(Ptr[1], (Ptr[1] & 0x40) ? SaveNext : 1,
                                   Ptr[2] >> 6, Ptr[2] & 0x3F, Context, Ptrs);
        }
        else if (*Ptr == 0xE9)  /* MSFT_OP_MACHINE_FRAME */
        {
            Context->Pc = ((PDWORD64)Context->Sp)[1];
            Context->Sp = ((PDWORD64)Context->Sp)[0];
            Context->ContextFlags &= ~CONTEXT_UNWOUND_TO_CALL;
            *FinalPcFromLr = FALSE;
        }
        else if (*Ptr == 0xEA)  /* MSFT_OP_CONTEXT */
        {
            ULONG Flags = Context->ContextFlags & ~CONTEXT_UNWOUND_TO_CALL;
            PCONTEXT SrcContext = (PCONTEXT)Context->Sp;

            *Context = *SrcContext;
            Context->ContextFlags = Flags | (SrcContext->ContextFlags & CONTEXT_UNWOUND_TO_CALL);
            if (Ptrs)
            {
                for (i = 19; i < 29; i++)
                    (&Ptrs->X19)[i - 19] = &SrcContext->X[i];
                for (i = 8; i < 16; i++)
                    (&Ptrs->D8)[i - 8] = &SrcContext->V[i].Low;
            }
            *FinalPcFromLr = FALSE;
        }
        else if (*Ptr == 0xEC)  /* MSFT_OP_CLEAR_UNWOUND_TO_CALL */
        {
            Context->Pc = Context->Lr;
            Context->ContextFlags &= ~CONTEXT_UNWOUND_TO_CALL;
            *FinalPcFromLr = FALSE;
        }
        else if (*Ptr == 0xFC)  /* pac_sign_lr */
        {
            RtlpArm64PacAuth(Context);
        }
        else
        {
            DPRINT1("RtlpArm64ProcessUnwindCodes: unsupported code %02x\n", *Ptr);
            return;
        }
        SaveNext = 2;
        Ptr += Len;
    }
}

static PEXCEPTION_ROUTINE
RtlpArm64UnwindPackedData(
    _In_ ULONG_PTR ImageBase,
    _In_ ULONG_PTR ControlPc,
    _In_ PRUNTIME_FUNCTION FunctionEntry,
    _Inout_ PCONTEXT Context,
    _Inout_opt_ PKNONVOLATILE_CONTEXT_POINTERS Ptrs)
{
    LONG i;
    ULONG Len, Offset;
    ULONG IntSize = FunctionEntry->RegI * 8;
    ULONG FpSize = FunctionEntry->RegF * 8;
    ULONG RegSave, LocalSize;
    ULONG IntRegs, FpRegs, SavedRegs, Homing = FunctionEntry->H;
    UCHAR Prologue[40], *PrologueEnd, Epilogue[40], *EpilogueEnd;
    ULONG PPos = 0, EPos = 0;
    BOOLEAN FinalPcFromLr = TRUE;

    if (FunctionEntry->CR == 1)
        IntSize += 8;
    if (FunctionEntry->RegF)
        FpSize += 8;

    RegSave = ((IntSize + FpSize + 8 * 8 * FunctionEntry->H) + 0xF) & ~0xF;
    LocalSize = FunctionEntry->FrameSize * 16 - RegSave;

    IntRegs = IntSize / 8;
    FpRegs = FpSize / 8;
    SavedRegs = RegSave / 8;

    Offset = ((ControlPc - ImageBase) - FunctionEntry->BeginAddress) / 4;

    if (FunctionEntry->H && FunctionEntry->RegI == 0 && FunctionEntry->RegF == 0 &&
        FunctionEntry->CR != 1)
    {
        LocalSize += RegSave;
        Homing = 0;
    }

    /* Synthesize prologue opcodes */
#define WRITE_ONE(x) do { Prologue[PPos++] = Epilogue[EPos++] = (x); } while (0)
#define WRITE_TWO(x) do { WRITE_ONE((x) >> 8); WRITE_ONE((x) & 0xFF); } while (0)
    if (FunctionEntry->CR == 2 || FunctionEntry->CR == 3)
    {
        WRITE_ONE(0xE1); /* set_fp */
        if (LocalSize <= 512)
        {
            WRITE_ONE(0x80 | (LocalSize / 8 - 1)); /* save_fplr_x */
        }
        else
        {
            WRITE_ONE(0x40); /* save_fplr */
        }
    }
    if ((FunctionEntry->CR <= 1 && LocalSize > 0) || LocalSize > 512)
    {
        if (LocalSize <= 512)
        {
            WRITE_ONE(LocalSize / 16); /* alloc_s */
        }
        else if (LocalSize <= 4080)
        {
            WRITE_TWO(0xC000 | (LocalSize / 16)); /* alloc_m */
        }
        else
        {
            WRITE_ONE((LocalSize - 4080) / 16); /* alloc_s */
            WRITE_TWO(0xC000 | (4080 / 16)); /* alloc_m */
        }
    }
    if (Homing)
    {
        Prologue[PPos++] = 0xE3; /* nop */
        Prologue[PPos++] = 0xE3; /* nop */
        Prologue[PPos++] = 0xE3; /* nop */
        Prologue[PPos++] = 0xE3; /* nop */
    }
    if (FunctionEntry->RegF > 0)
    {
        if (FunctionEntry->RegF % 2 == 0)
        {
            WRITE_TWO(0xDC00 | ((FunctionEntry->RegF) << 6) | (IntRegs + FpRegs - 1)); /* save_freg */
        }
        for (i = (FunctionEntry->RegF + 1) / 2 - 1; i >= 0; i--)
        {
            if (!i && !IntSize)
                WRITE_TWO(0xDA00 | ((0) << 6) | (SavedRegs - 1)); /* save_fregp_x */
            else
                WRITE_TWO(0xD800 | ((2 * i) << 6) | (IntRegs + 2 * i)); /* save_fregp */
        }
    }
    if (FunctionEntry->CR == 1 && FunctionEntry->RegI % 2 == 0)
    {
        if (FunctionEntry->RegI == 0)
            WRITE_TWO(0xD400 | ((30 - 19) << 5) | (SavedRegs - 1)); /* save_reg_x x30 */
        else
            WRITE_TWO(0xD000 | ((30 - 19) << 6) | (IntRegs - 1)); /* save_reg x30 */
    }
    if (FunctionEntry->RegI > 0)
    {
        if (FunctionEntry->RegI % 2)
        {
            if (FunctionEntry->CR == 1)
            {
                WRITE_TWO(0xD600 | (((FunctionEntry->RegI - 1) / 2) << 6) | (IntRegs - 2)); /* save_lrpair */
                if (FunctionEntry->RegI == 1)
                    WRITE_ONE(SavedRegs / 2); /* alloc_s */
            }
            else
            {
                if (FunctionEntry->RegI == 1)
                    WRITE_TWO(0xD400 | ((0) << 5) | (SavedRegs - 1)); /* save_reg_x x19 */
                else
                    WRITE_TWO(0xD000 | ((IntRegs - 1) << 6) | (IntRegs - 1)); /* save_reg */
            }
        }
        for (i = FunctionEntry->RegI / 2 - 1; i >= 0; i--)
        {
            if (i)
                WRITE_TWO(0xC800 | ((2 * i) << 6) | (2 * i)); /* save_regp */
            else
                WRITE_TWO(0xCC00 | ((0) << 6) | (SavedRegs - 1)); /* save_regp_x x19 */
        }
    }
    if (FunctionEntry->CR == 2)
        WRITE_ONE(0xFC); /* pac_sign_lr */
    WRITE_ONE(0xE4); /* end */
    PrologueEnd = &Prologue[PPos];
    EpilogueEnd = &Epilogue[EPos];
#undef WRITE_ONE
#undef WRITE_TWO

    if (FunctionEntry->Flag == 1)
    {
        if (Offset < (ULONG)(PrologueEnd - Prologue) ||
            Offset >= FunctionEntry->FunctionLength - (ULONG)(EpilogueEnd - Epilogue))
        {
            /* Check prologue */
            Len = RtlpArm64GetSequenceLength(Prologue, PrologueEnd);
            if (Offset < Len)
            {
                RtlpArm64ProcessUnwindCodes(Prologue, PrologueEnd, Context, Ptrs,
                                            Len - Offset, &FinalPcFromLr);
                return NULL;
            }
            /* Check epilogue */
            Len = RtlpArm64GetSequenceLength(Epilogue, EpilogueEnd);
            if (Offset >= FunctionEntry->FunctionLength - (Len + 1))
            {
                RtlpArm64ProcessUnwindCodes(Epilogue, EpilogueEnd, Context, Ptrs,
                                            Offset - (FunctionEntry->FunctionLength - (Len + 1)),
                                            &FinalPcFromLr);
                return NULL;
            }
        }
    }

    /* Execute full prologue */
    RtlpArm64ProcessUnwindCodes(Prologue, PrologueEnd, Context, Ptrs, 0, &FinalPcFromLr);
    return NULL;
}

typedef struct _ARM64_UNWIND_INFO_EPILOG
{
    ULONG Offset : 18;
    ULONG Res : 4;
    ULONG Index : 10;
} ARM64_UNWIND_INFO_EPILOG;

typedef struct _ARM64_UNWIND_INFO_EXT
{
    USHORT Epilog;
    UCHAR Codes;
    UCHAR Reserved;
} ARM64_UNWIND_INFO_EXT;

static PEXCEPTION_ROUTINE
RtlpArm64UnwindFullData(
    _In_ ULONG_PTR ImageBase,
    _In_ ULONG_PTR ControlPc,
    _In_ PRUNTIME_FUNCTION FunctionEntry,
    _Inout_ PCONTEXT Context,
    _Out_ PVOID *HandlerData,
    _Inout_opt_ PKNONVOLATILE_CONTEXT_POINTERS Ptrs,
    _Inout_ PBOOLEAN FinalPcFromLr)
{
    PIMAGE_ARM64_RUNTIME_FUNCTION_ENTRY_XDATA Info;
    ARM64_UNWIND_INFO_EPILOG *InfoEpilog;
    ULONG i, Codes, Epilogs, Len, Offset;
    PVOID Data;
    PUCHAR End;

    Info = (PIMAGE_ARM64_RUNTIME_FUNCTION_ENTRY_XDATA)(ImageBase + FunctionEntry->UnwindData);
    Data = Info + 1;
    Epilogs = Info->EpilogCount;
    Codes = Info->CodeWords;
    if (!Codes && !Epilogs)
    {
        ARM64_UNWIND_INFO_EXT *InfoExt = Data;
        Codes = InfoExt->Codes;
        Epilogs = InfoExt->Epilog;
        Data = InfoExt + 1;
    }
    InfoEpilog = Data;
    if (!Info->EpilogInHeader)
        Data = InfoEpilog + Epilogs;

    Offset = ((ControlPc - ImageBase) - FunctionEntry->BeginAddress) / 4;
    End = (PUCHAR)Data + Codes * 4;

    /* Check for prologue */
    if (Offset < Codes * 4)
    {
        Len = RtlpArm64GetSequenceLength(Data, End);
        if (Offset < Len)
        {
            RtlpArm64ProcessUnwindCodes(Data, End, Context, Ptrs, Len - Offset, FinalPcFromLr);
            return NULL;
        }
    }

    /* Check for epilogue */
    if (!Info->EpilogInHeader)
    {
        for (i = 0; i < Epilogs; i++)
        {
            if (Offset < InfoEpilog[i].Offset)
                break;
            if (Offset - InfoEpilog[i].Offset < Codes * 4 - InfoEpilog[i].Index)
            {
                PUCHAR Ptr = (PUCHAR)Data + InfoEpilog[i].Index;
                Len = RtlpArm64GetSequenceLength(Ptr, End);
                if (Offset <= InfoEpilog[i].Offset + Len)
                {
                    RtlpArm64ProcessUnwindCodes(Ptr, End, Context, Ptrs,
                                                Offset - InfoEpilog[i].Offset, FinalPcFromLr);
                    return NULL;
                }
            }
        }
    }
    else if (Info->FunctionLength - Offset <= Codes * 4 - Epilogs)
    {
        PUCHAR Ptr = (PUCHAR)Data + Epilogs;
        Len = RtlpArm64GetSequenceLength(Ptr, End) + 1;
        if (Offset >= Info->FunctionLength - Len)
        {
            RtlpArm64ProcessUnwindCodes(Ptr, End, Context, Ptrs,
                                        Offset - (Info->FunctionLength - Len), FinalPcFromLr);
            return NULL;
        }
    }

    RtlpArm64ProcessUnwindCodes(Data, End, Context, Ptrs, 0, FinalPcFromLr);

    /* Get handler since we are inside the main code */
    if (Info->ExceptionDataPresent)
    {
        PULONG HandlerRva = (PULONG)Data + Codes;
        *HandlerData = HandlerRva + 1;
        return (PEXCEPTION_ROUTINE)(ImageBase + *HandlerRva);
    }
    return NULL;
}

PEXCEPTION_ROUTINE
NTAPI
RtlVirtualUnwind(
    _In_ ULONG HandlerType,
    _In_ ULONG64 ImageBase,
    _In_ ULONG64 ControlPc,
    _In_ PRUNTIME_FUNCTION FunctionEntry,
    _Inout_ PCONTEXT Context,
    _Out_ PVOID *HandlerData,
    _Out_ PULONG64 EstablisherFrame,
    _Inout_opt_ PVOID ContextPointers)
{
    PEXCEPTION_ROUTINE Handler;
    BOOLEAN FinalPcFromLr = TRUE;
    PVOID Data = NULL;

    (VOID)HandlerType;

    if (HandlerData)
        *HandlerData = NULL;

    Context->ContextFlags |= CONTEXT_UNWOUND_TO_CALL;

    if (FunctionEntry == NULL)
    {
        /* Leaf function */
        if (ControlPc == Context->Lr)
        {
            /* Invalid leaf function: would loop forever */
            *EstablisherFrame = Context->Sp;
            Context->Pc = 0;
            return NULL;
        }
        Context->Pc = Context->Lr;
        *EstablisherFrame = Context->Sp;
        return NULL;
    }

    if (FunctionEntry->Flag)
        Handler = RtlpArm64UnwindPackedData(ImageBase, ControlPc, FunctionEntry,
                                            Context, ContextPointers);
    else
        Handler = RtlpArm64UnwindFullData(ImageBase, ControlPc, FunctionEntry,
                                          Context, &Data, ContextPointers,
                                          &FinalPcFromLr);

    if (FinalPcFromLr)
        Context->Pc = Context->Lr;
    *EstablisherFrame = Context->Sp;

    if (HandlerData)
        *HandlerData = Data;
    return Handler;
}

VOID
NTAPI
RtlRestoreContext(
    _In_ PCONTEXT Context,
    _In_opt_ PEXCEPTION_RECORD ExceptionRecord)
{
    if (ExceptionRecord &&
        ExceptionRecord->ExceptionCode == STATUS_LONGJUMP &&
        ExceptionRecord->NumberParameters >= 1)
    {
        struct
        {
            unsigned long long Frame, Reserved;
            unsigned long long X19, X20, X21, X22, X23, X24, X25, X26, X27, X28;
            unsigned long long Fp, Lr, Sp;
            unsigned long Fpcr, Fpsr;
            double D[8];
        } *JmpBuf = (PVOID)ExceptionRecord->ExceptionInformation[0];
        ULONG i;

        Context->X19 = JmpBuf->X19;
        Context->X20 = JmpBuf->X20;
        Context->X21 = JmpBuf->X21;
        Context->X22 = JmpBuf->X22;
        Context->X23 = JmpBuf->X23;
        Context->X24 = JmpBuf->X24;
        Context->X25 = JmpBuf->X25;
        Context->X26 = JmpBuf->X26;
        Context->X27 = JmpBuf->X27;
        Context->X28 = JmpBuf->X28;
        Context->Fp  = JmpBuf->Fp;
        Context->Pc  = JmpBuf->Lr;
        Context->Sp  = JmpBuf->Sp;
        Context->Fpcr = JmpBuf->Fpcr;
        Context->Fpsr = JmpBuf->Fpsr;
        for (i = 0; i < 8; i++)
            Context->V[8 + i].D[0] = JmpBuf->D[i];
    }

    RtlpRestoreContextInternal(Context);
}

/* Local helper: unwind one frame, refreshing the dispatcher context */
static NTSTATUS
RtlpArm64VirtualUnwindFrame(
    _In_ ULONG HandlerType,
    _Inout_ PDISPATCHER_CONTEXT DispatcherContext,
    _Inout_ PCONTEXT Context)
{
    DISPATCHER_CONTEXT_NONVOLREG_ARM64 *NonVolRegs;
    DWORD64 Pc = Context->Pc;
    ULONG64 ImageBase;
    ULONG i;

    DispatcherContext->ScopeIndex = 0;
    DispatcherContext->ControlPc = Pc;
    DispatcherContext->ControlPcIsUnwound =
        (Context->ContextFlags & CONTEXT_UNWOUND_TO_CALL) != 0;
    if (DispatcherContext->ControlPcIsUnwound)
        Pc -= 4;

    NonVolRegs = (DISPATCHER_CONTEXT_NONVOLREG_ARM64 *)DispatcherContext->NonVolatileRegisters;
    if (NonVolRegs)
    {
        RtlCopyMemory(NonVolRegs->GpNvRegs, &Context->X19, sizeof(NonVolRegs->GpNvRegs));
        for (i = 0; i < NONVOL_FP_NUMREG_ARM64; i++)
            NonVolRegs->FpNvRegs[i] = Context->V[i + 8].D[0];
    }

    DispatcherContext->FunctionEntry = RtlLookupFunctionEntry(Pc,
                                                              &ImageBase,
                                                              DispatcherContext->HistoryTable);
    DispatcherContext->ImageBase = ImageBase;

    if ((DispatcherContext->FunctionEntry == NULL) && (Pc == Context->Lr))
    {
        /* Invalid leaf function, no way to continue */
        return STATUS_INVALID_DISPOSITION;
    }

    DispatcherContext->LanguageHandler =
        RtlVirtualUnwind(HandlerType,
                         ImageBase,
                         Pc,
                         DispatcherContext->FunctionEntry,
                         Context,
                         &DispatcherContext->HandlerData,
                         (PULONG64)&DispatcherContext->EstablisherFrame,
                         NULL);

    if (Context->Pc == 0)
        return STATUS_INVALID_DISPOSITION;

    return STATUS_SUCCESS;
}

static BOOLEAN
RtlpArm64IsValidFrame(
    _In_ ULONG64 Frame)
{
    ULONG_PTR StackLow, StackHigh;

    RtlpGetStackLimits(&StackLow, &StackHigh);
    return ((Frame >= StackLow) &&
            (Frame <= StackHigh) &&
            ((Frame & 0x7) == 0));
}

VOID
NTAPI
RtlUnwindEx(
    _In_opt_ PVOID TargetFrame,
    _In_opt_ PVOID TargetIp,
    _In_opt_ PEXCEPTION_RECORD ExceptionRecord,
    _In_ PVOID ReturnValue,
    _In_ PCONTEXT ContextRecord,
    _Inout_opt_ PVOID HistoryTable)
{
    DISPATCHER_CONTEXT_NONVOLREG_ARM64 NonVolRegs;
    EXCEPTION_RECORD LocalExceptionRecord;
    DISPATCHER_CONTEXT DispatcherContext;
    CONTEXT NewContext;
    NTSTATUS Status;
    ULONG64 Frame;
    EXCEPTION_DISPOSITION Disposition;
    PVOID HandlerData;

    RtlCaptureContext(ContextRecord);
    NewContext = *ContextRecord;

    /* Build an exception record, if we do not have one */
    if (ExceptionRecord == NULL)
    {
        RtlZeroMemory(&LocalExceptionRecord, sizeof(LocalExceptionRecord));
        LocalExceptionRecord.ExceptionCode = STATUS_UNWIND;
        LocalExceptionRecord.ExceptionAddress = (PVOID)(ULONG_PTR)ContextRecord->Pc;
        ExceptionRecord = &LocalExceptionRecord;
    }

    ExceptionRecord->ExceptionFlags |= EXCEPTION_UNWINDING;
    if (TargetFrame == NULL)
        ExceptionRecord->ExceptionFlags |= EXCEPTION_EXIT_UNWIND;

    DispatcherContext.TargetPc = (ULONG64)(ULONG_PTR)TargetIp;
    DispatcherContext.ContextRecord = ContextRecord;
    DispatcherContext.HistoryTable = HistoryTable;
    DispatcherContext.NonVolatileRegisters = NonVolRegs.Buffer;

    for (;;)
    {
        Status = RtlpArm64VirtualUnwindFrame(UNW_FLAG_UHANDLER, &DispatcherContext, &NewContext);
        if (Status != STATUS_SUCCESS)
            RtlRaiseStatus(STATUS_BAD_STACK);

    UnwindDone:
        if (!DispatcherContext.EstablisherFrame)
            break;

        if (!RtlpArm64IsValidFrame(DispatcherContext.EstablisherFrame))
        {
            DPRINT1("RtlUnwindEx: invalid frame %p\n",
                    (PVOID)(ULONG_PTR)DispatcherContext.EstablisherFrame);
            ExceptionRecord->ExceptionFlags |= EXCEPTION_STACK_INVALID;
            break;
        }

        if (DispatcherContext.LanguageHandler)
        {
            if (TargetFrame && (DispatcherContext.EstablisherFrame > (ULONG64)(ULONG_PTR)TargetFrame))
            {
                DPRINT1("RtlUnwindEx: invalid end frame %p/%p\n",
                        (PVOID)(ULONG_PTR)DispatcherContext.EstablisherFrame, TargetFrame);
                RtlRaiseStatus(STATUS_INVALID_UNWIND_TARGET);
            }
            if (DispatcherContext.EstablisherFrame == (ULONG64)(ULONG_PTR)TargetFrame)
                ExceptionRecord->ExceptionFlags |= EXCEPTION_TARGET_UNWIND;

            Disposition = RtlpCallUnwindHandler(ExceptionRecord,
                                                DispatcherContext.EstablisherFrame,
                                                DispatcherContext.ContextRecord,
                                                &DispatcherContext,
                                                DispatcherContext.LanguageHandler);

            switch (Disposition)
            {
                case ExceptionContinueSearch:
                    ExceptionRecord->ExceptionFlags &= ~EXCEPTION_COLLIDED_UNWIND;
                    break;
                case ExceptionCollidedUnwind:
                    NewContext = *ContextRecord;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER,
                                     DispatcherContext.ImageBase,
                                     DispatcherContext.ControlPc,
                                     DispatcherContext.FunctionEntry,
                                     &NewContext,
                                     &HandlerData,
                                     &Frame,
                                     NULL);
                    ExceptionRecord->ExceptionFlags |= EXCEPTION_COLLIDED_UNWIND;
                    goto UnwindDone;
                default:
                    RtlRaiseStatus(STATUS_INVALID_DISPOSITION);
                    break;
            }
        }

        if (DispatcherContext.EstablisherFrame == (ULONG64)(ULONG_PTR)TargetFrame)
            break;

        *ContextRecord = NewContext;
    }

    if (ExceptionRecord->ExceptionCode != STATUS_UNWIND_CONSOLIDATE)
        ContextRecord->Pc = (ULONG64)(ULONG_PTR)TargetIp;

    ContextRecord->X0 = (ULONG64)(ULONG_PTR)ReturnValue;
    RtlRestoreContext(ContextRecord, ExceptionRecord);
}

VOID
NTAPI
RtlUnwind(
    _In_opt_ PVOID TargetFrame,
    _In_opt_ PVOID TargetIp,
    _In_opt_ PEXCEPTION_RECORD ExceptionRecord,
    _In_ PVOID ReturnValue)
{
    CONTEXT ContextRecord;

    RtlUnwindEx(TargetFrame,
                TargetIp,
                ExceptionRecord,
                ReturnValue,
                &ContextRecord,
                NULL);
}

BOOLEAN
NTAPI
RtlAddFunctionTable(
    _In_ PRUNTIME_FUNCTION FunctionTable,
    _In_ ULONG EntryCount,
    _In_ ULONG_PTR BaseAddress)
{
    (VOID)FunctionTable;
    (VOID)EntryCount;
    (VOID)BaseAddress;
    return FALSE;
}

BOOLEAN
NTAPI
RtlDeleteFunctionTable(
    _In_ PRUNTIME_FUNCTION FunctionTable)
{
    (VOID)FunctionTable;
    return FALSE;
}
