import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

import rsp  # noqa: E402


def wire(raw: bytes) -> bytes:
    """A packet exactly as given, with its checksum: for bytes frame() refuses."""
    return b"$" + raw + b"#" + b"%02x" % (sum(raw) & 0xFF)


class FramingTest(unittest.TestCase):
    def test_frame_and_checksum(self):
        self.assertEqual(rsp.frame(b"OK"), b"$OK#9a")
        self.assertEqual(rsp.frame(b""), b"$#00")

    def test_escape_round_trip(self):
        data = bytes(range(256))
        escaped = rsp.escape(data)
        for special in b"#$*":
            self.assertNotIn(special, escaped)
        self.assertEqual(rsp.unescape(escaped), data)
        self.assertEqual(rsp.escape(b"}"), b"}]")

    def test_escape_only_the_four_special_bytes(self):
        self.assertEqual(rsp.escape(b"#$*}"), b"}\x03}\x04}\x0a}\x5d")
        self.assertEqual(rsp.escape(b"\x00\x03\x20\x5d:,+-\xff"), b"\x00\x03\x20\x5d:,+-\xff")

    def test_unescape_refuses_a_dangling_escape(self):
        with self.assertRaises(ValueError):
            rsp.unescape(b"ab}")

    def test_frame_refuses_bytes_that_would_break_the_packet(self):
        # '$' and '#' end the framing; GDB reads '*' in a reply as a run length
        for payload in (b"a#b", b"$", b"00*"):
            with self.assertRaises(ValueError, msg=payload):
                rsp.frame(payload)
        self.assertEqual(rsp.frame(rsp.escape(b"#$*")), wire(b"}\x03}\x04}\x0a"))


class PacketReaderTest(unittest.TestCase):
    def test_packets_acks_and_interrupts(self):
        r = rsp.PacketReader()
        events = r.feed(b"+$g#67\x03-$m0,2#fb")
        self.assertEqual(events, [("ack", None), ("packet", b"g"), ("interrupt", None),
                                  ("nack", None), ("packet", b"m0,2")])

    def test_split_packet_waits_for_the_rest(self):
        r = rsp.PacketReader()
        self.assertEqual(r.feed(b"$qSupp"), [])
        self.assertEqual(r.feed(b"orted#"), [])
        self.assertEqual(r.feed(b"37"), [("packet", b"qSupported")])

    def test_bad_checksum(self):
        r = rsp.PacketReader()
        self.assertEqual(r.feed(b"$g#00$g#67"), [("bad", None), ("packet", b"g")])

    def test_noise_between_packets_is_dropped(self):
        r = rsp.PacketReader()
        self.assertEqual(r.feed(b"xyz$?#3f"), [("packet", b"?")])

    def test_split_at_every_position(self):
        x_data = b"}#$*\x03:"
        stream = (b"+" + rsp.frame(b"m4400,2") + b"\x03" +
                  rsp.frame(b"X2400,6:" + rsp.escape(x_data)) + b"-" + wire(b"X2400,3:a* "))
        expected = [("ack", None), ("packet", b"m4400,2"), ("interrupt", None),
                    ("packet", b"X2400,6:" + rsp.escape(x_data)), ("nack", None),
                    ("packet", b"X2400,3:a* ")]
        self.assertEqual(rsp.PacketReader().feed(stream), expected)
        for i in range(len(stream) + 1):
            r = rsp.PacketReader()
            self.assertEqual(r.feed(stream[:i]) + r.feed(stream[i:]), expected, i)
        r = rsp.PacketReader()
        self.assertEqual([e for i in range(len(stream)) for e in r.feed(stream[i:i + 1])],
                         expected)

    def test_two_packets_in_one_read(self):
        r = rsp.PacketReader()
        self.assertEqual(r.feed(b"$g#67$?#3f"), [("packet", b"g"), ("packet", b"?")])
        self.assertEqual(r.feed(b"$c#63$qAttached#8f"),
                         [("packet", b"c"), ("packet", b"qAttached")])

    def test_interrupt_between_packets(self):
        r = rsp.PacketReader()
        self.assertEqual(r.feed(b"\x03"), [("interrupt", None)])
        self.assertEqual(r.feed(b"$vCont;c#a8\x03"), [("packet", b"vCont;c"), ("interrupt", None)])
        self.assertEqual(r.feed(b"\x03$g#67\x03\x03"),
                         [("interrupt", None), ("packet", b"g"), ("interrupt", None),
                          ("interrupt", None)])

    def test_0x03_inside_a_packet_is_data(self):
        # an escaped '#' is '}' 0x03: no interrupt
        r = rsp.PacketReader()
        self.assertEqual(r.feed(b"$X2400,1:}\x03#35"), [("packet", b"X2400,1:}\x03")])

    def test_bad_checksums_get_a_bad_event(self):
        r = rsp.PacketReader()
        self.assertEqual(r.feed(b"$g#68"), [("bad", None)])
        self.assertEqual(r.feed(b"$g#6g"), [("bad", None)])  # not hex
        self.assertEqual(r.feed(b"$\x07# 7$\x07#+7"), [("bad", None), ("bad", None)])  # two hex digits
        self.assertEqual(r.feed(b"$m0,2#FB"), [("packet", b"m0,2")])  # either case

    def test_raw_star_in_X_packet_data_is_a_data_byte(self):
        # GDB need not escape '*' in what it sends, and never run-length
        # encodes: "a*  " is four bytes, not a run of 'a'
        data = b"a*  *"
        r = rsp.PacketReader()
        events = r.feed(wire(b"X2400,5:" + data))
        self.assertEqual(events, [("packet", b"X2400,5:a*  *")])
        self.assertEqual(rsp.unescape(events[0][1].split(b":", 1)[1]), data)
        self.assertEqual(r.feed(wire(b"X2400,1:*")), [("packet", b"X2400,1:*")])

    def test_garbage_before_a_packet_is_dropped(self):
        r = rsp.PacketReader()
        self.assertEqual(r.feed(b"\x00\xff}*#00 OK$?#3f"), [("packet", b"?")])
        self.assertEqual(r.feed(b"xyz"), [])
        self.assertEqual(r.feed(b"$g#67"), [("packet", b"g")])

    def test_a_new_packet_start_drops_an_unfinished_one(self):
        r = rsp.PacketReader()
        self.assertEqual(r.feed(b"$m44$g#67"), [("packet", b"g")])
        self.assertEqual(r.feed(b"$m44"), [])
        self.assertEqual(r.feed(b"$?#3f"), [("packet", b"?")])

    def test_escaped_bytes_in_X_packet_data(self):
        data = b"}#$*\x03:"
        packet = b"X2400,6:" + rsp.escape(data)
        self.assertEqual(packet, b"X2400,6:}]}\x03}\x04}\x0a\x03:")
        self.assertEqual(rsp.frame(packet), b"$X2400,6:}]}\x03}\x04}\x0a\x03:#59")
        events = rsp.PacketReader().feed(rsp.frame(packet))
        self.assertEqual(events, [("packet", packet)])
        head, body = events[0][1].split(b":", 1)
        self.assertEqual((head, rsp.unescape(body)), (b"X2400,6", data))

    def test_every_byte_value_through_an_X_packet(self):
        data = bytes(range(256))
        events = rsp.PacketReader().feed(rsp.frame(b"X2400,100:" + rsp.escape(data)))
        self.assertEqual(len(events), 1)
        self.assertEqual(rsp.unescape(events[0][1].split(b":", 1)[1]), data)


if __name__ == "__main__":
    unittest.main()
