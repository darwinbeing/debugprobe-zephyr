#pragma once
/* CDC-ACM <-> hardware UART bridge (custom, replacing zephyr,uart-bridge),
 * so phase 4b can tap byte flow for activity LEDs and intercept the
 * MAGIC_BAUD line-coding to drive autobaud.
 */
#include <zephyr/device.h>

namespace probe {

/* Start the bridge between the CDC-ACM and the hardware UART (both Zephyr
 * uart devices). Call once at startup after the devices are ready. */
void uart_bridge_init();

/* Apply the CDC line coding to the hardware UART (called from the usbd
 * line-coding message). If the requested bit rate is MAGIC_BAUD it starts
 * autobaud instead of setting the UART. */
void uart_bridge_line_coding(const struct device *cdc_dev);

} // namespace probe
