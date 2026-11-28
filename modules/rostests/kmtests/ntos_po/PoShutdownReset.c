/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     A restart tells the ACPI firmware what an S5 transition does
 * COPYRIGHT:   Copyright 2026 ReactOS Team
 */

/*
 * Windows goes through S5 for a restart ("A full shutdown (S5) occurs when a
 * system restart is requested"), so before the reset register is written the
 * firmware sees _TTS(5) and then _PTS(5), as it does for a power-off. Some
 * firmware depends on that to reset properly.
 *
 * This test needs a QEMU guest with a test SSDT (rostests kmtests
 * ntos_po/PoShutdownReset.asl, given to QEMU with -acpitable) and without
 * -no-reboot. The table's \_TTS and \_PTS append their argument to a log in
 * CMOS RAM, which a QEMU reset keeps. The first run arms the test and restarts
 * the machine with NtShutdownSystem(ShutdownReboot); the run on the next boot
 * reads the log. Without the table the test is skipped.
 */

#include <kmt_test.h>

#define CMOS_STATE      0x68    /* test state, written only by the test */
#define CMOS_LOG_COUNT  0x69    /* entries the SSDT logged */
#define CMOS_LOG        0x6A    /* up to 5 entries */
#define CMOS_LOG_MAX    5
#define CMOS_PRESENT    0x6F    /* 0xA5 from the SSDT's _INI on every boot */

#define STATE_ARMED     0x5A
#define SSDT_PRESENT    0xA5

#define LOG_TTS(s)      (0x10 | (s))
#define LOG_PTS(s)      (0x20 | (s))

/* SHUTDOWN_ACTION */
#define TEST_SHUTDOWN_REBOOT 1

typedef NTSTATUS (NTAPI *PFN_NT_SHUTDOWN_SYSTEM)(ULONG Action);

static PFN_NT_SHUTDOWN_SYSTEM pNtShutdownSystem;

static
UCHAR
ReadCmos(
    _In_ ULONG Address)
{
    UCHAR Value = 0;

    HalGetBusDataByOffset(Cmos, 0, Address, &Value, 0, sizeof(Value));
    return Value;
}

static
VOID
WriteCmos(
    _In_ ULONG Address,
    _In_ UCHAR Value)
{
    HalSetBusDataByOffset(Cmos, 0, Address, &Value, 0, sizeof(Value));
}

static KSTART_ROUTINE RestartThread;

static
VOID
NTAPI
RestartThread(
    _In_ PVOID Context)
{
    UNREFERENCED_PARAMETER(Context);

    /* A system thread, so the call is made from kernel mode */
    pNtShutdownSystem(TEST_SHUTDOWN_REBOOT);
    PsTerminateSystemThread(STATUS_SUCCESS);
}

static
VOID
CheckRestartLog(VOID)
{
    UCHAR Count, Index;
    UCHAR Log[CMOS_LOG_MAX];

    Count = ReadCmos(CMOS_LOG_COUNT);
    for (Index = 0; Index < CMOS_LOG_MAX; Index++)
        Log[Index] = ReadCmos(CMOS_LOG + Index);

    trace("restart log: count %u: %02x %02x %02x %02x %02x\n",
          Count, Log[0], Log[1], Log[2], Log[3], Log[4]);

    ok_eq_uint(Count, 2);
    ok(Log[0] == LOG_TTS(5), "first method: 0x%02x, expected _TTS(5) 0x%02x\n",
       Log[0], LOG_TTS(5));
    ok(Log[1] == LOG_PTS(5), "second method: 0x%02x, expected _PTS(5) 0x%02x\n",
       Log[1], LOG_PTS(5));
}

START_TEST(PoShutdownReset)
{
    UNICODE_STRING Name = RTL_CONSTANT_STRING(L"NtShutdownSystem");
    HANDLE Thread;
    NTSTATUS Status;
    UCHAR Index;

    if (skip(ReadCmos(CMOS_PRESENT) == SSDT_PRESENT,
             "No restart test SSDT in this machine\n"))
    {
        return;
    }

    if (ReadCmos(CMOS_STATE) == STATE_ARMED)
    {
        /* The boot after the restart */
        WriteCmos(CMOS_STATE, 0);
        CheckRestartLog();
        return;
    }

    pNtShutdownSystem = (PFN_NT_SHUTDOWN_SYSTEM)MmGetSystemRoutineAddress(&Name);
    if (skip(pNtShutdownSystem != NULL, "NtShutdownSystem is not exported\n"))
        return;

    /* Nothing logged before the restart */
    WriteCmos(CMOS_LOG_COUNT, 0);
    for (Index = 0; Index < CMOS_LOG_MAX; Index++)
        WriteCmos(CMOS_LOG + Index, 0);
    WriteCmos(CMOS_STATE, STATE_ARMED);

    trace("restarting; the test continues on the next boot\n");
    Status = PsCreateSystemThread(&Thread, THREAD_ALL_ACCESS, NULL, NULL, NULL,
                                  RestartThread, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
    {
        WriteCmos(CMOS_STATE, 0);
        return;
    }
    ZwWaitForSingleObject(Thread, FALSE, NULL);
    ZwClose(Thread);

    /* NtShutdownSystem(ShutdownReboot) must not come back */
    WriteCmos(CMOS_STATE, 0);
    ok(FALSE, "the machine did not restart\n");
}
