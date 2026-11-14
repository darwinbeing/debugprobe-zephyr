import io
import os
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

import image  # noqa: E402
import symelf  # noqa: E402
from image import Segment  # noqa: E402

NAMES = ("address\told_name\tnew_name\tconfidence\tpurpose\n"
         "05472\tdemo_old_05472\tdemo_func_05472\thigh\tdemo\n"
         "1450E\tdemo_old_1450E\tdemo_far_func_1450E\thigh\tdemo\n")
OBJECTS = ("address\told_name\tnew_name\tkind\tconfidence\tmeaning\n"
           "2404\tdemo_old_2404\tdemo_var_2404\tvar\tmedium\tdemo\n"
           "-\tdemo_old_type\tt_demo\tstruct\thigh\tdemo\n"
           "258E\tt_demo.m258E\tdemo_member\tmember\thigh\tdemo\n"
           "0200\tdemo_old_0200\tdemo_periph_0200\tvar\thigh\tdemo\n"
           "4640\tdemo_old_4640\tdemo_const\tconst\thigh\tdemo\n")


def read_elf(data):
    """A small readelf: header fields, sections by name, symbols."""
    assert data[:4] == b"\x7fELF" and data[4] == 1 and data[5] == 1
    (e_type, e_machine, _, e_entry, _, e_shoff, e_flags, _, _, e_phnum, e_shentsize, e_shnum,
     e_shstrndx) = struct.unpack_from("<HHIIIIIHHHHHH", data, 16)
    shdrs = [struct.unpack_from("<10I", data, e_shoff + i * e_shentsize) for i in range(e_shnum)]
    shstr = shdrs[e_shstrndx]
    names = data[shstr[4]:shstr[4] + shstr[5]]

    def name_at(table, off):
        return table[off:table.index(b"\0", off)].decode()

    sections = {name_at(names, s[0]): s for s in shdrs[1:]}
    sym, strt = sections[".symtab"], sections[".strtab"]
    strings = data[strt[4]:strt[4] + strt[5]]
    symbols = {}
    for off in range(sym[4] + 16, sym[4] + sym[5], 16):
        st_name, value, size, info, _, shndx = struct.unpack_from("<IIIBBH", data, off)
        symbols[name_at(strings, st_name)] = (value, size, info & 0xF, info >> 4, shndx)
    return {"type": e_type, "machine": e_machine, "entry": e_entry, "flags": e_flags,
            "phnum": e_phnum, "sections": sections, "symbols": symbols,
            "index": {name_at(names, s[0]): i for i, s in enumerate(shdrs) if i}}


class SymElfTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.names = self.path("names.tsv", NAMES)
        self.objects = self.path("objnames.tsv", OBJECTS)

    def tearDown(self):
        self.tmp.cleanup()

    def path(self, name, text=None):
        p = os.path.join(self.tmp.name, name)
        if text is not None:
            with open(p, "wb" if isinstance(text, bytes) else "w") as f:
                f.write(text)
        return p

    def run_tool(self, *args):
        out, err = io.StringIO(), io.StringIO()
        code = symelf.main(list(args), out=out, err=err)
        return code, out.getvalue(), err.getvalue()

    def table_error(self, text):
        """Run the tool on one table it must refuse; the error output."""
        out = self.path("x.elf")
        if os.path.exists(out):
            os.remove(out)
        code, _, err = self.run_tool(self.path("t.tsv", text), "-o", out)
        self.assertEqual(code, 1)
        self.assertFalse(os.path.exists(out))
        self.assertTrue(err.startswith("error: ") and err.count("\n") == 1, err)  # no traceback
        return err

    def table_symbols(self, text):
        """Run the tool on one table it must accept; (summary line, symbols)."""
        out = self.path("ok.elf")
        code, summary, err = self.run_tool(self.path("t.tsv", text), "-o", out, "--force")
        self.assertEqual((code, err), (0, ""))
        with open(out, "rb") as f:
            return summary, read_elf(f.read())["symbols"]

    def test_symbols_and_sections(self):
        elf_path = self.path("sym.elf")
        code, out, _ = self.run_tool(self.names, self.objects, "-o", elf_path)
        self.assertEqual(code, 0)
        self.assertIn("2 functions, 3 objects", out)
        with open(elf_path, "rb") as f:
            elf = read_elf(f.read())
        self.assertEqual((elf["type"], elf["machine"], elf["flags"], elf["phnum"]), (2, 105, 45, 0))
        text = elf["sections"][".text"]
        self.assertEqual((text[1], text[2], text[3], text[5]), (8, 6, 0x4400, 0x20000))  # NOBITS
        bss = elf["sections"][".bss"]
        self.assertEqual((bss[1], bss[3], bss[5]), (8, 0x1C00, 0x2800))
        syms = elf["symbols"]
        self.assertEqual(syms["demo_func_05472"], (0x5472, 0, 2, 1, elf["index"][".text"]))
        self.assertEqual(syms["demo_far_func_1450E"][0], 0x1450E)
        self.assertEqual(syms["demo_var_2404"], (0x2404, 0, 1, 1, elf["index"][".bss"]))
        self.assertEqual(syms["demo_periph_0200"][4], 0xFFF1)  # absolute
        self.assertEqual(syms["demo_const"][2:], (1, 1, elf["index"][".text"]))
        self.assertNotIn("t_demo", syms)
        self.assertNotIn("demo_member", syms)

    def test_attributes_select_msp430x_large(self):
        attr = symelf.attributes()
        self.assertEqual(attr[0:1], b"A")
        length = struct.unpack_from("<I", attr, 1)[0]
        self.assertEqual(length, len(attr) - 1)
        self.assertEqual(attr[5:12], b"mspabi\0")
        self.assertEqual(attr[12], 1)  # Tag_File
        self.assertEqual(struct.unpack_from("<I", attr, 13)[0], len(attr) - 12)
        self.assertEqual(attr[17:], bytes([4, 2, 6, 2, 8, 2]))
        elf_path = self.path("sym.elf")
        self.run_tool(self.names, "-o", elf_path)
        with open(elf_path, "rb") as f:
            data = f.read()
        sec = read_elf(data)["sections"][".MSP430.attributes"]
        self.assertEqual(sec[1], 0x70000003)
        self.assertEqual(data[sec[4]:sec[4] + sec[5]], attr)

    def test_image_fills_text(self):
        hex_path = self.path("fw.hex")
        image.save(hex_path, [Segment(0x4400, b"\x31\x40\x00\x44"), Segment(0xFFFE, b"\x1A\x44")])
        elf_path = self.path("sym.elf")
        code, out, _ = self.run_tool(self.names, "--image", hex_path, "-o", elf_path)
        self.assertEqual(code, 0)
        self.assertIn("main flash from the image", out)
        with open(elf_path, "rb") as f:
            data = f.read()
        elf = read_elf(data)
        text = elf["sections"][".text"]
        self.assertEqual(text[1], 1)  # PROGBITS
        body = data[text[4]:text[4] + text[5]]
        self.assertEqual(body[:4], b"\x31\x40\x00\x44")
        self.assertEqual(body[4], 0xFF)
        self.assertEqual(body[0xFFFE - 0x4400:0x10000 - 0x4400], b"\x1A\x44")
        self.assertEqual(elf["entry"], 0x441A)

    def test_errors(self):
        out = self.path("x.elf")
        bad = self.path("bad.tsv", "address\tnew_name\tkind\n4400\tf\tbogus\n")
        self.assertIn("unknown kind 'bogus'", self.run_tool(bad, "-o", out)[2])
        dup = self.path("dup.tsv", "address\tnew_name\n4400\tf\n4500\tf\n")
        self.assertIn("duplicate symbol 'f'", self.run_tool(dup, "-o", out)[2])
        noaddr = self.path("noaddr.tsv", "name\nf\n")
        self.assertIn("no 'address' column", self.run_tool(noaddr, "-o", out)[2])
        far = self.path("far.tsv", "address\tname\n100000\tf\n")
        self.assertIn("outside the 20-bit space", self.run_tool(far, "-o", out)[2])
        self.assertFalse(os.path.exists(out))

    def test_table_errors(self):
        cases = [
            ("", "t.tsv: empty table"),
            ("# only a comment\n\n", "t.tsv: empty table"),
            ("address\tlabel\n4400\tf\n", "t.tsv: no 'new_name' or 'name' column"),
            ("address\tnew_name\n4400\t\n", "t.tsv:2: no name"),
            ("address\tnew_name\n\tf\n", "t.tsv:2: no address"),
            ("address\tnew_name\nzz\tf\n", "t.tsv:2: bad address 'zz'"),
            ("address\tnew_name\n+4400\tf\n", "t.tsv:2: bad address '+4400'"),
            ("address\tnew_name\n44_00\tf\n", "t.tsv:2: bad address '44_00'"),
            ("address\tnew_name\n004400\tf\n", "t.tsv:2: bad address '004400'"),
            ("address\tnew_name\n0x\tf\n", "t.tsv:2: bad address '0x'"),
            ("address\tnew_name\n0x100000\tf\n", "t.tsv:2: address 0x100000 outside the 20-bit space"),
            ("address\tnew_name\tsize\n4400\tf\tten\n", "t.tsv:2: bad size 'ten'"),
            ("address\tnew_name\tsize\n4400\tf\t-1\n", "t.tsv:2: bad size '-1'"),
            ("address\tnew_name\tsize\n4400\tf\t0x100000\n", "t.tsv:2: size 0x100000 outside 0 to 0xFFFFF"),
            ("address\tnew_name\tsize\n4400\tf\t4294967296\n", "t.tsv:2: size 4294967296 outside 0 to 0xFFFFF"),
            ("address\tnew_name\n4401\tf\n", "t.tsv:2: function at odd address 0x04401"),
            ("# a comment\naddress\tnew_name\n\n# another\n4401\tf\n", "t.tsv:5: function at odd address"),
            (b"address\tnew_name\tpurpose\n4400\tf\tna\xefve\n", "t.tsv: not UTF-8 text"),
            ("address\tnew_name\tpurpose\n4400\tf\t" + "x" * 200000 + "\n", "t.tsv: field larger than"),
        ]
        for text, message in cases:
            with self.subTest(text=text[:60]):
                self.assertIn(message, self.table_error(text))

    def test_table_format(self):
        # Quotes are ordinary characters: an unbalanced one swallows no rows.
        summary, _ = self.table_symbols('address\tnew_name\tpurpose\n4400\tdemo_a\t"says hi\n'
                                        '4500\tdemo_b\tdemo\n4600\tdemo_c\tdemo "x\n')
        self.assertIn("3 functions, 0 objects", summary)
        summary, syms = self.table_symbols("﻿address\tnew_name\n0x4400\tdemo_func\n")
        self.assertIn("1 functions, 0 objects", summary)
        self.assertEqual(syms["demo_func"][0], 0x4400)
        # Comments, blank lines, '-', odd objects and sizes.
        summary, syms = self.table_symbols(
            "# demo table\naddress\tnew_name\tkind\tsize\n\n# 4400\tdemo_commented\tfunc\n"
            "-\tdemo_skipped\tfunc\t\n0X4400\tdemo_func\tfunc\t\n2405\tdemo_byte\tvar\t1\n"
            "2406\tdemo_buf\tdata\t0x20\n2426\tdemo_word\tvar\t010\n")
        self.assertIn("1 functions, 3 objects", summary)
        self.assertEqual(sorted(syms), ["demo_buf", "demo_byte", "demo_func", "demo_word"])
        self.assertEqual(syms["demo_byte"][:3], (0x2405, 1, 1))
        self.assertEqual(syms["demo_buf"][:2], (0x2406, 0x20))
        self.assertEqual(syms["demo_word"][:2], (0x2426, 10))

    def test_refuses_to_overwrite_without_force(self):
        out = self.path("x.elf", "keep")
        code, _, err = self.run_tool(self.names, "-o", out)
        self.assertEqual(code, 1)
        self.assertIn("already exists", err)
        self.assertEqual(self.run_tool(self.names, "-o", out, "--force")[0], 0)

    def test_same_name_with_other_kind_or_size_is_a_duplicate(self):
        out = self.path("x.elf")
        func = self.path("func.tsv", "address\tnew_name\n4400\tf\n")
        var = self.path("var.tsv", "address\tnew_name\tkind\n4400\tf\tvar\n")
        code, _, err = self.run_tool(func, var, "-o", out)
        self.assertEqual(code, 1)
        self.assertIn("duplicate symbol 'f': 0x04400 (function, size 0) and 0x04400 (object, size 0)", err)
        sized = self.path("sized.tsv", "address\tnew_name\tsize\n4400\tf\t2\n")
        code, _, err = self.run_tool(func, sized, "-o", out)
        self.assertEqual(code, 1)
        self.assertIn("duplicate symbol 'f': 0x04400 (function, size 0) and 0x04400 (function, size 2)", err)
        self.assertFalse(os.path.exists(out))

    def test_same_symbol_twice_is_kept_once(self):
        out = self.path("x.elf")
        self.assertEqual(self.run_tool(self.names, self.names, "-o", out)[0], 0)
        with open(out, "rb") as f:
            self.assertEqual(len(read_elf(f.read())["symbols"]), 2)


if __name__ == "__main__":
    unittest.main()
