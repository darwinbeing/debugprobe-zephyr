import contextlib
import errno
import hashlib
import io
import os
import signal
import socket
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.dirname(__file__))

import image  # noqa: E402
import probe as probe_mod  # noqa: E402
import rsp  # noqa: E402
import sbw  # noqa: E402
from fake_firmware import TARGET_ERROR, FakeFirmware  # noqa: E402
from image import Segment  # noqa: E402


def run(argv, fw):
    out, err = io.StringIO(), io.StringIO()
    code = sbw.main(argv, transport=fw, out=out, err=err)
    return code, out.getvalue(), err.getvalue()


class CliTest(unittest.TestCase):
    def test_info(self):
        fw = FakeFirmware()
        code, out, _ = run(["info"], fw)
        self.assertEqual(code, 0)
        self.assertIn("MSP430F5529: JTAG ID 0x91", out)
        self.assertIn("55h 29h", out)
        self.assertEqual(fw.state, 0)   # session closed
        self.assertTrue(fw.closed)

    def test_flash_then_read_back(self):
        fw = FakeFirmware()
        with tempfile.TemporaryDirectory() as d:
            src = os.path.join(d, "fw.hex")
            dump = os.path.join(d, "dump.hex")
            data = bytes(range(256)) * 2
            image.save(src, [Segment(0x4400, data)])
            code, out, _ = run(["flash", src], fw)
            self.assertEqual(code, 0)
            self.assertIn("programmed 512 bytes, verified", out)
            code, _, _ = run(["read", "0x4400", "512", "-o", dump], fw)
            self.assertEqual(code, 0)
            self.assertEqual(image.load(dump), [Segment(0x4400, data)])
            code, out, _ = run(["verify", src], fw)
            self.assertEqual((code, out.splitlines()[-1]), (0, "verified"))

    def test_erase_main_and_segment(self):
        fw = FakeFirmware()
        fw.mem = {0x4400: 0, 0x1800: 0}
        self.assertEqual(run(["erase"], fw)[0], 0)
        self.assertEqual(fw.mem, {0x1800: 0})
        self.assertEqual(run(["erase", "--segment", "0x1810"], fw)[0], 0)
        self.assertEqual(fw.mem, {})
        code, _, err = run(["erase", "--segment", "0x1980"], fw)
        self.assertEqual(code, 1)
        self.assertIn("not in programmable flash", err)

    def test_errors_exit_1_and_close_the_session(self):
        fw = FakeFirmware()
        fw.no_target = {0: 4, 1: 1}
        code, _, err = run(["info"], fw)
        self.assertEqual(code, 1)
        self.assertIn("error: no MSP430 found", err)
        self.assertTrue(fw.closed)

        fw = FakeFirmware()
        code, _, err = run(["flash", "/nonexistent/fw.hex"], fw)
        self.assertEqual(code, 1)
        self.assertEqual(fw.state, 0)   # disconnected despite the error

    def test_speed_is_validated_by_argparse(self):
        for speed in ("50", "1001", "fast"):
            fw = FakeFirmware()
            argparse_err = io.StringIO()
            with contextlib.redirect_stderr(argparse_err):
                with self.assertRaises(SystemExit) as cm:
                    run(["--speed", speed, "info"], fw)
            self.assertEqual(cm.exception.code, 2)
            self.assertIn("--speed", argparse_err.getvalue())
            self.assertEqual(fw.connects, [])

    def test_unknown_device_closes_the_session(self):
        fw = FakeFirmware(device_id=0x1234)
        code, _, err = run(["info"], fw)
        self.assertEqual(code, 1)
        self.assertIn("unsupported device", err)
        self.assertEqual(fw.state, 0)
        self.assertIn(("disconnect",), fw.ops)

    def test_nothing_is_sent_when_the_arguments_are_bad(self):
        with tempfile.TemporaryDirectory() as d:
            empty = os.path.join(d, "empty.hex")
            with open(empty, "w") as f:
                f.write(":00000001FF\n")
            out_path = os.path.join(d, "dump.hex")
            for argv in (["flash", "/nonexistent/fw.hex"], ["verify", "/nonexistent/fw.hex"],
                         ["flash", empty], ["verify", empty],
                         ["read", "0x4401", "16", "-o", out_path],
                         ["read", "0x4400", "0", "-o", out_path],
                         ["read", "0xFFFFE", "4", "-o", out_path]):
                fw = FakeFirmware()
                code, _, err = run(argv, fw)
                self.assertEqual(code, 1, argv)
                self.assertIn("error:", err)
                self.assertEqual(fw.connects, [], argv)
                self.assertEqual(fw.ops, [], argv)

    def test_target_error_while_flashing(self):
        fw = FakeFirmware()
        fw.fail_op = {7: TARGET_ERROR}
        with tempfile.TemporaryDirectory() as d:
            src = os.path.join(d, "fw.hex")
            image.save(src, [Segment(0x4400, b"\x55" * 64)])
            code, _, err = run(["flash", src], fw)
        self.assertEqual(code, 1)
        self.assertIn("error: programming failed at 0x", err)
        self.assertNotIn("Traceback", err)
        self.assertEqual(fw.state, 0)

    def test_failed_flash_leaves_the_progress_line_terminated(self):
        fw = FakeFirmware()
        fw.fail_flash_after = 100  # words; the 93-word chunk goes through, the next fails
        with tempfile.TemporaryDirectory() as d:
            src = os.path.join(d, "fw.hex")
            image.save(src, [Segment(0x4400, b"\x55" * 400)])
            code, out, err = run(["flash", src], fw)
        self.assertEqual(code, 1)
        self.assertIn("\rprogramming 186/400 bytes", out)
        self.assertTrue(out.endswith("\n"))
        self.assertTrue(err.startswith("error:"))

    def test_ctrl_c_during_flash(self):
        fw = FakeFirmware()
        fw.interrupt_on_op = 7
        with tempfile.TemporaryDirectory() as d:
            src = os.path.join(d, "fw.hex")
            image.save(src, [Segment(0x4400, b"\x55" * 400)])
            code, out, err = run(["flash", src], fw)
        self.assertEqual(code, 130)
        self.assertIn("interrupted", err)
        self.assertIn("partially erased or programmed", err)
        self.assertEqual(fw.state, 0)
        self.assertTrue(fw.closed)

    def test_ctrl_c_during_read_is_just_interrupted(self):
        fw = FakeFirmware()
        fw.interrupt_on_op = 3
        with tempfile.TemporaryDirectory() as d:
            code, _, err = run(["read", "0x4400", "16", "-o", os.path.join(d, "x.hex")], fw)
        self.assertEqual(code, 130)
        self.assertEqual(err.strip(), "interrupted")
        self.assertEqual(fw.state, 0)

    def _image(self, d, segs=None, name="fw.hex"):
        path = os.path.join(d, name)
        image.save(path, segs or [Segment(0x4400, bytes(range(16)))])
        return path

    def test_flash_reports_a_verify_failure(self):
        fw = FakeFirmware()
        fw.stuck = {0x4405: 0x00}
        with tempfile.TemporaryDirectory() as d:
            code, _, err = run(["flash", self._image(d)], fw)
        self.assertEqual(code, 1)
        self.assertIn("verify failed at 0x04405", err)

    def test_no_verify_skips_the_read_back(self):
        fw = FakeFirmware()
        fw.stuck = {0x4405: 0x00}
        with tempfile.TemporaryDirectory() as d:
            code, out, _ = run(["flash", self._image(d), "--no-verify"], fw)
        self.assertEqual(code, 0)
        self.assertNotIn("verified", out)
        self.assertEqual(fw.read_chunks, [])

    def test_verify_command_reports_a_mismatch(self):
        fw = FakeFirmware()
        with tempfile.TemporaryDirectory() as d:
            src = self._image(d)
            self.assertEqual(run(["flash", src], fw)[0], 0)
            fw.stuck = {0x4405: 0x00}
            code, _, err = run(["verify", src], fw)
        self.assertEqual(code, 1)
        self.assertIn("verify failed", err)

    def test_speed_limits(self):
        for speed in (100, 500, 1000):
            fw = FakeFirmware()
            self.assertEqual(run(["--speed", str(speed), "info"], fw)[0], 0)
            self.assertEqual(fw.speeds, [speed])
        for speed in ("99", "1001"):
            with contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as cm:
                    run(["--speed", speed, "info"], FakeFirmware())
            self.assertEqual(cm.exception.code, 2)

    def test_negative_read_address(self):
        fw = FakeFirmware()
        with tempfile.TemporaryDirectory() as d:
            code, _, err = run(["read", "-2", "4", "-o", os.path.join(d, "x.hex")], fw)
        self.assertEqual(code, 1)
        self.assertIn("error:", err)
        self.assertEqual(fw.connects, [])

    def test_image_above_the_address_space(self):
        with tempfile.TemporaryDirectory() as d:
            for segs in ([Segment(0x100000, b"\0\0")],
                         [Segment(0x4400, b"\1\2"), Segment(0xFFFFE, b"\0\0\0\0")]):
                src = self._image(d, segs)
                for cmd in ("flash", "verify"):
                    fw = FakeFirmware()
                    code, _, err = run([cmd, src], fw)
                    self.assertEqual(code, 1, cmd)
                    self.assertIn("0x100000", err)
                    self.assertEqual(fw.connects, [])
                    self.assertEqual(fw.ops, [])

    def test_ctrl_c_before_the_session_is_just_interrupted(self):
        with tempfile.TemporaryDirectory() as d:
            src = self._image(d)
            fw = FakeFirmware()
            fw.interrupt_on_op = 0  # INFO, while opening the session
            code, _, err = run(["flash", src], fw)
            self.assertEqual((code, err.strip()), (130, "interrupted"))
            fw = FakeFirmware()
            fw.interrupt_on_op = 1  # CONNECT
            code, _, err = run(["erase"], fw)
            self.assertEqual((code, err.strip()), (130, "interrupted"))
            with mock.patch.object(image, "load", side_effect=KeyboardInterrupt):
                code, _, err = run(["flash", src], FakeFirmware())
            self.assertEqual((code, err.strip()), (130, "interrupted"))

    def test_ctrl_c_during_erase_and_flash_data_warns(self):
        fw = FakeFirmware()
        fw.interrupt_on_op = 5
        code, _, err = run(["erase"], fw)
        self.assertEqual(code, 130)
        self.assertIn("partially erased or programmed", err)
        self.assertEqual(fw.state, 0)

    def test_verify_ignores_padding(self):
        fw = FakeFirmware()
        fw.mem = {0x4400: 0x12}
        with tempfile.TemporaryDirectory() as d:
            src = os.path.join(d, "odd.hex")
            image.save(src, [Segment(0x4401, b"\xAA")])
            code, out, _ = run(["flash", "--erase", "none", src], fw)
            self.assertEqual(code, 0)
            self.assertIn("programmed 2 bytes, verified", out)
            self.assertEqual(fw.mem[0x4400], 0x12)
            code, out, _ = run(["verify", src], fw)
            self.assertEqual((code, out.splitlines()[-1]), (0, "verified"))

    def test_reset(self):
        fw = FakeFirmware()
        self.assertEqual(run(["reset"], fw)[0], 0)
        self.assertIn(("disconnect",), fw.ops)

    def test_read_refuses_to_overwrite_without_force(self):
        fw = FakeFirmware()
        with tempfile.TemporaryDirectory() as d:
            dump = os.path.join(d, "backup.bin")
            with open(dump, "wb") as f:
                f.write(b"precious")
            code, _, err = run(["read", "0x4400", "16", "-o", dump], fw)
            self.assertEqual(code, 1)
            self.assertIn(dump, err)
            self.assertIn("--force", err)
            with open(dump, "rb") as f:
                self.assertEqual(f.read(), b"precious")
            self.assertEqual(fw.connects, [])
            code, _, _ = run(["read", "--force", "0x4400", "16", "-o", dump], fw)
            self.assertEqual(code, 0)
            with open(dump, "rb") as f:
                self.assertEqual(f.read(), b"\xFF" * 16)
            self.assertEqual(os.listdir(d), ["backup.bin"])

    def test_read_prints_the_sha256_of_the_data(self):
        fw = FakeFirmware()
        data = bytes(range(100))
        for i, b in enumerate(data):
            fw.mem[0x4400 + i] = b
        with tempfile.TemporaryDirectory() as d:
            code, out, _ = run(["read", "0x4400", "100", "-o", os.path.join(d, "x.bin")], fw)
        self.assertEqual(code, 0)
        lines = out.splitlines()
        self.assertTrue(lines[-2].startswith("read 100 bytes"))
        self.assertEqual(lines[-1], "sha256: " + hashlib.sha256(data).hexdigest())

    def test_read_to_bin_is_raw_bytes(self):
        fw = FakeFirmware()
        fw.mem[0x4400] = 0x42
        with tempfile.TemporaryDirectory() as d:
            dump = os.path.join(d, "x.bin")
            self.assertEqual(run(["read", "0x4400", "4", "-o", dump], fw)[0], 0)
            with open(dump, "rb") as f:
                self.assertEqual(f.read(), b"\x42\xFF\xFF\xFF")
            self.assertEqual(os.listdir(d), ["x.bin"])

    def test_failed_read_leaves_no_file_behind(self):
        fw = FakeFirmware()
        fw.fail_op = {3: 7}
        with tempfile.TemporaryDirectory() as d:
            dump = os.path.join(d, "dump.bin")
            code, _, _ = run(["read", "0x4400", "16", "-o", dump], fw)
            self.assertEqual(code, 1)
            self.assertEqual(os.listdir(d), [])
            with open(dump, "wb") as f:
                f.write(b"old")
            code, _, _ = run(["read", "--force", "0x4400", "16", "-o", dump], fw)
            self.assertEqual(code, 1)
            self.assertEqual(os.listdir(d), ["dump.bin"])
            with open(dump, "rb") as f:
                self.assertEqual(f.read(), b"old")

    def test_selftest(self):
        fw = FakeFirmware()
        fw.mem.update({0x4400: 0x12, 0x1800: 0x34})
        code, out, _ = run(["selftest"], fw)
        self.assertEqual(code, 0)
        self.assertEqual(out.splitlines()[-1], "selftest passed")
        self.assertEqual(fw.mem[0x4400], 0x12)
        self.assertEqual(fw.mem[0x1800], 0x34)
        self.assertNotIn(0x4401, fw.mem)
        self.assertEqual(fw.state, 0)

    def test_failing_selftest_exits_1(self):
        fw = FakeFirmware()
        fw.stuck = {0x2410: 0x00}
        code, out, err = run(["selftest"], fw)
        self.assertEqual(code, 1)
        self.assertIn("RAM read-back differs", err)
        self.assertNotIn("selftest passed", out)
        self.assertEqual(fw.state, 0)

    def test_trial(self):
        fw = FakeFirmware()
        for a in range(0x4400, 0x20000):
            fw.mem[a] = (a * 7) & 0xFF
        before = dict(fw.mem)
        with tempfile.TemporaryDirectory() as d:
            backup = os.path.join(d, "backup.hex")
            self.assertEqual(run(["read", "0x4400", "0x20000", "-o", backup], fw)[0], 0)
            code, out, _ = run(["trial", "--backup", backup], fw)
        self.assertEqual(code, 0)
        self.assertIn("trial passed: segment 0x24200", out)
        self.assertEqual(fw.mem, before)
        self.assertEqual(fw.state, 0)

    def test_trial_missing_backup_does_not_connect(self):
        fw = FakeFirmware()
        code, _, err = run(["trial", "--backup", "/nonexistent/backup.hex"], fw)
        self.assertEqual(code, 1)
        self.assertEqual(fw.connects, [])

    def _trial_fw(self):
        fw = FakeFirmware()
        for a in range(0x4400, 0x20000):
            fw.mem[a] = (a * 7) & 0xFF
        return fw

    def _trial_interrupted(self, op):
        fw = self._trial_fw()
        with tempfile.TemporaryDirectory() as d:
            backup = os.path.join(d, "backup.hex")
            self.assertEqual(run(["read", "0x4400", "0x20000", "-o", backup], fw)[0], 0)
            fw.interrupt_on_op = op
            code, _, err = run(["trial", "--backup", backup], fw)
        self.assertEqual(code, 130)
        self.assertEqual(fw.state, 0)
        self.assertTrue(fw.closed)
        return err

    def test_ctrl_c_during_trial_pattern_write(self):
        err = self._trial_interrupted(7)
        self.assertIn('segment 0x24200 may hold part of the test pattern: run '
                      '"erase --segment 0x24200", then "verify" with your backup', err)
        self.assertTrue(err.startswith("interrupted; "))

    def test_ctrl_c_during_trial_first_read(self):
        self.assertEqual(self._trial_interrupted(3).strip(), "interrupted")

    def test_trial_after_an_interrupted_trial(self):
        fw = self._trial_fw()
        with tempfile.TemporaryDirectory() as d:
            backup = os.path.join(d, "backup.hex")
            self.assertEqual(run(["read", "0x4400", "0x20000", "-o", backup], fw)[0], 0)
            for i in range(4):
                fw.mem[0x24200 + i] = 0x11 + i
            code, _, err = run(["trial", "--backup", backup], fw)
            self.assertEqual(code, 1)
            self.assertIn("nothing was changed", err)
            self.assertIn('erase --segment 0x24200', err)
            self.assertEqual([op for op in fw.ops if op[0] == "erase"], [])
            self.assertEqual(run(["erase", "--segment", "0x24200"], fw)[0], 0)
            code, out, _ = run(["trial", "--backup", backup], fw)
            self.assertEqual(code, 0)
            self.assertIn("trial passed", out)


def sbw_op(op):
    """A request's command bytes: the vendor command 0x90 and its op."""
    return bytes([probe_mod.VENDOR_CMD, op])


DAP_DISCONNECT = b"\x03"


class Recorder:
    """The fake firmware behind a transport that records every request's
    command: the DAP command byte, and for the vendor command 0x90 its op."""

    def __init__(self, fw):
        self.fw = fw
        self.commands = []

    def transfer(self, request, timeout_ms=3000):
        request = bytes(request)
        self.commands.append(request[:2] if request[0] == probe_mod.VENDOR_CMD else request[:1])
        return self.fw.transfer(request, timeout_ms)

    def close(self):
        self.fw.close()


class SignalOnOp(Recorder):
    """A Recorder that raises `signum` in this process when the SBW op `op`
    is sent, the first `times` times (before the request reaches the fake
    firmware)."""

    def __init__(self, fw, op, signum, times=1):
        super().__init__(fw)
        self.op, self.signum, self.times = op, signum, times

    def transfer(self, request, timeout_ms=3000):
        if self.times > 0 and bytes(request[:2]) == sbw_op(self.op):
            self.times -= 1
            signal.raise_signal(self.signum)
        return super().transfer(request, timeout_ms)


class ScriptedGdb:
    """A serve() for sbw.main: one GDB connection over a socket pair that
    sends `packets` (all different: the server drops a repeat as a copy GDB
    sent while it waited) and hangs up; the server's packets end up in
    `replies`."""

    def __init__(self, *packets):
        self.packets = packets
        self.calls = []
        self.replies = None

    def __call__(self, server, port):
        self.calls.append((server, port))
        server_sock, gdb_sock = socket.socketpair()
        with gdb_sock:
            try:
                for p in self.packets:
                    gdb_sock.sendall(rsp.frame(p.encode("latin-1")))
                gdb_sock.shutdown(socket.SHUT_WR)
                server.serve_connection(server_sock)
            finally:
                server_sock.close()
                data = b""
                try:
                    while chunk := gdb_sock.recv(4096):
                        data += chunk
                except OSError:
                    pass  # a reset instead of the end of the stream
                self.replies = [p.decode("latin-1") for kind, p in rsp.PacketReader().feed(data)
                                if kind == "packet"]


class GdbServerCliTest(unittest.TestCase):
    def run_server(self, argv, fw):
        calls = []

        def fake_serve(server, port):
            calls.append((server, port))

        out, err = io.StringIO(), io.StringIO()
        code = sbw.main(argv, transport=fw, out=out, err=err, serve=fake_serve)
        return code, out.getvalue(), err.getvalue(), calls

    def run_session(self, argv, fw, gdb):
        """sbw.main with a ScriptedGdb; also returns the commands sent to the probe."""
        t = Recorder(fw)
        out, err = io.StringIO(), io.StringIO()
        code = sbw.main(argv, transport=t, out=out, err=err, serve=gdb)
        return code, out.getvalue(), err.getvalue(), t.commands

    def test_defaults_attach_without_reset(self):
        fw = FakeFirmware()
        code, _, err, calls = self.run_server(["gdbserver"], fw)
        self.assertEqual((code, err), (0, ""))
        server, port = calls[0]
        self.assertEqual((port, server.speed_khz, server.attach_mode), (3333, 250, 0))
        self.assertEqual(fw.connects, [])  # no CONNECT: it would reset the target
        self.assertEqual(fw.ops, [("dap_disconnect",)])
        self.assertTrue(fw.closed)

    def test_explicit_attach_is_the_default_mode(self):
        fw = FakeFirmware()
        code, out, err, _ = self.run_session(["gdbserver", "--attach"], fw, ScriptedGdb("D"))
        self.assertEqual((code, err), (0, ""))  # no warning
        self.assertEqual(fw.attaches[0][0], probe_mod.ATTACH_RUNNING)
        self.assertIn("attached to MSP430F5529 without reset", out)

    def test_options(self):
        fw = FakeFirmware()
        _, _, _, calls = self.run_server(
            ["gdbserver", "--port", "4444", "--reset", "--speed", "500"], fw)
        server, port = calls[0]
        self.assertEqual((port, server.speed_khz, server.attach_mode), (4444, 500, 1))
        # the chunk sizes are configured for the speed (probe.configure)
        self.assertEqual((server.probe.read_words, server.probe.write_words), (508, 187))
        _, _, _, calls = self.run_server(["--speed", "100", "gdbserver"], FakeFirmware())
        self.assertEqual(calls[0][0].speed_khz, 100)
        _, _, _, calls = self.run_server(["--speed", "100", "gdbserver", "--speed", "1000"],
                                         FakeFirmware())
        self.assertEqual(calls[0][0].speed_khz, 1000)

    def test_attach_and_reset_exclude_each_other(self):
        with contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit):
                sbw.main(["gdbserver", "--attach", "--reset"], transport=FakeFirmware())

    def test_allow_flash_write_is_refused(self):
        fw = FakeFirmware()
        code, _, err, calls = self.run_server(["gdbserver", "--allow-flash-write"], fw)
        self.assertEqual((code, calls), (1, []))
        self.assertIn("phase B2", err)
        self.assertEqual(fw.ops, [])

    def test_allow_flash_write_is_refused_before_the_probe_is_opened(self):
        out, err = io.StringIO(), io.StringIO()
        with mock.patch.object(sbw.probe_mod, "UsbTransport",
                               side_effect=AssertionError("the probe was opened")):
            code = sbw.main(["gdbserver", "--allow-flash-write"], out=out, err=err,
                            serve=ScriptedGdb())
        self.assertEqual(code, 1)
        self.assertTrue(err.getvalue().startswith("error: --allow-flash-write"))
        self.assertIn("B2", err.getvalue())

    def refused_by_argparse(self, argv):
        """argparse must refuse argv (exit 2) before anything is sent; returns its message."""
        t = Recorder(FakeFirmware())
        gdb = ScriptedGdb()
        argparse_err = io.StringIO()
        with contextlib.redirect_stderr(argparse_err):
            with self.assertRaises(SystemExit) as cm:
                sbw.main(argv, transport=t, serve=gdb)
        self.assertEqual(cm.exception.code, 2, argv)
        self.assertEqual((t.commands, gdb.calls), ([], []), argv)
        return argparse_err.getvalue()

    def test_speed_outside_100_to_1000_khz_is_refused_before_anything_is_sent(self):
        for argv in (["gdbserver", "--speed", "99"], ["gdbserver", "--speed", "1001"],
                     ["gdbserver", "--speed", "fast"], ["--speed", "50", "gdbserver"]):
            self.assertIn("--speed", self.refused_by_argparse(argv))

    def test_a_port_outside_0_to_65535_is_refused_before_anything_is_sent(self):
        for port in ("-1", "65536", "gdb"):
            self.assertIn("--port", self.refused_by_argparse(["gdbserver", "--port", port]))
        for port in (0, 65535):  # 0: any free port, which serve() prints
            _, _, _, calls = self.run_server(["gdbserver", "--port", str(port)], FakeFirmware())
            self.assertEqual(calls[0][1], port)

    def test_abbreviated_options_are_refused_before_anything_is_sent(self):
        # --r, --re, --res must not select reset mode
        for argv in (["gdbserver", "--r"], ["gdbserver", "--re"], ["gdbserver", "--res"],
                     ["gdbserver", "--att"], ["gdbserver", "--allow"],
                     ["gdbserver", "--spe", "500"], ["gdbserver", "--po", "4444"]):
            self.assertIn("unrecognized arguments", self.refused_by_argparse(argv))

    def refused(self, fw):
        """Runs gdbserver against fw, which must be refused after DAP_Disconnect
        and INFO only; returns the error text."""
        t = Recorder(fw)
        gdb = ScriptedGdb()
        out, err = io.StringIO(), io.StringIO()
        code = sbw.main(["gdbserver"], transport=t, out=out, err=err, serve=gdb)
        self.assertEqual((code, gdb.calls), (1, []))
        self.assertEqual(t.commands, [DAP_DISCONNECT, sbw_op(probe_mod.OP_INFO)])
        self.assertTrue(fw.closed)
        return err.getvalue()

    def test_protocol_versions_other_than_2_are_refused(self):
        # version 1 has no debug ops, whatever its feature bits say; a later
        # version may have changed them
        for version, features in ((1, 1), (1, 3), (3, 3)):
            err = self.refused(FakeFirmware(version=version, features=features))
            self.assertIn(f"SBW protocol version {version} is not supported by this tool for "
                          "debugging", err)
            self.assertIn("update the tool or the probe firmware", err)

    def test_a_probe_without_the_debug_feature_is_refused(self):
        err = self.refused(FakeFirmware(version=2, features=1))
        self.assertIn("does not support debugging", err)
        self.assertIn("update the probe firmware", err)

    def test_a_probe_still_in_a_programming_session_is_refused(self):
        for state in (probe_mod.STATE_CONNECTED, probe_mod.STATE_FLASHING):
            fw = FakeFirmware()
            fw.state = state
            err = self.refused(fw)
            self.assertIn("the probe still holds a programming session from an earlier command",
                          err)
            self.assertIn("unplug and replug the probe", err)
            self.assertIn("sbw.py reset", err)
            self.assertEqual(fw.state, state)  # nothing was changed
        # a debug session left open is not in the way: DBG_ATTACH ends it
        for state in (probe_mod.STATE_DEBUG_HALTED, probe_mod.STATE_DEBUG_RUNNING):
            fw = FakeFirmware()
            fw.state = state
            code, _, _, calls = self.run_server(["gdbserver"], fw)
            self.assertEqual((code, len(calls)), (0, 1))

    def test_a_whole_session_sends_no_connect_erase_or_flash_command(self):
        fw = FakeFirmware()
        fw.mem.update({0x4400: 0x12, 0x4401: 0x34, 0x4402: 0x56, 0x4403: 0x78})
        gdb = ScriptedGdb("qSupported:multiprocess+;swbreak+;hwbreak+", "m4400,4", "D")
        code, out, err, commands = self.run_session(["gdbserver"], fw, gdb)
        self.assertEqual((code, err), (0, ""))
        self.assertEqual(gdb.replies, ["PacketSize=1000;qXfer:memory-map:read+;swbreak+;hwbreak+",
                                       "12345678", "OK"])
        # nothing but DAP_Disconnect, INFO and what the debug session needs
        self.assertEqual(commands, [DAP_DISCONNECT, sbw_op(probe_mod.OP_INFO),
                                    sbw_op(probe_mod.OP_DBG_ATTACH),
                                    sbw_op(probe_mod.OP_READ_MEM),
                                    sbw_op(probe_mod.OP_DBG_DETACH)])
        self.assertEqual(fw.connects, [])
        self.assertEqual(fw.ops, [("dap_disconnect",)])  # no DISCONNECT, ERASE, FLASH_*
        self.assertEqual(fw.attaches, [(probe_mod.ATTACH_RUNNING, 0, 250, 0x1C00, 0x4400)])
        self.assertEqual((fw.detaches, fw.state), ([probe_mod.DETACH_RUN], 0))
        self.assertIn("attached to MSP430F5529 without reset", out)
        self.assertTrue(fw.closed)

    def test_a_detach_that_reset_the_target_is_reported(self):
        fw = FakeFirmware()
        fw.detach_unrestored = True
        code, out, err, _ = self.run_session(["gdbserver"], fw, ScriptedGdb("D"))
        self.assertEqual((code, err), (0, ""))
        self.assertIn("detached, but the program does not continue where it stopped", out)
        self.assertNotIn("continues where it stopped", out)
        self.assertEqual((fw.detaches, fw.state), ([probe_mod.DETACH_RUN], 0))

    def test_reset_warns_once_that_the_firmware_restarts(self):
        fw = FakeFirmware()
        code, out, err, _ = self.run_session(["gdbserver", "--reset"], fw, ScriptedGdb("D"))
        self.assertEqual(code, 0)
        self.assertEqual(len(err.splitlines()), 1)
        self.assertTrue(err.startswith("warning: "))
        self.assertIn("reset vector", err)
        self.assertEqual(fw.attaches[0][0], probe_mod.ATTACH_RESET)
        self.assertEqual(fw.connects, [])
        self.assertIn("attached to MSP430F5529 after a reset", out)

    def test_ctrl_c_during_a_session_detaches_and_exits_130(self):
        fw = FakeFirmware()
        fw.interrupt_on_op = probe_mod.OP_DBG_READ_REGS
        code, out, err, _ = self.run_session(["gdbserver"], fw, ScriptedGdb("g"))
        self.assertEqual((code, err), (130, "interrupted; the GDB server has stopped\n"))
        self.assertEqual((fw.detaches, fw.state), ([probe_mod.DETACH_RUN], 0))
        self.assertIn("detached; the program continues where it stopped", out)
        self.assertTrue(fw.closed)

    def test_ctrl_c_outside_a_session_exits_130(self):
        def waiting_for_gdb(server, port):
            raise KeyboardInterrupt

        fw = FakeFirmware()
        out, err = io.StringIO(), io.StringIO()
        code = sbw.main(["gdbserver"], transport=fw, out=out, err=err, serve=waiting_for_gdb)
        self.assertEqual((code, err.getvalue()),
                         (130, "interrupted; the GDB server has stopped\n"))
        self.assertEqual(fw.attaches, [])
        self.assertTrue(fw.closed)

        fw = FakeFirmware()
        fw.interrupt_on_op = probe_mod.OP_INFO  # while checking the probe
        code, _, err, calls = self.run_server(["gdbserver"], fw)
        self.assertEqual((code, err, calls), (130, "interrupted\n", []))
        self.assertTrue(fw.closed)

    def test_errors_exit_1(self):
        fw = FakeFirmware()
        fw.has_handler = False
        code, _, err, calls = self.run_server(["gdbserver"], fw)
        self.assertEqual((code, calls), (1, []))
        self.assertIn("error: the probe does not answer the MSP430 SBW command", err)
        self.assertTrue(fw.closed)

    def test_a_port_in_use_suggests_another(self):
        in_use = OSError(errno.EADDRINUSE, os.strerror(errno.EADDRINUSE))

        def port_in_use(server, port):
            raise in_use

        fw = FakeFirmware()
        out, err = io.StringIO(), io.StringIO()
        code = sbw.main(["gdbserver"], transport=fw, out=out, err=err, serve=port_in_use)
        self.assertEqual((code, err.getvalue()),
                         (1, f"error: {in_use}; choose another with --port\n"))
        self.assertTrue(fw.closed)

    def stopped_by(self, signum, op, packets, times=1):
        """gdbserver with `signum` raised when `op` is sent (the first `times`
        times); the signal's own handler stays in place of the default action
        (which ends the process) and must be back afterwards. Returns code,
        out, err and the fake."""
        received = []

        def stand_in(signo, frame):
            received.append(signo)

        previous = signal.signal(signum, stand_in)
        try:
            fw = FakeFirmware()
            out, err = io.StringIO(), io.StringIO()
            code = sbw.main(["gdbserver"], transport=SignalOnOp(fw, op, signum, times),
                            out=out, err=err, serve=ScriptedGdb(*packets))
            self.assertIs(signal.getsignal(signum), stand_in)
        finally:
            signal.signal(signum, previous)
        self.assertEqual(received, [])  # the server's handler had it
        return code, out.getvalue(), err.getvalue(), fw

    def test_sigterm_and_sighup_stop_the_server_as_ctrl_c_does(self):
        for signum in (signal.SIGTERM, signal.SIGHUP):
            name = signal.Signals(signum).name
            code, out, err, fw = self.stopped_by(signum, probe_mod.OP_DBG_READ_REGS, ["g"])
            self.assertEqual((code, err),
                             (128 + signum, f"{name} received; the GDB server has stopped\n"))
            self.assertEqual((fw.detaches, fw.state), ([probe_mod.DETACH_RUN], 0))
            self.assertIn("detached; the program continues where it stopped", out)
            self.assertTrue(fw.closed)
            # before the server runs: nothing to detach
            code, _, err, fw = self.stopped_by(signum, probe_mod.OP_INFO, [])
            self.assertEqual((code, err, fw.attaches), (128 + signum, f"{name} received\n", []))
            self.assertTrue(fw.closed)

    def test_sigterm_and_sighup_during_the_detach_do_not_cut_it_off(self):
        for signum in (signal.SIGTERM, signal.SIGHUP):
            # on every try of DBG_DETACH: ignored while it is under way, so the
            # first try completes
            code, out, err, fw = self.stopped_by(signum, probe_mod.OP_DBG_DETACH, ["D"], 2)
            self.assertEqual((code, err), (0, ""), signal.Signals(signum).name)
            self.assertEqual((fw.detaches, fw.state), ([probe_mod.DETACH_RUN], 0))
            self.assertIn("detached; the program continues where it stopped", out)

    def test_signal_handlers_are_only_changed_while_the_server_runs(self):
        before = {s: signal.getsignal(s) for s in (signal.SIGTERM, signal.SIGHUP)}
        seen = {}

        def look(server, port):
            seen.update({s: signal.getsignal(s) for s in before})

        self.assertEqual(sbw.main(["gdbserver"], transport=FakeFirmware(), out=io.StringIO(),
                                  err=io.StringIO(), serve=look), 0)
        self.assertTrue(all(seen[s] is not before[s] for s in before), seen)
        self.assertEqual({s: signal.getsignal(s) for s in before}, before)
        run(["info"], FakeFirmware())
        self.assertEqual({s: signal.getsignal(s) for s in before}, before)

    def test_info_reports_debugging(self):
        code, out, _ = run(["info"], FakeFirmware())
        self.assertEqual(code, 0)
        self.assertIn("probe: SBW protocol version 2, debugging supported, "
                      "8 hardware breakpoints", out)
        code, out, _ = run(["info"], FakeFirmware(version=1, features=1))
        self.assertIn("probe: SBW protocol version 1, no debugging support", out)
        # the debug feature bit means nothing below protocol version 2
        code, out, _ = run(["info"], FakeFirmware(version=1, features=3))
        self.assertIn("probe: SBW protocol version 1, no debugging support", out)

    def test_info_sends_dap_disconnect_info_connect_and_disconnect(self):
        t = Recorder(FakeFirmware())
        self.assertEqual(sbw.main(["info"], transport=t, out=io.StringIO(), err=io.StringIO()), 0)
        self.assertEqual(t.commands, [DAP_DISCONNECT, sbw_op(probe_mod.OP_INFO),
                                      sbw_op(probe_mod.OP_CONNECT),
                                      sbw_op(probe_mod.OP_DISCONNECT)])


class EemPeekCliTest(unittest.TestCase):
    """sbw.py eem-peek: DBG_EEM_PEEK, the read-only check of the 32-bit SBW
    path (design spec correction 25). No CONNECT: the target is not reset."""

    PEEK = sbw_op(probe_mod.OP_DBG_EEM_PEEK)

    def peek(self, argv, fw):
        t = Recorder(fw)
        out, err = io.StringIO(), io.StringIO()
        code = sbw.main(argv, transport=t, out=out, err=err)
        return code, out.getvalue(), err.getvalue(), t.commands

    def test_prints_the_jtag_id_and_both_reads(self):
        fw = FakeFirmware()
        fw.eem[0x00] = 0x0004402A
        code, out, err, commands = self.peek(["eem-peek"], fw)
        self.assertEqual((code, out, err),
                         (0, "JTAG ID 0x91; EEM 0x00: 0x0004402A 0x0004402A (equal)\n", ""))
        # DAP_Disconnect, INFO and the peek: no CONNECT, DISCONNECT or debug session
        self.assertEqual(commands, [DAP_DISCONNECT, sbw_op(probe_mod.OP_INFO), self.PEEK])
        self.assertEqual((fw.peeks, fw.connects, fw.attaches, fw.state), ([(250, 0x00)], [], [], 0))
        self.assertTrue(fw.closed)

    def test_two_different_reads_exit_1(self):
        fw = FakeFirmware()
        fw.eem[0x06] = 0x00000001
        fw.peek_glitch = 0x80000001
        code, out, err, _ = self.peek(["eem-peek", "--addr", "0x06"], fw)
        self.assertEqual((code, out),
                         (1, "JTAG ID 0x91; EEM 0x06: 0x00000001 0x80000001 (different)\n"))
        self.assertEqual(err, "error: the two reads of EEM register 0x06 differ: the 32-bit SBW "
                              "path is not reliable at 250 kHz\n")
        self.assertTrue(fw.closed)

    def test_address_and_speed(self):
        fw = FakeFirmware()
        fw.eem[0x3E] = 0xFFFFFFFF
        code, out, _, _ = self.peek(["eem-peek", "--addr", "0x3E", "--speed", "500"], fw)
        self.assertEqual((code, out),
                         (0, "JTAG ID 0x91; EEM 0x3E: 0xFFFFFFFF 0xFFFFFFFF (equal)\n"))
        self.peek(["--speed", "100", "eem-peek", "--addr", "6"], fw)  # the global speed
        self.peek(["--speed", "100", "eem-peek", "--speed", "1000"], fw)  # its own wins
        self.assertEqual(fw.peeks, [(500, 0x3E), (100, 0x06), (1000, 0x00)])

    def refused_by_argparse(self, argv):
        t = Recorder(FakeFirmware())
        argparse_err = io.StringIO()
        with contextlib.redirect_stderr(argparse_err):
            with self.assertRaises(SystemExit) as cm:
                sbw.main(argv, transport=t)
        self.assertEqual((cm.exception.code, t.commands), (2, []), argv)
        return argparse_err.getvalue()

    def test_bad_addresses_and_speeds_are_refused_before_anything_is_sent(self):
        for addr in ("1", "0x3F", "0x40", "0x80", "-2", "256", "ten"):
            self.assertIn("--addr", self.refused_by_argparse(["eem-peek", "--addr", addr]))
        self.assertIn("even, 0x00-0x3E", self.refused_by_argparse(["eem-peek", "--addr", "0x41"]))
        for speed in ("99", "1001", "fast"):
            self.assertIn("--speed", self.refused_by_argparse(["eem-peek", "--speed", speed]))

    def test_no_target_is_tried_four_times(self):
        fw = FakeFirmware()
        fw.peek_no_target = 3
        code, out, _, commands = self.peek(["eem-peek"], fw)
        self.assertEqual(code, 0)
        self.assertEqual(commands.count(self.PEEK), 4)
        self.assertIn("(equal)", out)
        fw = FakeFirmware()
        fw.peek_no_target = 4
        code, out, err, commands = self.peek(["eem-peek"], fw)
        self.assertEqual((code, out), (1, ""))
        self.assertEqual(commands.count(self.PEEK), 4)
        self.assertEqual(err, "error: no MSP430 found (JTAG ID read as 0xFF); check the wiring and "
                              "the target supply, or try a lower --speed\n")
        self.assertTrue(fw.closed)

    def test_other_failures_exit_1(self):
        fw = FakeFirmware()
        fw.peek_status = probe_mod.STATUS_LOCKED
        code, _, err, commands = self.peek(["eem-peek"], fw)
        self.assertEqual((code, commands.count(self.PEEK)), (1, 1))
        self.assertEqual(err, "error: the device's JTAG lock key is programmed\n")
        fw = FakeFirmware()
        fw.peek_status = TARGET_ERROR
        code, _, err, _ = self.peek(["eem-peek"], fw)
        self.assertEqual(code, 1)
        self.assertEqual(err, "error: DBG_EEM_PEEK failed: TARGET_ERROR (the link failed: no core "
                              "ID, or the JTAG ID was not 0x91 before or after the 32-bit reads)\n")
        for state in (probe_mod.STATE_CONNECTED, probe_mod.STATE_DEBUG_HALTED):
            fw = FakeFirmware()
            fw.state = state
            code, _, err, _ = self.peek(["eem-peek"], fw)
            self.assertEqual(code, 1)
            self.assertIn("DBG_EEM_PEEK failed: BAD_STATE (the probe holds a programming or debug "
                          "session)", err)
            self.assertEqual(fw.state, state)

    def test_probes_without_debugging_are_refused_after_info(self):
        for version, features in ((1, 1), (3, 3), (2, 1)):
            fw = FakeFirmware(version=version, features=features)
            code, _, err, commands = self.peek(["eem-peek"], fw)
            self.assertEqual((code, commands), (1, [DAP_DISCONNECT, sbw_op(probe_mod.OP_INFO)]))
            self.assertTrue(err.startswith("error: "))
            self.assertIn("debugging", err)
            self.assertTrue(fw.closed)


if __name__ == "__main__":
    unittest.main()
