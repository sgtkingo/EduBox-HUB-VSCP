"""Both API 1.6 emulators mirror the shared server PING framing rules."""
import unittest

from emulator.engine.emulator import VSCPEmulator as BasicEmulator
from emulator.engine.emulator_patterns import VSCPEmulator as PatternEmulator


class PingTest(unittest.TestCase):
    def test_ping_before_init_and_after_init_does_not_change_session(self):
        for emulator_type in (BasicEmulator, PatternEmulator):
            emulator = emulator_type()
            with self.subTest(emulator=emulator_type):
                self.assertEqual(emulator.API_VERSION, "1.6")
                for sequence in ("1", "4294967295"):
                    response = emulator.parse_message(emulator.process_request(
                        f"?seq={sequence}&side=client&type=PING"))
                    self.assertEqual(response, {
                        "side": "server", "seq": sequence, "status": "1"})
                self.assertFalse(emulator.initialized)
                self.assertEqual(emulator.connected_sensors, {})
                emulator.process_request("?type=INIT&api=1.6&app=board&db=1.3")
                self.assertTrue(emulator.initialized)
                self.assertIn("status=1", emulator.process_request("?type=PING&side=client&seq=2"))
                self.assertTrue(emulator.initialized)

    def test_invalid_and_response_frames_are_consumed_without_reply(self):
        for emulator_type in (BasicEmulator, PatternEmulator):
            emulator = emulator_type()
            for frame in (
                "?side=client&seq=1&status=1",
                "?side=server&seq=999&status=1",
                "?type=PING&side=server&seq=1",
                "?type=PING&side=client&seq=1&status=1",
                "?type=PING&side=client&seq=1&status=0",
                "?type=PING&seq=1", "?type=PING&side=client",
                *(f"?type=PING&side=client&seq={seq}" for seq in
                  ("0", "01", "-1", "x", "4294967296", "12345678901", "%EF%BC%91")),
            ):
                with self.subTest(emulator=emulator_type, frame=frame):
                    self.assertEqual(emulator.process_request(frame), "")


if __name__ == "__main__":
    unittest.main()
