import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.dirname(__file__))

import devices  # noqa: E402
import flasher  # noqa: E402
from fake_firmware import BUSY, LOCKED, TIMEOUT, FakeFirmware  # noqa: E402
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
        # open_session leaves the session open; closing it is the caller's job
        self.assertEqual(fw.state, 1)


class OpenSessionRefusalTest(unittest.TestCase):
    def test_locked_device(self):
        fw = FakeFirmware()
        fw.connect_status = LOCKED
        with self.assertRaisesRegex(FlashError, "lock key"):
            flasher.open_session(SbwProbe(fw), 250)
        self.assertEqual(len(fw.connects), 1)

    def test_locked_message(self):
        fw = FakeFirmware()
        fw.connect_status = LOCKED
        with self.assertRaisesRegex(FlashError, "it cannot be read or programmed"):
            flasher.open_session(SbwProbe(fw), 250)

    def test_target_error_is_not_reported_as_locked(self):
        fw = FakeFirmware()
        fw.connect_status = 6
        with self.assertRaises(FlashError) as cm:
            flasher.open_session(SbwProbe(fw), 250)
        self.assertIn("TARGET_ERROR", str(cm.exception))
        self.assertNotIn("lock key", str(cm.exception))
        self.assertEqual(len(fw.connects), 1)

    def test_busy_swd_port(self):
        fw = FakeFirmware()
        fw.connect_status = BUSY
        with self.assertRaisesRegex(FlashError, "SWD"):
            flasher.open_session(SbwProbe(fw), 250)
        self.assertEqual(len(fw.connects), 1)

    def test_protocol_version_2(self):
        fw = FakeFirmware(version=2)
        with self.assertRaisesRegex(FlashError, "version 2"):
            flasher.open_session(SbwProbe(fw), 250)
        self.assertEqual(fw.connects, [])

    def test_firmware_without_cpuxv2_flash(self):
        fw = FakeFirmware(features=0)
        with self.assertRaisesRegex(FlashError, "does not support MSP430 CPUXv2"):
            flasher.open_session(SbwProbe(fw), 250)
        self.assertEqual(fw.connects, [])


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

    def test_erase_failure_stops_before_writing(self):
        fw, probe = session()
        fw.fail_op = {5: TIMEOUT}
        with self.assertRaisesRegex(FlashError, "erase at 0x04400 failed"):
            flasher.program(probe, F5529, [Segment(0x4400, b"\x55" * 8)])
        self.assertEqual([op for op in fw.ops if op[0] == "begin"], [])

    def test_flash_begin_failure(self):
        fw, probe = session()
        fw.fail_op = {6: TIMEOUT}
        with self.assertRaisesRegex(FlashError, "programming failed at 0x"):
            flasher.program(probe, F5529, [Segment(0x4400, b"\x55" * 8)])

    def test_flash_end_failure(self):
        fw, probe = session()
        fw.fail_op = {8: TIMEOUT}
        with self.assertRaisesRegex(FlashError, "programming failed at 0x"):
            flasher.program(probe, F5529, [Segment(0x4400, b"\x55" * 8)])

    def test_tail_chunk_that_would_end_a_packet_is_split(self):
        fw, probe = session()
        data = bytes(range(248))  # 124 words
        flasher.program(probe, F5529, [Segment(0x4400, data)])
        self.assertEqual(fw.data_chunks, [93, 30, 1])
        self.assertEqual(bytes(fw.mem[0x4400 + i] for i in range(248)), data)

    def test_verify_ignores_padding(self):
        fw, probe = session()
        fw.mem = {0x4400: 0x12}
        written = flasher.program(probe, F5529, [Segment(0x4401, b"\xAA")], erase="none")
        self.assertEqual(written, 2)
        self.assertEqual((fw.mem[0x4400], fw.mem[0x4401]), (0x12, 0xAA))

    def test_verify_segments_accepts_unaligned_segments(self):
        fw, probe = session()
        fw.mem = {0x4401: 0xAA, 0x4402: 0xBB, 0x4403: 0xCC}
        flasher.verify_segments(probe, [Segment(0x4401, b"\xAA\xBB\xCC")])
        with self.assertRaisesRegex(FlashError, "verify failed at 0x04402: read 0xBB, expected 0xBC"):
            flasher.verify_segments(probe, [Segment(0x4401, b"\xAA\xBC\xCC")])

    def test_verify_segment_ending_on_an_odd_address(self):
        fw, probe = session()
        fw.mem = {0x4400: 1, 0x4401: 2, 0x4402: 3}
        flasher.verify_segments(probe, [Segment(0x4400, bytes([1, 2, 3]))])
        fw.mem[0x4402] = 4
        with self.assertRaisesRegex(FlashError, "verify failed at 0x04402"):
            flasher.verify_segments(probe, [Segment(0x4400, bytes([1, 2, 3]))])

    def test_empty_image(self):
        fw, probe = session()
        with self.assertRaisesRegex(FlashError, "contains no data"):
            flasher.program(probe, F5529, [])
        self.assertEqual([op for op in fw.ops if op[0] in ("erase", "begin")], [])

    def test_progress_callback(self):
        fw, probe = session()
        seen = []
        flasher.program(probe, F5529, [Segment(0x4400, b"\x55" * 400)],
                        progress=lambda done, total: seen.append((done, total)))
        self.assertEqual(seen[-1], (400, 400))
        self.assertTrue(all(total == 400 for _, total in seen))


class SelftestTest(unittest.TestCase):
    def test_passes_on_a_healthy_target(self):
        fw = FakeFirmware()
        probe = SbwProbe(fw)
        identity, device = flasher.open_session(probe, 250)
        lines = []
        class Out:
            def write(self, s):
                lines.append(s)
        flasher.selftest(probe, identity, device, out=Out())
        self.assertTrue(lines)
        flasher.selftest(probe, identity, device)  # out is optional

    def test_stuck_ram_byte(self):
        fw = FakeFirmware()
        probe = SbwProbe(fw)
        identity, device = flasher.open_session(probe, 250)
        fw.stuck = {0x2410: 0x00}
        with self.assertRaisesRegex(FlashError, "RAM read-back differs at 0x02410"):
            flasher.selftest(probe, identity, device)

    def test_shifted_reads_are_caught(self):
        fw = FakeFirmware()
        probe = SbwProbe(fw)
        identity, device = flasher.open_session(probe, 250)
        fw.read_shift = 2
        with self.assertRaisesRegex(FlashError, "RAM read-back differs"):
            flasher.selftest(probe, identity, device)

    def test_high_id_byte_mismatch(self):
        fw = FakeFirmware()
        probe = SbwProbe(fw)
        identity, device = flasher.open_session(probe, 250)
        fw.mem[0x1A05] = 0x28
        with self.assertRaisesRegex(FlashError, "device ID read inside a block differs"):
            flasher.selftest(probe, identity, device)

    def test_descriptor_mismatch(self):
        fw = FakeFirmware()
        probe = SbwProbe(fw)
        identity, device = flasher.open_session(probe, 250)
        fw.mem[0x1A04] = 0x56
        with self.assertRaisesRegex(FlashError, "device ID read inside a block differs"):
            flasher.selftest(probe, identity, device)


class TrialTest(unittest.TestCase):
    def setUp(self):
        self.fw, self.probe = session()
        for a in range(0x4400, 0x20000):
            self.fw.mem[a] = (a * 7) & 0xFF
        self.backup = [Segment(0x4400, self.probe.read_mem(0x4400, 0x10000))]

    def backup_full(self):
        return [Segment(0x4400, self.probe.read_mem(0x4400, (0x24400 - 0x4400) // 2))]

    def erased(self):
        return [op for op in self.fw.ops if op[0] in ("erase", "begin")]

    def test_healthy(self):
        before = dict(self.fw.mem)
        lines = []
        class Out:
            def write(self, s):
                lines.append(s)
        base = flasher.trial(self.probe, F5529, self.backup, out=Out())
        self.assertEqual(base, 0x24200)
        self.assertEqual([op for op in self.fw.ops if op[0] == "erase"],
                         [("erase", 0, 0x24200, 0x2400)] * 2)
        self.assertEqual(self.fw.mem, before)
        self.assertTrue(lines)

    def test_backup_must_cover_main_flash(self):
        backup = [Segment(0x4400, self.backup[0].data[:0x1000])]
        with self.assertRaisesRegex(FlashError, "does not cover all of main flash.*"
                                    "read 0x4400 0x20000 -o FILE"):
            flasher.trial(self.probe, F5529, backup)
        self.assertEqual(self.erased(), [])

    def test_needs_a_blank_segment(self):
        for a in range(0x4400, 0x24400):
            self.fw.mem[a] = 0x55
        backup = [Segment(0x4400, self.probe.read_mem(0x4400, 0x10000))]
        with self.assertRaisesRegex(FlashError, "no blank segment in main flash"):
            flasher.trial(self.probe, F5529, backup)
        self.assertEqual(self.erased(), [])

    def test_target_must_match_the_backup(self):
        self.fw.mem[0x5000] ^= 0xFF
        with self.assertRaisesRegex(FlashError, "does not match the backup .*0x05000.*"
                                    "nothing was changed"):
            flasher.trial(self.probe, F5529, self.backup)
        self.assertEqual(self.erased(), [])

    ADVICE = ('segment 0x24200 may hold part of the test pattern: run '
              '"erase --segment 0x24200", then "verify" with your backup')

    def test_a_programmed_byte_in_the_top_segment_moves_the_trial_down(self):
        self.fw.mem[0x24300] = 0x12
        self.assertEqual(flasher.trial(self.probe, F5529, self.backup_full()), 0x24000)

    def test_pattern_verify_failure_gives_cleanup_advice(self):
        self.fw.stuck = {0x24210: 0xFF}
        with self.assertRaises(FlashError) as cm:
            flasher.trial(self.probe, F5529, self.backup_full())
        self.assertIn("verify failed at 0x24210", str(cm.exception))
        self.assertIn(self.ADVICE, str(cm.exception))

    def test_second_erase_does_nothing(self):
        self.fw.skip_segment_erases = {2}
        with self.assertRaises(FlashError) as cm:
            flasher.trial(self.probe, F5529, self.backup_full())
        self.assertIn("the test segment is not blank", str(cm.exception))
        self.assertIn(self.ADVICE, str(cm.exception))

    def test_failed_erase_gives_cleanup_advice(self):
        self.fw.fail_op = {5: 7}
        with self.assertRaises(FlashError) as cm:
            flasher.trial(self.probe, F5529, self.backup_full())
        self.assertIn(self.ADVICE, str(cm.exception))

    def test_leftover_pattern_is_explained(self):
        backup = self.backup_full()
        for i in range(4):
            self.fw.mem[0x24200 + i] = 0x11 + i
        with self.assertRaises(FlashError) as cm:
            flasher.trial(self.probe, F5529, backup)
        msg = str(cm.exception)
        self.assertIn("nothing was changed", msg)
        self.assertIn("if an earlier trial was interrupted, " + self.ADVICE, msg)
        self.assertEqual(self.erased(), [])

    def test_difference_outside_the_segment_has_no_advice(self):
        backup = self.backup_full()
        self.fw.mem[0x5000] ^= 0xFF
        with self.assertRaises(FlashError) as cm:
            flasher.trial(self.probe, F5529, backup)
        self.assertIn("nothing was changed", str(cm.exception))
        self.assertNotIn("erase --segment", str(cm.exception))

    def test_damage_during_the_trial(self):
        self.fw.also_erase = 0x4400
        with self.assertRaises(FlashError) as cm:
            flasher.trial(self.probe, F5529, self.backup)
        msg = str(cm.exception)
        self.assertIn("outside the test segment", msg)
        self.assertIn("0x04400", msg)
        self.assertIn("do not write anything", msg)
        self.assertNotIn("flash command", msg)


if __name__ == "__main__":
    unittest.main()
