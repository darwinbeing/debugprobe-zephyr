import array
import os
import struct
import sys
import types
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
        with self.assertRaisesRegex(StatusError, "DISCONNECT failed: BAD_STATE"):
            self.probe.disconnect()

    def test_read_mem_never_asks_for_a_64_byte_reply(self):
        self.fw.mem = {0x4400 + i: i for i in range(62)}
        self.probe.connect(250, probe_mod.ENTRY_NORMAL)
        self.probe.info()
        self.probe.configure(250)
        self.assertEqual(self.probe.read_mem(0x4400, 31), bytes(range(62)))
        self.assertEqual(self.fw.read_chunks, [30, 1])

    def test_connect_sends_speed_and_mode(self):
        self.probe.connect(250, probe_mod.ENTRY_NORMAL)
        self.assertEqual(self.fw.speeds, [250])
        self.assertEqual(self.fw.connects, [0])
        self.probe.disconnect()
        self.probe.connect(500, probe_mod.ENTRY_MAGIC_PATTERN)
        self.assertEqual((self.fw.speeds, self.fw.connects), ([250, 500], [0, 1]))

    def test_connect_rejects_bad_speed_and_mode(self):
        for speed, mode in ((50, 0), (1001, 0), (250, 2)):
            with self.assertRaisesRegex(StatusError, "CONNECT failed: BAD_ARG"):
                self.probe.connect(speed, mode)
        self.assertEqual(self.fw.connects, [])

    def test_write_mem_round_trip_obeys_the_64_byte_rule(self):
        self.probe.info()
        self.probe.configure(250)
        self.probe.connect(250, probe_mod.ENTRY_NORMAL)
        data = bytes((i * 7 + 3) & 0xFF for i in range(600))
        self.probe.write_mem(0x2400, data)
        self.assertEqual(self.probe.read_mem(0x2400, 300), data)
        self.assertEqual(sum(self.fw.write_chunks), 300)

    def test_write_mem_splits_a_chunk_that_would_fill_whole_packets(self):
        self.probe.info()
        self.probe.configure(250)
        self.probe.connect(250, probe_mod.ENTRY_NORMAL)
        self.probe.write_mem(0x2400, b"\x11\x22" * 29)  # 6 + 58 = 64 bytes
        self.assertEqual(self.fw.write_chunks, [28, 1])

    def test_write_mem_rejects_odd_or_empty_data(self):
        self.probe.connect(250, probe_mod.ENTRY_NORMAL)
        for data in (b"", b"\x01", b"\x01\x02\x03"):
            with self.assertRaises(ProbeError):
                self.probe.write_mem(0x2400, data)
        self.assertEqual(self.fw.write_chunks, [])


class FakeFirmwareTest(unittest.TestCase):
    def test_fixed_size_requests_must_be_exact(self):
        fw = FakeFirmware()
        self.assertEqual(fw.transfer(bytes([0x90, 0x00, 0x00])), bytes([0x90, 1]))        # INFO + 1 byte
        self.assertEqual(fw.transfer(bytes([0x90, 0x01, 0xFA, 0x00])), bytes([0x90, 1]))  # CONNECT, 2 bytes
        self.assertEqual(fw.connects, [])
        self.assertEqual(fw.transfer(bytes([0x90, 0x02, 0x00])), bytes([0x90, 2]))        # idle: state first


class FakeFirmwareRulesTest(unittest.TestCase):
    """The fake applies the firmware's argument rules and fault injection."""

    def cmd(self, fw, op, args=b""):
        return fw.transfer(bytes([0x90, op]) + args)

    def connected(self):
        fw = FakeFirmware()
        SbwProbe(fw).connect(250, 0)
        return fw

    def test_argument_rules(self):
        fw = self.connected()
        pk = struct.pack
        cases = [
            (3, pk("<IH", 0x4400, 0)), (3, pk("<IH", 0x4401, 1)),
            (3, pk("<IH", 0xFFFFE, 2)), (3, pk("<IH", 0x4400, 509)),
            (5, pk("<BII", 3, 0x4400, 0x2400)), (5, pk("<BII", 0, 0x4401, 0x2400)),
            (5, pk("<BII", 0, 0x100000, 0x2400)), (5, pk("<BII", 0, 0x4400, 0x2401)),
            (5, pk("<BII", 0, 0x4400, 0xFFF80)),
            (6, pk("<III", 0x4400, 0, 0x2400)), (6, pk("<III", 0x4401, 1, 0x2400)),
            (6, pk("<III", 0xFFFFE, 2, 0x2400)), (6, pk("<III", 0x4400, 1, 0x2401)),
            (6, pk("<III", 0x4400, 1, 0xFFF80)),
        ]
        for op, args in cases:
            self.assertEqual(self.cmd(fw, op, args), bytes([0x90, 1]), (op, args))
        self.assertEqual(fw.ops, [])

    def test_oversize_flash_data_abandons_the_write(self):
        fw = self.connected()
        self.cmd(fw, 6, struct.pack("<III", 0x4400, 600, 0x2400))
        self.assertEqual(self.cmd(fw, 7, b"\0" * 1018), bytes([0x90, 1]))
        self.assertEqual(fw.state, 1)

    def test_fail_op_after_the_checks(self):
        fw = self.connected()
        fw.fail_op = {3: 7}
        self.assertEqual(self.cmd(fw, 3, struct.pack("<IH", 0x4400, 0)), bytes([0x90, 1]))
        self.assertEqual(self.cmd(fw, 3, struct.pack("<IH", 0x4400, 1)), bytes([0x90, 7]))
        self.assertEqual(fw.state, 1)
        fw.fail_op = {3: 6}
        self.assertEqual(self.cmd(fw, 3, struct.pack("<IH", 0x4400, 1)), bytes([0x90, 6]))
        self.assertEqual(fw.state, 0)

    def test_interrupt_fires_once(self):
        fw = self.connected()
        fw.interrupt_on_op = 3
        with self.assertRaises(KeyboardInterrupt):
            self.cmd(fw, 3, struct.pack("<IH", 0x4400, 1))
        self.assertEqual(self.cmd(fw, 3, struct.pack("<IH", 0x4400, 1))[:2], bytes([0x90, 0]))

    def test_write_mem_rules(self):
        fw = FakeFirmware()
        pk = struct.pack
        ok = pk("<I", 0x2400) + b"\x01\x02"
        self.assertEqual(self.cmd(fw, 4, ok), bytes([0x90, 2]))  # not connected
        SbwProbe(fw).connect(250, 0)
        for args in (b"\x00\x24", pk("<I", 0x2400), pk("<I", 0x2400) + b"\x01",
                     pk("<I", 0x2401) + b"\x01\x02", pk("<I", 0xFFFFE) + b"\0" * 4,
                     pk("<I", 0x2400) + b"\0" * 1020):
            self.assertEqual(self.cmd(fw, 4, args), bytes([0x90, 1]), args)
        self.assertEqual(fw.write_chunks, [])
        self.assertEqual(self.cmd(fw, 4, ok), bytes([0x90, 0]))
        self.assertEqual(fw.write_chunks, [1])
        fw.fail_op = {4: 7}
        self.assertEqual(self.cmd(fw, 4, ok), bytes([0x90, 7]))

    def test_write_mem_to_flash_is_accepted_but_ignored(self):
        fw = self.connected()
        args = struct.pack("<I", 0x4400) + b"\x12\x34"
        self.assertEqual(self.cmd(fw, 4, args), bytes([0x90, 0]))
        self.assertEqual(self.cmd(fw, 3, struct.pack("<IH", 0x4400, 1)), bytes([0x90, 0, 0xFF, 0xFF]))
        self.cmd(fw, 4, struct.pack("<I", 0x2400) + b"\x12\x34")
        self.assertEqual(self.cmd(fw, 3, struct.pack("<IH", 0x2400, 1)), bytes([0x90, 0, 0x12, 0x34]))


class StubTransport:
    def __init__(self, reply):
        self.reply = reply

    def transfer(self, request, timeout_ms=3000):
        if isinstance(self.reply, Exception):
            raise self.reply
        return self.reply


class ReplyValidationTest(unittest.TestCase):
    OUT_OF_STEP = "out of step"

    def test_dap_disconnect_checks_the_reply(self):
        for reply in (b"\x90\x00", b""):
            with self.assertRaisesRegex(ProbeError, self.OUT_OF_STEP):
                SbwProbe(StubTransport(reply)).dap_disconnect()
        SbwProbe(StubTransport(b"\x03\x00")).dap_disconnect()

    def test_reply_with_another_id_is_out_of_step(self):
        for reply in (b"\x03\x00", b"", b"\x05\x00\x00", b"\x03"):
            with self.assertRaisesRegex(ProbeError, self.OUT_OF_STEP):
                SbwProbe(StubTransport(reply)).info()

    def test_bare_echo_means_no_handler(self):
        with self.assertRaisesRegex(ProbeError, "CONFIG_PROBE_MSP430_SBW"):
            SbwProbe(StubTransport(b"\x90")).info()

    def test_transport_errors_name_the_operation(self):
        probe = SbwProbe(StubTransport(ProbeError("no answer from the probe within 3000 ms")))
        with self.assertRaisesRegex(ProbeError, "^INFO: no answer"):
            probe.info()
        with self.assertRaisesRegex(ProbeError, "^READ_MEM: no answer"):
            probe.read_mem(0x4400, 1)

    def test_short_info_payload(self):
        probe = SbwProbe(StubTransport(b"\x90\x00\x01\x00"))
        with self.assertRaisesRegex(ProbeError, "INFO"):
            probe.info()

    def test_short_read_payload_names_read_mem(self):
        probe = SbwProbe(StubTransport(b"\x90\x00" + b"\x11" * 3))
        with self.assertRaisesRegex(ProbeError, r"READ_MEM: expected 4 bytes at 0x04400, got 3"):
            probe.read_mem(0x4400, 2)

    def test_out_of_step_advice_starts_with_running_again(self):
        with self.assertRaisesRegex(ProbeError, "run the command again, and if it keeps "
                                                "happening unplug the probe"):
            SbwProbe(StubTransport(b"\x03\x00")).info()

    def test_short_connect_reply_is_out_of_step(self):
        probe = SbwProbe(StubTransport(b"\x90\x00\x91\x06\x01"))
        with self.assertRaisesRegex(ProbeError, "out of step"):
            probe.connect(250, 0)


class _USBError(IOError):
    pass


class _USBTimeoutError(_USBError):
    pass


class _NoBackendError(ValueError):
    pass


class _Endpoint:
    def __init__(self, address):
        self.bEndpointAddress = address


class _StubDevice:
    def __init__(self, serial="A1", serial_error=None, stale=()):
        self._serial, self._serial_error = serial, serial_error
        self.queue = list(stale)
        self.read_error = None
        self.write_error = None
        self.written = []
        self.read_timeouts = []

    @property
    def serial_number(self):
        if self._serial_error:
            raise self._serial_error
        return self._serial

    def get_active_configuration(self):
        return {(0, 0): [_Endpoint(0x01), _Endpoint(0x81)]}

    def is_kernel_driver_active(self, n):
        return False

    def write(self, ep, data, timeout=None):
        if self.write_error:
            raise self.write_error
        self.written.append((ep, bytes(data)))

    def read(self, ep, size, timeout=None):
        self.read_timeouts.append(timeout)
        if self.read_error:
            raise self.read_error
        if self.queue:
            return self.queue.pop(0)
        raise _USBTimeoutError("timed out")


_MISSING = object()


class UsbTransportTest(unittest.TestCase):
    NAMES = ("usb", "usb.core", "usb.util")

    def setUp(self):
        self._saved = {n: sys.modules.get(n, _MISSING) for n in self.NAMES}
        self.devices = []
        self.find_error = None
        self.claimed = []
        usb = types.ModuleType("usb")
        core = types.ModuleType("usb.core")
        util = types.ModuleType("usb.util")
        core.USBError, core.USBTimeoutError, core.NoBackendError = (
            _USBError, _USBTimeoutError, _NoBackendError)
        core.find = self._find
        util.claim_interface = lambda dev, n: self.claimed.append(dev)
        util.release_interface = lambda dev, n: None
        util.dispose_resources = lambda dev: None
        usb.core, usb.util = core, util
        sys.modules.update({"usb": usb, "usb.core": core, "usb.util": util})

    def tearDown(self):
        for name, mod in self._saved.items():
            if mod is _MISSING:
                sys.modules.pop(name, None)
            else:
                sys.modules[name] = mod

    def _find(self, find_all=False, idVendor=None, idProduct=None):
        if self.find_error:
            raise self.find_error
        return list(self.devices)

    def test_finds_the_probe_and_its_endpoints(self):
        dev = _StubDevice()
        self.devices = [dev]
        t = probe_mod.UsbTransport()
        self.assertEqual((t._out, t._in), (0x01, 0x81))
        self.assertEqual(self.claimed, [dev])

    def test_drains_stale_replies_before_the_first_transfer(self):
        dev = _StubDevice(stale=[b"\x90\x00", b"\x03\x00"])
        self.devices = [dev]
        t = probe_mod.UsbTransport()
        self.assertEqual(dev.queue, [])
        self.assertEqual(dev.read_timeouts, [20, 20, 20])
        dev.queue.append(b"\x90\x00\x07")
        self.assertEqual(t.transfer(b"\x90\x00"), b"\x90\x00\x07")
        self.assertEqual(dev.written, [(0x01, b"\x90\x00")])

    def test_read_timeout_becomes_no_answer(self):
        self.devices = [_StubDevice()]
        t = probe_mod.UsbTransport()
        with self.assertRaisesRegex(ProbeError, "no answer from the probe within 500 ms"):
            t.transfer(b"\x90\x00", timeout_ms=500)

    def test_two_probes_need_a_serial(self):
        self.devices = [_StubDevice("A1"), _StubDevice("B2")]
        with self.assertRaisesRegex(ProbeError, "several probes.*A1.*B2"):
            probe_mod.UsbTransport()
        t = probe_mod.UsbTransport("B2")
        self.assertIs(t._dev, self.devices[1])

    def test_unreadable_serial_is_listed_as_question_mark(self):
        self.devices = [_StubDevice("A1"), _StubDevice(serial_error=ValueError("no langid"))]
        with self.assertRaisesRegex(ProbeError, r"A1, \?"):
            probe_mod.UsbTransport()
        self.devices = [_StubDevice(serial_error=_USBError("denied")),
                        _StubDevice(serial_error=NotImplementedError())]
        with self.assertRaisesRegex(ProbeError, "not found"):
            probe_mod.UsbTransport("A1")

    def test_claim_failure_mentions_another_tool(self):
        def refuse(dev, n):
            raise _USBError("busy")
        sys.modules["usb.util"].claim_interface = refuse
        self.devices = [_StubDevice()]
        with self.assertRaisesRegex(ProbeError, "another tool"):
            probe_mod.UsbTransport()

    def test_usb_error_during_transfer(self):
        dev = _StubDevice()
        self.devices = [dev]
        t = probe_mod.UsbTransport()
        dev.write_error = _USBError("pipe error")
        with self.assertRaisesRegex(ProbeError, "USB error"):
            t.transfer(b"\x90\x00")
        dev.write_error = None
        dev.read_error = _USBError("overflow")
        with self.assertRaisesRegex(ProbeError, "USB error"):
            t.transfer(b"\x90\x00")

    def test_usb_error_during_drain(self):
        dev = _StubDevice()
        dev.read_error = _USBError("io error")
        self.devices = [dev]
        with self.assertRaisesRegex(ProbeError, "USB error"):
            probe_mod.UsbTransport()

    def test_close_swallows_usb_errors(self):
        self.devices = [_StubDevice()]
        t = probe_mod.UsbTransport()

        def fail(*args):
            raise _USBError("gone")
        sys.modules["usb.util"].release_interface = fail
        sys.modules["usb.util"].dispose_resources = fail
        t.close()

    def test_array_reply_becomes_bytes(self):
        dev = _StubDevice()
        self.devices = [dev]
        t = probe_mod.UsbTransport()
        dev.queue.append(array.array("B", [0x90, 0, 7]))
        reply = t.transfer(b"\x90\x00")
        self.assertIsInstance(reply, bytes)
        self.assertEqual(reply, b"\x90\x00\x07")

    def test_no_device(self):
        with self.assertRaisesRegex(ProbeError, "not found"):
            probe_mod.UsbTransport()

    def test_no_libusb(self):
        self.find_error = _NoBackendError()
        with self.assertRaisesRegex(ProbeError, "libusb was not found"):
            probe_mod.UsbTransport()

    def test_no_pyusb(self):
        sys.modules["usb"] = None
        with self.assertRaisesRegex(ProbeError, "pyusb is not installed"):
            probe_mod.UsbTransport()


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
