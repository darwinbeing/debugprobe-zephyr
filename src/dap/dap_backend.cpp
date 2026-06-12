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
	/* DAP_TransferAbort (0x07) is an out-of-band signal in the original
	 * transport: it is delivered on a separate path and never produces a
	 * response. Phase 2 has a single-buffered transport, so handle it here
	 * by setting the abort flag and emitting no response. The phase-4
	 * double-buffered transport will route 0x07 properly before it reaches
	 * the engine; this simplification is acceptable until then.
	 */
	if (!request.empty() && request[0] == dap::kCmdTransferAbort) {
		request_abort();
		return 0;
	}

	return g_engine.process(request, response);
}

void request_abort()
{
	g_engine.request_abort();
}

} // namespace dap_backend

} // namespace probe
