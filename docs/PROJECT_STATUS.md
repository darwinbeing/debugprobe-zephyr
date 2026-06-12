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
| On-target hardware | **Verified (our firmware)** — live capture succeeded (`captured 31 responses`); macOS interface-claim fix validated end-to-end. Cross-checked vs the v2.3.1 source: **27 protocol-deterministic responses byte-identical + 1 environment-dependent (`swj_pins_read`, matched on the same wiring) + 3 whitelisted DAP_Info-string enhancements, 0 unexpected mismatches** (`compare` reports 28 identical / 3 whitelisted / 0 missing → PASS). **Confirmed by a live on-hardware A/B (2026-06-12): the official v2.3.1 firmware was flashed and captured live, matching the prediction byte-for-byte on all 31 commands.** See [CONFORMANCE_RESULTS.md](CONFORMANCE_RESULTS.md) for the full table. |

### Phase 5 hardware acceptance table (fill in per board × backend)

Run the acceptance suite in [`docs/HARDWARE_BRINGUP.md` — Full on-target
acceptance (phase 5)](HARDWARE_BRINGUP.md#full-on-target-acceptance-phase-5) and
record results here. A `—` means that board × backend combination has not been
run through the full suite yet (not a failure). `P*` = partial: passed every
check the available hardware allows (see the note below the table):

| Board | Backend | openocd accept | UART RT | autobaud | LEDs |
|---|---|---|---|---|---|
| rpi_pico | PIO | P* | ✓ | P* | n/a |
| rpi_pico | bitbang | — | — | — | — |
| rpi_pico2 | PIO | — | — | — | — |
| rpi_pico2 | bitbang | — | — | — | — |
| rpi_debug_probe | PIO | — | — | — | — |
| rpi_debug_probe | bitbang | — | — | — | — |

**rpi_pico / PIO, executed 2026-06-12 (`P*` detail):**
- **openocd accept (partial):** openocd 0.12.0 opens our CMSIS-DAP v2 interface,
  reports `FW Version = 2.0.0` and `Serial# = E661A4D41777B429`, initialises SWD,
  and reads `SWD DPIDR 0x10212927`. The full memory-R/W + flash step is **blocked
  by the multidrop RP2040 target** (the only target wired here), an environment
  limit, not a firmware defect. Two attempts on 2026-06-12 confirm this: a generic
  `cortex_m` connect reads the DPIDR then reports "Could not find MEM-AP"; the
  proper `target/rp2040.cfg` (multidrop-aware) reports `Failed to connect
  multidrop rp2040.dap0`. Our SWD is proven electrically working (DPIDR read OK);
  only the RP2040 multidrop DP-select fails — the same bus-contention issue that
  fails the **official** firmware identically (A/B-proven in phase 3). Full
  mem-R/W + flash acceptance still needs a single-drop SWD target (e.g. STM32 /
  nRF / bare Cortex-M), or resolution of the RP2040 multidrop bus contention.
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

## Verified on hardware vs pending

### Verified on hardware

- USB descriptors (phase 1): check_descriptors.py all pass.
- DAP engine (phase 2): openocd connects, reports Info, reads capabilities.
- SWD backends (phase 3): DPIDR read via both PIO and bitbang; matches official firmware.
- UART bridge + serial (phase 4-core): loopback echo both directions; serial from hwinfo propagated to DAP_Info and USB iSerialNumber.
- Autobaud apply path (phase 4b): MAGIC_BAUD sentinel + 4 KB stack overflow + race fixes all hold.
- Conformance A/B (phase 5): 27 protocol-deterministic byte-identical + 1 environment-dependent (`swj_pins_read`, matched) + 3 whitelisted DAP_Info strings, 0 unexpected mismatches vs official v2.3.1 (`compare` → 28 identical / 3 whitelisted → PASS).  **Live on-hardware A/B done 2026-06-12** — official firmware captured live, byte-for-byte identical to the source-derived prediction on all 31 commands.

### Pending (hardware-dependent)

1. **Autobaud detection happy-path** — the baud estimator measures line rate from
   uart1 RX (GP5).  Requires an external UART source wired to GP5; the GP4→GP5
   loopback cannot trigger detection.  The estimator logic is fully host-tested
   and the apply path is on-target-verified.

2. **Full openocd memory R/W + flash acceptance** — `mdw`/`mww` RAM round-trip
   and `load <elf>` / `verify_image` require a single-drop SWD target (STM32 /
   bare Cortex-M).  The multidrop RP2040 fails `connect` under both our firmware
   and the official v2.3.1, so it cannot be used as a target for this check.

---

## Overall conclusion

The rewrite is **feature-complete**.  On the hardware tested, it behaves
identically to the official debugprobe v2.3.1 at the CMSIS-DAP protocol level:
28 of 31 command responses are byte-for-byte identical to the source-derived
reference (27 protocol-deterministic plus `swj_pins_read`, which is
environment-dependent and matched on the same wiring); the remaining 3 are
deliberately enhanced (DAP_Info strings now populated, giving openocd a readable
`Serial#`).  No unexpected mismatches were found.
