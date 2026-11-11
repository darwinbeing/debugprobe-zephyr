import contextlib
import io
import os
import sys
import tempfile
import unittest

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


if __name__ == "__main__":
    unittest.main()
