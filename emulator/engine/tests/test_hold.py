"""INIT hold preference is accepted by both reactive server emulators."""
import unittest

from emulator.engine.emulator import VSCPEmulator as BasicEmulator
from emulator.engine.emulator_patterns import VSCPEmulator as PatternEmulator


class HoldTest(unittest.TestCase):
    def test_init_hold_defaults_and_validation(self):
        for emulator_type in (BasicEmulator, PatternEmulator):
            with self.subTest(emulator=emulator_type.__name__):
                emulator = emulator_type()
                self.assertIn("status=1", emulator.process_request("?type=INIT&api=1.7&hold=0"))
                self.assertFalse(emulator.hold_enabled)
                self.assertIn("status=1", emulator.process_request("?type=PING&side=client&seq=7"))
                self.assertIn("status=1", emulator.process_request("?type=INIT&api=1.7"))
                self.assertTrue(emulator.hold_enabled)
                self.assertIn("status=0", emulator.process_request("?type=INIT&api=1.7&hold=bad"))


if __name__ == "__main__":
    unittest.main()
