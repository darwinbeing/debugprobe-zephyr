#include "msp430/msp430_backend.hpp"

#include <zephyr/device.h>
#include <zephyr/devicetree.h>

#include "dap/dap_backend.hpp"
#include "dap/interfaces.hpp"
#include "leds.hpp"
#include "msp430/cpuxv2.hpp"
#include "msp430/pio_sbw_port.hpp"
#include "msp430/sbw_tap.hpp"
#include "msp430/session.hpp"
#include "pin_owner.hpp"
#include "sbw_pio.h"
#include "usb/dap_class.hpp"

namespace probe::msp430_backend {

namespace {

/* Largest data payload of one command: the DAP packet minus the longest
 * header ([0x90][op] + 4 address bytes), rounded down to leave slack. */
constexpr uint16_t kMaxDataBytes = probe::usb::kDapPacketSize - 8;

PioSbwPort g_port{DEVICE_DT_GET(DT_NODELABEL(sbw_pio))};
msp430::SbwTap g_tap{g_port};
msp430::Cpuxv2 g_cpu{g_tap};
msp430::Session g_session{g_cpu, kMaxDataBytes};
LedHostStatus g_led; /* stateless: drives the DAP-connected LED, if any */

class Handler final : public dap::VendorHandler {
public:
	size_t handle(std::span<const uint8_t> request,
		      std::span<uint8_t> response) override
	{
		const size_t n = g_session.handle(request, response);

		if (n != 0) {
			update_led();
		}
		return n;
	}

	/* Light the DAP-connected LED while an SBW session is open. */
	void update_led()
	{
		const bool open = g_session.state() != msp430::State::Idle;

		if (open != led_on_) {
			led_on_ = open;
			g_led.connected(open);
		}
	}

private:
	bool led_on_ = false;
};

Handler g_handler;

/* After an SWD session the SWD driver leaves the pins driven by its PIO. Hand
 * them back: high-impedance with the idle pulls, so an attached MSP430's RST
 * line is not held low. */
void restore_idle_pins()
{
	sbw_pio_release(DEVICE_DT_GET(DT_NODELABEL(sbw_pio)));
}

} // namespace

void init()
{
	pin_owner::set_sbw_force_close(abort);
	pin_owner::set_idle_hook(restore_idle_pins);
	dap_backend::set_vendor_handler(&g_handler);
}

void abort()
{
	g_session.abort();
	g_handler.update_led();
}

} // namespace probe::msp430_backend
