/*
 * NTFS Unicode Uppercase Table Builder
 * Based on ntfs-3g unistr.c (GPL-2.0-or-later)
 *
 * Generates the default NTFS upcase table (65536 entries).
 * This must match what Windows uses exactly, or directory
 * lookups will fail.
 */
#include "ntfs_types.h"

void ntfs_upcase_table_build(WCHAR *uc, ULONG uc_len)
{
    ULONG i;

    /* Initialize: each character maps to itself */
    for (i = 0; i < uc_len; i++)
        uc[i] = (WCHAR)i;

    /* Basic Latin lowercase -> uppercase (a-z) */
    for (i = 0x0061; i <= 0x007A; i++)
        uc[i] = (WCHAR)(i - 0x20);

    /* Latin-1 Supplement lowercase (0xE0-0xF6, 0xF8-0xFE) */
    for (i = 0x00E0; i <= 0x00F6; i++)
        uc[i] = (WCHAR)(i - 0x20);
    for (i = 0x00F8; i <= 0x00FE; i++)
        uc[i] = (WCHAR)(i - 0x20);

    /* Latin Extended-A: alternating pairs 0x0100-0x012F */
    for (i = 0x0101; i <= 0x012F; i += 2)
        uc[i] = (WCHAR)(i - 1);
    /* 0x0132-0x0137 */
    for (i = 0x0133; i <= 0x0137; i += 2)
        uc[i] = (WCHAR)(i - 1);
    /* 0x0139-0x0148 */
    for (i = 0x013A; i <= 0x0148; i += 2)
        uc[i] = (WCHAR)(i - 1);
    /* 0x014A-0x0177 */
    for (i = 0x014B; i <= 0x0177; i += 2)
        uc[i] = (WCHAR)(i - 1);
    /* 0x0179-0x017E */
    for (i = 0x017A; i <= 0x017E; i += 2)
        uc[i] = (WCHAR)(i - 1);

    /* Latin Extended-B selections */
    for (i = 0x01A1; i <= 0x01A5; i += 2)
        uc[i] = (WCHAR)(i - 1);
    for (i = 0x01B4; i <= 0x01B6; i += 2)
        uc[i] = (WCHAR)(i - 1);
    for (i = 0x01CE; i <= 0x01DC; i += 2)
        uc[i] = (WCHAR)(i - 1);
    for (i = 0x01DF; i <= 0x01EF; i += 2)
        uc[i] = (WCHAR)(i - 1);
    for (i = 0x01F9; i <= 0x021F; i += 2)
        uc[i] = (WCHAR)(i - 1);
    for (i = 0x0223; i <= 0x0233; i += 2)
        uc[i] = (WCHAR)(i - 1);

    /* Greek lowercase -> uppercase (0x03B1-0x03C9 -> 0x0391-0x03A9) */
    for (i = 0x03B1; i <= 0x03C1; i++)
        uc[i] = (WCHAR)(i - 0x20);
    /* 0x03C2 (final sigma) - Windows NTFS keeps as-is (no case mapping) */
    for (i = 0x03C3; i <= 0x03C9; i++)
        uc[i] = (WCHAR)(i - 0x20);

    /* Cyrillic lowercase -> uppercase (0x0430-0x044F -> 0x0410-0x042F) */
    for (i = 0x0430; i <= 0x044F; i++)
        uc[i] = (WCHAR)(i - 0x20);
    /* Cyrillic Extended (0x0450-0x045F -> 0x0400-0x040F) */
    for (i = 0x0450; i <= 0x045F; i++)
        uc[i] = (WCHAR)(i - 0x50);

    /* Coptic/Cyrillic pairs */
    for (i = 0x0461; i <= 0x0481; i += 2)
        uc[i] = (WCHAR)(i - 1);
    for (i = 0x048B; i <= 0x04BF; i += 2)
        uc[i] = (WCHAR)(i - 1);
    for (i = 0x04C2; i <= 0x04CE; i += 2)
        uc[i] = (WCHAR)(i - 1);
    for (i = 0x04D1; i <= 0x04F5; i += 2)
        uc[i] = (WCHAR)(i - 1);
    for (i = 0x04F9; i <= 0x04F9; i += 2)
        uc[i] = (WCHAR)(i - 1);

    /* Armenian lowercase (0x0561-0x0586 -> 0x0531-0x0556) */
    for (i = 0x0561; i <= 0x0586; i++)
        uc[i] = (WCHAR)(i - 0x30);

    /* Georgian (0x10D0-0x10F5 -> 0x10A0-0x10C5) */
    for (i = 0x10D0; i <= 0x10F5; i++)
        uc[i] = (WCHAR)(i - 0x30);

    /* Latin Extended Additional (0x1E01-0x1E95 pairs) */
    for (i = 0x1E01; i <= 0x1E95; i += 2)
        uc[i] = (WCHAR)(i - 1);
    /* 0x1EA1-0x1EF9 */
    for (i = 0x1EA1; i <= 0x1EF9; i += 2)
        uc[i] = (WCHAR)(i - 1);

    /* Greek Extended */
    for (i = 0x1F00; i <= 0x1F07; i++)
        uc[i] = (WCHAR)(i + 8);
    for (i = 0x1F10; i <= 0x1F15; i++)
        uc[i] = (WCHAR)(i + 8);
    for (i = 0x1F20; i <= 0x1F27; i++)
        uc[i] = (WCHAR)(i + 8);
    for (i = 0x1F30; i <= 0x1F37; i++)
        uc[i] = (WCHAR)(i + 8);
    for (i = 0x1F40; i <= 0x1F45; i++)
        uc[i] = (WCHAR)(i + 8);
    for (i = 0x1F60; i <= 0x1F67; i++)
        uc[i] = (WCHAR)(i + 8);
    for (i = 0x1F70; i <= 0x1F71; i++)
        uc[i] = (WCHAR)(i + 0x4A);
    for (i = 0x1F72; i <= 0x1F75; i++)
        uc[i] = (WCHAR)(i + 0x56);
    for (i = 0x1F76; i <= 0x1F77; i++)
        uc[i] = (WCHAR)(i + 0x64);
    for (i = 0x1F78; i <= 0x1F79; i++)
        uc[i] = (WCHAR)(i + 0x80);
    for (i = 0x1F7A; i <= 0x1F7B; i++)
        uc[i] = (WCHAR)(i + 0x70);
    for (i = 0x1F7C; i <= 0x1F7D; i++)
        uc[i] = (WCHAR)(i + 0x7E);
    for (i = 0x1F80; i <= 0x1F87; i++)
        uc[i] = (WCHAR)(i + 8);
    for (i = 0x1F90; i <= 0x1F97; i++)
        uc[i] = (WCHAR)(i + 8);
    for (i = 0x1FA0; i <= 0x1FA7; i++)
        uc[i] = (WCHAR)(i + 8);
    uc[0x1FB0] = 0x1FB8;
    uc[0x1FB1] = 0x1FB9;
    uc[0x1FD0] = 0x1FD8;
    uc[0x1FD1] = 0x1FD9;
    uc[0x1FE0] = 0x1FE8;
    uc[0x1FE1] = 0x1FE9;
    uc[0x1FE5] = 0x1FEC;

    /* Fullwidth Latin (0xFF41-0xFF5A -> 0xFF21-0xFF3A) */
    for (i = 0xFF41; i <= 0xFF5A; i++)
        uc[i] = (WCHAR)(i - 0x20);

    /* Special cases from direct_mappings */
    uc[0x00FF] = 0x0178;
    uc[0x0180] = 0x0243;
    uc[0x0183] = 0x0182;
    uc[0x0185] = 0x0184;
    uc[0x0188] = 0x0187;
    uc[0x018C] = 0x018B;
    uc[0x0192] = 0x0191;
    uc[0x0195] = 0x01F6;
    uc[0x0199] = 0x0198;
    uc[0x019A] = 0x023D;
    uc[0x019E] = 0x0220;
    uc[0x01A8] = 0x01A7;
    uc[0x01AD] = 0x01AC;
    uc[0x01B0] = 0x01AF;
    uc[0x01B9] = 0x01B8;
    uc[0x01BD] = 0x01BC;
    uc[0x01BF] = 0x01F7;
    uc[0x01C6] = 0x01C4;
    uc[0x01C9] = 0x01C7;
    uc[0x01CC] = 0x01CA;
    uc[0x01DD] = 0x018E;
    uc[0x01F3] = 0x01F1;
    uc[0x01F5] = 0x01F4;
    uc[0x023C] = 0x023B;
    uc[0x0242] = 0x0241;
    uc[0x0253] = 0x0181;
    uc[0x0254] = 0x0186;
    uc[0x0256] = 0x0189;
    uc[0x0257] = 0x018A;
    uc[0x0259] = 0x018F;
    uc[0x025B] = 0x0190;
    uc[0x0260] = 0x0193;
    uc[0x0263] = 0x0194;
    uc[0x0268] = 0x0197;
    uc[0x0269] = 0x0196;
    uc[0x026F] = 0x019C;
    uc[0x0272] = 0x019D;
    uc[0x0275] = 0x019F;
    uc[0x0280] = 0x01A6;
    uc[0x0283] = 0x01A9;
    uc[0x0288] = 0x01AE;
    uc[0x028A] = 0x01B1;
    uc[0x028B] = 0x01B2;
    uc[0x0292] = 0x01B7;

    /* Greek special */
    uc[0x03AC] = 0x0386;
    uc[0x03AD] = 0x0388;
    uc[0x03AE] = 0x0389;
    uc[0x03AF] = 0x038A;
    uc[0x03CA] = 0x03AA;
    uc[0x03CB] = 0x03AB;
    uc[0x03CC] = 0x038C;
    uc[0x03CD] = 0x038E;
    uc[0x03CE] = 0x038F;
    uc[0x03D7] = 0x03CF;
    for (i = 0x03D9; i <= 0x03EF; i += 2)
        uc[i] = (WCHAR)(i - 1);
    uc[0x03F2] = 0x03F9;
    uc[0x03F8] = 0x03F7;
    uc[0x03FB] = 0x03FA;

    /* Georgian: no case mapping in Windows NTFS upcase table */
    /* (0x10D0-0x10F5 was mapped to 0x10A0-0x10C5 above, undo it) */
    for (i = 0x10D0; i <= 0x10F5; i++)
        uc[i] = (WCHAR)i;

    /* Latin Extended-B additional */
    for (i = 0x0247; i <= 0x024F; i += 2)
        uc[i] = (WCHAR)(i - 1);
    uc[0x0250] = 0x2C6F;
    uc[0x0251] = 0x2C6D;
    uc[0x026B] = 0x2C62;
    uc[0x0271] = 0x2C6E;
    uc[0x027D] = 0x2C64;
    uc[0x0289] = 0x0244;
    uc[0x028C] = 0x0245;

    /* Greek Extended additional */
    for (i = 0x0371; i <= 0x0373; i += 2)
        uc[i] = (WCHAR)(i - 1);
    uc[0x0377] = 0x0376;
    for (i = 0x037B; i <= 0x037D; i++)
        uc[i] = (WCHAR)(i + 0x82);

    /* Cyrillic additional */
    uc[0x04CF] = 0x04C0;
    uc[0x04F7] = 0x04F6;
    for (i = 0x04FB; i <= 0x0523; i += 2)
        uc[i] = (WCHAR)(i - 1);

    /* Latin Extended Additional */
    uc[0x1D79] = 0xA77D;
    uc[0x1D7D] = 0x2C63;
    for (i = 0x1EFB; i <= 0x1EFF; i += 2)
        uc[i] = (WCHAR)(i - 1);

    /* Greek Extended additional */
    uc[0x1F51] = 0x1F59;
    uc[0x1F53] = 0x1F5B;
    uc[0x1F55] = 0x1F5D;
    uc[0x1F57] = 0x1F5F;
    uc[0x1FB3] = 0x1FBC;
    uc[0x1FC3] = 0x1FCC;
    uc[0x1FF3] = 0x1FFC;

    /* Letterlike Symbols / Number Forms */
    uc[0x214E] = 0x2132;
    for (i = 0x2170; i <= 0x217F; i++)
        uc[i] = (WCHAR)(i - 0x10);
    uc[0x2184] = 0x2183;

    /* Enclosed Alphanumerics */
    for (i = 0x24D0; i <= 0x24E9; i++)
        uc[i] = (WCHAR)(i - 0x1A);

    /* Glagolitic */
    for (i = 0x2C30; i <= 0x2C5E; i++)
        uc[i] = (WCHAR)(i - 0x30);
    uc[0x2C61] = 0x2C60;
    uc[0x2C65] = 0x023A;
    uc[0x2C66] = 0x023E;
    for (i = 0x2C68; i <= 0x2C6C; i += 2)
        uc[i] = (WCHAR)(i - 1);
    uc[0x2C73] = 0x2C72;
    uc[0x2C76] = 0x2C75;

    /* Coptic */
    for (i = 0x2C81; i <= 0x2CE3; i += 2)
        uc[i] = (WCHAR)(i - 1);

    /* Georgian Supplement (Mkhedruli -> Asomtavruli) */
    for (i = 0x2D00; i <= 0x2D25; i++)
        uc[i] = (WCHAR)(i - 0x1C60);

    /* Cyrillic Extended-B */
    for (i = 0xA641; i <= 0xA65F; i += 2)
        uc[i] = (WCHAR)(i - 1);
    for (i = 0xA663; i <= 0xA66D; i += 2)
        uc[i] = (WCHAR)(i - 1);
    for (i = 0xA681; i <= 0xA697; i += 2)
        uc[i] = (WCHAR)(i - 1);

    /* Latin Extended-D */
    for (i = 0xA723; i <= 0xA72F; i += 2)
        uc[i] = (WCHAR)(i - 1);
    for (i = 0xA733; i <= 0xA76F; i += 2)
        uc[i] = (WCHAR)(i - 1);
    for (i = 0xA77A; i <= 0xA77C; i += 2)
        uc[i] = (WCHAR)(i - 1);
    for (i = 0xA77F; i <= 0xA787; i += 2)
        uc[i] = (WCHAR)(i - 1);
    uc[0xA78C] = 0xA78B;
}
