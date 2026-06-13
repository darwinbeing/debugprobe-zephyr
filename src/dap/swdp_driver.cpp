#include "dap/swdp_driver.hpp"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

/* Drive the swdp device through the C shim (src/dap/swdp_shim.c): Zephyr's
 * <zephyr/drivers/swdp.h> is not C++-clean, so the adapter cannot include it
 * directly. The shim wraps each swdp_api call as a plain C function. */
#include "dap/swdp_shim.h"

LOG_MODULE_REGISTER(swdp_drv, LOG_LEVEL_INF);

namespace probe {

bool ZephyrSwdpDriver::ready() const
{
	return device_is_ready(dev_);
}

void ZephyrSwdpDriver::set_clock(uint32_t hz)
{
	probe_swdp_set_clock(dev_, hz);
}

void ZephyrSwdpDriver::connect()
{
	probe_swdp_port_on(dev_);
}

void ZephyrSwdpDriver::disconnect()
{
	probe_swdp_port_off(dev_);
}

void ZephyrSwdpDriver::configure(uint8_t turnaround_cycles, bool data_phase)
{
	/* swdp_configure takes turnaround as a 1-based cycle count and a
	 * data-phase flag (bool), matching DAP_SWD_Configure semantics. */
	probe_swdp_configure(dev_, turnaround_cycles, data_phase);
}

void ZephyrSwdpDriver::swj_sequence(std::span<const uint8_t> data, size_t bit_count)
{
	probe_swdp_output_sequence(dev_, static_cast<uint32_t>(bit_count),
				   data.data());
}

void ZephyrSwdpDriver::swd_output(std::span<const uint8_t> data, size_t bit_count)
{
	probe_swdp_output_sequence(dev_, static_cast<uint32_t>(bit_count),
				   data.data());
}

void ZephyrSwdpDriver::swd_input(std::span<uint8_t> data, size_t bit_count)
{
	probe_swdp_input_sequence(dev_, static_cast<uint32_t>(bit_count),
				  data.data());
}

dap::Ack ZephyrSwdpDriver::transfer(uint8_t request, uint32_t *data)
{
	uint8_t response = 0;
	uint32_t scratch = 0;
	int ret = probe_swdp_transfer(dev_, request, data ? data : &scratch,
				      idle_cycles_, &response);
	if (ret < 0) {
		return dap::Ack::ProtocolError;
	}
	return static_cast<dap::Ack>(response);
}

bool ZephyrSwdpDriver::transfer_block_read(uint8_t request, uint16_t count,
					   uint8_t *out, size_t out_cap,
					   uint16_t *completed, uint8_t *ack)
{
	/* The DMA builder emits no idle cycles, so only the idle==0 case (the
	 * usual one) can take the fast path; otherwise defer to the per-word
	 * loop, which honors idle_cycles_. */
	if (idle_cycles_ != 0) {
		return false;
	}
	return probe_swdp_transfer_block(dev_, request, count, out, out_cap,
					 completed, ack) == 0;
}

void ZephyrSwdpDriver::set_pins(uint8_t select, uint8_t value, uint32_t wait_us)
{
	probe_swdp_set_pins(dev_, select, value);
	if (wait_us != 0U) {
		k_busy_wait(wait_us);
	}
}

uint8_t ZephyrSwdpDriver::read_pins()
{
	uint8_t state = 0;
	probe_swdp_get_pins(dev_, &state);
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
