#!/usr/bin/env python3
"""Program and debug an MSP430F5529 over Spy-Bi-Wire with a debugprobe-zephyr probe.

    sbw.py info
    sbw.py flash firmware.hex [--erase main|segments|none] [--no-verify]
    sbw.py erase [--segment ADDR]
    sbw.py read ADDR LENGTH -o dump.hex [--force]
    sbw.py verify firmware.hex
    sbw.py selftest
    sbw.py trial --backup FILE
    sbw.py reset
    sbw.py gdbserver [--port 3333] [--attach | --reset] [--speed KHZ]

Every command except gdbserver resets the target; gdbserver attaches to the
running program unless --reset is given. See docs/MSP430_SBW.md for wiring
and docs/MSP430_DEBUG.md for debugging.
"""
from __future__ import annotations

import argparse
import contextlib
import errno
import hashlib
import os
import signal
import sys
import threading

import devices
import flasher
import gdbserver
import image
import probe as probe_mod
from flasher import FlashError
from image import ImageError, Segment
from probe import ProbeError, SbwProbe


ADDR_LIMIT = 0x100000  # 20-bit address space


def _int(text: str) -> int:
    return int(text, 0)


def _speed(text: str) -> int:
    try:
        value = int(text)
    except ValueError:
        raise argparse.ArgumentTypeError(f"invalid speed {text!r}") from None
    if not 100 <= value <= 1000:
        raise argparse.ArgumentTypeError("must be between 100 and 1000 kHz")
    return value


def _port(text: str) -> int:
    try:
        value = int(text)
    except ValueError:
        raise argparse.ArgumentTypeError(f"invalid port {text!r}") from None
    if not 0 <= value <= 0xFFFF:
        raise argparse.ArgumentTypeError("must be between 0 and 65535")
    return value


def _parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="sbw.py", description="MSP430F5529 Spy-Bi-Wire programmer "
                                                           "and GDB server for debugging")
    p.add_argument("--speed", type=_speed, default=250, metavar="KHZ",
                   help="SBW clock in kHz, 100-1000 (default 250)")
    p.add_argument("--serial", help="USB serial number of the probe")
    sub = p.add_subparsers(dest="cmd", required=True)

    sub.add_parser("info", help="identify the target")

    f = sub.add_parser("flash", help="erase, program and verify an image")
    f.add_argument("image", help="Intel HEX or TI-TXT file")
    f.add_argument("--erase", choices=("main", "segments", "none"), default="main",
                   help="main: erase all main flash (default); segments: only the "
                        "segments the image touches; none: do not erase")
    f.add_argument("--no-verify", action="store_true", help="skip the read-back")

    e = sub.add_parser("erase", help="erase all main flash, or one segment")
    e.add_argument("--segment", type=_int, metavar="ADDR",
                   help="erase only the segment that contains ADDR")

    r = sub.add_parser("read", help="dump memory to a file")
    r.add_argument("addr", type=_int)
    r.add_argument("length", type=_int, help="number of bytes")
    r.add_argument("-o", "--out", required=True, help=".bin for raw bytes, otherwise Intel HEX")
    r.add_argument("--force", action="store_true", help="replace the output file if it exists")

    v = sub.add_parser("verify", help="compare flash contents with an image")
    v.add_argument("image")

    sub.add_parser("selftest", help="check the probe's read and write paths using the "
                                    "target's RAM; flash is not touched")

    t = sub.add_parser("trial", help="erase, program and verify one blank flash segment, then "
                                     "check that the rest of main flash still matches the backup")
    t.add_argument("--backup", required=True, metavar="FILE",
                   help="a dump of all of main flash (read 0x4400 0x20000 -o FILE)")

    sub.add_parser("reset", help="reset the target and let it run")

    # no abbreviations: --r, --re, --res must not select reset mode
    g = sub.add_parser("gdbserver", allow_abbrev=False,
                       help="GDB server for debugging; read-only: flash is never written")
    g.add_argument("--port", type=_port, default=3333,
                   help="TCP port on 127.0.0.1 (default 3333; 0: any free port)")
    how = g.add_mutually_exclusive_group()
    how.add_argument("--attach", action="store_true",
                     help="attach to the running program without reset (default)")
    how.add_argument("--reset", action="store_true",
                     help="reset the target and halt at the reset vector")
    g.add_argument("--speed", type=_speed, dest="gdb_speed", metavar="KHZ",
                   help="SBW clock in kHz, 100-1000 (same as the global --speed)")
    g.add_argument("--allow-flash-write", action="store_true",
                   help="full mode (phase B2; not available in this version)")
    return p


class _Progress:
    """The 'programming N/M bytes' line; finish() ends it with a newline if one is open."""

    def __init__(self, out):
        self._out = out
        self._open = False

    def __call__(self, done: int, total: int) -> None:
        self._out.write(f"\rprogramming {done}/{total} bytes")
        self._out.flush()
        self._open = True

    def finish(self) -> None:
        if self._open:
            self._out.write("\n")
            self._open = False


def _load_image(path: str) -> list[Segment]:
    segments = image.load(path)
    if not any(seg.data for seg in segments):
        raise FlashError(f"{path}: the image contains no data")
    beyond = [max(seg.addr, ADDR_LIMIT) for seg in segments if seg.data and seg.end > ADDR_LIMIT]
    if beyond:
        raise FlashError(f"{path}: the image has data at 0x{min(beyond):05X}, beyond the "
                         f"20-bit address space (last address 0x{ADDR_LIMIT - 1:05X})")
    return segments


def _prepare(args):
    """Everything that can fail without the probe; returns the image, if the command has one."""
    if args.cmd in ("flash", "verify"):
        return _load_image(args.image)
    if args.cmd == "trial":
        segments = image.load(args.backup)
        if not any(seg.data for seg in segments):
            raise FlashError(f"{args.backup}: the backup contains no data")
        return segments
    if args.cmd == "read":
        if args.addr < 0 or args.addr & 1 or args.length <= 0:
            raise FlashError("the address must be even and not negative, and the length positive")
        if args.addr + args.length > ADDR_LIMIT:
            raise FlashError(f"0x{args.addr:05X} + {args.length} bytes runs past the end of "
                             f"the address space (0x{ADDR_LIMIT:05X})")
        if os.path.exists(args.out) and not args.force:
            raise FlashError(f"{args.out} already exists; choose another name or pass --force")
    if args.cmd == "gdbserver" and args.allow_flash_write:
        raise FlashError("--allow-flash-write (full mode) comes with phase B2; this version is "
                         "read-only and never writes flash")
    return None


def _run(args, probe: SbwProbe, out, segments) -> None:
    try:
        identity, device = flasher.open_session(probe, args.speed)
        args.session_open = True  # from here on an interrupt may leave flash half-written
        out.write(f"{device.name}: JTAG ID 0x{identity.jtag_id:02X}, core ID "
                  f"0x{identity.core_id:04X}, device ID bytes {identity.device_id & 0xFF:02X}h "
                  f"{identity.device_id >> 8:02X}h\n")
        if args.cmd == "info":
            info = probe.last_info
            debug = (f"debugging supported, {info.hw_breakpoints} hardware breakpoints"
                     if probe_mod.supports_debugging(info) else "no debugging support")
            out.write(f"probe: SBW protocol version {info.version}, {debug}\n")
        elif args.cmd == "flash":
            progress = _Progress(out)
            try:
                written = flasher.program(probe, device, segments, erase=args.erase,
                                          verify=not args.no_verify, progress=progress)
            finally:
                progress.finish()
            out.write(f"programmed {written} bytes" +
                      ("" if args.no_verify else ", verified") + "\n")
        elif args.cmd == "erase":
            if args.segment is None:
                probe.erase(probe_mod.ERASE_MAIN, device.main_start, device.ram_start)
                out.write("erased main flash\n")
            else:
                if device.region(args.segment) is None:
                    raise FlashError(f"0x{args.segment:05X} is not in programmable flash")
                base = device.erase_base(args.segment)
                probe.erase(probe_mod.ERASE_SEGMENT, base, device.ram_start)
                out.write(f"erased the segment at 0x{base:05X}\n")
        elif args.cmd == "read":
            data = probe.read_mem(args.addr, (args.length + 1) // 2)[:args.length]
            part = args.out + ".part"
            try:
                image.save(part, [Segment(args.addr, data)], format_of=args.out)
                os.replace(part, args.out)
            except BaseException:
                try:
                    os.remove(part)
                except OSError:
                    pass
                raise
            out.write(f"read {len(data)} bytes from 0x{args.addr:05X} into {args.out}\n")
            out.write(f"sha256: {hashlib.sha256(data).hexdigest()}\n")
        elif args.cmd == "trial":
            base = flasher.trial(probe, device, segments, out,
                                 on_segment=lambda b: setattr(args, "trial_base", b))
            out.write(f"trial passed: segment 0x{base:05X} was erased, programmed, verified "
                      "and erased again; main flash matches the backup\n")
        elif args.cmd == "selftest":
            flasher.selftest(probe, identity, device, out)
            out.write("selftest passed\n")
        elif args.cmd == "verify":
            flasher.verify_segments(probe, segments)
            out.write("verified\n")
        # "reset" needs nothing more: the session itself resets the target.
    finally:
        # Always release the target (it is halted once CONNECT succeeded).
        try:
            probe.disconnect()
        except (ProbeError, OSError):
            pass


def _gdbserver(args, probe: SbwProbe, out, err, serve) -> None:
    """Check the probe, then serve GDB connections until Ctrl-C. No CONNECT
    (a Phase A session would reset the target): DAP_Disconnect and INFO here,
    then only what the debug session needs."""
    speed = args.gdb_speed or args.speed
    probe.dap_disconnect()
    info = probe.info()
    if not probe_mod.supports_debugging(info):
        if info.version not in probe_mod.DEBUG_PROTOCOL_VERSIONS:
            needs = " or ".join(str(v) for v in probe_mod.DEBUG_PROTOCOL_VERSIONS)
            raise FlashError(f"the probe's SBW protocol version {info.version} is not supported "
                             f"by this tool for debugging (it needs version {needs}); update the "
                             "tool or the probe firmware")
        raise FlashError(f"the probe firmware does not support debugging (SBW protocol version "
                         f"{info.version} without the debug feature); update the probe "
                         "firmware to this version of debugprobe-zephyr")
    if info.state in (probe_mod.STATE_CONNECTED, probe_mod.STATE_FLASHING):
        # DBG_ATTACH would answer BAD_STATE; CONNECT would end it, but resets the target
        raise FlashError("the probe still holds a programming session from an earlier command; "
                         "unplug and replug the probe (or run sbw.py reset, which resets the "
                         "target)")
    probe.configure(speed)
    mode = probe_mod.ATTACH_RESET if args.reset else probe_mod.ATTACH_RUNNING
    if args.reset:
        err.write("warning: --reset: the target is reset each time GDB connects; its firmware "
                  "restarts from the reset vector\n")
    server = gdbserver.GdbServer(probe, devices.MSP430F5529, speed_khz=speed,
                                 attach_mode=mode, out=out)
    args.serving = True  # from here on Ctrl-C stops the server (which detaches first)
    try:
        serve(server, args.port)
    except OSError as e:
        if e.errno in (errno.EADDRINUSE, errno.EACCES):  # in use, or a privileged port
            raise FlashError(f"{e}; choose another with --port") from None
        raise


class _Stopped(KeyboardInterrupt):
    """SIGTERM or SIGHUP: the GDB server stops as on Ctrl-C."""

    def __init__(self, signum: int):
        super().__init__(signum)
        self.signum = signum


def _raise_stopped(signum, frame):
    raise _Stopped(signum)


STOP_SIGNALS = tuple(getattr(signal, name) for name in ("SIGTERM", "SIGHUP")
                     if hasattr(signal, name))


@contextlib.contextmanager
def _signals_stop():
    """Inside, SIGTERM and SIGHUP take Ctrl-C's way out: the server detaches
    an open session and the probe is closed. Python runs signal handlers on
    the main thread only, so on another thread nothing changes."""
    if threading.current_thread() is not threading.main_thread():
        yield
        return
    previous = {}
    try:
        for signum in STOP_SIGNALS:
            previous[signum] = signal.signal(signum, _raise_stopped)
        yield
    finally:
        for signum, handler in previous.items():
            # None: a handler not installed from Python, which cannot be put back
            signal.signal(signum, signal.SIG_DFL if handler is None else handler)


def main(argv=None, transport=None, out=sys.stdout, err=sys.stderr, serve=gdbserver.serve) -> int:
    args = _parser().parse_args(argv)
    try:
        with _signals_stop() if args.cmd == "gdbserver" else contextlib.nullcontext():
            segments = _prepare(args)
            t = transport if transport is not None else probe_mod.UsbTransport(args.serial)
            try:
                if args.cmd == "gdbserver":
                    _gdbserver(args, SbwProbe(t), out, err, serve)
                else:
                    _run(args, SbwProbe(t), out, segments)
            finally:
                try:
                    t.close()
                except (ProbeError, OSError):
                    pass
    except (ProbeError, FlashError, ImageError, OSError) as e:
        err.write(f"error: {e}\n")
        return 1
    except KeyboardInterrupt as e:
        if args.cmd == "gdbserver":
            stopped = isinstance(e, _Stopped)
            what = f"{signal.Signals(e.signum).name} received" if stopped else "interrupted"
            if getattr(args, "serving", False):
                what += "; the GDB server has stopped"
            err.write(what + "\n")
            return 128 + e.signum if stopped else 130
        if args.cmd == "trial" and getattr(args, "trial_base", None) is not None:
            err.write("interrupted; " + flasher._trial_cleanup(args.trial_base) + "\n")
        elif args.cmd in ("flash", "erase") and getattr(args, "session_open", False):
            err.write("interrupted; the target's flash may be partially erased or programmed\n")
        else:
            err.write("interrupted\n")
        return 130
    return 0


if __name__ == "__main__":
    sys.exit(main())
