/*
 * PIO-accelerated SWD port driver for RP2040/RP2350.
 *
 * This is a faithful, line-by-line port of the Raspberry Pi debugprobe
 * firmware to Zephyr's swdp_api:
 *   - PIO FIFO primitives + init/divider  <- ~/Developer/debugprobe/src/probe.c
 *   - SWD protocol (transfer/sequence)    <- ~/Developer/debugprobe/src/sw_dp_pio.c
 *   - PIO state-machine config            <- ~/Developer/debugprobe/src/probe.pio
 * Reference line ranges are cited per function. The protocol/timing logic is
 * kept byte-identical; only the Zephyr device-model / pin-access plumbing is
 * adapted (and commented where it differs from the originals).
 *
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2021-2023 Raspberry Pi (Trading) Ltd. (original firmware)
 * Copyright (c) 2013-2022 ARM Limited. (SWD protocol, via sw_dp_pio.c)
 */
#define DT_DRV_COMPAT raspberrypi_swdp_pio

#include <zephyr/device.h>
#include <zephyr/drivers/swdp.h>
#include <zephyr/drivers/misc/pio_rpi_pico/pio_rpi_pico.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <hardware/pio.h>

#include "probe_pio.h"

/* Out-of-tree driver: use a literal log level (not CONFIG_DP_DRIVER_LOG_LEVEL,
 * which belongs to the in-tree DP_DRIVER module).
 */
LOG_MODULE_REGISTER(swdp_pio, LOG_LEVEL_INF);

/* clk_sys, the PIO's source clock, mirrors probe.c:62
 *   uint clk_sys_freq_khz = clock_get_hz(clk_sys) / 1000;
 * The pico-sdk read is replaced by the configured clk_sys frequency from
 * devicetree (125 MHz on RP2040, 150 MHz on RP2350) -- a compile-time constant
 * equal to clock_get_hz(clk_sys), with no runtime clock_control dependency.
 */
#define CLK_SYS_HZ DT_PROP(DT_NODELABEL(clk_sys), clock_frequency)

struct swdp_pio_config {
	const struct device *piodev;
	const struct pinctrl_dev_config *pcfg; /* SWCLK/SWDIO/SWDI funcsel via pinctrl */
	struct gpio_dt_spec reset_gpio; /* may be {0} if absent */
	uint8_t swclk_pin;
	uint8_t swdio_pin;
	uint8_t swdi_pin;  /* separate SWD input pin; == swdio_pin when shared */
	uint8_t oen_pin;   /* level-shifter output-enable pin; valid iff use_oen */
	/* Two independent axes (the real Debug Probe is PROBE_IO_SWDI, not OEN):
	 *  - use_swdi: a separate SWD input pin (din-gpios) exists. The real
	 *    Debug Probe sets this (SWDI=GP13). in_pins reads swdi_pin; the SM
	 *    still uses probe_program (1-bit side-set, base = swclk). This is the
	 *    ONLY difference between the Pico (shared SWDIO) and the Debug Probe.
	 *  - use_oen: a level-shifter output-enable pin (oen-gpios) exists. This
	 *    selects probe_oen_program (2-bit side-set, base = oen, 3 pindirs).
	 *    None of our three boards set oen-gpios, so this path is latent.
	 */
	bool use_swdi;     /* separate SWD input pin (din-gpios) present */
	bool use_oen;      /* probe_oen program vs probe (oen-gpios present) */
};

struct swdp_pio_data {
	size_t sm;          /* allocated state machine index */
	uint offset;        /* program load offset in PIO instruction memory */
	bool loaded;        /* SM claimed + program added (one-time, lifetime-held) */
	uint8_t turnaround; /* swd_conf.turnaround (1-based) */
	bool data_phase;    /* swd_conf.data_phase */
	uint32_t cached_khz;
};

/* probe_pio_command_t -- probe.c:91-96. Each enum is an index that, added to
 * the program load offset, names the dispatch label the SM jumps to.
 */
typedef enum {
	CMD_WRITE = 0,
	CMD_SKIP,
	CMD_TURNAROUND,
	CMD_READ,
} probe_pio_command_t;

/* --- FIFO command helpers (probe.c:98-148) --- */

/* fmt_probe_command -- probe.c:98-105. Selects the dispatch label by command,
 * picking the probe_* or probe_oen_* offset family per cfg->use_oen, then packs
 * | 13:9 cmd_addr | 8 out_en | 7:0 (bit_count-1) | exactly as the original.
 */
static inline uint32_t fmt_probe_command(const struct swdp_pio_config *cfg,
					 const struct swdp_pio_data *d,
					 uint bit_count, bool out_en,
					 probe_pio_command_t cmd)
{
	uint cmd_addr;

	if (cfg->use_oen) {
		cmd_addr =
			cmd == CMD_WRITE      ? d->offset + probe_oen_offset_write_cmd :
			cmd == CMD_SKIP       ? d->offset + probe_oen_offset_get_next_cmd :
			cmd == CMD_TURNAROUND ? d->offset + probe_oen_offset_turnaround_cmd :
						d->offset + probe_oen_offset_read_cmd;
	} else {
		cmd_addr =
			cmd == CMD_WRITE      ? d->offset + probe_offset_write_cmd :
			cmd == CMD_SKIP       ? d->offset + probe_offset_get_next_cmd :
			cmd == CMD_TURNAROUND ? d->offset + probe_offset_turnaround_cmd :
						d->offset + probe_offset_read_cmd;
	}

	return ((bit_count - 1) & 0xff) | ((uint)out_en << 8) | (cmd_addr << 9);
}

/* probe_write_bits -- probe.c:107-114. Put write-cmd(out_en=1, n) then data.
 * Returns immediately so the next command can be queued while this one runs.
 */
static inline void probe_write_bits(const struct device *dev, uint bit_count,
				    uint32_t data_byte)
{
	const struct swdp_pio_config *cfg = dev->config;
	struct swdp_pio_data *d = dev->data;
	PIO pio = pio_rpi_pico_get_pio(cfg->piodev);

	pio_sm_put_blocking(pio, d->sm,
			    fmt_probe_command(cfg, d, bit_count, true, CMD_WRITE));
	pio_sm_put_blocking(pio, d->sm, data_byte);
}

/* probe_hiz_clocks -- probe.c:116-119. Put turnaround-cmd(out_en=0, n) then a
 * 0 data word.
 */
static inline void probe_hiz_clocks(const struct device *dev, uint bit_count)
{
	const struct swdp_pio_config *cfg = dev->config;
	struct swdp_pio_data *d = dev->data;
	PIO pio = pio_rpi_pico_get_pio(cfg->piodev);

	pio_sm_put_blocking(pio, d->sm,
			    fmt_probe_command(cfg, d, bit_count, false, CMD_TURNAROUND));
	pio_sm_put_blocking(pio, d->sm, 0);
}

/* probe_read_bits -- probe.c:121-133. Put read-cmd(out_en=0, n), block for the
 * pushed word, and shift the captured bits down to the LSBs (the SM shifts in
 * from the MSB end). Bounds: returns up to 32 bits.
 */
static inline uint32_t probe_read_bits(const struct device *dev, uint bit_count)
{
	const struct swdp_pio_config *cfg = dev->config;
	struct swdp_pio_data *d = dev->data;
	PIO pio = pio_rpi_pico_get_pio(cfg->piodev);
	uint32_t data;
	uint32_t data_shifted;

	pio_sm_put_blocking(pio, d->sm,
			    fmt_probe_command(cfg, d, bit_count, false, CMD_READ));
	data = pio_sm_get_blocking(pio, d->sm);
	data_shifted = data;
	if (bit_count < 32) {
		data_shifted = data >> (32 - bit_count);
	}

	return data_shifted;
}

/* DAP request/ack constants -- DAP.h in the original firmware. The Zephyr
 * swdp.h SWDP_* values are bit-identical (RnW=BIT(1), OK=BIT(0)=1, WAIT=BIT(1)=2,
 * FAULT=BIT(2)=4, TRANSFER_ERROR=BIT(3)=8), so the ported logic is unchanged.
 */
#define DAP_TRANSFER_RnW	SWDP_REQUEST_RnW
#define DAP_TRANSFER_OK		SWDP_ACK_OK
#define DAP_TRANSFER_WAIT	SWDP_ACK_WAIT
#define DAP_TRANSFER_FAULT	SWDP_ACK_FAULT
#define DAP_TRANSFER_ERROR	SWDP_TRANSFER_ERROR

/* --- swdp_api implementations (port sw_dp_pio.c + probe.c) --- */

/* sw_output_sequence <- SWJ_Sequence (sw_dp_pio.c:42-66).
 * Clock out `count` bits from `data`, 8 (or the remainder) bits per
 * probe_write_bits. The original's clock-delay re-sync (sw_dp_pio.c:46-49) is
 * dropped: Zephyr drives the clock via swdp_set_clock, not DAP delay cycles.
 */
static int sw_output_sequence(const struct device *dev, uint32_t count,
			      const uint8_t *data)
{
	uint32_t bits;
	uint32_t n;

	n = count;
	while (n > 0) {
		if (n > 8) {
			bits = 8;
		} else {
			bits = n;
		}
		probe_write_bits(dev, bits, *data++);
		n -= bits;
	}

	return 0;
}

/* sw_input_sequence <- SWD_Sequence input path (sw_dp_pio.c:69-102, DIN branch).
 * Capture `count` bits via probe_read_bits, 8 (or the remainder) bits at a time,
 * packing one byte per read into `data` (probe_read_bits returns LSB-aligned).
 * The non-DIN (output) branch of SWD_Sequence maps to sw_output_sequence above;
 * swdp_input_sequence is read-only, so only the input path is ported here.
 */
static int sw_input_sequence(const struct device *dev, uint32_t count,
			     uint8_t *data)
{
	uint32_t bits;
	uint32_t n;

	n = count;
	while (n > 0) {
		if (n > 8) {
			bits = 8;
		} else {
			bits = n;
		}
		*data++ = (uint8_t)probe_read_bits(dev, bits);
		n -= bits;
	}

	return 0;
}

/* sw_transfer <- SWD_Transfer (sw_dp_pio.c:110-208).
 * request: A[3:2] RnW APnDP (Zephyr passes it without start/stop/parity).
 * Ported exactly: request-packet build, ACK read (turnaround+3 >> turnaround),
 * OK read32+parity / write turnaround+32+parity, idle cycles, WAIT/FAULT
 * data-phase dummy, protocol-error backoff. The ACK byte is written to
 * *response and 0 is returned (Zephyr convention), in place of the original's
 * `return ack`.
 */
static int sw_transfer(const struct device *dev, uint8_t request, uint32_t *data,
		       uint8_t idle_cycles, uint8_t *response)
{
	struct swdp_pio_data *d = dev->data;
	uint8_t prq = 0;
	uint8_t ack;
	uint8_t bit;
	uint32_t val = 0;
	uint32_t parity = 0;
	uint32_t n;

	/* Generate the request packet -- sw_dp_pio.c:123-133. */
	prq |= (1 << 0); /* Start Bit */
	for (n = 1; n < 5; n++) {
		bit = (request >> (n - 1)) & 0x1;
		prq |= bit << n;
		parity += bit;
	}
	prq |= (parity & 0x1) << 5; /* Parity Bit */
	prq |= (0 << 6); /* Stop Bit */
	prq |= (1 << 7); /* Park bit */
	probe_write_bits(dev, 8, prq);

	/* Turnaround (ignore read bits) -- sw_dp_pio.c:135-137. */
	ack = probe_read_bits(dev, d->turnaround + 3);
	ack >>= d->turnaround;

	if (ack == DAP_TRANSFER_OK) {
		/* Data transfer phase -- sw_dp_pio.c:139-168. */
		if (request & DAP_TRANSFER_RnW) {
			/* Read RDATA[0:31] - probe_read shifts to LSBs. */
			val = probe_read_bits(dev, 32);
			bit = probe_read_bits(dev, 1);
			parity = __builtin_popcount(val);
			if ((parity ^ bit) & 1U) {
				/* Parity error */
				ack = DAP_TRANSFER_ERROR;
			}
			if (data) {
				*data = val;
			}
			/* Turnaround for line idle */
			probe_hiz_clocks(dev, d->turnaround);
		} else {
			/* Turnaround for write */
			probe_hiz_clocks(dev, d->turnaround);

			/* Write WDATA[0:31] */
			val = *data;
			probe_write_bits(dev, 32, val);
			parity = __builtin_popcount(val);
			/* Write Parity Bit */
			probe_write_bits(dev, 1, parity & 0x1);
		}

		/* Capture Timestamp -- sw_dp_pio.c:169-172. The DAP_TRANSFER_TIMESTAMP
		 * path stored DAP_Data.timestamp; Zephyr's swdp_api has no timestamp
		 * field, so this is intentionally omitted.
		 */

		/* Idle cycles - drive 0 for N clocks -- sw_dp_pio.c:174-185.
		 * Use the idle_cycles ARG (not a global), 256 at a time.
		 */
		if (idle_cycles) {
			for (n = idle_cycles; n;) {
				if (n > 256) {
					probe_write_bits(dev, 256, 0);
					n -= 256;
				} else {
					probe_write_bits(dev, n, 0);
					n -= n;
				}
			}
		}

		if (response) {
			*response = (uint8_t)ack;
		}
		return 0;
	}

	if ((ack == DAP_TRANSFER_WAIT) || (ack == DAP_TRANSFER_FAULT)) {
		/* WAIT/FAULT -- sw_dp_pio.c:189-201. */
		if (d->data_phase && ((request & DAP_TRANSFER_RnW) != 0U)) {
			/* Dummy Read RDATA[0:31] + Parity */
			probe_read_bits(dev, 33);
		}
		probe_hiz_clocks(dev, d->turnaround);
		if (d->data_phase && ((request & DAP_TRANSFER_RnW) == 0U)) {
			/* Dummy Write WDATA[0:31] + Parity */
			probe_write_bits(dev, 32, 0);
			probe_write_bits(dev, 1, 0);
		}
		if (response) {
			*response = (uint8_t)ack;
		}
		return 0;
	}

	/* Protocol error -- sw_dp_pio.c:203-207. Back off data phase. */
	n = d->turnaround + 32U + 1U;
	probe_read_bits(dev, n);
	if (response) {
		*response = (uint8_t)ack;
	}
	return 0;
}

/* sw_set_pins -- nRESET via probe_assert_reset semantics (probe.c:74-80):
 * drive the pin low to assert reset, or float (input / Hi-Z open-drain) to
 * de-assert. SWCLK/SWDIO are owned by the PIO SM and are not host-driveable
 * here, so only nRESET (pin bit 7) -- the bit hosts actually use -- is handled.
 */
static int sw_set_pins(const struct device *dev, uint8_t pins, uint8_t value)
{
	const struct swdp_pio_config *cfg = dev->config;

	if ((pins & BIT(SWDP_nRESET_PIN)) && cfg->reset_gpio.port) {
		if (value & BIT(SWDP_nRESET_PIN)) {
			/* de-assert: open-drain high -> input/Hi-Z */
			gpio_pin_configure_dt(&cfg->reset_gpio, GPIO_INPUT);
		} else {
			/* assert: drive low */
			gpio_pin_configure_dt(&cfg->reset_gpio, GPIO_OUTPUT_ACTIVE);
		}
	}

	return 0;
}

/* sw_get_pins -- nRESET level via probe_reset_level (probe.c:82-89). The
 * reset_gpio is GPIO_ACTIVE_LOW, so gpio_pin_get_dt() returns the logical
 * level; report it in the nRESET bit. SWCLK/SWDIO are PIO-owned and not read
 * back here.
 */
static int sw_get_pins(const struct device *dev, uint8_t *state)
{
	const struct swdp_pio_config *cfg = dev->config;
	uint8_t s = 0;

	if (cfg->reset_gpio.port) {
		int val = gpio_pin_get_dt(&cfg->reset_gpio);

		s |= (val > 0) ? BIT(SWDP_nRESET_PIN) : 0;
	}

	*state = s;
	return 0;
}

/* sw_set_clock <- probe_set_swclk_freq (probe.c:61-72).
 * divider = (((clk_sys_khz + freq_khz - 1) / freq_khz) + 3) / 4, clamped to
 * [1, 65535], then pio_sm_set_clkdiv_int_frac(pio, sm, divider, 0).
 * Zephyr passes the clock in Hz; convert to kHz to match the original's units.
 */
static int sw_set_clock(const struct device *dev, uint32_t clock)
{
	const struct swdp_pio_config *cfg = dev->config;
	struct swdp_pio_data *d = dev->data;
	PIO pio = pio_rpi_pico_get_pio(cfg->piodev);
	uint32_t clk_sys_freq_khz = CLK_SYS_HZ / 1000U;
	uint32_t freq_khz = clock / 1000U;
	uint32_t divider;

	if (freq_khz == 0U) {
		freq_khz = 1U;
	}

	/* Round up (otherwise fast swclks get faster) -- probe.c:64-65. */
	divider = (((clk_sys_freq_khz + freq_khz - 1) / freq_khz) + 3) / 4;
	if (divider == 0) {
		divider = 1;
	}
	if (divider > 65535) {
		divider = 65535;
	}

	pio_sm_set_clkdiv_int_frac(pio, d->sm, (uint16_t)divider, 0);
	d->cached_khz = freq_khz;

	return 0;
}

/* sw_configure -- store turnaround (1-based) + data_phase, as consumed by
 * sw_transfer (DAP_Data.swd_conf.turnaround / .data_phase in the original).
 */
static int sw_configure(const struct device *dev, uint8_t turnaround,
			bool data_phase)
{
	struct swdp_pio_data *d = dev->data;

	d->turnaround = turnaround;
	d->data_phase = data_phase;

	LOG_DBG("turnaround %d, data_phase %d", d->turnaround, d->data_phase);

	return 0;
}

/* probe_sm_init -- replicates the c-sdk probe_sm_init block of probe.pio
 * (probe.pio:102-124) OR probe_oen.pio (probe_oen.pio:81-98), selected by
 * cfg->use_oen. Both set out/set pins = SWDIO, both shift dirs right (LSB-first)
 * with autopull/autopush off. The two variants differ ONLY in:
 *   - side-set: probe.pio is `.side_set 1 opt` base = SWCLK (probe.pio:46,105);
 *     probe_oen.pio is `.side_set 2 opt` base = SWDIOEN (probe_oen.pio:14,84).
 *   - initial output pindirs: probe.pio drives SWCLK+SWDIO (count 2 from
 *     PROBE_PIN_OFFSET=SWCLK, probe.pio:118); probe_oen.pio drives
 *     SWDIOEN+SWCLK+SWDIO (count 3 from PROBE_PIN_OFFSET=SWDIOEN, probe_oen.pio:92).
 *   - in_pins: SWDI when a separate input pin exists (probe_oen always; probe
 *     only when PROBE_IO_SWDI, probe.pio:110-114 / probe_oen.pio:89), else SWDIO.
 * We do sm_config_set_sideset() explicitly because Zephyr's
 * RPI_PICO_PIO_DEFINE_PROGRAM emits no *_get_default_config helper.
 */
static void probe_sm_init(const struct device *dev, pio_sm_config *sm_config)
{
	const struct swdp_pio_config *cfg = dev->config;
	PIO pio = pio_rpi_pico_get_pio(cfg->piodev);
	struct swdp_pio_data *d = dev->data;

	if (cfg->use_oen) {
		/* probe_oen.pio:14 `.side_set 2 opt`, base = SWDIOEN (probe_oen.pio:84).
		 * sm_config_set_sideset bit_count INCLUDES the optional-enable bit
		 * (pico-sdk: "bits to steal from the delay field"), so 2 data + 1 opt
		 * = 3 -- matching pioasm's generated probe_oen_program_get_default_config.
		 */
		sm_config_set_sideset(sm_config, 3, true, false);
		sm_config_set_sideset_pins(sm_config, cfg->oen_pin);
	} else {
		/* probe.pio:46 `.side_set 1 opt`, base = SWCLK (probe.pio:105).
		 * bit_count INCLUDES the optional bit: 1 data + 1 opt = 2 -- matching
		 * pioasm's probe_program_get_default_config. (Was 1, which mis-decoded
		 * the side-set/delay field and broke SWCLK timing -> junk SWD ACK.)
		 */
		sm_config_set_sideset(sm_config, 2, true, false);
		sm_config_set_sideset_pins(sm_config, cfg->swclk_pin);
	}

	/* Set SWDIO offset (out/set) -- probe.pio:108-109 / probe_oen.pio:87-88. */
	sm_config_set_out_pins(sm_config, cfg->swdio_pin, 1);
	sm_config_set_set_pins(sm_config, cfg->swdio_pin, 1);

	/* in pins: SWDI when a separate input pin exists (OEN always reads SWDI,
	 * the Debug Probe reads SWDI via use_swdi); else the shared SWDIO. Because
	 * swdi_pin defaults to swdio_pin when neither flag is set, reading swdi_pin
	 * is correct for all three cases -- probe.pio:110-114 / probe_oen.pio:89.
	 */
	if (cfg->use_oen || cfg->use_swdi) {
		sm_config_set_in_pins(sm_config, cfg->swdi_pin);
	} else {
		sm_config_set_in_pins(sm_config, cfg->swdio_pin);
	}

	/* Initial output pindirs. The originals set N consecutive pindirs from
	 * PROBE_PIN_OFFSET (probe.pio:118 count 2 = SWCLK,SWDIO; probe_oen.pio:92
	 * count 3 = SWDIOEN,SWCLK,SWDIO). Zephyr pins are not guaranteed
	 * consecutive, so set each pindir out individually. The PIO flips SWDIO via
	 * `out pindirs,1` at runtime.
	 */
	pio_sm_set_consecutive_pindirs(pio, d->sm, cfg->swclk_pin, 1, true);
	pio_sm_set_consecutive_pindirs(pio, d->sm, cfg->swdio_pin, 1, true);
	if (cfg->use_oen) {
		pio_sm_set_consecutive_pindirs(pio, d->sm, cfg->oen_pin, 1, true);
	}

	/* shift output right, autopull off -- probe.pio:121 / probe_oen.pio:95. */
	sm_config_set_out_shift(sm_config, true, false, 0);
	/* shift input right (swd is lsb first), autopush off -- probe.pio:123 / probe_oen.pio:97. */
	sm_config_set_in_shift(sm_config, true, false, 0);
}

/* sw_port_on <- probe_init (probe.c:151-169) + probe_gpio_init (probe.pio:74-87).
 *
 * Idempotency contract (mirrors probe_init's `if (!probe.initted)` guard,
 * probe.c:151-168): the SM claim + pio_add_program are one-time, done only on
 * the first port_on, and the SM stays claimed + the program stays loaded for the
 * device's lifetime -- exactly as the original holds a FIXED PROBE_SM and adds
 * probe_program once (probe.c:154-167). Every port_on then re-runs the
 * configure/init/enable cycle (pin funcsel, probe_sm_init, pio_sm_init, default
 * clock, jump to get_next_cmd, enable). This makes connect/disconnect
 * (DAP_Connect->port_on / DAP_Disconnect->port_off) a balanced enable/disable
 * pair: previously claim+add ran on EVERY connect while port_off never
 * unclaimed/removed, leaking one SM + ~11-15 progmem words per connect until the
 * PIO's 4 SMs / 32-word instruction memory were exhausted (-EBUSY).
 */
static int sw_port_on(const struct device *dev)
{
	const struct swdp_pio_config *cfg = dev->config;
	struct swdp_pio_data *d = dev->data;
	PIO pio = pio_rpi_pico_get_pio(cfg->piodev);
	pio_sm_config sm_config;
	uint get_next_cmd_off;
	int ret;

	/* One-time: claim the SM + funcsel pins + add the program. Held for the
	 * device's lifetime (like the original's fixed PROBE_SM, probe.c:151-167);
	 * port_off only disables, never unclaims/removes, so this guard keeps
	 * connect/disconnect leak-free and idempotent.
	 */
	if (!d->loaded) {
		/* Claim a state machine (Zephyr-managed; original hard-codes
		 * PROBE_SM). Propagate the allocator error (-EBUSY etc.).
		 */
		ret = pio_rpi_pico_allocate_sm(cfg->piodev, &d->sm);
		if (ret < 0) {
			return ret;
		}

		/* Route SWCLK/SWDIO (and the separate SWDI / OEN pins when present)
		 * to the PIO via Zephyr pinctrl -- the Zephyr way (as in
		 * uart_rpi_pico_pio), instead of the pico-sdk pio_gpio_init. The
		 * board overlay's pinctrl-0 state sets each pad's PIO funcsel plus
		 * input-enable/pull (input-enable on the read pin is required for
		 * `in pins` to sample SWDIO). Equivalent of probe_gpio_init
		 * (probe.pio:74-87).
		 */
		ret = pinctrl_apply_state(cfg->pcfg, PINCTRL_STATE_DEFAULT);
		if (ret < 0) {
			return ret;
		}

		/* Add the matching program once -- probe.c:154. */
		if (cfg->use_oen) {
			d->offset = pio_add_program(pio, &probe_oen_program);
		} else {
			d->offset = pio_add_program(pio, &probe_program);
		}

		d->loaded = true;
	}

	/* probe_gpio_init nRESET (probe.pio:74-87): nRESET pull/dir is handled by
	 * GPIO_DT_SPEC config (GPIO_ACTIVE_LOW + pull from DT); here we drive it
	 * de-asserted (input/Hi-Z) to match probe_assert_reset(1).
	 */
	if (cfg->reset_gpio.port) {
		gpio_pin_configure_dt(&cfg->reset_gpio, GPIO_INPUT);
	}

	/* Configure the SM from the already-loaded program (every port_on). */
	if (cfg->use_oen) {
		sm_config = pio_get_default_sm_config();
		sm_config_set_wrap(&sm_config, d->offset + probe_oen_wrap_target,
				   d->offset + probe_oen_wrap);
		get_next_cmd_off = probe_oen_offset_get_next_cmd;
	} else {
		sm_config = pio_get_default_sm_config();
		sm_config_set_wrap(&sm_config, d->offset + probe_wrap_target,
				   d->offset + probe_wrap);
		get_next_cmd_off = probe_offset_get_next_cmd;
	}

	probe_sm_init(dev, &sm_config);
	pio_sm_init(pio, d->sm, d->offset, &sm_config);

	/* Set up divisor: default 1 MHz -- probe.c:162. */
	sw_set_clock(dev, 1000000U);

	/* Jump SM to command dispatch routine, and enable it -- probe.c:164-166. */
	pio_sm_exec(pio, d->sm, d->offset + get_next_cmd_off);
	pio_sm_set_enabled(pio, d->sm, true);

	return 0;
}

/* sw_port_off <- probe_deinit (probe.c:171-182). Disable the SM and assert
 * (de-assert) nRESET. probe_deinit calls probe_assert_reset(1), which floats
 * the line (de-assert); we replicate that and disable the SM.
 *
 * Unlike probe_deinit (which pio_remove_program()s, probe.c:176), we keep the
 * SM claimed and the program loaded (d->loaded stays true) for the device's
 * lifetime -- matching the original holding a fixed PROBE_SM -- so the next
 * port_on re-enables without re-claiming/re-adding. This is the balanced
 * disable half of the connect/disconnect enable/disable pair: leaving the SM
 * claimed is intentional and avoids the per-connect SM/progmem leak.
 */
static int sw_port_off(const struct device *dev)
{
	const struct swdp_pio_config *cfg = dev->config;
	struct swdp_pio_data *d = dev->data;
	PIO pio = pio_rpi_pico_get_pio(cfg->piodev);

	pio_sm_set_enabled(pio, d->sm, false);

	/* de-assert nRESET -- probe.c:178 probe_assert_reset(1) -> float/Hi-Z. */
	if (cfg->reset_gpio.port) {
		gpio_pin_configure_dt(&cfg->reset_gpio, GPIO_INPUT);
	}

	return 0;
}

static struct swdp_api swdp_pio_api = {
	.swdp_output_sequence = sw_output_sequence,
	.swdp_input_sequence = sw_input_sequence,
	.swdp_transfer = sw_transfer,
	.swdp_set_pins = sw_set_pins,
	.swdp_get_pins = sw_get_pins,
	.swdp_set_clock = sw_set_clock,
	.swdp_configure = sw_configure,
	.swdp_port_on = sw_port_on,
	.swdp_port_off = sw_port_off,
};

static int swdp_pio_init(const struct device *dev)
{
	const struct swdp_pio_config *cfg = dev->config;

	if (!device_is_ready(cfg->piodev)) {
		return -ENODEV;
	}

	/* nRESET starts de-asserted (input/Hi-Z open-drain); the SM is claimed
	 * and the program loaded lazily in sw_port_on.
	 */
	if (cfg->reset_gpio.port) {
		gpio_pin_configure_dt(&cfg->reset_gpio, GPIO_INPUT);
	}

	return 0;
}

/* swclk-pin / swdio-pin are the PIO side-set/out/in base GPIO numbers. swdi-pin
 * comes from din-gpios (a separate SWD input: the real Debug Probe's SWDI), and
 * defaults to swdio-pin (shared bus) when absent. oen-pin comes from oen-gpios
 * (a level-shifter output-enable; latent example-board path, no shipped board).
 * use_swdi and use_oen are two independent axes -- see swdp_pio_config.
 */
#define SWDP_PIO_DEFINE(n)							\
	PINCTRL_DT_INST_DEFINE(n);					\
	static const struct swdp_pio_config swdp_pio_cfg_##n = {		\
		.piodev = DEVICE_DT_GET(DT_INST_PHANDLE(n, piodev)),	\
		.pcfg = PINCTRL_DT_INST_DEV_CONFIG_GET(n),		\
		.reset_gpio = GPIO_DT_SPEC_INST_GET_OR(n, reset_gpios, {0}), \
		.swclk_pin = DT_INST_PROP(n, swclk_pin),		\
		.swdio_pin = DT_INST_PROP(n, swdio_pin),		\
		.swdi_pin = COND_CODE_1(DT_INST_NODE_HAS_PROP(n, din_gpios), \
			(DT_INST_GPIO_PIN(n, din_gpios)),		\
			(DT_INST_PROP(n, swdio_pin))),			\
		.oen_pin = COND_CODE_1(DT_INST_NODE_HAS_PROP(n, oen_gpios), \
			(DT_INST_GPIO_PIN(n, oen_gpios)), (0)),		\
		.use_swdi = DT_INST_NODE_HAS_PROP(n, din_gpios),	\
		.use_oen = DT_INST_NODE_HAS_PROP(n, oen_gpios),		\
	};								\
	static struct swdp_pio_data swdp_pio_data_##n;			\
	DEVICE_DT_INST_DEFINE(n, swdp_pio_init, NULL,			\
			      &swdp_pio_data_##n, &swdp_pio_cfg_##n,	\
			      POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE, \
			      &swdp_pio_api);

DT_INST_FOREACH_STATUS_OKAY(SWDP_PIO_DEFINE)
