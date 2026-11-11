import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.dirname(__file__))

import devices  # noqa: E402
import probe as probe_mod  # noqa: E402
from fake_firmware import FakeFirmware  # noqa: E402
from probe import ProbeError, SbwProbe, StatusError, chunks  # noqa: E402


class ChunksTest(unittest.TestCase):
    def test_sums_limits_and_never_a_multiple_of_64(self):
        for header in (2, 6):
            for limit in (1, 16, 31, 32, 93, 508):
                for nwords in range(1, 700):
                    parts = list(chunks(nwords, header, limit))
                    self.assertEqual(sum(parts), nwords)
                    for n in parts:
                        self.assertTrue(1 <= n <= limit)
                        self.assertNotEqual((header + 2 * n) % 64, 0, (header, limit, nwords, n))

    def test_31_words_with_a_2_byte_header_is_split(self):
        self.assertEqual(list(chunks(31, 2, 100)), [30, 1])


class SbwProbeTest(unittest.TestCase):
    def setUp(self):
        self.fw = FakeFirmware()
        self.probe = SbwProbe(self.fw)

    def test_info(self):
        info = self.probe.info()
        self.assertEqual((info.version, info.state, info.max_data_bytes, info.features),
                         (1, 0, 1016, 1))
        self.assertEqual(self.probe.max_words, 508)

    def test_configure_scales_chunks_with_speed(self):
        self.probe.info()
        self.probe.configure(250)
        self.assertEqual((self.probe.read_words, self.probe.write_words), (312, 93))
        self.probe.configure(1000)
        self.assertEqual((self.probe.read_words, self.probe.write_words), (508, 375))

    def test_connect_returns_identity(self):
        ident = self.probe.connect(250, probe_mod.ENTRY_NORMAL)
        self.assertEqual((ident.jtag_id, ident.core_id, ident.device_id_ptr, ident.device_id),
                         (0x91, 0x0106, 0x1A00, 0x2955))

    def test_no_target_carries_the_jtag_id(self):
        self.fw.no_target = {0: 1}
        with self.assertRaises(StatusError) as cm:
            self.probe.connect(250, probe_mod.ENTRY_NORMAL)
        self.assertEqual(cm.exception.status, probe_mod.STATUS_NO_TARGET)
        self.assertEqual(cm.exception.data, b"\xFF")

    def test_firmware_without_the_handler(self):
        self.fw.has_handler = False
        with self.assertRaisesRegex(ProbeError, "CONFIG_PROBE_MSP430_SBW"):
            self.probe.info()

    def test_read_mem_is_chunked_and_reassembled(self):
        self.fw.mem = {0x4400 + i: i & 0xFF for i in range(2000)}
        self.probe.connect(250, probe_mod.ENTRY_NORMAL)
        self.probe.info()
        self.probe.configure(250)
        data = self.probe.read_mem(0x4400, 1000)
        self.assertEqual(data, bytes(i & 0xFF for i in range(2000)))

    def test_status_error_message_names_the_status(self):
        with self.assertRaisesRegex(StatusError, "BAD_STATE"):
            self.probe.disconnect()


class FakeFirmwareTest(unittest.TestCase):
    def test_fixed_size_requests_must_be_exact(self):
        fw = FakeFirmware()
        self.assertEqual(fw.transfer(bytes([0x90, 0x00, 0x00])), bytes([0x90, 1]))        # INFO + 1 byte
        self.assertEqual(fw.transfer(bytes([0x90, 0x01, 0xFA, 0x00])), bytes([0x90, 1]))  # CONNECT, 2 bytes
        self.assertEqual(fw.connects, [])
        self.assertEqual(fw.transfer(bytes([0x90, 0x02, 0x00])), bytes([0x90, 2]))        # idle: state first


class DeviceTableTest(unittest.TestCase):
    def test_f5529_regions(self):
        d = devices.lookup(0x2955)
        self.assertEqual(d.name, "MSP430F5529")
        self.assertEqual(d.region(0x4400), (0x4400, 0x24400))
        self.assertEqual(d.region(0x243FF), (0x4400, 0x24400))
        self.assertIsNone(d.region(0x24400))
        self.assertEqual(d.region(0x1900), (0x1900, 0x1980))
        self.assertIsNone(d.region(0x1980))  # Info A
        self.assertIsNone(d.region(0x1000))  # BSL
        self.assertIsNone(d.region(0x2400))  # RAM

    def test_erase_base(self):
        d = devices.MSP430F5529
        self.assertEqual(d.erase_base(0x4400), 0x4400)
        self.assertEqual(d.erase_base(0x45FF), 0x4400)
        self.assertEqual(d.erase_base(0x4600), 0x4600)
        self.assertEqual(d.erase_base(0x18FF), 0x1880)

    def test_unknown_device(self):
        self.assertIsNone(devices.lookup(0x1234))


if __name__ == "__main__":
    unittest.main()
