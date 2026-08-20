/*
 * PROJECT:         ReactOS HAL
 * LICENSE:         GPL - See COPYING in the top level directory
 * FILE:            hal/halx86/generic/halinit.c
 * PURPOSE:         HAL Entrypoint and Initialization
 * PROGRAMMERS:     Alex Ionescu (alex.ionescu@reactos.org)
 */

/* INCLUDES ******************************************************************/

#include <hal.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS *******************************************************************/

//#ifdef CONFIG_SMP // FIXME: Reenable conditional once HAL is consistently compiled for SMP mode
BOOLEAN HalpOnlyBootProcessor;
//#endif
BOOLEAN HalpPciLockSettings;
BOOLEAN HalBootViaEfi;

/* PRIVATE FUNCTIONS *********************************************************/

static
CODE_SEG("INIT")
VOID
HalpGetParameters(
    _In_ PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    /* Make sure we have a loader block and command line */
    if (LoaderBlock && LoaderBlock->LoadOptions)
    {
        /* Read the command line */
        PCSTR CommandLine = LoaderBlock->LoadOptions;

//#ifdef CONFIG_SMP // FIXME: Reenable conditional once HAL is consistently compiled for SMP mode
        /* Check whether we should only start one CPU */
        if (strstr(CommandLine, "ONECPU"))
            HalpOnlyBootProcessor = TRUE;
//#endif

        /* Check if PCI is locked */
        if (strstr(CommandLine, "PCILOCK"))
            HalpPciLockSettings = TRUE;

        /* Check for initial breakpoint */
        if (strstr(CommandLine, "BREAK"))
            DbgBreakPoint();
    }
}

/*
 * There is no processor power management anywhere in the system - processr.sys
 * executes no _PSS/_PCT and nothing ever leaves the P-state the firmware
 * booted in, which on mobile parts is the fastest one with all cores lit.
 * Under sustained load (chkdsk, builds) that thermally shuts laptops down.
 * Until real P-state support exists, pin AMD family 15h cores to their
 * slowest software P-state. NOTHROTTLE on the command line skips this.
 */
static
VOID
HalpSetLowestAmdPstate(VOID)
{
    INT CpuInfo[4];
    ULONG Family;
    ULONG i;
    ULONG Lowest = 0;

    /* AuthenticAMD only; the MSRs below fault on anything else */
    __cpuid(CpuInfo, 0);
    if ((CpuInfo[1] != 0x68747541) ||
        (CpuInfo[3] != 0x69746E65) ||
        (CpuInfo[2] != 0x444D4163))
    {
        return;
    }

    __cpuid(CpuInfo, 1);
    Family = (CpuInfo[0] >> 8) & 0xF;
    if (Family == 0xF)
        Family += (CpuInfo[0] >> 20) & 0xFF;
    if (Family != 0x15)
        return;

    /* MSRC001_0064..006B hold the software P-state definitions; bit 63 marks
     * a valid entry and higher indices are slower */
    for (i = 0; i < 8; i++)
    {
        if (__readmsr(0xC0010064 + i) & (1ULL << 63))
            Lowest = i;
    }

    /* Disable Core Performance Boost so nothing overrides the pin:
     * MSRC001_0015 [HWCR] bit 25 = CpbDis */
    __writemsr(0xC0010015, __readmsr(0xC0010015) | (1ULL << 25));

    /* MSRC001_0062 [P-state Control]: target P-state index, per core */
    __writemsr(0xC0010062, Lowest);

    /* Prove the transition: MSRC001_0063 [P-state Status] holds the CURRENT
     * P-state; poll briefly, then report the actual core frequency from the
     * definition MSR (fam15h: CoreCOF = 100MHz * (CpuFid + 0x10) >> CpuDid) */
    {
        ULONG64 Status;
        ULONG64 Def;
        ULONG Fid, Did, Spin;

        for (Spin = 0; Spin < 1000000; Spin++)
        {
            Status = __readmsr(0xC0010063) & 7;
            if (Status == Lowest) break;
        }
        Def = __readmsr(0xC0010064 + (ULONG)Status);
        Fid = (ULONG)(Def & 0x3F);
        Did = (ULONG)((Def >> 6) & 7);
        DPRINT1("HAL: pinned CPU to AMD P-state %lu, status now %lu = %lu MHz, boost off (thermal guard, NOTHROTTLE to disable)\n",
                Lowest, (ULONG)Status, (100UL * (Fid + 0x10)) >> Did);
    }
}

/*
 * Enable AMD family 15h C1E (Enhanced Halt / "message triggered C1E") so the
 * idle HLT in HalProcessorIdle drops core voltage/frequency at idle, cutting
 * idle heat/fan on mobile parts (ASUS X550DP, A-series Trinity/Richland).
 *
 * OPT-IN ONLY (the "C1E" boot flag) and default OFF, because C1E gates the
 * per-core LAPIC timer (and on some parts the TSC) while idle.
 *
 * CLOCK-TICK RISK ASSESSMENT (verified against this HAL's sources):
 * In the APIC HAL (halmp/halmacpi/halaacpi) the OS clock tick is driven by the
 * CMOS RTC periodic interrupt routed through the IOAPIC to APIC_CLOCK_VECTOR
 * (apic/rtctimer.c: HalpInitializeClock arms RTC_REG_B_PI, HalpClockInterrupt-
 * Handler -> KeUpdateSystemTime).  It is NOT driven by the per-core LAPIC timer
 * (apic/apictimer.c), which this HAL uses only for profiling and leaves MASKED
 * unless profiling is started.  Therefore C1E stopping the LAPIC timer does NOT
 * stop the OS tick: the RTC interrupt arrives via the IOAPIC and still wakes the
 * core out of C1E.  This reasoning is NOT hardware-validated - hence opt-in.
 *
 * Mechanism (AMD BKDG fam15h Models 00h-0Fh, MSRC001_0055 Interrupt Pending
 * Message Register; cross-checked vs Linux/FreeBSD/coreboot):
 *   bit 28  C1eOnCmpHalt - set: enter C1E once all cores have executed HLT
 *   bit 27  SmiOnCmpHalt - the spec requires this be 0 when C1eOnCmpHalt is set
 *   bit 25  IntrPndMsg   - the spec requires this be 0 when C1eOnCmpHalt is set
 * Read-modify-write so every other/reserved bit is preserved.
 */
static
VOID
HalpEnableAmdC1E(VOID)
{
    INT CpuInfo[4];
    ULONG Family;
    ULONG64 IntPendMsg;

    /* AuthenticAMD only; the MSR below faults on anything else */
    __cpuid(CpuInfo, 0);
    if ((CpuInfo[1] != 0x68747541) ||
        (CpuInfo[3] != 0x69746E65) ||
        (CpuInfo[2] != 0x444D4163))
    {
        return;
    }

    __cpuid(CpuInfo, 1);
    Family = (CpuInfo[0] >> 8) & 0xF;
    if (Family == 0xF)
        Family += (CpuInfo[0] >> 20) & 0xFF;
    if (Family != 0x15)
        return;

    /* MSRC001_0055 [Interrupt Pending Message]: set C1eOnCmpHalt (bit 28) and
     * clear SmiOnCmpHalt (bit 27) + IntrPndMsg (bit 25) as the spec mandates,
     * leaving all other bits as read (never a blind write) */
    IntPendMsg = __readmsr(0xC0010055);
    IntPendMsg &= ~((1ULL << 25) | (1ULL << 27));
    IntPendMsg |= (1ULL << 28);
    __writemsr(0xC0010055, IntPendMsg);

    DPRINT1("HAL: AMD C1E enabled (MSR C001_0055 = 0x%I64x), idle now enters C1E (X550 idle-power test, C1E flag)\n",
            __readmsr(0xC0010055));
}

/* FUNCTIONS *****************************************************************/

VOID
NTAPI
HalInitializeProcessor(
    IN ULONG ProcessorNumber,
    IN PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    /* Hal specific initialization for this cpu */
    HalpInitProcessor(ProcessorNumber, LoaderBlock);

    /* Thermal guard: cap the core's P-state (see HalpSetLowestAmdPstate).
     * NOTHROTTLE means "no throttle behavior at all" and skips both the P-state
     * pin and the (opt-in) C1E enable below. */
    if (!(LoaderBlock && LoaderBlock->LoadOptions &&
          strstr(LoaderBlock->LoadOptions, "NOTHROTTLE")))
    {
        HalpSetLowestAmdPstate();

        /* Deeper idle: only when "C1E" is explicitly on the command line.
         * Default (flag absent) leaves idle as plain C1 (sti;hlt) unchanged.
         * See HalpEnableAmdC1E for the LAPIC-timer/tick hang-risk analysis. */
        if (LoaderBlock && LoaderBlock->LoadOptions &&
            strstr(LoaderBlock->LoadOptions, "C1E"))
        {
            HalpEnableAmdC1E();
        }
    }

    /* Set default stall count */
    KeGetPcr()->StallScaleFactor = INITIAL_STALL_COUNT;

    /* Update the interrupt affinity and processor mask */
    InterlockedBitTestAndSetAffinity(&HalpActiveProcessors, ProcessorNumber);
    InterlockedBitTestAndSetAffinity(&HalpDefaultInterruptAffinity, ProcessorNumber);

    if (ProcessorNumber == 0)
    {
        /* Register routines for KDCOM */
        HalpRegisterKdSupportFunctions();
    }
}

/*
 * @implemented
 */
CODE_SEG("INIT")
BOOLEAN
NTAPI
HalInitSystem(
    _In_ ULONG BootPhase,
    _In_ PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    PKPRCB Prcb = KeGetCurrentPrcb();
    NTSTATUS Status;

    /* Check the boot phase */
    if (BootPhase == 0)
    {
        /* Save bus type */
        HalpBusType = LoaderBlock->u.I386.MachineType & 0xFF;

        /* Get command-line parameters */
        HalpGetParameters(LoaderBlock);

#if (NTDDI_VERSION >= NTDDI_LONGHORN)
        HalBootViaEfi = LoaderBlock->FirmwareInformation.FirmwareTypeEfi;
#else
        HalBootViaEfi = FALSE;
#ifdef __REACTOS__
        ASSERT(LoaderBlock->Extension != NULL);
        if (LoaderBlock->Extension->Size >= FIELD_OFFSET(LOADER_PARAMETER_EXTENSION, LoaderPerformanceData))
            HalBootViaEfi = LoaderBlock->Extension->BootViaEFI;
#endif
#endif

        /* Check for PRCB version mismatch */
        if (Prcb->MajorVersion != PRCB_MAJOR_VERSION)
        {
            /* No match, bugcheck */
            KeBugCheckEx(MISMATCHED_HAL, 1, Prcb->MajorVersion, PRCB_MAJOR_VERSION, 0);
        }

        /* Checked/free HAL requires checked/free kernel */
        if (Prcb->BuildType != HalpBuildType)
        {
            /* No match, bugcheck */
            KeBugCheckEx(MISMATCHED_HAL, 2, Prcb->BuildType, HalpBuildType, 0);
        }

        /* Initialize ACPI */
        Status = HalpSetupAcpiPhase0(LoaderBlock);
        if (!NT_SUCCESS(Status))
        {
            KeBugCheckEx(ACPI_BIOS_ERROR, Status, 0, 0, 0);
        }

        /* Initialize the PICs */
        HalpInitializePICs(TRUE);

        /* Initialize CMOS lock */
        KeInitializeSpinLock(&HalpSystemHardwareLock);

        /* Initialize CMOS */
        HalpInitializeCmos();

        /* Fill out the dispatch tables */
        HalQuerySystemInformation = HaliQuerySystemInformation;
        HalSetSystemInformation = HaliSetSystemInformation;
        HalInitPnpDriver = HaliInitPnpDriver;
        HalGetDmaAdapter = HalpGetDmaAdapter;

        HalGetInterruptTranslator = NULL;  // FIXME: TODO
        HalResetDisplay = HalpBiosDisplayReset;
        HalHaltSystem = HaliHaltSystem;

        /* Setup I/O space */
        HalpDefaultIoSpace.Next = HalpAddressUsageList;
        HalpAddressUsageList = &HalpDefaultIoSpace;

        /* Setup busy waiting */
        HalpCalibrateStallExecution();

        /* Initialize the clock */
        HalpInitializeClock();

        /*
         * We could be rebooting with a pending profile interrupt,
         * so clear it here before interrupts are enabled
         */
        HalStopProfileInterrupt(ProfileTime);

        /* Do some HAL-specific initialization */
        HalpInitPhase0(LoaderBlock);

        /* Initialize Phase 0 of the x86 emulator */
        HalInitializeBios(0, LoaderBlock);
    }
    else if (BootPhase == 1)
    {
        /* Initialize bus handlers */
        HalpInitBusHandlers();

        /* Do some HAL-specific initialization */
        HalpInitPhase1();

        /* Initialize Phase 1 of the x86 emulator */
        HalInitializeBios(1, LoaderBlock);
    }

    /* All done, return */
    return TRUE;
}
