"""Stands in for the probe in tests: answers DAP_Disconnect and the SBW vendor
command the way the firmware's msp430::Session does, on top of a flash model."""
import struct

OK, BAD_ARG, BAD_STATE, NO_TARGET, TIMEOUT, UNKNOWN_OP = 0, 1, 2, 4, 7, 8
MAIN = (0x4400, 0x24400)

# Argument bytes of the fixed-size requests; the firmware rejects any other length.
FIXED_ARGS = {0: 0, 1: 3, 2: 0, 3: 6, 5: 9, 6: 12, 8: 0}


class FakeFirmware:
    def __init__(self, device_id=0x2955):
        self.device_id = device_id
        self.mem = {}                # address -> byte; missing = erased (0xFF)
        self.state = 0               # 0 idle, 1 connected, 2 flashing
        self.no_target = {}          # entry mode -> NO_TARGET answers still to give
        self.no_target_id = 0xFF
        self.connects = []           # entry modes of every CONNECT
        self.ops = []                # ("erase", mode, addr, ram), ("begin", addr, nwords, ram), ...
        self.fail_flash_after = None # words accepted before FLASH_DATA answers TIMEOUT
        self.stuck = {}              # address -> byte that reads back regardless
        self.has_handler = True
        self.closed = False
        self._flash_addr = 0
        self._remaining = 0
        self._written = 0

    # --- transport interface used by SbwProbe ---
    def transfer(self, request, timeout_ms=3000):
        request = bytes(request)
        assert len(request) % 64 != 0, "request length is a multiple of 64"
        response = self._handle(request)
        assert len(response) % 64 != 0, "response length is a multiple of 64"
        return response

    def close(self):
        self.closed = True

    # --- firmware model ---
    def _handle(self, req):
        if req[0] == 0x03:  # DAP_Disconnect
            self.ops.append(("dap_disconnect",))
            return b"\x03\x00"
        if req[0] != 0x90 or not self.has_handler:
            return req[:1]  # vendor echo
        if len(req) < 2:
            return bytes([0x90, BAD_ARG])
        status, data = self._op(req[1], req[2:])
        return bytes([0x90, status]) + data

    def _op(self, op, a):
        bad_len = op in FIXED_ARGS and len(a) != FIXED_ARGS[op]
        if op == 0:
            if bad_len:
                return BAD_ARG, b""
            return OK, struct.pack("<BBHI", 1, self.state, 1016, 1)
        if op == 1:
            if bad_len:
                return BAD_ARG, b""
            _speed, mode = struct.unpack("<HB", a[:3])
            self.connects.append(mode)
            self.state = 0
            if self.no_target.get(mode, 0) > 0:
                self.no_target[mode] -= 1
                return NO_TARGET, bytes([self.no_target_id])
            self.state = 1
            return OK, struct.pack("<BHIH", 0x91, 0x0106, 0x1A00, self.device_id)
        if op == 2:
            if self.state == 0:
                return BAD_STATE, b""
            if bad_len:
                return BAD_ARG, b""
            self.state = 0
            self.ops.append(("disconnect",))
            return OK, b""
        if op == 3:
            if self.state != 1:
                return BAD_STATE, b""
            if bad_len:
                return BAD_ARG, b""
            addr, n = struct.unpack("<IH", a[:6])
            return OK, bytes(self.stuck.get(x, self.mem.get(x, 0xFF))
                             for x in range(addr, addr + 2 * n))
        if op == 5:
            if self.state != 1:
                return BAD_STATE, b""
            if bad_len:
                return BAD_ARG, b""
            mode, addr, ram = struct.unpack("<BII", a[:9])
            self.ops.append(("erase", mode, addr, ram))
            if mode == 2:
                lo, hi = MAIN
            else:
                size = 512 if addr >= MAIN[0] else 128
                lo = addr - addr % size
                hi = lo + size
            for x in [x for x in self.mem if lo <= x < hi]:
                del self.mem[x]
            return OK, b""
        if op == 6:
            if self.state != 1:
                return BAD_STATE, b""
            if bad_len:
                return BAD_ARG, b""
            addr, nwords, ram = struct.unpack("<III", a[:12])
            self.ops.append(("begin", addr, nwords, ram))
            self._flash_addr, self._remaining, self.state = addr, nwords, 2
            return OK, b""
        if op == 7:
            if self.state != 2:
                return BAD_STATE, b""
            n = len(a) // 2
            if n == 0 or len(a) % 2 or n > self._remaining:
                self.state = 1
                return BAD_ARG, b""
            if self.fail_flash_after is not None and self._written + n > self.fail_flash_after:
                self.state = 1
                return TIMEOUT, b""
            for i, b in enumerate(a):  # flash can only clear bits
                x = self._flash_addr + i
                self.mem[x] = self.mem.get(x, 0xFF) & b
            self._flash_addr += len(a)
            self._remaining -= n
            self._written += n
            return OK, b""
        if op == 8:
            if self.state != 2:
                return BAD_STATE, b""
            if bad_len:
                return BAD_ARG, b""
            self.state = 1
            self.ops.append(("end",))
            return (OK if self._remaining == 0 else BAD_STATE), b""
        return UNKNOWN_OP, b""
