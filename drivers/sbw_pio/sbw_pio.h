/*
 * PIO Spy-Bi-Wire transport (compatible "debugprobe,sbw-pio").
 * Clocks JTAG cycles onto SBWTCK/SBWTDIO; knows nothing about TAP states.
 * Not thread-safe: all calls come from the DAP request thread.
 */
#ifndef SBW_PIO_H_
#define SBW_PIO_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct device;

/* Route the pins to this driver, drive SBWTCK low and SBWTDIO high. */
int sbw_pio_acquire(const struct device *dev);
/* Release the pins: stop the machine, both pins high-impedance, then apply the
 * pinctrl state again. Released means: routed to this PIO, high-impedance,
 * idle pulls active. Must not be called while the SWD driver is using the
 * pins. */
void sbw_pio_release(const struct device *dev);
/* SBWTCK frequency while clocking cycles (one slot = one period). Clamped to
 * 100 kHz..1 MHz; 0 selects the default of 250 kHz. */
void sbw_pio_set_clock(const struct device *dev, uint32_t slot_hz);

/* Static pin levels and one short (about 1 us) low pulse on SBWTCK. These stop
 * the state machine; the next sbw_pio_cycles() restarts it. They require
 * acquired pins. */
void sbw_pio_set_tck(const struct device *dev, bool level);
void sbw_pio_set_tdio(const struct device *dev, bool level);
void sbw_pio_tck_pulse(const struct device *dev);

/* Clock `count` (1..32) JTAG cycles. Bit i of tms/mid/tdi belongs to cycle i.
 * `mid` is the level on SBWTDIO during the low phase of the TMS slot: equal to
 * `tms`, except for TI's TMSLDH cycle (tms = 0, mid = 1), which keeps TCLK high.
 * On success returns 0 and stores the TDO samples in *tdo (bit i from cycle i);
 * SBWTCK is then high and SBWTDIO driven at the last TDI level.
 * `tdo` must not be NULL.
 * Preconditions: the pins were acquired and SBWTCK is high (the entry
 * sequence ends that way). Errors (*tdo is set to 0):
 *   -EINVAL    count out of range
 *   -EIO       the pins are not in the required state; nothing was clocked
 *   -ETIMEDOUT the state machine did not answer; the pins were released */
int sbw_pio_cycles(const struct device *dev, uint32_t tms, uint32_t mid,
		   uint32_t tdi, unsigned int count, uint32_t *tdo);

#ifdef __cplusplus
}
#endif

#endif /* SBW_PIO_H_ */
