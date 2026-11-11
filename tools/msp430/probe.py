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
PROTOCOL_VERSION = 1
USB_PACKET = 64

OP_INFO, OP_CONNECT, OP_DISCONNECT, OP_READ_MEM, OP_WRITE_MEM = 0, 1, 2, 3, 4
OP_ERASE, OP_FLASH_BEGIN, OP_FLASH_DATA, OP_FLASH_END = 5, 6, 7, 8

OP_NAMES = {0: "INFO", 1: "CONNECT", 2: "DISCONNECT", 3: "READ_MEM", 4: "WRITE_MEM",
            5: "ERASE", 6: "FLASH_BEGIN", 7: "FLASH_DATA", 8: "FLASH_END"}

FEATURE_CPUXV2_FLASH = 1

ENTRY_NORMAL, ENTRY_MAGIC_PATTERN = 0, 1
ERASE_SEGMENT, ERASE_BANK, ERASE_MAIN = 0, 1, 2

READ_SIZE = 4096  # bytes asked of the IN endpoint per read

STATUS_OK = 0
STATUS_BUSY = 3
STATUS_NO_TARGET = 4
STATUS_LOCKED = 5
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
}


OUT_OF_STEP = ("the probe's replies are out of step with the requests (an earlier run was "
               "interrupted); run the command again, and if it keeps happening unplug the "
               "probe and plug it in again")


class ProbeError(Exception):
    """Talking to the probe failed."""


class StatusError(ProbeError):
    """The probe answered a command with a non-zero status."""

    def __init__(self, op: int, status: int, data: bytes = b""):
        name = STATUS_NAMES.get(status, f"status 0x{status:02X}")
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


@dataclass(frozen=True)
class Identity:
    jtag_id: int
    core_id: int
    device_id_ptr: int
    device_id: int


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

    def transfer(self, request: bytes, timeout_ms: int = 3000) -> bytes:
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
        self.max_words = 16
        self.read_words = 16
        self.write_words = 16

    def _cmd(self, op: int, payload: bytes = b"") -> bytes:
        try:
            resp = self._t.transfer(bytes([VENDOR_CMD, op]) + payload)
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
        info = Info(*struct.unpack("<BBHI", data[:8]))
        self.max_words = info.max_data_bytes // 2
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

    def erase(self, mode: int, addr: int, ram_addr: int) -> None:
        self._cmd(OP_ERASE, struct.pack("<BII", mode, addr, ram_addr))

    def flash_begin(self, addr: int, nwords: int, ram_addr: int) -> None:
        self._cmd(OP_FLASH_BEGIN, struct.pack("<III", addr, nwords, ram_addr))

    def flash_data(self, data: bytes) -> None:
        """One FLASH_DATA command; the caller splits with chunks(..., 2, write_words)."""
        self._cmd(OP_FLASH_DATA, data)

    def flash_end(self) -> None:
        self._cmd(OP_FLASH_END)
