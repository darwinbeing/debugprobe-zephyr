#include "dap/dap_backend.hpp"

#include "dap/null_swd_driver.hpp"

#include "dap/constants.hpp"
#include "dap/engine.hpp"
#include "dap/interfaces.hpp"

#include <zephyr/kernel.h>

namespace probe {

/* No LEDs in phase 2: host-status requests are accepted but do nothing.
 * Phase 4 replaces this with the LED adapter alongside the real DAP_Info
 * parity work.
 */
class NullHostStatus final : public dap::HostStatus {
public:
	void connected(bool) override {}
	void running(bool) override {}
};

void NullSwdDriver::delay_us(uint32_t us)
{
	k_busy_wait(us);
}

uint32_t NullSwdDriver::timestamp()
{
	return k_cyc_to_us_floor32(k_cycle_get_32());
}

namespace dap_backend {

namespace {

NullSwdDriver g_driver;
NullHostStatus g_host;

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
