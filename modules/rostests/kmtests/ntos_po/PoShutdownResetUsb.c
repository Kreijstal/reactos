/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     A restart leaves no USB device suspended at an xHCI root port
 * COPYRIGHT:   Copyright 2026 ReactOS Team
 */

/*
 * A restart does not remove the power of the USB ports, so a device the
 * shutdown suspended is still suspended (U3) when the firmware starts again.
 * Boot firmware cannot always use a port in U3 (Linux: "Bootloader/firmware
 * on these systems can't handle usb ports in U3", xhci commits 34cd2db408d5
 * and c3bbacd61baa) and may then not find the boot device at all.
 *
 * This test needs a QEMU guest with a test SSDT (rostests kmtests
 * ntos_po/PoShutdownResetUsb.asl, given to QEMU with -acpitable), the xHCI
 * controller at 00:08.0 with a USB device on it, and no -no-reboot. The
 * table's \_PTS(5), which runs after every device has left D0, stores the
 * link state of every root port in CMOS RAM, which a QEMU reset keeps. The
 * first run arms the test and restarts the machine with
 * NtShutdownSystem(ShutdownReboot); the run on the next boot checks the
 * snapshot. Without the table the test is skipped.
 */

#include <kmt_test.h>

#define CMOS_PORTS      0x60    /* one snapshot byte per root port */
#define CMOS_PORT_MAX   8
#define CMOS_STATE      0x68    /* test state, written only by the test */
#define CMOS_PRESENT    0x6F    /* 0xA6 from the SSDT's _INI on every boot */

#define STATE_ARMED     0x5B
#define SSDT_PRESENT    0xA6

/* Snapshot byte: 0x80 | (CCS << 4) | PLS */
#define SNAP_VALID      0x80
#define SNAP_CONNECTED  0x10
#define SNAP_PLS(b)     ((b) & 0x0F)
#define PLS_U3          3

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
CheckPortSnapshot(VOID)
{
    UCHAR Snap[CMOS_PORT_MAX];
    ULONG Index, Valid = 0, Connected = 0;

    for (Index = 0; Index < CMOS_PORT_MAX; Index++)
    {
        Snap[Index] = ReadCmos(CMOS_PORTS + Index);
        if (Snap[Index] & SNAP_VALID)
            Valid++;
    }

    trace("root ports at _PTS(5): %02x %02x %02x %02x %02x %02x %02x %02x\n",
          Snap[0], Snap[1], Snap[2], Snap[3], Snap[4], Snap[5], Snap[6], Snap[7]);

    ok(Valid != 0, "_PTS(5) took no snapshot of the xHCI root ports\n");

    for (Index = 0; Index < CMOS_PORT_MAX; Index++)
    {
        if (!(Snap[Index] & SNAP_VALID) || !(Snap[Index] & SNAP_CONNECTED))
            continue;

        Connected++;
        ok(SNAP_PLS(Snap[Index]) != PLS_U3,
           "root port %lu: device left suspended (U3) for the reset\n", Index + 1);
    }

    if (Valid != 0)
        ok(Connected != 0, "no device on the xHCI root ports\n");
}

START_TEST(PoShutdownResetUsb)
{
    UNICODE_STRING Name = RTL_CONSTANT_STRING(L"NtShutdownSystem");
    HANDLE Thread;
    NTSTATUS Status;
    ULONG Index;

    if (skip(ReadCmos(CMOS_PRESENT) == SSDT_PRESENT,
             "No USB restart test SSDT in this machine\n"))
    {
        return;
    }

    if (ReadCmos(CMOS_STATE) == STATE_ARMED)
    {
        /* The boot after the restart */
        WriteCmos(CMOS_STATE, 0);
        CheckPortSnapshot();
        return;
    }

    pNtShutdownSystem = (PFN_NT_SHUTDOWN_SYSTEM)MmGetSystemRoutineAddress(&Name);
    if (skip(pNtShutdownSystem != NULL, "NtShutdownSystem is not exported\n"))
        return;

    /* Nothing recorded before the restart */
    for (Index = 0; Index < CMOS_PORT_MAX; Index++)
        WriteCmos(CMOS_PORTS + Index, 0);
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
