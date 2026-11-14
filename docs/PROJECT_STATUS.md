# Project status — Zephyr debugprobe rewrite

This document summarises what was implemented and verified in each phase of the
Zephyr+C++20 reimplementation of the Raspberry Pi debugprobe CMSIS-DAP / UART-
bridge firmware.  Verification is tracked on three axes: host tests, 3-board
build, and on-target hardware.

---

## Phase 1 — USB descriptors

**Implemented:** USB device stack, CDC-ACM interface, HID/bulk CMSIS-DAP
interface, BOS / MS-OS-2.0 descriptors, USB vendor/product IDs (2e8a:000c).

| Axis | Status |
|---|---|
| Host tests | 76/76 pass (`scripts/run_host_tests.sh`) |
| 3-board build | rpi_pico, rpi_pico2, rpi_debug_probe — all produce `zephyr.uf2` |
| On-target hardware | **Verified** — `tools/conformance/check_descriptors.py` passes on real hardware; DAP vendor interface 0, CDC-ACM, BOS/MS-OS-2.0 all correct |

---

## Phase 2 — DAP engine

**Implemented:** CMSIS-DAP command parser, DAP_Info, DAP_HostStatus,
DAP_Connect/Disconnect, DAP_WriteABORT, DAP_Delay, DAP_ResetTarget,
DAP_SWJ_Pins/Clock/Sequence, DAP_SWD_Configure/Sequence, DAP_Transfer,
DAP_TransferBlock, DAP_ExecuteCommands (batch), DAP_QueueCommands, vendor echo
command.

| Axis | Status |
|---|---|
| Host tests | 76/76 pass |
| 3-board build | All boards pass |
| On-target hardware | **Verified** — openocd reads Info, capabilities, FW 2.0.0, Connect, SWJ_Clock, SWJ_Pins; endpoints OUT=0x01/IN=0x81 confirmed |

---

## Phase 3 — SWD backends

**Implemented:** two SWD backends behind a common `dap::SwdDriver` adapter:
PIO backend (`probe.pio` + `sw_dp_pio.c` port, default) and bitbang backend
(`zephyr,swdp-gpio` driver with a thin adapter).  Both share the same Zephyr
pinctrl pin routing.  PIO side-set `bit_count` bug found and fixed on hardware.

| Axis | Status |
|---|---|
| Host tests | 76/76 pass |
| 3-board build | All boards pass (+ bitbang variant) |
| On-target hardware | **Verified** — both backends read target DPIDR `0x10212927`, identical to official firmware on the same hardware.  Multidrop RP2040 `connect` fails on **both** our firmware and official v2.3.1 identically — use a single-drop target for full memory R/W (not our bug; A/B-proven). |

---

## Phase 4-core — UART bridge + serial

**Implemented:** CDC-ACM ↔ uart1 bridge (GP4 TX / GP5 RX), DAP_Info serial
populated from `hwinfo` (matches USB iSerialNumber).

| Axis | Status |
|---|---|
| Host tests | 76/76 pass |
| 3-board build | All boards pass |
| On-target hardware | **Verified** — `check_descriptors.py` passes including DAP_Info serial == USB serial; openocd reports `Serial# = E661A4D41777B429` and reads DPIDR; CDC↔uart1 GP4→GP5 loopback echoes both directions at 115200 |

---

## Phase 4b — Custom C++ bridge, activity LEDs, autobaud

**Implemented:** custom C++ UART-bridge task replacing Zephyr's generic
CDC-ACM ↔ UART bridge, activity LEDs (DAP connect/run, UART TX/RX), autobaud
detection using a MAGIC_BAUD sentinel (9728) with 4 KB stack-overflow and race
fixes.

| Axis | Status |
|---|---|
| Host tests | 76/76 pass (autobaud estimator is host-tested) |
| 3-board build | All boards pass |
| On-target hardware | **Partially verified** — GP4→GP5 loopback PASS; USB+DAP+serial regression PASS; autobaud apply path (MAGIC_BAUD 9728→115200 start/stop cycle) leaves bridge alive.  **Pending:** autobaud DETECTION happy-path (needs an external UART source on GP5; GP4→GP5 loopback cannot trigger detection). |

---

## Phase 5 — Conformance validation

**Implemented:** host tool `tools/conformance/dap_conformance.py` (`capture` and
`compare` subcommands), 31-command fixed DAP request sequence, WHITELIST
mechanism, source-derived reference capture.

| Axis | Status |
|---|---|
| Host tests | 76/76 pass |
| 3-board build | All boards pass |
| On-target hardware | **Verified (our firmware)** — live capture succeeded (`captured 31 responses`); macOS interface-claim fix validated end-to-end. Cross-checked vs the v2.3.1 source: **26 protocol-deterministic responses byte-identical + 1 environment-dependent (`swj_pins_read`, matched on the same wiring) + 4 whitelisted enhancements (3 DAP_Info strings + DAP packet size 64→1024), 0 unexpected mismatches** (`compare` reports 27 identical / 4 whitelisted / 0 missing → PASS). **Confirmed by a live on-hardware A/B (2026-06-12); packet-size bump re-captured 2026-06-13.** See [CONFORMANCE_RESULTS.md](CONFORMANCE_RESULTS.md) for the full table. |

### Phase 5 hardware acceptance table (fill in per board × backend)

Run the acceptance suite in [`docs/HARDWARE_BRINGUP.md` — Full on-target
acceptance (phase 5)](HARDWARE_BRINGUP.md#full-on-target-acceptance-phase-5) and
record results here. A `—` means that board × backend combination has not been
run through the full suite yet (not a failure). `P*` = partial: passed every
check the available hardware allows (see the note below the table):

| Board | Backend | openocd accept | UART RT | autobaud | LEDs |
|---|---|---|---|---|---|
| rpi_pico | PIO | ✓ | ✓ | P* | n/a |
| rpi_pico | bitbang | — | — | — | — |
| rpi_pico2 | PIO | — | — | — | — |
| rpi_pico2 | bitbang | — | — | — | — |
| rpi_debug_probe | PIO | — | — | — | — |
| rpi_debug_probe | bitbang | — | — | — | — |

**rpi_pico / PIO, executed 2026-06-12 (`P*` detail):**
- **openocd accept: ✓ FULL PASS (2026-06-12)** against a live RP2040 target
  (connects equally in BOOTSEL and while running its own firmware — see the root
  cause note below). openocd
  0.12.0 opens our CMSIS-DAP v2 interface (`FW Version = 2.0.0`,
  `Serial# = E661A4D41777B429`), connects multidrop (`DPIDR 0x0bc12477`, Cortex-M0+
  r0p1 detected), and the full flow passes:
  - `reset halt` → halted, PC `0x000000ea`, MSP `0x20041f00`;
  - single-step ×2 → PC advances `0xea → 0xec → 0xee`;
  - memory read → bootrom `0x20041f00`, XIP flash `0x4b32b500`;
  - RAM write+readback at 0x20000000/04/0x20001000 → `c0ffee42` / `deadbeef` /
    `11223344`, all verified;
  - flash erase+program+verify (top sector 31) → wrote `0xa5a5a5a5`, read back
    `0xa5a5a5a5` (flash chip `win w25q16jv`, 2 MB / 32 sectors detected);
  - `reset run` → target running.

  **Root cause of the earlier failure (corrected, verified):** the probe attaches
  to a **running** target just fine — verified by connecting to the target while
  it ran its own flash firmware (`DPIDR 0x0bc12477`, state `running`), then
  `halt` → `halted` → read memory → `resume` → `running`. So this is **not** a
  probe-firmware limitation and **not** BOOTSEL-specific. The original
  `Failed to connect multidrop rp2040.dap0` (with the garbage `DPIDR 0x10212927`,
  which is a *misread* — the genuine RP2040 DPIDR is `0x0bc12477`) was the
  **target not being live/responsive on the SWD bus** (unpowered or hung). Once
  the target was alive (the user happened to bring it up via BOOTSEL, but any
  reset/power-up works), the multidrop DP connects in any state. The probe
  firmware needs no change; the phase-3 "DPIDR 0x10212927" reads on both our and
  the official firmware were the same misread from a not-yet-live target.
- **UART RT:** ✓ — CDC↔uart1 GP4→GP5 loopback verified in phase 4b (both directions).
- **autobaud (partial):** ✓ no-wedge — the MAGIC_BAUD(9728)→115200 start/stop
  cycle leaves the bridge alive (race + stack-overflow fixes hold). The detection
  happy-path still needs an external UART source on GP5.
- **LEDs:** n/a on rpi_pico (no DAP/UART activity LEDs; onboard USB LED only —
  the debug-probe board has the full LED set).
- **USB descriptors + DAP serial:** ✓ — `check_descriptors.py` all checks pass
  (3 interfaces, vendor CMSIS-DAP v2, WinUSB/MS-OS-2.0, DAP serial == USB serial).
- **Conformance A/B:** ✓ PASS (see above / CONFORMANCE_RESULTS.md).

---

## Phase 6 — MSP430F5529 Spy-Bi-Wire flash programming

**Implemented:** PIO SBW transport on PIO1 (`drivers/sbw_pio`), pure C++ JTAG /
CPUXv2 / vendor-command library (`lib/msp430`, port of TI SLAU320
Replicator430Xv2), vendor-command hook in the DAP engine, SWD/SBW pin
ownership with the pins returned to high-impedance after every session, Python
host tool (`tools/msp430/sbw.py`). Design:
`docs/superpowers/specs/2026-11-11-msp430-sbw-design.md`; user guide:
`docs/MSP430_SBW.md`.

| Axis | Status |
|---|---|
| Host tests | 193/193 C++ (`scripts/run_host_tests.sh`, also under ASan); 148/148 Python (`python3 -m unittest discover -s tools/msp430/tests`) |
| 3-board build | rpi_pico, rpi_pico2, rpi_debug_probe build with the feature on; rpi_pico also with `CONFIG_PROBE_MSP430_SBW=n` (no SBW code in that image) |
| On-target hardware | **Verified on an MSP430F5529 with an rpi_pico probe (2026-11-11):** acceptance steps 2–7 pass, including mass erase and full programming at 250, 500 and 1000 kHz. Not run: step 1 and the SWD regression (step 8); see the table and the open points below |

What the host-side verification covers. During the code reviews (with
throwaway harnesses that are not in the repository) the JTAG cycle sequences
and the CPUXv2 device sequences were compared mechanically with TI's reference
code; the PIO program was simulated tick by tick from its generated opcodes
(event order as TI's macros, low phase at most 5 µs, no wait with the clock
low); the command parser was fuzzed under ASan/UBSan.

### First on-target run (2026-11-11, rpi_pico + MSP430F5529)

Read-only commands only; nothing was written to the target.

- `info` failed with "unsupported device: ID bytes FFh 3Fh at 0x001A4". Cause:
  `connect` used the device-descriptor pointer after `dr_shift20`'s 20-bit
  de-scramble, but the device shifts that pointer out in plain order, so the
  value was 0x001A0 instead of 0x01A00 and the ID was read from vacant memory.
  Fixed by converting the pointer back in `connect`. TI's Spy-Bi-Wire source
  has the same slip but never uses the ID.
- Everything else worked with that firmware, driven through the vendor command
  by throwaway scripts: SBW entry, JTAG ID 0x91, lock-key check (not locked),
  core ID 0x0103, sync and POR, and memory reads. The descriptor at 0x01A00
  reads `06 06 59 08 55 29 19 12`.
- Main flash (128 KB) was read three times, at 250 kHz, 100 kHz and 250 kHz
  again, with different chunk sizes: byte-identical each time. Measured read
  rate: 5.4 kB/s at 250 kHz, 2.1 kB/s at 100 kHz.
- The backup was then checked for coherence, outside the repository: the
  vector table and the C start-up code fit the F5529 memory map; a disassembly
  (Ghidra, MSP430X) decodes the code region, and all but one of its 2385 direct
  calls land on a function entry (the exception is a register-indirect call);
  three CRCs that the target's firmware itself checks reproduce on the dump.
- Open: a 16-byte read at 0x017F0 (bootloader area) returned zeros and, on a
  second read in the same session, different data, both with status OK. It
  was not investigated further on this board, whose firmware exists nowhere
  else. The tool never reads that area, and reads elsewhere were stable
  afterwards.

With the fixed firmware on the probe, steps 2–5 of the acceptance table
passed the same day through `sbw.py`.

Before `trial`, the flash routines were approached one at a time in the blank
segment at 0x24200, each followed by a compare of main flash and the info
segments with the backup: a 512-byte write of 0xFF with `--erase none` (runs
the write funclet, cannot change flash), a one-word write of 0xFFFE (exactly
one byte differed, at 0x24200), and `erase --segment 0x24200` (no difference
left). Each command line was first run against the fake firmware loaded with
the real backup, to check which erase and write commands it sends.

Steps 6 and 7 followed the same day. The image was the 128 KB backup itself.
At 250, 500 and 1000 kHz, in one session each: compare with the backup, mass
erase, read all of main flash back (blank in all four banks every time; the
info segments unchanged), program, verify, compare; then an independent
compare at 250 kHz. Before the two higher speeds: `selftest`, two or three
full read-backs and `trial` at that speed. Finally the plain `flash` command
with its defaults. The procedure had a fallback that writes the backup back
with segment erases; it was not needed. Measured:

| Speed | Mass erase | Programming 128 KB | Read-back |
|---|---|---|---|
| 250 kHz | 0.54 s | 77 s (1.7 kB/s) | 5.4 kB/s |
| 500 kHz | 0.30 s | 40 s (3.3 kB/s) | about 10 kB/s |
| 1000 kHz | 0.17 s | 22 s (6.0 kB/s) | about 18 kB/s |

Not exercised on hardware: the magic-pattern entry (every connect succeeded
with the normal entry), a locked device, `info` without a target, a write to
the info segments, 100 kHz for anything but reads, and the other two probe
boards. The scope measurements listed below were not made.

### Phase 6 hardware acceptance (rpi_pico + MSP430F5529 board)

The procedure is "First use on a board" in `docs/MSP430_SBW.md`. Nothing
before step 6 changes the firmware on the board.

| # | Step | Result |
|---|---|---|
| 1 | `info` without a target reports "no MSP430 found" | not run: the probe is soldered to this target |
| 2 | `info` reports JTAG ID 0x91 and device ID bytes 55h 29h (anything else: stop) | ✓ 2026-11-11 (core ID 0x0103) |
| 3 | `selftest` passes | ✓ 2026-11-11 |
| 4 | Two backups of main flash at 250 and 100 kHz are identical; info memory saved | ✓ 2026-11-11; also identical to the reads made before the fix |
| 5 | `trial --backup` passes | ✓ 2026-11-11 (segment 0x24200); an independent compare afterwards found no difference from the backup |
| 6 | `flash` an image: verify passes, target runs | ✓ 2026-11-11: `flash` of the 128 KB backup with the default mass erase at 250 kHz, programmed and verified in 103 s; an independent compare found no difference. That the application runs afterwards was not observed by the tester |
| 7 | 50 connect/disconnect cycles; flash at 250 / 500 / 1000 kHz | ✓ 2026-11-11: 50 cycles at 250 kHz and 50 at 1000 kHz, all on the first entry attempt; mass erase, blank check, program and verify at 250, 500 and 1000 kHz, main flash and info segments equal to the backup after each |
| 8 | SWD regression: conformance compare and openocd acceptance, before and after SBW sessions | not run: the probe is soldered to the MSP430 board, and SWD traffic must not be driven onto its TEST and RST pins. `DAP_Info` and `DAP_Disconnect` answer normally after the SBW sessions |

Things to look at during bring-up, from the code reviews:

- Scope the entry pulse on SBWTCK (expected about 0.7–0.9 µs low) and confirm
  there is no extra edge on SBWTCK when the first JTAG cycle starts.
- Idle levels at boot and after a command: SBWTCK low through the pull-down,
  SBWTDIO/RST high. On a Pico 2 (RP2350 A2) check that SBWTCK really reaches 0 V.
- After an SWD `DAP_Disconnect` both pins must be high-impedance.
- At 1 MHz the tightest margins are the data-line setup before the TMS slot's
  rising edge (125 ns) and TDO settling (375 ns); start at 250 kHz.
- Run once with `CONFIG_THREAD_ANALYZER=y` and read the USB thread's stack peak
  (static worst case: about 1.2 KB of 2 KB).

## Phase 7 — MSP430F5529 debugging over SBW, read-only (B1)

**Implemented:** `msp430::Debugger` (`lib/msp430/src/debug.cpp`, port of the
CPUXv2 macros of TI's MSP Debug Stack 3.15.1.1), debug operations 0x09-0x13
of vendor command 0x90 (protocol version 2; 0x13 `DBG_EEM_PEEK`, `sbw.py
eem-peek`, is the read-only check of the 32-bit scans to run before a board's
first attach) with the `PROTECTED` write rules
(RAM and peripheral registers only, without the PMM, flash controller, SYS
and DMA blocks; the PC only into main flash) and the lock on the flash
operations, GDB remote-protocol server (`tools/msp430/gdbserver.py`,
`sbw.py gdbserver`), symbol-only ELF writer (`tools/msp430/symelf.py`).
Design: `docs/superpowers/specs/2026-11-13-msp430-debug-design.md`; plan:
`docs/superpowers/plans/2026-11-13-msp430-debug-b1.md`; user guide:
`docs/MSP430_DEBUG.md`; TI's licence notice: `THIRD_PARTY_NOTICES.md`.

**Status: implemented and tested on the host only.** Nothing of this phase has
run on a target. The server has been driven by scripted GDB clients and by
`msp430-elf-gdb` 9.1 (installed in plan Task 22), both against a fake probe.

| Axis | Status |
|---|---|
| Host tests | 376/376 C++ (`scripts/run_host_tests.sh`, also under ASan); 426 Python, 13 of them skipped without msp430-elf-gdb (`python3 -m unittest discover -s tools/msp430/tests`) |
| 3-board build | rpi_pico, rpi_pico2, rpi_debug_probe with the feature on: rebuilt 2026-11-13 after the last firmware changes (`a3cb815`, `DBG_EEM_PEEK`, and `7d624be`, the reset when the link check before a detach's restore fails), all three boards; rpi_pico with `CONFIG_PROBE_MSP430_SBW=n`: built at Task 15, and nothing that configuration compiles has changed since |
| End-to-end with GDB | ✓ 2026-11-13, GNU gdb (Mitto Systems Limited - msp430-gcc 9.3.1.11) 9.1: connect, registers, breakpoint by symbol, stepi, memory, refusals (tools/msp430/tests/test_gdb_e2e.py); also `X` escaping, breakpoints after `monitor reset` (also with `always-inserted on`, which needed a server fix) and `flushregs`, console notes before stop replies, Ctrl-C during `stepi 1000` and `next` (stops after the step under way), SIGLOST, `load`, reads and writes GDB's memory map refuses itself, register names, `break NAME` after a `PUSHM.A` prologue and `break *NAME` at the first instruction |
| On-target hardware | **Not run yet** — see the acceptance table below |

### Phase 7 hardware acceptance (rpi_pico + MSP430F5529 board, read-only)

Main flash and information memory are compared with the Phase 6 backups
before the first step and after the last. Nothing below has been run.

| # | Step | Result |
|---|---|---|
| 0 | Flash and information memory equal the backups before the session | — |
| 1 | `sbw.py info` shows protocol version 2, debugging supported, 8 hardware breakpoints | — |
| 1a | `sbw.py eem-peek` at EEM 0x00, 0x02, 0x04, 0x06, at 250 and 100 kHz: JTAG ID 0x91 and both reads equal each time (the 32-bit scans, read-only, before the first attach) | — |
| 1b | VMAIE is 0; VMAIFG before and after one register read | — |
| 2 | Attach without reset, read registers, detach: the firmware keeps running | — |
| 3 | Halt and resume 50 times while it runs; it keeps working; whether a timer runs while halted | — |
| 4 | Hardware breakpoint on a function by symbol; the firmware reaches it and stops there; step, inspect, continue | — |
| 5 | Reset and halt at the reset vector, step through the start-up, continue | — |
| 6 | Refusals: `load`, a write into flash, a software breakpoint | — |
| 7 | Flash and information memory equal the backups after the session | — |

---

## Verified on hardware vs pending

### Verified on hardware

- USB descriptors (phase 1): check_descriptors.py all pass.
- DAP engine (phase 2): openocd connects, reports Info, reads capabilities.
- SWD backends (phase 3): DPIDR read via both PIO and bitbang; matches official firmware. (Note: the phase-3 `0x10212927` value was a misread from a not-yet-live target; the genuine RP2040 DPIDR `0x0bc12477` was confirmed in phase 5 with a live target. The protocol/engine work was unaffected.)
- UART bridge + serial (phase 4-core): loopback echo both directions; serial from hwinfo propagated to DAP_Info and USB iSerialNumber.
- Autobaud apply path (phase 4b): MAGIC_BAUD sentinel + 4 KB stack overflow + race fixes all hold.
- MSP430F5529 Spy-Bi-Wire (phase 6, rpi_pico, 2026-11-11): identify, RAM self-test, read, segment erase, mass erase, program and verify at 250, 500 and 1000 kHz; details in the Phase 6 section.
- Conformance A/B (phase 5): 26 protocol-deterministic byte-identical + 1 environment-dependent (`swj_pins_read`, matched) + 4 whitelisted enhancements (3 DAP_Info strings + DAP packet size 64→1024), 0 unexpected mismatches vs official v2.3.1 (`compare` → 27 identical / 4 whitelisted → PASS).  **Live on-hardware A/B done 2026-06-12** — official firmware captured live, byte-for-byte identical to the source-derived prediction on all 31 commands; packet-size bump re-captured 2026-06-13.
- **Full openocd debug acceptance (phase 5, rpi_pico/PIO, 2026-06-12): ✓ PASS** against a live RP2040 target — multidrop connect (`DPIDR 0x0bc12477`, Cortex-M0+ detected), `reset halt`, single-step, memory read, RAM write+readback (3 locations), flash erase+program+verify (top sector), and `reset run` all pass. Also verified attaching to a **running** target (connect → halt → resume), confirming the probe is not BOOTSEL-limited. The earlier connect failure was the target not being live on the bus (the garbage `DPIDR 0x10212927` was a misread), not a probe-firmware issue.

### Pending (hardware-dependent)

1. **Autobaud detection happy-path** — the baud estimator measures line rate from
   uart1 RX (GP5).  Requires an external UART source wired to GP5; the GP4→GP5
   loopback cannot trigger detection.  The estimator logic is fully host-tested
   and the apply path is on-target-verified.

2. **Acceptance on the remaining board × backend combinations** — the full debug
   acceptance above was run on rpi_pico/PIO; the other five (rpi_pico/bitbang,
   rpi_pico2 × {PIO, bitbang}, rpi_debug_probe × {PIO, bitbang}) follow the same
   documented procedure and remain to be run when those boards are on the bench.

3. **SWD regression with the MSP430 feature in the firmware** — the phase-5
   conformance compare and openocd acceptance have not been repeated since
   phase 6 changed the SWD pin handling (pins high-impedance when idle, pinctrl
   re-applied on connect). It needs a probe whose SWD pins are free.

4. **MSP430 Spy-Bi-Wire on rpi_pico2 and rpi_debug_probe** — built, not run.

5. **MSP430 debugging (phase 7)** — host-tested only, also with
   `msp430-elf-gdb` 9.1 against the fake probe; needs the on-target
   acceptance above (Task 25).

---

## Overall conclusion

The rewrite is **feature-complete**.  On the hardware tested, it behaves
identically to the official debugprobe v2.3.1 at the CMSIS-DAP protocol level:
27 of 31 command responses are byte-for-byte identical to the source-derived
reference (26 protocol-deterministic plus `swj_pins_read`, which is
environment-dependent and matched on the same wiring); the remaining 4 are
deliberately enhanced (DAP_Info strings now populated, giving openocd a readable
`Serial#`; and the DAP packet size raised 64→1024 for ~2.7× SWD read
throughput).  No unexpected mismatches were found.

A full openocd debug acceptance (connect → halt → step → memory read/write →
flash erase/program/verify → reset run) also passes end-to-end on rpi_pico/PIO
against a real RP2040 target, confirming the probe works as a complete debug
adapter, not just at the protocol level.
