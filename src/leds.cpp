#include "leds.hpp"

#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>

namespace probe {

namespace {

/* A LED is present only if the alias resolves to a gpios property. Each entry
 * is compiled out to an empty spec ({0}) when its alias is absent, and the
 * helpers no-op on a null port. */
#define LED_SPEC(alias) \
	GPIO_DT_SPEC_GET_OR(DT_ALIAS(alias), gpios, {0})

const struct gpio_dt_spec dap_connected = LED_SPEC(dap_connected_led);
const struct gpio_dt_spec dap_running = LED_SPEC(dap_running_led);
const struct gpio_dt_spec usb_connected = LED_SPEC(usb_connected_led);

void led_configure(const struct gpio_dt_spec *led)
{
	if (led->port != nullptr && gpio_is_ready_dt(led)) {
		gpio_pin_configure_dt(led, GPIO_OUTPUT_INACTIVE);
	}
}

void led_set(const struct gpio_dt_spec *led, bool on)
{
	if (led->port != nullptr && gpio_is_ready_dt(led)) {
		gpio_pin_set_dt(led, on ? 1 : 0);
	}
}

struct LedInit {
	LedInit()
	{
		led_configure(&dap_connected);
		led_configure(&dap_running);
		led_configure(&usb_connected);
	}
};
const LedInit led_init;

} // namespace

void LedHostStatus::connected(bool on)
{
	led_set(&dap_connected, on);
}

void LedHostStatus::running(bool on)
{
	led_set(&dap_running, on);
}

void leds_usb_connected(bool on)
{
	led_set(&usb_connected, on);
}

} // namespace probe
