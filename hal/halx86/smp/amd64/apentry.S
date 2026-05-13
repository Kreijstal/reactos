/*
 * PROJECT:     ReactOS HAL
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     AMD64 Application Processor (AP) spinup trampoline
 * COPYRIGHT:   Copyright 2023 Justin Miller <justin.miller@reactos.org>
 */

/*
 * Three-stage AP wakeup, mirroring Linux's
 * arch/x86/realmode/rm/trampoline_64.S sequence.
 *
 * Stage 0 (HalpAPEntry16, .code16) -- real mode 16-bit:
 *   cli; clear segs; lgdt; CR4.PAE; CR3; CR0.PE.
 *   Note: EFER.LME is intentionally DEFERRED to stage 1. With LME=1
 *   set while the CPU is still in 16-bit protected mode, the 32-bit-
 *   operand far-jump (66 EA off:32 seg:16) misbehaves under both
 *   KVM and TCG (the AP ends up at offset+6 instead of offset, with
 *   the operand bytes ending up as instruction bytes -- triple
 *   fault). Enabling LME only after we're in 32-bit protected mode
 *   avoids the problem.
 *
 *   Far-jump (66 EA off:32 seg:16) to sel 0x20 (32-bit code, base 0).
 *   The 32-bit offset is the absolute linear address of
 *   HalpAPEntry32 inside the stub copy; HalStartNextProcessor patches
 *   it at runtime.
 *
 * Stage 1 (HalpAPEntry32, .code32) -- 32-bit protected mode:
 *   Reload data segments to sel 0x18. Enable EFER.LME, then CR0.PG
 *   (with EFER.LME=1 this activates IA-32e in 32-bit compat submode).
 *   Far-jump (32-bit direct EA off:32 seg:16, no prefix needed in
 *   .code32) to sel 0x10 (64-bit code, base 0). The 32-bit offset is
 *   the absolute linear address of HalpAPEntry64; spinup writes it
 *   into APE_Jump64Offset and we use an indirect FF /5 far-jump to
 *   pick it up.
 *
 * Stage 2 (HalpAPEntry64, .code64) -- 64-bit long mode:
 *   Load the real per-CPU GDT/IDT from KPROCESSOR_STATE, publish
 *   MSR_GS_BASE/MSR_KERNEL_GS_BASE, ltr, set RSP, then lret into
 *   KiSystemStartup(LoaderBlock) with RCX=KeLoaderBlock.
 *
 * CR3 keeps pointing at the temp PML4 (built by
 * HalpSetupTemporaryMappings) for the AP's lifetime up to first
 * context switch. The boot CPU's CR3 is NOT loaded -- it has no
 * low identity mapping and would unmap the stub the trampoline runs in.
 */

#include <asm.inc>
#include <ksamd64.inc>

#define APE_OFF(f) (f - HalpAPEntry16)

PUBLIC HalpAPEntry16
PUBLIC HalpAPEntry16End
PUBLIC HalpAPEntryData
PUBLIC HalpAPEntry32
PUBLIC HalpAPEntry64
PUBLIC HalpAPEntryEnd
PUBLIC APE_IndirectJumpOperandSite

.code

/******************************************************************************
 * Stage 0 -- 16-bit real-mode entry
 *****************************************************************************/
.code16
HalpAPEntry16:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

#ifdef _USE_ML
    data32 lgdt fword ptr cs:[APE_OFF(Gdtr32)]
#else
    data32 lgdt cs:[APE_OFF(Gdtr32)]
#endif

    /* CR4.PAE (required for long mode; safe to set in 16-bit prot
     * mode without LME). */
    mov eax, cr4
    or eax, HEX(20)
    mov cr4, eax

    /* CR3 = temp PML4 (read in 16-bit, but won't take effect until
     * CR0.PG is set in stage 1). */
    mov eax, cs:[APE_OFF(APE_PageTableRoot)]
    mov cr3, eax

    /* CR0.PE only -- enter 16-bit protected mode. No PG, no LME yet. */
    mov eax, cr0
    or eax, HEX(1)
    mov cr0, eax

    /* 32-bit-operand far-jump (66 EA off:32 seg:16) to sel 0x20
     * (32-bit code, base 0). The 32-bit offset is the absolute linear
     * address of HalpAPEntry32 (== HalpLowStubPhysicalAddress +
     * APE_OFF(HalpAPEntry32)). Spinup patches the offset. */
    .byte HEX(66)
    .byte HEX(EA)
HalpAPEntryData:
APE_Jump32To32Offset:
    .long 0                             /* patched: lin addr of HalpAPEntry32 */
APE_Jump32To32Segment:
    .long HEX(00000020)                 /* sel 0x20 (32-bit code, base 0) */
APE_Jump64Offset:
    .long 0                             /* patched: lin addr of HalpAPEntry64 */
APE_Jump64Segment:
    .long HEX(00000010)                 /* sel 0x10 (64-bit code, base 0) */
APE_SelfPtr:
    .quad 0
APE_PageTableRoot:
    .quad 0
APE_KxLoaderBlock:
    .quad 0
APE_ProcessorState:
    .quad 0
APE_Gdtr32_Pad:
    .word 0
Gdtr32:
    .word 0
    .long 0

APE_TempGdt:
    .quad HEX(0000000000000000)         /* 0x00: null */
    .quad HEX(0000000000000000)         /* 0x08: unused */
    .quad HEX(00209A0000000000)         /* 0x10: 64-bit code R0, L=1 */
    .quad HEX(00CF92000000FFFF)         /* 0x18: 32-bit data R0, 4 GB flat */
    .quad HEX(00CF9A000000FFFF)         /* 0x20: 32-bit code R0, base 0,
                                         *       4 GB flat */
APE_TempGdtEnd:

HalpAPEntry16End:
.endcode16


/******************************************************************************
 * Stage 1 -- 32-bit protected mode bridge
 *****************************************************************************/
.code32
HalpAPEntry32:
    /* Reload data segments to sel 0x18 (32-bit data, base 0, 4 GB flat). */
    mov ax, HEX(0018)
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    /* EFER.LME (deferred from stage 0 to avoid the 16-bit -> 32-bit
     * far-jump glitch). NXE too. SCE is deferred to stage 2. */
    mov ecx, HEX(C0000080)              /* MSR_EFER */
    rdmsr
    or eax, HEX(900)                    /* LME | NXE */
    wrmsr

    /* CR0.PG -- with EFER.LME=1 this activates IA-32e mode in 32-bit
     * compatibility submode. */
    mov eax, cr0
    or eax, HEX(80000000)               /* PG */
    mov cr0, eax

    /* 32-bit indirect far-jump to sel 0x10 (64-bit code).
     * DS.base=0, so ds:[APE_Jump64Offset_abs] reads the 6-byte
     * (offset:32 + selector:16) from the stub. The disp32 needs the
     * ABSOLUTE linear address of the field -- spinup ensures the
     * AP_ENTRY_DATA.Jump64Offset_Linear field is set up. */
    .byte HEX(FF), HEX(2D)              /* JMP FAR ds:[disp32] */
APE_IndirectJumpOperandSite:
    .long 0                             /* patched: lin addr of APE_Jump64Offset */


/******************************************************************************
 * Stage 2 -- 64-bit long mode handoff
 *****************************************************************************/
.code64
HalpAPEntry64:
    /* DS/ES/SS reload (mostly ignored in long mode but kept consistent). */
    mov ax, HEX(0018)
    mov ds, ax
    mov es, ax
    mov ss, ax

    /* RIP-relative load of ProcessorState. */
    mov rsi, qword ptr APE_ProcessorState[rip]

    /* Per-CPU GDT/IDT. KDESCRIPTOR layout: 6 bytes pad + 2 byte Limit
     * + 8 byte Base; lgdt/lidt want the 10-byte form at +6. */
    lea rax, [rsi + PsGdtr + 6]
    lgdt [rax]
    lea rax, [rsi + PsIdtr + 6]
    lidt [rax]

    /* CR4 from BSP. CR8 = IRQL. CR3 deliberately NOT reloaded. */
    mov rax, qword ptr [rsi + PsCr4]
    mov cr4, rax
    mov rax, qword ptr [rsi + PsCr8]
    mov cr8, rax

    /* EFER.SCE (BSP enables it in KiInitializeCpu). */
    mov ecx, HEX(C0000080)
    rdmsr
    or eax, HEX(1)
    wrmsr

    /* Debug registers. */
    mov rax, qword ptr [rsi + PsKernelDr0]
    mov dr0, rax
    mov rax, qword ptr [rsi + PsKernelDr1]
    mov dr1, rax
    mov rax, qword ptr [rsi + PsKernelDr2]
    mov dr2, rax
    mov rax, qword ptr [rsi + PsKernelDr3]
    mov dr3, rax
    mov rax, qword ptr [rsi + PsKernelDr6]
    mov dr6, rax
    mov rax, qword ptr [rsi + PsKernelDr7]
    mov dr7, rax

    /* Publish AP's KIPCR via the GS base MSRs. */
    mov ecx, HEX(C0000101)
    mov rax, qword ptr [rsi + SrMsrGsBase]
    mov rdx, rax
    shr rdx, 32
    wrmsr

    mov ecx, HEX(C0000102)
    mov rax, qword ptr [rsi + SrMsrGsSwap]
    mov rdx, rax
    shr rdx, 32
    wrmsr

    /* Load TSS. */
    ltr word ptr [rsi + PsTr]

    /* AP kernel stack. */
    mov rsp, qword ptr [rsi + PsContextFrame + CxRsp]

    /* Win64 arg0 = LoaderBlock. */
    mov rcx, qword ptr APE_KxLoaderBlock[rip]

    /* Far-return into KiSystemStartup. In 64-bit mode "lret" without a
     * REX.W prefix pops a 32-bit RIP + 16-bit CS off the stack, which
     * would chop the high 32 bits off KiSystemStartup's kernel address
     * and load them as the CS selector (GPF with error code = RIP>>32).
     * The 64-bit form (lretq, REX.W + CB) pops the 8/8 we actually
     * pushed. */
    mov rax, qword ptr [rsi + PsContextFrame + CxRip]
    mov rdx, KGDT64_R0_CODE
    push rdx
    push rax
    .byte HEX(48), HEX(CB)              /* lretq (REX.W + ret far) */

HalpAPEntryEnd:

END
