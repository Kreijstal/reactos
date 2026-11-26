/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Kernel-Mode Test Suite test for the IRQL kept across a debugger
 *              freeze when several processors enter the debugger at once
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

#include <kmt_test.h>

/*
 * Every DbgPrint that reaches an attached debugger freezes the other
 * processors (KeFreezeExecution) and thaws them again (KeThawExecution),
 * raising to HIGH_LEVEL in between. The IRQL the caller had must come back
 * when the print returns, whatever the other processors do meanwhile.
 *
 * Here every processor prints at the same time, half of them at PASSIVE_LEVEL
 * and half at DISPATCH_LEVEL, so that the freeze is handed from one processor
 * to the next all the time. A processor that returns from DbgPrint at the IRQL
 * of another processor's print is the failure this test counts.
 */

#define PRINTS_PER_THREAD 5000
#define MAX_THREADS 8

typedef struct _FREEZE_PRINTER
{
    ULONG Processor;
    KIRQL Irql;
    PKEVENT Start;
    volatile LONG *Mismatches;
    KIRQL WrongIrql;
    ULONG Prints;
} FREEZE_PRINTER, *PFREEZE_PRINTER;

static
VOID
NTAPI
PrinterThread(
    _In_ PVOID Context)
{
    PFREEZE_PRINTER Printer = Context;
    KIRQL OldIrql, Irql;
    ULONG i;

    KeSetSystemAffinityThread(AFFINITY_MASK(Printer->Processor));
    KeWaitForSingleObject(Printer->Start, Executive, KernelMode, FALSE, NULL);

    for (i = 0; i < PRINTS_PER_THREAD; i++)
    {
        KeRaiseIrql(Printer->Irql, &OldIrql);
        DbgPrint("KeFreezeIrql %lu.%lu\n", Printer->Processor, i);
        Irql = KeGetCurrentIrql();
        if (Irql != Printer->Irql)
        {
            /* Count it and put the IRQL back where it belongs */
            InterlockedIncrement(Printer->Mismatches);
            Printer->WrongIrql = Irql;
            if (Irql > Printer->Irql)
                KeLowerIrql(Printer->Irql);
            else
                KeRaiseIrql(Printer->Irql, &Irql);
        }
        KeLowerIrql(OldIrql);
        Printer->Prints++;
    }

    KeRevertToUserAffinityThread();
    PsTerminateSystemThread(STATUS_SUCCESS);
}

START_TEST(KeFreezeIrql)
{
    FREEZE_PRINTER Printers[MAX_THREADS];
    PVOID Threads[MAX_THREADS];
    volatile LONG Mismatches = 0;
    KEVENT Start;
    ULONG Count, i;
    HANDLE Handle;
    NTSTATUS Status;

    if (KeNumberProcessors < 2)
    {
        skip(FALSE, "Needs two processors\n");
        return;
    }
    if (!KD_DEBUGGER_ENABLED || KD_DEBUGGER_NOT_PRESENT)
    {
        skip(FALSE, "Needs an attached kernel debugger\n");
        return;
    }

    Count = min((ULONG)KeNumberProcessors, MAX_THREADS);
    KeInitializeEvent(&Start, NotificationEvent, FALSE);

    for (i = 0; i < Count; i++)
    {
        Printers[i].Processor = i;
        Printers[i].Irql = (i & 1) ? DISPATCH_LEVEL : PASSIVE_LEVEL;
        Printers[i].Start = &Start;
        Printers[i].Mismatches = &Mismatches;
        Printers[i].WrongIrql = Printers[i].Irql;
        Printers[i].Prints = 0;
        Threads[i] = NULL;

        Status = PsCreateSystemThread(&Handle, THREAD_ALL_ACCESS, NULL, NULL, NULL, PrinterThread, &Printers[i]);
        ok_eq_hex(Status, STATUS_SUCCESS);
        if (!NT_SUCCESS(Status))
            continue;
        Status = ObReferenceObjectByHandle(Handle, SYNCHRONIZE, *PsThreadType, KernelMode, &Threads[i], NULL);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ZwClose(Handle);
    }

    KeSetEvent(&Start, IO_NO_INCREMENT, FALSE);

    for (i = 0; i < Count; i++)
    {
        if (!Threads[i])
            continue;
        KeWaitForSingleObject(Threads[i], Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(Threads[i]);
        ok(Printers[i].Prints == PRINTS_PER_THREAD, "Processor %lu printed %lu times\n",
           i, Printers[i].Prints);
        ok(Printers[i].WrongIrql == Printers[i].Irql,
           "Processor %lu came back from DbgPrint at IRQL %u instead of %u\n",
           i, Printers[i].WrongIrql, Printers[i].Irql);
    }

    ok(Mismatches == 0, "%ld of %lu prints came back at another processor's IRQL\n",
       Mismatches, Count * PRINTS_PER_THREAD);
}
