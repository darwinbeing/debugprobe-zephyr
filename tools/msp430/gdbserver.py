"""GDB remote-protocol server for an MSP430 behind the probe (sbw.py gdbserver).

Read-only mode (phase B1): registers, memory, run, stop, single step and EEM
hardware breakpoints. Flash is never written: writes outside RAM and the
peripheral registers (the device table's peripheral holes excluded: PMM,
flash controller, SYS, DMA), PC writes outside main flash, software
breakpoints and flash programming are refused here, before anything reaches
the probe, and the probe refuses such writes again (PROTECTED). Reads stay
inside the memory map and out of the BSL area, and GDB's own reads never
touch a register whose read has a side effect (the device table lists them):
`monitor read` reads those on purpose.

One GDB connection at a time, on 127.0.0.1 only. The server attaches when
GDB connects and detaches when GDB detaches, kills, disconnects, or the
server is stopped: the target's program then continues where it stopped.

The register layout is msp430-elf-gdb's built-in one (gdb/msp430-tdep.c):
16 raw registers R0 (PC), R1 (SP), R2 (SR), R3 (CG), R4..R15, 4 bytes each,
little-endian. msp430-tdep.c does not use target descriptions, so none is
sent.
"""
from __future__ import annotations

import select
import socket
import struct
import sys

import probe as probe_mod
import rsp
from probe import ProbeError, StatusError

HOST = "127.0.0.1"    # GDB connects from this machine only
PACKET_SIZE = 0x1000  # rsp.MAX_UNFINISHED is sized for it
POLL_INTERVAL = 0.05  # seconds between DBG_STATUS polls while the target runs
ATTACH_TRIES = 4      # normal-entry tries, as flasher.NORMAL_ENTRY_TRIES
ADDR_LIMIT = probe_mod.ADDRESS_LIMIT
SIGINT, SIGTRAP = 2, 5
REG_CG = 3            # R3, the constant generator: never written
SR_CPUOFF = 0x0010
MONITOR_READ_WORDS = 256
_HEX_DIGITS = frozenset("0123456789abcdefABCDEF")

HELP = """monitor commands:
  monitor reset              reset the target and halt at the reset vector
  monitor read ADDR [WORDS]  read words on purpose, also registers whose read has a side effect
  monitor mode readonly      the only mode of this version: flash is never written
  monitor speed KHZ          SBW clock (100-1000) from the next attach
  monitor status             device, mode, speed, state, hardware breakpoints
"""
READ_USAGE = (f"usage: monitor read ADDR [WORDS]: ADDR even (0x... or decimal), "
              f"WORDS 1-{MONITOR_READ_WORDS}, default 1\n")


class Disconnected(Exception):
    """GDB closed the connection."""


class SessionEnded(Exception):
    """The debug session is over: GDB detached or killed, or the target was lost."""


class Refused(Exception):
    """Read-only mode does not allow the request; the message says why."""


def _hex(text) -> int:
    """A hex number as GDB writes it. ValueError for anything else: int(x, 16)
    would also take a sign, spaces, underscores and a 0x prefix."""
    if isinstance(text, bytes):
        text = text.decode("latin-1")
    if not text or not _HEX_DIGITS.issuperset(text):
        raise ValueError(f"{text!r} is not a hex number")
    return int(text, 16)


def _addr_len(text: str):
    addr, length = text.split(",")
    return _hex(addr), _hex(length)


def _reg_value(text: str) -> int:
    """A register value of a P packet: 4 bytes, little-endian."""
    data = bytes.fromhex(text)
    if len(data) != 4:
        raise ValueError(f"a register value has 4 bytes, not {len(data)}")
    return struct.unpack("<I", data)[0]


class GdbServer:
    def __init__(self, probe, device, speed_khz=250, attach_mode=probe_mod.ATTACH_RUNNING,
                 out=sys.stdout, poll_interval=POLL_INTERVAL):
        self.probe = probe
        self.device = device
        self.speed_khz = speed_khz
        self.attach_mode = attach_mode
        self.out = out
        self.poll_interval = poll_interval
        self.mode = "readonly"
        self.attached = False
        self.running = False
        self.slots = []        # hardware breakpoint index -> address or None
        self.regs = None       # cached registers while halted
        self.stop_reply = "S05"
        self.stop_pc = 0
        self._conn = None
        self._reader = None
        self._backlog = []
        self._deferred = []
        self._last = b""

    def say(self, line: str) -> None:
        self.out.write(line + "\n")
        if hasattr(self.out, "flush"):
            self.out.flush()

    # ---- one GDB connection ----

    def serve_connection(self, conn) -> None:
        """Attach, answer GDB's packets until it detaches or goes away, detach.
        A failed attach raises ProbeError; the caller closes the connection."""
        self._conn = conn
        self._reader = rsp.PacketReader()
        self._backlog = []   # events received but not handled yet, in order
        self._deferred = []  # packets that arrived while the target ran
        self._last = b""
        self._attach(self.attach_mode)
        try:
            while True:
                self._handle_event(*self._next_event(None, deferred=True))
        except (Disconnected, SessionEnded):
            pass
        finally:
            self._detach()

    def _handle_event(self, kind, payload):
        if kind in ("packet", "deferred"):
            if kind == "packet":
                self._put(b"+")
            reply = self._dispatch(payload)
            if reply is not None:
                self._send(reply)
        elif kind == "bad":
            self._put(b"-")
        elif kind == "nack" and self._last:
            self._put(self._last)
        # an interrupt while halted, and acks, need nothing

    def _next_event(self, timeout, deferred=False):
        """The next event: deferred packets first (if asked for), then what
        was already received, then new data from GDB, waiting up to `timeout`
        seconds (None: for ever). None on timeout."""
        if deferred and self._deferred:
            return ("deferred", self._deferred.pop(0))
        while not self._backlog:
            ready, _, _ = select.select([self._conn], [], [], timeout)
            if not ready:
                return None
            try:
                data = self._conn.recv(4096)
            except OSError:
                data = b""
            if not data:
                raise Disconnected()
            self._backlog += self._reader.feed(data)
        return self._backlog.pop(0)

    def _put(self, data: bytes) -> None:
        try:
            self._conn.sendall(data)
        except OSError:
            raise Disconnected() from None

    def _send(self, payload) -> None:
        if isinstance(payload, str):
            payload = payload.encode("latin-1")
        self._last = rsp.frame(payload)
        self._put(self._last)

    def _note(self, text: str) -> None:
        """A line on the server's console and on GDB's (while GDB waits for a
        stop reply or a monitor command)."""
        self.say(text)
        self._send("O" + (text + "\n").encode().hex())

    # ---- attach and detach ----

    def _attach(self, mode) -> None:
        # DBG_ATTACH first ends an open debug session (detached with Run)
        self.attached = False
        self.running = False
        last = None
        for _ in range(ATTACH_TRIES):
            try:
                a = self.probe.dbg_attach(mode, self.speed_khz, *self.device.ram)
                break
            except StatusError as e:
                if e.status == probe_mod.STATUS_LOCKED:
                    raise ProbeError("the device's JTAG lock key is programmed; "
                                     "it cannot be debugged") from None
                if e.status != probe_mod.STATUS_NO_TARGET:
                    raise ProbeError(str(e)) from None
                last = e
        else:
            jtag_id = last.data[0] if last.data else 0
            raise ProbeError(f"no MSP430 found (JTAG ID read as 0x{jtag_id:02X}); check the wiring "
                             "and the target supply, or try a lower --speed")
        if a.identity.device_id != self.device.device_id:
            try:
                self.probe.dbg_detach(probe_mod.DETACH_RUN)
            except ProbeError as e:
                self.say(f"detach failed: {e}")
            raw = a.identity.device_id
            raise ProbeError(f"unexpected device: ID bytes {raw & 0xFF:02X}h {raw >> 8:02X}h, "
                             f"this server is set up for {self.device.name}; "
                             "the target was left running")
        self.attached = True
        # the probe's client takes breakpoint indexes up to MAX_HW_BREAKPOINTS - 1
        self.slots = [None] * min(a.hw_breakpoints, probe_mod.MAX_HW_BREAKPOINTS)
        self.regs = None
        self._stopped(SIGTRAP, a.pc)
        how = "after a reset" if mode == probe_mod.ATTACH_RESET else "without reset"
        self.say(f"attached to {self.device.name} {how}, halted at 0x{a.pc:05X}; read-only mode; "
                 f"{len(self.slots)} hardware breakpoints")

    def _detach(self) -> None:
        if not self.attached:
            return
        self.attached = False
        try:
            self.probe.dbg_detach(probe_mod.DETACH_RUN)
            self.say("detached; the program continues where it stopped")
        except ProbeError as e:
            self.say(f"detach failed: {e}")

    def _lost(self, e) -> None:
        """The probe answered TARGET_ERROR: it has ended the session itself."""
        self.attached = False
        self.running = False
        self.say(f"target lost ({e}); the debug session has ended")

    # ---- packets ----

    def _dispatch(self, p: bytes):
        s = p.decode("latin-1")
        try:
            return self._dispatch_text(s, p)
        except Refused as e:
            return self._refuse(str(e))
        except (ValueError, IndexError) as e:
            self.say(f"bad packet {s[:40]!r}: {e}")
            return "E01"
        except StatusError as e:
            if e.status == probe_mod.STATUS_TARGET_ERROR:
                self._lost(e)
                self._send("E03")
                raise SessionEnded() from None
            self.say(f"error: {e}")
            return "E01"
        except ProbeError as e:
            # the probe's state is unknown: answer, end the session, detach if still attached
            self.say(f"error: {e}; the debug session has ended")
            self._send("E03")
            raise SessionEnded() from None

    def _dispatch_text(self, s: str, p: bytes):
        if s.startswith("qSupported"):
            return f"PacketSize={PACKET_SIZE:x};qXfer:memory-map:read+;swbreak+;hwbreak+"
        if s == "?":
            return self.stop_reply
        if s == "g":
            return "".join(struct.pack("<I", v).hex() for v in self._regs())
        if s[:1] == "G":
            return self._write_all_regs(bytes.fromhex(s[1:]))
        if s[:1] == "p":
            n = _hex(s[1:])
            return struct.pack("<I", self._regs()[n]).hex() if n < 16 else "E01"
        if s[:1] == "P":
            n, value = s[1:].split("=")
            return self._write_reg(_hex(n), _reg_value(value))
        if s[:1] == "m":
            return self._read_mem(*_addr_len(s[1:]))
        if s[:1] == "M":
            head, data = s[1:].split(":", 1)
            return self._write_mem(*_addr_len(head), bytes.fromhex(data))
        if s[:1] == "X":
            head, data = p[1:].split(b":", 1)
            return self._write_mem(*_addr_len(head.decode("latin-1")), rsp.unescape(data))
        if s[:1] == "c":
            return self._continue(s[1:])
        if s[:1] == "s":
            return self._step(s[1:])
        if s == "vCont?":
            return "vCont;c;C;s;S"
        if s.startswith("vCont;"):
            action = s[6:].split(";")[0].split(":")[0]
            if action[0] in "sS":
                return self._step("")
            if action[0] in "cC":
                return self._continue("")
            raise ValueError(f"vCont action {action!r} is not supported")
        if s[:1] in ("Z", "z"):
            return self._breakpoint(s)
        if s.startswith("vFlash"):
            raise Refused("flash programming (load) is not allowed in read-only mode")
        if s == "D" or s.startswith("D;"):
            self._detach()
            self._send("OK")
            raise SessionEnded()
        if s in ("k", "vKill") or s.startswith("vKill;"):
            self._detach()
            if s != "k":
                self._send("OK")
            raise SessionEnded()
        if s.startswith("qRcmd,"):
            return self._monitor(bytes.fromhex(s[6:]).decode("utf-8", "replace"))
        if s.startswith("qXfer:memory-map:read::"):
            offset, length = _addr_len(s.split("::", 1)[1])
            xml = self.memory_map_xml().encode()
            part = xml[offset:offset + length]
            return (b"l" if offset + length >= len(xml) else b"m") + rsp.escape(part)
        if s == "qAttached":
            return "1"
        if s[:1] == "H" or s == "qSymbol::":
            return "OK"
        return ""

    def _refuse(self, why: str) -> str:
        self.say(f"refused: {why}")
        return "E01"

    def _stopped(self, signal: int, pc: int, hwbreak: bool = False) -> str:
        """Record a stop; its stop reply, with the PC as expedited register 0."""
        self.stop_pc = pc
        self.stop_reply = (f"T{signal:02x}" + ("hwbreak:;" if hwbreak else "") +
                           "00:" + struct.pack("<I", pc).hex() + ";")
        return self.stop_reply

    # ---- registers ----

    def _regs(self) -> list:
        if self.regs is None:
            self.regs = self.probe.dbg_read_regs()
        return self.regs

    def _check_reg(self, n: int, value: int) -> None:
        if n > 15:
            raise ValueError(f"there is no register {n}")
        if n == REG_CG:
            raise ValueError("R3 is the constant generator; it cannot be written")
        # the probe's BAD_ARG rules too, so that a G packet is checked whole first
        if value >= ADDR_LIMIT:
            raise ValueError(f"0x{value:X} has more than 20 bits")
        if n == 2 and value > 0xFFFF:
            raise ValueError(f"SR has 16 bits, not 0x{value:X}")
        if n == 0 and value & 1:
            raise ValueError(f"the PC must be even, not 0x{value:05X}")
        if n == 0 and not self.device.debug_pc_writable(value, self._regs()[0]):
            raise Refused("read-only mode: the PC may only be set inside main flash "
                          f"0x{self.device.main_start:05X}-0x{self.device.main_end - 1:05X} "
                          f"(asked for 0x{value:05X})")

    def _write_reg(self, n: int, value: int) -> str:
        self._check_reg(n, value)
        self.regs = None
        self.probe.dbg_write_reg(n, value)
        return "OK"

    def _write_all_regs(self, data: bytes) -> str:
        """G: only the registers that changed are written, CG never; every
        change is checked before the first is written."""
        if len(data) != 64:
            raise ValueError(f"G carries {len(data)} bytes, not 16 registers of 4")
        current = self._regs()
        changes = [(n, v) for n, v in enumerate(struct.unpack("<16I", data))
                   if n != REG_CG and v != current[n]]
        for n, value in changes:
            self._check_reg(n, value)
        self.regs = None
        for n, value in changes:
            self.probe.dbg_write_reg(n, value)
        return "OK"

    # ---- memory ----

    def _check_readable(self, addr: int, length: int) -> None:
        """Refused unless every byte lies in the memory map, none in the BSL area."""
        if self.device.debug_readable(addr, length):
            return
        lo, hi = self.device.bsl
        what = f"read of 0x{addr:05X}-0x{addr + length - 1:05X}"
        if addr < hi and addr + length > lo:
            raise Refused(f"{what}: the BSL area 0x{lo:04X}-0x{hi - 1:04X} is never read")
        raise Refused(f"{what}: outside the target's memory map")

    def _check_side_effects(self, addr: int, length: int) -> None:
        """Refused if the (word-widened) read touches a register whose read
        changes the device: GDB reads memory on its own."""
        effect = self.device.debug_read_side_effect(addr, length)
        if effect is not None:
            lo, hi, name, what = effect
            raise Refused(f'read of {name} (0x{lo:04X}) {what}; use "monitor read 0x{lo:04X} '
                          f'{(hi - lo) // 2}" to read it on purpose')

    def _read_mem(self, addr: int, length: int) -> str:
        length = min(length, PACKET_SIZE // 2)
        if length == 0:
            return ""
        self._check_readable(addr, length)
        self._check_side_effects(addr, length)
        start = addr & ~1
        data = self.probe.read_mem(start, (addr + length - start + 1) // 2)
        return data[addr - start:addr - start + length].hex()

    def _write_mem(self, addr: int, length: int, data: bytes) -> str:
        """Whole words only reach the probe: a byte at either end is merged
        into the word read just before (the probe handles WDTCTL per word)."""
        if length != len(data):
            raise ValueError(f"the packet announces {length} bytes and carries {len(data)}")
        if not data:
            return "OK"
        start, end = addr & ~1, (addr + len(data) + 1) & ~1
        if not self.device.debug_writable(start, end - start):
            holes = ", ".join(f"{name} 0x{lo:04X}-0x{hi - 1:04X}"
                              for lo, hi, name in self.device.peripheral_holes)
            raise Refused(f"write of {len(data)} bytes at 0x{addr:05X}: read-only mode writes only "
                          f"RAM and peripheral registers, not {holes}")
        edges = sorted({a for a, partial in ((start, start != addr),
                                             (end - 2, end != addr + len(data))) if partial})
        for a in edges:
            effect = self.device.debug_read_side_effect(a, 2)
            if effect is not None:
                raise Refused(f"byte write at 0x{addr:05X} would read {effect[2]} "
                              f"(0x{effect[0]:04X}); write the whole word")
            if not self.device.debug_readable(a, 2):
                raise Refused(f"byte write at 0x{addr:05X} would read 0x{a:05X}, outside the "
                              "target's memory map; write the whole word")
        buf = bytearray(end - start)
        for a in edges:
            buf[a - start:a - start + 2] = self.probe.read_mem(a, 1)
        buf[addr - start:addr - start + len(data)] = data
        self.probe.write_mem(start, bytes(buf))
        return "OK"

    # ---- run control ----

    def _set_pc(self, arg: str) -> None:
        """c ADDR, s ADDR: the PC first, under the PC rule."""
        if arg:
            self._write_reg(0, _hex(arg))

    def _continue(self, arg: str) -> str:
        self._set_pc(arg)
        self.regs = None
        self.probe.dbg_resume()
        self.running = True
        try:
            while True:
                event = self._next_event(self.poll_interval)
                while event is not None:
                    kind, payload = event
                    if kind == "interrupt":
                        self.probe.dbg_halt()
                        st = self.probe.dbg_status()  # the halt may have met a breakpoint stop
                        self.running = False
                        hit = st.stop_reason == probe_mod.STOP_BREAKPOINT
                        return self._stopped(SIGTRAP if hit else SIGINT, st.pc, hit)
                    if kind == "packet":  # all-stop GDB waits; answer it after the stop
                        self._put(b"+")
                        self._deferred.append(payload)
                    elif kind == "bad":
                        self._put(b"-")
                    event = self._next_event(0)
                st = self.probe.dbg_status()
                if st.halted:
                    self.running = False
                    return self._stopped(SIGTRAP, st.pc,
                                         st.stop_reason == probe_mod.STOP_BREAKPOINT)
        except StatusError as e:
            if e.status != probe_mod.STATUS_TARGET_ERROR:
                raise
            self._lost(e)
            self._send("O" + f"target lost ({e})\n".encode().hex())
            self._send("W00")
            raise SessionEnded() from None

    def _step(self, arg: str) -> str:
        self._set_pc(arg)
        self.regs = None
        try:
            return self._stopped(SIGTRAP, self.probe.dbg_step())
        except StatusError as e:
            if e.status != probe_mod.STATUS_TIMEOUT:
                raise
        # The CPU did not stop by itself and the probe halted it. A step over
        # an instruction that enters a low-power mode ends like this too.
        st = self.probe.dbg_status()
        if self._regs()[2] & SR_CPUOFF:
            self._note("the CPU entered a low-power mode during the step; it is halted there")
            return self._stopped(SIGTRAP, st.pc)
        self._note("the step did not complete in time; the CPU was stopped")
        return self._stopped(SIGINT, st.pc)

    # ---- breakpoints ----

    def _breakpoint(self, s: str) -> str:
        """Z/z packets: type 1 (hardware) in the EEM; type 0 (software) refused,
        and answered E01, not empty, so that GDB does not write a breakpoint
        instruction into memory itself; watchpoints are not supported."""
        fields = s[1:].split(",")
        if fields[0] not in ("0", "1"):
            return ""
        addr = _hex(fields[1])
        _hex(fields[2].split(";")[0])  # the kind; ignored
        if fields[0] == "1":
            return self._insert_hw(addr) if s[0] == "Z" else self._remove_hw(addr)
        if s[0] == "z":
            return "OK"  # nothing was inserted
        in_ram = self.device.ram[0] <= addr < self.device.ram[1]
        raise Refused("software breakpoints are not allowed in read-only mode; use hbreak for the "
                      f"breakpoint at 0x{addr:05X}" +
                      (" (GDB's break asks for a software breakpoint in RAM; in flash it uses a "
                       "hardware breakpoint by itself)" if in_ram else ""))

    def _insert_hw(self, addr: int) -> str:
        if addr in self.slots:
            return "OK"
        if addr & 1 or addr >= ADDR_LIMIT:
            raise ValueError(f"no instruction starts at 0x{addr:X}")
        if None not in self.slots:
            raise Refused(f"all {len(self.slots)} hardware breakpoints are in use")
        index = self.slots.index(None)
        self.probe.dbg_bp_set(index, addr)
        self.slots[index] = addr
        return "OK"

    def _remove_hw(self, addr: int) -> str:
        if addr in self.slots:
            index = self.slots.index(addr)
            self.probe.dbg_bp_clear(index)
            self.slots[index] = None
        return "OK"

    # ---- monitor ----

    def _monitor(self, command: str) -> str:
        words = command.split()
        if not words or words == ["help"]:
            text = HELP
        elif words == ["reset"]:
            self._attach(probe_mod.ATTACH_RESET)
            text = (f"target reset, halted at 0x{self.stop_pc:05X}; "
                    "run 'flushregs' so that GDB reads the registers again\n")
        elif words[0] == "read":
            text = self._monitor_read(words[1:])
        elif words[0] == "mode" and len(words) == 2 and words[1] in ("readonly", "full"):
            text = ("read-only mode: flash is never written\n" if words[1] == "readonly" else
                    "full mode is not available in this version (phase B2); "
                    "the session stays read-only\n")
        elif words[0] == "speed" and len(words) == 2:
            try:
                khz = int(words[1])
            except ValueError:
                khz = 0
            if 100 <= khz <= 1000:
                self.speed_khz = khz
                text = (f"SBW clock {khz} kHz from the next attach "
                        "(monitor reset, or the next connection)\n")
            else:
                text = "speed must be 100-1000 kHz\n"
        elif words == ["status"]:
            used = sum(1 for s in self.slots if s is not None)
            state = "running" if self.running else f"halted at 0x{self._regs()[0]:05X}"
            text = (f"{self.device.name}, read-only mode, {self.speed_khz} kHz, {state}, "
                    f"hardware breakpoints {used} of {len(self.slots)} in use\n")
        else:
            text = f"unknown monitor command '{command}'\n" + HELP
        self._send("O" + text.encode().hex())
        return "OK"

    def _monitor_read(self, args: list) -> str:
        """monitor read ADDR [WORDS]: inside the memory map and out of the BSL
        area, side-effect registers included; names the side effects."""
        try:
            if not 1 <= len(args) <= 2:
                raise ValueError
            addr = int(args[0], 0)
            nwords = int(args[1], 0) if len(args) == 2 else 1
        except ValueError:
            return READ_USAGE
        if addr < 0 or addr & 1 or not 1 <= nwords <= MONITOR_READ_WORDS:
            return READ_USAGE
        try:
            self._check_readable(addr, 2 * nwords)
        except Refused as e:
            self.say(f"refused: {e}")
            return f"refused: {e}\n"
        data = self.probe.read_mem(addr, nwords)
        words = struct.unpack(f"<{nwords}H", data)
        lines = [f"0x{addr + 2 * i:05X}: " + " ".join(f"{w:04X}" for w in words[i:i + 8])
                 for i in range(0, nwords, 8)]
        effects = []
        for a in range(addr, addr + 2 * nwords, 2):
            effect = self.device.debug_read_side_effect(a, 2)
            if effect is not None and effect not in effects:
                effects.append(effect)
        lines += [f"side effect: reading {name} (0x{lo:04X}) {what}"
                  for lo, _, name, what in effects]
        return "\n".join(lines) + "\n"

    def memory_map_xml(self) -> str:
        lines = ['<?xml version="1.0"?>',
                 '<!DOCTYPE memory-map PUBLIC "+//IDN gnu.org//DTD GDB Memory Map V1.0//EN" '
                 '"http://sourceware.org/gdb/gdb-memory-map.dtd">',
                 "<memory-map>"]
        for start, end, kind in self.device.memory_map():
            lines.append(f'  <memory type="{kind}" start="0x{start:x}" '
                         f'length="0x{end - start:x}"/>')
        lines.append("</memory-map>")
        return "\n".join(lines) + "\n"


def serve(server: GdbServer, port: int) -> None:
    """Accept GDB connections on 127.0.0.1 one at a time, until Ctrl-C
    (KeyboardInterrupt, which detaches an open session first)."""
    listener = socket.create_server((HOST, port), backlog=1)
    try:
        while True:
            bound = listener.getsockname()[1]
            server.say(f"waiting for GDB on {HOST}:{bound} (in GDB: target remote :{bound})")
            conn, _ = listener.accept()
            with conn:
                conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                try:
                    server.serve_connection(conn)
                except ProbeError as e:
                    server.say(f"error: {e}")
    finally:
        listener.close()
