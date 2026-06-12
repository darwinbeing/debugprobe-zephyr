#pragma once
/* C-callable wrappers over Zephyr's swdp_api, so the C++ dap::SwdDriver
 * adapter can drive the swdp device without including the non-C++-clean
 * <zephyr/drivers/swdp.h>. Implemented in src/dap/swdp_shim.c.
 */
#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

int probe_swdp_output_sequence(const struct device *dev, uint32_t count,
			       const uint8_t *data);
int probe_swdp_input_sequence(const struct device *dev, uint32_t count,
			      uint8_t *data);
int probe_swdp_transfer(const struct device *dev, uint8_t request,
			uint32_t *data, uint8_t idle_cycles, uint8_t *response);
int probe_swdp_set_pins(const struct device *dev, uint8_t pins, uint8_t value);
int probe_swdp_get_pins(const struct device *dev, uint8_t *state);
int probe_swdp_set_clock(const struct device *dev, uint32_t clock);
int probe_swdp_configure(const struct device *dev, uint8_t turnaround,
			 bool data_phase);
int probe_swdp_port_on(const struct device *dev);
int probe_swdp_port_off(const struct device *dev);

#ifdef __cplusplus
}
#endif
