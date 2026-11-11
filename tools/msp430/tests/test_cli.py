import contextlib
import hashlib
import io
import os
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.dirname(__file__))

import image  # noqa: E402
import sbw  # noqa: E402
from fake_firmware import TARGET_ERROR, FakeFirmware  # noqa: E402
from image import Segment  # noqa: E402


def run(argv, fw):
    out, err = io.StringIO(), io.StringIO()
    code = sbw.main(argv, transport=fw, out=out, err=err)
    return code, out.getvalue(), err.getvalue()


class CliTest(unittest.TestCase):
    def test_info(self):
        fw = FakeFirmware()
        code, out, _ = run(["info"], fw)
        self.assertEqual(code, 0)
        self.assertIn("MSP430F5529: JTAG ID 0x91", out)
        self.assertIn("55h 29h", out)
        self.assertEqual(fw.state, 0)   # session closed
        self.assertTrue(fw.closed)

    def test_flash_then_read_back(self):
        fw = FakeFirmware()
        with tempfile.TemporaryDirectory() as d:
            src = os.path.join(d, "fw.hex")
            dump = os.path.join(d, "dump.hex")
            data = bytes(range(256)) * 2
            image.save(src, [Segment(0x4400, data)])
            code, out, _ = run(["flash", src], fw)
            self.assertEqual(code, 0)
            self.assertIn("programmed 512 bytes, verified", out)
            code, _, _ = run(["read", "0x4400", "512", "-o", dump], fw)
            self.assertEqual(code, 0)
            self.assertEqual(image.load(dump), [Segment(0x4400, data)])
            code, out, _ = run(["verify", src], fw)
            self.assertEqual((code, out.splitlines()[-1]), (0, "verified"))

    def test_erase_main_and_segment(self):
        fw = FakeFirmware()
        fw.mem = {0x4400: 0, 0x1800: 0}
        self.assertEqual(run(["erase"], fw)[0], 0)
        self.assertEqual(fw.mem, {0x1800: 0})
        self.assertEqual(run(["erase", "--segment", "0x1810"], fw)[0], 0)
        self.assertEqual(fw.mem, {})
        code, _, err = run(["erase", "--segment", "0x1980"], fw)
        self.assertEqual(code, 1)
        self.assertIn("not in programmable flash", err)

    def test_errors_exit_1_and_close_the_session(self):
        fw = FakeFirmware()
        fw.no_target = {0: 4, 1: 1}
        code, _, err = run(["info"], fw)
        self.assertEqual(code, 1)
        self.assertIn("error: no MSP430 found", err)
        self.assertTrue(fw.closed)

        fw = FakeFirmware()
        code, _, err = run(["flash", "/nonexistent/fw.hex"], fw)
        self.assertEqual(code, 1)
        self.assertEqual(fw.state, 0)   # disconnected despite the error

    def test_speed_is_validated_by_argparse(self):
        for speed in ("50", "1001", "fast"):
            fw = FakeFirmware()
            argparse_err = io.StringIO()
            with contextlib.redirect_stderr(argparse_err):
                with self.assertRaises(SystemExit) as cm:
                    run(["--speed", speed, "info"], fw)
            self.assertEqual(cm.exception.code, 2)
            self.assertIn("--speed", argparse_err.getvalue())
            self.assertEqual(fw.connects, [])

    def test_unknown_device_closes_the_session(self):
        fw = FakeFirmware(device_id=0x1234)
        code, _, err = run(["info"], fw)
        self.assertEqual(code, 1)
        self.assertIn("unsupported device", err)
        self.assertEqual(fw.state, 0)
        self.assertIn(("disconnect",), fw.ops)

    def test_nothing_is_sent_when_the_arguments_are_bad(self):
        with tempfile.TemporaryDirectory() as d:
            empty = os.path.join(d, "empty.hex")
            with open(empty, "w") as f:
                f.write(":00000001FF\n")
            out_path = os.path.join(d, "dump.hex")
            for argv in (["flash", "/nonexistent/fw.hex"], ["verify", "/nonexistent/fw.hex"],
                         ["flash", empty], ["verify", empty],
                         ["read", "0x4401", "16", "-o", out_path],
                         ["read", "0x4400", "0", "-o", out_path],
                         ["read", "0xFFFFE", "4", "-o", out_path]):
                fw = FakeFirmware()
                code, _, err = run(argv, fw)
                self.assertEqual(code, 1, argv)
                self.assertIn("error:", err)
                self.assertEqual(fw.connects, [], argv)
                self.assertEqual(fw.ops, [], argv)

    def test_target_error_while_flashing(self):
        fw = FakeFirmware()
        fw.fail_op = {7: TARGET_ERROR}
        with tempfile.TemporaryDirectory() as d:
            src = os.path.join(d, "fw.hex")
            image.save(src, [Segment(0x4400, b"\x55" * 64)])
            code, _, err = run(["flash", src], fw)
        self.assertEqual(code, 1)
        self.assertIn("error: programming failed at 0x", err)
        self.assertNotIn("Traceback", err)
        self.assertEqual(fw.state, 0)

    def test_failed_flash_leaves_the_progress_line_terminated(self):
        fw = FakeFirmware()
        fw.fail_flash_after = 100  # words; the 93-word chunk goes through, the next fails
        with tempfile.TemporaryDirectory() as d:
            src = os.path.join(d, "fw.hex")
            image.save(src, [Segment(0x4400, b"\x55" * 400)])
            code, out, err = run(["flash", src], fw)
        self.assertEqual(code, 1)
        self.assertIn("\rprogramming 186/400 bytes", out)
        self.assertTrue(out.endswith("\n"))
        self.assertTrue(err.startswith("error:"))

    def test_ctrl_c_during_flash(self):
        fw = FakeFirmware()
        fw.interrupt_on_op = 7
        with tempfile.TemporaryDirectory() as d:
            src = os.path.join(d, "fw.hex")
            image.save(src, [Segment(0x4400, b"\x55" * 400)])
            code, out, err = run(["flash", src], fw)
        self.assertEqual(code, 130)
        self.assertIn("interrupted", err)
        self.assertIn("partially erased or programmed", err)
        self.assertEqual(fw.state, 0)
        self.assertTrue(fw.closed)

    def test_ctrl_c_during_read_is_just_interrupted(self):
        fw = FakeFirmware()
        fw.interrupt_on_op = 3
        with tempfile.TemporaryDirectory() as d:
            code, _, err = run(["read", "0x4400", "16", "-o", os.path.join(d, "x.hex")], fw)
        self.assertEqual(code, 130)
        self.assertEqual(err.strip(), "interrupted")
        self.assertEqual(fw.state, 0)

    def _image(self, d, segs=None, name="fw.hex"):
        path = os.path.join(d, name)
        image.save(path, segs or [Segment(0x4400, bytes(range(16)))])
        return path

    def test_flash_reports_a_verify_failure(self):
        fw = FakeFirmware()
        fw.stuck = {0x4405: 0x00}
        with tempfile.TemporaryDirectory() as d:
            code, _, err = run(["flash", self._image(d)], fw)
        self.assertEqual(code, 1)
        self.assertIn("verify failed at 0x04405", err)

    def test_no_verify_skips_the_read_back(self):
        fw = FakeFirmware()
        fw.stuck = {0x4405: 0x00}
        with tempfile.TemporaryDirectory() as d:
            code, out, _ = run(["flash", self._image(d), "--no-verify"], fw)
        self.assertEqual(code, 0)
        self.assertNotIn("verified", out)
        self.assertEqual(fw.read_chunks, [])

    def test_verify_command_reports_a_mismatch(self):
        fw = FakeFirmware()
        with tempfile.TemporaryDirectory() as d:
            src = self._image(d)
            self.assertEqual(run(["flash", src], fw)[0], 0)
            fw.stuck = {0x4405: 0x00}
            code, _, err = run(["verify", src], fw)
        self.assertEqual(code, 1)
        self.assertIn("verify failed", err)

    def test_speed_limits(self):
        for speed in (100, 500, 1000):
            fw = FakeFirmware()
            self.assertEqual(run(["--speed", str(speed), "info"], fw)[0], 0)
            self.assertEqual(fw.speeds, [speed])
        for speed in ("99", "1001"):
            with contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as cm:
                    run(["--speed", speed, "info"], FakeFirmware())
            self.assertEqual(cm.exception.code, 2)

    def test_negative_read_address(self):
        fw = FakeFirmware()
        with tempfile.TemporaryDirectory() as d:
            code, _, err = run(["read", "-2", "4", "-o", os.path.join(d, "x.hex")], fw)
        self.assertEqual(code, 1)
        self.assertIn("error:", err)
        self.assertEqual(fw.connects, [])

    def test_image_above_the_address_space(self):
        with tempfile.TemporaryDirectory() as d:
            for segs in ([Segment(0x100000, b"\0\0")],
                         [Segment(0x4400, b"\1\2"), Segment(0xFFFFE, b"\0\0\0\0")]):
                src = self._image(d, segs)
                for cmd in ("flash", "verify"):
                    fw = FakeFirmware()
                    code, _, err = run([cmd, src], fw)
                    self.assertEqual(code, 1, cmd)
                    self.assertIn("0x100000", err)
                    self.assertEqual(fw.connects, [])
                    self.assertEqual(fw.ops, [])

    def test_ctrl_c_before_the_session_is_just_interrupted(self):
        with tempfile.TemporaryDirectory() as d:
            src = self._image(d)
            fw = FakeFirmware()
            fw.interrupt_on_op = 0  # INFO, while opening the session
            code, _, err = run(["flash", src], fw)
            self.assertEqual((code, err.strip()), (130, "interrupted"))
            fw = FakeFirmware()
            fw.interrupt_on_op = 1  # CONNECT
            code, _, err = run(["erase"], fw)
            self.assertEqual((code, err.strip()), (130, "interrupted"))
            with mock.patch.object(image, "load", side_effect=KeyboardInterrupt):
                code, _, err = run(["flash", src], FakeFirmware())
            self.assertEqual((code, err.strip()), (130, "interrupted"))

    def test_ctrl_c_during_erase_and_flash_data_warns(self):
        fw = FakeFirmware()
        fw.interrupt_on_op = 5
        code, _, err = run(["erase"], fw)
        self.assertEqual(code, 130)
        self.assertIn("partially erased or programmed", err)
        self.assertEqual(fw.state, 0)

    def test_verify_ignores_padding(self):
        fw = FakeFirmware()
        fw.mem = {0x4400: 0x12}
        with tempfile.TemporaryDirectory() as d:
            src = os.path.join(d, "odd.hex")
            image.save(src, [Segment(0x4401, b"\xAA")])
            code, out, _ = run(["flash", "--erase", "none", src], fw)
            self.assertEqual(code, 0)
            self.assertIn("programmed 2 bytes, verified", out)
            self.assertEqual(fw.mem[0x4400], 0x12)
            code, out, _ = run(["verify", src], fw)
            self.assertEqual((code, out.splitlines()[-1]), (0, "verified"))

    def test_reset(self):
        fw = FakeFirmware()
        self.assertEqual(run(["reset"], fw)[0], 0)
        self.assertIn(("disconnect",), fw.ops)

    def test_read_refuses_to_overwrite_without_force(self):
        fw = FakeFirmware()
        with tempfile.TemporaryDirectory() as d:
            dump = os.path.join(d, "backup.bin")
            with open(dump, "wb") as f:
                f.write(b"precious")
            code, _, err = run(["read", "0x4400", "16", "-o", dump], fw)
            self.assertEqual(code, 1)
            self.assertIn(dump, err)
            self.assertIn("--force", err)
            with open(dump, "rb") as f:
                self.assertEqual(f.read(), b"precious")
            self.assertEqual(fw.connects, [])
            code, _, _ = run(["read", "--force", "0x4400", "16", "-o", dump], fw)
            self.assertEqual(code, 0)
            with open(dump, "rb") as f:
                self.assertEqual(f.read(), b"\xFF" * 16)
            self.assertEqual(os.listdir(d), ["backup.bin"])

    def test_read_prints_the_sha256_of_the_data(self):
        fw = FakeFirmware()
        data = bytes(range(100))
        for i, b in enumerate(data):
            fw.mem[0x4400 + i] = b
        with tempfile.TemporaryDirectory() as d:
            code, out, _ = run(["read", "0x4400", "100", "-o", os.path.join(d, "x.bin")], fw)
        self.assertEqual(code, 0)
        lines = out.splitlines()
        self.assertTrue(lines[-2].startswith("read 100 bytes"))
        self.assertEqual(lines[-1], "sha256: " + hashlib.sha256(data).hexdigest())

    def test_read_to_bin_is_raw_bytes(self):
        fw = FakeFirmware()
        fw.mem[0x4400] = 0x42
        with tempfile.TemporaryDirectory() as d:
            dump = os.path.join(d, "x.bin")
            self.assertEqual(run(["read", "0x4400", "4", "-o", dump], fw)[0], 0)
            with open(dump, "rb") as f:
                self.assertEqual(f.read(), b"\x42\xFF\xFF\xFF")
            self.assertEqual(os.listdir(d), ["x.bin"])

    def test_failed_read_leaves_no_file_behind(self):
        fw = FakeFirmware()
        fw.fail_op = {3: 7}
        with tempfile.TemporaryDirectory() as d:
            dump = os.path.join(d, "dump.bin")
            code, _, _ = run(["read", "0x4400", "16", "-o", dump], fw)
            self.assertEqual(code, 1)
            self.assertEqual(os.listdir(d), [])
            with open(dump, "wb") as f:
                f.write(b"old")
            code, _, _ = run(["read", "--force", "0x4400", "16", "-o", dump], fw)
            self.assertEqual(code, 1)
            self.assertEqual(os.listdir(d), ["dump.bin"])
            with open(dump, "rb") as f:
                self.assertEqual(f.read(), b"old")

    def test_selftest(self):
        fw = FakeFirmware()
        fw.mem.update({0x4400: 0x12, 0x1800: 0x34})
        code, out, _ = run(["selftest"], fw)
        self.assertEqual(code, 0)
        self.assertEqual(out.splitlines()[-1], "selftest passed")
        self.assertEqual(fw.mem[0x4400], 0x12)
        self.assertEqual(fw.mem[0x1800], 0x34)
        self.assertNotIn(0x4401, fw.mem)
        self.assertEqual(fw.state, 0)

    def test_failing_selftest_exits_1(self):
        fw = FakeFirmware()
        fw.stuck = {0x2410: 0x00}
        code, out, err = run(["selftest"], fw)
        self.assertEqual(code, 1)
        self.assertIn("RAM read-back differs", err)
        self.assertNotIn("selftest passed", out)
        self.assertEqual(fw.state, 0)

    def test_trial(self):
        fw = FakeFirmware()
        for a in range(0x4400, 0x20000):
            fw.mem[a] = (a * 7) & 0xFF
        before = dict(fw.mem)
        with tempfile.TemporaryDirectory() as d:
            backup = os.path.join(d, "backup.hex")
            self.assertEqual(run(["read", "0x4400", "0x20000", "-o", backup], fw)[0], 0)
            code, out, _ = run(["trial", "--backup", backup], fw)
        self.assertEqual(code, 0)
        self.assertIn("trial passed: segment 0x24200", out)
        self.assertEqual(fw.mem, before)
        self.assertEqual(fw.state, 0)

    def test_trial_missing_backup_does_not_connect(self):
        fw = FakeFirmware()
        code, _, err = run(["trial", "--backup", "/nonexistent/backup.hex"], fw)
        self.assertEqual(code, 1)
        self.assertEqual(fw.connects, [])

    def _trial_interrupted(self, op):
        fw = FakeFirmware()
        for a in range(0x4400, 0x20000):
            fw.mem[a] = (a * 7) & 0xFF
        with tempfile.TemporaryDirectory() as d:
            backup = os.path.join(d, "backup.hex")
            self.assertEqual(run(["read", "0x4400", "0x20000", "-o", backup], fw)[0], 0)
            fw.interrupt_on_op = op
            code, _, err = run(["trial", "--backup", backup], fw)
        self.assertEqual(code, 130)
        self.assertEqual(fw.state, 0)
        self.assertTrue(fw.closed)
        return err

    def test_ctrl_c_during_trial_pattern_write(self):
        err = self._trial_interrupted(7)
        self.assertIn("partially erased or programmed", err)
        self.assertIn("compare the flash with your backup", err)

    def test_ctrl_c_during_trial_first_read(self):
        self._trial_interrupted(3)


if __name__ == "__main__":
    unittest.main()
