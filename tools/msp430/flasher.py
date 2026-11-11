"""Erase / program / verify flow on top of SbwProbe."""
from __future__ import annotations

import devices
import image
import probe as probe_mod
from probe import StatusError

NORMAL_ENTRY_TRIES = 4  # TI GetCoreID: MAX_ENTRY_TRY


class FlashError(Exception):
    """A step of the flow failed; the message is meant for the user."""


def open_session(probe, speed_khz: int):
    """Connect to the target: normal entry up to four times, then once with the
    magic-pattern entry. Returns (Identity, Device)."""
    probe.dap_disconnect()
    info = probe.info()
    if info.version != probe_mod.PROTOCOL_VERSION:
        raise FlashError(
            f"probe speaks SBW protocol version {info.version}, this tool needs "
            f"version {probe_mod.PROTOCOL_VERSION}"
        )
    if not info.features & probe_mod.FEATURE_CPUXV2_FLASH:
        raise FlashError("the probe firmware does not support MSP430 CPUXv2 flash devices")
    probe.configure(speed_khz)

    attempts = [probe_mod.ENTRY_NORMAL] * NORMAL_ENTRY_TRIES + [probe_mod.ENTRY_MAGIC_PATTERN]
    last = None
    for mode in attempts:
        try:
            identity = probe.connect(speed_khz, mode)
            break
        except StatusError as e:
            if e.status == probe_mod.STATUS_LOCKED:
                raise FlashError(
                    "the device's JTAG lock key is programmed; it cannot be read or programmed"
                ) from None
            if e.status != probe_mod.STATUS_NO_TARGET:
                raise FlashError(str(e)) from None
            last = e
    else:
        jtag_id = last.data[0] if last.data else 0
        hint = "the data line stayed high: nothing is connected" if jtag_id == 0xFF else \
            "check the wiring, the target supply and try a lower --speed"
        raise FlashError(f"no MSP430 found (JTAG ID read as 0x{jtag_id:02X}); {hint}")

    device = devices.lookup(identity.device_id)
    if device is None:
        raw = identity.device_id
        raise FlashError(
            f"unsupported device: ID bytes {raw & 0xFF:02X}h {raw >> 8:02X}h at "
            f"0x{identity.device_id_ptr + 4:05X} (JTAG ID 0x{identity.jtag_id:02X}); "
            "add it to tools/msp430/devices.py"
        )
    return identity, device


def validate(device, segments) -> None:
    """Every byte must lie in main flash or a writable information segment."""
    for seg in segments:
        addr = seg.addr
        while addr < seg.end:
            region = device.region(addr)
            if region is None:
                raise FlashError(
                    f"image has data at 0x{addr:05X}, outside the programmable flash of "
                    f"{device.name} (main 0x{device.main_start:05X}-0x{device.main_end - 1:05X}, "
                    "information segments B-D)"
                )
            addr = region[1]


def plan_erase(device, segments, mode: str):
    """List of (erase mode, address) operations for a validated image."""
    if mode == "none":
        return []
    main_bases, info_bases = set(), set()
    for seg in segments:
        addr = seg.addr
        while addr < seg.end:
            base = device.erase_base(addr)
            if device.in_main(addr):
                main_bases.add(base)
                addr = base + device.segment_size
            else:
                info_bases.add(base)
                addr = device.region(base)[1]
    ops = []
    if mode == "main":
        if main_bases:
            ops.append((probe_mod.ERASE_MAIN, device.main_start))
    else:  # "segments"
        ops += [(probe_mod.ERASE_SEGMENT, base) for base in sorted(main_bases)]
    ops += [(probe_mod.ERASE_SEGMENT, base) for base in sorted(info_bases)]
    return ops


def write_segments(probe, device, segments, progress=lambda done, total: None) -> None:
    total = sum(len(s.data) for s in segments)
    done = 0
    for seg in segments:
        off = 0
        try:
            probe.flash_begin(seg.addr, len(seg.data) // 2, device.ram_start)
            for n in probe_mod.chunks(len(seg.data) // 2, 2, probe.write_words):
                probe.flash_data(seg.data[off:off + 2 * n])
                off += 2 * n
                progress(done + off, total)
            probe.flash_end()
        except StatusError as e:
            raise FlashError(f"programming failed at 0x{seg.addr + off:05X}: {e}") from None
        done += len(seg.data)


def verify_segments(probe, segments) -> None:
    """Compare flash with the image; only the bytes the image contains are checked,
    and the segments need not be word-aligned."""
    for seg in segments:
        start = seg.addr & ~1
        words = (seg.end - start + 1) // 2
        lead = seg.addr - start
        actual = probe.read_mem(start, words)[lead:lead + len(seg.data)]
        if actual != seg.data:
            i = next(i for i, (a, b) in enumerate(zip(actual, seg.data)) if a != b)
            raise FlashError(
                f"verify failed at 0x{seg.addr + i:05X}: read 0x{actual[i]:02X}, "
                f"expected 0x{seg.data[i]:02X}"
            )


def program(probe, device, segments, erase: str = "main", verify: bool = True,
            progress=lambda done, total: None) -> int:
    """Erase, write and verify. The session must be open. The image is padded to
    whole words for writing but verified as given. Returns the bytes written."""
    if not any(seg.data for seg in segments):
        raise FlashError("the image contains no data")
    padded = image.pad_to_words(segments)
    validate(device, padded)
    for mode, addr in plan_erase(device, padded, erase):
        try:
            probe.erase(mode, addr, device.ram_start)
        except StatusError as e:
            raise FlashError(f"erase at 0x{addr:05X} failed: {e}") from None
    write_segments(probe, device, padded, progress)
    if verify:
        verify_segments(probe, segments)
    return sum(len(seg.data) for seg in padded)


SELFTEST_WORDS = 128


def selftest(probe, identity, device, out=None) -> None:
    """Check the write and read paths against RAM and the device descriptor.
    Flash is not touched."""
    def say(line):
        if out is not None:
            out.write(line + "\n")

    base = device.ram_start
    words = [(i * 0x1F35 + 0x2A17) & 0xFFFF for i in range(SELFTEST_WORDS)]
    for name, pattern in (("pattern", words), ("inverted pattern", [w ^ 0xFFFF for w in words])):
        wrote = b"".join(w.to_bytes(2, "little") for w in pattern)
        probe.write_mem(base, wrote)
        got = probe.read_mem(base, SELFTEST_WORDS)
        for i, (w, r) in enumerate(zip(wrote, got)):
            if w != r:
                raise FlashError(f"selftest: RAM read-back differs at 0x{base + i:05X}: "
                                 f"wrote 0x{w:02X}, read 0x{r:02X}")
        say(f"RAM round trip, {name}: {len(wrote)} bytes at 0x{base:05X} read back correctly")

    block = probe.read_mem(identity.device_id_ptr, 8)
    if block[4] != identity.device_id & 0xFF or block[5] != identity.device_id >> 8:
        raise FlashError("selftest: the device ID read inside a block differs from the one "
                         "read alone; the read path is not reliable")
    say(f"device descriptor at 0x{identity.device_id_ptr:05X}: ID bytes match")
