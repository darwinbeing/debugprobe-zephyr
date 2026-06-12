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

	Fixture() { connect(); }
	void connect()
	{
		uint8_t resp[8];
		const uint8_t req[] = {0x02, dap::kPortSwd};
		engine.process(req, resp);
		drv.events.clear();
	}
	std::vector<uint8_t> run(std::vector<uint8_t> req)
	{
		uint8_t resp[256] = {};
		size_t n = engine.process({req.data(), req.size()}, resp);
		return {resp, resp + n};
	}
};
} // namespace

TEST(transfer_configure_sets_state_and_acks)
{
	Fixture f;
	/* idle=2, wait_retry=0x0050, match_retry=0x0010 */
	auto r = f.run({0x04, 0x02, 0x50, 0x00, 0x10, 0x00});
	const uint8_t want[] = {0x04, dap::kDapOk};
	CHECK_BYTES(r, want);
}

TEST(transfer_single_dp_write)
{
	Fixture f;
	/* count=1: write DP addr 4 (0x04 -> A2 set), value 0x11223344.
	 * The reference issues a trailing RDBUFF read (NULL data) to capture
	 * the last write's ACK (DAP.c check_write, :902-908), so TWO SWD
	 * transfers occur for one DP write. */
	auto r = f.run({0x05, 0x00, 0x01, 0x04, 0x44, 0x33, 0x22, 0x11});
	const uint8_t want[] = {0x05, 0x01, dap::kXferOk};
	CHECK_BYTES(r, want);
	CHECK_EQ(f.drv.transfers.size(), 2u);
	CHECK_EQ(f.drv.transfers[0].request, 0x04);
	CHECK_EQ(f.drv.transfers[0].write_data, 0x11223344u);
	CHECK_EQ(f.drv.transfers[1].request, dap::kReqReadRdbuff);
	CHECK(f.drv.transfers[1].data_null);
}

TEST(transfer_write_fault_surfaces_via_trailing_read)
{
	Fixture f;
	/* Write ACKs OK, but the trailing check-write RDBUFF read FAULTs.
	 * The reference reports the write as completed (count=1) yet sets
	 * response_value to the trailing read's FAULT. */
	f.drv.script.push_back({dap::Ack::Ok, 0});    /* the write */
	f.drv.script.push_back({dap::Ack::Fault, 0}); /* trailing RDBUFF read */
	auto r = f.run({0x05, 0x00, 0x01, 0x04, 0x44, 0x33, 0x22, 0x11});
	const uint8_t want[] = {0x05, 0x01, dap::kXferFault};
	CHECK_BYTES(r, want);
	CHECK_EQ(f.drv.transfers.size(), 2u);
}

TEST(transfer_single_dp_read)
{
	Fixture f;
	f.drv.script.push_back({dap::Ack::Ok, 0xCAFEBABE});
	/* count=1: read DP addr 0 (RnW) */
	auto r = f.run({0x05, 0x00, 0x01, 0x02});
	const uint8_t want[] = {0x05, 0x01, dap::kXferOk, 0xBE, 0xBA, 0xFE, 0xCA};
	CHECK_BYTES(r, want);
}

TEST(transfer_ap_read_is_posted_and_flushed_via_rdbuff)
{
	Fixture f;
	/* AP read pipeline: post(read req, data=NULL) then RDBUFF read */
	f.drv.script.push_back({dap::Ack::Ok, 0});          /* post */
	f.drv.script.push_back({dap::Ack::Ok, 0x12345678}); /* RDBUFF */
	auto r = f.run({0x05, 0x00, 0x01, 0x03}); /* AP read addr 0 */
	const uint8_t want[] = {0x05, 0x01, dap::kXferOk, 0x78, 0x56, 0x34, 0x12};
	CHECK_BYTES(r, want);
	CHECK_EQ(f.drv.transfers.size(), 2u);
	CHECK_EQ(f.drv.transfers[0].request, 0x03);
	CHECK(f.drv.transfers[0].data_null); /* posted, no data collected */
	CHECK_EQ(f.drv.transfers[1].request, dap::kReqReadRdbuff);
}

TEST(transfer_back_to_back_ap_reads_pipeline)
{
	Fixture f;
	/* two AP reads: post A; read A while posting B; flush B via RDBUFF */
	f.drv.script.push_back({dap::Ack::Ok, 0});       /* post A */
	f.drv.script.push_back({dap::Ack::Ok, 0xAAAA0001}); /* re-post, data=A */
	f.drv.script.push_back({dap::Ack::Ok, 0xAAAA0002}); /* RDBUFF -> B */
	auto r = f.run({0x05, 0x00, 0x02, 0x03, 0x03});
	const uint8_t want[] = {0x05, 0x02, dap::kXferOk,
				0x01, 0x00, 0xAA, 0xAA,
				0x02, 0x00, 0xAA, 0xAA};
	CHECK_BYTES(r, want);
	CHECK_EQ(f.drv.transfers.size(), 3u);
	CHECK_EQ(f.drv.transfers[1].request, 0x03); /* read prev + post next */
	CHECK_EQ(f.drv.transfers[2].request, dap::kReqReadRdbuff);
}

TEST(transfer_write_after_posted_read_flushes_first)
{
	Fixture f;
	f.drv.script.push_back({dap::Ack::Ok, 0});          /* post AP read */
	f.drv.script.push_back({dap::Ack::Ok, 0x000000AA}); /* RDBUFF flush */
	f.drv.script.push_back({dap::Ack::Ok, 0});          /* DP write */
	auto r = f.run({0x05, 0x00, 0x02, 0x03, 0x04, 0x55, 0x00, 0x00, 0x00});
	const uint8_t want[] = {0x05, 0x02, dap::kXferOk, 0xAA, 0x00, 0x00, 0x00};
	CHECK_BYTES(r, want);
	CHECK_EQ(f.drv.transfers[1].request, dap::kReqReadRdbuff);
	CHECK_EQ(f.drv.transfers[2].request, 0x04);
}

TEST(transfer_wait_retries_then_succeeds)
{
	Fixture f;
	f.run({0x04, 0x00, 0x03, 0x00, 0x00, 0x00}); /* retry_count = 3 */
	f.drv.script.push_back({dap::Ack::Wait, 0});
	f.drv.script.push_back({dap::Ack::Wait, 0});
	f.drv.script.push_back({dap::Ack::Ok, 0x0000BEEF});
	auto r = f.run({0x05, 0x00, 0x01, 0x02});
	const uint8_t want[] = {0x05, 0x01, dap::kXferOk, 0xEF, 0xBE, 0x00, 0x00};
	CHECK_BYTES(r, want);
	CHECK_EQ(f.drv.transfers.size(), 3u);
}

TEST(transfer_wait_exhausts_retries)
{
	Fixture f;
	f.run({0x04, 0x00, 0x02, 0x00, 0x00, 0x00}); /* retry_count = 2 */
	for (int i = 0; i < 8; ++i) {
		f.drv.script.push_back({dap::Ack::Wait, 0});
	}
	auto r = f.run({0x05, 0x00, 0x01, 0x02});
	const uint8_t want[] = {0x05, 0x00, dap::kXferWait};
	CHECK_BYTES(r, want);
	/* 1 initial + 2 retries */
	CHECK_EQ(f.drv.transfers.size(), 3u);
}

TEST(transfer_fault_stops_batch)
{
	Fixture f;
	f.drv.script.push_back({dap::Ack::Ok, 0});    /* write 1 ok */
	f.drv.script.push_back({dap::Ack::Fault, 0}); /* write 2 faults */
	auto r = f.run({0x05, 0x00, 0x03,
			0x04, 0x01, 0x00, 0x00, 0x00,
			0x04, 0x02, 0x00, 0x00, 0x00,
			0x04, 0x03, 0x00, 0x00, 0x00});
	const uint8_t want[] = {0x05, 0x01, dap::kXferFault};
	CHECK_BYTES(r, want);
	CHECK_EQ(f.drv.transfers.size(), 2u); /* third never issued */
}

TEST(transfer_match_value_polls_until_match)
{
	Fixture f;
	f.run({0x04, 0x00, 0x64, 0x00, 0x05, 0x00}); /* match_retry = 5 */
	/* set match mask = 0xFF via write-match-mask request (bit5) */
	auto r = f.run({0x05, 0x00, 0x01, 0x20, 0xFF, 0x00, 0x00, 0x00});
	const uint8_t want_mask[] = {0x05, 0x01, dap::kXferOk};
	CHECK_BYTES(r, want_mask);
	CHECK(f.drv.transfers.empty()); /* mask write is engine-internal */

	/* DP read with value-match (bit4): expect 0x42 */
	f.drv.script.push_back({dap::Ack::Ok, 0x10}); /* mismatch */
	f.drv.script.push_back({dap::Ack::Ok, 0x42}); /* match */
	r = f.run({0x05, 0x00, 0x01, 0x12, 0x42, 0x00, 0x00, 0x00});
	const uint8_t want[] = {0x05, 0x01, dap::kXferOk};
	CHECK_BYTES(r, want);
	CHECK_EQ(f.drv.transfers.size(), 2u);
}

TEST(transfer_match_value_mismatch_flag)
{
	Fixture f;
	/* match_retry stays default 0; mask write then immediate mismatch */
	f.run({0x05, 0x00, 0x01, 0x20, 0xFF, 0x00, 0x00, 0x00});
	f.drv.script.push_back({dap::Ack::Ok, 0x10});
	auto r = f.run({0x05, 0x00, 0x01, 0x12, 0x42, 0x00, 0x00, 0x00});
	const uint8_t want[] = {0x05, 0x00,
				static_cast<uint8_t>(dap::kXferOk | dap::kXferMismatch)};
	CHECK_BYTES(r, want);
}

TEST(transfer_disconnected_dummy_response)
{
	MockSwdDriver drv;
	MockHostStatus host;
	dap::Config cfg;
	dap::DapEngine engine{drv, host, cfg}; /* never connected */
	uint8_t resp[16];
	const uint8_t req[] = {0x05, 0x00, 0x01, 0x02};
	size_t n = engine.process(req, resp);
	const uint8_t want[] = {0x05, 0x00, 0x00};
	CHECK_BYTES(std::span<const uint8_t>(resp, n), want);
	CHECK(drv.transfers.empty());
}

TEST(transfer_disconnected_stays_framed_in_batch)
{
	/* Disconnected DAP_Transfer (1 write) inside ExecuteCommands, followed
	 * by Delay — the dummy transfer must consume only its own bytes so the
	 * Delay still runs (DAP_Dummy_Transfer parity, DAP.c:1200-1231). */
	MockSwdDriver drv;
	MockHostStatus host;
	dap::Config cfg;
	dap::DapEngine engine{drv, host, cfg}; /* never connected */
	uint8_t resp[64] = {};
	/* batch count=2: Transfer{idx0,count1,write addr4 val 0x11223344}, Delay(50us) */
	const uint8_t req[] = {0x7F, 0x02,
			       0x05, 0x00, 0x01, 0x04, 0x44, 0x33, 0x22, 0x11,
			       0x09, 0x32, 0x00};
	size_t n = engine.process(req, resp);
	const uint8_t want[] = {0x7F, 0x02,
				0x05, 0x00, 0x00,         /* dummy transfer */
				0x09, dap::kDapOk};        /* delay ran */
	CHECK_BYTES(std::span<const uint8_t>(resp, n), want);
	CHECK_EQ(drv.delays_us, 50u);
}

TEST(transfer_truncated_request_no_crash)
{
	Fixture f;
	/* claims 2 transfers, provides 1 */
	auto r = f.run({0x05, 0x00, 0x02, 0x02});
	CHECK(r.size() >= 3);
	if (!r.empty()) { /* crash-guard around empty-vector indexing */
		CHECK_EQ(r[0], 0x05);
	}
	/* must not have read past the buffer; count reflects completed ones */
}

TEST(transfer_dp_read_with_timestamp_bit)
{
	Fixture f;
	f.drv.timestamp_value = 0x11223344;
	f.drv.script.push_back({dap::Ack::Ok, 0xCAFEBABE});
	/* count=1: DP read addr 0 with TIMESTAMP bit (RnW|Timestamp = 0x82).
	 * DP read path (transfer.cpp:288-304): transfer_retry fills data, then
	 * timestamp is stored BEFORE data (DAP.c:778-786 / :798-806). */
	auto r = f.run({0x05, 0x00, 0x01,
			static_cast<uint8_t>(dap::kReqRnW | dap::kReqTimestamp)});
	/* response: cmd(0x05), count(1), response_value(kXferOk),
	 * timestamp(4 LE) = 0x11223344, data(4 LE) = 0xCAFEBABE.
	 * Timestamp precedes data per DAP.c:778-786. */
	const uint8_t want[] = {0x05, 0x01, dap::kXferOk,
				0x44, 0x33, 0x22, 0x11,
				0xBE, 0xBA, 0xFE, 0xCA};
	CHECK_BYTES(r, want);
	CHECK_EQ(f.drv.transfers.size(), 1u); /* no RDBUFF; DP read is synchronous */
}
