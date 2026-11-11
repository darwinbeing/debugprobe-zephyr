#!/usr/bin/env python3
"""Program an MSP430F5529 over Spy-Bi-Wire with a debugprobe-zephyr probe.

    sbw.py info
    sbw.py flash firmware.hex [--erase main|segments|none] [--no-verify]
    sbw.py erase [--segment ADDR]
    sbw.py read ADDR LENGTH -o dump.hex
    sbw.py verify firmware.hex
    sbw.py reset

Every command resets the target. See docs/MSP430_SBW.md for wiring.
"""
from __future__ import annotations

import argparse
import sys

import flasher
import image
import probe as probe_mod
from flasher import FlashError
from image import ImageError, Segment
from probe import ProbeError, SbwProbe


def _int(text: str) -> int:
    return int(text, 0)


def _parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="sbw.py", description="MSP430F5529 Spy-Bi-Wire programmer")
    p.add_argument("--speed", type=int, default=250, metavar="KHZ",
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

    v = sub.add_parser("verify", help="compare flash contents with an image")
    v.add_argument("image")

    sub.add_parser("reset", help="reset the target and let it run")
    return p


def _progress(out):
    def show(done: int, total: int) -> None:
        out.write(f"\rprogramming {done}/{total} bytes")
        out.flush()
    return show


def _run(args, probe: SbwProbe, out) -> None:
    if not 100 <= args.speed <= 1000:
        raise FlashError("--speed must be between 100 and 1000 kHz")
    identity, device = flasher.open_session(probe, args.speed)
    try:
        out.write(f"{device.name}: JTAG ID 0x{identity.jtag_id:02X}, core ID "
                  f"0x{identity.core_id:04X}, device ID bytes {identity.device_id & 0xFF:02X}h "
                  f"{identity.device_id >> 8:02X}h\n")
        if args.cmd == "flash":
            segments = image.load(args.image)
            flasher.program(probe, device, segments, erase=args.erase,
                            verify=not args.no_verify, progress=_progress(out))
            size = sum(len(s.data) for s in segments)
            out.write(f"\nprogrammed {size} bytes" +
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
            if args.addr & 1 or args.length <= 0:
                raise FlashError("the address must be even and the length positive")
            data = probe.read_mem(args.addr, (args.length + 1) // 2)[:args.length]
            image.save(args.out, [Segment(args.addr, data)])
            out.write(f"read {len(data)} bytes from 0x{args.addr:05X} into {args.out}\n")
        elif args.cmd == "verify":
            segments = image.pad_to_words(image.load(args.image))
            flasher.verify_segments(probe, segments)
            out.write("verified\n")
        # "info" and "reset" need nothing more: the session itself resets the target.
    finally:
        try:
            probe.disconnect()
        except ProbeError:
            pass


def main(argv=None, transport=None, out=sys.stdout, err=sys.stderr) -> int:
    args = _parser().parse_args(argv)
    try:
        t = transport if transport is not None else probe_mod.UsbTransport(args.serial)
        try:
            _run(args, SbwProbe(t), out)
        finally:
            t.close()
    except (ProbeError, FlashError, ImageError, OSError) as e:
        err.write(f"error: {e}\n")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
