#include "testing.hpp"
#include "mock_swd_driver.hpp"
#include "dap/constants.hpp"
#include "dap/engine.hpp"

#include <vector>

namespace {

struct Fixture {
	MockSwdDriver drv;
	MockHostStatus host;
	dap::Config cfg; /* defaults match the original debugprobe */
	dap::DapEngine engine{drv, host, cfg};

	std::vector<uint8_t> run(std::initializer_list<uint8_t> req)
	{
		uint8_t resp[256] = {};
		size_t n = engine.process({req.begin(), req.size()}, resp);
		return {resp, resp + n};
	}
};

} // namespace

TEST(info_fw_version_includes_nul)
{
	Fixture f;
	auto r = f.run({0x00, dap::kInfoFwVersion});
	const uint8_t want[] = {0x00, 6, '2', '.', '0', '.', '0', 0x00};
	CHECK_BYTES(r, want);
}

TEST(info_empty_strings_when_unset)
{
	Fixture f;
	for (uint8_t id : {dap::kInfoVendor, dap::kInfoProduct, dap::kInfoSerial,
			   dap::kInfoDeviceVendor, dap::kInfoDeviceName}) {
		auto r = f.run({0x00, id});
		const uint8_t want[] = {0x00, 0};
		CHECK_BYTES(r, want);
	}
}

TEST(info_strings_injected)
{
	Fixture f;
	f.cfg.vendor = "Raspberry Pi";
	dap::DapEngine e{f.drv, f.host, f.cfg};
	uint8_t resp[64];
	const uint8_t req[] = {0x00, dap::kInfoVendor};
	size_t n = e.process(req, resp);
	CHECK_EQ(n, 2u + 13u); /* "Raspberry Pi" + NUL */
	CHECK_EQ(resp[1], 13);
	CHECK_EQ(resp[2], 'R');
	CHECK_EQ(resp[14], 0x00);
}

TEST(info_capabilities_swd_atomic_timestamp)
{
	Fixture f;
	auto r = f.run({0x00, dap::kInfoCapabilities});
	const uint8_t want[] = {0x00, 1, 0x31};
	CHECK_BYTES(r, want);
}

TEST(info_packet_size_and_count)
{
	Fixture f;
	auto rs = f.run({0x00, dap::kInfoPacketSize});
	const uint8_t want_size[] = {0x00, 2, 64, 0};
	CHECK_BYTES(rs, want_size);
	auto rc = f.run({0x00, dap::kInfoPacketCount});
	const uint8_t want_cnt[] = {0x00, 1, 6}; /* original's count-2 quirk */
	CHECK_BYTES(rc, want_cnt);
}

TEST(info_timestamp_clock)
{
	Fixture f;
	auto r = f.run({0x00, dap::kInfoTimestampClock});
	const uint8_t want[] = {0x00, 4, 0x40, 0x42, 0x0F, 0x00}; /* 1000000 LE */
	CHECK_BYTES(r, want);
}

TEST(info_unknown_id_and_swo_buffer_empty)
{
	Fixture f;
	for (uint8_t id : {uint8_t{0x42}, dap::kInfoSwoBufferSize}) {
		auto r = f.run({0x00, id});
		const uint8_t want[] = {0x00, 0};
		CHECK_BYTES(r, want);
	}
}

TEST(host_status_drives_leds)
{
	Fixture f;
	auto r = f.run({0x01, 0x00, 0x01}); /* connected LED on */
	const uint8_t want[] = {0x01, dap::kDapOk};
	CHECK_BYTES(r, want);
	CHECK_EQ(f.host.connected_calls, 1);
	CHECK(f.host.last_connected);

	r = f.run({0x01, 0x01, 0x00}); /* running LED off */
	CHECK_BYTES(r, want);
	CHECK_EQ(f.host.running_calls, 1);
	CHECK(!f.host.last_running);
}

TEST(host_status_bad_type_is_error)
{
	Fixture f;
	auto r = f.run({0x01, 0x02, 0x01});
	const uint8_t want[] = {0x01, dap::kDapError};
	CHECK_BYTES(r, want);
}

TEST(delay_calls_driver)
{
	Fixture f;
	auto r = f.run({0x09, 0xE8, 0x03}); /* 1000 us */
	const uint8_t want[] = {0x09, dap::kDapOk};
	CHECK_BYTES(r, want);
	CHECK_EQ(f.drv.delays_us, 1000u);
}

TEST(unknown_command_responds_invalid)
{
	Fixture f;
	for (uint8_t id : {uint8_t{0x17}, uint8_t{0x1C}, uint8_t{0x40},
			   uint8_t{0x7E}, uint8_t{0xFE}}) {
		auto r = f.run({id});
		const uint8_t want[] = {dap::kCmdInvalid};
		CHECK_BYTES(r, want);
	}
}

TEST(jtag_commands_respond_error)
{
	/* JTAG compiled out: handlers exist but answer DAP_ERROR
	 * (JTAG_Sequence DAP.c:~520; Configure/IDCODE similar). */
	Fixture f;
	auto r = f.run({0x14, 0x01, 0x01, 0xAA}); /* JTAG_Sequence */
	const uint8_t want14[] = {0x14, dap::kDapError};
	CHECK_BYTES(r, want14);
	r = f.run({0x15, 0x01, 0x04}); /* JTAG_Configure */
	const uint8_t want15[] = {0x15, dap::kDapError};
	CHECK_BYTES(r, want15);
	r = f.run({0x16, 0x00}); /* JTAG_IDCODE: 1-byte error (DAP.c id_error path) */
	const uint8_t want16[] = {0x16, dap::kDapError};
	CHECK_BYTES(r, want16);
}

TEST(vendor_command_echoes_id_only)
{
	Fixture f;
	auto r = f.run({0x85, 0x01, 0x02});
	const uint8_t want[] = {0x85};
	CHECK_BYTES(r, want);
}

TEST(execute_commands_batches)
{
	Fixture f;
	/* 0x7F, count=2: Delay(100us), Info(FW ver) */
	auto r = f.run({0x7F, 0x02, 0x09, 0x64, 0x00, 0x00, dap::kInfoFwVersion});
	const uint8_t want[] = {0x7F, 0x02,
				0x09, dap::kDapOk,
				0x00, 6, '2', '.', '0', '.', '0', 0x00};
	CHECK_BYTES(r, want);
	CHECK_EQ(f.drv.delays_us, 100u);
}

TEST(empty_request_is_invalid)
{
	Fixture f;
	uint8_t resp[8];
	size_t n = f.engine.process({}, resp);
	CHECK_EQ(n, 1u);
	CHECK_EQ(resp[0], dap::kCmdInvalid);
}

TEST(truncated_info_request_no_crash)
{
	Fixture f;
	auto r = f.run({0x00}); /* missing info id byte */
	const uint8_t want[] = {0x00, 0};
	CHECK_BYTES(r, want);
}

TEST(execute_commands_vendor_mid_batch_stays_in_sync)
{
	Fixture f;
	/* batch: Vendor5(no payload), Delay(100us) — vendor must consume 1 byte */
	auto r = f.run({0x7F, 0x02, 0x85, 0x09, 0x64, 0x00});
	const uint8_t want[] = {0x7F, 0x02, 0x85, 0x09, dap::kDapOk};
	CHECK_BYTES(r, want);
	CHECK_EQ(f.drv.delays_us, 100u);
}

TEST(execute_commands_jtag_configure_mid_batch_stays_in_sync)
{
	Fixture f;
	/* batch: JTAG_Configure(count=2, ir lengths 4,5), then Delay(7us) */
	auto r = f.run({0x7F, 0x02, 0x15, 0x02, 0x04, 0x05, 0x09, 0x07, 0x00});
	const uint8_t want[] = {0x7F, 0x02, 0x15, dap::kDapError, 0x09, dap::kDapOk};
	CHECK_BYTES(r, want);
	CHECK_EQ(f.drv.delays_us, 7u);
}

TEST(execute_commands_jtag_sequence_mid_batch_stays_in_sync)
{
	Fixture f;
	/* JTAG_Sequence: 1 sequence, info 0x08 (8 TCK cycles -> 1 data byte),
	 * data 0xAA; then Delay(7us) */
	auto r = f.run({0x7F, 0x02, 0x14, 0x01, 0x08, 0xAA, 0x09, 0x07, 0x00});
	const uint8_t want[] = {0x7F, 0x02, 0x14, dap::kDapError, 0x09, dap::kDapOk};
	CHECK_BYTES(r, want);
	CHECK_EQ(f.drv.delays_us, 7u);
}

TEST(execute_commands_jtag_idcode_mid_batch_stays_in_sync)
{
	Fixture f;
	auto r = f.run({0x7F, 0x02, 0x16, 0x00, 0x09, 0x07, 0x00});
	const uint8_t want[] = {0x7F, 0x02, 0x16, dap::kDapError, 0x09, dap::kDapOk};
	CHECK_BYTES(r, want);
	CHECK_EQ(f.drv.delays_us, 7u);
}
