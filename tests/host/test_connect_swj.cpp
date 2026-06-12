#include "testing.hpp"
#include "mock_swd_driver.hpp"
#include "dap/constants.hpp"
#include "dap/engine.hpp"

#include <vector>

namespace {
struct Fixture {
	MockSwdDriver drv;
	MockHostStatus host;
	dap::Config cfg;
	dap::DapEngine engine{drv, host, cfg};
	std::vector<uint8_t> run(std::initializer_list<uint8_t> req)
	{
		uint8_t resp[256] = {};
		size_t n = engine.process({req.begin(), req.size()}, resp);
		return {resp, resp + n};
	}
};
} // namespace

TEST(connect_default_selects_swd)
{
	Fixture f;
	auto r = f.run({0x02, dap::kPortDefault});
	const uint8_t want[] = {0x02, dap::kPortSwd};
	CHECK_BYTES(r, want);
	CHECK_EQ(f.drv.events.size(), 1u);
	if (!f.drv.events.empty()) {
		CHECK(f.drv.events[0] == "connect");
	}
}

TEST(connect_swd_explicit)
{
	Fixture f;
	auto r = f.run({0x02, dap::kPortSwd});
	const uint8_t want[] = {0x02, dap::kPortSwd};
	CHECK_BYTES(r, want);
}

TEST(connect_jtag_refused)
{
	Fixture f;
	auto r = f.run({0x02, dap::kPortJtag});
	const uint8_t want[] = {0x02, dap::kPortDisabled};
	CHECK_BYTES(r, want);
	CHECK(f.drv.events.empty()); /* no pin takeover on refusal */
}

TEST(disconnect_releases_port)
{
	Fixture f;
	f.run({0x02, dap::kPortSwd});
	auto r = f.run({0x03});
	const uint8_t want[] = {0x03, dap::kDapOk};
	CHECK_BYTES(r, want);
	CHECK(!f.drv.events.empty() && f.drv.events.back() == "disconnect");
}

TEST(reset_target_ok_no_device_hook)
{
	Fixture f;
	auto r = f.run({0x0A});
	const uint8_t want[] = {0x0A, dap::kDapOk, 0x00};
	CHECK_BYTES(r, want);
}

TEST(swj_clock_sets_driver)
{
	Fixture f;
	auto r = f.run({0x11, 0x40, 0x42, 0x0F, 0x00}); /* 1 MHz LE */
	const uint8_t want[] = {0x11, dap::kDapOk};
	CHECK_BYTES(r, want);
	CHECK_EQ(f.drv.last_clock, 1000000u);
}

TEST(swj_clock_zero_is_error)
{
	Fixture f;
	auto r = f.run({0x11, 0x00, 0x00, 0x00, 0x00});
	const uint8_t want[] = {0x11, dap::kDapError};
	CHECK_BYTES(r, want);
}

TEST(swj_pins_sets_and_reads)
{
	Fixture f;
	f.drv.pins_state = dap::kPinNreset | dap::kPinSwclk;
	/* drive nRESET low, select only nRESET, wait 10us */
	auto r = f.run({0x10, 0x00, dap::kPinNreset, 0x0A, 0x00, 0x00, 0x00});
	const uint8_t want[] = {0x10, static_cast<uint8_t>(dap::kPinNreset | dap::kPinSwclk)};
	CHECK_BYTES(r, want);
	CHECK_EQ(f.drv.last_pin_select, dap::kPinNreset);
	CHECK_EQ(f.drv.last_pin_value, 0);
	CHECK_EQ(f.drv.last_pin_wait, 10u);
}

TEST(write_abort_when_connected)
{
	Fixture f;
	f.run({0x02, dap::kPortSwd});
	/* index 0, value 0x0000001E -> DP write addr 0 (request byte 0x00) */
	auto r = f.run({0x08, 0x00, 0x1E, 0x00, 0x00, 0x00});
	const uint8_t want[] = {0x08, dap::kDapOk};
	CHECK_BYTES(r, want);
	CHECK_EQ(f.drv.transfers.size(), 1u);
	if (!f.drv.transfers.empty()) {
		CHECK_EQ(f.drv.transfers[0].request, 0x00);
		CHECK_EQ(f.drv.transfers[0].write_data, 0x0000001Eu);
	}
}

TEST(write_abort_disconnected_is_error)
{
	Fixture f;
	auto r = f.run({0x08, 0x00, 0x1E, 0x00, 0x00, 0x00});
	const uint8_t want[] = {0x08, dap::kDapError};
	CHECK_BYTES(r, want);
	CHECK(f.drv.transfers.empty());
}
