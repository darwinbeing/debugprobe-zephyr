#pragma once
/* Drives the probe's status LEDs from the DAP engine's HostStatus.
 * Uses devicetree aliases (dap-connected-led, dap-running-led,
 * usb-connected-led); any alias that is absent on a board is a no-op.
 */
#include "dap/interfaces.hpp"

namespace probe {

class LedHostStatus final : public dap::HostStatus {
public:
	void connected(bool on) override;   /* DAP target connected (HostStatus type 0) */
	void running(bool on) override;     /* DAP target running (HostStatus type 1) */
};

/* Turn the USB-connected indicator on once the USB device is configured. */
void leds_usb_connected(bool on);

/* UART bridge activity: call on each byte; the LED lights and auto-clears
 * ~40 ms after the last byte (debounce). No-op if the alias is absent. */
void leds_uart_tx_activity();
void leds_uart_rx_activity();

} // namespace probe
