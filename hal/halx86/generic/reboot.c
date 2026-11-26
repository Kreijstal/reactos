/*
 * PROJECT:         ReactOS HAL
 * LICENSE:         GPL - See COPYING in the top level directory
 * FILE:            hal/halx86/generic/reboot.c
 * PURPOSE:         Reboot functions
 * PROGRAMMERS:     Alex Ionescu (alex.ionescu@reactos.org)
 *                  Eric Kohl
 */

/* INCLUDES ******************************************************************/

#include <hal.h>

/* PRIVATE FUNCTIONS *********************************************************/

static VOID
HalpWriteResetCommand(VOID)
{
    ULONG Wait;

    /* The firmware's own lever first: the ACPI FADT reset register, as the
     * ACPI HAL of Windows and Linux (reboot=acpi, its default) do. The board
     * names here how it wants to be reset; on the ASUS X550DP it is an SMI
     * to the firmware (I/O port 0xB2, the SMI command port, <- 0x8E). */
    if (HalpAcpiWriteResetRegister())
    {
        /* Give the firmware time to act before falling back */
        KeStallExecutionProcessor(500 * 1000);
    }

    /* Generate RESET signal via keyboard controller. The controller only
     * takes a command once its input buffer is empty; a byte written while
     * it is full is lost. On boards where the EC emulates the KBC the pulse
     * is acknowledged and may be ignored, so the lever below follows. */
    for (Wait = 0; Wait < 100000; Wait++)
    {
        if (!(READ_PORT_UCHAR((PUCHAR)0x64) & 0x02)) break;
        KeStallExecutionProcessor(2);
    }
    WRITE_PORT_UCHAR((PUCHAR)0x64, 0xFE);
    KeStallExecutionProcessor(50 * 1000);

#if defined(__GNUC__)
    /* Last resort, a triple fault: a fault with an empty IDT cannot be
     * delivered and the CPU enters SHUTDOWN. Whether the chipset turns that
     * into a reset is chipset policy, and a reset that way may not reset
     * the PCI devices. It cannot return, so it must come after every lever
     * that can. */
    {
        static const struct
        {
            USHORT Limit;
            ULONG_PTR Base;
        } __attribute__((packed)) NullIdt = { 0, 0 };

        __asm__ __volatile__("lidt %0" : : "m"(NullIdt));
        __asm__ __volatile__("int3");
    }
#endif
};

DECLSPEC_NORETURN
VOID
HalpReboot(VOID)
{
    PHYSICAL_ADDRESS PhysicalAddress;
    UCHAR Data;
    PVOID ZeroPageMapping;

    /* Map the first physical page */
    PhysicalAddress.QuadPart = 0;
    ZeroPageMapping = HalpMapPhysicalMemory64(PhysicalAddress, 1);

    /* Enable warm reboot */
    ((PUSHORT)ZeroPageMapping)[0x239] = 0x1234;

    /* Lock CMOS Access (and disable interrupts) */
    HalpAcquireCmosSpinLock();

    /* Setup control register B */
    WRITE_PORT_UCHAR((PUCHAR)0x70, 0x0B);
    KeStallExecutionProcessor(1);

    /* Read periodic register and clear the interrupt enable */
    Data = READ_PORT_UCHAR((PUCHAR)0x71);
    WRITE_PORT_UCHAR((PUCHAR)0x71, Data & ~0x40);
    KeStallExecutionProcessor(1);

    /* Setup control register A */
    WRITE_PORT_UCHAR((PUCHAR)0x70, 0x0A);
    KeStallExecutionProcessor(1);

    /* Read divider rate and reset it */
    Data = READ_PORT_UCHAR((PUCHAR)0x71);
    WRITE_PORT_UCHAR((PUCHAR)0x71, (Data & ~0x9) | 0x06);
    KeStallExecutionProcessor(1);

    /* Reset neutral CMOS address */
    WRITE_PORT_UCHAR((PUCHAR)0x70, 0x15);
    KeStallExecutionProcessor(1);

    /* Flush write buffers and send the reset command */
    KeFlushWriteBuffer();
    HalpWriteResetCommand();

    /* Halt the CPU */
    __halt();
    UNREACHABLE;
}

/* PUBLIC FUNCTIONS **********************************************************/

#ifndef _MINIHAL_
/*
 * @implemented
 */
VOID
NTAPI
HalReturnToFirmware(
    _In_ FIRMWARE_REENTRY Action)
{
    /* Check what kind of action this is */
    switch (Action)
    {
        /* All recognized actions */
        case HalHaltRoutine:
        case HalPowerDownRoutine:
        case HalRestartRoutine:
        case HalRebootRoutine:
        {
            /* Acquire the display */
            InbvAcquireDisplayOwnership();

            /* Call the internal reboot function */
            HalpReboot();
        }

        /* Anything else */
        default:
        {
            /* Print message and break */
            DbgPrint("HalReturnToFirmware called!\n");
            DbgBreakPoint();
        }
    }
}
#endif // _MINIHAL_

/* EOF */
