import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.dirname(__file__))

import devices  # noqa: E402
import flasher  # noqa: E402
from fake_firmware import FakeFirmware  # noqa: E402
from flasher import FlashError  # noqa: E402
from image import Segment  # noqa: E402
from probe import SbwProbe  # noqa: E402

F5529 = devices.MSP430F5529


def session(fw=None):
    fw = fw or FakeFirmware()
    probe = SbwProbe(fw)
    flasher.open_session(probe, 250)
    return fw, probe


class OpenSessionTest(unittest.TestCase):
    def test_connects_first_try(self):
        fw = FakeFirmware()
        identity, device = flasher.open_session(SbwProbe(fw), 250)
        self.assertEqual(device, F5529)
        self.assertEqual(identity.jtag_id, 0x91)
        self.assertEqual(fw.ops[0], ("dap_disconnect",))  # SWD is switched off first
        self.assertEqual(fw.connects, [0])

    def test_retries_the_normal_entry(self):
        fw = FakeFirmware()
        fw.no_target = {0: 3}
        flasher.open_session(SbwProbe(fw), 250)
        self.assertEqual(fw.connects, [0, 0, 0, 0])

    def test_falls_back_to_the_magic_pattern(self):
        fw = FakeFirmware()
        fw.no_target = {0: 4}
        flasher.open_session(SbwProbe(fw), 250)
        self.assertEqual(fw.connects, [0, 0, 0, 0, 1])

    def test_no_target_at_all(self):
        fw = FakeFirmware()
        fw.no_target = {0: 4, 1: 1}
        with self.assertRaisesRegex(FlashError, "0xFF.*nothing is connected"):
            flasher.open_session(SbwProbe(fw), 250)
        self.assertEqual(len(fw.connects), 5)

    def test_wrong_id_suggests_wiring_and_speed(self):
        fw = FakeFirmware()
        fw.no_target = {0: 4, 1: 1}
        fw.no_target_id = 0x89
        with self.assertRaisesRegex(FlashError, "0x89.*--speed"):
            flasher.open_session(SbwProbe(fw), 250)

    def test_unknown_device(self):
        fw = FakeFirmware(device_id=0x1234)
        with self.assertRaisesRegex(FlashError, "34h 12h"):
            flasher.open_session(SbwProbe(fw), 250)


class ValidateTest(unittest.TestCase):
    def test_accepts_main_and_info_b_to_d(self):
        flasher.validate(F5529, [Segment(0x4400, b"\0" * 4), Segment(0x243FE, b"\0\0"),
                                 Segment(0x1800, b"\0" * 0x180)])

    def test_rejects_everything_else(self):
        for addr in (0x1000, 0x1980, 0x2400, 0x43FE, 0x24400):
            with self.assertRaisesRegex(FlashError, f"0x{addr:05X}"):
                flasher.validate(F5529, [Segment(addr, b"\0\0")])

    def test_rejects_a_segment_running_into_info_a(self):
        with self.assertRaisesRegex(FlashError, "0x01980"):
            flasher.validate(F5529, [Segment(0x1900, b"\0" * 0x82)])


class PlanEraseTest(unittest.TestCase):
    SEGS = [Segment(0x4400, b"\0" * 600), Segment(0x1880, b"\0\0")]

    def test_main(self):
        self.assertEqual(flasher.plan_erase(F5529, self.SEGS, "main"),
                         [(2, 0x4400), (0, 0x1880)])

    def test_segments(self):
        self.assertEqual(flasher.plan_erase(F5529, self.SEGS, "segments"),
                         [(0, 0x4400), (0, 0x4600), (0, 0x1880)])

    def test_none(self):
        self.assertEqual(flasher.plan_erase(F5529, self.SEGS, "none"), [])

    def test_info_only_image_does_not_erase_main(self):
        self.assertEqual(flasher.plan_erase(F5529, [Segment(0x1800, b"\0" * 0x100)], "main"),
                         [(0, 0x1800), (0, 0x1880)])


class ProgramTest(unittest.TestCase):
    def test_erase_write_verify(self):
        fw, probe = session()
        fw.mem = {0x4400 + i: 0x00 for i in range(64)}  # old firmware
        data = bytes(range(200)) * 3
        flasher.program(probe, F5529, [Segment(0x4400, data), Segment(0x1800, b"\x12\x34")])
        self.assertEqual(bytes(fw.mem[0x4400 + i] for i in range(len(data))), data)
        self.assertEqual((fw.mem[0x1800], fw.mem[0x1801]), (0x12, 0x34))
        names = [op[0] for op in fw.ops if op[0] != "dap_disconnect"]
        self.assertEqual(names, ["erase", "erase", "begin", "end", "begin", "end"])
        self.assertIn(("erase", 2, 0x4400, 0x2400), fw.ops)
        self.assertIn(("erase", 0, 0x1800, 0x2400), fw.ops)
        self.assertIn(("begin", 0x1800, 1, 0x2400), fw.ops)
        self.assertIn(("begin", 0x4400, 300, 0x2400), fw.ops)

    def test_odd_image_is_padded_with_ff(self):
        fw, probe = session()
        flasher.program(probe, F5529, [Segment(0x4401, b"\xAA")])
        self.assertEqual((fw.mem[0x4400], fw.mem[0x4401]), (0xFF, 0xAA))

    def test_invalid_image_touches_nothing(self):
        fw, probe = session()
        with self.assertRaises(FlashError):
            flasher.program(probe, F5529, [Segment(0x1980, b"\0\0")])
        self.assertEqual([op for op in fw.ops if op[0] in ("erase", "begin")], [])

    def test_verify_reports_the_first_bad_address(self):
        fw, probe = session()
        fw.stuck = {0x4405: 0x00}
        with self.assertRaisesRegex(FlashError, "verify failed at 0x04405: read 0x00, expected 0x05"):
            flasher.program(probe, F5529, [Segment(0x4400, bytes(range(16)))])

    def test_no_erase_over_old_data_fails_verify(self):
        fw, probe = session()
        fw.mem = {0x4400: 0x00}
        with self.assertRaisesRegex(FlashError, "verify failed at 0x04400"):
            flasher.program(probe, F5529, [Segment(0x4400, b"\xFF\xFF")], erase="none")

    def test_write_failure_reports_the_address(self):
        fw, probe = session()
        fw.fail_flash_after = 150  # words
        with self.assertRaisesRegex(FlashError, "programming failed at 0x044BA.*TIMEOUT"):
            flasher.program(probe, F5529, [Segment(0x4400, b"\x55" * 800)])

    def test_progress_callback(self):
        fw, probe = session()
        seen = []
        flasher.program(probe, F5529, [Segment(0x4400, b"\x55" * 400)],
                        progress=lambda done, total: seen.append((done, total)))
        self.assertEqual(seen[-1], (400, 400))
        self.assertTrue(all(total == 400 for _, total in seen))


if __name__ == "__main__":
    unittest.main()
