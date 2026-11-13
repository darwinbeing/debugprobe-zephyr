#pragma once
/* Hardware abstraction for the two SBW wires. The PIO driver implements it on
 * target; host tests use a recording mock.
 */
#include <cstdint>

namespace msp430 {

class SbwPort {
public:
	virtual ~SbwPort() = default;

	/* Take both pins and drive SBWTCK low, SBWTDIO high. Returns false if
	 * the pins are owned by SWD. */
	virtual bool acquire() = 0;
	/* Stop driving the pins this port holds (high-impedance). Safe to call
	 * repeatedly, before acquire(), and while SWD owns the pins. */
	virtual void release() = 0;
	/* SBWTCK frequency while clocking cycles. */
	virtual void set_clock(uint32_t slot_hz) = 0;

	/* Static levels and a single short low pulse on SBWTCK; used only by
	 * the entry sequence. tck_pulse() starts from SBWTCK high and makes one
	 * low pulse of about 1 us (well under the 7 us limit); not interruptible. */
	virtual void set_tck(bool level) = 0;
	virtual void set_tdio(bool level) = 0;
	virtual void tck_pulse() = 0;

	/* Clock `count` (1..32) JTAG cycles. Bit i of tms/tdi belongs to cycle
	 * i. Every TMS slot is plain (never the TMSLDH variant). Afterwards
	 * SBWTCK is high and SBWTDIO is still driven at the last TDI level.
	 * Returns the sampled TDO bits, bit i from cycle i. Consecutive calls
	 * continue the same cycle stream; nothing is clocked between them, so
	 * one scan may span several calls. */
	virtual uint32_t cycles(uint32_t tms, uint32_t tdi, unsigned count) = 0;
	/* One Run-Test/Idle cycle (TMS = 0) that sets TCLK to `level`.
	 * `was_high` selects the TMSLDH variant of the TMS slot. */
	virtual void tclk_cycle(bool was_high, bool level) = 0;

	/* Waits at least `us` microseconds; may sleep. */
	virtual void delay_us(uint32_t us) = 0;
	virtual uint32_t millis() = 0;
};

} // namespace msp430
