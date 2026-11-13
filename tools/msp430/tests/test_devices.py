import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

import devices  # noqa: E402

F5529 = devices.MSP430F5529


class DeviceTableTest(unittest.TestCase):
    def test_memory_map_leaves_out_the_bsl_and_vacant_memory(self):
        self.assertEqual(F5529.memory_map(), [
            (0x0000, 0x1000, "ram"), (0x1800, 0x1A00, "rom"), (0x1A00, 0x1B00, "rom"),
            (0x1C00, 0x4400, "ram"), (0x4400, 0x24400, "rom")])

    def test_debug_readable(self):
        self.assertTrue(F5529.debug_readable(0x4400, 0x20000))
        self.assertTrue(F5529.debug_readable(0x0000, 0x1000))  # every peripheral
        self.assertTrue(F5529.debug_readable(0x19FE, 4))  # info memory into the TLV
        self.assertTrue(F5529.debug_readable(0x43FE, 4))  # RAM into flash
        self.assertFalse(F5529.debug_readable(0x1000, 2))  # BSL
        self.assertFalse(F5529.debug_readable(0x17FE, 2))  # BSL, last word
        self.assertFalse(F5529.debug_readable(0x0FFE, 4))  # peripherals into the BSL
        self.assertFalse(F5529.debug_readable(0x17FE, 4))  # BSL into info memory
        self.assertFalse(F5529.debug_readable(0x1B00, 2))  # vacant
        self.assertFalse(F5529.debug_readable(0x1AFE, 4))  # TLV into vacant
        self.assertFalse(F5529.debug_readable(0x1BFE, 4))  # vacant into RAM
        self.assertFalse(F5529.debug_readable(0x243FE, 4))  # past the end of flash

    def test_debug_writable(self):
        for addr, n in ((0x2400, 2), (0x1C00, 0x2800), (0x0200, 2), (0x013E, 2), (0x0150, 2)):
            self.assertTrue(F5529.debug_writable(addr, n), hex(addr))
        for addr, n in ((0x4400, 2), (0x43FE, 4), (0x1800, 2), (0x1000, 2), (0x1A00, 2),
                        (0x0140, 2), (0x013E, 4), (0x014E, 2), (0x0FFE, 4), (0x2400, 0)):
            self.assertFalse(F5529.debug_writable(addr, n), hex(addr))

    def test_refused_peripherals_are_the_probes(self):
        """kKnownDevices in lib/msp430/src/session.cpp: FCTL, SYS, DMA."""
        self.assertEqual([(lo, hi) for lo, hi, _ in F5529.peripheral_holes],
                         [(0x0140, 0x0150), (0x0180, 0x01A0), (0x0500, 0x0540)])

    def test_debug_writable_at_every_edge_of_the_probes_rule(self):
        """Session::writable: RAM 0x1C00-0x43FF, and the peripherals
        0x0000-0x0FFF without FCTL 0x0140-0x014F, SYS 0x0180-0x019F and DMA
        0x0500-0x053F."""
        writable = ((0x0000, 2), (0x0000, 0x0140), (0x013E, 2), (0x013F, 1), (0x0150, 2),
                    (0x017E, 2), (0x01A0, 2), (0x04FE, 2), (0x0540, 2), (0x05C0, 2),
                    (0x0FFE, 2), (0x1C00, 2), (0x43FE, 2))
        refused = ((0x013E, 4), (0x0140, 2), (0x014E, 2), (0x014F, 1), (0x014E, 4),
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
