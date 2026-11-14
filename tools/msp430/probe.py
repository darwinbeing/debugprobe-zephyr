"""USB access to the probe and the client side of the SBW vendor command (0x90).

Wire format, all fields little-endian:
    request:  [0x90][op][args...]      response: [0x90][status][data...]

The probe's bulk endpoints never send or expect zero-length packets, so a
packet whose length is a multiple of 64 bytes would not terminate the USB
transfer. `chunks()` picks word counts that avoid such lengths.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass

VID, PID = 0x2E8A, 0x000C
VENDOR_CMD = 0x90
PROTOCOL_VERSION = 2
# The flash commands have not changed since version 1.
FLASH_PROTOCOL_VERSIONS = tuple(range(1, PROTOCOL_VERSION + 1))
# The debug commands came with version 2; a later version may change them.
DEBUG_PROTOCOL_VERSIONS = (2,)
USB_PACKET = 64
ADDRESS_LIMIT = 1 << 20  # 20-bit addresses and register values

OP_INFO, OP_CONNECT, OP_DISCONNECT, OP_READ_MEM, OP_WRITE_MEM = 0, 1, 2, 3, 4
OP_ERASE, OP_FLASH_BEGIN, OP_FLASH_DATA, OP_FLASH_END = 5, 6, 7, 8
OP_DBG_ATTACH, OP_DBG_DETACH, OP_DBG_HALT, OP_DBG_RESUME, OP_DBG_STATUS = 9, 10, 11, 12, 13
OP_DBG_STEP, OP_DBG_READ_REGS, OP_DBG_WRITE_REG, OP_DBG_BP_SET, OP_DBG_BP_CLEAR = 14, 15, 16, 17, 18

OP_NAMES = {0: "INFO", 1: "CONNECT", 2: "DISCONNECT", 3: "READ_MEM", 4: "WRITE_MEM",
            5: "ERASE", 6: "FLASH_BEGIN", 7: "FLASH_DATA", 8: "FLASH_END",
            9: "DBG_ATTACH", 10: "DBG_DETACH", 11: "DBG_HALT", 12: "DBG_RESUME",
            13: "DBG_STATUS", 14: "DBG_STEP", 15: "DBG_READ_REGS", 16: "DBG_WRITE_REG",
            17: "DBG_BP_SET", 18: "DBG_BP_CLEAR"}

FEATURE_CPUXV2_FLASH = 1
FEATURE_DEBUG = 2

STATE_IDLE, STATE_CONNECTED, STATE_FLASHING, STATE_DEBUG_HALTED, STATE_DEBUG_RUNNING = range(5)
ATTACH_RUNNING, ATTACH_RESET = 0, 1  # attach without reset / reset and halt
DETACH_RUN, DETACH_RESET = 0, 1      # restore the context and run / reset and run
STOP_NONE, STOP_REQUEST, STOP_BREAKPOINT, STOP_STEP, STOP_RESET = range(5)
NO_BREAKPOINT = 0xFF    # DBG_STATUS: no breakpoint at the stop address
ALL_BREAKPOINTS = 0xFF  # DBG_BP_CLEAR: every breakpoint
MAX_HW_BREAKPOINTS = 8  # breakpoint indexes 0-7: the F5529's EEM triggers (B1's only device)

ENTRY_NORMAL, ENTRY_MAGIC_PATTERN = 0, 1
ERASE_SEGMENT, ERASE_BANK, ERASE_MAIN = 0, 1, 2

READ_SIZE = 4096  # bytes asked of the IN endpoint per read

# USB timeouts. A command waits TIMEOUT_MS for its reply, except the debug
# commands that may sync with the CPU on the probe (Debugger::sync_conditional,
# lib/msp430/src/debug.cpp). At worst a sync makes 3000 EEM-control polls,
# 30000 wait-signal polls and 10000 CPUSUSP polls: about 1.45 million JTAG
# cycles with what comes around them, at 3.5 SBW slots per cycle (sbw.pio),
# 20.3 s at 250 kHz, 50.8 s at 100 kHz. Those commands wait that long per
# sync at the session's SBW speed, a quarter more, plus TIMEOUT_MS, and at
# least SYNC_TIMEOUT_MIN_MS. DBG_ATTACH may sync twice: it first ends an open
# debug session (a running CPU is halted). DBG_HALT, DBG_STEP, DBG_STATUS (on
# a breakpoint stop) and DBG_DETACH (of a running CPU) sync once; DBG_RESUME
# only releases the CPU.
TIMEOUT_MS = 3000
SYNC_TIMEOUT_MIN_MS = 15000
SYNC_WORST_CYCLES = 1_450_000
SLOTS_PER_CYCLE_X2 = 7  # 3.5 slots per JTAG cycle, doubled to keep integers
MIN_SPEED_KHZ = 100     # the slowest SBW clock the probe accepts
SYNCS = {OP_DBG_ATTACH: 2, OP_DBG_HALT: 1, OP_DBG_STEP: 1, OP_DBG_STATUS: 1, OP_DBG_DETACH: 1}


def command_timeout_ms(op: int, speed_khz: int) -> int:
    """The USB timeout for `op` in a debug session at `speed_khz`."""
    syncs = SYNCS.get(op, 0)
    if not syncs:
        return TIMEOUT_MS
    # worst case in ms: syncs * cycles * 3.5 / kHz; with a quarter more: * 5 / 4
    worst = -(-syncs * SYNC_WORST_CYCLES * SLOTS_PER_CYCLE_X2 * 5 // (2 * 4 * speed_khz))
    return max(SYNC_TIMEOUT_MIN_MS, TIMEOUT_MS + worst)

STATUS_OK = 0
STATUS_BAD_ARG = 1
STATUS_BAD_STATE = 2
STATUS_BUSY = 3
STATUS_NO_TARGET = 4
STATUS_LOCKED = 5
STATUS_TARGET_ERROR = 6
STATUS_TIMEOUT = 7
STATUS_UNKNOWN_OP = 8
STATUS_PROTECTED = 9
STATUS_RUNNING = 10
STATUS_NAMES = {
    0: "OK",
    1: "BAD_ARG",
    2: "BAD_STATE",
    3: "BUSY (the SWD port is in use)",
    4: "NO_TARGET",
    5: "LOCKED (the JTAG lock key is programmed)",
    6: "TARGET_ERROR (lost synchronization with the CPU)",
    7: "TIMEOUT (no answer from the target)",
    8: "UNKNOWN_OP",
    9: "PROTECTED (read-only debug session: the probe does not write there)",
    10: "RUNNING (the CPU must be halted first)",
}
# Statuses that mean something particular for one operation.
OP_STATUS_NAMES = {
    (OP_DBG_STEP, STATUS_TIMEOUT):
        "TIMEOUT (the step did not complete in time; the probe stopped the CPU)",
    (OP_DBG_WRITE_REG, STATUS_PROTECTED):
        "PROTECTED (read-only mode: the PC may only be set inside main flash)",
}


OUT_OF_STEP = ("the probe's replies are out of step with the requests (an earlier run was "
               "interrupted); run the command again, and if it keeps happening unplug the "
               "probe and plug it in again")


class ProbeError(Exception):
    """Talking to the probe failed."""


class StatusError(ProbeError):
    """The probe answered a command with a non-zero status."""

    def __init__(self, op: int, status: int, data: bytes = b""):
        name = (OP_STATUS_NAMES.get((op, status))
                or STATUS_NAMES.get(status, f"status 0x{status:02X}"))
        super().__init__(f"{OP_NAMES.get(op, f'command 0x{op:02X}')} failed: {name}")
        self.op = op
        self.status = status
        self.data = data


@dataclass(frozen=True)
class Info:
    version: int
    state: int
    max_data_bytes: int
    features: int
    hw_breakpoints: int = 0  # protocol version 2


def supports_debugging(info: Info) -> bool:
    """The probe speaks the debug commands of this tool: a protocol version
    in DEBUG_PROTOCOL_VERSIONS and the debug feature."""
    return info.version in DEBUG_PROTOCOL_VERSIONS and bool(info.features & FEATURE_DEBUG)


@dataclass(frozen=True)
class Identity:
    jtag_id: int
    core_id: int
    device_id_ptr: int
    device_id: int


@dataclass(frozen=True)
class Attached:
    identity: Identity
    pc: int
    stop_reason: int
    hw_breakpoints: int


@dataclass(frozen=True)
class DebugStatus:
    halted: bool
    stop_reason: int
    breakpoint: int  # index of the breakpoint at the stop address, or NO_BREAKPOINT
    pc: int          # 0 while running


def _check(op: int, ok: bool, problem: str) -> None:
    """Refuse an out-of-range request argument before anything is sent."""
    if not ok:
        raise ProbeError(f"{OP_NAMES[op]}: {problem}")


def chunks(nwords: int, header_bytes: int, limit_words: int):
    """Yield word counts that add up to nwords, each at most limit_words and
    each giving a packet (header + data) that is not a multiple of 64 bytes."""
    while nwords > 0:
        n = min(nwords, limit_words)
        if n > 1 and (header_bytes + 2 * n) % USB_PACKET == 0:
            n -= 1
        yield n
        nwords -= n


class UsbTransport:
    """The probe's CMSIS-DAP v2 bulk interface (interface 0)."""

    def __init__(self, serial: str | None = None):
        try:
            import usb.core
            import usb.util
        except ImportError:
            raise ProbeError(
                "pyusb is not installed; run: pip install -r tools/msp430/requirements.txt"
            ) from None

        self._usb_util = usb.util
        self._usb_core = usb.core
        try:
            devs = list(usb.core.find(find_all=True, idVendor=VID, idProduct=PID))
        except usb.core.NoBackendError:
            raise ProbeError("libusb was not found; install it (macOS: brew install libusb)") from None

        def serial_of(dev) -> str:
            try:
                return dev.serial_number
            except (ValueError, usb.core.USBError, NotImplementedError):
                return "?"  # e.g. no permission to read the string descriptor

        if serial is not None:
            devs = [d for d in devs if serial_of(d) == serial]
        if not devs:
            raise ProbeError(f"probe {VID:04x}:{PID:04x} not found")
        if len(devs) > 1:
            found = ", ".join(serial_of(d) for d in devs)
            raise ProbeError(f"several probes found ({found}); choose one with --serial")
        self._dev = devs[0]

        intf = self._dev.get_active_configuration()[(0, 0)]
        self._out = next((e.bEndpointAddress for e in intf if not e.bEndpointAddress & 0x80), None)
        self._in = next((e.bEndpointAddress for e in intf if e.bEndpointAddress & 0x80), None)
        if self._out is None or self._in is None:
            raise ProbeError("bulk endpoints not found on interface 0")
        try:
            if self._dev.is_kernel_driver_active(0):
                self._dev.detach_kernel_driver(0)
        except NotImplementedError:
            pass  # macOS
        try:
            usb.util.claim_interface(self._dev, 0)
        except usb.core.USBError as e:
            raise ProbeError(
                f"cannot claim the probe's DAP interface ({e}); is openocd or another tool using it?"
            ) from None
        # An earlier, interrupted run may have left replies in the IN endpoint.
        for _ in range(16):
            try:
                self._dev.read(self._in, READ_SIZE, timeout=20)
            except usb.core.USBTimeoutError:
                break
            except usb.core.USBError as e:
                raise ProbeError(f"USB error: {e}") from None

    def transfer(self, request: bytes, timeout_ms: int = TIMEOUT_MS) -> bytes:
        core = self._usb_core
        try:
            self._dev.write(self._out, request, timeout=timeout_ms)
            return bytes(self._dev.read(self._in, READ_SIZE, timeout=timeout_ms))
        except core.USBTimeoutError:
            raise ProbeError(f"no answer from the probe within {timeout_ms} ms") from None
        except core.USBError as e:
            raise ProbeError(f"USB error: {e}") from None

    def close(self) -> None:
        try:
            self._usb_util.release_interface(self._dev, 0)
        except self._usb_core.USBError:
            pass
        try:
            self._usb_util.dispose_resources(self._dev)
        except self._usb_core.USBError:
            pass


class SbwProbe:
    """Client for the firmware's msp430::Session."""

    def __init__(self, transport):
        self._t = transport
        self.last_info = None
        self.max_words = 16
        self.read_words = 16
        self.write_words = 16
        self.debug_khz = None  # SBW speed of the last DBG_ATTACH; None: none sent yet

    def _cmd(self, op: int, payload: bytes = b"", speed_khz: int | None = None) -> bytes:
        """`speed_khz`: the SBW speed the command's syncs run at (see
        command_timeout_ms); by default the debug session's, the slowest
        possible before any DBG_ATTACH."""
        khz = speed_khz or self.debug_khz or MIN_SPEED_KHZ
        try:
            resp = self._t.transfer(bytes([VENDOR_CMD, op]) + payload,
                                    timeout_ms=command_timeout_ms(op, khz))
        except ProbeError as e:
            raise ProbeError(f"{OP_NAMES.get(op, f'command 0x{op:02X}')}: {e}") from None
        if resp == bytes([VENDOR_CMD]):
            raise ProbeError(
                "the probe does not answer the MSP430 SBW command; its firmware is "
                "too old or was built without CONFIG_PROBE_MSP430_SBW"
            )
        if len(resp) < 2 or resp[0] != VENDOR_CMD:
            raise ProbeError(OUT_OF_STEP)
        if resp[1] != STATUS_OK:
            raise StatusError(op, resp[1], resp[2:])
        return resp[2:]

    def dap_disconnect(self) -> None:
        """Standard DAP_Disconnect: make sure the SWD port has released the pins."""
        resp = self._t.transfer(b"\x03")
        if not resp or resp[0] != 0x03:
            raise ProbeError(OUT_OF_STEP)

    def info(self) -> Info:
        data = self._cmd(OP_INFO)
        if len(data) < 8:
            raise ProbeError(f"INFO: expected 8 bytes, got {len(data)}")
        version = data[0]
        if version >= 2 and len(data) < 9:  # version 2 added hw_breakpoints
            raise ProbeError(
                f"INFO: expected 9 bytes from a version {version} probe, got {len(data)}")
        info = Info(*struct.unpack("<BBHI", data[:8]),
                    hw_breakpoints=data[8] if version >= 2 else 0)
        self.max_words = info.max_data_bytes // 2
        self.last_info = info
        return info

    def configure(self, speed_khz: int) -> None:
        """Size the data commands so that each takes roughly 100 ms on the probe."""
        self.read_words = min(self.max_words, max(32, speed_khz * 5 // 4))
        self.write_words = min(self.max_words, max(32, speed_khz * 3 // 8))

    def connect(self, speed_khz: int, entry_mode: int) -> Identity:
        data = self._cmd(OP_CONNECT, struct.pack("<HB", speed_khz, entry_mode))
        if len(data) < 9:
            raise ProbeError(OUT_OF_STEP)
        return Identity(*struct.unpack("<BHIH", data[:9]))

    def disconnect(self) -> None:
        self._cmd(OP_DISCONNECT)

    def read_mem(self, addr: int, nwords: int) -> bytes:
        out = bytearray()
        for n in chunks(nwords, 2, self.read_words):
            at = addr + len(out)
            part = self._cmd(OP_READ_MEM, struct.pack("<IH", at, n))
            if len(part) != 2 * n:
                raise ProbeError(f"READ_MEM: expected {2 * n} bytes at 0x{at:05X}, got {len(part)}")
            out += part
        return bytes(out)

    def write_mem(self, addr: int, data: bytes) -> None:
        """Write RAM or peripherals (the firmware cannot program flash this way)."""
        if not data or len(data) % 2:
            raise ProbeError("WRITE_MEM: the data must be a non-empty whole number of words")
        done = 0
        for n in chunks(len(data) // 2, 6, self.write_words):
            part = data[done:done + 2 * n]
            self._cmd(OP_WRITE_MEM, struct.pack("<I", addr + done) + part)
            done += 2 * n

    def erase(self, mode: int, addr: int, ram_addr: int) -> None:
        self._cmd(OP_ERASE, struct.pack("<BII", mode, addr, ram_addr))

    def flash_begin(self, addr: int, nwords: int, ram_addr: int) -> None:
        self._cmd(OP_FLASH_BEGIN, struct.pack("<III", addr, nwords, ram_addr))

    def flash_data(self, data: bytes) -> None:
        """One FLASH_DATA command; the caller splits with chunks(..., 2, write_words)."""
        self._cmd(OP_FLASH_DATA, data)

    def flash_end(self) -> None:
        self._cmd(OP_FLASH_END)

    # --- debugging (protocol version 2) ---

    def dbg_attach(self, mode: int, speed_khz: int, ram_lo: int, ram_hi: int) -> Attached:
        """Attach without reset (ATTACH_RUNNING) or reset and halt (ATTACH_RESET).
        ram_lo/ram_hi: the RAM a read-only session may write (the probe narrows
        it to the RAM it knows for the device)."""
        # an open debug session is ended at its own speed first
        slowest = min(speed_khz, self.debug_khz or speed_khz)
        self.debug_khz = speed_khz
        data = self._cmd(OP_DBG_ATTACH, struct.pack("<BBHII", mode, 0, speed_khz, ram_lo, ram_hi),
                         speed_khz=slowest)
        if len(data) < 15:
            raise ProbeError(OUT_OF_STEP)
        identity = Identity(*struct.unpack("<BHIH", data[:9]))
        pc, reason, nbp = struct.unpack("<IBB", data[9:15])
        return Attached(identity, pc, reason, nbp)

    def dbg_detach(self, action: int = DETACH_RUN) -> None:
        self._cmd(OP_DBG_DETACH, bytes([action]))

    def dbg_halt(self) -> int:
        """Stop a running CPU (OK when halted already); returns the PC."""
        return self._u32(self._cmd(OP_DBG_HALT))

    def dbg_resume(self) -> None:
        self._cmd(OP_DBG_RESUME)

    def dbg_status(self) -> DebugStatus:
        data = self._cmd(OP_DBG_STATUS)
        if len(data) < 7:
            raise ProbeError(OUT_OF_STEP)
        state, reason, bp, pc = struct.unpack("<BBBI", data[:7])
        return DebugStatus(state == 1, reason, bp, pc)

    def dbg_step(self) -> int:
        """One instruction; returns the new PC. A StatusError with TIMEOUT
        means the CPU did not stop by itself: the probe halted it (stop
        reason STOP_REQUEST)."""
        return self._u32(self._cmd(OP_DBG_STEP))

    def dbg_read_regs(self) -> list:
        data = self._cmd(OP_DBG_READ_REGS)
        if len(data) < 64:
            raise ProbeError(OUT_OF_STEP)
        return list(struct.unpack("<16I", data[:64]))

    def dbg_write_reg(self, index: int, value: int) -> None:
        _check(OP_DBG_WRITE_REG, 0 <= index <= 15, f"register index {index} is not 0-15")
        _check(OP_DBG_WRITE_REG, 0 <= value < ADDRESS_LIMIT, f"value {value:#x} is not 20 bits")
        self._cmd(OP_DBG_WRITE_REG, struct.pack("<BI", index, value))

    def dbg_bp_set(self, index: int, addr: int) -> None:
        _check(OP_DBG_BP_SET, 0 <= index < MAX_HW_BREAKPOINTS,
               f"breakpoint index {index} is not 0-{MAX_HW_BREAKPOINTS - 1}")
        _check(OP_DBG_BP_SET, 0 <= addr < ADDRESS_LIMIT, f"address {addr:#x} is not 20 bits")
        self._cmd(OP_DBG_BP_SET, struct.pack("<BI", index, addr))

    def dbg_bp_clear(self, index: int = ALL_BREAKPOINTS) -> None:
        """Clear breakpoint `index`; ALL_BREAKPOINTS (the default) clears all."""
        _check(OP_DBG_BP_CLEAR, 0 <= index < MAX_HW_BREAKPOINTS or index == ALL_BREAKPOINTS,
               f"breakpoint index {index} is not 0-{MAX_HW_BREAKPOINTS - 1} "
               f"or 0x{ALL_BREAKPOINTS:02X}")
        self._cmd(OP_DBG_BP_CLEAR, bytes([index]))

    @staticmethod
    def _u32(data: bytes) -> int:
        if len(data) < 4:
            raise ProbeError(OUT_OF_STEP)
        return struct.unpack("<I", data[:4])[0]
