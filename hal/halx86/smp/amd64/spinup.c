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

/* The trampoline image; defined in hal/halx86/smp/amd64/apentry.S. */
extern UCHAR HalpAPEntry16[];
extern UCHAR HalpAPEntryData[];
extern UCHAR HalpAPEntry64[];
extern UCHAR HalpAPEntry16End[];

/* Boot CPU is the first started processor (counted as 1). */
ULONG HalpStartedProcessorCount = 1;

#ifndef Add2Ptr
#define Add2Ptr(P,I) ((PVOID)((PUCHAR)(P) + (I)))
#endif

/*
 * Layout of the in-page data block embedded in the trampoline at
 * HalpAPEntryData. The asm side accesses these fields by their fixed
 * offsets (CS-relative in 16-bit, RIP-relative in 64-bit) so the struct
 * here must stay in lockstep with apentry.S. Natural amd64 alignment
 * already matches the asm layout -- no packing pragma needed.
 */
typedef struct _AP_ENTRY_DATA
{
    /* +0  Absolute (physical = virtual, since the trampoline page is
     *     identity-mapped in the temp PML4) address of HalpAPEntry64
     *     inside the stub copy. Used as the 32-bit operand of the
     *     16-bit-encoded far-jump that switches CPU into long mode. */
    ULONG Jump32Offset;

    /* +4  Far-jump target selector. Always KGDT64_R0_CODE -- the temp
     *     GDT slot at the same selector value. The asm reserves a full
     *     dword here for layout; only the low 16 bits are consumed by
     *     the far-jump encoding. */
    ULONG Jump32Segment;

    /* +8  Physical address of this data block. Diagnostic only. */
    ULONG64 SelfPtr;

    /* +16 Physical address of the temp PML4 the AP loads into CR3 in
     *     stage 1. Built by HalpSetupTemporaryMappings. */
    ULONG64 PageTableRoot;

    /* +24 Virtual address of KeLoaderBlock. The 64-bit handoff stuffs
     *     this into RCX (Win64 ABI arg0) before lret'ing into
     *     KiSystemStartup(LoaderBlock). */
    ULONG64 KxLoaderBlock;

    /* +32 Virtual address of the AP's KPROCESSOR_STATE. Stage 2 reads
     *     CR0/CR4/CR8/DR0..7, Gdtr, Idtr, Tr, MsrGsBase, MsrGsSwap,
     *     Rsp, Rip and SegCs from it. */
    ULONG64 ProcessorState;

    /* +40 Two-byte pad so the Gdtr32 base field below sits at a
     *     4-byte-aligned offset, matching the asm. */
    USHORT Gdtr32_Pad;

    /* +42 Limit of the 4-entry temp GDT (= sizeof(TempGdt) - 1 = 31).
     *     The 16-bit "data32 lgdt cs:[Gdtr32]" reads this and the base
     *     below as a 6-byte fword. */
    USHORT Gdtr32_Limit;

    /* +44 Physical address of the temp GDT inside the stub copy. */
    ULONG Gdtr32_Base;

    /* +48 Temp GDT itself: null, unused, KGDT64_R0_CODE (L=1),
     *     KGDT64_R0_DATA (4 GB flat). Pre-initialized in apentry.S; the
     *     copy into HalpLowStub carries the right bytes, so we don't
     *     touch this field at runtime. Reserved here so the struct
     *     size matches the asm-side data block. */
    ULONG64 TempGdt[4];
} AP_ENTRY_DATA, *PAP_ENTRY_DATA;

C_ASSERT(FIELD_OFFSET(AP_ENTRY_DATA, Jump32Offset)   == 0);
C_ASSERT(FIELD_OFFSET(AP_ENTRY_DATA, Jump32Segment)  == 4);
C_ASSERT(FIELD_OFFSET(AP_ENTRY_DATA, SelfPtr)        == 8);
C_ASSERT(FIELD_OFFSET(AP_ENTRY_DATA, PageTableRoot)  == 16);
C_ASSERT(FIELD_OFFSET(AP_ENTRY_DATA, KxLoaderBlock)  == 24);
C_ASSERT(FIELD_OFFSET(AP_ENTRY_DATA, ProcessorState) == 32);
C_ASSERT(FIELD_OFFSET(AP_ENTRY_DATA, Gdtr32_Limit)   == 42);
C_ASSERT(FIELD_OFFSET(AP_ENTRY_DATA, Gdtr32_Base)    == 44);
C_ASSERT(FIELD_OFFSET(AP_ENTRY_DATA, TempGdt)        == 48);

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

BOOLEAN
NTAPI
HalStartNextProcessor(
    _In_ PLOADER_PARAMETER_BLOCK LoaderBlock,
    _In_ PKPROCESSOR_STATE ProcessorState)
{
    PHYSICAL_ADDRESS TempPml4Pa;
    SIZE_T TrampolineLength;
    SIZE_T DataBlockOffset;
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

    /* Copy the trampoline (code + embedded data block) into the
     * low-memory stub page. The SIPI vector tells the AP to start at
     * HalpLowStubPhysicalAddress, which is identity-mapped in the
     * temp PML4 we just built. */
    TrampolineLength = (SIZE_T)(HalpAPEntry16End - HalpAPEntry16);
    ASSERT(TrampolineLength <= PAGE_SIZE);
    RtlCopyMemory(HalpLowStub, HalpAPEntry16, TrampolineLength);

    /* Compute link-time offsets of the labels inside the trampoline
     * image so we can patch the data block in the *copy* (HalpLowStub),
     * not the read-only original. */
    DataBlockOffset = (SIZE_T)(HalpAPEntryData - HalpAPEntry16);
    Entry64Offset   = (SIZE_T)(HalpAPEntry64  - HalpAPEntry16);
    TempGdtOffset   = DataBlockOffset + FIELD_OFFSET(AP_ENTRY_DATA, TempGdt);

    ApData = (PAP_ENTRY_DATA)Add2Ptr(HalpLowStub, DataBlockOffset);

    /* Absolute virtual/physical address of HalpAPEntry64 in the stub
     * copy. The temp PML4 identity-maps the stub, so VA == PA here. */
    ApData->Jump32Offset = (ULONG)(HalpLowStubPhysicalAddress.QuadPart + Entry64Offset);
    ApData->Jump32Segment = KGDT64_R0_CODE;

    /* Diagnostic only -- gives the AP a way to find its own data block
     * by reading SelfPtr. Spinup doesn't need it but the field exists
     * in the asm layout. */
    ApData->SelfPtr = (ULONG64)HalpLowStubPhysicalAddress.QuadPart +
                       (ULONG64)DataBlockOffset;

    ApData->PageTableRoot  = (ULONG64)TempPml4Pa.QuadPart;
    ApData->KxLoaderBlock  = (ULONG64)LoaderBlock;
    ApData->ProcessorState = (ULONG64)ProcessorState;

    /* Temp GDT: 4 descriptors of 8 bytes each = 32 bytes; lgdt wants
     * limit = byte-count - 1. The descriptor bytes themselves come
     * along with the RtlCopyMemory above. */
    ApData->Gdtr32_Limit = (USHORT)(sizeof(ApData->TempGdt) - 1);
    ApData->Gdtr32_Base  = (ULONG)(HalpLowStubPhysicalAddress.QuadPart + TempGdtOffset);

    /* Kick the AP. INIT-SIPI is shared between amd64 and i386. */
    ApicStartApplicationProcessor(HalpStartedProcessorCount, HalpLowStubPhysicalAddress);

    HalpStartedProcessorCount++;
    return TRUE;
}
