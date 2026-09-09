"""Guard the AR9300 EEPROM reference templates.

ar9003_eeprom_templates.h is 2954 lines of data extracted verbatim from ath9k.
Nobody reads it, so nothing but a test will notice if it is truncated, edited,
or re-extracted with an off-by-one -- which happened once already: the first
extraction dropped ar9003_eeprom_struct_find_by_id()'s closing brace, and the
compiler reported it 200 lines away as "invalid storage class for function".
A brace count found it in one line.

These templates are not decoration.  The compressed EEPROM image is a DIFF, and
every field the card does not carry is inherited from here.  A zeroed or
truncated template silently becomes zeroed hardware configuration -- exactly the
bug this file was added to fix (AR_PHY_SWITCH_CHAIN_0 0x2a0 -> 0).
"""

import os
import re
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
HEADER = Path(os.environ.get(
    "AR9485_TEMPLATES_PATH",
    REPO / "drivers" / "network" / "dd" / "ar9485" / "ath9k" /
    "ar9003_eeprom_templates.h"))

TEXT = HEADER.read_text(encoding="latin-1")

# templateVersion of each upstream template (ar9003_eeprom.c).
EXPECTED_TEMPLATES = {
    "ar9300_default": 2,
    "ar9300_x113": 6,
    "ar9300_h112": 3,
    "ar9300_x112": 5,
    "ar9300_h116": 4,
}


class TemplateFileIsWhole(unittest.TestCase):
    def test_braces_balance(self):
        """The check that actually caught the real defect."""
        self.assertEqual(
            TEXT.count("{"), TEXT.count("}"),
            "unbalanced braces: the file is truncated or a function lost its "
            "closing brace (the compiler will blame a line far away)")

    def test_all_five_templates_are_present(self):
        found = set(re.findall(r"^static const struct ar9300_eeprom (\w+) = \{",
                               TEXT, re.MULTILINE))
        self.assertEqual(found, set(EXPECTED_TEMPLATES),
                         "a template went missing; the diff would silently "
                         "inherit zeros for any card referencing it")

    def test_each_template_declares_its_expected_version(self):
        """find_by_id() matches on templateVersion; a wrong one is unreachable."""
        for name, version in EXPECTED_TEMPLATES.items():
            with self.subTest(template=name):
                start = TEXT.index(f"static const struct ar9300_eeprom {name} = {{")
                body = TEXT[start:start + 4000]
                match = re.search(r"\.templateVersion = (\d+)", body)
                self.assertIsNotNone(match, f"{name} has no templateVersion")
                self.assertEqual(int(match.group(1)), version)

    def test_lookup_table_lists_every_template(self):
        block = re.search(
            r"ar9300_eep_templates\[\] = \{(.*?)\};", TEXT, re.DOTALL)
        self.assertIsNotNone(block, "the lookup array is gone")
        listed = set(re.findall(r"&(\w+)", block.group(1)))
        self.assertEqual(listed, set(EXPECTED_TEMPLATES),
                         "a template exists but find_by_id() cannot reach it")

    def test_find_by_id_is_closed(self):
        # assertTrue, not assertRegex: a failing assertRegex prints the entire
        # 94 KB file into the failure message, which buries the one line that
        # matters.  A test that is unreadable when it fails has not helped.
        closed = re.search(
            r"ar9003_eeprom_struct_find_by_id\(int id\)\s*\{.*?return NULL;\s*\}",
            TEXT, re.DOTALL)
        self.assertTrue(
            closed,
            "find_by_id() is truncated -- the exact defect the first extraction "
            "shipped, which the compiler reported 200 lines away as 'invalid "
            "storage class for function'")


class DefaultTemplateHasTheValuesTheHardwareNeeds(unittest.TestCase):
    """The four fields whose absence zeroed the antenna configuration.

    Values are upstream's ar9300_default 2G modal header.  If these ever read
    back as 0 again, hw_board.c writes zeros to the PHY.
    """

    def setUp(self):
        start = TEXT.index("static const struct ar9300_eeprom ar9300_default = {")
        end = TEXT.index(".modalHeader5G", start)
        self.body = TEXT[start:end]

    def test_ant_ctrl_common_is_not_zero(self):
        self.assertIn(".antCtrlCommon = LE32(0x110)", self.body)

    def test_ant_ctrl_common_2_is_not_zero(self):
        self.assertIn(".antCtrlCommon2 = LE32(0x22222)", self.body)

    def test_ant_ctrl_chain_is_not_zero(self):
        self.assertIn("LE16(0x150), LE16(0x150), LE16(0x150)", self.body)


class MacPlaceholderGuardStillExists(unittest.TestCase):
    """Seeding the template is only safe because this check rejects it.

    ar9300_default.macAddr is {0, 2, 3, 4, 5, 6}.  Before the seed, a card whose
    diff omitted the MAC produced all zeroes; now it produces that placeholder --
    a plausible-looking wrong address.  ar9485_is_valid_mac() rejects it.  If
    someone ever deletes that check as redundant, this fails.
    """

    def test_hw_eeprom_still_rejects_the_placeholder(self):
        source = (REPO / "drivers" / "network" / "dd" / "ar9485" / "ath9k" /
                  "hw_eeprom.c").read_text(encoding="latin-1")
        self.assertIn("ar9300_default_macaddr[6] = { 0, 2, 3, 4, 5, 6 }", source)
        self.assertIn("RtlCompareMemory(addr, ar9300_default_macaddr, 6) == 6",
                      source)

    def test_the_template_seed_is_actually_wired_up(self):
        source = (REPO / "drivers" / "network" / "dd" / "ar9485" / "ath9k" /
                  "hw_eeprom.c").read_text(encoding="latin-1")
        self.assertIn("RtlCopyMemory(mptr, &ar9300_default, mdata_size)", source,
                      "the seed is gone; every uncarried field returns to 0")
        self.assertIn("ar9003_eeprom_struct_find_by_id(reference)", source,
                      "the block reference is ignored again")


if __name__ == "__main__":
    unittest.main()
