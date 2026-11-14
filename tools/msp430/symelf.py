#!/usr/bin/env python3
"""Write a symbol-only ELF file for msp430-elf-gdb from name tables.

    symelf.py names.tsv [objnames.tsv ...] -o symbols.elf [--image backup.hex]

Each table is tab-separated UTF-8 text (a byte-order mark is ignored) with a
header row. Quotes are ordinary characters. Blank lines and lines starting
with # (comments) are skipped. Columns used:
  address   1 to 5 hex digits, with or without 0x; "-" (a type, not a
            symbol) skips the row, an empty address is an error
  new_name  the symbol name (or a column called "name")
  kind      optional: func (the default when the column is missing), var,
            const, data; struct, union and member rows are skipped
  size      optional, decimal or 0x hex, below 0x100000; 0 when missing
Functions must be at even addresses. A name may appear once; an exact repeat
of a row counts once, and a name that appears again with another address,
kind or size is an error.

The file is an ELF32 for the MSP430X (EM_MSP430) with the attributes
msp430-elf-gdb reads to choose the MSP430X instruction set and the large code
model (a .MSP430.attributes section). Main flash is the .text section and RAM
the .bss section, both without contents; with --image, .text holds the
image's main-flash bytes, so GDB can disassemble without a target. Symbols in
neither are absolute. Tables and images of third-party firmware stay outside
the repository.
"""
from __future__ import annotations

import argparse
import csv
import os
import re
import struct
import sys

import devices
import image

EM_MSP430 = 105
E_MSP430_MACH_MSP430X = 45
ELFOSABI_STANDALONE = 255  # what msp430-elf-gcc writes
SHT_PROGBITS, SHT_SYMTAB, SHT_STRTAB, SHT_NOBITS = 1, 2, 3, 8
SHT_MSP430_ATTRIBUTES = 0x70000003
SHF_WRITE, SHF_ALLOC, SHF_EXECINSTR = 1, 2, 4
STB_GLOBAL, STT_OBJECT, STT_FUNC = 1, 1, 2
SHN_ABS = 0xFFF1
# OFBA_MSPABI tags and values (binutils include/elf/msp430.h)
TAG_ISA, TAG_CODE_MODEL, TAG_DATA_MODEL = 4, 6, 8
ISA_MSP430X, MODEL_LARGE = 2, 2

FUNC_KINDS = {"func", "function", "code"}
OBJECT_KINDS = {"var", "const", "data", "object"}
SKIP_KINDS = {"struct", "union", "member", "type"}
SPACE = 0x100000  # the MSP430X's 20-bit address space
ADDRESS = re.compile(r"(?:0[xX])?[0-9A-Fa-f]{1,5}")
HEX = re.compile(r"(?:0[xX])?[0-9A-Fa-f]+")
SIZE = re.compile(r"0[xX][0-9A-Fa-f]+|[0-9]+")


class SymbolError(Exception):
    """A table cannot be turned into symbols; the message names the row."""


def _address(text: str, where: str) -> int:
    if ADDRESS.fullmatch(text):
        return int(text, 16)
    if HEX.fullmatch(text) and int(text, 16) >= SPACE:
        raise SymbolError(f"{where}: address 0x{int(text, 16):X} outside the 20-bit space")
    raise SymbolError(f"{where}: bad address '{text}'")


def _size(text: str, where: str) -> int:
    if not text:
        return 0
    if not SIZE.fullmatch(text):
        raise SymbolError(f"{where}: bad size '{text}'")
    size = int(text, 16) if text[:2] in ("0x", "0X") else int(text, 10)
    if size >= SPACE:
        raise SymbolError(f"{where}: size {text} outside 0 to 0x{SPACE - 1:X}")
    return size


def read_table(path: str) -> list:
    """[(name, address, size, STT_FUNC | STT_OBJECT)] from one table."""
    try:
        with open(path, newline="", encoding="utf-8-sig") as f:
            reader = csv.reader(f, delimiter="\t", quoting=csv.QUOTE_NONE)
            rows = [(reader.line_num, row) for row in reader]
    except UnicodeDecodeError:
        raise SymbolError(f"{path}: not UTF-8 text") from None
    except csv.Error as e:
        raise SymbolError(f"{path}: {e}") from None
    rows = [(line, row) for line, row in rows
            if any(cell.strip() for cell in row) and not row[0].lstrip().startswith("#")]
    if not rows:
        raise SymbolError(f"{path}: empty table")
    header = [h.strip() for h in rows[0][1]]
    if "address" not in header:
        raise SymbolError(f"{path}: no 'address' column")
    name_col = "new_name" if "new_name" in header else "name"
    if name_col not in header:
        raise SymbolError(f"{path}: no 'new_name' or 'name' column")
    col = {h: i for i, h in enumerate(header)}
    symbols = []
    for line, row in rows[1:]:
        where = f"{path}:{line}"

        def cell(name):
            i = col.get(name)
            return row[i].strip() if i is not None and i < len(row) else ""

        addr_text = cell("address")
        kind = cell("kind").lower() or "func"
        if addr_text == "-" or kind in SKIP_KINDS:
            continue
        if kind in FUNC_KINDS:
            stype = STT_FUNC
        elif kind in OBJECT_KINDS:
            stype = STT_OBJECT
        else:
            raise SymbolError(f"{where}: unknown kind '{kind}'")
        name = cell(name_col)
        if not name:
            raise SymbolError(f"{where}: no name")
        if not addr_text:
            raise SymbolError(f"{where}: no address ('-' skips a row)")
        addr = _address(addr_text, where)
        size = _size(cell("size"), where)
        if stype == STT_FUNC and addr & 1:
            raise SymbolError(f"{where}: function at odd address 0x{addr:05X}")
        symbols.append((name, addr, size, stype))
    return symbols


def _uleb(value: int) -> bytes:
    out = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        out.append(byte | (0x80 if value else 0))
        if not value:
            return bytes(out)


def attributes() -> bytes:
    """.MSP430.attributes: format 'A', vendor "mspabi", file-scope attributes
    ISA = MSP430X, code model = large, data model = large."""
    attrs = (_uleb(TAG_ISA) + _uleb(ISA_MSP430X) + _uleb(TAG_CODE_MODEL) + _uleb(MODEL_LARGE) +
             _uleb(TAG_DATA_MODEL) + _uleb(MODEL_LARGE))
    file_sub = b"\x01" + struct.pack("<I", 5 + len(attrs)) + attrs
    vendor = b"mspabi\0"
    return b"A" + struct.pack("<I", 4 + len(vendor) + len(file_sub)) + vendor + file_sub


def build(symbols: list, device, text_data: bytes | None = None, entry: int = 0) -> bytes:
    """The ELF file as bytes. text_data: main-flash contents, or None."""
    def describe(addr, size, stype):
        return f"0x{addr:05X} ({'function' if stype == STT_FUNC else 'object'}, size {size})"

    seen = {}
    for name, *rest in symbols:
        if name in seen and seen[name] != tuple(rest):
            raise SymbolError(f"duplicate symbol '{name}': {describe(*seen[name])} and {describe(*rest)}")
        seen[name] = tuple(rest)
    symbols = sorted({(n, a, s, t) for n, a, s, t in symbols}, key=lambda x: (x[1], x[0]))

    text_lo, text_hi = device.main_start, device.main_end
    bss_lo, bss_hi = device.ram
    shstr = b"\0.text\0.bss\0.MSP430.attributes\0.symtab\0.strtab\0.shstrtab\0"

    def sh_name(n):
        return shstr.index(n.encode() + b"\0")

    strtab = bytearray(b"\0")
    symtab = bytearray(16)  # symbol 0
    for name, addr, size, stype in symbols:
        if text_lo <= addr < text_hi:
            shndx = 1
        elif bss_lo <= addr < bss_hi:
            shndx = 2
        else:
            shndx = SHN_ABS
        symtab += struct.pack("<IIIBBH", len(strtab), addr, size, (STB_GLOBAL << 4) | stype, 0, shndx)
        strtab += name.encode() + b"\0"

    attr = attributes()
    body = bytearray()
    offsets = {}

    def place(key, data):
        offsets[key] = 52 + len(body)
        body.extend(data)
        while len(body) % 4:
            body.append(0)

    if text_data is not None:
        place("text", text_data)
    place("attr", attr)
    place("symtab", symtab)
    place("strtab", strtab)
    place("shstr", shstr)
    shoff = 52 + len(body)

    def shdr(name, stype, flags, addr, off, size, link=0, info=0, align=1, entsize=0):
        return struct.pack("<10I", name, stype, flags, addr, off, size, link, info, align, entsize)

    sections = [
        bytes(40),
        shdr(sh_name(".text"), SHT_PROGBITS if text_data is not None else SHT_NOBITS,
             SHF_ALLOC | SHF_EXECINSTR, text_lo, offsets.get("text", shoff), text_hi - text_lo,
             align=2),
        shdr(sh_name(".bss"), SHT_NOBITS, SHF_ALLOC | SHF_WRITE, bss_lo, shoff, bss_hi - bss_lo, align=2),
        shdr(sh_name(".MSP430.attributes"), SHT_MSP430_ATTRIBUTES, 0, 0, offsets["attr"], len(attr)),
        shdr(sh_name(".symtab"), SHT_SYMTAB, 0, 0, offsets["symtab"], len(symtab), link=5, info=1,
             align=4, entsize=16),
        shdr(sh_name(".strtab"), SHT_STRTAB, 0, 0, offsets["strtab"], len(strtab)),
        shdr(sh_name(".shstrtab"), SHT_STRTAB, 0, 0, offsets["shstr"], len(shstr)),
    ]
    # ELF32, little-endian, version 1
    ident = b"\x7fELF" + bytes([1, 1, 1, ELFOSABI_STANDALONE]) + bytes(8)
    header = ident + struct.pack("<HHIIIIIHHHHHH", 2, EM_MSP430, 1, entry, 0, shoff,
                                 E_MSP430_MACH_MSP430X, 52, 0, 0, 40, len(sections), 6)
    return header + bytes(body) + b"".join(sections)


def main(argv=None, out=sys.stdout, err=sys.stderr) -> int:
    p = argparse.ArgumentParser(prog="symelf.py", description=__doc__.splitlines()[0])
    p.add_argument("tables", nargs="+", help="tab-separated name tables")
    p.add_argument("-o", "--out", required=True, help="the ELF file to write")
    p.add_argument("--image", help="Intel HEX or TI-TXT image: main flash goes into .text")
    p.add_argument("--force", action="store_true", help="replace the output file if it exists")
    args = p.parse_args(argv)
    device = devices.MSP430F5529
    try:
        if os.path.exists(args.out) and not args.force:
            raise SymbolError(f"{args.out} already exists; choose another name or pass --force")
        symbols = []
        for path in args.tables:
            symbols += read_table(path)
        symbols = list(dict.fromkeys(symbols))  # the same row in two tables counts once
        text, entry, warning = None, 0, None
        if args.image:
            base, end = device.main_start, device.main_end
            flash = bytearray(b"\xFF" * (end - base))
            copied = 0
            for seg in image.load(args.image):
                lo, hi = max(seg.addr, base), min(seg.end, end)
                if lo < hi:
                    flash[lo - base:hi - base] = seg.data[lo - seg.addr:hi - seg.addr]
                    copied += hi - lo
            if not copied:
                warning = f"{args.image} has no bytes in main flash (0x{base:05X}-0x{end - 1:05X})"
            text = bytes(flash)
            entry = flash[0xFFFE - base] | (flash[0xFFFF - base] << 8)
        data = build(symbols, device, text, entry)
        with open(args.out, "wb") as f:
            f.write(data)
    except (SymbolError, image.ImageError, OSError) as e:
        err.write(f"error: {e}\n")
        return 1
    if warning:
        err.write(f"warning: {warning}\n")
    functions = sum(1 for s in symbols if s[3] == STT_FUNC)
    out.write(f"wrote {args.out}: {functions} functions, {len(symbols) - functions} objects"
              + (", main flash from the image" if text is not None else "") + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
