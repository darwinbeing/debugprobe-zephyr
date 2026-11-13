import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.dirname(__file__))

import devices  # noqa: E402
import probe as probe_mod  # noqa: E402
from fake_firmware import FakeFirmware  # noqa: E402
from probe import ProbeError, SbwProbe, StatusError  # noqa: E402

OK, BAD_ARG, BAD_STATE, NO_TARGET, TARGET_ERROR, TIMEOUT, UNKNOWN_OP = 0, 1, 2, 4, 6, 7, 8
PROTECTED, RUNNING = 9, 10
HALTED, RUNNING_STATE = 3, 4


def reply(status):
    return bytes([0x90, status])


def attach_args(mode=0, flags=0, lo=0x1C00, hi=0x4400, speed=250):
    return struct.pack("<BBHII", mode, flags, speed, lo, hi)


class Stub:
    """A transport that answers every request with `reply` and keeps the requests."""

    def __init__(self, reply):
        self.reply = reply
        self.sent = []

    def transfer(self, request, timeout_ms=3000):
        self.sent.append(bytes(request))
        return self.reply


class ReplyDecodingTest(unittest.TestCase):
    """Replies written out byte by byte from the layout in session.cpp."""

    def test_status(self):
        st = SbwProbe(Stub(bytes.fromhex("9000" "01" "02" "05" "3C430200"))).dbg_status()
        self.assertEqual(st, probe_mod.DebugStatus(True, 2, 5, 0x2433C))
        st = SbwProbe(Stub(bytes.fromhex("9000" "00" "00" "FF" "00000000"))).dbg_status()
        self.assertEqual(st, probe_mod.DebugStatus(False, 0, 0xFF, 0))

    def test_attach(self):
        identity = "91" "0601" "001A0000" "5529"
        a = SbwProbe(Stub(bytes.fromhex("9000" + identity + "B4A20100" "01" "08"))).dbg_attach(
            probe_mod.ATTACH_RUNNING, 250, 0x1C00, 0x4400)
        self.assertEqual(a, probe_mod.Attached(probe_mod.Identity(0x91, 0x0106, 0x1A00, 0x2955),
                                               0x1A2B4, probe_mod.STOP_REQUEST, 8))

    def test_halt_step_and_registers(self):
        self.assertEqual(SbwProbe(Stub(bytes.fromhex("9000" "FEFF0100"))).dbg_halt(), 0x1FFFE)
        self.assertEqual(SbwProbe(Stub(bytes.fromhex("9000" "B6A20100"))).dbg_step(), 0x1A2B6)
        regs = [0x10000 * (i % 16) + 0x1234 + i for i in range(16)]
        data = b"\x90\x00" + b"".join(r.to_bytes(4, "little") for r in regs)
        self.assertEqual(SbwProbe(Stub(data)).dbg_read_regs(), regs)

    def test_every_short_reply_is_out_of_step(self):
        """Each fixed-size reply one byte short; the full size decodes."""
        calls = (("dbg_attach", (0, 250, 0x1C00, 0x4400), 15), ("dbg_halt", (), 4),
                 ("dbg_status", (), 7), ("dbg_step", (), 4), ("dbg_read_regs", (), 64),
                 ("connect", (250, 0), 9))
        for name, args, n in calls:
            with self.assertRaisesRegex(ProbeError, "out of step", msg=name):
                getattr(SbwProbe(Stub(b"\x90\x00" + bytes(n - 1))), name)(*args)
            getattr(SbwProbe(Stub(b"\x90\x00" + bytes(n))), name)(*args)

    def test_info_of_a_version_2_probe_needs_nine_bytes(self):
        eight = b"\x90\x00" + struct.pack("<BBHI", 2, 0, 1016, 3)
        with self.assertRaisesRegex(ProbeError, "INFO: expected 9 bytes"):
            SbwProbe(Stub(eight)).info()
        self.assertEqual(SbwProbe(Stub(eight + b"\x08")).info().hw_breakpoints, 8)
        version_1 = b"\x90\x00" + struct.pack("<BBHI", 1, 0, 1016, 1)
        self.assertEqual(SbwProbe(Stub(version_1)).info().hw_breakpoints, 0)


class DebugClientTest(unittest.TestCase):
    def setUp(self):
        self.fw = FakeFirmware()
        self.probe = SbwProbe(self.fw)

    def attach(self, mode=probe_mod.ATTACH_RUNNING):
        return self.probe.dbg_attach(mode, 250, 0x1C00, 0x4400)

    def test_attach_returns_identity_pc_and_breakpoints(self):
        a = self.attach()
        self.assertEqual((a.identity.jtag_id, a.identity.device_id), (0x91, 0x2955))
        self.assertEqual((a.pc, a.stop_reason, a.hw_breakpoints),
                         (0x4520, probe_mod.STOP_REQUEST, 8))
        self.assertEqual(self.fw.attaches, [(0, 0, 250, 0x1C00, 0x4400)])
        self.assertEqual(self.fw.state, probe_mod.STATE_DEBUG_HALTED)

    def test_attach_with_reset(self):
        a = self.attach(probe_mod.ATTACH_RESET)
        self.assertEqual((a.pc, a.stop_reason), (0x441A, probe_mod.STOP_RESET))

    def test_attach_failure_carries_the_status(self):
        self.fw.attach_status = probe_mod.STATUS_NO_TARGET
        with self.assertRaises(StatusError) as cm:
            self.attach()
        self.assertEqual(cm.exception.status, probe_mod.STATUS_NO_TARGET)
        self.assertEqual(cm.exception.data, b"\xFF")

    def test_run_poll_halt(self):
        self.attach()
        self.probe.dbg_resume()
        self.assertEqual(self.probe.dbg_status(), probe_mod.DebugStatus(False, 0, 0xFF, 0))
        self.assertEqual(self.probe.dbg_halt(), 0x4ABC)
        st = self.probe.dbg_status()
        self.assertEqual((st.halted, st.stop_reason, st.pc), (True, probe_mod.STOP_REQUEST, 0x4ABC))

    def test_breakpoint_stop(self):
        self.attach()
        self.probe.dbg_bp_set(2, 0x4600)
        self.probe.dbg_resume()
        self.assertFalse(self.probe.dbg_status().halted)
        st = self.probe.dbg_status()
        self.assertEqual(st, probe_mod.DebugStatus(True, probe_mod.STOP_BREAKPOINT, 2, 0x4600))
        self.probe.dbg_bp_clear(2)
        self.probe.dbg_bp_clear()
        self.assertEqual(self.fw.breakpoints, {})

    def test_registers(self):
        self.attach()
        regs = self.probe.dbg_read_regs()
        self.assertEqual(len(regs), 16)
        self.assertEqual(regs[:3], [0x4520, 0x43F0, 0x0008])
        self.probe.dbg_write_reg(12, 0xA5432)
        self.assertEqual(self.probe.dbg_read_regs()[12], 0xA5432)
        with self.assertRaisesRegex(StatusError, "DBG_WRITE_REG failed: BAD_ARG"):
            self.probe.dbg_write_reg(3, 0)

    def test_step(self):
        self.attach()
        self.assertEqual(self.probe.dbg_step(), 0x4522)

    def test_running_and_protected_statuses_name_themselves(self):
        self.attach()
        with self.assertRaisesRegex(StatusError, "WRITE_MEM failed: PROTECTED"):
            self.probe.write_mem(0x4400, b"\x00\x00")
        self.probe.dbg_resume()
        with self.assertRaisesRegex(StatusError, "DBG_READ_REGS failed: RUNNING"):
            self.probe.dbg_read_regs()

    def test_memory_in_a_debug_session(self):
        self.attach()
        self.probe.write_mem(0x2400, b"\x34\x12")
        self.probe.write_mem(0x0200, b"\x01\x00")
        self.assertEqual(self.probe.read_mem(0x2400, 1), b"\x34\x12")
        self.assertEqual(self.fw.mem[0x0200], 1)

    def test_detach(self):
        self.attach()
        self.probe.dbg_detach()
        self.assertEqual((self.fw.detaches, self.fw.state), ([0], 0))
        with self.assertRaisesRegex(StatusError, "DBG_DETACH failed: BAD_STATE"):
            self.probe.dbg_detach()

    def test_short_replies_are_out_of_step(self):
        self.attach()
        self.fw.transfer = lambda req, timeout_ms=3000: bytes([0x90, 0x00])
        for call in (self.probe.dbg_halt, self.probe.dbg_status, self.probe.dbg_step,
                     self.probe.dbg_read_regs):
            with self.assertRaisesRegex(ProbeError, "out of step"):
                call()

    def test_short_attach_reply_is_out_of_step(self):
        self.fw.transfer = lambda req, timeout_ms=3000: bytes([0x90, 0x00]) + bytes(14)
        with self.assertRaisesRegex(ProbeError, "out of step"):
            self.attach()

    def test_requests_on_the_wire(self):
        sent = []
        transfer = self.fw.transfer
        self.fw.transfer = lambda req, timeout_ms=3000: sent.append(bytes(req)) or transfer(req)
        self.probe.dbg_attach(probe_mod.ATTACH_RESET, 500, 0x2400, 0x4400)
        self.probe.dbg_write_reg(4, 0x12345)
        self.probe.dbg_bp_set(7, 0x24400 - 2)
        self.probe.dbg_bp_clear()
        self.probe.dbg_detach(probe_mod.DETACH_RESET)
        self.assertEqual(sent, [
            bytes([0x90, 0x09, 1, 0, 0xF4, 0x01]) + struct.pack("<II", 0x2400, 0x4400),
            bytes([0x90, 0x10, 4, 0x45, 0x23, 0x01, 0x00]),
            bytes([0x90, 0x11, 7, 0xFE, 0x43, 0x02, 0x00]),
            bytes([0x90, 0x12, 0xFF]),
            bytes([0x90, 0x0A, 1]),
        ])
        self.assertEqual(self.fw.detaches, [1])

    def test_step_timeout_is_a_status_error(self):
        self.attach()
        self.fw.fail_op = {14: TIMEOUT}
        with self.assertRaisesRegex(StatusError, "DBG_STEP failed: TIMEOUT") as cm:
            self.probe.dbg_step()
        self.assertEqual(cm.exception.status, probe_mod.STATUS_TIMEOUT)

    def test_pcs_above_16_bits(self):
        self.fw.regs[0] = 0x1A2B4
        self.assertEqual(self.attach().pc, 0x1A2B4)
        self.assertEqual(self.probe.dbg_step(), 0x1A2B6)
        self.fw.halt_pc = 0x1FFFE
        self.probe.dbg_resume()
        self.assertEqual(self.probe.dbg_halt(), 0x1FFFE)
        self.fw.stop_at, self.fw.run_polls = 0x2433C, 1
        self.probe.dbg_resume()
        self.assertEqual(self.probe.dbg_status().pc, 0x2433C)
        self.assertEqual(self.probe.dbg_read_regs()[0], 0x2433C)

    def test_arguments_are_checked_before_anything_is_sent(self):
        self.attach()
        sent = []
        transfer = self.fw.transfer
        self.fw.transfer = lambda req, timeout_ms=3000: sent.append(bytes(req)) or transfer(req)
        bad = ((self.probe.dbg_write_reg, (16, 0)), (self.probe.dbg_write_reg, (-1, 0)),
               (self.probe.dbg_write_reg, (4, 1 << 20)), (self.probe.dbg_write_reg, (4, -1)),
               (self.probe.dbg_bp_set, (8, 0x4400)), (self.probe.dbg_bp_set, (0xFF, 0x4400)),
               (self.probe.dbg_bp_set, (-1, 0x4400)), (self.probe.dbg_bp_set, (0, 1 << 20)),
               (self.probe.dbg_bp_set, (0, -2)), (self.probe.dbg_bp_clear, (8,)),
               (self.probe.dbg_bp_clear, (0xFE,)), (self.probe.dbg_bp_clear, (-1,)),
               (self.probe.dbg_bp_clear, (0x100,)))
        for call, args in bad:
            with self.assertRaises(ProbeError, msg=(call.__name__, args)) as cm:
                call(*args)
            self.assertIs(type(cm.exception), ProbeError)
            self.assertIn(call.__name__.upper(), str(cm.exception))
        self.assertEqual(sent, [])
        self.probe.dbg_write_reg(15, 0xFFFFF)
        self.probe.dbg_bp_set(7, 0xFFFFE)
        self.probe.dbg_bp_clear(7)
        self.probe.dbg_bp_set(0, 0x4400)
        self.probe.dbg_bp_clear(probe_mod.ALL_BREAKPOINTS)
        self.assertEqual(len(sent), 5)
        self.assertEqual(self.fw.breakpoints, {})

    def test_status_errors_that_mean_something_particular(self):
        self.attach()
        self.fw.fail_op = {14: TIMEOUT, 15: TIMEOUT}
        with self.assertRaisesRegex(StatusError, r"^DBG_STEP failed: TIMEOUT \(the step did not "
                                                 r"complete in time; the probe stopped the CPU\)$"):
            self.probe.dbg_step()
        with self.assertRaisesRegex(StatusError, r"^DBG_WRITE_REG failed: PROTECTED \(read-only "
                                                 r"mode: the PC may only be set inside main "
                                                 r"flash\)$"):
            self.probe.dbg_write_reg(0, 0x2400)
        # the same statuses elsewhere keep their general wording
        with self.assertRaisesRegex(StatusError, r"^DBG_READ_REGS failed: TIMEOUT \(no answer"):
            self.probe.dbg_read_regs()
        with self.assertRaisesRegex(StatusError, r"^WRITE_MEM failed: PROTECTED \(read-only debug"):
            self.probe.write_mem(0x4400, b"\x00\x00")

    def test_info_is_kept(self):
        self.assertIsNone(self.probe.last_info)
        info = self.probe.info()
        self.assertIs(self.probe.last_info, info)
        self.assertTrue(info.features & probe_mod.FEATURE_DEBUG)


class FakeFirmwareDebugRulesTest(unittest.TestCase):
    """The fake follows msp430::Session's debug rules."""

    def cmd(self, fw, op, args=b""):
        return fw.transfer(bytes([0x90, op]) + args)

    def attached(self, lo=0x1C00, hi=0x4400):
        fw = FakeFirmware()
        SbwProbe(fw).dbg_attach(0, 250, lo, hi)
        return fw

    def write(self, fw, addr, nbytes=2):
        return self.cmd(fw, 4, struct.pack("<I", addr) + b"\x5A" * nbytes)

    def write_reg(self, fw, index, value):
        return self.cmd(fw, 16, struct.pack("<BI", index, value))

    def status(self, fw):
        return self.cmd(fw, 13)

    def test_attach_argument_rules(self):
        fw = FakeFirmware()
        pk = struct.pack
        for args in (pk("<BBHII", 2, 0, 250, 0x1C00, 0x4400),
                     pk("<BBHII", 0, 1, 250, 0x1C00, 0x4400),
                     pk("<BBHII", 0, 0, 99, 0x1C00, 0x4400),
                     pk("<BBHII", 0, 0, 250, 0x1C01, 0x4400),
                     pk("<BBHII", 0, 0, 250, 0x4400, 0x1C00),
                     pk("<BBHII", 0, 0, 250, 0, 0x100002),
                     pk("<BBHII", 0, 0, 250, 0x1C00, 0x4400)[:-1]):
            self.assertEqual(self.cmd(fw, 9, args), bytes([0x90, 1]), args)
        self.assertEqual(fw.attaches, [])

    def test_state_running_then_size(self):
        fw = FakeFirmware()
        self.assertEqual(self.cmd(fw, 15), bytes([0x90, 2]))
        fw = self.attached()
        self.cmd(fw, 12)
        self.assertEqual(self.cmd(fw, 15, b"\x00"), bytes([0x90, 10]))
        self.assertEqual(self.cmd(fw, 13, b"\x00"), bytes([0x90, 1]))

    def test_flash_ops_and_disconnect_in_a_debug_session(self):
        fw = self.attached()
        self.assertEqual(self.cmd(fw, 5, struct.pack("<BII", 2, 0x4400, 0x2400)), bytes([0x90, 9]))
        self.assertEqual(self.cmd(fw, 6, struct.pack("<III", 0x4400, 1, 0x2400)), bytes([0x90, 9]))
        self.assertEqual(self.cmd(fw, 2), bytes([0x90, 2]))

    def test_ram_is_narrowed_to_the_device(self):
        fw = self.attached(0x1C00, 0x24400)
        self.assertEqual(self.cmd(fw, 4, struct.pack("<I", 0x43FE) + b"\0\0"), bytes([0x90, 0]))
        self.assertEqual(self.cmd(fw, 4, struct.pack("<I", 0x4400) + b"\0\0"), bytes([0x90, 9]))

    def test_connect_detaches_a_debug_session(self):
        fw = self.attached()
        SbwProbe(fw).connect(250, 0)
        self.assertEqual((fw.detaches, fw.state), ([0], 1))

    def test_lost_target_while_running(self):
        fw = self.attached()
        self.cmd(fw, 12)
        fw.lost_on_poll = True
        self.assertEqual(self.cmd(fw, 13), bytes([0x90, 6]))
        self.assertEqual(fw.state, 0)

    # --- the probe's rules as built (lib/msp430/src/session.cpp) ---

    def test_attach_refused_while_connected_or_flashing(self):
        """BAD_STATE, before the size check; nothing attached, the session kept."""
        fw = FakeFirmware()
        SbwProbe(fw).connect(250, 0)
        for state in (1, 2):
            if state == 2:
                self.assertEqual(self.cmd(fw, 6, struct.pack("<III", 0x4400, 2, 0x2400)), reply(OK))
            self.assertEqual(self.cmd(fw, 9, attach_args()), reply(BAD_STATE))
            self.assertEqual(self.cmd(fw, 9, b"\x00"), reply(BAD_STATE))
            self.assertEqual((fw.state, fw.attaches, fw.detaches), (state, [], []))

    def test_attach_in_a_debug_session_detaches_it_first(self):
        """Detach with Run, then a new session: the new request's RAM window,
        no breakpoints."""
        for mode, pc, reason in ((0, 0x4520, 1), (1, 0x441A, 4)):
            for running in (False, True):
                fw = self.attached()
                self.cmd(fw, 17, struct.pack("<BI", 5, 0x4600))
                if running:
                    self.cmd(fw, 12)
                r = self.cmd(fw, 9, attach_args(mode, lo=0x2400, hi=0x3000))
                self.assertEqual(r[:2], reply(OK))
                self.assertEqual(r[11:], struct.pack("<IBB", pc, reason, 8))
                self.assertEqual((fw.detaches, fw.state, fw.breakpoints), ([0], HALTED, {}))
                self.assertEqual(fw.attaches[1:], [(mode, 0, 250, 0x2400, 0x3000)])
                self.assertEqual(self.status(fw),
                                 reply(OK) + struct.pack("<BBBI", 1, reason, 0xFF, pc))
                self.assertEqual(self.write(fw, 0x2400), reply(OK))
                self.assertEqual(self.write(fw, 0x2FFE), reply(OK))
                self.assertEqual(self.write(fw, 0x1C00), reply(PROTECTED))
                self.assertEqual(self.write(fw, 0x3000), reply(PROTECTED))

    def test_bad_attach_leaves_the_debug_session_untouched(self):
        bad = (attach_args(2), attach_args(flags=1), attach_args(flags=2), attach_args(flags=0x80),
               attach_args(lo=0x1C01), attach_args(lo=0x4400, hi=0x1C00),
               attach_args(lo=0x2400, hi=0x2400), attach_args(hi=0x100002),
               attach_args(lo=0x2400, hi=0x3000, speed=99), attach_args(speed=1001),
               attach_args()[:-1], attach_args() + b"\x00", b"")
        for running in (False, True):
            fw = self.attached()
            self.cmd(fw, 17, struct.pack("<BI", 5, 0x4600))
            if running:
                self.cmd(fw, 12)
            for args in bad:
                self.assertEqual(self.cmd(fw, 9, args), reply(BAD_ARG), args)
            self.assertEqual((fw.state, fw.detaches, len(fw.attaches), fw.breakpoints),
                             (RUNNING_STATE if running else HALTED, [], 1, {5: 0x4600}))
            if running:  # breakpoint 5 still set: the next poll stops there
                fw.run_polls = 1
                self.assertEqual(self.status(fw), reply(OK) + struct.pack("<BBBI", 1, 2, 5, 0x4600))
            self.assertEqual(self.write(fw, 0x1C00), reply(OK))  # the first attach's window
            self.assertEqual(self.write(fw, 0x43FE), reply(OK))

    def test_failing_attach_leaves_idle(self):
        fw = self.attached()
        fw.attach_status = NO_TARGET
        self.assertEqual(self.cmd(fw, 9, attach_args()), bytes([0x90, NO_TARGET, 0xFF]))
        self.assertEqual((fw.detaches, fw.state, len(fw.attaches)), ([0], 0, 2))
        self.assertEqual(self.status(fw), reply(BAD_STATE))
        fw.attach_status = None
        fw.fail_op = {9: TARGET_ERROR}
        self.assertEqual(self.cmd(fw, 9, attach_args()), reply(TARGET_ERROR))
        self.assertEqual(fw.state, 0)

    def test_debug_ops_need_a_debug_session(self):
        ops = ((10, b"\x00"), (11, b""), (12, b""), (13, b""), (14, b""), (15, b""),
               (16, struct.pack("<BI", 4, 0)), (17, struct.pack("<BI", 0, 0x4400)), (18, b"\xFF"))
        fw = FakeFirmware()
        for state in (0, 1, 2):
            if state == 1:
                SbwProbe(fw).connect(250, 0)
            if state == 2:
                self.cmd(fw, 6, struct.pack("<III", 0x4400, 2, 0x2400))
            for op, args in ops:
                self.assertEqual(self.cmd(fw, op, args), reply(BAD_STATE), (state, op))
            self.assertEqual(fw.state, state)
        self.assertEqual(fw.detaches, [])

    def test_state_error_wins_over_size_error(self):
        wrong_size = ((10, b""), (11, b"\0"), (12, b"\0"), (13, b"\0"), (14, b"\0"), (15, b"\0"),
                      (16, b"\x04\x00"), (17, b"\x00\x00\x44"), (18, b""),
                      (3, b"\x00\x24\x00\x00"), (4, b"\x00\x24\x00"))
        fw = FakeFirmware()
        for op, args in wrong_size:
            self.assertEqual(self.cmd(fw, op, args), reply(BAD_STATE), op)
        fw = self.attached()
        self.cmd(fw, 12)
        # running: the operations allowed while running check their size
        for op, args in wrong_size:
            want = BAD_ARG if op in (10, 11, 13) else RUNNING
            self.assertEqual(self.cmd(fw, op, args), reply(want), op)
        self.assertEqual(fw.state, RUNNING_STATE)

    def test_halted_only_ops_answer_running(self):
        fw = self.attached()
        self.cmd(fw, 12)
        for op, args in ((12, b""), (14, b""), (15, b""), (16, struct.pack("<BI", 4, 0)),
                         (17, struct.pack("<BI", 0, 0x4400)), (18, b"\xFF"),
                         (3, struct.pack("<IH", 0x2400, 1)),
                         (4, struct.pack("<I", 0x2400) + b"\0\0"),
                         (4, struct.pack("<I", 0x4400) + b"\0\0")):  # RUNNING before PROTECTED
            self.assertEqual(self.cmd(fw, op, args), reply(RUNNING), op)
        self.assertEqual((fw.state, fw.reg_writes, fw.breakpoints, fw.write_chunks),
                         (RUNNING_STATE, [], {}, []))

    def test_info_reports_the_debug_states(self):
        fw = self.attached()
        self.assertEqual(self.cmd(fw, 0)[2:], struct.pack("<BBHIB", 2, HALTED, 1016, 3, 8))
        self.cmd(fw, 12)
        self.assertEqual(self.cmd(fw, 0)[3], RUNNING_STATE)

    def test_write_mem_whitelist(self):
        """session_dbg_write_mem_whitelist in test_msp430_session_debug.cpp."""
        fw = self.attached()
        for addr, n in ((0x2400, 4), (0x1C00, 2), (0x43FE, 2), (0x0200, 2), (0x0000, 2),
                        (0x0FFE, 2), (0x011E, 2), (0x0150, 2), (0x017E, 2), (0x01A0, 2),
                        (0x04FE, 2), (0x0540, 2), (0x05C0, 2), (0x05FE, 2), (0x0600, 2),
                        (0x1C00, 1016)):
            self.assertEqual(self.write(fw, addr, n), reply(OK), hex(addr))
        mem, chunks = dict(fw.mem), list(fw.write_chunks)
        for addr in (0x4400, 0x4402, 0xFFFE, 0x10000, 0x1FFFE, 0x243FE, 0x24400, 0xFFFFE,
                     0x1800, 0x1980, 0x19FE, 0x1A00, 0x1B00, 0x1BFE, 0x1000, 0x17FE,
                     0x1C00 - 0x200, 0x0120, 0x0122, 0x0124, 0x012C, 0x0130, 0x013E, 0x0140,
                     0x0142, 0x0144, 0x0146, 0x0148, 0x014E, 0x0180, 0x0182, 0x0186, 0x019E,
                     0x0500, 0x0510, 0x053A, 0x053E):
            self.assertEqual(self.write(fw, addr), reply(PROTECTED), hex(addr))
        for addr, n in ((0x43FE, 4), (0x4010, 1016), (0x011E, 4), (0x0110, 0x20), (0x013E, 4),
                        (0x014E, 4), (0x0100, 0x50), (0x017E, 4), (0x019E, 4), (0x0170, 0x40),
                        (0x04FE, 4), (0x053E, 4), (0x0400, 0x300), (0x0FFE, 4), (0x0FF0, 0x3F8),
                        (0x1BFE, 4), (0x1BFE, 0x100)):
            self.assertEqual(self.write(fw, addr, n), reply(PROTECTED), (hex(addr), n))
        self.assertEqual((fw.mem, fw.write_chunks), (mem, chunks))

    def test_write_mem_whitelist_is_the_device_tables(self):
        """Word by word up to main flash: the fake writes exactly what
        devices.py's debug_writable allows (RAM, peripherals minus the holes)."""
        fw = self.attached()
        d = devices.MSP430F5529
        for addr in range(0, d.main_start + 2, 2):
            want = reply(OK) if d.debug_writable(addr, 2) else reply(PROTECTED)
            self.assertEqual(self.write(fw, addr), want, hex(addr))
        for lo, hi, name in d.peripheral_holes:
            self.assertEqual(self.write(fw, lo), reply(PROTECTED), name)
            self.assertEqual(self.write(fw, hi - 2), reply(PROTECTED), name)

    def test_write_mem_malformed_is_bad_arg(self):
        """Form first (BAD_ARG), then the whitelist (PROTECTED): a range that
        leaves the 20-bit space is outside every window."""
        fw = self.attached()
        for addr, n in ((0x2401, 2), (0x4401, 2), (0x0141, 2), (0x2400, 3), (0x2400, 1),
                        (0x4400, 1), (0x2400, 0), (0x1C00, 1020)):  # more than 1016
            self.assertEqual(self.write(fw, addr, n), reply(BAD_ARG), (hex(addr), n))
        self.assertEqual(self.cmd(fw, 4, b"\x00\x24\x00"), reply(BAD_ARG))
        for addr, n in ((0xFFFFE, 4), (0xFFFFFFFE, 2), (0x100000, 2)):
            self.assertEqual(self.write(fw, addr, n), reply(PROTECTED), hex(addr))
        self.assertEqual(fw.write_chunks, [])

    def test_ram_window_is_the_intersection_with_the_device(self):
        cases = (
            ((0x2400, 0x3000), {0x2400: OK, 0x2FFE: OK, 0x1C00: PROTECTED, 0x23FE: PROTECTED,
                                0x3000: PROTECTED, 0x0200: OK}),
            ((0x1000, 0x4400), {0x1000: PROTECTED, 0x1800: PROTECTED, 0x1BFE: PROTECTED,
                                0x1C00: OK}),
            ((0x0000, 0x100000), {0x1BFE: PROTECTED, 0x1C00: OK, 0x43FE: OK, 0x4400: PROTECTED,
                                  0x0140: PROTECTED}),
            ((0x4400, 0x24400), {0x4400: PROTECTED, 0x2400: PROTECTED, 0x0200: OK}),
        )
        for (lo, hi), want in cases:
            fw = self.attached(lo, hi)
            for addr, status in want.items():
                self.assertEqual(self.write(fw, addr), reply(status), (hex(lo), hex(addr)))
        fw = self.attached(0x2400, 0x3000)
        self.assertEqual(self.write(fw, 0x2FFE, 4), reply(PROTECTED))

    def test_unknown_device_writes_nothing(self):
        fw = FakeFirmware(device_id=0x1234)
        SbwProbe(fw).dbg_attach(0, 250, 0x1C00, 0x4400)
        for addr in (0x2400, 0x1C00, 0x43FE, 0x4400, 0x0000, 0x0200, 0x013E, 0x0140, 0x0600,
                     0x0FFE):
            self.assertEqual(self.write(fw, addr), reply(PROTECTED), hex(addr))
        self.assertEqual(fw.write_chunks, [])

    def test_window_is_set_anew_by_every_attach(self):
        fw = self.attached()
        self.assertEqual(self.write(fw, 0x1C00), reply(OK))
        self.assertEqual(self.write_reg(fw, 0, 0x4400), reply(OK))
        fw.device_id = 0x1234  # the next attach identifies a device the probe does not know
        self.assertEqual(self.cmd(fw, 9, attach_args())[:2], reply(OK))
        self.assertEqual(self.write(fw, 0x1C00), reply(PROTECTED))
        self.assertEqual(self.write_reg(fw, 0, 0x4402), reply(PROTECTED))

    def test_write_reg_moves_the_pc_only_into_main_flash(self):
        fw = self.attached()
        for pc in (0x1000, 0x1800, 0x1C00, 0x2400, 0x43FE, 0x24400, 0x0004, 0x0000, 0x0FFE,
                   0xFFFFE, 0x100000, 0xFFFFFFFE):
            self.assertEqual(self.write_reg(fw, 0, pc), reply(PROTECTED), hex(pc))
        self.assertEqual(self.write_reg(fw, 0, 0x2401), reply(BAD_ARG))  # odd: the form first
        self.assertEqual(self.write_reg(fw, 0, 0x4401), reply(BAD_ARG))
        self.assertEqual(fw.regs[0], 0x4520)
        self.assertEqual(self.write_reg(fw, 0, 0x4520), reply(OK))  # unchanged
        self.assertEqual(self.write_reg(fw, 0, 0x4400), reply(OK))
        self.assertEqual(self.write_reg(fw, 0, 0x243FE), reply(OK))
        self.assertEqual(self.status(fw), reply(OK) + struct.pack("<BBBI", 1, 1, 0xFF, 0x243FE))
        self.assertEqual(self.write_reg(fw, 4, 0x2400), reply(OK))  # other registers: any value
        self.assertEqual(fw.reg_writes, [(0, 0x4520), (0, 0x4400), (0, 0x243FE), (4, 0x2400)])

        fw = FakeFirmware()  # halted in code the firmware runs from RAM
        fw.regs[0] = 0x2400
        SbwProbe(fw).dbg_attach(0, 250, 0x1C00, 0x4400)
        self.assertEqual(self.write_reg(fw, 0, 0x2400), reply(OK))
        self.assertEqual(self.write_reg(fw, 0, 0x2402), reply(PROTECTED))
        self.assertEqual(self.write_reg(fw, 0, 0x4400), reply(OK))  # out into main flash
        self.assertEqual(self.write_reg(fw, 0, 0x2400), reply(PROTECTED))  # not back

        fw = FakeFirmware(device_id=0x1234)  # an unknown device: the PC stays where it is
        SbwProbe(fw).dbg_attach(0, 250, 0x1C00, 0x4400)
        self.assertEqual(self.write_reg(fw, 0, 0x4520), reply(OK))
        for pc in (0x4400, 0x4522, 0x243FE, 0x2400, 0x1000):
            self.assertEqual(self.write_reg(fw, 0, pc), reply(PROTECTED), hex(pc))
        self.assertEqual(fw.regs[0], 0x4520)

    def test_write_reg_current_pc_follows_every_stop(self):
        fw = FakeFirmware()
        fw.regs[0] = 0x2400
        SbwProbe(fw).dbg_attach(0, 250, 0x1C00, 0x4400)
        fw.step_size = 4
        self.assertEqual(self.cmd(fw, 14), reply(OK) + struct.pack("<I", 0x2404))
        self.assertEqual(self.write_reg(fw, 0, 0x2404), reply(OK))
        self.assertEqual(self.write_reg(fw, 0, 0x2400), reply(PROTECTED))
        fw.halt_pc = 0x2408
        self.cmd(fw, 12)
        self.assertEqual(self.cmd(fw, 11), reply(OK) + struct.pack("<I", 0x2408))
        self.assertEqual(self.write_reg(fw, 0, 0x2408), reply(OK))
        self.assertEqual(self.write_reg(fw, 0, 0x2404), reply(PROTECTED))
        fw.stop_at, fw.run_polls = 0x240C, 1
        self.cmd(fw, 12)
        self.assertEqual(self.status(fw), reply(OK) + struct.pack("<BBBI", 1, 2, 0xFF, 0x240C))
        self.assertEqual(self.write_reg(fw, 0, 0x240C), reply(OK))
        self.assertEqual(self.write_reg(fw, 0, 0x2408), reply(PROTECTED))

    def test_write_reg_arguments(self):
        fw = self.attached()
        for index, value in ((3, 0), (16, 0), (0xFF, 0), (4, 0x100000), (2, 0x10000)):
            self.assertEqual(self.write_reg(fw, index, value), reply(BAD_ARG), (index, value))
        self.assertEqual(self.cmd(fw, 16, b"\x04\x00"), reply(BAD_ARG))
        self.assertEqual(self.cmd(fw, 16, struct.pack("<BI", 4, 0) + b"\x00"), reply(BAD_ARG))
        self.assertEqual(self.cmd(fw, 15, b"\x00"), reply(BAD_ARG))
        self.assertEqual(fw.reg_writes, [])
        self.assertEqual(self.write_reg(fw, 1, 0x2FFF), reply(OK))  # SP: bit 0 cleared
        self.assertEqual(self.write_reg(fw, 2, 0xFFFF), reply(OK))
        self.assertEqual(self.write_reg(fw, 15, 0xFFFFF), reply(OK))
        regs = SbwProbe(fw).dbg_read_regs()
        self.assertEqual((regs[1], regs[2], regs[3], regs[15]), (0x2FFE, 0xFFFF, 0, 0xFFFFF))
        self.assertEqual(fw.reg_writes, [(1, 0x2FFF), (2, 0xFFFF), (15, 0xFFFFF)])

    def test_breakpoint_arguments(self):
        fw = self.attached()
        for index, addr in ((8, 0x4400), (0xFF, 0x4400), (0, 0x4401), (0, 0x100000)):
            self.assertEqual(self.cmd(fw, 17, struct.pack("<BI", index, addr)), reply(BAD_ARG))
        self.assertEqual(self.cmd(fw, 17, b"\x00\x00\x44"), reply(BAD_ARG))
        self.assertEqual(self.cmd(fw, 17, struct.pack("<BI", 0, 0x4400) + b"\x00"), reply(BAD_ARG))
        for args in (b"\x08", b"\xFE", b"", b"\x00\x00"):
            self.assertEqual(self.cmd(fw, 18, args), reply(BAD_ARG), args)
        self.assertEqual(fw.breakpoints, {})
        self.assertEqual(self.cmd(fw, 17, struct.pack("<BI", 7, 0x243FE)), reply(OK))
        self.assertEqual(self.cmd(fw, 17, struct.pack("<BI", 7, 0x4400)), reply(OK))  # moved
        self.assertEqual(fw.breakpoints, {7: 0x4400})
        self.assertEqual(self.cmd(fw, 18, b"\x07"), reply(OK))
        self.assertEqual(self.cmd(fw, 18, b"\x07"), reply(OK))  # not set: still OK
        self.assertEqual(self.cmd(fw, 18, b"\xFF"), reply(OK))

    def test_stops(self):
        fw = self.attached()
        for index, addr in ((6, 0x4600), (3, 0x4700), (5, 0x4522)):
            self.cmd(fw, 17, struct.pack("<BI", index, addr))
        # a step reports no breakpoint, even onto a breakpoint's address
        self.assertEqual(self.cmd(fw, 14), reply(OK) + struct.pack("<I", 0x4522))
        self.assertEqual(self.status(fw), reply(OK) + struct.pack("<BBBI", 1, 3, 0xFF, 0x4522))
        # a halt of a halted CPU: OK, the PC, nothing changes
        self.assertEqual(self.cmd(fw, 11), reply(OK) + struct.pack("<I", 0x4522))
        self.assertEqual(self.status(fw), reply(OK) + struct.pack("<BBBI", 1, 3, 0xFF, 0x4522))
        # run_polls polls: the lowest-index breakpoint
        fw.run_polls = 3
        self.cmd(fw, 12)
        running = reply(OK) + struct.pack("<BBBI", 0, 0, 0xFF, 0)
        self.assertEqual([self.status(fw) for _ in range(2)], [running, running])
        self.assertEqual(self.status(fw), reply(OK) + struct.pack("<BBBI", 1, 2, 3, 0x4700))
        self.assertEqual(self.status(fw), reply(OK) + struct.pack("<BBBI", 1, 2, 3, 0x4700))
        # stop_at: where the CPU stops; no breakpoint there
        fw.stop_at = 0x5000
        self.cmd(fw, 12)
        self.assertEqual([self.status(fw) for _ in range(2)], [running, running])
        self.assertEqual(self.status(fw), reply(OK) + struct.pack("<BBBI", 1, 2, 0xFF, 0x5000))
        self.assertEqual(fw.resumes, 2)

    def test_halt_finds_a_breakpoint_stop_first(self):
        """Debugger::halt looks as poll() does before it stops the CPU itself."""
        fw = self.attached()
        self.cmd(fw, 17, struct.pack("<BI", 3, 0x4600))
        self.cmd(fw, 12)
        running = reply(OK) + struct.pack("<BBBI", 0, 0, 0xFF, 0)
        self.assertEqual(self.status(fw), running)  # the first look; the stop is due at the second
        self.assertEqual(self.cmd(fw, 11), reply(OK) + struct.pack("<I", 0x4600))
        self.assertEqual(self.status(fw), reply(OK) + struct.pack("<BBBI", 1, 2, 3, 0x4600))
        self.cmd(fw, 12)  # not due yet: the halt stops the CPU itself
        self.assertEqual(self.cmd(fw, 11), reply(OK) + struct.pack("<I", 0x4ABC))
        self.assertEqual(self.status(fw), reply(OK) + struct.pack("<BBBI", 1, 1, 0xFF, 0x4ABC))

    def test_reads_and_writes_are_recorded(self):
        fw = self.attached()
        SbwProbe(fw).read_mem(0x2400, 2)
        self.assertEqual(self.write(fw, 0x0200, 4), reply(OK))
        self.assertEqual(self.write(fw, 0x4400), reply(PROTECTED))
        self.assertEqual(self.write(fw, 0x2401), reply(BAD_ARG))
        self.assertEqual(self.cmd(fw, 3, struct.pack("<IH", 0x2401, 1)), reply(BAD_ARG))
        self.assertEqual((fw.reads, fw.writes), ([(0x2400, 2)], [(0x0200, b"\x5A" * 4)]))
        self.assertEqual((fw.read_chunks, fw.write_chunks), ([2], [2]))

    def test_a_version_1_probe_knows_no_debug_ops(self):
        fw = FakeFirmware(version=1, features=1)
        self.assertEqual(self.cmd(fw, 9, attach_args()), reply(UNKNOWN_OP))
        for op in range(10, 19):
            self.assertEqual(self.cmd(fw, op), reply(UNKNOWN_OP), op)
            self.assertEqual(self.cmd(fw, op, b"\x00"), reply(UNKNOWN_OP), op)
        self.assertEqual((fw.state, fw.attaches), (0, []))
        with self.assertRaisesRegex(StatusError, "DBG_ATTACH failed: UNKNOWN_OP"):
            SbwProbe(fw).dbg_attach(0, 250, 0x1C00, 0x4400)

    def test_step_timeout_pc(self):
        fw = self.attached()
        fw.fail_op = {14: TIMEOUT}
        fw.step_timeout_pc = 0x4C00
        self.assertEqual(self.cmd(fw, 14), reply(TIMEOUT))
        self.assertEqual(fw.state, HALTED)
        self.assertEqual(self.status(fw), reply(OK) + struct.pack("<BBBI", 1, 1, 0xFF, 0x4C00))
        self.assertEqual(self.write_reg(fw, 0, 0x4C00), reply(OK))  # the current PC

    def test_step_with_the_cpu_off_executes_nothing(self):
        fw = FakeFirmware()
        fw.regs[2] = 0x0018  # LPM0: CPUOFF and GIE
        SbwProbe(fw).dbg_attach(0, 250, 0x1C00, 0x4400)
        self.assertEqual(self.cmd(fw, 14), reply(OK) + struct.pack("<I", 0x4520))
        self.assertEqual(self.status(fw), reply(OK) + struct.pack("<BBBI", 1, 3, 0xFF, 0x4520))
        self.assertEqual(self.write_reg(fw, 2, 0x0008), reply(OK))  # CPUOFF cleared
        self.assertEqual(self.cmd(fw, 14), reply(OK) + struct.pack("<I", 0x4522))
        self.assertEqual(fw.steps, 2)

    def test_step_timeout_leaves_the_cpu_halted(self):
        fw = self.attached()
        self.cmd(fw, 14)
        fw.fail_op = {14: TIMEOUT}
        self.assertEqual(self.cmd(fw, 14), reply(TIMEOUT))
        self.assertEqual(fw.state, HALTED)
        self.assertEqual(self.status(fw), reply(OK) + struct.pack("<BBBI", 1, 1, 0xFF, 0x4522))

    def test_target_error_ends_the_session(self):
        """From a memory, register or breakpoint operation the probe detaches
        with Run (Session::abort); from DBG_HALT, DBG_STATUS and DBG_STEP the
        Debugger has already let the CPU go: Idle, no detach."""
        for op, args, detaches in ((3, struct.pack("<IH", 0x2400, 1), [0]),
                                   (4, struct.pack("<I", 0x2400) + b"\0\0", [0]),
                                   (15, b"", [0]), (16, struct.pack("<BI", 4, 1), [0]),
                                   (17, struct.pack("<BI", 0, 0x4400), [0]), (18, b"\x00", [0]),
                                   (14, b"", [])):
            fw = self.attached()
            fw.fail_op = {op: TARGET_ERROR}
            self.assertEqual(self.cmd(fw, op, args), reply(TARGET_ERROR), op)
            self.assertEqual((fw.state, fw.detaches), (0, detaches), op)
            self.assertEqual(self.status(fw), reply(BAD_STATE))
        fw = self.attached()
        self.cmd(fw, 12)
        fw.lost_on_poll = True
        self.assertEqual(self.cmd(fw, 11), reply(TARGET_ERROR))
        self.assertEqual((fw.state, fw.detaches), (0, []))
        fw = self.attached()  # DBG_DETACH of a lost running target: OK
        self.cmd(fw, 12)
        fw.lost_on_poll = True
        self.assertEqual(self.cmd(fw, 10, b"\x00"), reply(OK))
        self.assertEqual((fw.state, fw.detaches), (0, [0]))

    def test_detach_arguments_and_running(self):
        fw = self.attached()
        for args in (b"\x02", b"", b"\x00\x00"):
            self.assertEqual(self.cmd(fw, 10, args), reply(BAD_ARG), args)
        self.assertEqual((fw.state, fw.detaches), (HALTED, []))
        self.cmd(fw, 12)
        self.assertEqual(self.cmd(fw, 10, b"\x01"), reply(OK))
        self.assertEqual((fw.state, fw.detaches, fw.breakpoints), (0, [1], {}))

    def test_connect_ends_a_running_debug_session_after_its_checks(self):
        fw = self.attached()
        self.cmd(fw, 12)
        self.assertEqual(self.cmd(fw, 1, struct.pack("<HB", 250, 2)), reply(BAD_ARG))
        self.assertEqual(self.cmd(fw, 1, struct.pack("<H", 250)), reply(BAD_ARG))
        self.assertEqual((fw.state, fw.detaches), (RUNNING_STATE, []))
        self.assertEqual(self.cmd(fw, 1, struct.pack("<HB", 250, 0))[:2], reply(OK))
        self.assertEqual((fw.state, fw.detaches), (1, [0]))


if __name__ == "__main__":
    unittest.main()
