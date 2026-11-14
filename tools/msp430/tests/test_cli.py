import contextlib
import hashlib
import io
import os
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

    def test_options(self):
        fw = FakeFirmware()
        _, _, _, calls = self.run_server(["gdbserver", "--port", "4444", "--reset", "--speed", "500"], fw)
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

    def test_speed_outside_100_to_1000_khz_is_refused_before_anything_is_sent(self):
        for argv in (["gdbserver", "--speed", "99"], ["gdbserver", "--speed", "1001"],
                     ["gdbserver", "--speed", "fast"], ["--speed", "50", "gdbserver"]):
            t = Recorder(FakeFirmware())
            gdb = ScriptedGdb()
            argparse_err = io.StringIO()
            with contextlib.redirect_stderr(argparse_err):
                with self.assertRaises(SystemExit) as cm:
                    sbw.main(argv, transport=t, serve=gdb)
            self.assertEqual(cm.exception.code, 2, argv)
            self.assertIn("--speed", argparse_err.getvalue())
            self.assertEqual((t.commands, gdb.calls), ([], []), argv)

    def test_a_port_outside_0_to_65535_is_refused_before_anything_is_sent(self):
        for port in ("-1", "65536", "gdb"):
            t = Recorder(FakeFirmware())
            gdb = ScriptedGdb()
            argparse_err = io.StringIO()
            with contextlib.redirect_stderr(argparse_err):
                with self.assertRaises(SystemExit) as cm:
                    sbw.main(["gdbserver", "--port", port], transport=t, serve=gdb)
            self.assertEqual(cm.exception.code, 2, port)
            self.assertIn("--port", argparse_err.getvalue())
            self.assertEqual((t.commands, gdb.calls), ([], []), port)
        for port in (0, 65535):  # 0: any free port, which serve() prints
            _, _, _, calls = self.run_server(["gdbserver", "--port", str(port)], FakeFirmware())
            self.assertEqual(calls[0][1], port)

    def test_old_probe_firmware_is_refused(self):
        code, _, err, calls = self.run_server(["gdbserver"], FakeFirmware(version=1, features=1))
        self.assertEqual((code, calls), (1, []))
        self.assertIn("does not support debugging", err)

    def test_a_probe_without_debug_support_is_refused_before_any_debug_command(self):
        # protocol version 1, and version 2 without the debug feature bit
        for fw in (FakeFirmware(version=1, features=1), FakeFirmware(version=2, features=1)):
            t = Recorder(fw)
            gdb = ScriptedGdb()
            out, err = io.StringIO(), io.StringIO()
            code = sbw.main(["gdbserver"], transport=t, out=out, err=err, serve=gdb)
            self.assertEqual((code, gdb.calls), (1, []))
            self.assertIn("update the probe firmware", err.getvalue())
            self.assertEqual(t.commands, [DAP_DISCONNECT, sbw_op(probe_mod.OP_INFO)])
            self.assertTrue(fw.closed)

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
        self.assertEqual((code, err.getvalue()), (130, "interrupted; the GDB server has stopped\n"))
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

        def port_in_use(server, port):
            raise OSError(48, "Address already in use")

        fw = FakeFirmware()
        out, err = io.StringIO(), io.StringIO()
        code = sbw.main(["gdbserver"], transport=fw, out=out, err=err, serve=port_in_use)
        self.assertEqual(code, 1)
        self.assertIn("error: [Errno 48] Address already in use", err.getvalue())
        self.assertTrue(fw.closed)

    def test_info_reports_debugging(self):
        code, out, _ = run(["info"], FakeFirmware())
        self.assertEqual(code, 0)
        self.assertIn("probe: SBW protocol version 2, debugging supported, 8 hardware breakpoints", out)
        code, out, _ = run(["info"], FakeFirmware(version=1, features=1))
        self.assertIn("probe: SBW protocol version 1, no debugging support", out)


if __name__ == "__main__":
    unittest.main()
