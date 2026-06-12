/* Firmware autobaud glue. Faithful port of the structure of debugprobe
 * src/autobaud.c (autobaud_init/deinit + autobaud_thread, autobaud.c:311-387)
 * to Zephyr, with two deliberate substitutions:
 *
 *   1. Capture is a thread that drains the PIO RX FIFO with pio_sm_get
 *      (nonblocking) instead of the original's DMA ring buffer
 *      (autobaud.c:146-193, 205-263). The estimator is statistical, so the
 *      occasional dropped sample at high baud is acceptable -- recorded
 *      simplification.
 *   2. The PIO SM is configured the Zephyr way: pin funcsel via pinctrl
 *      (instead of pio_gpio_init) and SM allocation via the pio_rpi_pico
 *      misc HAL (instead of a hard-coded SM), mirroring drivers/dp_pio/
 *      swdp_pio.c. The SM config itself replicates autobaud.pio's
 *      autobaud_program_init c-sdk block (autobaud.pio:24-40):
 *      in_pins = jmp_pin = rx_pin, input pindir, clkdiv = clk_sys/125MHz.
 *
 * Pin hand-off (the trickiest, hardware-only-verifiable part): uart1's RX pin
 * (GP5 on all three boards) is temporarily routed to PIO0 while measuring and
 * routed BACK to UART1 on stop. Two single-state pinctrl dev-configs are used
 * (the robust "two node" approach blessed by the plan, rather than two named
 * states on one node): autobaud_pio (GP5->PIO0) applied on start, autobaud_uart
 * (GP5->UART1) applied in teardown. The teardown ALWAYS re-applies the UART
 * state and re-enables uart1 RX, so autobaud_stop() reliably hands the pin back
 * to the bridge -- the #1 correctness requirement.
 */
#include "autobaud/autobaud.hpp"
#include "autobaud/estimator.hpp"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/logging/log.h>

/* Pull in the pico-sdk PIO HAL (C++ inline fns) first under normal C++ linkage,
 * so its include guard is set before the extern "C" wrap below. */
#include <hardware/pio.h>

/* pio_rpi_pico.h declares pio_rpi_pico_allocate_sm with C linkage but lacks
 * extern "C" guards; wrap so this C++ TU resolves the unmangled symbol. The
 * header's own #include <hardware/pio.h> is already guarded out by the line
 * above, so only the allocate_sm prototype is affected by the wrap. */
extern "C" {
#include <zephyr/drivers/misc/pio_rpi_pico/pio_rpi_pico.h>
}

#include "autobaud_pio.h"

LOG_MODULE_REGISTER(autobaud, LOG_LEVEL_INF);

namespace probe {

namespace {

/* PIO clock the estimator's bit-times are measured against -- autobaud.c:25
 * (PIO_CLOCK_FREQUENCY 125000000). The clkdiv below scales clk_sys down to it. */
constexpr uint32_t kPioClockHz = 125000000U;

/* clk_sys, the PIO source clock (compile-time DT constant, as in swdp_pio.c:43).
 * div = clk_sys / 125MHz mirrors autobaud.c:323. */
constexpr uint32_t kClkSysHz = DT_PROP(DT_NODELABEL(clk_sys), clock_frequency);

/* uart1 RX GPIO number (GP5 on all three boards) carried by the autobaud DT
 * anchor; passed to the PIO SM as in_pins/jmp_pin (autobaud.pio:31-32). */
constexpr uint32_t kRxPin = DT_PROP(DT_NODELABEL(autobaud_pio), rx_pin);

/* The bridged hardware UART whose RX pin autobaud borrows and whose baud it
 * sets on a confident estimate. */
const struct device *const uart_dev = DEVICE_DT_GET(DT_NODELABEL(uart1));

/* PIO0 -- the SAME pio the SWD driver uses; autobaud claims a SEPARATE SM only
 * while running (4 SMs total; SWD holds 1 for the device lifetime). Resolved via
 * the DT phandle (not DT_NODELABEL(pio0)) because pico-sdk's hardware/pio.h
 * #defines `pio0` to `pio0_hw`, which would corrupt the DT_CAT token paste. */
const struct device *const pio_dev =
	DEVICE_DT_GET(DT_PHANDLE(DT_NODELABEL(autobaud_pio), piodev));

/* Two single-state pinctrl dev-configs for the GP5 hand-off (see file header). */
PINCTRL_DT_DEFINE(DT_NODELABEL(autobaud_pio));
PINCTRL_DT_DEFINE(DT_NODELABEL(autobaud_uart));
const struct pinctrl_dev_config *const pio_pcfg =
	PINCTRL_DT_DEV_CONFIG_GET(DT_NODELABEL(autobaud_pio));
const struct pinctrl_dev_config *const uart_pcfg =
	PINCTRL_DT_DEV_CONFIG_GET(DT_NODELABEL(autobaud_uart));

/* Start/stop signalling. START asks the idle thread to begin a run; STOP asks
 * a running thread to tear down; STOPPED is posted by the thread once teardown
 * has fully restored the UART pin, so autobaud_stop() can block until then
 * (mirrors autobaud_wait_stop, autobaud.c:351-354). g_running gives
 * autobaud_start() its idempotent no-op-if-already-running check
 * (autobaud.c:363-371's autobaud_running guard). */
constexpr uint32_t kEvtStart = BIT(0);
constexpr uint32_t kEvtStop = BIT(1);
constexpr uint32_t kEvtStopped = BIT(2);
K_EVENT_DEFINE(g_evt);
atomic_t g_running = ATOMIC_INIT(0);

/* Restore uart1's RX pin to the UART function and re-arm UART RX. ALWAYS run on
 * teardown (valid estimate OR stop), even if measurement never converged --
 * this is what lets the bridge survive autobaud. Mirrors the pin/state cleanup
 * half of autobaud_deinit (autobaud.c:265-309), minus the DMA teardown. */
void restore_uart_pin()
{
	int ret = pinctrl_apply_state(uart_pcfg, PINCTRL_STATE_DEFAULT);
	if (ret < 0) {
		LOG_ERR("restore uart pinctrl failed: %d", ret);
	}
	/* We disabled uart1 RX at start so the UART wouldn't fight the pin while
	 * PIO owned it; re-arm it. The bridge re-enables peer TX on the next
	 * received byte. */
	uart_irq_rx_enable(uart_dev);
}

/* One measurement run: set up the SM, drain its FIFO into the Estimator, apply
 * the detected baud, and ALWAYS tear the pin back to UART before returning.
 * Structure mirrors autobaud_init (autobaud.c:311-345) + the estimate loop of
 * autobaud_thread (autobaud.c:372-385). */
void run_once()
{
	PIO pio = pio_rpi_pico_get_pio(pio_dev);
	size_t sm = 0;

	/* Claim a free SM from the Zephyr-managed allocator (the SWD driver uses
	 * the same call). autobaud.c:314 pio_claim_unused_sm. */
	int ret = pio_rpi_pico_allocate_sm(pio_dev, &sm);
	if (ret < 0) {
		LOG_ERR("no free PIO SM: %d", ret);
		restore_uart_pin();
		return;
	}

	/* Route GP5 -> PIO0 (input-enable) so the SM sees the line edges. This is
	 * the Zephyr equivalent of the receive-only funcsel in autobaud.pio's
	 * c-sdk (autobaud.pio:30 "No need to set PIO funcsel ... receive-only";
	 * we still need the pad on the PIO input function + input-enable). */
	ret = pinctrl_apply_state(pio_pcfg, PINCTRL_STATE_DEFAULT);
	if (ret < 0) {
		LOG_ERR("apply pio pinctrl failed: %d", ret);
		pio_sm_unclaim(pio, sm);
		restore_uart_pin();
		return;
	}

	/* Add the program -- autobaud.c:318 pio_add_program. */
	uint offset = pio_add_program(pio, RPI_PICO_PIO_GET_PROGRAM(autobaud));

	/* Configure the SM, replicating autobaud_program_init (autobaud.pio:24-40):
	 * in_pins = jmp_pin = rx_pin, input pindir, clkdiv = clk_sys/125MHz,
	 * wrap from the generated header. RPI_PICO_PIO_DEFINE_PROGRAM emits no
	 * *_get_default_config, so set wrap explicitly (as swdp_pio.c does). */
	pio_sm_config c = pio_get_default_sm_config();
	sm_config_set_wrap(&c, offset + RPI_PICO_PIO_GET_WRAP_TARGET(autobaud),
			   offset + RPI_PICO_PIO_GET_WRAP(autobaud));
	sm_config_set_in_pins(&c, kRxPin);   /* autobaud.pio:31 */
	sm_config_set_jmp_pin(&c, kRxPin);   /* autobaud.pio:32 */
	pio_sm_set_consecutive_pindirs(pio, sm, kRxPin, 1, false); /* input, autobaud.pio:33 */
	sm_config_set_clkdiv(&c, (float)kClkSysHz / (float)kPioClockHz); /* autobaud.c:323 */

	pio_sm_init(pio, sm, offset, &c);
	pio_sm_clear_fifos(pio, sm);
	pio_sm_set_enabled(pio, sm, true); /* autobaud.c:325 */

	/* Fresh estimator per run (autobaud.c:327-328 create_table). */
	autobaud::Estimator est(kPioClockHz);

	LOG_INF("autobaud measuring on GP%u (PIO SM %u)", kRxPin, (unsigned)sm);

	/* Drain loop. Equivalent to autobaud_thread's estimate_baud_rate poll
	 * (autobaud.c:382-384) but pulling samples directly from the PIO RX FIFO
	 * instead of a DMA ring. Stop on a confident estimate or a STOP event. */
	bool converged = false;
	while (true) {
		if (k_event_test(&g_evt, kEvtStop)) {
			break;
		}

		bool drained_any = false;
		while (!pio_sm_is_rx_fifo_empty(pio, sm)) {
			drained_any = true;
			uint32_t raw = pio_sm_get(pio, sm);          /* autobaud.c:217 */
			autobaud::Result r = est.add_sample(raw);    /* autobaud.c:219-258 */
			if (r.valid) {
				/* Apply the detected baud to uart1 (replaces the
				 * original's queue push to cdc_thread,
				 * autobaud.c:252-256). Keep all other UART params. */
				struct uart_config cfg;
				if (uart_config_get(uart_dev, &cfg) == 0) {
					cfg.baudrate = r.baud;
					int cret = uart_configure(uart_dev, &cfg);
					if (cret < 0) {
						LOG_ERR("uart_configure(%u) failed: %d",
							r.baud, cret);
					} else {
						LOG_INF("autobaud detected %u baud "
							"(validity %d/100)",
							r.baud,
							(int)(r.validity * 100.0f));
					}
				}
				converged = true;
				break;
			}
		}

		if (converged) {
			break;
		}
		if (!drained_any) {
			/* FIFO empty: yield (autobaud.c:383-384 xTaskDelayUntil 1). */
			k_sleep(K_MSEC(1));
		}
	}

	/* Teardown -- ALWAYS runs (converged OR stop). Disable + unclaim the SM,
	 * remove the program, restore the UART pin. autobaud.c:283-289. */
	pio_sm_set_enabled(pio, sm, false);
	pio_sm_unclaim(pio, sm);
	pio_remove_program(pio, RPI_PICO_PIO_GET_PROGRAM(autobaud), offset);
	restore_uart_pin();

	LOG_INF("autobaud stopped (%s)", converged ? "converged" : "aborted");
}

/* The autobaud thread. Mirrors autobaud_thread (autobaud.c:357-387): idle until
 * START, run a measurement, then return to idle. */
void thread_fn(void *, void *, void *)
{
	while (true) {
		/* Idle: block until a START arrives (autobaud.c:363-365). Clear
		 * any stale STOP/STOPPED so they do not affect the upcoming run. */
		k_event_wait(&g_evt, kEvtStart, false, K_FOREVER);
		k_event_clear(&g_evt, kEvtStart | kEvtStop | kEvtStopped);

		atomic_set(&g_running, 1);
		/* Stop uart1 RX so the UART peripheral does not fight PIO for the
		 * pad while PIO owns GP5; restored (re-enabled) in teardown. */
		uart_irq_rx_disable(uart_dev);

		run_once();

		/* Mark not-running, THEN signal stopped: a caller waiting in
		 * autobaud_stop() is released only after run_once()'s teardown has
		 * restored the UART pin, so the bridge's follow-up uart_configure
		 * never races a PIO-owned pad (autobaud_wait_stop, autobaud.c:351). */
		atomic_set(&g_running, 0);
		k_event_post(&g_evt, kEvtStopped);
	}
}

K_THREAD_STACK_DEFINE(g_stack, 2048);
struct k_thread g_thread;
k_tid_t g_tid;

} // namespace

void autobaud_start()
{
	/* Idempotent: a no-op if a run is already in progress (autobaud.c's
	 * autobaud_running guard, autobaud.c:363). The thread is spawned lazily
	 * on the first start. */
	if (atomic_get(&g_running)) {
		return;
	}
	if (g_tid == nullptr) {
		g_tid = k_thread_create(&g_thread, g_stack,
					K_THREAD_STACK_SIZEOF(g_stack),
					thread_fn, nullptr, nullptr, nullptr,
					K_PRIO_PREEMPT(7), 0, K_NO_WAIT);
		k_thread_name_set(g_tid, "autobaud");
	}
	k_event_post(&g_evt, kEvtStart);
}

void autobaud_stop()
{
	/* Signal the thread to tear down and BLOCK until it has (the thread posts
	 * STOPPED after restoring the UART pin). Synchronous so the bridge's
	 * follow-up uart_configure runs only once GP5 is back on UART1 -- the #1
	 * correctness requirement. No-op if nothing is running. Mirrors
	 * autobaud_wait_stop (autobaud.c:351-354). */
	if (!atomic_get(&g_running)) {
		return;
	}
	k_event_clear(&g_evt, kEvtStopped);
	k_event_post(&g_evt, kEvtStop);
	/* Bounded wait so a wedged thread can never deadlock the caller; the
	 * teardown is a handful of register writes and a pinctrl apply. */
	k_event_wait(&g_evt, kEvtStopped, false, K_MSEC(500));
}

} // namespace probe
