/*
 * PROJECT:     ReactOS HAL
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     AMD64 Application Processor (AP) spinup trampoline
 * COPYRIGHT:   Copyright 2023 Justin Miller <justin.miller@reactos.org>
 */

/*
 * The boot CPU calls HalStartNextProcessor for each AP. That routine copies
 * the bytes from HalpAPEntry16 through HalpAPEntry16End into the low-memory
 * stub page (HalpLowStub), patches the data block (HalpAPEntryData) with
 * runtime values, and then asks the local APIC to send INIT-SIPI-SIPI to
 * the AP. The SIPI vector takes the high 8 bits of the trampoline's
 * physical address (page-aligned, < 1 MB).
 *
 * Stage layout in the stub page after copying:
 *
 *  +0000  HalpAPEntry16     16-bit real-mode entry: load a temporary GDT,
 *                           switch directly to long mode by setting
 *                           CR4.PAE, EFER.LME|NXE|SCE, and CR0.PG|PE in
 *                           one shot, then far-jump (66 EA op16:32) to
 *                           the 64-bit code at HalpAPEntry64.
 *
 *         HalpAPEntryData   runtime-filled fields (Jump32Offset, segment,
 *                           SelfPtr, PageTableRoot, KxLoaderBlock,
 *                           ProcessorState, temp GDTR + temp GDT).
 *
 *         HalpAPEntry64     64-bit long-mode handoff: load the real
 *                           per-CPU GDT/IDT/TSS from KPROCESSOR_STATE,
 *                           publish KIPCR via MSR_GS_BASE/MSR_KERNEL_GS_BASE,
 *                           set RSP, then lretq into KiSystemStartup with
 *                           RCX = KeLoaderBlock (Win64 ABI arg0).
 *
 *  +xxxx  HalpAPEntry16End  end marker (byte count to copy).
 *
 * CR3 is left pointing at the temporary PML4 that HalStartNextProcessor
 * built. That PML4 contains a copy of the boot CPU's PML4 entries (so the
 * kernel image, the KPROCESSOR_STATE and the per-AP APINFO are all
 * reachable at their kernel VAs) plus an identity mapping for the
 * trampoline page itself (so this code keeps executing after CR0.PG is
 * set). Loading the boot CPU's CR3 here would unmap the trampoline before
 * we ever reach the lretq -- intentionally avoided. The first context
 * switch on this AP will writecr3() into the real per-process PML4 and
 * the temp PML4 falls out of use.
 */

#include <asm.inc>
#include <ksamd64.inc>

/* Offsets from the start of the trampoline copy, used in the 16-bit code
 * to address the in-page data block via CS-relative loads. */
#define APE_OFF(f) (f - HalpAPEntry16)

/* CR0 / CR4 / EFER bits we need.  Spelled out so the file builds with both
 * MASM and GAS and we don't have to depend on every header being available. */
#define CR0_PE_BIT    HEX(00000001)
#define CR0_MP_BIT    HEX(00000002)
#define CR0_NE_BIT    HEX(00000020)
#define CR0_WP_BIT    HEX(00010000)
#define CR0_PG_BIT    HEX(80000000)

#define CR4_PAE_BIT   HEX(00000020)
#define CR4_PSE_BIT   HEX(00000010)

/* MSR_EFER bits */
#define EFER_SCE      HEX(00000001)
#define EFER_LME      HEX(00000100)
#define EFER_NXE      HEX(00000800)

PUBLIC HalpAPEntry16
PUBLIC HalpAPEntry16End
PUBLIC HalpAPEntryData
PUBLIC HalpAPEntry64

.code

/******************************************************************************
 * Stage 1 -- 16-bit real-mode entry
 *
 * On entry CS = HalpLowStubPhysicalAddress >> 4 and IP = 0; all other
 * segment registers and the stack are undefined. Interrupts are disabled
 * (SIPI clears IF). The CPU is in 8086 real mode with paging off.
 *****************************************************************************/
.code16
HalpAPEntry16:
    cli

    /* Flat ds so we can re-fetch the data block in 32-bit-displacement form
     * later if we want.  ss = 0 keeps a sane stack base even though we
     * don't push/pop until long mode. */
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    /* Load the temporary GDT.  data32 makes lgdt take a 32-bit base, which
     * is what HalStartNextProcessor wrote into HalpAPEntryData.Gdtr32. */
#ifdef _USE_ML
    data32 lgdt fword ptr cs:[APE_OFF(Gdtr32)]
#else
    data32 lgdt cs:[APE_OFF(Gdtr32)]
#endif

    /* Load CR3 with the temporary PML4 root that HalStartNextProcessor
     * built.  It identity-maps this page and also mirrors the boot CPU's
     * upper-half mappings so the per-CPU KPROCESSOR_STATE we'll touch
     * shortly is reachable at its kernel VA. */
    mov eax, cs:[APE_OFF(APE_PageTableRoot)]
    mov cr3, eax

    /* Enable PAE (required for long mode) + PSE (allows 2MB pages in the
     * temp PML4 if HalpSetupTemporaryMappings used them). */
    mov eax, cr4
    or eax, CR4_PAE_BIT or CR4_PSE_BIT
    mov cr4, eax

    /* Enable EFER.LME (long mode), NXE (matches BSP, set in KiInitializeCpu
     * at kiinit.c:231), and SCE (so SYSCALL is allowed once we land in
     * KiSystemStartup -- the BSP sets it for the same reason). */
    mov ecx, HEX(C0000080)              /* MSR_EFER */
    rdmsr
    or eax, EFER_LME or EFER_NXE or EFER_SCE
    wrmsr

    /* Set CR0.PG|PE|MP|NE|WP in one shot.  Combined with EFER.LME this puts
     * us directly into IA-32e mode (the CPU does NOT visit 32-bit
     * protected mode in between), but our CS hidden descriptor is still
     * the real-mode one until we far-jump. */
    mov eax, cr0
    or eax, CR0_PE_BIT or CR0_PG_BIT or CR0_WP_BIT or CR0_MP_BIT or CR0_NE_BIT
    mov cr0, eax

    /* Far-jump to HalpAPEntry64 with the temp GDT's KGDT64_R0_CODE
     * selector (which has L=1).  Encoding is 66 EA <offset:32> <sel:16>.
     * HalStartNextProcessor patched the offset to point at the long-mode
     * absolute address of HalpAPEntry64 inside the stub copy. */
.align 2
    .byte HEX(66)
    .byte HEX(EA)
HalpAPEntryData:
APE_Jump32Offset:
    .long 0                             /* low 32 of HalpAPEntry64 in stub */
APE_Jump32Segment:
    .long HEX(00000010)                 /* KGDT64_R0_CODE (low 16 used) */
APE_SelfPtr:
    .quad 0                             /* phys addr of HalpAPEntryData */
APE_PageTableRoot:
    .quad 0                             /* phys addr of temp PML4 */
APE_KxLoaderBlock:
    .quad 0                             /* virtual addr of KeLoaderBlock */
APE_ProcessorState:
    .quad 0                             /* virtual addr of KPROCESSOR_STATE */
APE_Gdtr32_Pad:
    .word 0                             /* pad so Limit is 4-byte aligned */
Gdtr32:
    .word 0                             /* limit (filled by spinup) */
    .long 0                             /* base  (filled by spinup) */

    .align 8
APE_TempGdt:
    .quad HEX(0000000000000000)         /* sel 0x00: null */
    .quad HEX(0000000000000000)         /* sel 0x08: unused */
    .quad HEX(0020980000000000)         /* sel 0x10: long-mode R0 code, L=1 */
    .quad HEX(00CF92000000FFFF)         /* sel 0x18: R0 data, 4 GB flat */
APE_TempGdtEnd:

HalpAPEntry16End:
.endcode16


/******************************************************************************
 * Stage 2 -- 64-bit long mode handoff
 *
 * Reached by the 66 EA far-jump above.  CS now points at the temp GDT's
 * 0x10 selector (L=1, 64-bit code).  DS/ES/FS/GS/SS are still null
 * selectors; that's fine for long mode but we'll reload them once the
 * real per-CPU GDT is loaded.  RSP is undefined.
 *
 * Everything from here on uses RIP-relative addressing so the same bytes
 * keep working after the trampoline is copied into HalpLowStub.
 *****************************************************************************/
.code64
HalpAPEntry64:

    /* Load DS/ES/SS to the temp GDT's data selector so they're sensible
     * until the real per-CPU GDT/IDT come online.  FS/GS we leave at null
     * here -- their bases will be re-published via MSRs below. */
    mov ax, HEX(0018)                   /* temp GDT data selector */
    mov ds, ax
    mov es, ax
    mov ss, ax

    /* RIP-relative load of the AP's KPROCESSOR_STATE pointer. */
    mov rsi, qword ptr APE_ProcessorState[rip]

    /* Switch to the real per-CPU GDT and IDT that KeStartAllProcessors
     * filled into the AP's KPROCESSOR_STATE.  KDESCRIPTOR has 6 bytes of
     * pad then 2-byte Limit + 8-byte Base; lgdt/lidt in long mode want a
     * 10-byte operand starting at the Limit, hence the +6.  GAS infers
     * the operand size from .code64 — no fword/tbyte keyword needed. */
    lea rax, [rsi + PsGdtr + 6]
    lgdt [rax]
    lea rax, [rsi + PsIdtr + 6]
    lidt [rax]

    /* Load CR4 (BSP may have enabled OSFXSR/OSXMMEXCPT/OSXSAVE etc.) and
     * CR8 (IRQL) from the captured BSP state.  CR3 is intentionally NOT
     * loaded -- see file header. */
    mov rax, qword ptr [rsi + PsCr4]
    mov cr4, rax
    mov rax, qword ptr [rsi + PsCr8]
    mov cr8, rax

    /* Debug registers from KSPECIAL_REGISTERS.KernelDrN. */
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

    /* Publish this AP's KIPCR via the GS base MSRs -- matches what
     * KiInitializeCpu(Pcr) does for the BSP in kiinit.c:169-170. The two
     * MSRs hold the same value so swapgs is a no-op at first. */
    mov ecx, HEX(C0000101)              /* MSR_GS_BASE */
    mov rax, qword ptr [rsi + SrMsrGsBase]
    mov rdx, rax
    shr rdx, 32
    wrmsr

    mov ecx, HEX(C0000102)              /* MSR_KERNEL_GS_BASE */
    mov rax, qword ptr [rsi + SrMsrGsSwap]
    mov rdx, rax
    shr rdx, 32
    wrmsr

    /* Load TSS selector (KGDT64_SYS_TSS).  The descriptor at this slot in
     * the per-CPU GDT points at this AP's KTSS64. */
    ltr word ptr [rsi + PsTr]

    /* Switch to the kernel stack KeStartAllProcessors allocated for this
     * AP.  The Rsp value was 16-byte aligned at fill time. */
    mov rsp, qword ptr [rsi + PsContextFrame + CxRsp]

    /* Win64 ABI: first arg in RCX.  KiSystemStartup(LoaderBlock). */
    mov rcx, qword ptr APE_KxLoaderBlock[rip]

    /* Build a long-mode far-return frame:
     *   [rsp]   = target RIP (KiSystemStartup)
     *   [rsp+8] = target CS  (KGDT64_R0_CODE, now from per-CPU GDT)
     * lretq pops RIP then CS, atomically switching CS to the per-CPU
     * GDT's 0x10 entry.  The temp GDT is no longer reachable after this. */
    mov rax, qword ptr [rsi + PsContextFrame + CxRip]
    mov rdx, KGDT64_R0_CODE
    push rdx
    push rax
    lret

END
