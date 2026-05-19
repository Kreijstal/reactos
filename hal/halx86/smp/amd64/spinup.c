/*
 * PROJECT:     ReactOS HAL
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     AMD64 Application Processor (AP) spinup orchestration
 * COPYRIGHT:   Copyright 2023 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES ******************************************************************/

#include <hal.h>
#include <smp.h>

#define NDEBUG
#include <debug.h>

/* GLOBALS *******************************************************************/

extern BOOLEAN HalpOnlyBootProcessor;
extern PPROCESSOR_IDENTITY HalpProcessorIdentity;
extern PHYSICAL_ADDRESS HalpLowStubPhysicalAddress;
extern PVOID HalpLowStub;
extern HALP_APIC_INFO_TABLE HalpApicInfoTable;

#ifndef _NO_AP_TRAMPOLINE_
/* The trampoline image; defined in hal/halx86/smp/amd64/apentry.S. */
extern UCHAR HalpAPEntry16[];
extern UCHAR HalpAPEntryData[];
extern UCHAR HalpAPEntry32[];
extern UCHAR HalpAPEntry64[];
extern UCHAR HalpAPEntry16End[];
extern UCHAR HalpAPEntryEnd[];
extern UCHAR APE_IndirectJumpOperandSite[];
#endif

/* Boot CPU is the first started processor (counted as 1). */
ULONG HalpStartedProcessorCount = 1;

#ifndef Add2Ptr
#define Add2Ptr(P,I) ((PVOID)((PUCHAR)(P) + (I)))
#endif

/*
 * Layout of the in-page data block embedded in the trampoline at
 * HalpAPEntryData. The asm side reads these fields by their fixed
 * offsets, so the struct here must stay in lockstep with apentry.S.
 * Layout is dense (tight, no auto-padding) to match the asm's raw
 * .word/.long/.quad sequence.
 */
typedef struct _AP_ENTRY_DATA
{
    /* +0  Absolute linear address of HalpAPEntry32 inside the stub
     *     copy. Stage-0's 32-bit far-jump (66 EA off:32 seg:16)
     *     reads this as its offset operand. */
    ULONG Jump32To32Offset;

    /* +4  Stage-0 far-jump target selector. 0x20 = temp GDT's 32-bit
     *     code descriptor (base 0, 4 GB flat). Pre-init in asm. */
    ULONG Jump32To32Segment;

    /* +8  Absolute linear address of HalpAPEntry64 inside the stub.
     *     Stage-1's indirect far-jump (FF /5 ds:[disp32]) reads this
     *     and the segment below as a 6-byte m16:32. */
    ULONG Jump64Offset;

    /* +12 Stage-1 far-jump target selector. 0x10 = temp GDT's 64-bit
     *     code descriptor (L=1, base=0). Pre-init in asm. */
    ULONG Jump64Segment;

    /* +16 Diagnostic: physical address of this data block. */
    ULONG64 SelfPtr;

    /* +24 Physical address of the temp PML4. */
    ULONG64 PageTableRoot;

    /* +32 Virtual address of KeLoaderBlock (Win64 arg0). */
    ULONG64 KxLoaderBlock;

    /* +40 Virtual address of the AP's KPROCESSOR_STATE. */
    ULONG64 ProcessorState;

    /* +48 2-byte pad (matches asm; keeps Gdtr32 base 4-aligned). */
    USHORT Gdtr32_Pad;

    /* +50 Temp GDT limit (= sizeof(TempGdt) - 1). */
    USHORT Gdtr32_Limit;

    /* +52 Physical address of the temp GDT inside the stub. */
    ULONG Gdtr32_Base;

    /* +56 Temp GDT: null / unused / KGDT64_R0_CODE (L=1) /
     *     KGDT64_R0_DATA / 32-bit code (base 0). All entries
     *     pre-initialized in asm; spinup does not patch them. */
    ULONG64 TempGdt[5];
} AP_ENTRY_DATA, *PAP_ENTRY_DATA;

C_ASSERT(FIELD_OFFSET(AP_ENTRY_DATA, Jump32To32Offset)  == 0);
C_ASSERT(FIELD_OFFSET(AP_ENTRY_DATA, Jump32To32Segment) == 4);
C_ASSERT(FIELD_OFFSET(AP_ENTRY_DATA, Jump64Offset)      == 8);
C_ASSERT(FIELD_OFFSET(AP_ENTRY_DATA, Jump64Segment)     == 12);
C_ASSERT(FIELD_OFFSET(AP_ENTRY_DATA, SelfPtr)           == 16);
C_ASSERT(FIELD_OFFSET(AP_ENTRY_DATA, PageTableRoot)     == 24);
C_ASSERT(FIELD_OFFSET(AP_ENTRY_DATA, KxLoaderBlock)     == 32);
C_ASSERT(FIELD_OFFSET(AP_ENTRY_DATA, ProcessorState)    == 40);
C_ASSERT(FIELD_OFFSET(AP_ENTRY_DATA, Gdtr32_Pad)        == 48);
C_ASSERT(FIELD_OFFSET(AP_ENTRY_DATA, Gdtr32_Limit)      == 50);
C_ASSERT(FIELD_OFFSET(AP_ENTRY_DATA, Gdtr32_Base)       == 52);
C_ASSERT(FIELD_OFFSET(AP_ENTRY_DATA, TempGdt)           == 56);
C_ASSERT(sizeof(AP_ENTRY_DATA)                          == 96);

/* FUNCTIONS *****************************************************************/

/*
 * Build the temporary 4-level page tables that the AP runs on between
 * the SIPI vector and KiSystemStartup. The 5-page HalpLowStub is laid
 * out as:
 *
 *   Page 0 -- the trampoline code + embedded data block (already copied).
 *   Page 1 -- temp PML4. Mirror of the boot CPU's PML4 (so kernel VAs
 *             stay reachable) with slot [0] replaced by our identity
 *             mapping for the trampoline.
 *   Page 2 -- temp PDPT for the identity mapping (PML4[0] points here).
 *   Page 3 -- temp PD   for the identity mapping.
 *   Page 4 -- temp PT   for the identity mapping. Maps the
 *             trampoline page (and the other four stub pages, for
 *             symmetry) at virtual = physical so the 16-bit-to-64-bit
 *             flip in apentry.S keeps fetching the right code.
 *
 * Loading the boot CPU's CR3 here would unmap the trampoline page
 * (the boot CPU's PML4 has no low identity), so the AP keeps the
 * temp PML4 active all the way through KiSystemStartup. The first
 * context switch on this AP writes the per-process CR3 in
 * KiSwapContextResume; the temp PML4 falls out of use at that point
 * and the 5-page stub stays leaked (acceptable for at most a handful
 * of APs).
 */
#ifndef _NO_AP_TRAMPOLINE_
static PHYSICAL_ADDRESS
HalpSetupTemporaryMappings(VOID)
{
    PMMPTE Pml4 = (PMMPTE)Add2Ptr(HalpLowStub, 1 * PAGE_SIZE);
    PMMPTE Pdpt = (PMMPTE)Add2Ptr(HalpLowStub, 2 * PAGE_SIZE);
    PMMPTE Pd   = (PMMPTE)Add2Ptr(HalpLowStub, 3 * PAGE_SIZE);
    PMMPTE Pt   = (PMMPTE)Add2Ptr(HalpLowStub, 4 * PAGE_SIZE);
    PHYSICAL_ADDRESS Pml4Pa, PdptPa, PdPa, PtPa;
    ULONG StartPti;
    ULONG i;

    /* Copy the boot CPU's PML4 wholesale so kernel-half mappings
     * (image, pool, hyperspace, paged pool, system cache, ...) all
     * survive after the AP loads our temp PML4 into CR3. */
    RtlCopyMemory(Pml4, MiAddressToPxe(NULL), PAGE_SIZE);

    /* Patch PML4[0] to point at our identity-mapping PDPT. This slot
     * is normally the user half of whichever process the boot CPU was
     * in at the moment KeStartAllProcessors ran; we don't care about
     * preserving it -- the AP never executes user code on the temp
     * PML4, and the first context switch will install the real
     * per-process PML4 anyway. */
    PdptPa = MmGetPhysicalAddress(Pdpt);
    Pml4[0].u.Long = 0;
    Pml4[0].u.Hard.Valid = 1;
    Pml4[0].u.Hard.Write = 1;
    Pml4[0].u.Hard.PageFrameNumber = PdptPa.QuadPart >> PAGE_SHIFT;

    /* PDPT[0] -> PD */
    RtlZeroMemory(Pdpt, PAGE_SIZE);
    PdPa = MmGetPhysicalAddress(Pd);
    Pdpt[0].u.Hard.Valid = 1;
    Pdpt[0].u.Hard.Write = 1;
    Pdpt[0].u.Hard.PageFrameNumber = PdPa.QuadPart >> PAGE_SHIFT;

    /* PD[0] -> PT */
    RtlZeroMemory(Pd, PAGE_SIZE);
    PtPa = MmGetPhysicalAddress(Pt);
    Pd[0].u.Hard.Valid = 1;
    Pd[0].u.Hard.Write = 1;
    Pd[0].u.Hard.PageFrameNumber = PtPa.QuadPart >> PAGE_SHIFT;

    /* PT entries: identity-map all 5 stub pages so any of the stub
     * (code, data, temp PML4/PDPT/PD/PT) can be touched without
     * faulting while the AP is running on the temp PML4. Only the
     * code page is strictly required, but mapping all five is no
     * extra cost and keeps the page table self-consistent. */
    RtlZeroMemory(Pt, PAGE_SIZE);
    StartPti = (ULONG)((HalpLowStubPhysicalAddress.QuadPart >> PAGE_SHIFT) & 0x1FF);
    ASSERT(StartPti + HALP_LOW_STUB_SIZE_IN_PAGES <= 512);
    for (i = 0; i < HALP_LOW_STUB_SIZE_IN_PAGES; i++)
    {
        Pt[StartPti + i].u.Hard.Valid = 1;
        Pt[StartPti + i].u.Hard.Write = 1;
        Pt[StartPti + i].u.Hard.PageFrameNumber =
            (HalpLowStubPhysicalAddress.QuadPart >> PAGE_SHIFT) + i;
    }

    Pml4Pa = MmGetPhysicalAddress(Pml4);
    return Pml4Pa;
}
#endif /* _NO_AP_TRAMPOLINE_ */

BOOLEAN
NTAPI
HalStartNextProcessor(
    _In_ PLOADER_PARAMETER_BLOCK LoaderBlock,
    _In_ PKPROCESSOR_STATE ProcessorState)
{
#ifdef _NO_AP_TRAMPOLINE_
    /* ml64 / MSVC builds cannot assemble the .code16 trampoline, so
     * SMP AP bringup is unavailable.  Return FALSE so KeStartAllProcessors
     * stops after the boot CPU, matching the UP HAL behaviour. */
    UNREFERENCED_PARAMETER(LoaderBlock);
    UNREFERENCED_PARAMETER(ProcessorState);
    return FALSE;
#else
    PHYSICAL_ADDRESS TempPml4Pa;
    SIZE_T TrampolineLength;
    SIZE_T DataBlockOffset;
    SIZE_T Entry32Offset;
    SIZE_T Entry64Offset;
    SIZE_T TempGdtOffset;
    PAP_ENTRY_DATA ApData;

    /* Bail out if the user asked for ONECPU. */
    if (HalpOnlyBootProcessor)
        return FALSE;

    /* Bail out once every MADT-reported APIC has been started. The
     * kernel-side KeStartAllProcessors keeps calling us until we
     * return FALSE, so this is the natural termination condition. */
    if (HalpStartedProcessorCount == HalpApicInfoTable.ProcessorCount)
        return FALSE;

    /* Build the temp page tables the trampoline will run on. */
    TempPml4Pa = HalpSetupTemporaryMappings();
    if (!TempPml4Pa.QuadPart)
        return FALSE;

    /* Copy the trampoline (all three stages + embedded data block) into
     * the low-memory stub page. The SIPI vector tells the AP to start at
     * HalpLowStubPhysicalAddress, which is identity-mapped in the temp
     * PML4 we just built. HalpAPEntry16End only marks the end of stage 0
     * (it has to live inside .code16 in apentry.S); the stage-0 far jump
     * lands at HalpAPEntry32 inside the SAME stub page, so we must copy
     * the full HalpAPEntry16..HalpAPEntryEnd range, not just stage 0. */
    TrampolineLength = (SIZE_T)(HalpAPEntryEnd - HalpAPEntry16);
    ASSERT(TrampolineLength <= PAGE_SIZE);
    RtlCopyMemory(HalpLowStub, HalpAPEntry16, TrampolineLength);

    /* Compute link-time offsets so we can patch the *copy* of the
     * trampoline in HalpLowStub. */
    DataBlockOffset = (SIZE_T)(HalpAPEntryData - HalpAPEntry16);
    Entry32Offset   = (SIZE_T)(HalpAPEntry32  - HalpAPEntry16);
    Entry64Offset   = (SIZE_T)(HalpAPEntry64  - HalpAPEntry16);
    TempGdtOffset   = DataBlockOffset + FIELD_OFFSET(AP_ENTRY_DATA, TempGdt);

    ApData = (PAP_ENTRY_DATA)Add2Ptr(HalpLowStub, DataBlockOffset);

    /* The temp PML4 identity-maps the stub, so virtual = physical
     * for these addresses. */

    /* Stage-0 -> Stage-1 jump: absolute linear address of
     * HalpAPEntry32 with selector 0x20 (32-bit code, base 0). */
    ApData->Jump32To32Offset = (ULONG)(HalpLowStubPhysicalAddress.QuadPart + Entry32Offset);
    /* Jump32To32Segment is pre-set to 0x20 in the asm. */

    /* Stage-1 -> Stage-2 jump: absolute linear address of
     * HalpAPEntry64 with selector 0x10 (64-bit code, base 0). */
    ApData->Jump64Offset = (ULONG)(HalpLowStubPhysicalAddress.QuadPart + Entry64Offset);
    /* Jump64Segment is pre-set to 0x10 in the asm. */

    /* The stage-1 indirect far-jump reads its m16:32 from
     * ds:[disp32]. DS.base = 0 (sel 0x18), so disp32 must be the
     * ABSOLUTE linear address of the Jump64Offset field. The asm
     * leaves the disp32 placeholder at APE_IndirectJumpOperandSite;
     * patch it now. */
    {
        SIZE_T OperandSiteOffset =
            (SIZE_T)(APE_IndirectJumpOperandSite - HalpAPEntry16);
        ULONG *p = (ULONG *)Add2Ptr(HalpLowStub, OperandSiteOffset);
        *p = (ULONG)(HalpLowStubPhysicalAddress.QuadPart + DataBlockOffset +
                     FIELD_OFFSET(AP_ENTRY_DATA, Jump64Offset));
    }

    /* Diagnostic SelfPtr. */
    ApData->SelfPtr = (ULONG64)HalpLowStubPhysicalAddress.QuadPart +
                       (ULONG64)DataBlockOffset;

    ApData->PageTableRoot  = (ULONG64)TempPml4Pa.QuadPart;
    ApData->KxLoaderBlock  = (ULONG64)LoaderBlock;
    ApData->ProcessorState = (ULONG64)ProcessorState;

    /* GDT pointer: lgdt limit = sizeof(GDT) - 1 = 5*8 - 1 = 39. */
    ApData->Gdtr32_Limit = (USHORT)(sizeof(ApData->TempGdt) - 1);
    ApData->Gdtr32_Base  = (ULONG)(HalpLowStubPhysicalAddress.QuadPart + TempGdtOffset);

    /* No GDT entries need runtime patching -- all five descriptors
     * (including sel 0x20 with base 0) are pre-initialized in asm. */

    /* Kick the AP. INIT-SIPI is shared between amd64 and i386. */
    ApicStartApplicationProcessor(HalpStartedProcessorCount, HalpLowStubPhysicalAddress);

    HalpStartedProcessorCount++;
    return TRUE;
#endif /* _NO_AP_TRAMPOLINE_ */
}
