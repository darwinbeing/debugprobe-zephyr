#include "dap/swdp_driver.hpp"

#include <zephyr/drivers/swdp.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(swdp_drv, LOG_LEVEL_INF);

namespace probe {

namespace {
const struct swdp_api *api(const struct device *dev)
{
	return static_cast<const struct swdp_api *>(dev->api);
}
} // namespace

bool ZephyrSwdpDriver::ready() const
{
	return device_is_ready(dev_);
}

void ZephyrSwdpDriver::set_clock(uint32_t hz)
{
	api(dev_)->swdp_set_clock(dev_, hz);
}

void ZephyrSwdpDriver::connect()
{
	api(dev_)->swdp_port_on(dev_);
}

void ZephyrSwdpDriver::disconnect()
{
	api(dev_)->swdp_port_off(dev_);
}

void ZephyrSwdpDriver::configure(uint8_t turnaround_cycles, bool data_phase)
{
	/* swdp_configure takes turnaround as a 1-based cycle count and a
	 * data-phase flag (bool), matching DAP_SWD_Configure semantics. */
	api(dev_)->swdp_configure(dev_, turnaround_cycles, data_phase);
}

void ZephyrSwdpDriver::swj_sequence(std::span<const uint8_t> data, size_t bit_count)
{
	api(dev_)->swdp_output_sequence(dev_, static_cast<uint32_t>(bit_count),
					data.data());
}

void ZephyrSwdpDriver::swd_output(std::span<const uint8_t> data, size_t bit_count)
{
	api(dev_)->swdp_output_sequence(dev_, static_cast<uint32_t>(bit_count),
					data.data());
}

void ZephyrSwdpDriver::swd_input(std::span<uint8_t> data, size_t bit_count)
{
	api(dev_)->swdp_input_sequence(dev_, static_cast<uint32_t>(bit_count),
				       data.data());
}

dap::Ack ZephyrSwdpDriver::transfer(uint8_t request, uint32_t *data)
{
	uint8_t response = 0;
	uint32_t scratch = 0;
	int ret = api(dev_)->swdp_transfer(dev_, request,
					   data ? data : &scratch,
					   idle_cycles_, &response);
	if (ret < 0) {
		return dap::Ack::ProtocolError;
	}
	return static_cast<dap::Ack>(response);
}

void ZephyrSwdpDriver::set_pins(uint8_t select, uint8_t value, uint32_t wait_us)
{
	api(dev_)->swdp_set_pins(dev_, select, value);
	if (wait_us != 0U) {
		k_busy_wait(wait_us);
	}
}

uint8_t ZephyrSwdpDriver::read_pins()
{
	uint8_t state = 0;
	api(dev_)->swdp_get_pins(dev_, &state);
	return state;
}

void ZephyrSwdpDriver::delay_us(uint32_t us)
{
	k_busy_wait(us);
}

uint32_t ZephyrSwdpDriver::timestamp()
{
	return k_cyc_to_us_floor32(k_cycle_get_32());
}

} // namespace probe
