#include "testing.hpp"
#include "mock_swd_driver.hpp"
#include "dap/engine.hpp"

#include <algorithm>
#include <vector>

namespace {

struct RecordingVendor : dap::VendorHandler {
	std::vector<uint8_t> last_request;
	std::vector<uint8_t> reply; /* empty -> decline */
	int calls = 0;

	size_t handle(std::span<const uint8_t> request, std::span<uint8_t> response) override
	{
		++calls;
		last_request.assign(request.begin(), request.end());
		if (reply.empty() || reply.size() > response.size()) {
			return 0;
		}
		std::copy(reply.begin(), reply.end(), response.begin());
		return reply.size();
	}
};

struct Fixture {
	MockSwdDriver drv;
	MockHostStatus host;
	dap::Config cfg;
	dap::DapEngine engine{drv, host, cfg};
	RecordingVendor vendor;

	std::vector<uint8_t> run(std::initializer_list<uint8_t> req)
	{
		uint8_t resp[256] = {};
		size_t n = engine.process({req.begin(), req.size()}, resp);
		return {resp, resp + n};
	}
};

} // namespace

TEST(vendor_hook_receives_whole_request_and_answers)
{
	Fixture f;
	f.engine.set_vendor_handler(&f.vendor);
	f.vendor.reply = {0x90, 0x00, 0xAA, 0xBB};
	auto r = f.run({0x90, 0x01, 0x02, 0x03});
	const uint8_t want_req[] = {0x90, 0x01, 0x02, 0x03};
	const uint8_t want[] = {0x90, 0x00, 0xAA, 0xBB};
	CHECK_BYTES(f.vendor.last_request, want_req);
	CHECK_BYTES(r, want);
}

TEST(vendor_hook_decline_falls_back_to_echo)
{
	Fixture f;
	f.engine.set_vendor_handler(&f.vendor); /* empty reply -> declines */
	auto r = f.run({0x85, 0x01});
	const uint8_t want[] = {0x85};
	CHECK_BYTES(r, want);
	CHECK_EQ(f.vendor.calls, 1);
}

TEST(vendor_hook_not_called_for_standard_commands)
{
	Fixture f;
	f.engine.set_vendor_handler(&f.vendor);
	f.run({0x00, 0x04}); /* DAP_Info: firmware version */
	CHECK_EQ(f.vendor.calls, 0);
}

TEST(vendor_hook_handled_command_consumes_rest_of_batch)
{
	Fixture f;
	f.engine.set_vendor_handler(&f.vendor);
	f.vendor.reply = {0x90, 0x00};
	/* ExecuteCommands announcing 2 sub-commands: vendor 0x90, then bytes that
	 * look like DAP_Delay. A handled vendor command swallows the rest of the
	 * packet, so the delay is never dispatched. */
	auto r = f.run({0x7F, 0x02, 0x90, 0x09, 0x64, 0x00});
	const uint8_t want[] = {0x7F, 0x02, 0x90, 0x00};
	CHECK_BYTES(r, want);
	CHECK_EQ(f.drv.delays_us, 0u);
}
