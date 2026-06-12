#include "leds.hpp"

#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>

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
const struct gpio_dt_spec uart_tx_led = LED_SPEC(uart_tx_led);
const struct gpio_dt_spec uart_rx_led = LED_SPEC(uart_rx_led);

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

void tx_off(struct k_timer *) { led_set(&uart_tx_led, false); }
void rx_off(struct k_timer *) { led_set(&uart_rx_led, false); }
K_TIMER_DEFINE(tx_timer, tx_off, nullptr);
K_TIMER_DEFINE(rx_timer, rx_off, nullptr);
constexpr k_timeout_t kDebounce = K_MSEC(40);

struct LedInit {
	LedInit()
	{
		led_configure(&dap_connected);
		led_configure(&dap_running);
		led_configure(&usb_connected);
		led_configure(&uart_tx_led);
		led_configure(&uart_rx_led);
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

void leds_uart_tx_activity()
{
	led_set(&uart_tx_led, true);
	k_timer_start(&tx_timer, kDebounce, K_NO_WAIT);
}

void leds_uart_rx_activity()
{
	led_set(&uart_rx_led, true);
	k_timer_start(&rx_timer, kDebounce, K_NO_WAIT);
}

} // namespace probe
