#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <array>
#include <span>

#include "usb/usb_device.hpp"
#include "uart_bridge.hpp"

#if defined(CONFIG_PROBE_MSP430_SBW)
#include "msp430/msp430_backend.hpp"
#endif

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

namespace {

/* C++20 smoke check: if concepts + std::span compile, the toolchain and
 * Kconfig selections are correct.
 */
template <typename T>
concept ByteBuffer = requires(T t) {
	{ t.size() } -> std::convertible_to<size_t>;
	{ t.data() };
};

static_assert(ByteBuffer<std::array<uint8_t, 4>>);
static_assert(ByteBuffer<std::span<const uint8_t>>);

} // namespace

int main()
{
	LOG_INF("debugprobe-zephyr starting");

#if defined(CONFIG_PROBE_MSP430_SBW)
	/* MSP430 Spy-Bi-Wire: vendor command 0x90 on the DAP interface. */
	probe::msp430_backend::init();
#endif

	if (int err = probe::usb::init(); err != 0) {
		LOG_ERR("USB init failed: %d", err);
		return err;
	}

	/* Wire the CDC-ACM <-> uart1 data path (custom interrupt-driven bridge,
	 * replacing zephyr,uart-bridge). */
	probe::uart_bridge_init();

	/* USB runs on the usbd thread + controller IRQs; nothing for main to do. */
	return 0;
}
