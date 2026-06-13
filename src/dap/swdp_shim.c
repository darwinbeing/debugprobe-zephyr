/* C shim over Zephyr's swdp_api.
 *
 * Zephyr's <zephyr/drivers/swdp.h> defines static-inline syscall helpers
 * (z_impl_swdp_*) that do `const struct swdp_api *api = dev->api;` — an
 * implicit `const void*` -> `const swdp_api*` conversion that is valid C but
 * an error in C++ (the header is not C++-clean). The dap::SwdDriver adapter
 * must be C++ (it derives from a C++ interface), so it cannot include that
 * header directly. This thin C file includes it where the conversion is legal
 * and re-exposes the API as plain C functions for the adapter to call.
 *
 * These wrappers map 1:1 onto swdp_api; see src/dap/swdp_driver.cpp.
 */
#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/swdp.h>
#include <zephyr/sys/util.h>

#include "dap/swdp_shim.h"

int probe_swdp_output_sequence(const struct device *dev, uint32_t count,
			       const uint8_t *data)
{
	return swdp_output_sequence(dev, count, data);
}

int probe_swdp_input_sequence(const struct device *dev, uint32_t count,
			      uint8_t *data)
{
	return swdp_input_sequence(dev, count, data);
}

int probe_swdp_transfer(const struct device *dev, uint8_t request,
			uint32_t *data, uint8_t idle_cycles, uint8_t *response)
{
	return swdp_transfer(dev, request, data, idle_cycles, response);
}

int probe_swdp_set_pins(const struct device *dev, uint8_t pins, uint8_t value)
{
	return swdp_set_pins(dev, pins, value);
}

int probe_swdp_get_pins(const struct device *dev, uint8_t *state)
{
	return swdp_get_pins(dev, state);
}

int probe_swdp_set_clock(const struct device *dev, uint32_t clock)
{
	return swdp_set_clock(dev, clock);
}

int probe_swdp_configure(const struct device *dev, uint8_t turnaround,
			 bool data_phase)
{
	return swdp_configure(dev, turnaround, data_phase);
}

int probe_swdp_port_on(const struct device *dev)
{
	return swdp_port_on(dev);
}

int probe_swdp_port_off(const struct device *dev)
{
	return swdp_port_off(dev);
}

/* The DMA block path lives in the PIO driver (not the swdp_api vtable, which is
 * Zephyr's and unextendable). Call its exported entry point directly for the
 * PIO backend; decline for the bitbang/null backends so the engine uses the
 * per-word path. */
#if defined(CONFIG_PROBE_SWD_PIO)
int swdp_pio_transfer_block_read(const struct device *dev, uint8_t request,
				 uint16_t count, uint8_t *out, size_t out_cap,
				 uint16_t *completed, uint8_t *ack);

int probe_swdp_transfer_block(const struct device *dev, uint8_t request,
			      uint16_t count, uint8_t *out, size_t out_cap,
			      uint16_t *completed, uint8_t *ack)
{
	return swdp_pio_transfer_block_read(dev, request, count, out, out_cap,
					    completed, ack);
}
#else
int probe_swdp_transfer_block(const struct device *dev, uint8_t request,
			      uint16_t count, uint8_t *out, size_t out_cap,
			      uint16_t *completed, uint8_t *ack)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(request);
	ARG_UNUSED(count);
	ARG_UNUSED(out);
	ARG_UNUSED(out_cap);
	ARG_UNUSED(completed);
	ARG_UNUSED(ack);
	return -ENOTSUP;
}
#endif
