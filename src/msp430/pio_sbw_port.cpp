#include "msp430/pio_sbw_port.hpp"

#include <zephyr/device.h>
#include <zephyr/kernel.h>

#include "pin_owner.hpp"
#include "sbw_pio.h"

namespace probe {

bool PioSbwPort::acquire()
{
	fault_ = false; /* a new session starts clean */
	if (!device_is_ready(dev_) || !pin_owner::sbw_claim()) {
		return false;
	}
	if (sbw_pio_acquire(dev_) < 0) {
		pin_owner::sbw_release();
		return false;
	}
	return true;
}

void PioSbwPort::release()
{
	/* SbwTap::open() releases before it acquires. Only touch the pins if
	 * this port holds them: the driver's release re-routes them to its own
	 * PIO, which must never happen while an SWD debugger is using them. */
	if (!pin_owner::sbw_owns()) {
		return;
	}
	sbw_pio_release(dev_);
	pin_owner::sbw_release();
}

void PioSbwPort::set_clock(uint32_t slot_hz)
{
	sbw_pio_set_clock(dev_, slot_hz);
}

void PioSbwPort::set_tck(bool level)
{
	sbw_pio_set_tck(dev_, level);
}

void PioSbwPort::set_tdio(bool level)
{
	sbw_pio_set_tdio(dev_, level);
}

void PioSbwPort::tck_pulse()
{
	sbw_pio_tck_pulse(dev_);
}

uint32_t PioSbwPort::cycles(uint32_t tms, uint32_t tdi, unsigned count)
{
	/* Plain cycles: the line keeps the TMS level during the TMS slot. */
	return clock_cycles(tms, tms, tdi, count);
}

void PioSbwPort::tclk_cycle(bool was_high, bool level)
{
	/* TMS = 0; raise the line in the low phase of the TMS slot when TCLK is
	 * high (TI's TMSLDH); the TDI slot carries the new TCLK level. */
	(void)clock_cycles(0U, was_high ? 1U : 0U, level ? 1U : 0U, 1U);
}

/* A driver error means the pins were not in the state the PIO program needs,
 * or the state machine did not answer; the driver has logged it once. From
 * then on this port reports all-zero TDO without touching the driver again
 * until the next acquire(). All-zero TDO makes every check in the JTAG layers
 * fail (JTAG ID, full-emulation state, mailbox ready), and memory transfers
 * end with such a check, so the command in progress ends with an error status
 * and the session is closed. */
uint32_t PioSbwPort::clock_cycles(uint32_t tms, uint32_t mid, uint32_t tdi, unsigned count)
{
	uint32_t tdo = 0;

	if (fault_) {
		return 0;
	}
	if (sbw_pio_cycles(dev_, tms, mid, tdi, count, &tdo) < 0) {
		fault_ = true;
		return 0;
	}
	return tdo;
}

void PioSbwPort::delay_us(uint32_t us)
{
	if (us >= 1000U) {
		k_sleep(K_USEC(us));
	} else {
		k_busy_wait(us);
	}
}

uint32_t PioSbwPort::millis()
{
	return k_uptime_get_32();
}

} // namespace probe
