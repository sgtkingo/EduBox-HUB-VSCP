"""BYE closes the protocol session and preserves connected devices."""
import unittest
from emulator.engine.emulator import VSCPEmulator as BasicEmulator
from emulator.engine.emulator_patterns import VSCPEmulator as PatternEmulator
from emulator.engine.vscp_session import API_VERSION, LIBRARY_VERSION


class ByeTest(unittest.TestCase):
    def test_server_bye_keeps_transport_and_pins(self):
        class Serial:
            is_open = True
            def __init__(self): self.sent = []
            def write(self, data): self.sent.append(data)
            def flush(self): pass
            def close(self): self.is_open = False
        for emulator_type in (BasicEmulator, PatternEmulator):
            emulator = emulator_type()
            emulator.ser = Serial()
            emulator.initialized = True
            emulator.connected_sensors["S01"] = [7]
            self.assertTrue(emulator.bye())
            self.assertEqual(emulator.ser.sent, [b"?type=BYE&side=server\n"])
            self.assertFalse(emulator.initialized)
            self.assertTrue(emulator.ser.is_open)
            self.assertEqual(emulator.connected_sensors, {"S01": [7]})

    def test_close_reinitialize_and_keep_pins(self):
        self.assertEqual((API_VERSION, LIBRARY_VERSION), ("1.6", "2.2.2"))
        for emulator_type in (BasicEmulator, PatternEmulator):
            with self.subTest(emulator=emulator_type.__name__):
                emulator = emulator_type()
                self.assertEqual(emulator.process_request("?type=BYE&side=client"), "")
                self.assertFalse(emulator.initialized)
                self.assertIn("status=1", emulator.process_request("?type=INIT&api=1.6"))
                self.assertIn("status=1", emulator.process_request("?type=CONNECT&id=S01&pins=7"))
                pins = dict(emulator.connected_sensors)
                self.assertTrue(pins)
                for invalid in ("?type=BYE&side=server", "?type=BYE", "?type=BYE&side=client&status=1"):
                    self.assertEqual(emulator.process_request(invalid), "")
                    self.assertTrue(emulator.initialized)
                self.assertEqual(emulator.process_request("?side=client&type=bye"), "")
                self.assertEqual(emulator.process_request("?type=BYE&side=client"), "")
                self.assertFalse(emulator.initialized)
                self.assertEqual(emulator.connected_sensors, pins)
                self.assertIn("Protocol not initialized", emulator.process_request("?type=UPDATE&id=S01"))
                self.assertEqual(emulator.parse_message(emulator.process_request("?type=PING&side=client&seq=22")),
                                 {"side": "server", "seq": "22", "status": "1"})
                self.assertIn("status=1", emulator.process_request("?type=INIT&api=1.6"))
                self.assertEqual(emulator.connected_sensors, pins)


if __name__ == "__main__":
    unittest.main()
