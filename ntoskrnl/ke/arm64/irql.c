/*
 * PROJECT:         ReactOS Kernel (ARM64)
 * PURPOSE:         Interrupt request level management for ARM64
 *
 * DESIGN:
 *
 * IRQL is the GIC CPU-interface priority mask (ICC_PMR_EL1 on GICv3, GICC_PMR
 * on GICv2), exactly as it is CR8/TPR on x64: raising IRQL to N stops the GIC
 * from signalling any interrupt whose priority maps to IRQL <= N, and HIGH_LEVEL
 * programs PMR 0x00, which no interrupt can pass. IRQL transitions never touch
 * PSTATE.I (DAIF.I) -- like RFLAGS.IF on x64 it belongs to the exception entry
 * and to explicit _disable()/_enable() regions, so KeRaiseIrql(HIGH_LEVEL)
 * leaves interrupts "enabled" from the CPU's point of view, as on Windows.
 *
 * PASSIVE/APC/DISPATCH all use PMR 0xFF; the software interrupts at those
 * levels are requested through PRCB/thread state (HalRequestSoftwareInterrupt
 * sends no SGI) and delivered by KfLowerIrql, in the manner of the x86 PIC HAL:
 * with interrupts enabled while the handler runs and the caller's interrupt
 * state restored afterwards.
 *
 * Until the executive sets KiHalInitialized (after HAL phase 1) the HAL's PMR
 * path cannot be used yet, so HIGH_LEVEL is honoured with DAIF instead (see
 * the early path in KiApplyIrqMaskForIrqlTransition).
 */

#include <ntoskrnl.h>
//#define NDEBUG  /* Temporarily disabled for timer debugging */
#include <debug.h>

/*
 * ARM64: IRQL is stored in Pcr->CurrentIrql (read via TPIDR_EL1 by KeGetCurrentIrql macro).
 * KeArm64CurrentIrql is kept only as an early-boot fallback before TPIDR_EL1 points
 * at the current PCR.
 */
extern KIRQL KeArm64CurrentIrql;

/*
 * KiHalInitialized - Flag tracking whether HAL.DLL has been initialized
 *
 * CRITICAL: This flag prevents calls to HalSetGicPriorityMask() before the HAL
 * is loaded and its import table is resolved. During early kernel boot (before
 * HalInitSystem), HAL imports are not yet available and calling them causes
 * immediate crashes (jump to NULL or unresolved address).
 *
 * Boot sequence:
 *   1. FreeLdr jumps to kernel entry point
 *   2. Kernel PE loader resolves imports from HAL.DLL
 *   3. Early kernel init (PCR setup, etc.) - KiHalInitialized = FALSE
 *   4. HalInitSystem(0) is called and returns successfully
 *   5. ExpInitializeExecutive calls ExArchPostHalInitSystemPhase0 and sets
 *      KiHalInitialized = TRUE before the generic post-HAL _enable()
 *   6. GIC priority masking becomes available
 *
 * Until KiHalInitialized is TRUE, we use DAIF masking exclusively.
 *
 * IMPORTANT: This flag is set by the kernel (ex/init.c), NOT by the HAL.
 * This avoids circular import dependencies between kernel and HAL.
 */
BOOLEAN KiHalInitialized = FALSE;

/*
 * Interrupt mask state captured when IRQL crosses up into HIGH_LEVEL on the
 * pre-HAL DAIF-only path, restored when it crosses back down. Pre-HAL boot is
 * single-processor and HIGH_LEVEL masks all interrupts, so a single slot
 * cannot be raced.
 */
static BOOLEAN KiEarlyHighLevelIrqsWereEnabled = FALSE;

#if DBG
/*
 * PMR-TRACE diagnostic: ring of the most recent IRQL transitions that
 * programmed the GIC priority mask, with a PMR read-back after each write.
 * Dumped from the interrupt dispatcher when a spinlock-already-owned state
 * is detected, to identify who left PMR unmasked at elevated IRQL.
 */
typedef struct _KI_PMR_TRACE_ENTRY
{
    PVOID Inner;     /* caller of KiApplyIrqMaskForIrqlTransition */
    PVOID Outer;     /* caller of KfRaiseIrql/KfLowerIrql, if any */
    UCHAR OldIrql;
    UCHAR NewIrql;
    UCHAR PmrAfter;  /* PMR read back right after the transition */
    UCHAR Path;      /* which branch programmed the PMR */
} KI_PMR_TRACE_ENTRY;

#define KI_PMR_TRACE_DEPTH 16
static KI_PMR_TRACE_ENTRY KiPmrTrace[KI_PMR_TRACE_DEPTH];
static volatile LONG KiPmrTraceIndex = 0;
static PVOID KiPmrTraceOuterCaller = NULL;

VOID
KiPmrTraceLog(
    _In_ PVOID Inner,
    _In_ KIRQL OldIrql,
    _In_ KIRQL NewIrql,
    _In_ UCHAR Path)
{
    ULONG Slot = (ULONG)(InterlockedIncrement(&KiPmrTraceIndex) - 1) %
                 KI_PMR_TRACE_DEPTH;
    KiPmrTrace[Slot].Inner = Inner;
    KiPmrTrace[Slot].Outer = KiPmrTraceOuterCaller;
    KiPmrTrace[Slot].OldIrql = OldIrql;
    KiPmrTrace[Slot].NewIrql = NewIrql;
    KiPmrTrace[Slot].PmrAfter = (UCHAR)HalGetGicPriorityMask();
    KiPmrTrace[Slot].Path = Path;
}

VOID
KiPmrTraceDump(VOID)
{
    LONG Total = KiPmrTraceIndex;
    LONG First = (Total > KI_PMR_TRACE_DEPTH) ? (Total - KI_PMR_TRACE_DEPTH) : 0;
    LONG i;

    DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
               "PMR-TRACE: %ld transitions total, dumping %ld..%ld\n",
               Total, First, Total - 1);
    for (i = First; i < Total; i++)
    {
        KI_PMR_TRACE_ENTRY *E = &KiPmrTrace[i % KI_PMR_TRACE_DEPTH];
        DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
                   "PMR-TRACE[%ld]: %u->%u pmr=%02x path=%u inner=%p outer=%p\n",
                   i, E->OldIrql, E->NewIrql, E->PmrAfter, E->Path,
                   E->Inner, E->Outer);
    }
}
#define KI_PMR_TRACE_SET_OUTER() (KiPmrTraceOuterCaller = _ReturnAddress())
#define KI_PMR_TRACE(Old, New, Path) \
    KiPmrTraceLog(_ReturnAddress(), (Old), (New), (Path))
#else
#define KI_PMR_TRACE_SET_OUTER()
#define KI_PMR_TRACE(Old, New, Path)
#endif

#undef KeLowerIrql
#undef KeRaiseIrql
#undef KeGetCurrentIrql

#define ARM64_MASK_ALL()   __asm__ __volatile__("msr daifset, #0xf" ::: "memory")
#define ARM64_MASK_SERROR() __asm__ __volatile__("msr daifset, #0x4" ::: "memory")
/*
 * ARM64_UNMASK_ALL_NO_SERROR: Unmask D, I, F but NOT A (SError).
 * Used at most IRQL levels where we want to keep SError masked.
 * Immediate 0xB = bits 3,1,0 (D,I,F).
 */
#define ARM64_UNMASK_ALL_NO_SERROR() __asm__ __volatile__("msr daifclr, #0xb" ::: "memory")
/*
 * The PCR IRQL is read and written with single x18-relative instructions.
 * Below DISPATCH_LEVEL the thread can be preempted between any two
 * instructions and resume on another processor (x18 then names the new
 * processor's PCR): a PCR address computed into another register beforehand
 * would read, or write, the previous processor's IRQL. The value itself is
 * the thread's own IRQL wherever it runs, so a separate read and write are
 * fine as long as each is one instruction against the current x18.
 */
FORCEINLINE
KIRQL
KiQueryCurrentIrql(VOID)
{
    if (__getReg(18) != 0)
    {
        return KiArm64ReadCurrentIrql();
    }

    /* Fallback for very early boot (before the PCR is installed in x18) */
    return KeArm64CurrentIrql;
}

VOID
KiSetCurrentIrql(
    _In_ KIRQL Irql)
{
    if (__getReg(18) != 0)
    {
        __asm__ __volatile__("strb %w0, [x18, #%c1]"
                             :
                             : "r"(Irql), "i"(FIELD_OFFSET(KIPCR, CurrentIrql))
                             : "memory");
        return;
    }

    /* Fallback for very early boot (before the PCR is installed in x18) */
    KeArm64CurrentIrql = Irql;
}

FORCEINLINE
VOID
KiTraceSerrorPolicy(
    _In_z_ PCSTR Site,
    _In_ KIRQL OldIrql,
    _In_ KIRQL NewIrql)
{
    UNREFERENCED_PARAMETER(Site);
    UNREFERENCED_PARAMETER(OldIrql);
    UNREFERENCED_PARAMETER(NewIrql);
}

/*
 * KiHalInitialized is set by the kernel's ARM64 post-HAL phase-0 hook right
 * after HalInitSystem(0) returns successfully. This ensures HAL exports are
 * available before we start using GIC priority masking.
 *
 * Prior design (REMOVED): HAL called KeArmInitializeGicSupport() to flip this flag.
 * This created a circular import dependency (HAL imports kernel, kernel imports HAL).
 *
 * New design: Kernel owns the flag and sets it after HalInitSystem(0) completes.
 * This eliminates the circular dependency and gives the kernel explicit control
 * over when GIC priority masking becomes active.
 */

FORCEINLINE
BOOLEAN
KiIrqlTransitionNeedsGicPmrUpdate(
    _In_ KIRQL OldIrql,
    _In_ KIRQL NewIrql)
{
    /*
     * PASSIVE/APC/DISPATCH all use the unmasked GIC PMR value. Avoid the
     * system-register/MMIO PMR write on normal thread and spinlock transitions.
     */
    return !((OldIrql <= DISPATCH_LEVEL) && (NewIrql <= DISPATCH_LEVEL));
}

FORCEINLINE
VOID
KiUpdateSerrorMaskOnlyForIrql(
    _In_ KIRQL NewIrql)
{
    UNREFERENCED_PARAMETER(NewIrql);

    ARM64_MASK_SERROR();

    __asm__ __volatile__("isb" ::: "memory");
}

ULONG
NTAPI
KeGetCurrentProcessorNumber(VOID)
{
    PKPRCB Prcb = KeGetCurrentPrcb();
    return Prcb ? Prcb->Number : 0;
}

ULONG
NTAPI
KeGetCurrentProcessorNumberEx(
    _Out_opt_ PPROCESSOR_NUMBER ProcNumber)
{
    PKPRCB Prcb = KeGetCurrentPrcb();
    ULONG Processor = Prcb ? Prcb->Number : 0;

    if (ProcNumber)
    {
        ProcNumber->Group = 0;
        ProcNumber->Number = (UCHAR)Processor;
        ProcNumber->Reserved = 0;
    }

    return Processor;
}

/*
 * KiApplyIrqMaskForIrqlTransition - make the interrupt mask match a new IRQL
 *
 * IRQL maps to the GIC priority mask (HalpArm64IrqlToPmr / HalpIrqlToGicPriority
 * in the HAL; an interrupt is signalled only if its priority value is below
 * PMR):
 *
 *   IRQL 0-2   PMR 0xFF   every hardware interrupt passes
 *   IRQL 3-12  PMR 0xC0-0x30, devices at or below the IRQL blocked
 *   IRQL 13    PMR 0x20   clock blocked, IPI passes
 *   IRQL 14    PMR 0x10   IPI blocked
 *   IRQL 15    PMR 0x00   everything blocked
 *
 * Callers order the PCR IRQL store against this call (raise: mask first, then
 * IRQL; lower: IRQL first, then mask), so an interrupt is never taken while
 * the PCR claims a level that should have blocked it, and an interrupt taken
 * just after the mask opens finds the lowered IRQL already in place. An
 * interrupt the CPU had already latched when PMR was raised acknowledges as
 * spurious (1023), since the CPU interface re-checks PMR on acknowledge.
 */
VOID
KiApplyIrqMaskForIrqlTransition(
    _In_ KIRQL OldIrql,
    _In_ KIRQL NewIrql)
{
    /*
     * EARLY BOOT GUARD: Before HAL is initialized, use DAIF masking exclusively.
     *
     * During early kernel boot (before HalInitSystem), HAL.DLL imports are not
     * yet resolved. Calling HalSetGicPriorityMask() at this stage would jump to
     * an unresolved address (0 or garbage), causing an immediate crash.
     *
     * Until KiHalInitialized is TRUE, we use the legacy binary DAIF masking
     * (all IRQs on/off). Early boot may lower the logical IRQL before HAL
     * phase 0, but IRQ/FIQ delivery must stay masked until the GIC is
     * configured and the executive explicitly enables interrupts after HAL.
     */
    if (!KiHalInitialized)
    {
        /* Fallback to binary DAIF masking before HAL is initialized */
        if (NewIrql >= HIGH_LEVEL)
        {
            /*
             * Capture the interrupt mask state at the raise so the matching
             * drop below HIGH_LEVEL can restore it. Before the executive's
             * post-HAL-phase-0 _enable() this records "masked" and lowering
             * keeps interrupts off; after it, a HIGH_LEVEL round trip (e.g.
             * the KD port lock around every debug print) must hand back the
             * enabled state it found, or the CPU stays masked forever.
             */
            if (OldIrql < HIGH_LEVEL)
            {
                ULONG64 Daif;
                __asm__ __volatile__("mrs %0, daif" : "=r"(Daif));
                KiEarlyHighLevelIrqsWereEnabled = !(Daif & (1ULL << 7));
            }
            ARM64_MASK_ALL();
            ARM64_SYNC_BARRIER();
            KiTraceSerrorPolicy("early-high", OldIrql, NewIrql);
        }
        else if (OldIrql >= HIGH_LEVEL && NewIrql < HIGH_LEVEL)
        {
            /* Restore the interrupt mask state captured at the raise */
            if (KiEarlyHighLevelIrqsWereEnabled)
            {
                ARM64_UNMASK_ALL_NO_SERROR();
                ARM64_SYNC_BARRIER();
                KiTraceSerrorPolicy("early-drop-unmask", OldIrql, NewIrql);
            }
            else
            {
                /*
                 * Interrupts were masked when HIGH_LEVEL was entered. Keep
                 * IRQ/FIQ masked; the executive enables them explicitly
                 * after HAL phase 0 has configured the GIC.
                 */
                KiUpdateSerrorMaskOnlyForIrql(NewIrql);
                KiTraceSerrorPolicy("early-drop-masked", OldIrql, NewIrql);
            }
        }
        else if (NewIrql != OldIrql)
        {
            /*
             * Keep DAIF.I/F state unchanged in early boot, but still enforce
             * the SError mask policy by IRQL.
             */
            KiUpdateSerrorMaskOnlyForIrql(NewIrql);
            KiTraceSerrorPolicy("early-mask", OldIrql, NewIrql);
        }
        return;
    }

    /*
     * The GIC priority mask is the whole of IRQL, as CR8 is on x64. PSTATE.I
     * is not IRQL state and is left exactly as the caller has it: raising to
     * HIGH_LEVEL with interrupts enabled keeps them enabled (PMR 0x00 already
     * blocks every interrupt), and lowering never enables interrupts a
     * _disable() or an exception entry turned off.
     */
    if (KiIrqlTransitionNeedsGicPmrUpdate(OldIrql, NewIrql))
    {
        HalSetGicPriorityMask(NewIrql);
        KI_PMR_TRACE(OldIrql, NewIrql, 1);
        ARM64_SYNC_BARRIER();
    }
}

/*
 * Exported KeGetCurrentIrql function for drivers and other modules.
 * The kernel itself uses the macro in ketypes.h which reads from per-CPU PCR.
 * This exported version also reads from PCR via KiQueryCurrentIrql.
 */
#undef KeGetCurrentIrql
KIRQL
NTAPI
KeGetCurrentIrql(VOID)
{
    return KiQueryCurrentIrql();
}

KIRQL
FASTCALL
KfRaiseIrql(
    _In_ KIRQL NewIrql)
{
    KIRQL OldIrql = KiQueryCurrentIrql();

    if (NewIrql > HIGH_LEVEL)
    {
        DPRINT1("KfRaiseIrql: invalid IRQL %u\n", NewIrql);
        KeBugCheckEx(IRQL_NOT_GREATER_OR_EQUAL, NewIrql, OldIrql, 0, 0);
    }

    if (NewIrql <= OldIrql)
    {
        return OldIrql;
    }

    /*
     * PASSIVE/APC/DISPATCH share the same unmasked GIC PMR after HAL init.
     * Raising inside that band is a logical PCR update only, so avoid the
     * DMB/PMR/ISB path that is needed only when CPU interrupt masking changes.
     */
    if (KiHalInitialized &&
        (OldIrql <= DISPATCH_LEVEL) &&
        (NewIrql <= DISPATCH_LEVEL))
    {
        KiSetCurrentIrql(NewIrql);
        return OldIrql;
    }

    /*
     * ARM64 Memory Barrier: Ensure all pending loads/stores complete before
     * IRQL raise. This prevents critical section violations where stores to
     * shared data might reorder past the IRQL raise on ARM64's relaxed memory model.
     */
    __asm__ __volatile__("dmb ish" ::: "memory");

    KI_PMR_TRACE_SET_OUTER();
    KiApplyIrqMaskForIrqlTransition(OldIrql, NewIrql);
    KiSetCurrentIrql(NewIrql);

    /*
     * ARM64 Instruction Barrier: Ensure IRQL change and GIC PMR update take
     * effect before continuing. Prevents speculative execution from reading
     * stale IRQL values or executing code that should be protected by the new IRQL.
     */
    __asm__ __volatile__("isb" ::: "memory");

    return OldIrql;
}

/*
 * KiDeliverSoftwareInterrupts - run the software interrupts a lowering unmasks
 *
 * ARM64 has no self-interrupt that the GIC priority mask could hold pending
 * until IRQL drops (PASSIVE..DISPATCH share PMR 0xFF), so DISPATCH and APC
 * requests live in PRCB/thread state and are delivered here. As in the x86 PIC
 * HAL (KfLowerIrql / _HalpDispatchInterruptHandler / HalpApcInterruptHandler),
 * a handler always runs with interrupts enabled -- the DPC and APC routines
 * may wait on interrupt-driven progress -- and the caller's PSTATE.I is put
 * back afterwards, so a KeLowerIrql issued with interrupts disabled (an
 * interrupt exit, KeThawExecution) neither loses the request nor returns with
 * interrupts enabled.
 */
static
VOID
KiDeliverSoftwareInterrupts(
    _In_ KIRQL OldIrql,
    _In_ KIRQL NewIrql)
{
    PKPRCB Prcb;
    PKTHREAD Thread;
    BOOLEAN InterruptsWereEnabled;

    /* Nothing can be pending before the PCR is installed */
    if (__getReg(18) == 0)
        return;

    /* Pin the thread to this processor before looking at its PRCB */
    InterruptsWereEnabled = KeDisableInterrupts();
    Prcb = KeGetCurrentPrcb();

    if ((NewIrql < DISPATCH_LEVEL) &&
        (Prcb->DpcInterruptRequested ||
         Prcb->TimerRequest ||
         (Prcb->DpcData[0].DpcQueueDepth > 0) ||
         Prcb->QuantumEnd ||
         Prcb->NextThread))
    {
        /*
         * Consume the request before running it; a DPC queued while the
         * dispatcher runs sets it again and the dispatcher's own lowering
         * delivers it. KiDispatchInterrupt raises to DISPATCH_LEVEL, enables
         * interrupts around its work and restores NewIrql when done. It may
         * switch threads, so nothing PRCB-derived is reused after it.
         */
        Prcb->DpcInterruptRequested = FALSE;
        KiDispatchInterrupt();
        _disable();
    }

    if ((NewIrql < APC_LEVEL) && (OldIrql >= APC_LEVEL))
    {
        Thread = KeGetCurrentThread();
        if ((Thread != NULL) &&
            Thread->ApcState.KernelApcPending &&
            !Thread->SpecialApcDisable)
        {
            KiSetCurrentIrql(APC_LEVEL);
            _enable();
            KiDeliverApc(KernelMode, NULL, NULL);
            _disable();
            KiSetCurrentIrql(NewIrql);
        }
    }

    KeRestoreInterrupts(InterruptsWereEnabled);
}

VOID
FASTCALL
KfLowerIrql(
    _In_ KIRQL NewIrql)
{
    KIRQL OldIrql = KiQueryCurrentIrql();

    if (NewIrql > OldIrql)
    {
        KeBugCheckEx(IRQL_NOT_GREATER_OR_EQUAL, NewIrql, OldIrql,
                     (ULONG_PTR)_ReturnAddress(), 0);
    }

    /*
     * Publish the new IRQL before opening the mask: an interrupt the lowered
     * PMR lets through must find the PCR at the level it interrupted, or
     * HalBeginSystemInterrupt's raise would see the old, higher IRQL and not
     * raise at all.
     */
    KiSetCurrentIrql(NewIrql);
    __asm__ __volatile__("dmb ish" ::: "memory");

    KI_PMR_TRACE_SET_OUTER();
    KiApplyIrqMaskForIrqlTransition(OldIrql, NewIrql);

    if (NewIrql < DISPATCH_LEVEL)
    {
        KiDeliverSoftwareInterrupts(OldIrql, NewIrql);
    }
}

NTKERNELAPI
KIRQL
KxGetCurrentIrql(VOID)
{
    return KeGetCurrentIrql();
}

NTKERNELAPI
VOID
KxLowerIrql(
    _In_ KIRQL NewIrql)
{
    KfLowerIrql(NewIrql);
}

NTKERNELAPI
KIRQL
KxRaiseIrql(
    _In_ KIRQL NewIrql)
{
    return KfRaiseIrql(NewIrql);
}

NTKERNELAPI
KIRQL
KxRaiseIrqlToDpcLevel(VOID)
{
    return KeRaiseIrqlToDpcLevel();
}

KIRQL
NTAPI
KeRaiseIrqlToDpcLevel(VOID)
{
    return KfRaiseIrql(DISPATCH_LEVEL);
}

NTKERNELAPI
KIRQL
KxRaiseIrqlToSynchLevel(VOID)
{
    return KeRaiseIrqlToSynchLevel();
}

KIRQL
NTAPI
KeRaiseIrqlToSynchLevel(VOID)
{
    return KfRaiseIrql(SYNCH_LEVEL);
}

VOID
NTAPI
KeLowerIrql(
    _In_ KIRQL NewIrql)
{
    KfLowerIrql(NewIrql);
}

VOID
NTAPI
KeRaiseIrql(
    _In_ KIRQL NewIrql,
    _Out_ PKIRQL OldIrql)
{
    *OldIrql = KfRaiseIrql(NewIrql);
}
