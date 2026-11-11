/*
 * PIO Spy-Bi-Wire transport for RP2040/RP2350.
 *
 * The state machine (sbw.pio) is started by the first sbw_pio_cycles() call and
 * then stays enabled, idle with the clock high, until a static-level call or a
 * release stops it. The entry sequence needs static pin levels; those are
 * produced by executing single instructions on the stopped machine, the same mechanism the pico-sdk
 * uses in pio_sm_set_consecutive_pindirs().
 *
 * SPDX-License-Identifier: MIT
 */
#define DT_DRV_COMPAT debugprobe_sbw_pio

#include <zephyr/device.h>
#include <zephyr/drivers/misc/pio_rpi_pico/pio_rpi_pico.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <hardware/pio.h>

#include "sbw_pack.h"
#include "sbw_pio.h"
#include "sbw_pio_program.h"

LOG_MODULE_REGISTER(sbw_pio, LOG_LEVEL_INF);

/* clk_sys from devicetree, as in drivers/dp_pio/swdp_pio.c. */
#define CLK_SYS_HZ DT_PROP(DT_NODELABEL(clk_sys), clock_frequency)

/* sbw.pio spends 8 state-machine ticks per slot. */
#define SBW_TICKS_PER_SLOT 8U
#define SBW_DEFAULT_SLOT_HZ 250000U
#define SBW_MIN_SLOT_HZ 100000U
#define SBW_MAX_SLOT_HZ 1000000U

/* A full 32-cycle call takes at most about 1.2 ms. */
#define SBW_RX_TIMEOUT_MS 10U

#define SBW_PULSE_LOOPS 8U /* about 0.5..1 us of low time at 125..150 MHz */

struct sbw_pio_config {
	const struct device *piodev;
	const struct pinctrl_dev_config *pcfg;
	uint8_t tck_pin;
	uint8_t tdio_pin;
	uint8_t tdi_in_pin; /* == tdio_pin unless the board has a separate input */
};

struct sbw_pio_data {
	size_t sm;
	uint offset;
	bool running;
};

static inline PIO sbw_pio(const struct device *dev)
{
	const struct sbw_pio_config *cfg = dev->config;

	return pio_rpi_pico_get_pio(cfg->piodev);
}

static void sbw_stop(const struct device *dev)
{
	struct sbw_pio_data *d = dev->data;

	if (d->running) {
		/* The machine is idle with the clock high. */
		pio_sm_set_enabled(sbw_pio(dev), d->sm, false);
		d->running = false;
	}
}

static int sbw_start(const struct device *dev)
{
	const struct sbw_pio_config *cfg = dev->config;
	struct sbw_pio_data *d = dev->data;
	PIO pio = sbw_pio(dev);

	if (!d->running) {
		const uint32_t oe_need = BIT(cfg->tck_pin) | BIT(cfg->tdio_pin);

		/* Check the PIO's actual outputs. Do not repair: raising SBWTCK
		 * now would be a rising edge that puts the target out of phase. */
		if ((pio->dbg_padoe & oe_need) != oe_need ||
		    (pio->dbg_padout & BIT(cfg->tck_pin)) == 0U) {
			LOG_ERR("pins not ready (oe 0x%08x out 0x%08x)",
				(unsigned int)pio->dbg_padoe,
				(unsigned int)pio->dbg_padout);
			return -EIO;
		}
		pio_sm_clear_fifos(pio, d->sm);
		pio_sm_restart(pio, d->sm);
		pio_sm_exec(pio, d->sm, pio_encode_jmp(d->offset + sbw_offset_entry));
		pio_sm_set_enabled(pio, d->sm, true);
		d->running = true;
	}

	return 0;
}

static void sbw_exec_tck(const struct device *dev, bool level)
{
	struct sbw_pio_data *d = dev->data;

	pio_sm_exec(sbw_pio(dev), d->sm,
		    pio_encode_nop() | pio_encode_sideset_opt(1, level ? 1U : 0U));
}

int sbw_pio_acquire(const struct device *dev)
{
	const struct sbw_pio_config *cfg = dev->config;
	struct sbw_pio_data *d = dev->data;
	PIO pio = sbw_pio(dev);
	int ret;

	sbw_stop(dev);

	/* The SWD driver may have routed the pins to PIO0 since the last
	 * session; take them back. */
	ret = pinctrl_apply_state(cfg->pcfg, PINCTRL_STATE_DEFAULT);
	if (ret < 0) {
		return ret;
	}

	/* Levels first, then directions: SBWTCK low, SBWTDIO high (TI
	 * DrvSignals). */
	sbw_exec_tck(dev, false);
	pio_sm_exec(pio, d->sm, pio_encode_set(pio_pins, 1));
	pio_sm_set_consecutive_pindirs(pio, d->sm, cfg->tck_pin, 1, true);
	pio_sm_set_consecutive_pindirs(pio, d->sm, cfg->tdio_pin, 1, true);

	return 0;
}

void sbw_pio_release(const struct device *dev)
{
	const struct sbw_pio_config *cfg = dev->config;
	struct sbw_pio_data *d = dev->data;
	PIO pio = sbw_pio(dev);

	int ret;

	sbw_stop(dev);
	pio_sm_set_consecutive_pindirs(pio, d->sm, cfg->tdio_pin, 1, false);
	pio_sm_set_consecutive_pindirs(pio, d->sm, cfg->tck_pin, 1, false);

	/* Released = routed to this PIO, high-impedance, idle pulls active. */
	ret = pinctrl_apply_state(cfg->pcfg, PINCTRL_STATE_DEFAULT);
	if (ret < 0) {
		LOG_ERR("pinctrl apply failed: %d", ret);
	}
}

void sbw_pio_set_clock(const struct device *dev, uint32_t slot_hz)
{
	struct sbw_pio_data *d = dev->data;
	uint64_t tick_hz;
	uint64_t div256;

	if (slot_hz == 0U) {
		slot_hz = SBW_DEFAULT_SLOT_HZ;
	}
	/* The low phase is half a slot. Below SBW_MIN_SLOT_HZ it would approach
	 * the 7 us after which the target's SBW logic resets, so the bound is
	 * enforced here and not left to the caller. */
	slot_hz = CLAMP(slot_hz, SBW_MIN_SLOT_HZ, SBW_MAX_SLOT_HZ);
	tick_hz = (uint64_t)slot_hz * SBW_TICKS_PER_SLOT;
	/* Divider in 1/256 steps, rounded up so the clock is never faster
	 * than requested. */
	div256 = ((uint64_t)CLK_SYS_HZ * 256U + tick_hz - 1U) / tick_hz;
	div256 = CLAMP(div256, 256U, (65535U * 256U) + 255U);

	pio_sm_set_clkdiv_int_frac(sbw_pio(dev), d->sm, (uint16_t)(div256 >> 8),
				   (uint8_t)(div256 & 0xFFU));
}

void sbw_pio_set_tck(const struct device *dev, bool level)
{
	sbw_stop(dev);
	sbw_exec_tck(dev, level);
}

void sbw_pio_set_tdio(const struct device *dev, bool level)
{
	struct sbw_pio_data *d = dev->data;

	sbw_stop(dev);
	pio_sm_exec(sbw_pio(dev), d->sm, pio_encode_set(pio_pins, level ? 1U : 0U));
}

void __ramfunc sbw_pio_tck_pulse(const struct device *dev)
{
	struct sbw_pio_data *d = dev->data;
	PIO pio = sbw_pio(dev);
	const uint32_t low = pio_encode_nop() | pio_encode_sideset_opt(1, 0);
	const uint32_t high = pio_encode_nop() | pio_encode_sideset_opt(1, 1);
	unsigned int key;

	sbw_stop(dev);
	/* TI: _DINT(); ClrTST(); usDelay(1); SetTST(); _EINT(). The low time
	 * must stay far below 7 us, so the pulse runs from RAM with interrupts
	 * locked and no calls between the two register writes. */
	key = irq_lock();
	pio->sm[d->sm].instr = low;
	for (volatile uint32_t i = 0; i < SBW_PULSE_LOOPS; i++) {
	}
	pio->sm[d->sm].instr = high;
	irq_unlock(key);
}

int sbw_pio_cycles(const struct device *dev, uint32_t tms, uint32_t mid,
		   uint32_t tdi, unsigned int count, uint32_t *tdo)
{
	struct sbw_pio_data *d = dev->data;
	PIO pio = sbw_pio(dev);
	unsigned int len[SBW_MAX_CYCLES / SBW_CYCLES_PER_WORD + 1U];
	unsigned int words = 0;
	unsigned int first = 0;
	uint32_t acc = 0;
	int ret;

	*tdo = 0;
	if (count == 0U || count > SBW_MAX_CYCLES) {
		return -EINVAL;
	}

	ret = sbw_start(dev);
	if (ret < 0) {
		return ret;
	}

	/* At most 4 words: they fit the TX FIFO, their results the RX FIFO. */
	while (first < count) {
		const unsigned int n = MIN(count - first, SBW_CYCLES_PER_WORD);

		pio_sm_put_blocking(pio, d->sm, sbw_pack_word(tms, mid, tdi, first, n));
		len[words++] = n;
		first += n;
	}

	first = 0;
	for (unsigned int i = 0; i < words; i++) {
		const uint32_t t0 = k_uptime_get_32();
		uint32_t v;

		while (pio_sm_is_rx_fifo_empty(pio, d->sm)) {
			if ((k_uptime_get_32() - t0) > SBW_RX_TIMEOUT_MS) {
				LOG_ERR("RX timeout, releasing pins");
				sbw_pio_release(dev);
				return -ETIMEDOUT;
			}
		}
		/* The machine shifts samples in from the MSB side. */
		v = pio_sm_get(pio, d->sm) >> (32U - len[i]);

		acc |= v << first;
		first += len[i];
	}

	*tdo = acc;
	return 0;
}

static int sbw_pio_init(const struct device *dev)
{
	const struct sbw_pio_config *cfg = dev->config;
	struct sbw_pio_data *d = dev->data;
	pio_sm_config c;
	PIO pio;
	int ret;

	if (!device_is_ready(cfg->piodev)) {
		return -ENODEV;
	}
	pio = pio_rpi_pico_get_pio(cfg->piodev);

	ret = pio_rpi_pico_allocate_sm(cfg->piodev, &d->sm);
	if (ret < 0) {
		return ret;
	}
	if (!pio_can_add_program(pio, RPI_PICO_PIO_GET_PROGRAM(sbw))) {
		return -ENOMEM;
	}
	d->offset = pio_add_program(pio, RPI_PICO_PIO_GET_PROGRAM(sbw));

	c = pio_get_default_sm_config();
	sm_config_set_wrap(&c, d->offset + RPI_PICO_PIO_GET_WRAP_TARGET(sbw),
			   d->offset + RPI_PICO_PIO_GET_WRAP(sbw));
	/* `.side_set 1 opt`: one data bit plus the enable bit. */
	sm_config_set_sideset(&c, 2, true, false);
	sm_config_set_sideset_pins(&c, cfg->tck_pin);
	sm_config_set_out_pins(&c, cfg->tdio_pin, 1);
	sm_config_set_set_pins(&c, cfg->tdio_pin, 1);
	sm_config_set_in_pins(&c, cfg->tdi_in_pin);
	sm_config_set_out_shift(&c, true, false, 32);
	sm_config_set_in_shift(&c, true, false, 32);
	pio_sm_init(pio, d->sm, d->offset + sbw_offset_entry, &c);

	sbw_pio_set_clock(dev, SBW_DEFAULT_SLOT_HZ);

	/* Idle state from boot: both pins inputs, pulls from pinctrl (pull-up
	 * on SBWTDIO so the probe does not fight the target's RST pull-up,
	 * pull-down on SBWTCK). */
	pio_sm_set_consecutive_pindirs(pio, d->sm, cfg->tdio_pin, 1, false);
	pio_sm_set_consecutive_pindirs(pio, d->sm, cfg->tck_pin, 1, false);

	return pinctrl_apply_state(cfg->pcfg, PINCTRL_STATE_DEFAULT);
}

#define SBW_PIO_DEFINE(n)							\
	PINCTRL_DT_INST_DEFINE(n);						\
	static const struct sbw_pio_config sbw_pio_cfg_##n = {			\
		.piodev = DEVICE_DT_GET(DT_INST_PHANDLE(n, piodev)),		\
		.pcfg = PINCTRL_DT_INST_DEV_CONFIG_GET(n),			\
		.tck_pin = DT_INST_PROP(n, tck_pin),				\
		.tdio_pin = DT_INST_PROP(n, tdio_pin),				\
		.tdi_in_pin = DT_INST_PROP_OR(n, tdi_in_pin,			\
					      DT_INST_PROP(n, tdio_pin)),	\
	};									\
	static struct sbw_pio_data sbw_pio_data_##n;				\
	DEVICE_DT_INST_DEFINE(n, sbw_pio_init, NULL,				\
			      &sbw_pio_data_##n, &sbw_pio_cfg_##n,		\
			      POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE,	\
			      NULL);

DT_INST_FOREACH_STATUS_OKAY(SBW_PIO_DEFINE)
