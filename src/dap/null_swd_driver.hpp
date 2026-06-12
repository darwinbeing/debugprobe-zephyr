#pragma once
/* Placeholder SwdDriver until the PIO driver lands (phase 3).
 * All operations are no-ops; transfers answer NoAck so hosts see a
 * well-formed "target not responding" instead of garbage.
 */
#include "dap/interfaces.hpp"

namespace probe {

class NullSwdDriver final : public dap::SwdDriver {
public:
	void set_clock(uint32_t) override {}
	void connect() override {}
	void disconnect() override {}
	void configure(uint8_t, bool) override {}
	void swj_sequence(std::span<const uint8_t>, size_t) override {}
	void swd_output(std::span<const uint8_t>, size_t) override {}
	void swd_input(std::span<uint8_t> data, size_t) override
	{
		for (auto &b : data) {
			b = 0;
		}
	}
	dap::Ack transfer(uint8_t, uint32_t *) override
	{
		return dap::Ack::NoAck;
	}
	void set_pins(uint8_t, uint8_t, uint32_t) override {}
	uint8_t read_pins() override { return 0; }
	void delay_us(uint32_t us) override;
	uint32_t timestamp() override;
};

} // namespace probe
