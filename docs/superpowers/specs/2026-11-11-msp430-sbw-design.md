# MSP430F5529 Spy-Bi-Wire Support — Design (Phase A: Flash Programming)

Date: 2026-11-11
Status: Implemented and host-verified; on-target acceptance pending (see
`docs/PROJECT_STATUS.md`, Phase 6). This document describes the code as built.
Extends: `2026-06-11-debugprobe-zephyr-design.md` (the overall firmware design).

## Background and Goal

Add the ability to program a TI **MSP430F5529** through its two-wire
**Spy-Bi-Wire (SBW)** interface, using the same probe hardware and the same USB
device as the existing CMSIS-DAP/SWD function.

SBW multiplexes the JTAG signals TMS, TDI and TDO onto two wires: SBWTCK (the
target's TEST pin) and SBWTDIO (the target's RST/NMI pin). Each JTAG clock cycle
becomes three SBWTCK pulses ("slots"). The MSP430F5529 is a CPUXv2 device
(5xx family, JTAG ID `0x91`) with flash memory.

Why nothing existing can be reused on the host side:

- openocd, pyocd and probe-rs have no MSP430 support, so CMSIS-DAP as a pure
  transport is not an option.
- mspdebug has a `mehfet` driver for RP2040 probes, but its `jtaglib.c` only
  implements the classic 1xx/2xx/4xx sequences (JTAG ID `0x89`). CPUXv2 support
  has been an open request since 2019 (dlbeer/mspdebug issue #87).
- spycoprobe (`~/Developer/spycoprobe`) implements CPUXv2 entry, POR and
  word-wise memory access, but writes memory directly. That works for FRAM
  only; its erase function is a stub.

So the MSP430 device logic lives in this firmware, ported from TI's reference
implementation.

### Requirement Decisions (confirmed with the user)

| Decision | Choice |
|---|---|
| Scope | Phase A: flash programming (erase, program, verify, reset, memory read). Phase B: GDB debugging, designed separately later. |
| Architecture | Device logic in the firmware, exposed through a CMSIS-DAP vendor command. No USB descriptor change. |
| Probe used for verification | Raspberry Pi Pico (RP2040). The other two boards build and are wired the same way but are not verified in this phase. |
| Target | A custom MSP430F5529 board. Supply voltage, RST pull-up and RST capacitor are unknown. |
| Electrical defaults | 3.3 V direct connection, SBW clock 250 kHz by default and adjustable, target self-powered. |
| Host tool | Python command-line tool, same dependency (pyusb) as `tools/conformance`. |

### Non-goals (Phase A)

- Debugging: halt, resume, registers, single-step, breakpoints, GDB server.
- 4-wire JTAG.
- Other MSP430 families. The firmware code is generic for CPUXv2 flash devices,
  but only the F5529 is in the host device table and only it is verified.
  Classic (1xx/2xx/4xx) and FRAM devices are out of scope.
- Programming the JTAG lock key, password unlock, unlocking the BSL area or
  Info segment A.
- Level shifting and target power control.
- ELF input (use `objcopy -O ihex`), PSA-based fast verify.

## Reference Material

- TI **SLAU320** "MSP430 Programming With the JTAG Interface" and its source
  package (v1.22, BSD-3-Clause), project `Replicator430Xv2`:
  `JTAGfunc430Xv2.c` (device sequences), `LowLevelFunc430Xv2.{c,h}` (SBW slot
  macros, TCLK handling), `FlashWrite.c` / `FlashErase.c` (RAM funclets),
  `Config430Xv2.h` (F5529 memory constants). Download:
  `https://www.ti.com/lit/zip/slau320`.
- spycoprobe (MIT), local checkout, as a second reading of the same sequences
  on RP2040.
- MSP430F5529 datasheet (SLAS590) and the 5xx/6xx family guide (SLAU208) for
  the memory map and flash controller behaviour.

Ported sequences and the funclet arrays keep TI's BSD-3-Clause notice in the
files that contain them.

## Architecture

```
lib/msp430/                 pure C++20, zero Zephyr deps, host-tested (same rules as lib/dap)
  include/msp430/
    sbw_port.hpp            SbwPort: hardware abstraction (PIO driver on target, mock on host)
    tap.hpp                 Tap interface (JTAG primitives)
    sbw_tap.hpp             SbwTap: Tap over SbwPort (IR/DR shifts, TCLK, entry sequences)
    cpuxv2.hpp              Cpuxv2: connect/identify, POR, memory, erase, flash write
    session.hpp             Session: vendor-command parser and session state machine
    protocol.hpp            op codes, status codes, limits
  src/
    sbw_tap.cpp  cpuxv2.cpp  session.cpp
    funclets.cpp            TI FlashWrite_o / FlashErase_o arrays
drivers/sbw_pio/            sbw.pio + C driver: clocks JTAG cycles, samples TDO
dts/bindings/               debugprobe,sbw-pio binding
src/pin_owner.*             pin-ownership arbiter between SWD and SBW
src/msp430/                 glue: PioSbwPort, backend (owns the objects, registers the hooks)
tools/msp430/               Python CLI and its unit tests
tests/host/                 new C++ tests for lib/msp430 and the vendor hook
```

Dependency direction: `src/` → `lib/msp430` + `lib/dap` + drivers. `lib/msp430`
does not depend on `lib/dap`; the glue adapts one to the other.

The feature is controlled by `CONFIG_PROBE_MSP430_SBW` (default `y`, depends on
`PROBE_SWD_PIO`). With it disabled no vendor handler is registered, vendor ID
`0x90` echoes like every other vendor ID, and nothing touches the pins outside
SWD.

## Component Design

### 1. PIO SBW transport (`drivers/sbw_pio/`)

The driver does one thing: clock a sequence of JTAG cycles onto the two wires
with exact timing and return the sampled TDO bits. It knows nothing about TAP
states or MSP430 registers.

Waveform contract, per JTAG cycle (taken from the `TMSH/TMSL/TMSLDH/TDIH/TDIL/
TDO_RD` macros in `LowLevelFunc430Xv2.h`):

| Slot | SBWTDIO | SBWTCK |
|---|---|---|
| TMS | Probe drives the TMS value while SBWTCK is high. In the "LDH" variant the probe raises SBWTDIO during the low phase. | one low pulse |
| TDI | Probe drives the TDI value while SBWTCK is high. | one low pulse |
| TDO | Probe releases SBWTDIO before the falling edge, samples it at the end of the low phase, and drives it again after the rising edge. | one low pulse |

Timing rules:

- SBWTCK idles **high**. The slot rate is configurable from 100 kHz to 1 MHz,
  default 250 kHz. A slot is 8 state-machine ticks, 4 low and 4 high; the
  hand-over after the TDO slot adds 4 high ticks per cycle. At the lowest rate
  the low phase is 5 µs, inside the 7 µs limit after which the target's SBW
  logic resets.
- The state machine may only stall (empty TX FIFO, full RX FIFO) while SBWTCK
  is high. A stall with SBWTCK low would violate the 7 µs limit. This removes
  every real-time requirement from the CPU: no interrupt masking is needed.
- One driver call clocks 1 to 32 cycles, so a full IR shift (14 cycles),
  16-bit DR shift (21) or 20-bit DR shift (25) is one call. Internally a call
  is split into PIO commands of at most 9 cycles (one TX word in, one RX word
  out); the four words of a 32-cycle call exactly fit the 4-deep FIFOs.
- Each cycle carries three bits: TMS, TDI and the level of the data line
  during the low phase of the TMS slot, which differs from TMS only for
  TMSLDH.

The program runs on **PIO1**. PIO0's 32-instruction memory already holds the SWD
program (11) and the autobaud program (8); the remaining 13 are too few to rely
on. `sbw.pio` is assembled by `scripts/gen_pio.sh` into a generated header,
exactly like `probe.pio`; opcodes are never hand-edited.

The driver also provides what the entry sequence needs: static levels on both
pins, and a single SBWTCK low pulse of about 0.7–0.9 µs. Both are produced by
executing single instructions on the stopped state machine; the pulse runs
from RAM with interrupts locked, so its width does not depend on the flash
cache.

The driver protects the target against caller errors and its own faults:

- The slot rate is clamped to 100 kHz–1 MHz in the driver itself.
- Before it starts a stopped state machine it reads back what the PIO block is
  actually driving. Both pins must be outputs and SBWTCK must be high;
  otherwise nothing is clocked and the call fails. (Starting with the clock
  low would add a rising edge and leave the target one slot out of phase.)
- Waiting for the state machine's answer has a 10 ms limit. On a timeout the
  pins are released.
- "Released" means: directions dropped, then, after the lines have settled,
  the pins are routed to this PIO with the idle pulls.

`PioSbwPort` (the glue) turns a driver error into all-zero TDO and stops
calling the driver until the next `acquire`. All-zero TDO fails every check in
the layers above, so the command in progress ends with an error.

`lib/msp430` sees the driver through this interface (a mock implements it in
the host tests):

```cpp
class SbwPort {
public:
	virtual bool acquire() = 0;             // take both pins; false if SWD owns them
	virtual void release() = 0;             // both pins Hi-Z
	virtual void set_clock(uint32_t slot_hz) = 0;
	virtual void set_tck(bool level) = 0;   // static levels, entry sequence only
	virtual void set_tdio(bool level) = 0;
	virtual void tck_pulse() = 0;           // one short low pulse on SBWTCK
	/* Clock `count` (1..32) JTAG cycles. Bit i of tms/tdi belongs to cycle i.
	 * Returns the sampled TDO bits, bit i from cycle i. */
	virtual uint32_t cycles(uint32_t tms, uint32_t tdi, unsigned count) = 0;
	/* One Run-Test/Idle cycle (TMS = 0) that sets TCLK to `level`.
	 * `was_high` selects the TMSLDH variant. */
	virtual void tclk_cycle(bool was_high, bool level) = 0;
	virtual void delay_us(uint32_t us) = 0;
	virtual uint32_t millis() = 0;
};
```

Devicetree: a `debugprobe,sbw-pio` node with `piodev = <&pio1>`, the TCK and
TDIO pin numbers, an optional separate input pin (Debug Probe: SWDI/GP13), and
a pinctrl state that routes the pins to PIO1 with a pull-up on TDIO and a
pull-down on TCK.

### 2. Pin sharing with SWD

SBW reuses the SWD pins:

| | rpi_pico / rpi_pico2 | rpi_debug_probe |
|---|---|---|
| SBWTCK → target TEST | GP2 (SWCLK) | GP12 (SWCLK) |
| SBWTDIO ↔ target RST | GP3 (SWDIO) | GP14 out, GP13 in |

- **Mux switching.** An SBW session applies the SBW pinctrl state (PIO1); an
  SWD connect applies the SWD state (PIO0). `swdp_pio.c` re-applies its
  pinctrl state on every `port_on` (before this feature it did so only on the
  first).
- **Mutual exclusion.** A small pin-ownership arbiter (`src/pin_owner.*`) tracks
  the owner (none, SWD, SBW). SBW `CONNECT` while SWD is connected returns
  `BUSY`. `DAP_Connect` while an SBW session is open force-closes the SBW
  session first, so a crashed SBW tool cannot lock out openocd. All requests
  are serialized on the USB thread, so no locking is needed.
- **Idle state.** Outside a session both pins are high-impedance, routed to
  PIO1 with the idle pulls. That holds after boot, after an SBW session, and
  after an SWD session: when `DAP_Disconnect` ends an SWD session, the pin
  arbiter runs a hook that releases the pins the SWD driver would otherwise
  leave driven. At boot the
  SBW driver applies its pinctrl state, which puts a **pull-up on the
  SWDIO/SBWTDIO pin**. Before this feature that pin had the RP2040 reset-default pull-down
  until the first SWD connect. That pull-down (50–80 kΩ)
  and the target's 47 kΩ RST pull-up form a divider that holds RST at
  1.7–2.1 V, which can keep an attached MSP430 in reset. A pull-up on SWDIO is
  harmless for SWD targets.
- **Robustness fix found on the way.** `sw_port_off` disables state machine
  `d->sm` even when the SWD driver was never connected (`d->sm` is then 0,
  which can be the autobaud state machine). That call is now skipped when
  nothing was loaded; nRESET is still released, as CMSIS-DAP requires. The
  host tool sends `DAP_Disconnect` before every SBW session, which makes this
  path reachable.

### 3. TAP layer (`msp430::SbwTap`)

Implements the `Tap` interface over `SbwPort`:

```cpp
class Tap {
public:
	virtual void set_speed(uint32_t slot_hz) = 0;
	virtual bool open() = 0;                 // release, settle, take the pins
	virtual void enter(Entry entry) = 0;     // SBW entry sequence (RST high or low) + TAP reset
	virtual void close() = 0;                // release pins (Hi-Z)
	virtual uint8_t  ir_shift(uint8_t ir) = 0;      // returns the captured IR (JTAG ID)
	virtual uint16_t dr_shift16(uint16_t value) = 0;
	virtual uint32_t dr_shift20(uint32_t value) = 0;
	virtual void set_tclk() = 0;
	virtual void clr_tclk() = 0;
	virtual void delay_ms(uint32_t ms) = 0;
	virtual uint32_t millis() = 0;
};
```

`SbwTap` builds the per-cycle TMS/TDI bit vectors exactly as TI's `IR_Shift`,
`DR_Shift16`, `DR_Shift20`, `AllShifts`, `ResetTAP`, `SetTCLK_sbw` and
`ClrTCLK_sbw` do, including:

- the TDI level of the first and last cycle of every shift following the saved
  TCLK level, so TCLK does not glitch when leaving or re-entering
  Run-Test/Idle;
- the TMSLDH variant in `set_tclk`/`clr_tclk` when TCLK is currently high;
- MSB-first data, the pre-reversed IR constants, and the 20-bit descramble
  `((v << 16) + (v >> 4)) & 0xFFFFF`.

Entry sequences (`EntrySequences_RstHigh_SBW`, `EntrySequences_RstLow_SBW`) use
the port's static pin levels, millisecond delays, and a short low pulse for
the one timing-critical step. Taking the pins (`open`) is separate from the
entry sequence (`enter`) because the magic-pattern connect performs two
entries with a mailbox write in between.

### 4. Device layer (`msp430::Cpuxv2`)

A port of `JTAGfunc430Xv2.c`, written against `Tap`:

| Operation | TI function(s) |
|---|---|
| `connect(mode)` | `GetCoreID` (one attempt), `IsLockKeyProgrammed`, `GetCoreipIdXv2`, `SyncJtag_AssertPor` → `ExecutePOR_430Xv2` (includes watchdog hold), then read the device ID at `DeviceIdPointer + 4` |
| `disconnect()` | `ReleaseDevice_430Xv2(V_RESET)`, then release the pins |
| `read_mem` | `ReadMemQuick_430Xv2`, plus a final JTAG-control check |
| `write_mem` | `WriteMemQuick_430Xv2` (RAM and peripherals only), plus a final JTAG-control check |
| `erase(mode, addr)` | `EraseFLASH_430Xv2` with the `FlashErase_o` funclet |
| `flash_begin/data/end` | `WriteFLASH_430Xv2` with the `FlashWrite_o` funclet, split into three steps |

Details that matter:

- **Entry modes.** Mode 0 is the normal RST-high entry. Mode 1 is TI's
  "magic pattern" fallback: RST-low entry, write `0xA55A` to the JTAG mailbox so
  the boot code does not start the user program, RST-high entry again, and on
  success write `0x4020` to `IR_TEST_3V_REG`. It recovers targets whose
  firmware blocks a normal entry (for example LPMx.5). The firmware performs a
  single attempt per `CONNECT`; the host tool retries mode 0 up to four times
  and then tries mode 1 once, as TI's `GetCoreID` does.
- **Accepted JTAG ID.** Only `0x91`. Anything else is reported as "no target"
  together with the value read.
- **Funclets.** Both arrays are loaded at the device's RAM start (`0x2400` on
  the F5529, passed in by the host). The header words are patched with the
  target address, length or erase mode, and the Info-A lock key, which is
  always the *locked* value `0xA548`.
- **Erase modes.** Segment (`0xA502`), bank (`0xA504`), all main memory
  (`0xA506`). Mass erase of main memory does not touch the information
  segments or the BSL area.
- **Flash write handshake.** After `flash_begin` starts the funclet, wait for
  mailbox value `0xABADBABE`. Each data word goes through `i_WriteJmbIn16`.
  `flash_end` waits for `0xCAFEBABE`, re-synchronizes with a POR, and
  overwrites the funclet RAM with `0x3FFF` (`JMP $`), as TI does.
- **Timeouts are time-based** (`Tap::millis()`), not poll-count-based, so they
  do not depend on the SBW clock: funclet ready 50 ms, erase done 200 ms,
  write done 100 ms, mailbox input ready 20 ms. The clock is read before each
  poll, so one poll always happens after the deadline.
- **Write state is explicit.** `flash_data` and `flash_end` are refused
  (`BadState`) unless `flash_begin` succeeded; every other operation is refused
  while a write is active; `connect` and `disconnect` clear the state.
- **Memory transfers end with a check.** `read_mem` and `write_mem` finish by
  confirming the CPU is still under JTAG control. A link that dies mid-transfer
  is reported as `TargetError` instead of returning made-up data.
- **The link check uses the JTAG ID.** Every IR scan captures the JTAG ID. The
  full-emulation-state check and every mailbox access require it to be `0x91`,
  so a data line that reads constantly low (probe fault) or constantly high
  (wire off, pull-up wins) is detected. Checking only the control-signal bits,
  as TI does, would accept an all-ones line, and all-ones data looks like
  erased flash.
- **A failed cleanup outranks the operation's result.** If the re-sync after a
  funclet fails, the operation returns `TargetError` whatever it would have
  returned, and the RAM overwrite is skipped (the CPU may be running).
- **Deviations from TI** are few and deliberate; they are listed in the header
  comment of `lib/msp430/src/cpuxv2.cpp`.
- **Every session resets the target.** `connect` performs a POR and
  `disconnect` a JTAG reset, so the target restarts from its reset vector
  after any command, including `info` and `read`.

The firmware has no device table and does not enforce a memory map. The BSL
area and Info A are protected by the device's own lock bits, which this
firmware never clears.

### 5. Vendor command (`msp430::Session` + DAP engine hook)

`lib/dap` gets an optional hook:

```cpp
class VendorHandler {
public:
	/* request starts at the command ID byte and runs to the end of the packet.
	 * Returns the response length, or 0 to decline (engine echoes the ID). */
	virtual size_t handle(std::span<const uint8_t> request,
			      std::span<uint8_t> response) = 0;
};
```

IDs `0x80`–`0x9F` without a handler, or declined by it, keep the existing
one-byte echo, so the conformance check on `0x85` is unaffected. A handled
vendor command consumes the rest of the packet and must therefore be the only
or the last command in it.

SBW uses **`0x90`** (`ID_DAP_Vendor16`). All multi-byte fields are
little-endian.

```
request:  [0x90][op][args...]
response: [0x90][status][data...]
```

| op | Name | Request args | Response data |
|---|---|---|---|
| 0x00 | INFO | – | `u8 version` (1), `u8 state` (0 Idle, 1 Connected, 2 Flashing), `u16 max_data_bytes`, `u32 features` (bit 0: CPUXv2 flash programming) |
| 0x01 | CONNECT | `u16 speed_khz` (100–1000), `u8 entry_mode` (0 normal, 1 magic pattern) | `u8 jtag_id`, `u16 core_id`, `u32 device_id_ptr`, `u16 device_id` |
| 0x02 | DISCONNECT | – | – |
| 0x03 | READ_MEM | `u32 addr`, `u16 nwords` | `nwords × u16` |
| 0x04 | WRITE_MEM | `u32 addr`, `n × u16` | – |
| 0x05 | ERASE | `u8 mode` (0 segment, 1 bank, 2 main), `u32 addr`, `u32 ram_addr` | – |
| 0x06 | FLASH_BEGIN | `u32 addr`, `u32 nwords`, `u32 ram_addr` | – |
| 0x07 | FLASH_DATA | `n × u16` | – |
| 0x08 | FLASH_END | – | – |

Status codes:

| Code | Name | Meaning |
|---|---|---|
| 0x00 | OK | |
| 0x01 | BAD_ARG | truncated, odd address, out-of-range address or length, more flash data than announced |
| 0x02 | BAD_STATE | op not valid in the current session state |
| 0x03 | BUSY | the SWD port owns the pins |
| 0x04 | NO_TARGET | JTAG ID is not `0x91`; the response still carries the ID read |
| 0x05 | LOCKED | the JTAG lock key is programmed |
| 0x06 | TARGET_ERROR | synchronization or POR failed, or the CPU is not in full-emulation state |
| 0x07 | TIMEOUT | a mailbox handshake timed out |
| 0x08 | UNKNOWN_OP | |

Argument rules, all answered with `BAD_ARG` when violated:

- `speed_khz` outside 100–1000, or an unknown `entry_mode` or erase `mode`.
- Addresses must be even, and `addr + 2 × nwords` must not exceed `0x100000`.
- Data payloads must be a whole number of words, not empty, and at most
  `max_data_bytes`; `READ_MEM` must request between 1 and
  `max_data_bytes / 2` words.
- `FLASH_BEGIN` needs `nwords > 0`.
- `ram_addr` must be even and the whole funclet must fit below `0x100000`
  (178 bytes for `ERASE`, 240 for `FLASH_BEGIN`).
- Requests without a data payload must have exactly their size; missing or
  extra bytes are both rejected.

The state check comes first: an operation that is not allowed in the current
state answers `BAD_STATE` even if its arguments are malformed. `INFO` is
allowed in every state.

A `NO_TARGET` response carries only `jtag_id`.

Transport rule: the probe's bulk endpoints neither send nor expect zero-length
packets, so a request or response whose length is an exact multiple of 64
bytes would not terminate the USB transfer. The fixed-size commands never have
such a length. For `READ_MEM`, `WRITE_MEM` and `FLASH_DATA` the client must
pick word counts that avoid it (the host tool sends one word fewer and
continues in the next command).

Session states: `Idle` → (`CONNECT`) → `Connected` → (`FLASH_BEGIN`) →
`Flashing` → (`FLASH_END`) → `Connected` → (`DISCONNECT`) → `Idle`.

- A failed `CONNECT` releases the pins and stays in `Idle`.
- `CONNECT` in any state other than `Idle` first closes the open session.
- Any error during `FLASH_DATA` ends the write: the firmware cleans up by
  itself (POR and RAM wipe) and returns to `Connected`.
- `FLASH_END` before all announced words were sent skips the completion wait,
  cleans up, and returns `BAD_STATE`.
- **`TARGET_ERROR` closes the session.** The target is then in an unknown
  state, so the firmware releases the pins and goes to `Idle`; the host must
  connect again.
- `DISCONNECT` in `Idle` returns `BAD_STATE`. Clients that disconnect as
  cleanup ignore it.
- When the DAP USB interface is disabled (bus reset, deconfiguration) an open
  session is closed with the normal disconnect sequence. A host tool that
  crashes does not disable the interface: its session stays open until the
  next `CONNECT`, the next `DAP_Connect`, or a bus reset. Every run of the host
  tool starts with `CONNECT`, which closes whatever was left open.

While a session is open the DAP-connected LED is lit (boards that have one).

### 6. Host tool (`tools/msp430/`)

```
sbw.py          CLI (argparse)
probe.py        USB access and the vendor-command client
image.py        Intel HEX and TI-TXT readers, Intel HEX / binary writers
devices.py      device table
flasher.py      erase/write/verify planning and orchestration
tests/          unittest-based tests with a fake probe
```

The probe is found the same way `tools/conformance` does it (VID `2E8A`, PID
`000C`, the CMSIS-DAP interface), optionally narrowed by `--serial`.

| Command | Action |
|---|---|
| `info` | connect, print JTAG ID, core ID, device ID and the matched device |
| `flash IMAGE [--erase main\|segments\|none] [--no-verify]` | erase, program, verify, reset |
| `erase [--segment ADDR]` | erase all main memory, or one segment |
| `read ADDR LEN -o FILE [--force]` | dump memory to `.hex` or `.bin`; never replaces an existing file without `--force`; writes the file only after the whole read succeeded; prints the SHA-256 of the data |
| `selftest` | write known data to the target's RAM and read it back; re-read the device ID inside a block. Flash is not touched |
| `trial --backup FILE` | check the target against the backup, program and verify a test pattern in one segment that is blank in the backup, erase it again, check the target against the backup again |
| `verify IMAGE` | compare flash contents with an image |
| `reset` | connect and disconnect |

Global options: `--speed KHZ` (default 250), `--serial`.

Everything that can fail without the probe happens before the USB device is
opened: argument checks, reading the image, rejecting an empty image or one
with data above the 20-bit address space. Exit status: 0 success, 1 failure,
2 usage error, 130 interrupted.

Device table, one entry in this phase:

| Field | MSP430F5529 |
|---|---|
| Device ID bytes at `0x1A04` | `55h 29h` (to be confirmed by acceptance step 2; the tool prints the raw value when it does not match) |
| Main flash | `0x4400`–`0x243FF`, 512-byte segments, four 32 KB banks |
| Info B, C, D | `0x1900`, `0x1880`, `0x1800`, 128 bytes each |
| Info A (never written) | `0x1980`–`0x19FF` |
| BSL (never written) | `0x1000`–`0x17FF` |
| Funclet RAM address | `0x2400` |

`flash` behaviour:

1. Send `DAP_Disconnect`, `INFO` (check the protocol version), then `CONNECT`
   with retries as described above.
2. Look the device ID up in the table. An unknown ID is an error.
3. Validate the image: every byte must lie in main flash or Info B–D.
   Anything else is an error before the target is modified. Sections are
   padded to word boundaries with `0xFF`.
4. Erase. `main` (default): one mass erase of main memory. `segments`: erase
   only the 512-byte segments the image touches. In both modes, Info B–D
   segments are erased individually and only if the image has data in them.
5. Write each contiguous section with `FLASH_BEGIN` / `FLASH_DATA`… /
   `FLASH_END`.
6. Verify by reading back with `READ_MEM`, comparing exactly the bytes that
   are in the image (not the `0xFF` padding).
7. `DISCONNECT` (also on failure, as cleanup). Exit code 0 only if every step
   succeeded.

## Timing and Throughput

Blocking time on the USB thread, which the DAP class shares with the CDC-ACM
bridge:

| Command | At 250 kHz |
|---|---|
| READ_MEM, WRITE_MEM, FLASH_DATA | chunked by the host tool to about 100 ms per command |
| ERASE, FLASH_BEGIN, FLASH_END | 0.3–0.5 s (up to about 1.2 s at 100 kHz), dominated by loading the funclet and wiping its RAM |
| CONNECT | about 60 ms (mode 0), about 250 ms (mode 1) |

The UART bridge can stall for the duration of one SBW command. The target is
halted or in reset during a session, so it is not sending UART data then.

Expected throughput at 250 kHz: about 2 kB/s for flash writes (77 JTAG cycles
per word through the mailbox, 14 µs per cycle) and about 6 kB/s for reads (23
cycles per word). A 16 KB image takes about 11 s to program and verify; a full
128 KB image about 70 s plus 21 s. `--speed 500` matches TI's reference
implementation and roughly halves these times.

## Error Handling Principles

- Protocol layer: malformed or out-of-bounds requests produce an error status,
  never a crash. Every buffer access is bounds-checked, as in `lib/dap`.
- No target, wrong wiring: the tool reports the JTAG ID that was read. `0xFF`
  means the data line stayed high, which almost always means nothing is
  connected.
- Locked device: refused. No unlock is attempted.
- Mailbox timeout: the flash operation is aborted and the tool reports the
  address it was working on.
- Verify mismatch: the tool prints the first differing address and exits
  non-zero.
- USB bus reset or deconfiguration mid-session: the firmware closes the
  session. A crashed host tool leaves it open; the next `CONNECT` closes it
  and starts from a clean entry sequence.
- Driver fault (pins not in the required state, state machine not answering):
  logged by the driver, reported to the host as `TARGET_ERROR`, session closed.
- Host tool: nothing is erased before the image is fully validated; the
  session is closed on every exit path, including Ctrl-C; stale replies left
  in the probe by an interrupted run are drained when the device is opened.
- No exceptions, no heap, no RTTI in `lib/msp430`.

## Testing and Acceptance

As built: 192 C++ host tests and 139 Python tests. Beyond the tests listed
below, the reviews compared `SbwTap` and `Cpuxv2` mechanically with TI's
reference source, simulated the PIO program tick by tick from its generated
opcodes, fuzzed `Session` under ASan/UBSan, and ran mutation passes over
`cpuxv2.cpp`, `session.cpp` and the host tool to find behaviour no test pinned.

1. **Host unit tests (C++, `scripts/run_host_tests.sh`, also under ASan).**
   - `SbwTap`: recorded cycle sequences for IR/DR shifts, TAP reset and TCLK
     changes compared with vectors derived by hand from the TI macros, for
     both TCLK levels; TDO bit assembly and the 20-bit descramble.
   - `Cpuxv2` against a scripted fake `Tap`: successful connect, wrong JTAG
     ID, locked device, sync timeout, memory read/write, funclet header
     patching, mailbox handshakes and each timeout.
   - `Session`: framing, bounds, every state transition, error cleanup.
   - `DapEngine`: vendor hook dispatch, decline path, `0x85` echo unchanged,
     behaviour inside `DAP_ExecuteCommands`.
2. **Python unit tests (`python -m unittest`).** Intel HEX and TI-TXT parsing
   (including extended addresses and odd lengths), address validation, erase
   and write planning, the flash flow against a fake probe including failures.
3. **Build.** All three boards build with the feature enabled; rpi_pico also
   builds with it disabled, and that image contains no SBW code.
4. **On-target acceptance (Pico + the F5529 board).** Nothing before the
   last two steps changes the firmware on the board.
   1. No target attached: `info` fails cleanly with "no MSP430 found".
   2. Target attached: `info` reports JTAG ID `0x91` and device ID bytes
      `55h 29h`. Any other ID is a hard stop: the read path is suspect, and
      the device table must not be edited to make it pass.
   3. `selftest`: RAM round trip and device-ID re-read.
   4. Two backups of main flash at different speeds into different files,
      identical; a copy of the information memory.
   5. `trial --backup`: erase, program, verify and erase one blank segment,
      with the rest of main flash compared against the backup before and after.
   6. `flash` the user's firmware (or the backup): verify passes and the board
      runs.
   7. 50 connect/disconnect cycles; repeat step 6 at 250, 500 and 1000 kHz.
   8. Regression: `tools/conformance` compare still passes; the openocd
      acceptance flow against an RP2040 target still passes before and after
      SBW sessions (pin mux hand-over in both directions).

   Steps 2–7 need the F5529 board on the probe. `docs/PROJECT_STATUS.md`
   records which steps were actually run.

Documentation delivered with the implementation: `docs/MSP430_SBW.md` (wiring,
electrical checks, usage, limitations), plus updates to `README.md` and
`docs/PROJECT_STATUS.md`.

Wiring and electrical notes that the document must contain:

- GP2 → TEST/SBWTCK, GP3 → RST/SBWTDIO, GND → GND. Keep the wires short.
- Measure the target's VCC before connecting. Below 3.0 V, do not connect
  directly: the Pico drives 3.3 V.
- The RST pin should have about 47 kΩ to VCC and at most 2.2 nF to ground.
  Larger capacitors require a lower `--speed` and may not work at all.
- Do not leave an unpowered probe attached to a powered target: the RP2040's
  protection diodes load RST and TEST.
- Do not run SWD tools (openocd) against the probe while an MSP430 is attached.

## Risks and Mitigations

- **The PIO waveform cannot be verified on the host.** The cycle-level logic
  is covered by vector tests; the PIO program is kept minimal and comes from
  pioasm; the first on-target steps are read-only.
- **Unknown electrical properties of the target board.** Conservative default
  speed, adjustable speed, explicit checks in the documentation.
- **Blocking the USB thread for up to about half a second** (more at
  100 kHz). Accepted for this phase. If it becomes a problem, funclet loading
  can be chunked or moved to a worker thread.
- **Stack depth on the USB thread.** The SBW call chain is deeper than SWD's:
  about 1.2 KB in the worst case. `USBD_THREAD_STACK_SIZE` is raised from 1024
  to 2048 when the feature is enabled.
- **Regression in the SWD path** from the pinctrl change and the vendor hook.
  Covered by the existing host tests, the conformance comparison and the
  openocd acceptance flow.
- **Device ID byte order** in the table is taken from the datasheet and
  confirmed in acceptance step 2.

## Phase B Outlook (not part of this design)

Debugging adds halt/resume, register access, single-step and EEM breakpoints
to `Cpuxv2`, new ops under the same vendor command (`INFO.features`
advertises them), and a GDB remote-protocol server in the host tool. The
layering above is chosen so that Phase B extends `Cpuxv2` and `Session`
without touching the transport or TAP layers.
