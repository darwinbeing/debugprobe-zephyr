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
/* Both pins high-impedance. */
void sbw_pio_release(const struct device *dev);
/* SBWTCK frequency while clocking cycles (one slot = one period). */
void sbw_pio_set_clock(const struct device *dev, uint32_t slot_hz);

/* Static pin levels and one short (about 1 us) low pulse on SBWTCK. These stop
 * the state machine; the next sbw_pio_cycles() restarts it. */
void sbw_pio_set_tck(const struct device *dev, bool level);
void sbw_pio_set_tdio(const struct device *dev, bool level);
void sbw_pio_tck_pulse(const struct device *dev);

/* Clock `count` (1..32) JTAG cycles. Bit i of tms/mid/tdi belongs to cycle i
 * (see sbw_pack.h for `mid`). Returns the TDO samples, bit i from cycle i. */
uint32_t sbw_pio_cycles(const struct device *dev, uint32_t tms, uint32_t mid,
			uint32_t tdi, unsigned int count);

#ifdef __cplusplus
}
#endif

#endif /* SBW_PIO_H_ */
