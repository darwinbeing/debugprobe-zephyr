#pragma once
/* Adapts a Zephyr swdp_api device (bitbang or PIO) to dap::SwdDriver,
 * so the phase-2 DapEngine drives real hardware. Backend is chosen by
 * devicetree (the node carrying the `swd_dev` nodelabel).
 */
#include <zephyr/device.h>

#include "dap/interfaces.hpp"

namespace probe {

class ZephyrSwdpDriver final : public dap::SwdDriver {
public:
	explicit ZephyrSwdpDriver(const struct device *dev) : dev_(dev) {}

	bool ready() const;

	void set_clock(uint32_t hz) override;
	void connect() override;
	void disconnect() override;
	void configure(uint8_t turnaround_cycles, bool data_phase) override;
	void set_idle_cycles(uint8_t cycles) override { idle_cycles_ = cycles; }
	void swj_sequence(std::span<const uint8_t> data, size_t bit_count) override;
	void swd_output(std::span<const uint8_t> data, size_t bit_count) override;
	void swd_input(std::span<uint8_t> data, size_t bit_count) override;
	dap::Ack transfer(uint8_t request, uint32_t *data) override;
	bool transfer_block_read(uint8_t request, uint16_t count, uint8_t *out,
				 size_t out_cap, uint16_t *completed,
				 uint8_t *ack) override;
	void set_pins(uint8_t select, uint8_t value, uint32_t wait_us) override;
	uint8_t read_pins() override;
	void delay_us(uint32_t us) override;
	uint32_t timestamp() override;

private:
	const struct device *dev_;
	uint8_t idle_cycles_ = 0;
};

} // namespace probe
