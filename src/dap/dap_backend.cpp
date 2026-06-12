#include "dap/dap_backend.hpp"

#include "dap/constants.hpp"
#include "dap/engine.hpp"
#include "dap/interfaces.hpp"
#include "leds.hpp"

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>

/* SWD backend selection (Kconfig PROBE_SWD_*):
 *   NULL    -> NullSwdDriver (no SWD; bring-up only)
 *   BITBANG -> ZephyrSwdpDriver over the zephyr,swdp-gpio device (swd_bitbang)
 *   PIO     -> ZephyrSwdpDriver over the raspberrypi,swdp-pio device (swd_pio)
 * Both real backends share the single adapter; the device is selected by
 * devicetree (the swd_bitbang/swd_pio nodelabel) per the Kconfig choice.
 */
#if defined(CONFIG_PROBE_SWD_NULL)
#include "dap/null_swd_driver.hpp"
#else
#include "dap/swdp_driver.hpp"
#endif

namespace probe {

#if defined(CONFIG_PROBE_SWD_NULL)
void NullSwdDriver::delay_us(uint32_t us)
{
	k_busy_wait(us);
}

uint32_t NullSwdDriver::timestamp()
{
	return k_cyc_to_us_floor32(k_cycle_get_32());
}
#endif

namespace dap_backend {

namespace {

#if defined(CONFIG_PROBE_SWD_NULL)
NullSwdDriver g_driver;
#elif defined(CONFIG_PROBE_SWD_PIO)
ZephyrSwdpDriver g_driver{DEVICE_DT_GET(DT_NODELABEL(swd_pio))};
#else
ZephyrSwdpDriver g_driver{DEVICE_DT_GET(DT_NODELABEL(swd_bitbang))};
#endif
LedHostStatus g_host;

/* Engine configuration. serial is left nullptr until phase 4 wires the
 * hwinfo-derived serial number; DAP_Info(Serial) reports an empty string for
 * now (the original debugprobe also returned empty strings here).
 */
dap::Config g_config = {
	.vendor = "Raspberry Pi",
	.product = CONFIG_PROBE_PRODUCT_STRING,
	.serial = nullptr, /* TODO(phase 4): hwinfo serial number */
	.fw_version = "2.0.0",
	.packet_size = 64,
	.packet_count = 6,
};

dap::DapEngine g_engine{g_driver, g_host, g_config};

} // namespace

size_t process(std::span<const uint8_t> request, std::span<uint8_t> response)
{
	/* In this single-in-flight-OUT design — and in the original RP2040
	 * debugprobe — abort-during-transfer is architecturally impossible: the
	 * OUT endpoint is not re-armed until the current request finishes.  A
	 * raw DAP_TransferAbort (0x07) therefore cannot arrive while a Transfer
	 * is in progress; it falls to the engine's default branch and is
	 * answered with DAP_Invalid (0xFF), exactly as the original firmware's
	 * DAP_ProcessCommand does (DAP.c has no case ID_DAP_TransferAbort).
	 * request_abort() remains available for the phase-4 async/double-buffered
	 * transport, which can call it out-of-band on the abort endpoint.
	 */
	return g_engine.process(request, response);
}

void request_abort()
{
	g_engine.request_abort();
}

} // namespace dap_backend

} // namespace probe
