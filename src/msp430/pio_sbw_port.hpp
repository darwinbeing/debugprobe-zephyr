#pragma once
/* msp430::SbwPort over the PIO driver (drivers/sbw_pio), with pin ownership
 * arbitration against SWD. */
#include "msp430/sbw_port.hpp"

struct device;

namespace probe {

class PioSbwPort final : public msp430::SbwPort {
public:
	explicit PioSbwPort(const struct device *dev) : dev_(dev) {}

	bool acquire() override;
	void release() override;
	void set_clock(uint32_t slot_hz) override;
	void set_tck(bool level) override;
	void set_tdio(bool level) override;
	void tck_pulse() override;
	uint32_t cycles(uint32_t tms, uint32_t tdi, unsigned count) override;
	void tclk_cycle(bool was_high, bool level) override;
	void delay_us(uint32_t us) override;
	uint32_t millis() override;

private:
	uint32_t clock_cycles(uint32_t tms, uint32_t mid, uint32_t tdi, unsigned count);

	const struct device *dev_;
};

} // namespace probe
