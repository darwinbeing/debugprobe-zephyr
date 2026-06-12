# debugprobe-zephyr Phase 4b (Autobaud + Activity LEDs, custom bridge) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Reach full UART-bridge parity with the original debugprobe: a custom C++ `UartBridge` that owns the CDC↔uart1 data path (replacing the closed `zephyr,uart-bridge`), drives TX/RX activity LEDs, and runs PIO autobaud — measuring the target's TX baud when the host requests it via the MAGIC_BAUD sentinel and applying it to uart1.

**Architecture:** `UartBridge` (modeled on Zephyr's `drivers/serial/uart_bridge.c`: two interrupt-driven Zephyr UART devices — `cdc_acm_uart0` and `uart1` — with a ring buffer each direction) gains two taps the closed bridge couldn't offer: activity-LED blink on byte flow, and line-coding handling that starts autobaud on MAGIC_BAUD (9728) instead of forwarding it. Autobaud = a host-testable `dap`-free `Autobaud` estimator (port of `autobaud.c`'s histogram/bit-time/validity logic, no heap) fed by a PIO frequency-counter (`autobaud.pio`, pioasm-generated like phase 3) whose FIFO a thread drains; on a confident estimate it reconfigures uart1. The decision to replace `zephyr,uart-bridge` was made explicitly (it has no per-byte hook for the LEDs and no clean autobaud integration point).

**Tech Stack:** Zephyr 4.4.0 interrupt-driven UART + CDC-ACM + ring_buf + GPIO LED + RP2 PIO HAL (`pio_rpi_pico`, pioasm via `gen_pio.sh`) + a Zephyr thread; C++20. Reference firmware: `~/Developer/debugprobe/src/{cdc_uart.c,autobaud.c,autobaud.pio,autobaud.h}` (MAGIC_BAUD=9728). Reference Zephyr driver to model the bridge on: `~/Developer/zephyrproject/zephyr/drivers/serial/uart_bridge.c`.

---

## Why a custom bridge (recorded decision)

Phase-4 core used `zephyr,uart-bridge` (clean, verified). Phase 4b needs (a) TX/RX activity LEDs and (b) autobaud, both of which require owning the byte-level data path and the line-coding decision. Zephyr's `uart_bridge.h` exposes only `uart_bridge_settings_update` — no per-byte or activity hook — so this phase replaces it with a custom `UartBridge`. The custom bridge re-implements the same interrupt-driven, ring-buffered, line-coding-propagating behavior (so the phase-4-core loopback parity is preserved) and adds the two taps. `zephyr,uart-bridge` (the DT node + `CONFIG_UART_BRIDGE`) is removed.

## Hardware-verification boundary

A board is attached (Pico as probe). What's verifiable now: the bridge loopback (GP4→GP5) and activity LEDs (visual / onboard LED), the host-tested `Autobaud` estimator (CI), and the autobaud end-to-end IF a UART source drives GP5 at a known baud (the controller will arrange a test: e.g. a second serial device, or the probe's own uart0 TX wired to uart1 RX). PIO-timing-blind risks (like phase 3) apply to `autobaud.pio`; mitigated by pioasm-reproducible opcodes + line-by-line parity with `autobaud.pio`/`autobaud.c`, and the estimator's host tests.

## Board LED facts (for activity LEDs)

| Board | UART TX LED | UART RX LED |
|---|---|---|
| rpi_debug_probe | `green_led_uart` (GP7) or `yellow_led_uart` (GP8) — original: TX=GP8, RX=GP7 | the other |
| rpi_pico / pico2 | none (only the single `led0`, used for USB-connected in phase 4 core) | none |

So activity LEDs are a debug-probe-only visible feature; on the Picos the `uart-tx-led`/`uart-rx-led` aliases are absent → no-op (same `GPIO_DT_SPEC_GET_OR({0})` pattern as `LedHostStatus`).

**Build/flash/test env** (same as phases 1-4): `ZEPHYR_TOOLCHAIN_VARIANT=gnuarmemb`, `GNUARMEMB_TOOLCHAIN_PATH=$HOME/Developer/zephyrproject/arm-gnu-toolchain-14.2.rel1-darwin-x86_64-arm-none-eabi`, west via `/usr/local/opt/python@3.14/bin/python3.14 -m west`; `scripts/build_all.sh`, `scripts/run_host_tests.sh`, `scripts/gen_pio.sh`. BOOTSEL to flash.

**Branch:** create `phase4b-autobaud` from master; merge at the end.

---

### Task 1: Replace `zephyr,uart-bridge` with a custom C++ `UartBridge`

**Files:**
- Create: `src/uart_bridge.hpp`
- Create: `src/uart_bridge.cpp`
- Modify: `src/usb/usb_device.cpp` (line-coding callback → bridge), `CMakeLists.txt`, `prj.conf`
- Modify: `boards/rpi_pico.overlay`, `boards/rpi_pico2_rp2350a_m33.overlay`, `boards/rpi_debug_probe.overlay` (remove the `uart-bridge0` node; keep uart1 + pinctrl)

Read `~/Developer/zephyrproject/zephyr/drivers/serial/uart_bridge.c` FIRST — model the bridge on it (per-peer `ring_buf`, `uart_irq_rx_ready`/`uart_fifo_read`/`uart_fifo_fill`, RX-disable on ring-full + re-enable). Our version binds two fixed devices and exposes hooks.

- [ ] **Step 1: Write `src/uart_bridge.hpp`**

```cpp
#pragma once
/* CDC-ACM <-> hardware UART bridge (custom, replacing zephyr,uart-bridge),
 * so phase 4b can tap byte flow for activity LEDs and intercept the
 * MAGIC_BAUD line-coding to drive autobaud.
 */
#include <zephyr/device.h>

namespace probe {

/* Start the bridge between the CDC-ACM and the hardware UART (both Zephyr
 * uart devices). Call once at startup after the devices are ready. */
void uart_bridge_init();

/* Apply the CDC line coding to the hardware UART (called from the usbd
 * line-coding message). If the requested bit rate is MAGIC_BAUD it starts
 * autobaud instead of setting the UART. */
void uart_bridge_line_coding(const struct device *cdc_dev);

} // namespace probe
```

- [ ] **Step 2: Write `src/uart_bridge.cpp`** — the bridge. Faithful re-implementation of `uart_bridge.c`'s mechanism for the two fixed devices `DEVICE_DT_GET(DT_NODELABEL(cdc_acm_uart0))` and `DEVICE_DT_GET(DT_NODELABEL(uart1))`, plus the line-coding hook (no LEDs/autobaud yet — those are Tasks 2 and 4; leave clearly-marked hook points).

```cpp
#include "uart_bridge.hpp"

#include <zephyr/drivers/uart.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(uart_bridge, LOG_LEVEL_INF);

namespace probe {

namespace {

constexpr size_t kRingSize = 256;
constexpr size_t kFullThreshold = 16;

const struct device *const cdc_dev = DEVICE_DT_GET(DT_NODELABEL(cdc_acm_uart0));
const struct device *const uart_dev = DEVICE_DT_GET(DT_NODELABEL(uart1));

struct Peer {
	const struct device *dev;
	const struct device *peer;
	struct ring_buf rb;
	uint8_t buf[kRingSize];
};

Peer g_cdc;   /* data received FROM cdc, to be written TO uart */
Peer g_uart;  /* data received FROM uart, to be written TO cdc */

Peer &peer_for(const struct device *dev)
{
	return (dev == cdc_dev) ? g_cdc : g_uart;
}

/* Drain RX of `dev` into its ring; push ring into the peer's TX. Mirrors
 * uart_bridge.c interrupt_handler. */
void isr(const struct device *dev, void *user_data)
{
	ARG_UNUSED(user_data);
	Peer &self = peer_for(dev);

	while (uart_irq_update(dev) && uart_irq_is_pending(dev)) {
		if (uart_irq_rx_ready(dev)) {
			uint8_t *claim = nullptr;
			uint32_t n = ring_buf_put_claim(&self.rb, &claim, kRingSize);
			if (n == 0) {
				/* ring full: stop reading this side until drained */
				uart_irq_rx_disable(dev);
				break;
			}
			int got = uart_fifo_read(dev, claim, n);
			ring_buf_put_finish(&self.rb, got > 0 ? got : 0);
			if (got > 0) {
				/* TODO(Task 2): rx/tx activity LED blink here */
				uart_irq_tx_enable(self.peer);
			}
		}
		if (uart_irq_tx_ready(dev)) {
			/* dev is a peer's TX target: drain the OTHER side's ring
			 * (the ring whose .peer == dev). */
			Peer &src = (dev == cdc_dev) ? g_uart : g_cdc;
			uint8_t *out = nullptr;
			uint32_t n = ring_buf_get_claim(&src.rb, &out, kRingSize);
			if (n == 0) {
				uart_irq_tx_disable(dev);
			} else {
				int sent = uart_fifo_fill(dev, out, n);
				ring_buf_get_finish(&src.rb, sent > 0 ? sent : 0);
				/* re-enable the source RX if it was paused */
				uart_irq_rx_enable(src.dev);
			}
		}
	}
}

} // namespace

void uart_bridge_init()
{
	if (!device_is_ready(cdc_dev) || !device_is_ready(uart_dev)) {
		LOG_ERR("bridge devices not ready");
		return;
	}
	g_cdc = {cdc_dev, uart_dev, {}, {}};
	g_uart = {uart_dev, cdc_dev, {}, {}};
	ring_buf_init(&g_cdc.rb, kRingSize, g_cdc.buf);
	ring_buf_init(&g_uart.rb, kRingSize, g_uart.buf);

	uart_irq_callback_user_data_set(cdc_dev, isr, nullptr);
	uart_irq_callback_user_data_set(uart_dev, isr, nullptr);
	uart_irq_rx_enable(cdc_dev);
	uart_irq_rx_enable(uart_dev);
}

void uart_bridge_line_coding(const struct device *cdc)
{
	struct uart_config cfg;
	if (uart_config_get(cdc, &cfg) != 0) {
		return;
	}
	/* TODO(Task 4): if (cfg.baudrate == 9728 /MAGIC_BAUD/) { autobaud_start(); return; } */
	uart_configure(uart_dev, &cfg);
}

} // namespace probe
```

The `isr` peer/TX wiring above is subtle — verify against `uart_bridge.c` that "data received on dev goes to peer's TX" is modeled correctly. If the claim/finish + the cross-peer TX drain doesn't match uart_bridge.c's structure, adopt uart_bridge.c's exact two-index scheme (it uses `peer[idx]` rings keyed by `uart_bridge_get_idx`). Correctness of the loopback test (Task 1 Step 7) is the gate.

- [ ] **Step 3: Remove the `zephyr,uart-bridge` node from all 3 overlays**

Delete the `uart_bridge0 { compatible="zephyr,uart-bridge"; peers=... };` node from `boards/rpi_pico.overlay`, `boards/rpi_pico2_rp2350a_m33.overlay`, `boards/rpi_debug_probe.overlay`. Keep the uart1 enable + pinctrl (the custom bridge uses the uart1 device directly). Keep `cdc_acm_uart0`.

- [ ] **Step 4: prj.conf — swap the bridge Kconfig**

Remove `CONFIG_UART_BRIDGE=y`. Keep/ensure `CONFIG_SERIAL=y`, `CONFIG_UART_INTERRUPT_DRIVEN=y` (now required explicitly since UART_BRIDGE no longer selects it), `CONFIG_UART_LINE_CTRL=y`, `CONFIG_RING_BUFFER=y`.

- [ ] **Step 5: usb_device.cpp — route line coding to the custom bridge**

In `usb_msg_cb`, replace the `uart_bridge_settings_update(msg->dev, DEVICE_DT_GET(DT_NODELABEL(uart_bridge0)))` call (CDC line-coding/control-line-state cases) with `probe::uart_bridge_line_coding(msg->dev);`. Remove the `#include <zephyr/drivers/uart/uart_bridge.h>` and the `uart_bridge0` DT reference; add `#include "uart_bridge.hpp"`. Keep the USB-LED cases unchanged.

- [ ] **Step 6: main.cpp — start the bridge; CMakeLists — add the source**

In `src/main.cpp`, after `probe::usb::init()`, call `probe::uart_bridge_init();` (add `#include "uart_bridge.hpp"`). Add `src/uart_bridge.cpp` to `CMakeLists.txt` `target_sources`.

- [ ] **Step 7: Build all three boards + host tests**

```bash
~/Developer/debugprobe-zephyr/scripts/build_all.sh
~/Developer/debugprobe-zephyr/scripts/run_host_tests.sh
```

Expected: three uf2s; `OK: 73 tests`.

- [ ] **Step 8: Commit**

```bash
cd ~/Developer/debugprobe-zephyr
git add -A
git commit -m "feat: custom C++ UartBridge replacing zephyr,uart-bridge"
```

- [ ] **Step 9: On-target (controller)** — flash rpi_pico, GP4→GP5 loopback, confirm the CDC echoes bytes both directions (same parity test as phase-4 core). Note pending.

---

### Task 2: TX/RX activity LEDs in the bridge

**Files:**
- Modify: `src/leds.hpp`, `src/leds.cpp` (add uart-activity LED helpers + debounce)
- Modify: `src/uart_bridge.cpp` (blink on byte flow)
- Modify: `boards/rpi_debug_probe.overlay` (uart-tx-led / uart-rx-led aliases)

Mirror the original (`cdc_uart.c`): light the LED on a byte, hold it ~40 ms, then off (debounce). Use a Zephyr `k_timer` that turns the LED off; each byte re-arms it.

- [ ] **Step 1: Add activity LEDs to `src/leds.hpp`**

```cpp
/* UART bridge activity: call on each byte; the LED lights and auto-clears
 * ~40 ms after the last byte (debounce). No-op if the alias is absent. */
void leds_uart_tx_activity();
void leds_uart_rx_activity();
```

- [ ] **Step 2: Implement in `src/leds.cpp`** — two LEDs (`uart-tx-led`, `uart-rx-led`) + a shared debounce timer per LED.

```cpp
#include <zephyr/kernel.h>

namespace {
const struct gpio_dt_spec uart_tx_led = LED_SPEC(uart_tx_led);
const struct gpio_dt_spec uart_rx_led = LED_SPEC(uart_rx_led);

void tx_off(struct k_timer *) { led_set(&uart_tx_led, false); }
void rx_off(struct k_timer *) { led_set(&uart_rx_led, false); }
K_TIMER_DEFINE(tx_timer, tx_off, nullptr);
K_TIMER_DEFINE(rx_timer, rx_off, nullptr);
constexpr k_timeout_t kDebounce = K_MSEC(40);
} // namespace

void leds_uart_tx_activity()
{
	led_set(&uart_tx_led, true);
	k_timer_start(&tx_timer, kDebounce, K_NO_WAIT);
}
void leds_uart_rx_activity()
{
	led_set(&uart_rx_led, true);
	k_timer_start(&rx_timer, kDebounce, K_NO_WAIT);
}
```

(Add `led_configure(&uart_tx_led); led_configure(&uart_rx_led);` to the existing `LedInit` ctor. The `LED_SPEC` macro and `led_set`/`led_configure` helpers already exist from phase 4 core — reuse them; if they're in an anonymous namespace, place the new specs/timers alongside.)

- [ ] **Step 3: Blink from the bridge ISR (`src/uart_bridge.cpp`)**

At the `/* TODO(Task 2) */` points: when bytes are read FROM the CDC (they go TO uart1 = host→target TX) call `probe::leds_uart_tx_activity()`; when bytes are read FROM uart1 (they go TO the CDC = target→host RX) call `probe::leds_uart_rx_activity()`. Add `#include "leds.hpp"`. Map exactly as the original: TX LED = host-to-target data, RX LED = target-to-host data.

- [ ] **Step 4: Debug-probe overlay aliases**

In `boards/rpi_debug_probe.overlay` aliases, add (original: UART_TX_LED=GP8=`yellow_led_uart`, UART_RX_LED=GP7=`green_led_uart` — verify the node labels & GPIO numbers in the board DTS):

```dts
		uart-tx-led = &yellow_led_uart;
		uart-rx-led = &green_led_uart;
```

(Picos have no such LEDs; leave their overlays without these aliases → no-op.)

- [ ] **Step 5: Build all three boards + host tests**

Expected: three uf2s; `OK: 73 tests`.

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "feat: UART TX/RX activity LEDs (debounced) in the bridge"
```

- [ ] **Step 7: On-target (controller)** — on a debug-probe board the two UART LEDs blink on bridge traffic; on rpi_pico the aliases are absent (no visible LED). Note pending.

---

### Task 3: autobaud PIO program + host-tested `Autobaud` estimator

**Files:**
- Create: `drivers/dp_pio/autobaud.pio`
- Modify: `scripts/gen_pio.sh` (assemble autobaud.pio too)
- Modify: `drivers/dp_pio/probe_pio.h` is generated; autobaud opcodes go to a new generated `drivers/dp_pio/autobaud_pio.h`
- Create: `lib/dap/include/dap/autobaud.hpp` — wait, autobaud is not DAP. Put it under the app: `src/autobaud/estimator.hpp`, `src/autobaud/estimator.cpp`
- Create: `tests/host/test_autobaud.cpp`
- Modify: `tests/host/CMakeLists.txt`

The estimator is pure C++ (no Zephyr) so it lives where host tests can compile it. Keep it under `src/autobaud/` but make `tests/host/CMakeLists.txt` compile `src/autobaud/estimator.cpp` directly (it has zero Zephyr deps).

- [ ] **Step 1: Copy + adapt `autobaud.pio`**

```bash
cp ~/Developer/debugprobe/src/autobaud.pio ~/Developer/debugprobe-zephyr/drivers/dp_pio/autobaud.pio
```

Strip the trailing `% c-sdk { ... %}` block (the Zephyr driver does SM config itself, Task 4). Keep `.program autobaud` + the instructions verbatim (falling_edge/count_cycles/rising_edge counter).

- [ ] **Step 2: Extend `scripts/gen_pio.sh` to assemble autobaud.pio**

Add, alongside the probe.pio/probe_oen.pio generation, a third invocation producing `drivers/dp_pio/autobaud_pio.h` via the same `RPI_PICO_PIO_DEFINE_PROGRAM` reshaping (pioasm `-o c-sdk`, strip pico-sdk-only bits, keep opcodes + wrap + offsets). Run `~/Developer/debugprobe-zephyr/scripts/gen_pio.sh` and confirm it reproduces both headers deterministically.

- [ ] **Step 3: Write the failing host tests** `tests/host/test_autobaud.cpp`

The estimator consumes raw PIO samples (`raw`, where cycle_count = `(UINT32_MAX - raw) * 2`, per autobaud.c:218) and emits a baud estimate + validity. Test it with synthesized samples for a known baud: at PIO clock 125 MHz, one bit at baud B lasts `125e6 / B` PIO cycles. Feed many samples clustered at that bit-time (plus a few noise outliers) and assert the estimate converges within 0.5%.

```cpp
#include "testing.hpp"
#include "autobaud/estimator.hpp"

#include <cstdint>

namespace {
/* raw such that (UINT32_MAX - raw)*2 == cycles */
uint32_t raw_for_cycles(uint32_t cycles)
{
	return UINT32_MAX - (cycles / 2);
}
/* PIO cycles for one bit at `baud` (PIO clock 125 MHz). */
uint32_t bit_cycles(uint32_t baud) { return 125000000u / baud; }
} // namespace

TEST(autobaud_converges_to_115200)
{
	autobaud::Estimator est(125000000u);
	uint32_t bc = bit_cycles(115200); /* ~1085 */
	autobaud::Result r{};
	for (int i = 0; i < 200; i++) {
		/* mostly 1-bit periods, a few 2-3 bit runs (longer lows) */
		uint32_t cyc = (i % 7 == 0) ? bc * 2 : bc;
		if (i % 13 == 0) cyc = bc * 3;
		r = est.add_sample(raw_for_cycles(cyc));
	}
	CHECK(r.valid);
	/* within 0.5% of 115200 */
	CHECK(r.baud > 114624 && r.baud < 115776);
}

TEST(autobaud_converges_to_9600)
{
	autobaud::Estimator est(125000000u);
	uint32_t bc = bit_cycles(9600);
	autobaud::Result r{};
	for (int i = 0; i < 300; i++) {
		uint32_t cyc = (i % 5 == 0) ? bc * 2 : bc;
		r = est.add_sample(raw_for_cycles(cyc));
	}
	CHECK(r.valid);
	CHECK(r.baud > 9552 && r.baud < 9648);
}

TEST(autobaud_noise_only_is_invalid)
{
	autobaud::Estimator est(125000000u);
	autobaud::Result r{};
	/* random-ish scattered periods, no dominant bit-time */
	uint32_t periods[] = {500, 1700, 30, 9000, 220, 4400, 77, 12000};
	for (int i = 0; i < 50; i++) {
		r = est.add_sample(raw_for_cycles(periods[i % 8] + i));
	}
	CHECK(!r.valid);
}
```

Add `test_autobaud.cpp` + `../../src/autobaud/estimator.cpp` to `tests/host/CMakeLists.txt` (compile the estimator into the host test binary). Run `scripts/run_host_tests.sh`; expect link/compile failure (estimator not implemented yet).

- [ ] **Step 4: Write `src/autobaud/estimator.hpp`**

```cpp
#pragma once
/* Baud-rate estimator: consumes raw PIO frequency-counter samples and
 * estimates the target's UART baud. Pure C++ (no Zephyr, no heap), a port
 * of the original debugprobe autobaud.c histogram/bit-time/validity logic.
 */
#include <cstddef>
#include <cstdint>

namespace autobaud {

struct Result {
	uint32_t baud = 0;
	float validity = 0.0f;
	bool valid = false; /* validity > 0.6 and baud changed meaningfully */
};

class Estimator {
public:
	explicit Estimator(uint32_t pio_clock_hz);

	/* Feed one raw PIO sample. Returns the current best estimate; the
	 * result is `valid` only when confidence crosses the threshold. */
	Result add_sample(uint32_t raw);

	void reset();

private:
	static constexpr size_t kTableSize = 509; /* prime, open addressing */
	struct Entry { uint32_t key; uint32_t count; };

	uint32_t insert_and_count(uint32_t cycles); /* hash table */

	uint32_t pio_clock_;
	Entry table_[kTableSize];
	uint32_t total_samples_;
	uint32_t min_cycles_;
	uint32_t max_cycles_;
	uint32_t bit_time_sum_;
	uint32_t bit_time_count_;
	uint32_t outlier_count_;
	float baud_;
};

} // namespace autobaud
```

- [ ] **Step 5: Write `src/autobaud/estimator.cpp`** — port `autobaud.c` lines 91-263 (hash/insert/get_count + estimate_baud_rate body) into the class, using a fixed `table_[kTableSize]` instead of malloc, `cycles = (UINT32_MAX - raw) * 2`, the min/max + 1-bit-window + average-bit-time + completeness/consistency/validity heuristics (autobaud.c:236-258), with the same constants (`MIN_FREQUENCY 0.05`, validity > 0.6, baud_changed 0.5% tolerance autobaud.c:197-203). Use `<cmath>` for `expf`/`fminf`/`roundf` (host + target both have it). Cite the autobaud.c line ranges in comments.

- [ ] **Step 6: Run host tests until green** — all three autobaud tests pass plus the existing 73 (`OK: 76 tests`). Tune nothing in the algorithm to pass — if a test's expectation is wrong vs the ported algorithm, fix the test's synthesized input, not the algorithm; the algorithm must match autobaud.c.

- [ ] **Step 7: Commit**

```bash
git add drivers/dp_pio/autobaud.pio drivers/dp_pio/autobaud_pio.h scripts/gen_pio.sh \
        src/autobaud tests/host
git commit -m "feat: autobaud PIO program + host-tested baud estimator"
```

---

### Task 4: Autobaud capture + integration (PIO on uart1 RX, MAGIC_BAUD trigger)

**Files:**
- Create: `src/autobaud/autobaud.hpp`, `src/autobaud/autobaud.cpp` (the firmware glue: PIO freq counter on uart1 RX, thread-drain → Estimator, pin switching, apply baud)
- Modify: `src/uart_bridge.cpp` (MAGIC_BAUD → autobaud_start; stop on a real baud)
- Modify: `boards/*.overlay` (an `autobaud` pinctrl state routing the uart1 RX pin to PIO0; a `&pio0` SM is already enabled)
- Modify: `CMakeLists.txt`

Autobaud temporarily repurposes uart1's RX pin (GP5 on Pico, GP5 on debug-probe) from UART to the PIO frequency counter, measures, applies the detected baud to uart1, and returns the pin to UART. The host requests this by setting the CDC baud to MAGIC_BAUD (9728).

- [ ] **Step 1: Write `src/autobaud/autobaud.hpp`**

```cpp
#pragma once
/* Firmware autobaud: runs the PIO frequency counter on the target UART RX
 * pin, feeds the Estimator, and applies the detected baud to the bridged
 * UART. Triggered when the host sets MAGIC_BAUD.
 */
#include <cstdint>

namespace probe {

inline constexpr uint32_t kMagicBaud = 9728; /* autobaud.h MAGIC_BAUD 0x2600 */

/* Begin measuring; on a confident estimate, reconfigure uart1 to the
 * detected baud and stop. Idempotent. */
void autobaud_start();
/* Abort measuring and restore uart1's RX pin to the UART function. */
void autobaud_stop();

} // namespace probe
```

- [ ] **Step 2: Write `src/autobaud/autobaud.cpp`** — the glue. Mirror `autobaud.c`'s init/deinit + the FreeRTOS thread, but: capture via thread-drain (no DMA), and pin-switch via Zephyr pinctrl. Structure (fill bodies, citing autobaud.c):
  - State: a dedicated thread (`K_THREAD_STACK_DEFINE` + `k_thread`), a `k_event`/flag for start/stop, the `autobaud::Estimator`, the PIO device (`DEVICE_DT_GET(DT_NODELABEL(pio0))`), claimed SM + program offset (load `autobaud_program` from `autobaud_pio.h`).
  - `autobaud_start()`: signal the thread to begin. The thread: apply the `autobaud` pinctrl state (uart1 RX pin → PIO0, input-enable) so the PIO sees edges; configure+enable the SM (set_in_pins/jmp_pin = RX pin, clkdiv = clk_sys/125MHz, like autobaud_program_init); loop: `pio_sm_get` (nonblocking; skip if FIFO empty) → `Estimator::add_sample(raw)`; on a `valid` result → `uart_configure(uart1, {baud=result.baud,...})`, then stop (return the pin to the UART default pinctrl, disable+unclaim the SM).
  - `autobaud_stop()`: signal stop; the thread restores the uart1 default pinctrl state and tears down the SM.
  - Pin switching: the board overlays define two pinctrl states for the autobaud RX pin — the existing uart1 `default` (RX = UART funcsel) and a new `autobaud` state (RX = PIO0 funcsel, input-enable). Apply via `pinctrl_apply_state(uart1_pcfg, PINCTRL_STATE_*)` — BUT the uart1 device owns its pinctrl; cleaner is a dedicated pinctrl dev-config on the autobaud node. Simplest robust approach: give the autobaud a small DT node `raspberrypi,swdp-pio`-style with its own `pinctrl-0=<&autobaud_default>` (RX→PIO) and apply it on start; on stop, re-apply uart1's default via `uart_configure` re-init OR by re-applying the uart1 pinctrl. Pick the approach that builds and correctly hands the pin back; record it. (If dynamic re-pinctrl of a live uart proves fragile, the fallback is: during autobaud, also `uart_irq_rx_disable(uart1)` so the UART ignores the pin, and only toggle the funcsel.)

NOTE: this pin hand-off is the trickiest, hardware-only-verifiable part. Keep `autobaud_stop()` robust: it MUST restore normal UART RX on the pin so the bridge keeps working after autobaud.

- [ ] **Step 3: Trigger from the bridge** — in `src/uart_bridge.cpp` `uart_bridge_line_coding`, replace the Task-1 TODO:

```cpp
	struct uart_config cfg;
	if (uart_config_get(cdc, &cfg) != 0) {
		return;
	}
	if (cfg.baudrate == probe::kMagicBaud) {
		probe::autobaud_start();
		return; /* do not push MAGIC_BAUD to the real UART */
	}
	probe::autobaud_stop(); /* a real baud cancels any running autobaud */
	uart_configure(uart_dev, &cfg);
```

Add `#include "autobaud/autobaud.hpp"`.

- [ ] **Step 4: Overlays — autobaud RX-pin pinctrl state + CMake**

Add to each board overlay an `autobaud_default` pinctrl state routing the uart1 RX pin to PIO0 (rpi_pico/pico2: GP5 → `PIO0_P5` input-enable; rpi_debug_probe: GP5 → `PIO0_P5` input-enable — confirm the actual uart1 RX pin per board) and whatever DT node the driver uses to find it. Add `src/autobaud/autobaud.cpp` + `src/autobaud/estimator.cpp` to `CMakeLists.txt` target_sources. Run `gen_pio.sh` so `autobaud_pio.h` is current.

- [ ] **Step 5: Build all three boards + host tests**

```bash
~/Developer/debugprobe-zephyr/scripts/gen_pio.sh
~/Developer/debugprobe-zephyr/scripts/build_all.sh
~/Developer/debugprobe-zephyr/scripts/run_host_tests.sh
```

Expected: three uf2s; `OK: 76 tests` (estimator tests from Task 3 + the existing 73).

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "feat: PIO autobaud capture + MAGIC_BAUD trigger, applies detected baud"
```

- [ ] **Step 7: On-target (controller)** — flash rpi_pico; drive uart1 RX (GP5) from a known UART source (e.g. the probe's own uart0 TX, or a second serial adapter) at e.g. 9600/115200; open the CDC at MAGIC_BAUD (9728) to start autobaud; confirm uart1 gets reconfigured to the detected baud (bridge then passes data correctly at that baud). Note pending — needs a UART source.

---

### Task 5: Spec sync + wrap-up

**Files:**
- Modify: `docs/superpowers/specs/2026-06-11-debugprobe-zephyr-design.md`

- [ ] **Step 1: Update the spec ③ section** — record that phase 4b replaced `zephyr,uart-bridge` with a custom `UartBridge` (to enable activity LEDs + autobaud), added TX/RX activity LEDs, and implemented autobaud as a PIO frequency counter + a host-tested `Estimator` (no DMA — thread-drain) triggered by MAGIC_BAUD. Update the "Testing and Acceptance" autobaud line to active (no longer deferred). Note the DMA-vs-thread-drain decision and that capture is thread-drained (robustness note: may drop samples at very high baud; the estimator is statistical).

- [ ] **Step 2: Verify the phase-4b acceptance checklist**

- `scripts/run_host_tests.sh` → `OK: 76 tests` (3 estimator tests added).
- `scripts/build_all.sh` → 3 boards.
- `scripts/gen_pio.sh` reproduces probe_pio.h + autobaud_pio.h with no diff.
- On-target: bridge loopback still works (custom bridge); activity LEDs blink (debug-probe); autobaud detects a known UART source baud and applies it (controller, needs a UART source).

- [ ] **Step 3: Commit**

```bash
git add docs/
git commit -m "docs: sync spec for phase 4b (custom bridge, activity LEDs, autobaud)"
```

---

## Known Risks and Handling

- **Custom bridge correctness**: the trickiest part is the interrupt-driven ring/TX wiring. Model it EXACTLY on `uart_bridge.c` (the loopback test is the gate); if the simplified `isr` here misbehaves, adopt uart_bridge.c's `peer[idx]` two-index scheme verbatim.
- **Autobaud pin hand-off (hardware-only)**: returning uart1's RX pin from PIO back to UART after autobaud is the highest-risk item; `autobaud_stop()` must reliably restore UART RX or the bridge dies after autobaud. Test the stop path on hardware first (start autobaud, then set a real baud, confirm the bridge still echoes).
- **Thread-drain vs DMA**: the original uses DMA for no-miss capture; we thread-drain the PIO FIFO (`push noblock`, so it drops on overflow). Acceptable for a statistical estimate; documented. If high-baud detection proves unreliable on hardware, a Zephyr `DMA_RPI_PICO` cyclic channel is the future enhancement (out of scope here).
- **autobaud.pio timing-blind**: same class of risk as phase-3 PIO; mitigated by pioasm-reproducible opcodes + line-by-line parity with autobaud.pio + the host-tested estimator. The PIO program is simple (edge-to-edge cycle counter).
- **PIO SM budget**: phase-3 SWD uses one pio0 SM (held for device lifetime). Autobaud claims another pio0 SM only while running, releasing it on stop — confirm pio0 has a free SM (4 total; SWD uses 1).
- **Estimator host/target parity**: the estimator is pure C++ compiled in BOTH the host test binary and the firmware — identical code, so host tests genuinely validate the firmware logic.

