#pragma once
/* MSP430 JTAG primitives. Cpuxv2 is written against this interface; SbwTap
 * implements it over an SbwPort, and host tests script it directly.
 */
#include <cstdint>

namespace msp430 {

enum class Entry : uint8_t { RstHigh, RstLow };

class Tap {
public:
	virtual ~Tap() = default;

	virtual void set_speed(uint32_t slot_hz) = 0;
	/* Release the pins, let the target's test logic reset, take the pins.
	 * Returns false if the pins are not available. */
	virtual bool open() = 0;
	/* Requires a successful open(). SBW entry sequence followed by a TAP reset. Leaves the TAP in
	 * Run-Test/Idle with TCLK high. */
	virtual void enter(Entry entry) = 0;
	/* Release the pins. */
	virtual void close() = 0;

	/* Shifts and TCLK calls are valid only after enter(). All shifts start
	 * and end in Run-Test/Idle. */
	virtual uint8_t ir_shift(uint8_t ir) = 0; /* returns the captured IR, assembled MSB first
	                                           (CPUXv2 returns its JTAG ID, 0x91) */
	virtual uint16_t dr_shift16(uint16_t value) = 0;
	virtual uint32_t dr_shift20(uint32_t value) = 0; /* returns the de-scrambled 20-bit value */
	virtual void set_tclk() = 0;
	virtual void clr_tclk() = 0;

	virtual void delay_ms(uint32_t ms) = 0;
	virtual uint32_t millis() = 0;
};

} // namespace msp430
