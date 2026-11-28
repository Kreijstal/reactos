/*
 * QEMU test SSDT for the restart path of the xHCI driver (kmtest
 * PoShutdownResetUsb), e.g.
 *   iasl PoShutdownResetUsb.asl &&
 *   qemu-system-x86_64 -acpitable file=PoShutdownResetUsb.aml \
 *       -device qemu-xhci,id=xhci,addr=0x8 -device usb-storage,bus=xhci.0,drive=... ...
 *
 * \_PTS(5) runs after every device has left D0, right before the reset. It
 * finds the xHCI controller at PCI 00:08.0 (QEMU's slot object \_SB.PCI0.S40),
 * reads PORTSC of its first 8 root ports and stores one byte per port in CMOS
 * RAM 0x60..0x67: 0x80 | (CCS << 4) | PLS. QEMU keeps CMOS RAM across a system
 * reset, so the test reads the snapshot on the boot after the restart.
 * 0x68 is the test state byte, 0x6F is set to 0xA6 by _INI on every boot to
 * show the test that this table is loaded.
 */
DefinitionBlock ("PoShutdownResetUsb.aml", "SSDT", 2, "ROSTST", "RSTUSB", 1)
{
    OperationRegion (UCMS, SystemIO, 0x70, 0x02)
    Field (UCMS, ByteAcc, NoLock, Preserve)
    {
        UIDX,   8,
        UDAT,   8
    }
    IndexField (UIDX, UDAT, ByteAcc, NoLock, Preserve)
    {
        Offset (0x60),
        UP1,    8,
        UP2,    8,
        UP3,    8,
        UP4,    8,
        UP5,    8,
        UP6,    8,
        UP7,    8,
        UP8,    8,
        Offset (0x6F),
        UPRS,   8
    }

    External (\_SB.PCI0.S40, DeviceObj)

    Scope (\_SB.PCI0.S40)
    {
        OperationRegion (UPCI, PCI_Config, Zero, 0x18)
        Field (UPCI, DWordAcc, NoLock, Preserve)
        {
            Offset (0x08),
            UCLS,   32,
            Offset (0x10),
            UBR0,   32,
            UBR1,   32
        }
    }

    /* Snapshot byte of a PORTSC value */
    Method (USNP, 1, NotSerialized)
    {
        Return ((0x80 | ((Arg0 & 0x01) << 4) | ((Arg0 >> 5) & 0x0F)))
    }

    Method (USAV, 0, Serialized)
    {
        /* 00:08.0 must be an xHCI controller (class 0C0330) */
        If (((\_SB.PCI0.S40.UCLS >> 8) != 0x0C0330))
        {
            Return (Zero)
        }

        Local0 = (\_SB.PCI0.S40.UBR0 & 0xFFFFFFF0)
        Local1 = \_SB.PCI0.S40.UBR1
        If (((Local0 == Zero) || (Local1 != Zero)))
        {
            Return (Zero)
        }

        OperationRegion (UCAP, SystemMemory, Local0, 0x08)
        Field (UCAP, DWordAcc, NoLock, Preserve)
        {
            UCPL,   32,
            UHS1,   32
        }

        Local2 = (UHS1 >> 24)
        Local3 = (Local0 + (UCPL & 0xFF) + 0x0400)
        OperationRegion (UPSC, SystemMemory, Local3, 0x80)
        Field (UPSC, DWordAcc, NoLock, Preserve)
        {
            PS1,    32, Offset (0x10),
            PS2,    32, Offset (0x20),
            PS3,    32, Offset (0x30),
            PS4,    32, Offset (0x40),
            PS5,    32, Offset (0x50),
            PS6,    32, Offset (0x60),
            PS7,    32, Offset (0x70),
            PS8,    32
        }

        If ((Local2 >= 1)) { UP1 = USNP (PS1) }
        If ((Local2 >= 2)) { UP2 = USNP (PS2) }
        If ((Local2 >= 3)) { UP3 = USNP (PS3) }
        If ((Local2 >= 4)) { UP4 = USNP (PS4) }
        If ((Local2 >= 5)) { UP5 = USNP (PS5) }
        If ((Local2 >= 6)) { UP6 = USNP (PS6) }
        If ((Local2 >= 7)) { UP7 = USNP (PS7) }
        If ((Local2 >= 8)) { UP8 = USNP (PS8) }
        Return (One)
    }

    Method (\_PTS, 1, NotSerialized)
    {
        If ((Arg0 == 0x05))
        {
            USAV ()
        }
    }

    Scope (\_SB)
    {
        Device (RSTU)
        {
            Name (_HID, EisaId ("PNP0C02"))
            Name (_UID, 0x5255)
            Method (_STA, 0, NotSerialized) { Return (0x0B) }
            Method (_INI, 0, NotSerialized) { UPRS = 0xA6 }
        }
    }
}
