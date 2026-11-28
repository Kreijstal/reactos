/*
 * QEMU test SSDT for the restart path (kmtest PoShutdownReset), e.g.
 *   iasl PoShutdownReset.asl && qemu-system-x86_64 -acpitable file=PoShutdownReset.aml ...
 *
 * \_TTS and \_PTS append (0x10 | Arg0) and (0x20 | Arg0) to a log in
 * CMOS RAM 0x69..0x6E (count at 0x69). QEMU keeps CMOS RAM across a system
 * reset, so the test reads the log back on the boot after the restart.
 * 0x68 is the test's own state byte, 0x6F is set to 0xA5 by _INI on every boot
 * to show the test that this table is loaded.
 */
DefinitionBlock ("PoShutdownReset.aml", "SSDT", 2, "ROSTST", "RSTTEST", 1)
{
    OperationRegion (RCMS, SystemIO, 0x70, 0x02)
    Field (RCMS, ByteAcc, NoLock, Preserve)
    {
        RIDX,   8,
        RDAT,   8
    }
    IndexField (RIDX, RDAT, ByteAcc, NoLock, Preserve)
    {
        Offset (0x69),
        RCNT,   8,
        RLG0,   8,
        RLG1,   8,
        RLG2,   8,
        RLG3,   8,
        RLG4,   8,
        RPRS,   8
    }

    Mutex (RLCK, 0)
    Method (RLOG, 1, Serialized)
    {
        Acquire (RLCK, 0xFFFF)
        Local0 = RCNT
        If ((Local0 == 0x00)) { RLG0 = Arg0 }
        If ((Local0 == 0x01)) { RLG1 = Arg0 }
        If ((Local0 == 0x02)) { RLG2 = Arg0 }
        If ((Local0 == 0x03)) { RLG3 = Arg0 }
        If ((Local0 == 0x04)) { RLG4 = Arg0 }
        If ((Local0 < 0xFF)) { RCNT = (Local0 + 1) }
        Release (RLCK)
    }

    Method (\_TTS, 1, NotSerialized)
    {
        RLOG ((0x10 | Arg0))
    }

    Method (\_PTS, 1, NotSerialized)
    {
        RLOG ((0x20 | Arg0))
    }

    Scope (\_SB)
    {
        Device (RSTD)
        {
            Name (_HID, EisaId ("PNP0C02"))
            Name (_UID, 0x5254)
            Method (_STA, 0, NotSerialized) { Return (0x0B) }
            Method (_INI, 0, NotSerialized) { RPRS = 0xA5 }
        }
    }
}
