/*
 * PROJECT:         ReactOS HAL
 * LICENSE:         GNU GPL - See COPYING in the top level directory
 * FILE:            hal/halx86/apic/apic.c
 * PURPOSE:         HAL APIC Management and Control Code
 * PROGRAMMERS:     Timo Kreuzer (timo.kreuzer@reactos.org)
 * REFERENCES:      https://web.archive.org/web/20190407074221/http://www.joseflores.com/docs/ExploringIrql.html
 *                  https://www.codeproject.com/KB/system/soviet_kernel_hack.aspx
 *                  http://bbs.unixmap.net/thread-2022-1-1.html (DEAD_LINK)
 *                  https://codemachine.com/articles/interrupt_dispatching.html
 *                  https://www.osronline.com/article.cfm%5Earticle=211.htm
 */

/* INCLUDES *******************************************************************/

#include <hal.h>
#include "apicp.h"
#define NDEBUG
#include <debug.h>

#ifndef _M_AMD64
#define APIC_LAZY_IRQL
#endif

/* GLOBALS ********************************************************************/

ULONG ApicVersion;
UCHAR HalpVectorToIndex[256];
volatile LONG HalpIoApicLock;

#define HALP_IOAPIC_POLICY_POLARITY_VALID  0x01
#define HALP_IOAPIC_POLICY_ACTIVE_LOW      0x02
#define HALP_IOAPIC_POLICY_TRIGGER_VALID   0x04
#define HALP_IOAPIC_POLICY_LEVEL_TRIGGERED 0x08
#define HALP_IOAPIC_POLICY_LOGGED          0x10

static volatile LONG
HalpIoApicInputPolicy[APIC_ABSOLUTE_MAX_IRQ];
static volatile LONG HalpIoApicPolicyLock;
static BOOLEAN HalpIoApicInitialized;

#ifndef _M_AMD64
const UCHAR
HalpIRQLtoTPR[32] =
{
    0x00, /*  0 PASSIVE_LEVEL */
    0x3d, /*  1 APC_LEVEL */
    0x41, /*  2 DISPATCH_LEVEL */
    0x41, /*  3 \  */
    0x51, /*  4  \ */
    0x61, /*  5  | */
    0x71, /*  6  | */
    0x81, /*  7  | */
    0x91, /*  8  | */
    0xa1, /*  9  | */
    0xb1, /* 10  | */
    0xb1, /* 11  | */
    0xb1, /* 12  | */
    0xb1, /* 13  | */
    0xb1, /* 14  | */
    0xb1, /* 15 DEVICE IRQL */
    0xb1, /* 16  | */
    0xb1, /* 17  | */
    0xb1, /* 18  | */
    0xb1, /* 19  | */
    0xb1, /* 20  | */
    0xb1, /* 21  | */
    0xb1, /* 22  | */
    0xb1, /* 23  | */
    0xb1, /* 24  | */
    0xb1, /* 25  / */
    0xb1, /* 26 /  */
    0xc1, /* 27 PROFILE_LEVEL */
    0xd1, /* 28 CLOCK2_LEVEL */
    0xe1, /* 29 IPI_LEVEL */
    0xef, /* 30 POWER_LEVEL */
    0xff, /* 31 HIGH_LEVEL */
};

const KIRQL
HalVectorToIRQL[16] =
{
       0, /* 00 PASSIVE_LEVEL */
    0xff, /* 10 */
    0xff, /* 20 */
       1, /* 3D APC_LEVEL */
       2, /* 41 DISPATCH_LEVEL */
       4, /* 50 \ */
       5, /* 60  \ */
       6, /* 70  | */
       7, /* 80 DEVICE IRQL */
       8, /* 90  | */
       9, /* A0  / */
      10, /* B0 /  */
      27, /* C1 PROFILE_LEVEL */
      28, /* D1 CLOCK2_LEVEL */
      29, /* E1 IPI_LEVEL / EF POWER_LEVEL */
      31, /* FF HIGH_LEVEL */
};
#endif

/* Dynamic IOAPIC pin count, read from hardware in ApicInitializeIOApic */
UCHAR ApicMaxIrq = APIC_DEFAULT_MAX_IRQ;

/* PRIVATE FUNCTIONS **********************************************************/

/* Must be static: these touch the file-static HalpIoApicPolicyLock, and
 * FORCEINLINE expands to `extern inline' for GCC, which may not. */
static __inline
ULONG_PTR
HalpAcquireIoApicPolicyLock(VOID)
{
    ULONG_PTR Flags;

    Flags = __readeflags();
    _disable();
    while (InterlockedCompareExchange(&HalpIoApicPolicyLock, 1, 0) != 0)
        YieldProcessor();

    return Flags;
}

static __inline
VOID
HalpReleaseIoApicPolicyLock(
    _In_ ULONG_PTR Flags)
{
    InterlockedExchange(&HalpIoApicPolicyLock, 0);
    if (Flags & EFLAGS_INTERRUPT_MASK)
        _enable();
}

FORCEINLINE
ULONG
IOApicRead(UCHAR Register)
{
    ULONG_PTR Flags;
    ULONG Value;

    /* IOREGSEL/IOWIN is one shared indexed register pair for all CPUs. */
    Flags = __readeflags();
    _disable();
    while (InterlockedCompareExchange(&HalpIoApicLock, 1, 0) != 0)
        YieldProcessor();

    /* Select the register, then do the read */
    ASSERT(Register <= 0x10 + 2 * ApicMaxIrq);
    WRITE_REGISTER_ULONG((PULONG)(IOAPIC_BASE + IOAPIC_IOREGSEL), Register);
    Value = READ_REGISTER_ULONG((PULONG)(IOAPIC_BASE + IOAPIC_IOWIN));

    InterlockedExchange(&HalpIoApicLock, 0);
    if (Flags & EFLAGS_INTERRUPT_MASK)
        _enable();

    return Value;
}

FORCEINLINE
VOID
IOApicWrite(UCHAR Register, ULONG Value)
{
    ULONG_PTR Flags;

    /* Keep the selector stable until its data write has completed. */
    Flags = __readeflags();
    _disable();
    while (InterlockedCompareExchange(&HalpIoApicLock, 1, 0) != 0)
        YieldProcessor();

    /* Select the register, then do the write */
    ASSERT(Register <= 0x10 + 2 * ApicMaxIrq);
    WRITE_REGISTER_ULONG((PULONG)(IOAPIC_BASE + IOAPIC_IOREGSEL), Register);
    WRITE_REGISTER_ULONG((PULONG)(IOAPIC_BASE + IOAPIC_IOWIN), Value);

    InterlockedExchange(&HalpIoApicLock, 0);
    if (Flags & EFLAGS_INTERRUPT_MASK)
        _enable();
}

FORCEINLINE
VOID
ApicWriteIORedirectionEntry(
    UCHAR Index,
    IOAPIC_REDIRECTION_REGISTER ReDirReg)
{
    ASSERT(Index < ApicMaxIrq);
    IOApicWrite(IOAPIC_REDTBL + 2 * Index, ReDirReg.Long0);
    IOApicWrite(IOAPIC_REDTBL + 2 * Index + 1, ReDirReg.Long1);
}

FORCEINLINE
IOAPIC_REDIRECTION_REGISTER
ApicReadIORedirectionEntry(
    UCHAR Index)
{
    IOAPIC_REDIRECTION_REGISTER ReDirReg;

    ASSERT(Index < ApicMaxIrq);
    ReDirReg.Long0 = IOApicRead(IOAPIC_REDTBL + 2 * Index);
    ReDirReg.Long1 = IOApicRead(IOAPIC_REDTBL + 2 * Index + 1);

    return ReDirReg;
}

FORCEINLINE
VOID
ApicRequestSelfInterrupt(IN UCHAR Vector, UCHAR TriggerMode)
{
    ULONG Flags;
    APIC_INTERRUPT_COMMAND_REGISTER Icr;
    APIC_INTERRUPT_COMMAND_REGISTER IcrStatus;

    /*
     * The IRR registers are spaced 16 bytes apart and hold 32 status bits each.
     * Pre-compute the register and bit that match our vector.
     */
    ULONG VectorHigh = Vector / 32;
    ULONG VectorLow = Vector % 32;
    ULONG Irr = APIC_IRR + 0x10 * VectorHigh;
    ULONG IrrBit = 1UL << VectorLow;

    /* Setup the command register */
    Icr.LongLong = 0;
    Icr.Vector = Vector;
    Icr.MessageType = APIC_MT_Fixed;
    Icr.TriggerMode = TriggerMode;
    Icr.DestinationShortHand = APIC_DSH_Self;

    /* Disable interrupts so that we can change IRR without being interrupted */
    Flags = __readeflags();
    _disable();

    /* Wait for the APIC to be idle */
    do
    {
        IcrStatus.Long0 = ApicRead(APIC_ICR0);
    } while (IcrStatus.DeliveryStatus);

    /* Write high dword first, then low dword to send the interrupt */
    ApicWrite(APIC_ICR1, Icr.Long1);
    ApicWrite(APIC_ICR0, Icr.Long0);

    /* Wait until we see the interrupt request.
     * It will stay in requested state until we re-enable interrupts.
     */
    while (!(ApicRead(Irr) & IrrBit))
    {
        YieldProcessor();
    }

    /* Finally, restore the original interrupt state */
    if (Flags & EFLAGS_INTERRUPT_MASK)
    {
        _enable();
    }
}

FORCEINLINE
VOID
ApicSendEOI(void)
{
    ApicWrite(APIC_EOI, 0);
}

FORCEINLINE
KIRQL
ApicGetProcessorIrql(VOID)
{
    /* Read the TPR and convert it to an IRQL */
    return TprToIrql(ApicRead(APIC_PPR));
}

FORCEINLINE
KIRQL
ApicGetCurrentIrql(VOID)
{
#ifdef _M_AMD64
    return (KIRQL)__readcr8();
#elif defined(APIC_LAZY_IRQL)
    /* Return the field in the PCR */
    return (KIRQL)__readfsbyte(FIELD_OFFSET(KPCR, Irql));
#else
    /* Read the TPR and convert it to an IRQL */
    return TprToIrql(ApicRead(APIC_TPR));
#endif
}

FORCEINLINE
VOID
ApicSetIrql(KIRQL Irql)
{
#ifdef _M_AMD64
    __writecr8(Irql);
#elif defined(APIC_LAZY_IRQL)
    __writefsbyte(FIELD_OFFSET(KPCR, Irql), Irql);
#else
    /* Convert IRQL and write the TPR */
    ApicWrite(APIC_TPR, IrqlToTpr(Irql));
#endif
}
#define ApicRaiseIrql ApicSetIrql

#ifdef APIC_LAZY_IRQL
FORCEINLINE
VOID
ApicLowerIrql(KIRQL Irql)
{
    __writefsbyte(FIELD_OFFSET(KPCR, Irql), Irql);

    /* Is the new Irql lower than set in the TPR? */
    if (Irql < KeGetPcr()->IRR)
    {
        /* Save the new hard IRQL in the IRR field */
        KeGetPcr()->IRR = Irql;

        /* Need to lower it back */
        ApicWrite(APIC_TPR, IrqlToTpr(Irql));
    }
}
#else
#define ApicLowerIrql ApicSetIrql
#endif

UCHAR
FASTCALL
HalpIrqToVector(UCHAR Irq)
{
    IOAPIC_REDIRECTION_REGISTER ReDirReg;

    /* Read low dword of the redirection entry */
    ReDirReg.Long0 = IOApicRead(IOAPIC_REDTBL + 2 * Irq);

    /* Return the vector */
    return (UCHAR)ReDirReg.Vector;
}

KIRQL
FASTCALL
HalpVectorToIrql(UCHAR Vector)
{
    return TprToIrql(Vector);
}

UCHAR
FASTCALL
HalpVectorToIrq(UCHAR Vector)
{
    return HalpVectorToIndex[Vector];
}

VOID
NTAPI
HalpSendEOI(VOID)
{
    ApicSendEOI();
}

VOID
NTAPI
ApicInitializeLocalApic(ULONG Cpu)
{
    APIC_BASE_ADDRESS_REGISTER BaseRegister;
    APIC_SPURIOUS_INERRUPT_REGISTER SpIntRegister;
    LVT_REGISTER LvtEntry;

    /* Enable the APIC if it wasn't yet */
    BaseRegister.LongLong = __readmsr(MSR_APIC_BASE);
    BaseRegister.Enable = 1;
    BaseRegister.BootStrapCPUCore = (Cpu == 0);
    __writemsr(MSR_APIC_BASE, BaseRegister.LongLong);

    /* Set spurious vector and SoftwareEnable to 1 */
    SpIntRegister.Long = ApicRead(APIC_SIVR);
    SpIntRegister.Vector = APIC_SPURIOUS_VECTOR;
    SpIntRegister.SoftwareEnable = 1;
    SpIntRegister.FocusCPUCoreChecking = 0;
    ApicWrite(APIC_SIVR, SpIntRegister.Long);

    /* Read the version and save it globally */
    if (Cpu == 0) ApicVersion = ApicRead(APIC_VER);

    /* Set the mode to flat (max 8 CPUs supported!) */
    ApicWrite(APIC_DFR, APIC_DF_Flat);

    /* Set logical apic ID */
    ApicWrite(APIC_LDR, ApicLogicalId(Cpu) << 24);

    /* Set the spurious ISR */
    KeRegisterInterruptHandler(APIC_SPURIOUS_VECTOR, ApicSpuriousService);

    /* Create a template LVT */
    LvtEntry.Long = 0;
    LvtEntry.Vector = APIC_FREE_VECTOR;
    LvtEntry.MessageType = APIC_MT_Fixed;
    LvtEntry.DeliveryStatus = 0;
    LvtEntry.RemoteIRR = 0;
    LvtEntry.TriggerMode = APIC_TGM_Edge;
    LvtEntry.Mask = 1;
    LvtEntry.TimerMode = 0;

    /* Initialize and mask LVTs */
    ApicWrite(APIC_TMRLVTR, LvtEntry.Long);
    ApicWrite(APIC_THRMLVTR, LvtEntry.Long);
    ApicWrite(APIC_PCLVTR, LvtEntry.Long);
    ApicWrite(APIC_EXT0LVTR, LvtEntry.Long);
    ApicWrite(APIC_EXT1LVTR, LvtEntry.Long);
    ApicWrite(APIC_EXT2LVTR, LvtEntry.Long);
    ApicWrite(APIC_EXT3LVTR, LvtEntry.Long);

    /* LINT0 */
    LvtEntry.Vector = APIC_SPURIOUS_VECTOR;
    LvtEntry.MessageType = APIC_MT_ExtInt;
    ApicWrite(APIC_LINT0, LvtEntry.Long);

    /* Enable LINT1 (NMI) */
    LvtEntry.Mask = 0;
    LvtEntry.Vector = APIC_NMI_VECTOR;
    LvtEntry.MessageType = APIC_MT_NMI;
    LvtEntry.TriggerMode = APIC_TGM_Level;
    ApicWrite(APIC_LINT1, LvtEntry.Long);

    /* Enable error LVTR */
    LvtEntry.Vector = APIC_ERROR_VECTOR;
    LvtEntry.MessageType = APIC_MT_Fixed;
    ApicWrite(APIC_ERRLVTR, LvtEntry.Long);

    /* Set the IRQL from the PCR */
    ApicSetIrql(KeGetPcr()->Irql);
#ifdef APIC_LAZY_IRQL
    /* Save the new hard IRQL in the IRR field */
    KeGetPcr()->IRR = KeGetPcr()->Irql;
#endif
}

BOOLEAN
NTAPI
HalpSetIoApicInterruptAttributes(
    _In_ ULONG Gsi,
    _In_ BOOLEAN PolarityValid,
    _In_ BOOLEAN ActiveLow,
    _In_ BOOLEAN TriggerValid,
    _In_ BOOLEAN LevelTriggered)
{
    IOAPIC_REDIRECTION_REGISTER ReDirReg;
    ULONG_PTR Flags;
    LONG OldPolicy;
    LONG NewPolicy;

    if (Gsi >= ApicMaxIrq)
    {
        DPRINT1("IOAPIC: GSI %lu exceeds the %u available inputs\n",
                Gsi, ApicMaxIrq);
        return FALSE;
    }

    Flags = HalpAcquireIoApicPolicyLock();
    OldPolicy = HalpIoApicInputPolicy[Gsi];

    if ((PolarityValid &&
         (OldPolicy & HALP_IOAPIC_POLICY_POLARITY_VALID) &&
         (!!(OldPolicy & HALP_IOAPIC_POLICY_ACTIVE_LOW) != !!ActiveLow)) ||
        (TriggerValid &&
         (OldPolicy & HALP_IOAPIC_POLICY_TRIGGER_VALID) &&
         (!!(OldPolicy & HALP_IOAPIC_POLICY_LEVEL_TRIGGERED) !=
          !!LevelTriggered)))
    {
        HalpReleaseIoApicPolicyLock(Flags);
        DPRINT1("IOAPIC: conflicting attributes for GSI %lu\n", Gsi);
        return FALSE;
    }

    NewPolicy = OldPolicy & ~HALP_IOAPIC_POLICY_LOGGED;
    if (PolarityValid)
    {
        NewPolicy |= HALP_IOAPIC_POLICY_POLARITY_VALID;
        if (ActiveLow)
            NewPolicy |= HALP_IOAPIC_POLICY_ACTIVE_LOW;
        else
            NewPolicy &= ~HALP_IOAPIC_POLICY_ACTIVE_LOW;
    }

    if (TriggerValid)
    {
        NewPolicy |= HALP_IOAPIC_POLICY_TRIGGER_VALID;
        if (LevelTriggered)
            NewPolicy |= HALP_IOAPIC_POLICY_LEVEL_TRIGGERED;
        else
            NewPolicy &= ~HALP_IOAPIC_POLICY_LEVEL_TRIGGERED;
    }

    if (HalpIoApicInitialized)
    {
        ReDirReg = ApicReadIORedirectionEntry((UCHAR)Gsi);
        if (!ReDirReg.Mask &&
            (((NewPolicy & HALP_IOAPIC_POLICY_POLARITY_VALID) &&
              (ReDirReg.Polarity !=
               !!(NewPolicy & HALP_IOAPIC_POLICY_ACTIVE_LOW))) ||
             ((NewPolicy & HALP_IOAPIC_POLICY_TRIGGER_VALID) &&
              (ReDirReg.TriggerMode !=
               !!(NewPolicy & HALP_IOAPIC_POLICY_LEVEL_TRIGGERED)))))
        {
            HalpReleaseIoApicPolicyLock(Flags);
            DPRINT1("IOAPIC: GSI %lu is already enabled with different attributes\n",
                    Gsi);
            return FALSE;
        }
    }

    InterlockedExchange(&HalpIoApicInputPolicy[Gsi], NewPolicy);
    HalpReleaseIoApicPolicyLock(Flags);
    return TRUE;
}

static
LONG
HalpGetIoApicInterruptPolicy(
    _In_ UCHAR Gsi)
{
    /* Aligned LONG loads are atomic and acquire-ordered on x86/x64. */
    return HalpIoApicInputPolicy[Gsi];
}

static
VOID
HalpApplyIoApicInterruptAttributes(
    _In_ UCHAR Gsi,
    _Inout_ IOAPIC_REDIRECTION_REGISTER *ReDirReg)
{
    LONG Policy = HalpGetIoApicInterruptPolicy(Gsi);

    if (Policy & HALP_IOAPIC_POLICY_POLARITY_VALID)
    {
        ReDirReg->Polarity = !!(Policy & HALP_IOAPIC_POLICY_ACTIVE_LOW);
    }

    if (Policy & HALP_IOAPIC_POLICY_TRIGGER_VALID)
    {
        ReDirReg->TriggerMode =
            (Policy & HALP_IOAPIC_POLICY_LEVEL_TRIGGERED) ?
            APIC_TGM_Level : APIC_TGM_Edge;
    }
}

#if DBG
static
VOID
HalpLogIoApicInterruptAttributes(
    _In_ UCHAR Gsi,
    _In_ IOAPIC_REDIRECTION_REGISTER ReDirReg)
{
    LONG Policy;

    Policy = HalpGetIoApicInterruptPolicy(Gsi);
    if (Policy & HALP_IOAPIC_POLICY_LOGGED)
        return;

    Policy = InterlockedOr(&HalpIoApicInputPolicy[Gsi],
                           HALP_IOAPIC_POLICY_LOGGED);
    if (Policy & HALP_IOAPIC_POLICY_LOGGED)
        return;

    DPRINT1("IOAPIC: GSI %u vector 0x%02x RTE %08lx:%08lx "
            "polarity=%s trigger=%s policy=%u/%u\n",
            Gsi,
            (ULONG)ReDirReg.Vector,
            ReDirReg.Long1,
            ReDirReg.Long0,
            ReDirReg.Polarity ? "low" : "high",
            ReDirReg.TriggerMode ? "level" : "edge",
            !!(Policy & HALP_IOAPIC_POLICY_POLARITY_VALID),
            !!(Policy & HALP_IOAPIC_POLICY_TRIGGER_VALID));
}
#else
#define HalpLogIoApicInterruptAttributes(Gsi, ReDirReg) do { } while (0)
#endif

UCHAR
NTAPI
HalpAllocateSystemInterrupt(
    _In_ UCHAR Irq,
    _In_ UCHAR Vector)
{
    IOAPIC_REDIRECTION_REGISTER ReDirReg;

    ASSERT(Irq < ApicMaxIrq);
    ASSERT(HalpVectorToIndex[Vector] == APIC_FREE_VECTOR);

    /* Setup a redirection entry */
    ReDirReg.Vector = Vector;
    ReDirReg.MessageType = APIC_MT_LowestPriority;
    ReDirReg.DestinationMode = APIC_DM_Logical;
    ReDirReg.DeliveryStatus = 0;
    ReDirReg.Polarity = 0;
    ReDirReg.RemoteIRR = 0;
    ReDirReg.TriggerMode = APIC_TGM_Edge;
    ReDirReg.Mask = 1;
    ReDirReg.Reserved = 0;
    ReDirReg.Destination = ApicRead(APIC_ID) >> 24;

    HalpApplyIoApicInterruptAttributes(Irq, &ReDirReg);

    /* Initialize entry */
    ApicWriteIORedirectionEntry(Irq, ReDirReg);

    /* Save irq in the table */
    HalpVectorToIndex[Vector] = Irq;

    return Vector;
}

ULONG
NTAPI
HalpGetRootInterruptVector(
    _In_ ULONG BusInterruptLevel,
    _In_ ULONG BusInterruptVector,
    _Out_ PKIRQL OutIrql,
    _Out_ PKAFFINITY OutAffinity)
{
    UCHAR Vector;
    KIRQL Irql;

    if (BusInterruptLevel >= ApicMaxIrq)
    {
        DPRINT1("IOAPIC: GSI %lu exceeds the %u available inputs\n",
                BusInterruptLevel, ApicMaxIrq);
        *OutAffinity = 0;
        *OutIrql = 0;
        return 0;
    }

    /* Get the vector currently registered */
    Vector = HalpIrqToVector((UCHAR)BusInterruptLevel);

    /* Check if it's used */
    if (Vector != APIC_FREE_VECTOR)
    {
        /* Calculate IRQL */
        NT_ASSERT(HalpVectorToIndex[Vector] == BusInterruptLevel);
        *OutIrql = HalpVectorToIrql(Vector);
    }
    else
    {
        ULONG Offset;

        /* Outer loop to find alternative slots, when all IRQLs are in use */
        for (Offset = 0; Offset < 15; Offset++)
        {
            /* Loop allowed IRQL range */
            for (Irql = CLOCK_LEVEL - 1; Irql >= CMCI_LEVEL; Irql--)
            {
                /* Calculate the vactor */
                Vector = IrqlToTpr(Irql) + Offset;

                /* Check if the vector is free */
                if (HalpVectorToIrq(Vector) == APIC_FREE_VECTOR)
                {
                    /* Found one, allocate the interrupt */
                    Vector = HalpAllocateSystemInterrupt((UCHAR)BusInterruptLevel, Vector);
                    *OutIrql = Irql;
                    goto Exit;
                }
            }
        }

        DPRINT1("Failed to get an interrupt vector for IRQ %lu\n", BusInterruptLevel);
        *OutAffinity = 0;
        *OutIrql = 0;
        return 0;
    }

Exit:

    *OutAffinity = HalpDefaultInterruptAffinity;
    ASSERT(HalpDefaultInterruptAffinity);

    return Vector;
}

VOID
NTAPI
ApicInitializeIOApic(VOID)
{
    PHARDWARE_PTE Pte;
    IOAPIC_REDIRECTION_REGISTER ReDirReg;
    UCHAR Index;
    ULONG Vector;

    /* Map the I/O Apic page */
    Pte = HalAddressToPte(IOAPIC_BASE);
    Pte->PageFrameNumber = IOAPIC_PHYS_BASE / PAGE_SIZE;
    Pte->Valid = 1;
    Pte->Write = 1;
    Pte->Owner = 1;
    Pte->CacheDisable = 1;
    Pte->Global = 1;
    _ReadWriteBarrier();

    /* Read the actual IOAPIC pin count from the Version Register */
    {
        ULONG IoApicVersion = IOApicRead(IOAPIC_VER);
        UCHAR MaxRedir = (UCHAR)((IoApicVersion >> 16) & 0xFF);
        ApicMaxIrq = MaxRedir + 1; /* MaxRedirEntry is 0-based */
        if (ApicMaxIrq > APIC_ABSOLUTE_MAX_IRQ)
            ApicMaxIrq = APIC_ABSOLUTE_MAX_IRQ;
        DPRINT1("IOAPIC: %u redirection entries detected\n", ApicMaxIrq);
    }

    /* Setup a redirection entry */
    ReDirReg.Vector = APIC_FREE_VECTOR;
    ReDirReg.MessageType = APIC_MT_Fixed;
    ReDirReg.DestinationMode = APIC_DM_Physical;
    ReDirReg.DeliveryStatus = 0;
    ReDirReg.Polarity = 0;
    ReDirReg.RemoteIRR = 0;
    ReDirReg.TriggerMode = APIC_TGM_Edge;
    ReDirReg.Mask = 1;
    ReDirReg.Reserved = 0;
    ReDirReg.Destination = ApicRead(APIC_ID) >> 24;

    /* Loop all table entries */
    for (Index = 0; Index < ApicMaxIrq; Index++)
    {
        /* Initialize entry */
        ApicWriteIORedirectionEntry(Index, ReDirReg);
    }

    /* Init the vactor to index table */
    for (Vector = 0; Vector <= 255; Vector++)
    {
        HalpVectorToIndex[Vector] = APIC_FREE_VECTOR;
    }

    /* Enable the timer interrupt (but keep it masked) */
    ReDirReg.Vector = APIC_CLOCK_VECTOR;
    ReDirReg.MessageType = APIC_MT_Fixed;
    ReDirReg.DestinationMode = APIC_DM_Physical;
    ReDirReg.TriggerMode = APIC_TGM_Level;
    ReDirReg.Mask = 1;
    ReDirReg.Destination = ApicRead(APIC_ID) >> 24;
    ApicWriteIORedirectionEntry(APIC_CLOCK_INDEX, ReDirReg);

    HalpIoApicInitialized = TRUE;
}

VOID
NTAPI
HalpInitializePICs(IN BOOLEAN EnableInterrupts)
{
    ULONG_PTR EFlags;

    /* Save EFlags and disable interrupts */
    EFlags = __readeflags();
    _disable();

    /* Initialize and mask the PIC */
    HalpInitializeLegacyPICs();

    /* Initialize the I/O APIC */
    ApicInitializeIOApic();

    /* Manually reserve some vectors */
    HalpVectorToIndex[APC_VECTOR] = APIC_RESERVED_VECTOR;
    HalpVectorToIndex[DISPATCH_VECTOR] = APIC_RESERVED_VECTOR;
    HalpVectorToIndex[APIC_CLOCK_VECTOR] = 8;
    HalpVectorToIndex[CLOCK_IPI_VECTOR] = APIC_RESERVED_VECTOR;
    HalpVectorToIndex[APIC_SPURIOUS_VECTOR] = APIC_RESERVED_VECTOR;

    /* Set interrupt handlers in the IDT */
    KeRegisterInterruptHandler(APIC_CLOCK_VECTOR, HalpClockInterrupt);
    KeRegisterInterruptHandler(CLOCK_IPI_VECTOR, HalpClockIpi);
#ifndef _M_AMD64
    KeRegisterInterruptHandler(APC_VECTOR, HalpApcInterrupt);
    KeRegisterInterruptHandler(DISPATCH_VECTOR, HalpDispatchInterrupt);
#endif

    /* Register the vectors for APC and dispatch interrupts */
    HalpRegisterVector(IDT_INTERNAL, 0, APC_VECTOR, APC_LEVEL);
    HalpRegisterVector(IDT_INTERNAL, 0, DISPATCH_VECTOR, DISPATCH_LEVEL);

    /* Restore interrupt state */
    if (EnableInterrupts) EFlags |= EFLAGS_INTERRUPT_MASK;
    __writeeflags(EFlags);
}


/* SOFTWARE INTERRUPT TRAPS ***************************************************/

#ifndef _M_AMD64
VOID
DECLSPEC_NORETURN
FASTCALL
HalpApcInterruptHandler(IN PKTRAP_FRAME TrapFrame)
{
    KPROCESSOR_MODE ProcessorMode;
    KIRQL OldIrql;
    ASSERT(ApicGetProcessorIrql() == APC_LEVEL);

   /* Enter trap */
    KiEnterInterruptTrap(TrapFrame);

#ifdef APIC_LAZY_IRQL
    if (!HalBeginSystemInterrupt(APC_LEVEL, APC_VECTOR, &OldIrql))
    {
        /* "Spurious" interrupt, exit the interrupt */
        KiEoiHelper(TrapFrame);
    }
#else
    /* Save the old IRQL */
    OldIrql = ApicGetCurrentIrql();
    ASSERT(OldIrql < APC_LEVEL);
#endif

    /* Raise to APC_LEVEL */
    ApicRaiseIrql(APC_LEVEL);

    /* End the interrupt */
    ApicSendEOI();

    /* Kernel or user APC? */
    if (KiUserTrap(TrapFrame)) ProcessorMode = UserMode;
    else if (TrapFrame->EFlags & EFLAGS_V86_MASK) ProcessorMode = UserMode;
    else ProcessorMode = KernelMode;

    /* Enable interrupts and call the kernel's APC interrupt handler */
    _enable();
    KiDeliverApc(ProcessorMode, NULL, TrapFrame);

    /* Disable interrupts */
    _disable();

    /* Restore the old IRQL */
    ApicLowerIrql(OldIrql);

    /* Exit the interrupt */
    KiEoiHelper(TrapFrame);
}

VOID
DECLSPEC_NORETURN
FASTCALL
HalpDispatchInterruptHandler(IN PKTRAP_FRAME TrapFrame)
{
    KIRQL OldIrql;
    ASSERT(ApicGetProcessorIrql() == DISPATCH_LEVEL);

   /* Enter trap */
    KiEnterInterruptTrap(TrapFrame);

#ifdef APIC_LAZY_IRQL
    if (!HalBeginSystemInterrupt(DISPATCH_LEVEL, DISPATCH_VECTOR, &OldIrql))
    {
        /* "Spurious" interrupt, exit the interrupt */
        KiEoiHelper(TrapFrame);
    }
#else
    /* Get the current IRQL */
    OldIrql = ApicGetCurrentIrql();
    ASSERT(OldIrql < DISPATCH_LEVEL);
#endif

    /* Raise to DISPATCH_LEVEL */
    ApicRaiseIrql(DISPATCH_LEVEL);

    /* End the interrupt */
    ApicSendEOI();

    /* Enable interrupts and call the kernel's DPC interrupt handler */
    _enable();
    KiDispatchInterrupt();
    _disable();

    /* Restore the old IRQL */
    ApicLowerIrql(OldIrql);

    /* Exit the interrupt */
    KiEoiHelper(TrapFrame);
}
#endif


/* SOFTWARE INTERRUPTS ********************************************************/


VOID
FASTCALL
HalRequestSoftwareInterrupt(IN KIRQL Irql)
{
    /* Convert irql to vector and request an interrupt */
    ApicRequestSelfInterrupt(IrqlToSoftVector(Irql), APIC_TGM_Edge);
}

VOID
FASTCALL
HalClearSoftwareInterrupt(
    IN KIRQL Irql)
{
    /* Nothing to do */
}


/* SYSTEM INTERRUPTS **********************************************************/

BOOLEAN
NTAPI
HalEnableSystemInterrupt(
    IN ULONG Vector,
    IN KIRQL Irql,
    IN KINTERRUPT_MODE InterruptMode)
{
    IOAPIC_REDIRECTION_REGISTER ReDirReg;
    ULONG_PTR Flags;
    LONG Policy;
    BOOLEAN RequestedLevel;
    UCHAR Index;
    ASSERT(Irql <= HIGH_LEVEL);
    ASSERT((IrqlToTpr(Irql) & 0xF0) == (Vector & 0xF0));

    /* Get the irq for this vector */
    Index = HalpVectorToIndex[Vector];

    /* Check if its valid */
    if (Index == APIC_FREE_VECTOR)
    {
        /* Interrupt is not in use */
        return FALSE;
    }

    Flags = HalpAcquireIoApicPolicyLock();
    Policy = HalpIoApicInputPolicy[Index];
    RequestedLevel = (InterruptMode == LevelSensitive);

    if ((Policy & HALP_IOAPIC_POLICY_TRIGGER_VALID) &&
        (!!(Policy & HALP_IOAPIC_POLICY_LEVEL_TRIGGERED) != RequestedLevel))
    {
        HalpReleaseIoApicPolicyLock(Flags);
        DPRINT1("IOAPIC: GSI %u trigger policy conflicts with mode %u\n",
                Index, InterruptMode);
        return FALSE;
    }

    /* Read the redirection entry */
    ReDirReg = ApicReadIORedirectionEntry(Index);

    /* Check if the interrupt is already enabled */
    if (ReDirReg.Mask == FALSE)
    {
        if (ReDirReg.Vector != Vector)
        {
            HalpReleaseIoApicPolicyLock(Flags);
            return FALSE;
        }

        if (((Policy & HALP_IOAPIC_POLICY_POLARITY_VALID) &&
             (ReDirReg.Polarity !=
              !!(Policy & HALP_IOAPIC_POLICY_ACTIVE_LOW))) ||
            ((Policy & HALP_IOAPIC_POLICY_TRIGGER_VALID) &&
             (ReDirReg.TriggerMode !=
              !!(Policy & HALP_IOAPIC_POLICY_LEVEL_TRIGGERED))))
        {
            HalpReleaseIoApicPolicyLock(Flags);
            DPRINT1("IOAPIC: refusing to repolarize enabled GSI %u\n", Index);
            return FALSE;
        }

        HalpReleaseIoApicPolicyLock(Flags);
        HalpLogIoApicInterruptAttributes(Index, ReDirReg);
        return TRUE;
    }

    /* Set up the redirection entry */
    ReDirReg.Vector = Vector;
    ReDirReg.MessageType = APIC_MT_Fixed;
    ReDirReg.DestinationMode = APIC_DM_Physical;
    ReDirReg.Destination = ApicRead(APIC_ID) >> 24;
    ReDirReg.TriggerMode = RequestedLevel ? APIC_TGM_Level : APIC_TGM_Edge;
    HalpApplyIoApicInterruptAttributes(Index, &ReDirReg);
    ReDirReg.Mask = FALSE;

    /* Write back the entry */
    ApicWriteIORedirectionEntry(Index, ReDirReg);
    HalpReleaseIoApicPolicyLock(Flags);
    HalpLogIoApicInterruptAttributes(Index, ReDirReg);

    return TRUE;
}

VOID
NTAPI
HalDisableSystemInterrupt(
    IN ULONG Vector,
    IN KIRQL Irql)
{
    IOAPIC_REDIRECTION_REGISTER ReDirReg;
    UCHAR Index;
    ASSERT(Irql <= HIGH_LEVEL);
    ASSERT(Vector < RTL_NUMBER_OF(HalpVectorToIndex));

    Index = HalpVectorToIndex[Vector];

    /* Read lower dword of redirection entry */
    ReDirReg.Long0 = IOApicRead(IOAPIC_REDTBL + 2 * Index);

    /* Mask it */
    ReDirReg.Mask = 1;

    /* Write back lower dword */
    IOApicWrite(IOAPIC_REDTBL + 2 * Index, ReDirReg.Long0);
}

BOOLEAN
NTAPI
HalBeginSystemInterrupt(
    IN KIRQL Irql,
    IN ULONG Vector,
    OUT PKIRQL OldIrql)
{
    KIRQL CurrentIrql;

    /* Get the current IRQL */
    CurrentIrql = ApicGetCurrentIrql();

#ifdef APIC_LAZY_IRQL
    /* Check if this interrupt is allowed */
    if (CurrentIrql >= Irql)
    {
        IOAPIC_REDIRECTION_REGISTER RedirReg;
        UCHAR Index;

        /* It is not, set the real Irql in the TPR! */
        ApicWrite(APIC_TPR, IrqlToTpr(CurrentIrql));

        /* Save the new hard IRQL in the IRR field */
        KeGetPcr()->IRR = CurrentIrql;

        /* End this interrupt */
        ApicSendEOI();

        /* Get the irq for this vector */
        Index = HalpVectorToIndex[Vector];

        /* Check if it's valid */
        if (Index < ApicMaxIrq)
        {
            /* Read the I/O redirection entry */
            RedirReg = ApicReadIORedirectionEntry(Index);

            /* Re-request the interrupt to be handled later */
            ApicRequestSelfInterrupt(Vector, (UCHAR)RedirReg.TriggerMode);
       }
       else
       {
            /* This should be a reserved vector! */
            ASSERT(Index == APIC_RESERVED_VECTOR);

            /* Re-request the interrupt to be handled later */
            ApicRequestSelfInterrupt(Vector, APIC_TGM_Edge);
       }

        /* Pretend it was a spurious interrupt */
        return FALSE;
    }
#endif
    /* Save the current IRQL */
    *OldIrql = CurrentIrql;

    /* Set the new IRQL */
    ApicRaiseIrql(Irql);

    /* Turn on interrupts */
    _enable();

    /* Success */
    return TRUE;
}

VOID
NTAPI
HalEndSystemInterrupt(
    IN KIRQL OldIrql,
    IN PKTRAP_FRAME TrapFrame)
{
    /* Send an EOI */
    ApicSendEOI();

    /* Restore the old IRQL */
    ApicLowerIrql(OldIrql);
}


/* IRQL MANAGEMENT ************************************************************/

#ifndef _M_AMD64
KIRQL
NTAPI
KeGetCurrentIrql(VOID)
{
    /* Read the current TPR and convert it to an IRQL */
    return ApicGetCurrentIrql();
}

VOID
FASTCALL
KfLowerIrql(
    IN KIRQL OldIrql)
{
#if DBG
    /* Validate correct lower */
    if (OldIrql > ApicGetCurrentIrql())
    {
        /* Crash system */
        KeBugCheck(IRQL_NOT_LESS_OR_EQUAL);
    }
#endif
    /* Set the new IRQL */
    ApicLowerIrql(OldIrql);
}

KIRQL
FASTCALL
KfRaiseIrql(
    IN KIRQL NewIrql)
{
    KIRQL OldIrql;

    /* Read the current IRQL */
    OldIrql = ApicGetCurrentIrql();
#if DBG
    /* Validate correct raise */
    if (OldIrql > NewIrql)
    {
        /* Crash system */
        KeBugCheck(IRQL_NOT_GREATER_OR_EQUAL);
    }
#endif
    /* Convert the new IRQL to a TPR value and write the register */
    ApicRaiseIrql(NewIrql);

    /* Return old IRQL */
    return OldIrql;
}

KIRQL
NTAPI
KeRaiseIrqlToDpcLevel(VOID)
{
    return KfRaiseIrql(DISPATCH_LEVEL);
}

KIRQL
NTAPI
KeRaiseIrqlToSynchLevel(VOID)
{
    return KfRaiseIrql(SYNCH_LEVEL);
}

#endif /* !_M_AMD64 */
