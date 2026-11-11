import io
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.dirname(__file__))

import image  # noqa: E402
import sbw  # noqa: E402
from fake_firmware import FakeFirmware  # noqa: E402
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
        code, _, err = run(["--speed", "50", "info"], fw)
        self.assertEqual(code, 1)
        self.assertIn("--speed", err)

        fw = FakeFirmware()
        code, _, err = run(["flash", "/nonexistent/fw.hex"], fw)
        self.assertEqual(code, 1)
        self.assertEqual(fw.state, 0)   # disconnected despite the error

    def test_reset(self):
        fw = FakeFirmware()
        self.assertEqual(run(["reset"], fw)[0], 0)
        self.assertIn(("disconnect",), fw.ops)


if __name__ == "__main__":
    unittest.main()
