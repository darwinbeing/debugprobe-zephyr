"""Firmware image files: Intel HEX and TI-TXT readers, Intel HEX and raw writers."""
from __future__ import annotations

import re
from dataclasses import dataclass

CTRL_Z = "\x1a"  # some tools append it as an end-of-file mark
_HEX_BYTE = re.compile(r"[0-9A-Fa-f]{2}")
_HEX_ADDR = re.compile(r"[0-9A-Fa-f]{1,8}")


class ImageError(Exception):
    """The image file is malformed."""


@dataclass(frozen=True)
class Segment:
    """A contiguous run of bytes at an absolute address."""

    addr: int
    data: bytes

    @property
    def end(self) -> int:
        return self.addr + len(self.data)


def _segments(byte_map: dict[int, int]) -> list[Segment]:
    """Group an address->byte map into contiguous segments, sorted by address."""
    segs: list[Segment] = []
    start = prev = None
    buf = bytearray()
    for addr in sorted(byte_map):
        if prev is None or addr != prev + 1:
            if buf:
                segs.append(Segment(start, bytes(buf)))
            start, buf = addr, bytearray()
        buf.append(byte_map[addr])
        prev = addr
    if buf:
        segs.append(Segment(start, bytes(buf)))
    return segs


def _store(byte_map: dict[int, int], addr: int, value: int, lineno: int) -> None:
    if addr in byte_map:
        raise ImageError(f"line {lineno}: overlapping data at 0x{addr:05X}")
    byte_map[addr] = value


def parse_ihex(text: str) -> list[Segment]:
    byte_map: dict[int, int] = {}
    base = 0
    eof = False
    for lineno, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if not line or line == CTRL_Z:
            continue
        if eof:
            raise ImageError(f"line {lineno}: data after the end-of-file record")
        if not line.startswith(":"):
            raise ImageError(f"line {lineno}: not an Intel HEX record")
        try:
            rec = bytes.fromhex(line[1:])
        except ValueError:
            raise ImageError(f"line {lineno}: not hexadecimal") from None
        if len(rec) < 5 or len(rec) != rec[0] + 5:
            raise ImageError(f"line {lineno}: wrong record length")
        if sum(rec) & 0xFF:
            raise ImageError(f"line {lineno}: bad checksum")
        offset = (rec[1] << 8) | rec[2]
        rtype = rec[3]
        data = rec[4:-1]
        if rtype == 0x00:
            for i, value in enumerate(data):
                _store(byte_map, base + offset + i, value, lineno)
        elif rtype == 0x01:
            eof = True
        elif rtype in (0x02, 0x04):
            if len(data) != 2:
                raise ImageError(f"line {lineno}: wrong record length")
            base = ((data[0] << 8) | data[1]) << (4 if rtype == 0x02 else 16)
        elif rtype in (0x03, 0x05):
            pass  # start address: not used
        else:
            raise ImageError(f"line {lineno}: unsupported record type {rtype:02X}")
    if not eof:
        raise ImageError("missing end-of-file record")
    return _segments(byte_map)


def parse_titxt(text: str) -> list[Segment]:
    byte_map: dict[int, int] = {}
    addr = None
    done = False
    for lineno, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if not line or line == CTRL_Z:
            continue
        if done:
            raise ImageError(f"line {lineno}: data after the terminating 'q'")
        try:
            if line.startswith("@"):
                text_addr = line[1:].strip()
                if not _HEX_ADDR.fullmatch(text_addr):
                    raise ValueError(text_addr)
                addr = int(text_addr, 16)
            elif line.lower() == "q":
                done = True
            else:
                if addr is None:
                    raise ImageError(f"line {lineno}: data before the first @address")
                for token in line.split():
                    if not _HEX_BYTE.fullmatch(token):
                        raise ValueError(token)
                    _store(byte_map, addr, int(token, 16), lineno)
                    addr += 1
        except ValueError:
            raise ImageError(f"line {lineno}: not hexadecimal") from None
    if not done:
        raise ImageError("missing terminating 'q'")
    return _segments(byte_map)


def load(path: str) -> list[Segment]:
    """Read an image; the format is detected from the first character."""
    with open(path, "r", encoding="utf-8-sig", errors="replace") as f:  # utf-8-sig drops a BOM
        text = f.read()
    first = text.lstrip()[:1]
    try:
        if first == ":":
            return parse_ihex(text)
        if first == "@":
            return parse_titxt(text)
    except ImageError as e:
        raise ImageError(f"{path}: {e}") from None
    raise ImageError(f"{path}: neither Intel HEX nor TI-TXT (use objcopy -O ihex for ELF files)")


def pad_to_words(segments: list[Segment], fill: int = 0xFF) -> list[Segment]:
    """Make every segment start on an even address and have an even length."""
    byte_map = {s.addr + i: b for s in segments for i, b in enumerate(s.data)}
    for addr in list(byte_map):
        byte_map.setdefault(addr ^ 1, fill)
    return _segments(byte_map)


def _record(rtype: int, offset: int, data: bytes) -> str:
    rec = bytes([len(data), (offset >> 8) & 0xFF, offset & 0xFF, rtype]) + data
    return ":" + (rec + bytes([-sum(rec) & 0xFF])).hex().upper()


def write_ihex(segments: list[Segment]) -> str:
    lines = []
    upper = 0
    for seg in segments:
        off = 0
        while off < len(seg.data):
            addr = seg.addr + off
            if addr >> 16 != upper:
                upper = addr >> 16
                lines.append(_record(0x04, 0, bytes([upper >> 8, upper & 0xFF])))
            n = min(16, len(seg.data) - off, 0x10000 - (addr & 0xFFFF))  # stay inside 64 KB
            lines.append(_record(0x00, addr & 0xFFFF, seg.data[off:off + n]))
            off += n
    lines.append(_record(0x01, 0, b""))
    return "\n".join(lines) + "\n"


def save(path: str, segments: list[Segment]) -> None:
    """Write `.bin` as raw bytes (one segment only), anything else as Intel HEX."""
    if path.lower().endswith(".bin"):
        if len(segments) != 1:
            raise ImageError("a .bin file can hold only one contiguous block")
        with open(path, "wb") as f:
            f.write(segments[0].data)
    else:
        with open(path, "w") as f:
            f.write(write_ihex(segments))
