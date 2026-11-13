"""GDB Remote Serial Protocol framing: packets, acknowledgements, interrupts.

    packet:    $<payload>#<two hex digits: sum of the payload bytes mod 256>
    ack/nack:  + / -
    interrupt: the single byte 0x03 (Ctrl-C in GDB), sent between packets

Binary payloads (the X packet, qXfer replies) escape '#', '$', '}' and '*'
as '}' followed by the byte XOR 0x20.

Run-length encoding: '*' and a count character c repeat the character
before the '*' c - 29 more times ("0* " is "0000"). The checksum covers the
packet as sent, runs unexpanded. The GDB manual describes runs in replies;
GDB sends none, and it escapes '*' in its binary data, so the reader can
expand runs in what it receives without misreading data. This module never
sends runs, and frame() refuses a '*' because GDB would read it as the
start of one.
"""
from __future__ import annotations

INTERRUPT = 0x03
ESCAPE = 0x7D  # '}'
_SPECIAL = b"#$*}"
_HEX = frozenset(b"0123456789abcdefABCDEF")
_SINGLE = {INTERRUPT: "interrupt", ord("+"): "ack", ord("-"): "nack"}


def checksum(payload: bytes) -> int:
    return sum(payload) & 0xFF


def frame(payload: bytes) -> bytes:
    """The packet for `payload`. '$', '#' and '*' are refused (ValueError):
    escape binary data first."""
    for b in b"$#*":
        if b in payload:
            raise ValueError(f"{chr(b)!r} in a packet payload; binary data must be escaped")
    return b"$" + payload + b"#" + b"%02x" % checksum(payload)


def escape(data: bytes) -> bytes:
    out = bytearray()
    for b in data:
        if b in _SPECIAL:
            out += bytes([ESCAPE, b ^ 0x20])
        else:
            out.append(b)
    return bytes(out)


def unescape(data: bytes) -> bytes:
    """The bytes escape() encoded. ValueError if `data` ends in a lone '}'."""
    out = bytearray()
    it = iter(data)
    for b in it:
        if b == ESCAPE:
            b = next(it, None)
            if b is None:
                raise ValueError("binary data ends with the escape character '}'")
            b ^= 0x20
        out.append(b)
    return bytes(out)


def run_length_decode(data: bytes) -> bytes:
    """`data` with its runs expanded. ValueError for a '*' with nothing
    before it, without a count character, or with a count below 1 (the
    counts GDB's own decoder accepts)."""
    if b"*" not in data:
        return bytes(data)
    out = bytearray()
    i = 0
    while i < len(data):
        b = data[i]
        if b != 0x2A:  # '*'
            out.append(b)
            i += 1
            continue
        if not out or i + 1 >= len(data) or data[i + 1] - 29 < 1:
            raise ValueError("invalid run-length encoding")
        out += out[-1:] * (data[i + 1] - 29)
        i += 2
    return bytes(out)


class PacketReader:
    """Splits the bytes GDB sends into events:
    ("packet", payload), ("bad", None) for a packet with a wrong checksum or
    an invalid run (answer it with a nack and GDB sends it again),
    ("interrupt", None), ("ack", None) and ("nack", None). The payload has
    its runs expanded; binary data in it is still escaped. 0x03 inside a
    packet is data (an escaped '#'), not an interrupt. Other bytes between
    packets are dropped, and so is an unfinished packet when a new '$'
    arrives (packets never contain a raw '$'). An incomplete packet waits
    for more data."""

    def __init__(self):
        self._buf = bytearray()

    def feed(self, data: bytes) -> list:
        buf = self._buf
        buf += data
        events = []
        while buf:
            c = buf[0]
            if c != 0x24:  # '$'
                del buf[0]
                if c in _SINGLE:
                    events.append((_SINGLE[c], None))
                continue
            end = buf.find(b"#")
            restart = buf.find(b"$", 1, end if end >= 0 else len(buf))
            if restart > 0:
                del buf[:restart]
                continue
            if end < 0 or len(buf) < end + 3:
                break
            raw, digits = bytes(buf[1:end]), bytes(buf[end + 1:end + 3])
            del buf[:end + 3]
            events.append(self._packet(raw, digits))
        return events

    @staticmethod
    def _packet(raw: bytes, digits: bytes) -> tuple:
        if not _HEX.issuperset(digits) or int(digits, 16) != checksum(raw):
            return ("bad", None)
        try:
            return ("packet", run_length_decode(raw))
        except ValueError:
            return ("bad", None)
