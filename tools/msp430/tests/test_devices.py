import ast
import dataclasses
import os
import re
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

import devices  # noqa: E402

F5529 = devices.MSP430F5529
SESSION_CPP = os.path.join(os.path.dirname(__file__), "..", "..", "..",
                           "lib", "msp430", "src", "session.cpp")

# The registers of read_side_effects as TI's msp430f5529.h defines them
# ("#define UCA0RXBUF_ (0x05CCu)"); copied by hand, the header is not in the
# repository.
HEADER_ADDRESSES = {
    "SYSJMBI0": 0x0188, "SYSJMBI1": 0x018A, "SYSBERRIV": 0x0198, "SYSUNIV": 0x019A,
    "SYSSNIV": 0x019C, "SYSRSTIV": 0x019E, "P1IV": 0x020E, "P2IV": 0x021E,
    "TA0IV": 0x036E, "TA1IV": 0x03AE, "TB0IV": 0x03EE, "TA2IV": 0x042E, "RTCIV": 0x04AE,
    "DMAIV": 0x050E, "UCA0RXBUF": 0x05CC, "UCA0IV": 0x05DE, "UCB0RXBUF": 0x05EC,
    "UCB0IV": 0x05FE, "UCA1RXBUF": 0x060C, "UCA1IV": 0x061E, "UCB1RXBUF": 0x062C,
    "UCB1IV": 0x063E, "ADC12IV": 0x070E, "ADC12MEM0": 0x0720, "ADC12MEM15": 0x073E,
    "CBIV": 0x08CE, "USBVECINT": 0x0932,
}


def probe_tables():
    """kPeripheralEnd and kKnownDevices from the probe's session.cpp:
    (peripheral_end, {device_id: (ram, main_flash, [hole, ...])})."""
    with open(SESSION_CPP) as f:
        text = f.read()
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    text = re.sub(r"\b(0x[0-9A-Fa-f]+)[uU]\b", r"\1", text)
    end = re.search(r"constexpr uint32_t kPeripheralEnd = (0x[0-9A-Fa-f]+);", text)
    table = re.search(r"constexpr KnownDevice kKnownDevices\[\] = (\{.*?\});", text, re.S)
    entries = ast.literal_eval(table.group(1).replace("{", "[").replace("}", "]"))
    return int(end.group(1), 16), {e[0]: (tuple(e[1]), tuple(e[2]), [tuple(h) for h in e[3]])
                                   for e in entries}


class ProbeMirrorTest(unittest.TestCase):
    """The host's write and PC rules are the probe's: a change to either
    kKnownDevices / kPeripheralEnd or this table without the other fails."""

    def test_the_host_table_is_the_probes(self):
        peripheral_end, probe = probe_tables()
        self.assertEqual(sorted(probe), sorted(devices.DEVICES))
        for device_id, (ram, main_flash, holes) in probe.items():
            d = devices.lookup(device_id)
            self.assertEqual(d.peripherals, (0x0000, peripheral_end), d.name)
            self.assertEqual(d.ram, ram, d.name)
            self.assertEqual((d.main_start, d.main_end), main_flash, d.name)
            self.assertEqual([(lo, hi) for lo, hi, _ in d.peripheral_holes], holes, d.name)


class DeviceTableTest(unittest.TestCase):
    def test_memory_map_leaves_out_the_bsl_and_vacant_memory(self):
        """Peripherals SFRIE1 (0x0100) to USBFUNADR (0x093F), the lowest and
        highest register in TI's msp430f5529.h."""
        self.assertEqual(F5529.memory_map(), [
            (0x0100, 0x0940, "ram"), (0x1800, 0x1A00, "rom"), (0x1A00, 0x1B00, "rom"),
            (0x1C00, 0x4400, "ram"), (0x4400, 0x24400, "rom")])

    def test_no_map_region_overlaps_the_bsl(self):
        lo, hi = F5529.bsl
        for start, end, _ in F5529.memory_map():
            self.assertTrue(end <= lo or start >= hi, hex(start))

    def test_debug_readable(self):
        self.assertTrue(F5529.debug_readable(0x4400, 0x20000))
        self.assertTrue(F5529.debug_readable(0x0100, 0x0840))  # every peripheral register
        self.assertTrue(F5529.debug_readable(0x093E, 2))  # USBIFG, USBFUNADR
        self.assertTrue(F5529.debug_readable(0x19FE, 4))  # info memory into the TLV
        self.assertTrue(F5529.debug_readable(0x43FE, 4))  # RAM into flash
        self.assertFalse(F5529.debug_readable(0x0000, 2))  # vacant
        self.assertFalse(F5529.debug_readable(0x00FE, 4))  # vacant into the SFRs
        self.assertFalse(F5529.debug_readable(0x093E, 4))  # USB into vacant
        self.assertFalse(F5529.debug_readable(0x0940, 2))  # vacant
        self.assertFalse(F5529.debug_readable(0x0FFE, 2))  # vacant
        self.assertFalse(F5529.debug_readable(0x1000, 2))  # BSL
        self.assertFalse(F5529.debug_readable(0x17FE, 2))  # BSL, last word
        self.assertFalse(F5529.debug_readable(0x0FFE, 4))  # vacant into the BSL
        self.assertFalse(F5529.debug_readable(0x17FE, 4))  # BSL into info memory
        self.assertFalse(F5529.debug_readable(0x1B00, 2))  # vacant
        self.assertFalse(F5529.debug_readable(0x1AFE, 4))  # TLV into vacant
        self.assertFalse(F5529.debug_readable(0x1BFE, 4))  # vacant into RAM
        self.assertFalse(F5529.debug_readable(0x243FE, 4))  # past the end of flash

    def test_debug_readable_refuses_empty_and_negative_lengths(self):
        self.assertFalse(F5529.debug_readable(0x2400, 0))
        self.assertFalse(F5529.debug_readable(0x2400, -2))

    def test_the_bsl_is_never_readable_even_if_the_map_covered_it(self):
        wrong = dataclasses.replace(F5529, info=(0x1000, 0x1A00))
        self.assertIn((0x1000, 0x1A00, "rom"), wrong.memory_map())
        self.assertFalse(wrong.debug_readable(0x1000, 2))
        self.assertFalse(wrong.debug_readable(0x17FE, 4))
        self.assertTrue(wrong.debug_readable(0x1800, 2))

    def test_every_boundary_is_even(self):
        """gdbserver widens odd byte accesses to words; with even boundaries a
        widened access stays on the same side of each one."""
        ranges = [(s, e) for s, e, _ in F5529.memory_map()]
        ranges += [(s, e) for s, e, _ in F5529.peripheral_holes]
        ranges += list(F5529.vacant)
        ranges += [(s, e) for s, e, _, _ in F5529.read_side_effects]
        ranges += [F5529.peripherals, F5529.bsl, F5529.info, F5529.tlv, F5529.ram,
                   (F5529.main_start, F5529.main_end)]
        for start, end in ranges:
            self.assertEqual((start % 2, end % 2), (0, 0), (hex(start), hex(end)))

    def test_read_side_effects_are_the_header_registers(self):
        mem = ("ADC12MEM0", "ADC12MEM15")
        expected = [(a, a + 2, n) for n, a in HEADER_ADDRESSES.items() if n not in mem]
        expected.append((HEADER_ADDRESSES[mem[0]], HEADER_ADDRESSES[mem[1]] + 2,
                         "ADC12MEM0-ADC12MEM15"))
        self.assertEqual(sorted((s, e, n) for s, e, n, _ in F5529.read_side_effects),
                         sorted(expected))

    def test_read_side_effects_are_sorted_readable_and_say_what_they_do(self):
        entries = F5529.read_side_effects
        self.assertEqual(list(entries), sorted(entries))
        for (_, end, _, _), (start, _, name, _) in zip(entries, entries[1:]):
            self.assertLessEqual(end, start, name)
        for start, end, name, effect in entries:
            self.assertTrue(F5529.debug_readable(start, end - start), name)
            self.assertTrue(effect.startswith("clears "), name)

    def test_debug_read_side_effect(self):
        def hit(addr, n):
            entry = F5529.debug_read_side_effect(addr, n)
            return entry and entry[2]
        self.assertEqual(hit(0x05CD, 1), "UCA0RXBUF")  # odd byte of UCA0RXBUF's word
        self.assertEqual(hit(0x05CC, 2), "UCA0RXBUF")
        self.assertEqual(hit(0x05C0, 0x40), "UCA0RXBUF")  # USCI_A0 and B0: the first hit
        self.assertEqual(hit(0x05DD, 2), "UCA0IV")  # widened to 0x05DC-0x05DF
        self.assertEqual(hit(0x0200, 0x0F), "P1IV")  # widened to 0x0200-0x020F
        self.assertEqual(hit(0x073F, 1), "ADC12MEM0-ADC12MEM15")
        self.assertEqual(hit(0x019E, 2), "SYSRSTIV")
        self.assertEqual(F5529.debug_read_side_effect(0x0932, 2)[3],
                         "clears the highest pending flag")
        for addr, n in ((0x05CA, 2), (0x05CB, 1), (0x05CE, 0x10), (0x05DD, 1), (0x0200, 0x0E),
                        (0x0740, 2), (0x2400, 0x100), (0x4400, 0x20000), (0x05CC, 0)):
            self.assertIsNone(F5529.debug_read_side_effect(addr, n), (hex(addr), n))

    def test_debug_writable(self):
        for addr, n in ((0x2400, 2), (0x1C00, 0x2800), (0x0200, 2), (0x011E, 2), (0x0150, 2)):
            self.assertTrue(F5529.debug_writable(addr, n), hex(addr))
        for addr, n in ((0x4400, 2), (0x43FE, 4), (0x1800, 2), (0x1000, 2), (0x1A00, 2),
                        (0x0140, 2), (0x013E, 2), (0x013E, 4), (0x014E, 2), (0x0FFE, 4),
                        (0x2400, 0)):
            self.assertFalse(F5529.debug_writable(addr, n), hex(addr))

    def test_refused_peripherals(self):
        """PMM, FCTL, SYS, DMA, as in kKnownDevices (ProbeMirrorTest)."""
        self.assertEqual([(lo, hi) for lo, hi, _ in F5529.peripheral_holes],
                         [(0x0120, 0x0140), (0x0140, 0x0150), (0x0180, 0x01A0), (0x0500, 0x0540)])

    def test_debug_writable_at_every_edge_of_the_probes_rule(self):
        """Session::writable: RAM 0x1C00-0x43FF, and the peripherals
        0x0000-0x0FFF without PMM 0x0120-0x013F, FCTL 0x0140-0x014F, SYS
        0x0180-0x019F and DMA 0x0500-0x053F."""
        writable = ((0x0000, 2), (0x0000, 0x0120), (0x011E, 2), (0x011F, 1), (0x0150, 2),
                    (0x017E, 2), (0x01A0, 2), (0x04FE, 2), (0x0540, 2), (0x05C0, 2),
                    (0x0FFE, 2), (0x1C00, 2), (0x43FE, 2))
        refused = ((0x011E, 4), (0x0120, 1), (0x0120, 2), (0x0130, 2), (0x013E, 2), (0x013F, 1),
                   (0x013E, 4), (0x0140, 2), (0x014E, 2), (0x014F, 1), (0x014E, 4),
                   (0x017E, 4), (0x0180, 2), (0x019E, 2), (0x019E, 4),
                   (0x04FE, 4), (0x0500, 2), (0x053E, 2), (0x053E, 4),
                   (0x0000, 0x1000), (0x0FFE, 4), (0x1000, 2), (0x1BFE, 2), (0x1BFE, 4),
                   (0x43FE, 4), (0x4400, 2), (0x243FE, 2), (0x24400, 2))
        for addr, n in writable:
            self.assertTrue(F5529.debug_writable(addr, n), (hex(addr), n))
        for addr, n in refused:
            self.assertFalse(F5529.debug_writable(addr, n), (hex(addr), n))

    def test_debug_pc_writable_only_into_main_flash(self):
        current = 0x5000
        for pc in (0x4400, 0x243FE):
            self.assertTrue(F5529.debug_pc_writable(pc, current), hex(pc))
        for pc in (0x24400, 0x1000, 0x2400, 0x43FE, 0x1800, 0x1A00, 0x0000):
            self.assertFalse(F5529.debug_pc_writable(pc, current), hex(pc))

    def test_debug_pc_writable_leaves_the_pc_where_it_is(self):
        """GDB's G packet writes every register back, also when the CPU is
        halted in code the firmware runs from RAM."""
        self.assertTrue(F5529.debug_pc_writable(0x2400, 0x2400))
        self.assertFalse(F5529.debug_pc_writable(0x2402, 0x2400))


if __name__ == "__main__":
    unittest.main()
