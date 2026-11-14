import io
import os
import signal
import socket
import struct
import sys
import threading
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.dirname(__file__))

import devices  # noqa: E402
import gdbserver  # noqa: E402
import probe as probe_mod  # noqa: E402
import rsp  # noqa: E402
from fake_firmware import (BAD_STATE, BUSY, NO_TARGET, TARGET_ERROR, TIMEOUT,  # noqa: E402
                           FakeFirmware)
from probe import ProbeError, SbwProbe  # noqa: E402


class Client:
    """A scripted GDB: sends packets, acknowledges and returns the replies."""

    def __init__(self, sock):
        self.sock = sock
        self.sock.settimeout(5)
        self.reader = rsp.PacketReader()
        self.events = []
        self.raw = []  # every event seen

    def _event(self):
        while not self.events:
            data = self.sock.recv(4096)
            if not data:
                return ("closed", None)
            self.events += self.reader.feed(data)
        ev = self.events.pop(0)
        self.raw.append(ev)
        return ev

    def reply(self):
        """The next packet from the server (acknowledged); O packets are collected."""
        while True:
            kind, payload = self._event()
            if kind == "closed":
                return None
            if kind == "packet":
                try:
                    self.sock.sendall(b"+")
                except OSError:
                    pass  # the server has already closed the connection
                return payload.decode("latin-1")

    def cmd(self, payload):
        if isinstance(payload, str):
            payload = payload.encode("latin-1")
        self.sock.sendall(rsp.frame(payload))
        return self.reply()


class ServerFixture(unittest.TestCase):
    def setUp(self):
        self.fw = FakeFirmware()
        self.out = io.StringIO()
        self.server = gdbserver.GdbServer(SbwProbe(self.fw), devices.MSP430F5529,
                                          out=self.out, poll_interval=0.001, linger=0.05)
        self.server_sock, client_sock = socket.socketpair()
        self.gdb = Client(client_sock)
        self.error = None
        self.thread = threading.Thread(target=self._serve, daemon=True)

    def _serve(self):
        try:
            self.server.serve_connection(self.server_sock)
        except (ProbeError, KeyboardInterrupt) as e:
            self.error = e
        finally:
            self.server_sock.close()

    def start(self):
        self.thread.start()

    def finish(self):
        self.gdb.sock.close()
        self.thread.join(5)
        self.assertFalse(self.thread.is_alive())

    def tearDown(self):
        if self.thread.is_alive():
            self.finish()
        self.gdb.sock.close()


class ConnectionTest(ServerFixture):
    def test_attach_without_reset_and_stop_reply(self):
        self.start()
        self.assertEqual(self.gdb.cmd("qSupported:multiprocess+;swbreak+;hwbreak+"),
                         "PacketSize=1000;qXfer:memory-map:read+;swbreak+;hwbreak+")
        self.assertEqual(self.gdb.cmd("?"), "T0500:20450000;")
        self.assertEqual(self.fw.attaches, [(0, 0, 250, 0x1C00, 0x4400)])
        self.assertIn("halted at 0x04520; read-only mode; 8 hardware breakpoints", self.out.getvalue())

    def test_attach_with_reset(self):
        self.server.attach_mode = probe_mod.ATTACH_RESET
        self.start()
        self.assertEqual(self.gdb.cmd("?"), "T0500:1a440000;")

    def test_unknown_packets_get_an_empty_reply(self):
        self.start()
        self.assertEqual(self.gdb.cmd("qTStatus"), "")
        self.assertEqual(self.gdb.cmd("vMustReplyEmpty"), "")
        self.assertEqual(self.gdb.cmd("Hg0"), "OK")
        self.assertEqual(self.gdb.cmd("qAttached"), "1")

    def test_bad_checksum_gets_a_nack(self):
        self.start()
        self.gdb.sock.sendall(b"$g#00")
        self.assertEqual(self.gdb._event(), ("nack", None))

    def test_attach_failure(self):
        self.fw.attach_status = NO_TARGET
        self.start()
        self.thread.join(5)
        self.assertIn("no MSP430 found (JTAG ID read as 0xFF)", str(self.error))
        self.assertEqual(len(self.fw.attaches), 4)
        self.assertIsNone(self.gdb.reply())  # connection closed

    def test_wrong_device_is_left_running(self):
        self.fw.device_id = 0x1234
        self.start()
        self.thread.join(5)
        self.assertIn("unexpected device", str(self.error))
        self.assertEqual(self.fw.detaches, [0])


class RegisterTest(ServerFixture):
    def test_g_is_sixteen_little_endian_words(self):
        self.start()
        g = self.gdb.cmd("g")
        self.assertEqual(len(g), 128)
        self.assertEqual(list(struct.unpack("<16I", bytes.fromhex(g))), self.fw.regs)

    def test_p_and_P(self):
        self.start()
        self.assertEqual(self.gdb.cmd("p1"), "f0430000")
        self.assertEqual(self.gdb.cmd("Pc=3254" + "0a00"), "OK")
        self.assertEqual(self.fw.regs[12], 0xA5432)
        self.assertEqual(self.gdb.cmd("pc"), "32540a00")
        self.assertEqual(self.gdb.cmd("P3=00000000"), "E01")
        self.assertEqual(self.gdb.cmd("p10"), "E01")

    def test_G_writes_only_what_changed(self):
        self.start()
        regs = list(self.fw.regs)
        regs[5] = 0x1234
        regs[3] = 0x9999  # CG is never written
        self.assertEqual(self.gdb.cmd("G" + struct.pack("<16I", *regs).hex()), "OK")
        self.assertEqual(self.fw.regs[5], 0x1234)
        self.assertEqual(self.fw.regs[3], 0)


class MemoryTest(ServerFixture):
    def test_read_flash_ram_and_unaligned(self):
        self.fw.mem.update({0x4400: 0x11, 0x4401: 0x22, 0x4402: 0x33, 0x2400: 0xAA})
        self.start()
        self.assertEqual(self.gdb.cmd("m4400,3"), "112233")
        self.assertEqual(self.gdb.cmd("m4401,1"), "22")
        self.assertEqual(self.gdb.cmd("m2400,1"), "aa")
        self.assertEqual(self.gdb.cmd("m4400,0"), "")

    def test_bsl_and_vacant_memory_are_never_read(self):
        self.start()
        chunks = list(self.fw.read_chunks)
        for packet in ("m1000,2", "m17f0,10", "mffe,4", "m1b00,2", "m24400,2", "mffffe,4"):
            self.assertEqual(self.gdb.cmd(packet), "E01", packet)
        self.assertEqual(self.fw.read_chunks, chunks)
        self.assertIn("the BSL area 0x1000-0x17FF is never read", self.out.getvalue())

    def test_write_ram_and_peripherals(self):
        self.start()
        self.assertEqual(self.gdb.cmd("M2400,2:3412"), "OK")
        self.assertEqual((self.fw.mem[0x2400], self.fw.mem[0x2401]), (0x34, 0x12))
        self.assertEqual(self.gdb.cmd("M2403,1:77"), "OK")  # odd: read-modify-write
        self.assertEqual(self.fw.mem[0x2403], 0x77)
        self.assertEqual(self.gdb.cmd(b"X2404,2:" + rsp.escape(b"#}")), "OK")
        self.assertEqual((self.fw.mem[0x2404], self.fw.mem[0x2405]), (0x23, 0x7D))
        self.assertEqual(self.gdb.cmd("X2400,0:"), "OK")
        self.assertEqual(self.gdb.cmd("M0200,2:0100"), "OK")

    def test_writes_outside_ram_and_peripherals_are_refused_before_the_probe(self):
        self.start()
        writes = list(self.fw.write_chunks)
        for packet in ("M4400,2:0000", "M1800,2:0000", "M1000,2:0000", "M0140,2:0000",
                       "M43fe,4:00000000", "Mfffe,2:0000", "X4400,2:\x00\x00",
                       "M0000,2:0000", "M00fe,4:00000000", "M0940,2:0000", "M0ffe,2:0000"):
            self.assertEqual(self.gdb.cmd(packet), "E01", packet)
        self.assertEqual(self.fw.write_chunks, writes)
        self.assertIn("read-only mode writes only RAM and peripheral registers", self.out.getvalue())

    def test_flash_programming_is_refused(self):
        self.start()
        self.assertEqual(self.gdb.cmd("vFlashErase:4400,200"), "E01")
        self.assertEqual(self.gdb.cmd("vFlashWrite:4400:\x00\x00"), "E01")
        self.assertEqual(self.gdb.cmd("vFlashDone"), "E01")
        self.assertNotIn(("erase", 0, 0x4400, 0x2400), self.fw.ops)

    def test_memory_map(self):
        self.start()
        whole = self.gdb.cmd("qXfer:memory-map:read::0,1000")
        self.assertTrue(whole.startswith("l<?xml"))
        self.assertIn('<memory type="ram" start="0x1c00" length="0x2800"/>', whole)
        self.assertIn('<memory type="rom" start="0x4400" length="0x20000"/>', whole)
        self.assertNotIn('start="0x1000"', whole)
        first = self.gdb.cmd("qXfer:memory-map:read::0,20")
        self.assertEqual(first[0], "m")
        self.assertEqual(first[1:], whole[1:0x21])


class RunControlTest(ServerFixture):
    def test_hardware_breakpoint_and_continue(self):
        self.start()
        self.assertEqual(self.gdb.cmd("Z1,4600,2"), "OK")
        self.assertEqual(self.fw.breakpoints, {0: 0x4600})
        self.assertEqual(self.gdb.cmd("c"), "T05hwbreak:;00:00460000;")
        self.assertEqual(self.fw.state, probe_mod.STATE_DEBUG_HALTED)
        self.assertEqual(self.gdb.cmd("z1,4600,2"), "OK")
        self.assertEqual(self.fw.breakpoints, {})

    def test_software_breakpoints_are_refused(self):
        self.start()
        self.assertEqual(self.gdb.cmd("Z0,2400,2"), "E01")
        self.assertEqual(self.gdb.cmd("z0,2400,2"), "OK")
        self.assertIn("software breakpoints are not allowed", self.out.getvalue())
        self.assertEqual(self.fw.write_chunks, [])

    def test_breakpoint_slots(self):
        self.start()
        for i in range(8):
            self.assertEqual(self.gdb.cmd(f"Z1,{0x4600 + 2 * i:x},2"), "OK")
        self.assertEqual(self.gdb.cmd("Z1,4600,2"), "OK")  # already set: no new slot
        self.assertEqual(self.gdb.cmd("Z1,5000,2"), "E01")
        self.assertIn("all 8 hardware breakpoints are in use", self.out.getvalue())
        self.assertEqual(self.gdb.cmd("z1,4602,2"), "OK")
        self.assertEqual(self.gdb.cmd("Z1,5000,2"), "OK")
        self.assertEqual(self.fw.breakpoints[1], 0x5000)

    def test_interrupt_halts(self):
        self.start()
        self.gdb.sock.sendall(rsp.frame(b"vCont;c"))
        self.gdb.sock.sendall(b"\x03")
        self.assertEqual(self.gdb.reply(), "T0200:bc4a0000;")
        self.assertEqual(self.fw.state, probe_mod.STATE_DEBUG_HALTED)
        self.assertGreaterEqual(self.fw.resumes, 1)

    def test_interrupt_in_the_same_read_as_the_continue(self):
        self.start()
        self.gdb.sock.sendall(rsp.frame(b"c") + b"\x03")
        self.assertEqual(self.gdb.reply(), "T0200:bc4a0000;")

    def test_packet_sent_while_running_is_answered_after_the_stop(self):
        self.start()
        self.assertEqual(self.gdb.cmd("Z1,4600,2"), "OK")
        self.gdb.sock.sendall(rsp.frame(b"c") + rsp.frame(b"qAttached"))
        self.assertEqual(self.gdb.reply(), "T05hwbreak:;00:00460000;")
        self.assertEqual(self.gdb.reply(), "1")

    def test_step(self):
        self.start()
        self.assertEqual(self.gdb.cmd("vCont?"), "vCont;c;C;s;S")
        self.assertEqual(self.gdb.cmd("s"), "T0500:22450000;")
        self.assertEqual(self.gdb.cmd("vCont;s:1"), "T0500:24450000;")
        self.assertEqual(self.fw.steps, 2)

    def test_continue_from_an_address(self):
        self.start()
        self.assertEqual(self.gdb.cmd("Z1,4700,2"), "OK")
        self.assertEqual(self.gdb.cmd("c4500"), "T05hwbreak:;00:00470000;")
        self.assertEqual(self.fw.reg_writes, [(0, 0x4500)])
        self.assertEqual(self.fw.resumes, 1)

    def test_lost_target_while_running(self):
        self.fw.lost_on_poll = True
        self.start()
        self.gdb.sock.sendall(rsp.frame(b"c"))
        replies = [self.gdb.reply(), self.gdb.reply()]
        self.assertTrue(replies[0].startswith("O"))
        self.assertIn("target lost", bytes.fromhex(replies[0][1:]).decode())
        self.assertEqual(replies[1], "X1d")  # SIGLOST
        self.thread.join(5)
        self.assertEqual(self.fw.detaches, [])  # nothing left to detach

    def test_target_error_on_a_command_ends_the_session(self):
        self.start()
        self.fw.fail_op = {3: 6}
        self.assertEqual(self.gdb.cmd("m4400,2"), "E03")
        self.thread.join(5)
        self.assertFalse(self.thread.is_alive())


class DetachTest(ServerFixture):
    def test_D_detaches_and_ends_the_session(self):
        self.start()
        self.assertEqual(self.gdb.cmd("D"), "OK")
        self.thread.join(5)
        self.assertEqual(self.fw.detaches, [0])
        self.assertEqual(self.fw.state, 0)
        self.assertIn("the program continues where it stopped", self.out.getvalue())

    def test_k_detaches_too(self):
        self.start()
        self.gdb.sock.sendall(rsp.frame(b"k"))
        self.thread.join(5)
        self.assertEqual(self.fw.detaches, [0])

    def test_dropped_connection_detaches(self):
        self.start()
        self.assertEqual(self.gdb.cmd("?"), "T0500:20450000;")
        self.finish()
        self.assertEqual(self.fw.detaches, [0])

    def test_dropped_connection_while_running_detaches(self):
        self.start()
        self.gdb.sock.sendall(rsp.frame(b"c"))
        self.gdb._event()  # the ack: the server is running the target
        self.finish()
        self.assertEqual(self.fw.detaches, [0])
        self.assertEqual(self.fw.state, 0)


class MonitorTest(ServerFixture):
    def monitor(self, text):
        self.gdb.sock.sendall(rsp.frame(b"qRcmd," + text.encode().hex().encode()))
        out = self.gdb.reply()
        self.assertTrue(out.startswith("O"), out)
        self.assertEqual(self.gdb.reply(), "OK")
        return bytes.fromhex(out[1:]).decode()

    def test_help_status_mode_speed(self):
        self.start()
        self.assertIn("monitor reset", self.monitor("help"))
        self.assertIn("MSP430F5529, read-only mode, 250 kHz, halted at 0x04520, "
                      "hardware breakpoints 0 of 8 in use", self.monitor("status"))
        self.assertIn("not available in this version", self.monitor("mode full"))
        self.assertIn("read-only mode", self.monitor("mode readonly"))
        self.assertIn("500 kHz from the next attach", self.monitor("speed 500"))
        self.assertIn("100-1000", self.monitor("speed 5000"))
        self.assertIn("unknown monitor command", self.monitor("frobnicate"))

    def test_reset(self):
        self.start()
        text = self.monitor("reset")
        self.assertIn("halted at 0x0441A", text)
        # GDB 9 runs "maintenance flush-symbol-cache" for "maintenance flush
        # register-cache" (prefix match); flushregs works in every version
        self.assertIn("run 'flushregs'", text)
        self.assertNotIn("maintenance", text)
        self.assertNotIn("set again", text)
        self.assertEqual([a[0] for a in self.fw.attaches], [0, 1])
        self.assertEqual(self.gdb.cmd("?"), "T0500:1a440000;")

    def test_reset_sets_the_inserted_breakpoints_again(self):
        # with "set breakpoint always-inserted on" GDB inserts a breakpoint
        # once and does not insert it again on resume; the attach of the
        # reset clears every EEM trigger
        self.start()
        self.assertEqual(self.gdb.cmd("Z1,4600,2"), "OK")
        self.assertEqual(self.gdb.cmd("Z1,4700,2"), "OK")
        self.assertEqual(self.gdb.cmd("z1,4600,2"), "OK")  # slot 0 free, slot 1 in use
        text = self.monitor("reset")
        self.assertIn("1 hardware breakpoint set again", text)
        self.assertEqual(self.fw.breakpoints, {1: 0x4700})
        self.assertEqual(self.gdb.cmd("c"), "T05hwbreak:;00:00470000;")
        self.assertEqual(self.gdb.cmd("z1,4700,2"), "OK")
        self.assertEqual(self.fw.breakpoints, {})

    def test_reset_names_the_breakpoints_it_could_not_set_again(self):
        self.start()
        self.assertEqual(self.gdb.cmd("Z1,4600,2"), "OK")
        self.assertEqual(self.gdb.cmd("Z1,4700,2"), "OK")
        self.fw.fail_op = {17: BAD_STATE}
        text = self.monitor("reset")
        self.assertIn("target reset, halted at 0x0441A; run 'flushregs'", text)  # no "set again"
        for addr in ("0x04600", "0x04700"):
            self.assertIn(f"breakpoint at {addr} could not be set again; "
                          "delete it and set it again", text)
        self.assertIn("DBG_BP_SET failed: BAD_STATE", self.out.getvalue())
        self.assertEqual(self.fw.breakpoints, {})
        # their slots are free: GDB's delete and a new insert work
        self.fw.fail_op = {}
        self.assertEqual(self.gdb.cmd("z1,4600,2"), "OK")
        self.assertEqual(self.gdb.cmd("Z1,4600,2"), "OK")
        self.assertEqual(self.fw.breakpoints, {0: 0x4600})

    def test_reset_names_a_breakpoint_the_new_attach_has_no_trigger_for(self):
        self.start()
        self.assertEqual(self.gdb.cmd("Z1,4600,2"), "OK")
        self.assertEqual(self.gdb.cmd("Z1,4700,2"), "OK")
        self.fw.hw_breakpoints = 1
        text = self.monitor("reset")
        self.assertIn("target reset, halted at 0x0441A; 1 hardware breakpoint set again; "
                      "run 'flushregs'", text)
        self.assertIn("breakpoint at 0x04700 could not be set again; delete it and set it again",
                      text)
        self.assertEqual(self.fw.breakpoints, {0: 0x4600})

    def test_a_target_lost_while_setting_breakpoints_again_ends_the_session(self):
        self.start()
        self.assertEqual(self.gdb.cmd("Z1,4600,2"), "OK")
        self.fw.fail_op = {17: TARGET_ERROR}
        self.assertEqual(self.gdb.cmd("qRcmd," + b"reset".hex()), "E03")
        self.thread.join(5)
        self.assertFalse(self.thread.is_alive())
        self.assertIn("target lost", self.out.getvalue())
        self.assertEqual(self.fw.detaches, [0, 0])  # the reset's attach, then the session's end


# ---- the addenda ----

def console(packet):
    """The text of an O packet."""
    return bytes.fromhex(packet[1:]).decode()


class WatchdogByteWriteTest(ServerFixture):
    """The probe handles WDTCTL (0x015C) only as a word: a byte write there
    must reach it as a whole-word write of the word just read."""

    def test_byte_writes_at_wdtctl_become_word_read_modify_writes(self):
        self.fw.mem.update({0x015C: 0x80, 0x015D: 0x69})
        self.start()
        for packet, word in (("M15c,1:88", b"\x88\x69"), ("M15d,1:5a", b"\x88\x5a")):
            reads, writes = len(self.fw.reads), len(self.fw.writes)
            self.assertEqual(self.gdb.cmd(packet), "OK")
            self.assertEqual(self.fw.reads[reads:], [(0x015C, 1)], packet)
            self.assertEqual(self.fw.writes[writes:], [(0x015C, word)], packet)

    def test_a_word_write_at_wdtctl_reads_nothing(self):
        self.start()
        reads = len(self.fw.reads)
        self.assertEqual(self.gdb.cmd("M15c,2:805a"), "OK")
        self.assertEqual(len(self.fw.reads), reads)
        self.assertEqual(self.fw.writes[-1], (0x015C, b"\x80\x5a"))


class StepTimeoutTest(ServerFixture):
    """DBG_STEP answers TIMEOUT when the CPU did not stop by itself."""

    def step(self):
        self.gdb.sock.sendall(rsp.frame(b"s"))
        note = self.gdb.reply()
        self.assertTrue(note.startswith("O"), note)
        return console(note), self.gdb.reply()

    def test_a_step_into_a_low_power_mode_is_a_normal_stop(self):
        self.fw.fail_op = {14: TIMEOUT}
        self.fw.step_timeout_pc = 0x4522
        self.fw.regs[2] = 0x00D8  # BIS #LPM3+GIE,SR executed: CPUOFF set
        self.start()
        note, stop = self.step()
        self.assertEqual(stop, "T0500:22450000;")
        self.assertIn("the CPU entered a low-power mode", note)
        self.assertIn("the CPU entered a low-power mode", self.out.getvalue())
        self.assertNotIn("did not complete", self.out.getvalue())

    def test_a_step_that_does_not_complete_is_reported_as_an_interrupt(self):
        self.fw.fail_op = {14: TIMEOUT}
        self.fw.step_timeout_pc = 0x4ABC
        self.start()
        note, stop = self.step()
        self.assertEqual(stop, "T0200:bc4a0000;")
        self.assertIn("the step did not complete in time; the CPU was stopped", note)
        self.assertIn("the step did not complete in time; the CPU was stopped",
                      self.out.getvalue())
        self.assertEqual(self.gdb.cmd("?"), "T0200:bc4a0000;")


class PcRuleTest(ServerFixture):
    """Read-only mode moves the PC only into main flash, or leaves it alone."""

    MESSAGE = "read-only mode: the PC may only be set inside main flash 0x04400-0x243FF"

    def test_P_outside_main_flash_is_refused_before_the_probe(self):
        self.start()
        for pc in (0x2400, 0x17FE, 0x1800, 0x24400, 0x4300):
            self.assertEqual(self.gdb.cmd("P0=" + struct.pack("<I", pc).hex()), "E01", hex(pc))
        self.assertEqual(self.fw.reg_writes, [])
        self.assertIn(self.MESSAGE, self.out.getvalue())
        self.assertEqual(self.gdb.cmd("P0=" + struct.pack("<I", 0x243FE).hex()), "OK")
        self.assertEqual(self.fw.reg_writes, [(0, 0x243FE)])

    def test_G_moving_the_pc_out_of_main_flash_writes_nothing(self):
        self.start()
        regs = list(self.fw.regs)
        regs[5], regs[0] = 0x1234, 0x2400
        self.assertEqual(self.gdb.cmd("G" + struct.pack("<16I", *regs).hex()), "E01")
        self.assertEqual(self.fw.reg_writes, [])
        self.assertIn(self.MESSAGE, self.out.getvalue())

    def test_the_pc_may_stay_where_it_is_outside_main_flash(self):
        self.fw.regs[0] = 0x2400  # halted in code the firmware runs from RAM
        self.start()
        regs = list(self.fw.regs)
        regs[5] = 0x1234
        self.assertEqual(self.gdb.cmd("G" + struct.pack("<16I", *regs).hex()), "OK")
        self.assertEqual(self.fw.reg_writes, [(5, 0x1234)])
        self.assertEqual(self.gdb.cmd("P0=00240000"), "OK")
        self.assertNotIn(self.MESSAGE, self.out.getvalue())

    def test_continue_and_step_from_outside_main_flash_are_refused(self):
        self.start()
        self.assertEqual(self.gdb.cmd("c2400"), "E01")
        self.assertEqual(self.gdb.cmd("s1000"), "E01")
        self.assertEqual((self.fw.reg_writes, self.fw.resumes, self.fw.steps), ([], 0, 0))
        self.assertIn(self.MESSAGE, self.out.getvalue())

    def test_the_probes_bad_args_are_checked_before_a_G_writes_anything(self):
        self.start()
        self.assertEqual(self.gdb.cmd("P0=01450000"), "E01")
        self.assertIn("the PC must be even", self.out.getvalue())
        self.assertEqual(self.gdb.cmd("P2=00000100"), "E01")
        self.assertIn("SR has 16 bits", self.out.getvalue())
        for n, value in ((0, 0x4501), (2, 0x10000), (1, 0x100000)):
            regs = list(self.fw.regs)
            regs[5], regs[n] = 0x1234, value
            self.assertEqual(self.gdb.cmd("G" + struct.pack("<16I", *regs).hex()), "E01", n)
        self.assertEqual(self.fw.reg_writes, [])
        self.assertEqual(self.gdb.cmd("?"), "T0500:20450000;")  # the session goes on


class RefusedPeripheralsTest(ServerFixture):
    def test_pmm_flash_controller_sys_and_dma_are_refused_and_named(self):
        self.start()
        for packet in ("M0120,2:0000", "M013e,2:0000", "M014f,1:00", "M0180,2:0000",
                       "M019e,2:0000", "M0500,2:0000", "M053e,2:0000"):
            self.assertEqual(self.gdb.cmd(packet), "E01", packet)
        self.assertEqual(self.fw.writes, [])
        self.assertEqual(self.fw.reads, [])
        for block in ("PMM 0x0120-0x013F", "flash controller 0x0140-0x014F",
                      "SYS 0x0180-0x019F", "DMA 0x0500-0x053F"):
            self.assertIn(block, self.out.getvalue())
        self.assertEqual(self.gdb.cmd("M011e,2:0000"), "OK")  # just below the PMM
        self.assertEqual(self.gdb.cmd("M0540,2:0000"), "OK")  # just above the DMA


class SideEffectReadTest(ServerFixture):
    """GDB reads memory on its own; registers whose read changes the device
    are read only with monitor read."""

    def monitor(self, text):
        self.gdb.sock.sendall(rsp.frame(b"qRcmd," + text.encode().hex().encode()))
        out = self.gdb.reply()
        self.assertTrue(out.startswith("O"), out)
        self.assertEqual(self.gdb.reply(), "OK")
        return console(out)

    def test_reads_touching_a_side_effect_register_are_refused(self):
        self.start()
        for packet in ("m5cd,1", "m5c0,40", "m5ec,2", "m720,2", "m73f,1"):
            self.assertEqual(self.gdb.cmd(packet), "E01", packet)
        self.assertEqual(self.fw.reads, [])
        out = self.out.getvalue()
        self.assertIn('refused: read of UCA0RXBUF (0x05CC) clears UCRXIFG and the receive error '
                      'flags; use "monitor read 0x05CC 1" to read it on purpose', out)
        self.assertIn('use "monitor read 0x05EC 1"', out)
        self.assertIn('ADC12MEM0-ADC12MEM15 (0x0720) clears ADC12IFGx; use "monitor read 0x0720 16"',
                      out)

    def test_neighbouring_registers_stay_readable(self):
        self.fw.mem.update({0x05CA: 0x12, 0x05CB: 0x34})
        self.start()
        self.assertEqual(self.gdb.cmd("m5ca,2"), "1234")
        self.assertEqual(self.gdb.cmd("m5ce,2"), "ffff")
        self.assertEqual(self.fw.reads, [(0x05CA, 1), (0x05CE, 1)])

    def test_monitor_read_reads_a_side_effect_register_on_purpose(self):
        self.fw.mem.update({0x05EC: 0x41, 0x05ED: 0x00})
        self.start()
        text = self.monitor("read 0x05EC 1")
        self.assertIn("0x005EC: 0041", text)
        self.assertIn("UCB0RXBUF (0x05EC) clears UCRXIFG and the receive error flags", text)
        self.assertEqual(self.fw.reads, [(0x05EC, 1)])

    def test_monitor_read_of_several_words(self):
        self.fw.mem.update({0x4400: 0x11, 0x4401: 0x22, 0x4402: 0x33})
        self.start()
        text = self.monitor("read 0x4400 10")
        self.assertIn("0x04400: 2211 FF33 FFFF FFFF FFFF FFFF FFFF FFFF\n0x04410: FFFF FFFF", text)
        self.assertNotIn("side effect", text)
        self.assertEqual(self.monitor("read 0x4400").count("0x04400: 2211"), 1)

    def test_monitor_read_names_every_side_effect(self):
        self.start()
        text = self.monitor("read 0x05DC 2")  # UCA0ICTL, UCA0IV
        self.assertIn("UCA0IV (0x05DE) clears the highest pending flag", text)
        text = self.monitor("read 0x05CC 10")  # UCA0RXBUF ... UCA0IV
        self.assertIn("UCA0RXBUF", text)
        self.assertIn("UCA0IV", text)

    def test_monitor_read_refuses_the_bsl_and_unmapped_memory(self):
        self.start()
        self.assertIn("the BSL area 0x1000-0x17FF is never read", self.monitor("read 0x1000 1"))
        self.assertIn("refused", self.monitor("read 0x1b00 1"))
        self.assertIn("refused", self.monitor("read 0x17fe 2"))
        self.assertIn("refused", self.monitor("read 0xffffe 2"))
        self.assertEqual(self.fw.reads, [])
        self.assertIn("refused", self.out.getvalue())

    def test_monitor_read_arguments(self):
        self.start()
        for bad in ("read", "read zz", "read 0x2400 0", "read 0x2400 x", "read 0x2401 1",
                    "read -2 1", "read 0x2400 1 2"):
            self.assertIn("monitor read ADDR [WORDS]", self.monitor(bad), bad)
        self.assertEqual(self.fw.reads, [])

    def test_a_byte_write_that_would_read_a_side_effect_register_is_refused(self):
        self.start()
        self.assertEqual(self.gdb.cmd("M5cd,1:00"), "E01")
        self.assertEqual(self.gdb.cmd("M5cc,1:00"), "E01")
        self.assertEqual((self.fw.reads, self.fw.writes), ([], []))
        self.assertIn("byte write at 0x005CD would read UCA0RXBUF (0x05CC); write the whole word",
                      self.out.getvalue())
        self.assertEqual(self.gdb.cmd("M5cc,2:0000"), "OK")  # the whole word: nothing read
        self.assertEqual((self.fw.reads, self.fw.writes), ([], [(0x05CC, b"\x00\x00")]))

    def test_a_software_breakpoint_in_ram_points_to_hbreak(self):
        self.start()
        self.assertEqual(self.gdb.cmd("Z0,2400,2"), "E01")
        self.assertIn("hbreak", self.out.getvalue())
        self.assertEqual(self.fw.breakpoints, {})


class MalformedPacketTest(ServerFixture):
    def test_malformed_arguments_get_E01_and_the_session_goes_on(self):
        self.start()
        for packet in ("p", "pzz", "p-1", "P1", "Pzz=00000000", "P1=12", "P1=zz000000",
                       "m", "mzz,2", "m4400", "m4400,zz", "m-2,2", "m4400,-2", "m+4400,2",
                       "M2400,2", "Mzz,2:0000", "M2400,2:zz00", "M2400,2:00", "M2400,1:0000",
                       "X2400", "X2400,2:}", "Xzz,2:ab", "X2400,1:ab",
                       "Z1", "Z1,zz,2", "Z1,-2,2", "Z1,+4600,2", "z1,zz,2", "Z0,zz,2", "z0",
                       "czz", "c-2", "szz", "s 4500", "G", "Gzz", "G00",
                       "vCont;", "vCont;x", "vCont;t",
                       "qXfer:memory-map:read::zz,10", "qXfer:memory-map:read::0",
                       "qRcmd,zz", "qRcmd,4"):
            self.assertEqual(self.gdb.cmd(packet), "E01", packet)
        self.assertEqual(self.gdb.cmd("?"), "T0500:20450000;")  # still attached
        self.assertEqual((self.fw.reg_writes, self.fw.writes, self.fw.reads), ([], [], []))
        self.assertEqual((self.fw.breakpoints, self.fw.resumes, self.fw.steps), ({}, 0, 0))

    def test_watchpoints_are_not_supported(self):
        self.start()
        for packet in ("Z2,2400,2", "Z3,2400,2", "Z4,2400,2", "z2,2400,2"):
            self.assertEqual(self.gdb.cmd(packet), "", packet)


# ---- the Task 18 review ----

class FaultyTransport:
    """FakeFirmware behind a link that fails once on `op`: `error` is raised
    after the firmware has handled the request, so its answer is lost."""

    def __init__(self, fw, op, error):
        self.fw, self.op, self.error = fw, op, error

    def transfer(self, request, timeout_ms=3000):
        response = self.fw.transfer(request, timeout_ms)
        if self.op is not None and request[1:2] == bytes([self.op]):
            self.op = None
            raise self.error
        return response


class FlakySocket:
    """The server's end of the connection, whose recv or sendall can be made to fail."""

    def __init__(self, sock):
        self.sock = sock
        self.fail_recv = self.fail_send = False

    def fileno(self):
        return self.sock.fileno()

    def recv(self, n):
        if self.fail_recv:
            raise ConnectionResetError("connection reset by peer")
        return self.sock.recv(n)

    def sendall(self, data):
        if self.fail_send:
            raise BrokenPipeError("broken pipe")
        return self.sock.sendall(data)

    def close(self):
        self.sock.close()


class InterruptWhileSteppingTest(ServerFixture):
    """GDB's stepi N sends one step after the other; Ctrl-C must end the series."""

    def test_an_interrupt_with_the_step_stops_after_it(self):
        self.start()
        self.gdb.sock.sendall(rsp.frame(b"vCont;s:1") + b"\x03")
        self.assertEqual(self.gdb.reply(), "T0200:22450000;")
        self.assertEqual(self.fw.steps, 1)
        self.assertEqual(self.gdb.cmd("vCont;s:1"), "T0500:24450000;")  # used up

    def test_an_interrupt_between_steps_answers_the_next_step_without_stepping(self):
        self.start()
        self.assertEqual(self.gdb.cmd("vCont;s:1"), "T0500:22450000;")
        self.gdb.sock.sendall(b"\x03")
        self.assertEqual(self.gdb.cmd("vCont;s:1"), "T0200:22450000;")
        self.assertEqual(self.fw.steps, 1)

    def test_an_interrupt_while_halted_answers_the_next_continue_without_resuming(self):
        self.start()
        self.gdb.sock.sendall(b"\x03")
        self.assertEqual(self.gdb.cmd("g")[:8], "20450000")  # other packets are unaffected
        self.assertEqual(self.gdb.cmd("vCont;c"), "T0200:20450000;")
        self.assertEqual(self.fw.resumes, 0)
        self.assertEqual(self.fw.state, probe_mod.STATE_DEBUG_HALTED)
        self.assertEqual(self.gdb.cmd("Z1,4600,2"), "OK")
        self.assertEqual(self.gdb.cmd("c"), "T05hwbreak:;00:00460000;")  # used up
        self.assertEqual(self.fw.resumes, 1)


class InterruptAtAStopTest(ServerFixture):
    def test_an_interrupt_that_arrives_with_a_breakpoint_stop_is_used_up_by_it(self):
        gdb, fw = self.gdb, self.fw
        polls = []

        class CtrlCAtTheStop:  # GDB's 0x03 arrives while DBG_STATUS finds the stop
            def transfer(self, request, timeout_ms=3000):
                response = fw.transfer(request, timeout_ms)
                if request[1:2] == bytes([probe_mod.OP_DBG_STATUS]):
                    polls.append(response)
                    if len(polls) == fw.run_polls:
                        gdb.sock.sendall(b"\x03")
                return response

        self.server.probe = SbwProbe(CtrlCAtTheStop())
        self.start()
        self.assertEqual(self.gdb.cmd("Z1,4600,2"), "OK")
        self.assertEqual(self.gdb.cmd("c"), "T05hwbreak:;00:00460000;")
        polls.clear()
        self.fw.stop_at = 0x4700
        self.assertEqual(self.gdb.cmd("c"), "T0500:00470000;")  # not T02
        self.assertEqual(self.fw.resumes, 2)


class ResyncTest(ServerFixture):
    """GDB sends a packet again, or a nack, when an answer takes longer than
    its remotetimeout (2 s); every reply must still answer its own packet."""

    def test_copies_gdb_sent_during_a_slow_attach_get_one_reply(self):
        q = rsp.frame(b"qSupported:multiprocess+;swbreak+;hwbreak+")
        self.gdb.sock.sendall(q + q)
        self.start()
        self.assertEqual(self.gdb.reply(), "PacketSize=1000;qXfer:memory-map:read+;swbreak+;hwbreak+")
        self.assertEqual(self.gdb.cmd("qAttached"), "1")
        self.assertEqual(self.gdb.raw[0], ("ack", None))
        self.assertEqual(self.gdb.raw[1][0], "packet")  # the copy got no second ack

    def test_a_copy_sent_while_the_server_works_gets_no_reply(self):
        gdb, fw = self.gdb, self.fw

        class SlowRegisters:  # GDB's remotetimeout expires during DBG_READ_REGS
            def transfer(self, request, timeout_ms=3000):
                if request[1:2] == bytes([probe_mod.OP_DBG_READ_REGS]):
                    gdb.sock.sendall(rsp.frame(b"g") + b"-")
                return fw.transfer(request, timeout_ms)

        self.server.probe = SbwProbe(SlowRegisters())
        self.start()
        self.assertEqual(len(self.gdb.cmd("g")), 128)
        self.assertEqual(self.gdb.cmd("qAttached"), "1")
        self.assertEqual(self.gdb.cmd("?"), "T0500:20450000;")

    def test_two_copies_in_one_segment_get_one_reply(self):
        self.start()
        self.gdb.sock.sendall(rsp.frame(b"vCont?") * 2)
        self.assertEqual(self.gdb.reply(), "vCont;c;C;s;S")
        self.assertEqual(self.gdb.cmd("?"), "T0500:20450000;")

    def test_a_nack_sent_while_gdb_waited_resends_nothing(self):
        self.start()
        self.gdb.sock.sendall(rsp.frame(b"qAttached") + b"-")
        self.assertEqual(self.gdb.reply(), "1")
        self.assertEqual(self.gdb.cmd("?"), "T0500:20450000;")

    def test_a_nack_after_an_acknowledged_reply_resends_nothing(self):
        self.start()
        self.assertEqual(self.gdb.cmd("qAttached"), "1")
        self.gdb.sock.sendall(b"-")
        self.assertEqual(self.gdb.cmd("?"), "T0500:20450000;")

    def test_a_nack_of_an_unacknowledged_reply_resends_it(self):
        self.start()
        self.gdb.sock.sendall(rsp.frame(b"qAttached"))
        self.assertEqual(self.gdb._event(), ("ack", None))
        self.assertEqual(self.gdb._event(), ("packet", b"1"))
        self.gdb.sock.sendall(b"-")
        self.assertEqual(self.gdb.reply(), "1")
        self.assertEqual(self.gdb.cmd("?"), "T0500:20450000;")

    def test_acks_and_nacks_while_the_target_runs(self):
        self.start()
        self.gdb.sock.sendall(rsp.frame(b"vCont;c"))
        self.assertEqual(self.gdb._event(), ("ack", None))
        self.gdb.sock.sendall(b"+-+-")
        self.gdb.sock.sendall(b"\x03")
        self.assertEqual(self.gdb.reply(), "T0200:bc4a0000;")
        self.assertEqual(self.gdb.cmd("?"), "T0200:bc4a0000;")
        self.assertEqual([e for e in self.gdb.raw if e[0] == "packet"],
                         [("packet", b"T0200:bc4a0000;")] * 2)


class RunFailureTest(ServerFixture):
    """GDB waits for a stop reply while the target runs or steps: a failure
    must not come back as an error reply, after which GDB believes the
    target stopped."""

    def run_to_failure(self, packet):
        self.gdb.sock.sendall(rsp.frame(packet))
        note = self.gdb.reply()
        self.assertTrue(note.startswith("O"), note)
        return console(note), self.gdb.reply()

    def lost(self, op, status, packet):
        self.fw.fail_op = {op: status}
        self.start()
        note, stop = self.run_to_failure(packet)
        self.assertIn("target lost", note)
        self.assertEqual(stop, "X1d")
        self.thread.join(5)
        self.assertFalse(self.thread.is_alive())
        self.assertEqual(self.fw.detaches, [])  # the probe has no session left

    def test_target_error_while_running_is_a_lost_target(self):
        self.lost(13, TARGET_ERROR, b"c")

    def test_bad_state_while_running_is_a_lost_target(self):
        self.lost(13, BAD_STATE, b"c")

    def test_another_status_while_running_halts_the_cpu(self):
        self.fw.fail_op = {13: TIMEOUT}
        self.start()
        note, stop = self.run_to_failure(b"c")
        self.assertIn("DBG_STATUS failed: TIMEOUT", note)
        self.assertIn("the CPU was stopped", note)
        self.assertEqual(stop, "T0200:bc4a0000;")
        self.assertEqual(self.fw.state, probe_mod.STATE_DEBUG_HALTED)
        self.fw.fail_op = {}
        self.assertEqual(self.gdb.cmd("g")[:8], "bc4a0000")  # the session goes on

    def test_a_halt_that_fails_too_ends_the_session_and_detaches(self):
        self.fw.fail_op = {13: TIMEOUT, 11: TIMEOUT}
        self.start()
        note, stop = self.run_to_failure(b"c")
        self.assertIn("DBG_HALT failed: TIMEOUT", note)
        self.assertEqual(stop, "X1d")
        self.thread.join(5)
        self.assertEqual(self.fw.detaches, [0])
        self.assertEqual(self.fw.state, 0)

    def test_target_error_on_a_step_is_a_lost_target(self):
        self.lost(14, TARGET_ERROR, b"vCont;s:1")

    def test_bad_state_on_a_step_is_a_lost_target(self):
        self.lost(14, BAD_STATE, b"vCont;s:1")

    def test_another_status_on_a_step_halts_and_reports_an_interrupt(self):
        self.fw.fail_op = {14: BUSY}
        self.start()
        note, stop = self.run_to_failure(b"vCont;s:1")
        self.assertIn("DBG_STEP failed: BUSY", note)
        self.assertEqual(stop, "T0200:20450000;")
        self.assertEqual(self.gdb.cmd("?"), "T0200:20450000;")

    def test_a_failure_after_a_step_timeout_is_a_run_failure_too(self):
        self.fw.fail_op = {14: TIMEOUT, 13: TARGET_ERROR}
        self.start()
        note, stop = self.run_to_failure(b"s")
        self.assertIn("target lost", note)
        self.assertEqual(stop, "X1d")

    def test_a_usb_error_while_running_ends_the_session_and_detaches(self):
        self.server.probe = SbwProbe(FaultyTransport(self.fw, probe_mod.OP_DBG_STATUS,
                                                     ProbeError("USB error: pipe error")))
        self.start()
        note, stop = self.run_to_failure(b"c")
        self.assertIn("DBG_STATUS: USB error: pipe error; the debug session has ended", note)
        self.assertEqual(stop, "X1d")
        self.thread.join(5)
        self.assertEqual(self.fw.detaches, [0])
        self.assertEqual(self.fw.state, 0)


class LingerTest(ServerFixture):
    """After the last reply of a session GDB closes the connection itself
    (target-remote mode); closing first, with GDB's acks unread, resets the
    connection and GDB reports a communication error instead of the reply."""

    def test_after_a_lost_target_the_server_waits_for_gdb_to_close(self):
        self.server.linger = 5
        self.fw.lost_on_poll = True
        self.start()
        self.gdb.sock.sendall(rsp.frame(b"c"))
        self.assertTrue(self.gdb.reply().startswith("O"))
        self.assertEqual(self.gdb.reply(), "X1d")  # acknowledged, as GDB does
        self.thread.join(0.2)
        self.assertTrue(self.thread.is_alive())
        self.gdb.sock.close()
        self.thread.join(2)
        self.assertFalse(self.thread.is_alive())

    def test_a_new_packet_ends_the_wait(self):
        self.server.linger = 5
        self.start()
        self.fw.fail_op = {3: TARGET_ERROR}
        self.assertEqual(self.gdb.cmd("m4400,2"), "E03")
        self.assertIsNone(self.gdb.cmd("g"))  # not answered: the connection closes
        self.thread.join(2)
        self.assertFalse(self.thread.is_alive())

    def test_the_wait_ends_after_linger_seconds(self):
        self.server.linger = 0.3
        self.start()
        self.assertEqual(self.gdb.cmd("D"), "OK")
        self.thread.join(0.1)
        self.assertTrue(self.thread.is_alive())
        self.thread.join(2)
        self.assertFalse(self.thread.is_alive())


class LeavingTest(ServerFixture):
    """Every way out of a session detaches, if the probe may still have one."""

    def test_vkill_with_a_pid(self):
        self.start()
        self.assertEqual(self.gdb.cmd("vKill;1"), "OK")
        self.thread.join(5)
        self.assertFalse(self.thread.is_alive())
        self.assertEqual(self.fw.detaches, [0])

    def test_ctrl_c_of_the_server_while_halted_detaches(self):
        self.fw.interrupt_on_op = probe_mod.OP_DBG_READ_REGS
        self.start()
        self.gdb.sock.sendall(rsp.frame(b"g"))
        self.thread.join(5)
        self.assertIsInstance(self.error, KeyboardInterrupt)
        self.assertEqual(self.fw.detaches, [0])

    def test_ctrl_c_of_the_server_while_running_detaches(self):
        self.fw.interrupt_on_op = probe_mod.OP_DBG_STATUS
        self.start()
        self.gdb.sock.sendall(rsp.frame(b"c"))
        self.thread.join(5)
        self.assertIsInstance(self.error, KeyboardInterrupt)
        self.assertEqual(self.fw.detaches, [0])
        self.assertEqual(self.fw.state, 0)

    def test_ctrl_c_during_the_attach_still_detaches(self):
        self.server.probe = SbwProbe(FaultyTransport(self.fw, probe_mod.OP_DBG_ATTACH,
                                                     KeyboardInterrupt()))
        self.start()
        self.thread.join(5)
        self.assertIsInstance(self.error, KeyboardInterrupt)
        self.assertEqual(self.fw.detaches, [0])  # the probe had attached

    def test_a_detach_interrupted_once_is_sent_again(self):
        self.start()
        self.fw.interrupt_on_op = probe_mod.OP_DBG_DETACH
        self.assertEqual(self.gdb.cmd("D"), "OK")
        self.thread.join(5)
        self.assertEqual(self.fw.detaches, [0])

    def test_a_socket_read_error_while_running_detaches(self):
        flaky = self.server_sock = FlakySocket(self.server_sock)
        self.start()
        self.gdb.sock.sendall(rsp.frame(b"c"))
        self.assertEqual(self.gdb._event(), ("ack", None))
        flaky.fail_recv = True
        self.gdb.sock.sendall(b"+")
        self.thread.join(5)
        self.assertFalse(self.thread.is_alive())
        self.assertIsNone(self.error)
        self.assertEqual(self.fw.detaches, [0])
        self.assertEqual(self.fw.state, 0)  # detached with Run: the firmware runs

    def test_a_socket_write_error_while_running_detaches(self):
        flaky = self.server_sock = FlakySocket(self.server_sock)
        self.start()
        self.gdb.sock.sendall(rsp.frame(b"c"))
        self.assertEqual(self.gdb._event(), ("ack", None))
        flaky.fail_send = True
        self.gdb.sock.sendall(rsp.frame(b"qAttached"))  # its ack cannot be sent
        self.thread.join(5)
        self.assertFalse(self.thread.is_alive())
        self.assertEqual(self.fw.detaches, [0])
        self.assertEqual(self.fw.state, 0)


class DuringOp:
    """FakeFirmware behind a transport that calls `action` once, while the
    firmware handles `op` (GDB sends something while the probe works)."""

    def __init__(self, fw, op, action):
        self.fw, self.op, self.action = fw, op, action

    def transfer(self, request, timeout_ms=3000):
        if self.op is not None and request[1:2] == bytes([self.op]):
            self.op = None
            self.action()
        return self.fw.transfer(request, timeout_ms)


class ReviewTwoTest(ServerFixture):
    """A Ctrl-C that arrives while a step times out or a poll fails belongs
    to that step or run; a session's last reply is sent once."""

    def ctrl_c_during(self, op):
        self.server.probe = SbwProbe(DuringOp(self.fw, op, lambda: self.gdb.sock.sendall(b"\x03")))

    def nack_during(self, op):
        self.server.probe = SbwProbe(DuringOp(self.fw, op, lambda: self.gdb.sock.sendall(b"-")))

    def next_continue_runs(self, resumes):
        self.fw.fail_op = {}
        self.assertEqual(self.gdb.cmd("Z1,4600,2"), "OK")
        self.assertEqual(self.gdb.cmd("c"), "T05hwbreak:;00:00460000;")
        self.assertEqual(self.fw.resumes, resumes)

    def test_ctrl_c_during_a_step_that_times_out(self):
        self.fw.fail_op = {14: TIMEOUT}
        self.fw.step_timeout_pc = 0x4ABC
        self.ctrl_c_during(probe_mod.OP_DBG_STEP)
        self.start()
        self.gdb.sock.sendall(rsp.frame(b"vCont;s:1"))
        self.assertTrue(self.gdb.reply().startswith("O"))
        self.assertEqual(self.gdb.reply(), "T0200:bc4a0000;")
        self.next_continue_runs(1)

    def test_ctrl_c_during_a_step_into_a_low_power_mode_is_an_interrupt(self):
        self.fw.fail_op = {14: TIMEOUT}
        self.fw.step_timeout_pc = 0x4522
        self.fw.regs[2] = 0x00D8  # CPUOFF
        self.ctrl_c_during(probe_mod.OP_DBG_STEP)
        self.start()
        self.gdb.sock.sendall(rsp.frame(b"vCont;s:1"))
        self.assertIn("low-power mode", console(self.gdb.reply()))
        self.assertEqual(self.gdb.reply(), "T0200:22450000;")
        self.fw.regs[2] = 0x0008
        self.next_continue_runs(1)

    def test_ctrl_c_during_a_failing_poll(self):
        self.fw.fail_op = {13: TIMEOUT}
        self.ctrl_c_during(probe_mod.OP_DBG_STATUS)
        self.start()
        self.gdb.sock.sendall(rsp.frame(b"c"))
        self.assertIn("the CPU was stopped", console(self.gdb.reply()))
        self.assertEqual(self.gdb.reply(), "T0200:bc4a0000;")
        self.next_continue_runs(2)

    def test_a_slow_m_ending_in_target_error_gets_one_E03(self):
        self.fw.fail_op = {3: TARGET_ERROR}
        self.nack_during(probe_mod.OP_READ_MEM)  # GDB's remotetimeout expired
        self.start()
        self.assertEqual(self.gdb.cmd("m4400,2"), "E03")
        self.assertIsNone(self.gdb.reply())  # nothing more: the connection closes

    def test_a_lost_run_gets_one_X1d(self):
        self.fw.lost_on_poll = True
        self.nack_during(probe_mod.OP_DBG_STATUS)
        self.start()
        self.gdb.sock.sendall(rsp.frame(b"c"))
        self.assertTrue(self.gdb.reply().startswith("O"))
        self.assertEqual(self.gdb.reply(), "X1d")
        self.assertIsNone(self.gdb.reply())

    def slow_detach(self, packet):
        self.nack_during(probe_mod.OP_DBG_DETACH)
        self.start()
        self.assertEqual(self.gdb.cmd(packet), "OK")
        self.assertIsNone(self.gdb.reply())
        self.assertEqual(self.fw.detaches, [0])

    def test_a_slow_D_gets_one_OK(self):
        self.slow_detach("D")

    def test_a_slow_vkill_gets_one_OK(self):
        self.slow_detach("vKill;1")

    def test_ctrl_c_while_the_sigint_guard_is_installed_still_detaches(self):
        real = gdbserver._sigint_ignored
        calls = []

        def guard():
            calls.append(1)
            if len(calls) == 1:
                raise KeyboardInterrupt  # the second Ctrl-C lands here
            return real()

        gdbserver._sigint_ignored = guard
        try:
            self.start()
            self.assertEqual(self.gdb.cmd("D"), "OK")
            self.thread.join(5)
        finally:
            gdbserver._sigint_ignored = real
        self.assertEqual(self.fw.detaches, [0])
        self.assertEqual(len(calls), 2)


class DetachSignalTest(unittest.TestCase):
    def test_the_detach_ignores_ctrl_c_on_the_main_thread(self):
        fw = FakeFirmware()
        seen = []

        class Watch:
            def transfer(self, request, timeout_ms=3000):
                if request[1:2] == bytes([probe_mod.OP_DBG_DETACH]):
                    seen.append(signal.getsignal(signal.SIGINT))
                return fw.transfer(request, timeout_ms)

        server = gdbserver.GdbServer(SbwProbe(Watch()), devices.MSP430F5529, out=io.StringIO())
        mine, gdb = socket.socketpair()
        gdb.close()  # GDB went away at once
        before = signal.getsignal(signal.SIGINT)
        try:
            server.serve_connection(mine)
        finally:
            mine.close()
        self.assertEqual(seen, [signal.SIG_IGN])
        self.assertIs(signal.getsignal(signal.SIGINT), before)
        self.assertEqual(fw.detaches, [0])


class StopReplyTest(ServerFixture):
    def test_a_halt_that_finds_a_breakpoint_stop_reports_the_breakpoint(self):
        self.fw.run_polls = 1  # the first look after the resume finds the CPU stopped
        self.start()
        self.assertEqual(self.gdb.cmd("Z1,4600,2"), "OK")
        self.gdb.sock.sendall(rsp.frame(b"c") + b"\x03")  # the halt is that first look
        self.assertEqual(self.gdb.reply(), "T05hwbreak:;00:00460000;")
        self.assertEqual(self.fw.resumes, 1)

    def test_hwbreak_only_with_a_breakpoint_at_the_stop(self):
        self.fw.stop_at = 0x4800  # a breakpoint stop at no breakpoint's address
        self.start()
        self.assertEqual(self.gdb.cmd("c"), "T0500:00480000;")


class LimitsTest(ServerFixture):
    def test_m_is_capped_at_half_the_packet_size(self):
        self.start()
        reply = self.gdb.cmd("m4400,1000")
        self.assertEqual(len(reply), 2 * (gdbserver.PACKET_SIZE // 2))
        self.assertEqual(sum(n for _, n in self.fw.reads), gdbserver.PACKET_SIZE // 4)

    def test_memory_map_m_and_l_at_the_boundary(self):
        self.start()
        xml = self.server.memory_map_xml()
        n = len(xml)
        self.assertEqual(self.gdb.cmd(f"qXfer:memory-map:read::0,{n - 1:x}"), "m" + xml[:-1])
        self.assertEqual(self.gdb.cmd(f"qXfer:memory-map:read::0,{n:x}"), "l" + xml)
        self.assertEqual(self.gdb.cmd(f"qXfer:memory-map:read::{n - 1:x},1"), "l" + xml[-1])
        self.assertEqual(self.gdb.cmd(f"qXfer:memory-map:read::{n:x},10"), "l")


class PacketSizeTest(unittest.TestCase):
    def test_the_reader_buffers_what_the_packet_size_allows(self):
        self.assertEqual(rsp.MAX_UNFINISHED, 2 * gdbserver.PACKET_SIZE + 4)


class ServeTest(unittest.TestCase):
    """serve() listens on the loopback interface only."""

    def test_binds_127_0_0_1_only_and_closes_the_listener(self):
        listeners = []

        class Listener:
            def __init__(self, address, backlog):
                self.address, self.backlog, self.closed = address, backlog, False

            def getsockname(self):
                return self.address

            def accept(self):
                raise KeyboardInterrupt  # Ctrl-C while waiting for GDB

            def close(self):
                self.closed = True

        def create_server(address, backlog=None, **kwargs):
            listeners.append(Listener(address, backlog))
            return listeners[-1]

        server = gdbserver.GdbServer(SbwProbe(FakeFirmware()), devices.MSP430F5529,
                                     out=io.StringIO())
        real = gdbserver.socket.create_server
        gdbserver.socket.create_server = create_server
        try:
            with self.assertRaises(KeyboardInterrupt):
                gdbserver.serve(server, 3333)
        finally:
            gdbserver.socket.create_server = real
        self.assertEqual((listeners[0].address, listeners[0].backlog), (("127.0.0.1", 3333), 1))
        self.assertTrue(listeners[0].closed)
        self.assertIn("waiting for GDB on 127.0.0.1:3333; in GDB:\n"
                      "  set remotetimeout 10\n"
                      "  set remote hardware-breakpoint-limit 8\n"
                      "  target remote :3333\n", server.out.getvalue())


if __name__ == "__main__":
    unittest.main()
