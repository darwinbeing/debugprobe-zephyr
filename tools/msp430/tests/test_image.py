import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

import image  # noqa: E402
from image import ImageError, Segment  # noqa: E402


class IntelHexTest(unittest.TestCase):
    def test_single_record(self):
        segs = image.parse_ihex(":0400100001020304E2\n:00000001FF\n")
        self.assertEqual(segs, [Segment(0x10, bytes([1, 2, 3, 4]))])

    def test_extended_linear_address(self):
        text = ":020000040001F9\n:02440000AABB55\n:00000001FF\n"
        self.assertEqual(image.parse_ihex(text), [Segment(0x14400, b"\xAA\xBB")])

    def test_adjacent_records_merge(self):
        text = ":024400000102B7\n:024402000304B1\n:00000001FF\n"
        self.assertEqual(image.parse_ihex(text), [Segment(0x4400, bytes([1, 2, 3, 4]))])

    def test_bad_checksum(self):
        with self.assertRaisesRegex(ImageError, "line 1: bad checksum"):
            image.parse_ihex(":0400100001020304E3\n:00000001FF\n")

    def test_missing_end_record(self):
        with self.assertRaisesRegex(ImageError, "end-of-file"):
            image.parse_ihex(":0400100001020304E2\n")

    def test_overlapping_data(self):
        with self.assertRaisesRegex(ImageError, "overlap"):
            image.parse_ihex(":0400100001020304E2\n:0400100001020304E2\n:00000001FF\n")

    def test_garbage(self):
        with self.assertRaisesRegex(ImageError, "line 1"):
            image.parse_ihex("hello\n")

    def test_write_round_trip(self):
        segs = [Segment(0x4400, bytes(range(40))), Segment(0x14400, b"\xAA\xBB")]
        self.assertEqual(image.parse_ihex(image.write_ihex(segs)), segs)


class TiTxtTest(unittest.TestCase):
    def test_two_sections(self):
        text = "@4400\n01 02 03\n@FFFE\n00 44\nq\n"
        self.assertEqual(
            image.parse_titxt(text),
            [Segment(0x4400, bytes([1, 2, 3])), Segment(0xFFFE, b"\x00\x44")],
        )

    def test_missing_q(self):
        with self.assertRaisesRegex(ImageError, "'q'"):
            image.parse_titxt("@4400\n01 02\n")

    def test_data_before_address(self):
        with self.assertRaisesRegex(ImageError, "line 1"):
            image.parse_titxt("01 02\nq\n")


class TolerantReadingTest(unittest.TestCase):
    def _load(self, raw: bytes):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "x.dat")
            with open(path, "wb") as f:
                f.write(raw)
            return image.load(path)

    def test_bom_and_ctrl_z_in_intel_hex(self):
        want = [Segment(0x10, bytes([1, 2, 3, 4]))]
        raw = b":0400100001020304E2\n:00000001FF\n"
        self.assertEqual(self._load(b"\xef\xbb\xbf" + raw), want)
        self.assertEqual(self._load(raw + b"\x1a\n"), want)
        self.assertEqual(self._load(raw + b"\x1a"), want)

    def test_bom_and_ctrl_z_in_titxt(self):
        want = [Segment(0x10, bytes([1, 2, 3, 4]))]
        raw = b"@0010\n01 02 03 04\nq\n"
        self.assertEqual(self._load(b"\xef\xbb\xbf" + raw), want)
        self.assertEqual(self._load(raw + b"\x1a\n"), want)
        self.assertEqual(self._load(raw + b"\x1a"), want)

    def test_titxt_tokens_must_be_plain_hex(self):
        with self.assertRaisesRegex(ImageError, "line 2: not hexadecimal"):
            image.parse_titxt("@4400\n+1 02\nq\n")
        with self.assertRaisesRegex(ImageError, "line 1: not hexadecimal"):
            image.parse_titxt("@-10\n01\nq\n")
        with self.assertRaisesRegex(ImageError, "line 1: not hexadecimal"):
            image.parse_titxt("@123456789\n01\nq\n")
        with self.assertRaisesRegex(ImageError, "line 2: not hexadecimal"):
            image.parse_titxt("@4400\n0x 02\nq\n")


class WriteIhexTest(unittest.TestCase):
    def test_records_do_not_cross_64k(self):
        seg = Segment(0xFFF8, bytes(range(32)))
        text = image.write_ihex([seg])
        for line in text.splitlines():
            rec = bytes.fromhex(line[1:])
            if rec[3] == 0:
                self.assertLessEqual(((rec[1] << 8) | rec[2]) + rec[0], 0x10000, line)
        self.assertEqual(image.parse_ihex(text), [seg])
        self.assertIn(":020000040001F9", text)


class PaddingTest(unittest.TestCase):
    def test_odd_start_and_length(self):
        segs = image.pad_to_words([Segment(0x4401, bytes([1, 2, 3]))])
        self.assertEqual(segs, [Segment(0x4400, bytes([0xFF, 1, 2, 3]))])

    def test_padding_joins_neighbours(self):
        segs = image.pad_to_words(
            [Segment(0x4400, bytes([1, 2, 3])), Segment(0x4404, bytes([4]))]
        )
        self.assertEqual(segs, [Segment(0x4400, bytes([1, 2, 3, 0xFF, 4, 0xFF]))])

    def test_even_segments_unchanged(self):
        segs = [Segment(0x4400, b"\x01\x02"), Segment(0x5000, b"\x03\x04")]
        self.assertEqual(image.pad_to_words(segs), segs)


class LoadSaveTest(unittest.TestCase):
    def test_load_by_content(self):
        with tempfile.TemporaryDirectory() as d:
            hex_path = os.path.join(d, "a.dat")
            txt_path = os.path.join(d, "b.dat")
            with open(hex_path, "w") as f:
                f.write(":0400100001020304E2\n:00000001FF\n")
            with open(txt_path, "w") as f:
                f.write("@0010\n01 02 03 04\nq\n")
            want = [Segment(0x10, bytes([1, 2, 3, 4]))]
            self.assertEqual(image.load(hex_path), want)
            self.assertEqual(image.load(txt_path), want)

    def test_save_hex_and_bin(self):
        with tempfile.TemporaryDirectory() as d:
            seg = Segment(0x4400, bytes(range(20)))
            hex_path = os.path.join(d, "out.hex")
            bin_path = os.path.join(d, "out.bin")
            image.save(hex_path, [seg])
            image.save(bin_path, [seg])
            self.assertEqual(image.load(hex_path), [seg])
            with open(bin_path, "rb") as f:
                self.assertEqual(f.read(), seg.data)


if __name__ == "__main__":
    unittest.main()
