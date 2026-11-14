"""Stands in for the probe in tests: answers DAP_Disconnect and the SBW vendor
command the way the firmware's msp430::Session does, on top of a flash model.

In a Phase A session op 4 (WRITE_MEM) takes effect only in RAM
(0x2400-0x43FF); like the real device it accepts, but ignores, writes
addressed to flash or anywhere else.

Fault injection knobs: version, features, connect_status, fail_op,
interrupt_on_op, fail_flash_after, stuck, no_target, has_handler, also_erase,
skip_segment_erases, read_shift; for debugging: attach_status, run_polls,
stop_at, halt_pc, step_size, step_timeout_pc, lost_on_poll, hw_breakpoints.
`polls` counts the looks at the running CPU since the last DBG_RESUME.

Debug model (ops 9-18, states 3 DebugHalted / 4 DebugRunning; a probe of
version 1 answers them UNKNOWN_OP), after msp430::Session in
lib/msp430/src/session.cpp, which is the authority: sixteen registers, eight
hardware breakpoints, and a CPU that, once resumed, is found stopped by the
`run_polls`-th look after DBG_RESUME (each DBG_STATUS looks, and DBG_HALT
looks before it stops the CPU itself, as Debugger::halt does): at `stop_at`,
or at the breakpoint with the lowest index if `stop_at` is None; with neither
it runs until DBG_HALT. DBG_STEP advances the PC by `step_size`, not at all
while SR has CPUOFF. Checks in the firmware's order: state, then RUNNING,
then size, then arguments. In a debug session WRITE_MEM writes only the RAM
the host passed with DBG_ATTACH narrowed to the device's, and the
peripherals minus the device's holes (PMM, flash controller, SYS, DMA), both
from devices.py; nothing on a device that table does not know. DBG_WRITE_REG
moves the PC only into main flash or leaves it where it is. Elsewhere, and
for ERASE and FLASH_BEGIN, it answers PROTECTED. CONNECT and DBG_ATTACH end
an open debug session as DBG_DETACH 0 does."""
import struct

import devices  # tools/msp430, which the test modules put on sys.path

OK, BAD_ARG, BAD_STATE, BUSY, NO_TARGET, LOCKED, TARGET_ERROR, TIMEOUT, UNKNOWN_OP = range(9)
PROTECTED, RUNNING = 9, 10
MAIN = (0x4400, 0x24400)
RAM = (0x2400, 0x4400)
HALTED, RUNNING_STATE = 3, 4  # session states DebugHalted, DebugRunning
STOP_NONE, STOP_REQUEST, STOP_BREAKPOINT, STOP_STEP, STOP_RESET = range(5)
NO_BREAKPOINT = 0xFF    # DBG_STATUS: no breakpoint at the stop address
ALL_BREAKPOINTS = 0xFF  # DBG_BP_CLEAR: every breakpoint
HW_BREAKPOINTS = 8
SR_CPUOFF = 0x0010

# Argument bytes of the fixed-size requests; the firmware rejects any other length.
FIXED_ARGS = {0: 0, 1: 3, 2: 0, 3: 6, 5: 9, 6: 12, 8: 0,
              9: 12, 10: 1, 11: 0, 12: 0, 13: 0, 14: 0, 15: 0, 16: 5, 17: 5, 18: 1}
# The debug ops a running CPU allows: DBG_DETACH, DBG_HALT, DBG_STATUS.
RUNNING_OPS = (10, 11, 13)
# A TARGET_ERROR from these leaves the Debugger attached and the Session
# detaches it with Run; from DBG_HALT, DBG_STATUS and DBG_STEP the Debugger
# has already let the CPU go.
DETACHING_OPS = (3, 4, 15, 16, 17, 18)

ADDR_LIMIT = 0x100000    # 20-bit address space
MAX_DATA_BYTES = 1016    # data bytes per command
ERASE_FUNCLET_BYTES = 178
FLASH_FUNCLET_BYTES = 240


class FakeFirmware:
    def __init__(self, device_id=0x2955, version=2, features=3):
        self.device_id = device_id
        self.version = version       # reported by INFO
        self.features = features     # reported by INFO
        self.mem = {}                # address -> byte; missing = erased (0xFF)
        self.mem[0x1A04] = device_id & 0xFF   # the device descriptor
        self.mem[0x1A05] = device_id >> 8
        self.state = 0               # 0 idle, 1 connected, 2 flashing, 3/4 debug halted/running
        self.no_target = {}          # entry mode -> NO_TARGET answers still to give
        self.no_target_id = 0xFF
        self.connect_status = None   # if set: CONNECT answers this status, stays idle
        self.fail_op = {}            # op -> status answered after the firmware's checks
        self.interrupt_on_op = None  # op that raises KeyboardInterrupt in transfer, once
        self.connects = []           # entry modes of every accepted CONNECT
        self.speeds = []             # speed_khz of every accepted CONNECT
        self.data_chunks = []        # word count of every accepted FLASH_DATA
        self.read_chunks = []        # word count of every accepted READ_MEM
        self.write_chunks = []       # word count of every accepted WRITE_MEM
        self.reads = []              # (addr, nwords) of every accepted READ_MEM
        self.writes = []             # (addr, data) of every accepted WRITE_MEM
        self.ops = []                # ("erase", mode, addr, ram), ("begin", addr, nwords, ram), ...
        self.fail_flash_after = None # words accepted before FLASH_DATA answers TIMEOUT
        self.stuck = {}              # address -> byte that reads back regardless
        self.has_handler = True
        self.skip_segment_erases = set()  # 1-based ordinals of mode-0 erases that do nothing
        self.read_shift = 0          # READ_MEM returns the bytes this many bytes further on
        self._segment_erases = 0
        self.also_erase = None       # address: a SEGMENT erase also erases the segment holding it
        self.closed = False
        self._flash_addr = 0
        self._remaining = 0
        self._written = 0
        # --- debug model ---
        self.regs = [0x4520, 0x43F0, 0x0008, 0] + [0x0400 + i for i in range(4, 16)]
        self.reset_pc = 0x441A
        self.breakpoints = {}        # index -> address
        self.attach_status = None    # if set: DBG_ATTACH answers this status, ends idle
        self.run_polls = 2           # the look after DBG_RESUME that finds the CPU stopped
        self.stop_at = None          # where a resumed CPU stops (None: lowest-index breakpoint)
        self.halt_pc = 0x4ABC        # PC after DBG_HALT stops a running CPU
        self.step_size = 2           # bytes DBG_STEP advances the PC
        self.step_timeout_pc = None  # PC after a DBG_STEP that answers TIMEOUT
                                     # (fail_op {14: TIMEOUT}); None: unchanged
        self.lost_on_poll = False    # the target is lost while running: DBG_STATUS and
                                     # DBG_HALT answer TARGET_ERROR, the session ends
        self.hw_breakpoints = HW_BREAKPOINTS  # EEM triggers INFO and DBG_ATTACH report
        self.stop_reason = STOP_NONE
        self.bp_hit = NO_BREAKPOINT
        self.ram_range = (0, 0)      # RAM a debug session may write, [lo, hi)
        self.debug_device = None     # the devices.Device identified by DBG_ATTACH
        self.attaches = []           # (mode, flags, speed, ram_lo, ram_hi), every valid DBG_ATTACH
        self.detaches = []           # action of every detach, also the implicit ones
        self.resumes = 0
        self.steps = 0
        self.reg_writes = []         # (index, value) of every accepted DBG_WRITE_REG
        self.polls = 0               # looks at the running CPU since the last DBG_RESUME

    # --- transport interface used by SbwProbe ---
    def transfer(self, request, timeout_ms=3000):
        request = bytes(request)
        assert len(request) % 64 != 0, "request length is a multiple of 64"
        if (self.interrupt_on_op is not None and len(request) > 1
                and request[0] == 0x90 and request[1] == self.interrupt_on_op):
            self.interrupt_on_op = None
            raise KeyboardInterrupt
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

    def _fail(self, op):
        status = self.fail_op[op]
        if status == TARGET_ERROR:
            # Session::handle closes the session; a halted debug session is
            # detached with Run unless the Debugger has let the CPU go itself
            self._end_debug(0 if self.state == HALTED and op in DETACHING_OPS else None)
        elif op in (7, 8):
            self.state = 1
        elif op == 14 and status == TIMEOUT:
            # the step did not stop by itself: the probe halted the CPU
            if self.step_timeout_pc is not None:
                self.regs[0] = self.step_timeout_pc
            self.stop_reason, self.bp_hit = STOP_REQUEST, NO_BREAKPOINT
        return status, b""

    def _end_debug(self, action):
        """Idle; an open debug session ends: detached with `action` (recorded),
        or let go by the Debugger itself when `action` is None."""
        if action is not None and self.state in (HALTED, RUNNING_STATE):
            self.detaches.append(action)
        self.breakpoints = {}
        self.ram_range = (0, 0)
        self.debug_device = None
        self.state = 0

    def _abort(self):
        """Session::abort(): an open debug session is detached with Run."""
        self._end_debug(0)

    def _debug_writable(self, addr, n):
        """Session::writable: inside the RAM window, or inside the device's
        peripherals and clear of its holes."""
        end = addr + n
        lo, hi = self.ram_range
        if lo <= addr and end <= hi:
            return True
        d = self.debug_device
        if d is None:
            return False
        return (d.peripherals[0] <= addr and end <= d.peripherals[1]
                and not any(addr < h_hi and end > h_lo for h_lo, h_hi, _ in d.peripheral_holes))

    def _pc_writable(self, pc):
        """Session's PC rule: into main flash or unchanged; only unchanged on a
        device devices.py does not know."""
        if self.debug_device is None:
            return pc == self.regs[0]
        return self.debug_device.debug_pc_writable(pc, self.regs[0])

    def _debug_check(self, op, a):
        """Ops 10-18 in the firmware's order: state, RUNNING, size, arguments.
        None if the operation may go ahead."""
        if self.state not in (HALTED, RUNNING_STATE):
            return BAD_STATE
        if self.state == RUNNING_STATE and op not in RUNNING_OPS:
            return RUNNING
        if len(a) != FIXED_ARGS[op]:
            return BAD_ARG
        if op == 10 and a[0] > 1:
            return BAD_ARG
        if op == 16:
            index, value = struct.unpack("<BI", a)
            # Session's PC rule first; an odd PC is the Debugger's BAD_ARG
            if index == 0 and not value & 1 and not self._pc_writable(value):
                return PROTECTED
            if (index > 15 or index == 3 or value >= ADDR_LIMIT or (index == 0 and value & 1)
                    or (index == 2 and value > 0xFFFF)):
                return BAD_ARG
        if op == 17:
            index, addr = struct.unpack("<BI", a)
            if index >= self.hw_breakpoints or addr & 1 or addr >= ADDR_LIMIT:
                return BAD_ARG
        if op == 18 and a[0] != ALL_BREAKPOINTS and a[0] >= self.hw_breakpoints:
            return BAD_ARG
        return None

    def _op(self, op, a):
        if 9 <= op <= 18 and self.version < 2:
            return UNKNOWN_OP, b""  # the debug ops came with protocol version 2
        bad_len = op in FIXED_ARGS and len(a) != FIXED_ARGS[op]
        if op == 0:
            if bad_len:
                return BAD_ARG, b""
            if op in self.fail_op:
                return self._fail(op)
            info = struct.pack("<BBHI", self.version, self.state, MAX_DATA_BYTES, self.features)
            return OK, info + (bytes([self.hw_breakpoints]) if self.version >= 2 else b"")
        if op == 1:
            if bad_len:
                return BAD_ARG, b""
            speed, mode = struct.unpack("<HB", a[:3])
            if not 100 <= speed <= 1000 or mode > 1:
                return BAD_ARG, b""
            self.connects.append(mode)
            self.speeds.append(speed)
            self._abort()
            if self.connect_status is not None:
                return self.connect_status, b""
            if self.no_target.get(mode, 0) > 0:
                self.no_target[mode] -= 1
                return NO_TARGET, bytes([self.no_target_id])
            self.state = 1
            return OK, struct.pack("<BHIH", 0x91, 0x0106, 0x1A00, self.device_id)
        if op == 2:
            if self.state in (0, HALTED, RUNNING_STATE):  # a debug session ends with DBG_DETACH
                return BAD_STATE, b""
            if bad_len:
                return BAD_ARG, b""
            if op in self.fail_op:
                return self._fail(op)
            self.state = 0
            self.ops.append(("disconnect",))
            return OK, b""
        if op == 3:
            if self.state == RUNNING_STATE:
                return RUNNING, b""
            if self.state not in (1, HALTED):
                return BAD_STATE, b""
            if bad_len:
                return BAD_ARG, b""
            addr, n = struct.unpack("<IH", a[:6])
            if n == 0 or addr & 1 or addr + 2 * n > ADDR_LIMIT or 2 * n > MAX_DATA_BYTES:
                return BAD_ARG, b""
            if op in self.fail_op:
                return self._fail(op)
            self.read_chunks.append(n)
            self.reads.append((addr, n))
            return OK, bytes(self.stuck.get(x, self.mem.get(x, 0xFF))
                             for x in range(addr + self.read_shift, addr + self.read_shift + 2 * n))
        if op == 4:
            if self.state == RUNNING_STATE:
                return RUNNING, b""
            if self.state not in (1, HALTED):
                return BAD_STATE, b""
            if len(a) < 4:
                return BAD_ARG, b""
            addr = struct.unpack("<I", a[:4])[0]
            data = a[4:]
            if not data or len(data) % 2 or len(data) > MAX_DATA_BYTES or addr & 1:
                return BAD_ARG, b""
            # The whitelist before the range check: a range that leaves the
            # 20-bit space lies outside every window.
            if self.state == HALTED and not self._debug_writable(addr, len(data)):
                return PROTECTED, b""
            if addr + len(data) > ADDR_LIMIT:
                return BAD_ARG, b""
            if op in self.fail_op:
                return self._fail(op)
            self.write_chunks.append(len(data) // 2)
            self.writes.append((addr, bytes(data)))
            for i, b in enumerate(data):
                if self.state == HALTED or RAM[0] <= addr + i < RAM[1]:
                    self.mem[addr + i] = b
            return OK, b""
        if op == 5:
            if self.state in (HALTED, RUNNING_STATE):
                return PROTECTED, b""  # no flash writes in a debug session, even malformed
            if self.state != 1:
                return BAD_STATE, b""
            if bad_len:
                return BAD_ARG, b""
            mode, addr, ram = struct.unpack("<BII", a[:9])
            if (mode > 2 or addr & 1 or addr >= ADDR_LIMIT
                    or ram & 1 or ram + ERASE_FUNCLET_BYTES > ADDR_LIMIT):
                return BAD_ARG, b""
            if op in self.fail_op:
                return self._fail(op)
            self.ops.append(("erase", mode, addr, ram))
            if mode == 0:
                self._segment_erases += 1
                if self._segment_erases in self.skip_segment_erases:
                    return OK, b""
            if mode == 2:
                lo, hi = MAIN
            else:
                size = 512 if addr >= MAIN[0] else 128
                lo = addr - addr % size
                hi = lo + size
            spans = [(lo, hi)]
            if mode == 0 and self.also_erase is not None:
                a = self.also_erase
                size = 512 if a >= MAIN[0] else 128
                spans.append((a - a % size, a - a % size + size))
            for lo, hi in spans:
                for x in [x for x in self.mem if lo <= x < hi]:
                    del self.mem[x]
            return OK, b""
        if op == 6:
            if self.state in (HALTED, RUNNING_STATE):
                return PROTECTED, b""
            if self.state != 1:
                return BAD_STATE, b""
            if bad_len:
                return BAD_ARG, b""
            addr, nwords, ram = struct.unpack("<III", a[:12])
            if (nwords == 0 or addr & 1 or addr + 2 * nwords > ADDR_LIMIT
                    or ram & 1 or ram + FLASH_FUNCLET_BYTES > ADDR_LIMIT):
                return BAD_ARG, b""
            if op in self.fail_op:
                return self._fail(op)
            self.ops.append(("begin", addr, nwords, ram))
            self._flash_addr, self._remaining, self.state = addr, nwords, 2
            return OK, b""
        if op == 7:
            if self.state != 2:
                return BAD_STATE, b""
            n = len(a) // 2
            if len(a) > MAX_DATA_BYTES or n == 0 or len(a) % 2 or n > self._remaining:
                self.state = 1
                return BAD_ARG, b""
            if op in self.fail_op:
                return self._fail(op)
            if self.fail_flash_after is not None and self._written + n > self.fail_flash_after:
                self.state = 1
                return TIMEOUT, b""
            for i, b in enumerate(a):  # flash can only clear bits
                x = self._flash_addr + i
                self.mem[x] = self.mem.get(x, 0xFF) & b
            self._flash_addr += len(a)
            self._remaining -= n
            self._written += n
            self.data_chunks.append(n)
            return OK, b""
        if op == 8:
            if self.state != 2:
                return BAD_STATE, b""
            if bad_len:
                return BAD_ARG, b""
            if op in self.fail_op:
                return self._fail(op)
            self.state = 1
            self.ops.append(("end",))
            return (OK if self._remaining == 0 else BAD_STATE), b""
        if op == 9:
            return self._dbg_attach(a)
        if 10 <= op <= 18:
            status = self._debug_check(op, a)
            if status is not None:
                return status, b""
            if op in self.fail_op:
                return self._fail(op)
            return self._dbg_op(op, a)
        return UNKNOWN_OP, b""

    def _dbg_attach(self, a):
        """Session::op_dbg_attach: BAD_STATE in Connected and Flashing; the
        arguments are checked before anything else, so a bad request leaves an
        open debug session as it was; then that session is detached with Run
        and a new one attached. A failure leaves the probe idle."""
        if self.state in (1, 2):
            return BAD_STATE, b""
        if len(a) != FIXED_ARGS[9]:
            return BAD_ARG, b""
        mode, flags, speed, lo, hi = struct.unpack("<BBHII", a)
        if (mode > 1 or flags != 0 or not 100 <= speed <= 1000 or lo & 1 or lo >= hi
                or hi > ADDR_LIMIT):
            return BAD_ARG, b""
        self.attaches.append((mode, flags, speed, lo, hi))
        self._abort()
        if self.attach_status is not None:
            no_target_id = bytes([self.no_target_id]) if self.attach_status == NO_TARGET else b""
            return self.attach_status, no_target_id
        if 9 in self.fail_op:
            return self._fail(9)
        self.state = HALTED
        self.breakpoints = {}  # the attach clears every EEM trigger
        # the device's limits; the writable RAM is the host's range narrowed to the device's
        self.debug_device = devices.lookup(self.device_id)
        if self.debug_device is not None:
            lo, hi = max(lo, self.debug_device.ram[0]), min(hi, self.debug_device.ram[1])
            self.ram_range = (lo, hi) if lo < hi else (0, 0)
        if mode == 1:
            self.regs[0], self.regs[2] = self.reset_pc, 0
        self.stop_reason = STOP_RESET if mode == 1 else STOP_REQUEST
        self.bp_hit = NO_BREAKPOINT
        identity = struct.pack("<BHIH", 0x91, 0x0106, 0x1A00, self.device_id)
        return OK, identity + struct.pack("<IBB", self.regs[0], self.stop_reason,
                                          self.hw_breakpoints)

    def _stop(self, pc, reason):
        self.state = HALTED
        self.regs[0] = pc
        self.stop_reason = reason
        # only a breakpoint stop names the breakpoint at the stop address (Debugger::poll)
        self.bp_hit = NO_BREAKPOINT
        if reason == STOP_BREAKPOINT:
            self.bp_hit = next((i for i, x in sorted(self.breakpoints.items()) if x == pc),
                               NO_BREAKPOINT)

    def _poll(self):
        """Debugger::poll of a running CPU: TARGET_ERROR when the target is lost
        (the session ends), else None; the CPU may now be stopped."""
        if self.lost_on_poll:
            self._end_debug(None)
            return TARGET_ERROR
        self.polls += 1
        target = self.stop_at
        if target is None and self.breakpoints:
            target = self.breakpoints[min(self.breakpoints)]
        if target is not None and self.polls >= self.run_polls:
            self._stop(target, STOP_BREAKPOINT)
        return None

    def _dbg_op(self, op, a):
        """Ops 10-18 once _debug_check has passed them."""
        if op == 10:  # DBG_DETACH
            self._end_debug(a[0])
            return OK, b""
        if op == 11:  # DBG_HALT
            if self.state == RUNNING_STATE:
                status = self._poll()  # Debugger::halt looks as poll() does first
                if status is not None:
                    return status, b""
            if self.state == RUNNING_STATE:
                self._stop(self.halt_pc, STOP_REQUEST)
            return OK, struct.pack("<I", self.regs[0])
        if op == 12:  # DBG_RESUME
            self.state = RUNNING_STATE
            self.stop_reason, self.bp_hit = STOP_NONE, NO_BREAKPOINT
            self.resumes += 1
            self.polls = 0
            return OK, b""
        if op == 13:  # DBG_STATUS
            if self.state == RUNNING_STATE:
                status = self._poll()
                if status is not None:
                    return status, b""
            if self.state == RUNNING_STATE:
                return OK, struct.pack("<BBBI", 0, STOP_NONE, NO_BREAKPOINT, 0)
            return OK, struct.pack("<BBBI", 1, self.stop_reason, self.bp_hit, self.regs[0])
        if op == 14:  # DBG_STEP
            self.steps += 1
            # a CPU that is off executes nothing (no interrupts in this model)
            size = 0 if self.regs[2] & SR_CPUOFF else self.step_size
            self._stop(self.regs[0] + size, STOP_STEP)
            return OK, struct.pack("<I", self.regs[0])
        if op == 15:  # DBG_READ_REGS
            return OK, struct.pack("<16I", *self.regs)
        if op == 16:  # DBG_WRITE_REG
            index, value = struct.unpack("<BI", a)
            # SP: bit 0 cleared, as the DLL does
            self.regs[index] = value & ~1 if index == 1 else value
            self.reg_writes.append((index, value))
            return OK, b""
        if op == 17:  # DBG_BP_SET
            index, addr = struct.unpack("<BI", a)
            self.breakpoints[index] = addr
            return OK, b""
        # op == 18, DBG_BP_CLEAR
        if a[0] == ALL_BREAKPOINTS:
            self.breakpoints = {}
        else:
            self.breakpoints.pop(a[0], None)
        return OK, b""
