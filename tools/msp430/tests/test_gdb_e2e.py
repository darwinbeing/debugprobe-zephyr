"""End to end: msp430-elf-gdb --batch against the GDB server and the fake
firmware. Skipped unless msp430-elf-gdb 9.x is on PATH (TI's MSP430-GCC):
the expected texts are GDB 9.1's.

Besides a session and the read-only refusals, these tests pin GDB behaviour
the server and docs/MSP430_DEBUG.md rely on (checked with GDB 9.1 of
MSP430-GCC 9.3.1.11): X packets escape *, #, $ and }; breakpoints after
monitor reset; break NAME after a PUSHM prologue, break *NAME at the first
instruction; console notes before a stop reply; Ctrl-C while stepping;
SIGLOST; load; what GDB's memory map refuses before the server is asked; the
register names."""
import io
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.dirname(__file__))

import devices  # noqa: E402
import gdbserver  # noqa: E402
import image  # noqa: E402
import symelf  # noqa: E402
from fake_firmware import RUNNING_STATE, TIMEOUT, FakeFirmware  # noqa: E402
from probe import SbwProbe  # noqa: E402

GDB = shutil.which("msp430-elf-gdb")
GDB_TIMEOUT = 60  # seconds for one GDB run; a run takes well under one


def _gdb_version():
    """The first line of `msp430-elf-gdb --version`, or None."""
    try:
        done = subprocess.run([GDB, "--version"], capture_output=True, text=True,
                              timeout=GDB_TIMEOUT)
    except (OSError, subprocess.SubprocessError):
        return None
    return (done.stdout.splitlines() or [None])[0]


def _skip_reason():
    """Why the tests cannot run here, or None."""
    if GDB is None:
        return "msp430-elf-gdb is not installed"
    version = _gdb_version()
    major = re.search(r"\s(\d+)\.\d+[.\d]*$", version or "")
    if major is None or major.group(1) != "9":
        return f"the expected texts are msp430-elf-gdb 9.x's; this one says {version!r}"
    return None


SKIP_REASON = _skip_reason()


class HookedFirmware(FakeFirmware):
    """Calls on_transfer(self) after every request, in the server's thread."""
    on_transfer = None

    def transfer(self, request, timeout_ms=3000):
        response = super().transfer(request, timeout_ms)
        if self.on_transfer is not None:
            self.on_transfer(self)
        return response


@unittest.skipIf(SKIP_REASON, SKIP_REASON)
class GdbEndToEndTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        names = os.path.join(self.tmp.name, "names.tsv")
        with open(names, "w") as f:
            f.write("address\tnew_name\n4520\tdemo_main_loop\n4600\tdemo_key_handler\n"
                    "4700\tdemo_long\n")
        self.elf = os.path.join(self.tmp.name, "symbols.elf")
        self.assertEqual(symelf.main([names, "-o", self.elf], out=io.StringIO()), 0)
        hex_path = os.path.join(self.tmp.name, "fw.hex")
        image.save(hex_path, [image.Segment(0x4400, b"\x31\x40\x00\x44")])
        self.image_elf = os.path.join(self.tmp.name, "image.elf")  # .text with contents: loadable
        self.assertEqual(symelf.main([names, "--image", hex_path, "-o", self.image_elf],
                                     out=io.StringIO()), 0)
        self.fw = HookedFirmware()
        self.fw.mem.update({0x4600: 0x30, 0x4601: 0x41})  # RET at demo_key_handler
        self.out = io.StringIO()
        self.server = gdbserver.GdbServer(SbwProbe(self.fw), devices.MSP430F5529, out=self.out,
                                          poll_interval=0.001)
        self.listener = socket.socket()
        self.addCleanup(self.listener.close)
        self.listener.settimeout(GDB_TIMEOUT)  # a GDB that never connects ends the server too
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(1)
        self.port = self.listener.getsockname()[1]
        self.steps_at_interrupt = None
        self.error = None
        self.thread = threading.Thread(target=self._serve, daemon=True)
        self.thread.start()

    def _serve(self):
        try:
            conn, _ = self.listener.accept()
            with conn:
                self.server.serve_connection(conn)
        except Exception as e:  # noqa: BLE001 -- tearDown reports it
            self.error = e

    def tearDown(self):
        # GDB has exited: the server has seen the connection end
        self.thread.join(GDB_TIMEOUT)
        self.assertFalse(self.thread.is_alive(), "the server did not end the session")
        self.assertIsNone(self.error)

    def gdb(self, *commands, elf=None, interrupt_when=None):
        """Run GDB with these commands; its stdout, then its stderr (where it
        shows the server's console notes). interrupt_when(fw): GDB gets one
        SIGINT, as from Ctrl-C, after the first request that makes it true."""
        argv = [GDB, "--batch", "-nx", "-ex", "set confirm off",
                "-ex", f"target remote 127.0.0.1:{self.port}"]
        for c in commands:
            argv += ["-ex", c]
        argv.append(elf or self.elf)
        proc = subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, text=True)
        if interrupt_when is not None:
            def hook(fw):
                if interrupt_when(fw):
                    fw.on_transfer = None
                    self.steps_at_interrupt = fw.steps
                    proc.send_signal(signal.SIGINT)
            self.fw.on_transfer = hook
        try:
            out, err = proc.communicate(timeout=GDB_TIMEOUT)
        except subprocess.TimeoutExpired:
            proc.kill()
            out, err = proc.communicate()
            self.fail(f"GDB did not finish in {GDB_TIMEOUT} s:\n{out}{err}")
        return out + err

    def test_session(self):
        text = self.gdb("maint print remote-registers", "info registers pc sp r12",
                        "break demo_key_handler", "continue", "stepi", "x/2xb 0x2400",
                        "set var *(unsigned char *)0x2400 = 0x5a", "detach")
        self.assertIn("demo_key_handler", text)
        self.assertRegex(text, r"pc\s+0x4520")
        self.assertIn("Breakpoint 1, 0x00004600 in demo_key_handler", text)
        self.assertEqual(self.fw.mem.get(0x2400), 0x5A)
        self.assertEqual(self.fw.detaches, [0])
        # 16 raw registers, 4 bytes each, remote numbers 0..15 at g/G offset 4n
        # (maint print remote-registers: Name Nr Rel Offset Size Type Rmt-Nr g/G-Offset)
        for n in range(16):
            self.assertRegex(text, rf"''\s+{n}\s+{n}\s+{4 * n}\s+4\s+uint32_t\s+{n}\s+{4 * n}\b")

    def test_read_only_refusals(self):
        text = self.gdb("set var *(unsigned short *)0x4400 = 0", "load", "break *0x2400",
                        "continue", "detach", elf=self.image_elf)
        self.assertIn("Cannot access memory at address 0x4400", text)
        self.assertRegex(text, r"Load failed|Cannot access memory|Writing to flash memory forbidden")
        self.assertIn("Load failed", text)  # GDB 9.1 refuses the rom region itself
        self.assertRegex(text, r"Cannot insert breakpoint")  # Z0 in RAM refused
        self.assertEqual(self.fw.write_chunks, [])
        self.assertEqual(self.fw.mem.get(0x4400), None)
        self.assertEqual(self.fw.resumes, 0)  # GDB does not resume with a breakpoint missing
        self.assertEqual(self.fw.detaches, [0])

    # ---- GDB behaviour the server and the guide rely on ----

    def test_x_packets_escape_star_hash_dollar_and_brace(self):
        text = self.gdb("set debug remote 1", "set var *(unsigned long *)0x2400 = 0x7d24232a",
                        "set debug remote 0", "detach")
        sent = [line for line in text.splitlines() if "Sending packet: $X2400,4:" in line]
        self.assertEqual(len(sent), 1, text)
        self.assertEqual(sent[0].count("}"), 4, sent[0])  # *, #, $ and } each escaped
        self.assertIn("}]#", sent[0])  # } is sent as } ]
        self.assertEqual(self.fw.writes, [(0x2400, b"*#$}")])

    def test_monitor_reset_keeps_breakpoints_and_flushregs_reads_the_pc(self):
        # by default GDB removes its breakpoints at a stop and inserts them on resume
        text = self.gdb("break demo_key_handler", "continue", "monitor reset",
                        "info registers pc", "flushregs", "info registers pc", "continue",
                        "detach", interrupt_when=self._ran_past_the_breakpoint)
        self.assertIn("run 'flushregs'", text)
        # GDB keeps its register cache across a monitor command, until flushregs
        self.assertEqual(re.findall(r"^pc\s+(0x[0-9a-f]+)", text, re.M), ["0x4600", "0x441a"])
        self.assertEqual(text.count("Breakpoint 1, 0x00004600 in demo_key_handler"), 2, text)
        self.assertNotIn("SIGINT", text)
        self.assertEqual([a[0] for a in self.fw.attaches], [0, 1])
        self.assertEqual(self.fw.resumes, 2)

    def test_monitor_reset_keeps_breakpoints_always_inserted(self):
        # GDB inserts the breakpoint once and not again on resume
        text = self.gdb("set breakpoint always-inserted on", "break demo_key_handler",
                        "monitor reset", "continue", "detach",
                        interrupt_when=self._ran_past_the_breakpoint)
        self.assertIn("1 hardware breakpoint set again", text)
        self.assertIn("Breakpoint 1, 0x00004600 in demo_key_handler", text)
        self.assertNotIn("SIGINT", text)

    def test_break_name_skips_the_prologue_and_break_star_name_does_not(self):
        # demo_saver: PUSHM.A #2,R10 (0x141A), MOV.W R12,R10 (0x4C0A), RET (0x4130)
        names = os.path.join(self.tmp.name, "saver.tsv")
        with open(names, "w") as f:
            f.write("address\tnew_name\n4520\tdemo_main_loop\n4680\tdemo_saver\n")
        elf = os.path.join(self.tmp.name, "saver.elf")
        self.assertEqual(symelf.main([names, "-o", elf], out=io.StringIO()), 0)
        self.fw.mem.update(dict(enumerate(bytes.fromhex("1a140a4c3041"), start=0x4680)))
        text = self.gdb("x/2i 0x4680", "break demo_saver", "continue", "delete",
                        "break *demo_saver", "continue", "detach", elf=elf)
        self.assertRegex(text, r"0x4680 <demo_saver>:\s+pushm\.a\s+#2,\s+r10")
        # GDB's prologue analysis (msp430-tdep.c) reads the code from the
        # target and skips the PUSHM.A
        self.assertIn("Breakpoint 1 at 0x4682", text)
        self.assertIn("Breakpoint 1, 0x00004682 in demo_saver ()", text)
        self.assertIn("Breakpoint 2 at 0x4680", text)
        self.assertIn("Breakpoint 2, 0x00004680 in demo_saver ()", text)
        # both are hardware breakpoints, which GDB chose by itself in flash
        self.assertIn("automatically using hardware breakpoints for read-only addresses", text)
        self.assertNotIn("Cannot insert breakpoint", text)
        self.assertEqual(self.fw.resumes, 2)
        self.assertEqual(self.fw.detaches, [0])

    @staticmethod
    def _ran_past_the_breakpoint(fw):
        """The safety net of the monitor reset tests: a CPU that has run for
        2000 polls without stopping has passed the breakpoint; GDB's Ctrl-C
        then ends the run instead of the test's timeout."""
        return fw.state == RUNNING_STATE and fw.polls >= 2000

    def test_notes_before_a_stop_reply(self):
        self.fw.fail_op = {14: TIMEOUT}
        self.fw.step_timeout_pc = 0x4522
        self.fw.regs[2] = 0x00D8  # BIS #LPM3+GIE,SR executed: CPUOFF set
        text = self.gdb("stepi", "info registers pc", "set var $sr = 0", "stepi",
                        "info registers pc", "detach")
        self.assertIn("the CPU entered a low-power mode during the step; it is halted there", text)
        self.assertIn("0x00004522 in demo_main_loop ()", text)
        self.assertIn("the step did not complete in time; the CPU was stopped", text)
        self.assertIn("Program received signal SIGINT, Interrupt.", text)
        self.assertEqual(text.count("SIGINT"), 1)  # the first step was a normal stop
        self.assertEqual(self.fw.detaches, [0])

    def test_lost_target(self):
        self.fw.lost_on_poll = True
        text = self.gdb("continue", "info registers pc", "detach")
        self.assertIn("Program terminated with signal SIGLOST, Resource lost.", text)
        self.assertIn("target lost (DBG_STATUS failed: TARGET_ERROR", text)
        self.assertIn("The program is not being run.", text)  # detach: nothing left to detach
        self.assertEqual(self.fw.detaches, [])

    # The SIGINT is sent once the fake has done a step, before the server
    # answers it. GDB passes it on as 0x03 when it next checks for Ctrl-C:
    # before its next step request (that step is then answered SIGINT
    # without stepping), or while waiting for it (that one step runs).
    STEP_BOUND = "more than one step after the Ctrl-C"

    def test_ctrl_c_during_stepi_1000(self):
        text = self.gdb("stepi 1000", "info registers pc", "detach",
                        interrupt_when=lambda fw: fw.steps >= 50)
        self.assertIn("Program received signal SIGINT, Interrupt.", text)
        self.assertLessEqual(self.fw.steps - self.steps_at_interrupt, 1, self.STEP_BOUND)
        self.assertRegex(text, rf"pc\s+0x{0x4520 + 2 * self.fw.steps:x}\s")
        self.assertEqual(self.fw.detaches, [0])

    def test_ctrl_c_during_next(self):
        self.fw.regs[0] = 0x4700  # demo_long: no symbol after it, so it ends with main flash
        text = self.gdb("next", "detach", interrupt_when=lambda fw: fw.steps >= 50)
        self.assertIn("Single stepping until exit from function demo_long", text)
        self.assertIn("Program received signal SIGINT, Interrupt.", text)
        self.assertLessEqual(self.fw.steps - self.steps_at_interrupt, 1, self.STEP_BOUND)
        self.assertEqual(self.fw.resumes, 0)
        self.assertEqual(self.fw.detaches, [0])

    def test_load_of_the_symbol_elf_without_an_image(self):
        text = self.gdb("load", "detach")
        self.assertIn("Start address 0x00000000, load size 0", text)
        self.assertIn("Could not write register \"\"; remote failure reply 'E01'", text)
        self.assertIn("the PC may only be set inside main flash", self.out.getvalue())
        self.assertEqual((self.fw.writes, self.fw.reg_writes), ([], []))

    def test_what_gdbs_memory_map_refuses_never_reaches_the_server(self):
        text = self.gdb("x/2xh 0x1000", "x/2xh 0x30000", "set var *(unsigned short *)0x1800 = 0",
                        "x/xh 0x019e", "set var *(unsigned short *)0x0140 = 0",
                        "monitor read 0x1000", "detach")
        for addr in ("0x1000", "0x30000", "0x1800", "0x19e", "0x140"):
            self.assertIn(f"Cannot access memory at address {addr}\n", text)
        # 0x1000 and 0x30000 lie outside the map, 0x1800 is rom: GDB refused
        # them itself; SYSRSTIV and FCTL1 lie in the map and the server refused them
        self.assertEqual(self.out.getvalue().count("refused:"), 3, self.out.getvalue())
        self.assertIn("refused: read of SYSRSTIV", self.out.getvalue())
        self.assertIn("refused: write of 2 bytes at 0x00140", self.out.getvalue())
        self.assertIn("refused: read of 0x01000-0x01001: the BSL area", text)  # monitor read
        read = {a + i for a, n in self.fw.reads for i in range(2 * n)}
        self.assertEqual(read & {0x1000, 0x30000, 0x019E}, set())
        self.assertEqual(self.fw.writes, [])

    def test_register_names(self):
        text = self.gdb("p $pc", "p $sp", "p $sr", "p $cg", "p $r4", "p $r15", "p $r0", "p $r3",
                        "detach")
        values = re.findall(r"^\$\d+ = (.*)$", text, re.M)
        self.assertEqual(values, ["(void (*)()) 0x4520 <demo_main_loop>", "17392", "8", "0",
                                  "1028", "1039", "void", "void"])
