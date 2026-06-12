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
	Fixture()
	{
		uint8_t resp[8];
		const uint8_t req[] = {0x02, dap::kPortSwd};
		engine.process(req, resp);
		drv.events.clear();
	}
	std::vector<uint8_t> run(std::vector<uint8_t> req)
	{
		uint8_t resp[512] = {};
		size_t n = engine.process({req.data(), req.size()}, resp);
		return {resp, resp + n};
	}
};
} // namespace

TEST(block_write_two_words)
{
	Fixture f;
	auto r = f.run({0x06, 0x00, 0x02, 0x00, 0x04, /* DP write addr 4 */
			0x11, 0x00, 0x00, 0x00,
			0x22, 0x00, 0x00, 0x00});
	const uint8_t want[] = {0x06, 0x02, 0x00, dap::kXferOk};
	CHECK_BYTES(r, want);
	/* 2 writes + 1 unconditional trailing "check last write" RDBUFF read
	 * (DAP.c:1344-1348) = 3 SWD transfers. */
	CHECK_EQ(f.drv.transfers.size(), 3u);
	CHECK_EQ(f.drv.transfers[0].write_data, 0x11u);
	CHECK_EQ(f.drv.transfers[1].write_data, 0x22u);
	CHECK_EQ(f.drv.transfers[2].request, dap::kReqReadRdbuff);
	CHECK(f.drv.transfers[2].data_null);
}

TEST(block_read_dp_two_words)
{
	Fixture f;
	f.drv.script.push_back({dap::Ack::Ok, 0x000000AA});
	f.drv.script.push_back({dap::Ack::Ok, 0x000000BB});
	auto r = f.run({0x06, 0x00, 0x02, 0x00, 0x06}); /* DP read addr 4 */
	const uint8_t want[] = {0x06, 0x02, 0x00, dap::kXferOk,
				0xAA, 0x00, 0x00, 0x00,
				0xBB, 0x00, 0x00, 0x00};
	CHECK_BYTES(r, want);
}

TEST(block_read_ap_uses_pipeline)
{
	Fixture f;
	f.drv.script.push_back({dap::Ack::Ok, 0});          /* post */
	f.drv.script.push_back({dap::Ack::Ok, 0x00000001}); /* word 1 (re-post) */
	f.drv.script.push_back({dap::Ack::Ok, 0x00000002}); /* RDBUFF (last) */
	auto r = f.run({0x06, 0x00, 0x02, 0x00, 0x07}); /* AP read addr 4 */
	const uint8_t want[] = {0x06, 0x02, 0x00, dap::kXferOk,
				0x01, 0x00, 0x00, 0x00,
				0x02, 0x00, 0x00, 0x00};
	CHECK_BYTES(r, want);
	CHECK_EQ(f.drv.transfers.size(), 3u);
	CHECK(f.drv.transfers[0].data_null);
	CHECK_EQ(f.drv.transfers[2].request, dap::kReqReadRdbuff);
}

TEST(block_fault_reports_partial_count)
{
	Fixture f;
	f.drv.script.push_back({dap::Ack::Ok, 0});
	f.drv.script.push_back({dap::Ack::Fault, 0});
	auto r = f.run({0x06, 0x00, 0x03, 0x00, 0x04,
			0x01, 0x00, 0x00, 0x00,
			0x02, 0x00, 0x00, 0x00,
			0x03, 0x00, 0x00, 0x00});
	const uint8_t want[] = {0x06, 0x01, 0x00, dap::kXferFault};
	CHECK_BYTES(r, want);
}

TEST(block_disconnected_dummy)
{
	MockSwdDriver drv;
	MockHostStatus host;
	dap::Config cfg;
	dap::DapEngine engine{drv, host, cfg};
	uint8_t resp[16];
	const uint8_t req[] = {0x06, 0x00, 0x02, 0x00, 0x06};
	size_t n = engine.process(req, resp);
	const uint8_t want[] = {0x06, 0x00, 0x00, 0x00};
	CHECK_BYTES(std::span<const uint8_t>(resp, n), want);
}

TEST(block_truncated_write_data_no_crash)
{
	Fixture f;
	auto r = f.run({0x06, 0x00, 0x02, 0x00, 0x04, 0x01, 0x00});
	CHECK(r.size() >= 4);
	CHECK_EQ(r[0], 0x06);
	CHECK(f.drv.transfers.size() <= 1);
}

TEST(transfer_abort_packet_sets_flag_async)
{
	/* The 0x07 packet is handled by the transport calling request_abort();
	 * the engine never sees it as a command. Simulate: set the flag, then
	 * run a long batch — it must stop after the first item. */
	Fixture f;
	f.engine.request_abort();
	f.drv.script.push_back({dap::Ack::Ok, 0});
	auto r = f.run({0x05, 0x00, 0x03,
			0x04, 0x01, 0x00, 0x00, 0x00,
			0x04, 0x02, 0x00, 0x00, 0x00,
			0x04, 0x03, 0x00, 0x00, 0x00});
	/* abort flag is cleared at entry per the reference (DAP.c:682), so all
	 * three run; now test the flag DURING a batch via a scripted hook. */
	CHECK_EQ(r[1], 3); /* pre-set flag is cleared at entry — parity check */
}

TEST(transfer_abort_mid_batch_stops)
{
	Fixture f;
	/* Abort after the first driver transfer completes: use the mock's
	 * script size as the trigger by aborting from a wrapper. */
	struct AbortingDriver : MockSwdDriver {
		dap::DapEngine *eng = nullptr;
		dap::Ack transfer(uint8_t req, uint32_t *data) override
		{
			dap::Ack a = MockSwdDriver::transfer(req, data);
			if (transfers.size() == 1 && eng) {
				eng->request_abort();
			}
			return a;
		}
	};
	AbortingDriver drv;
	MockHostStatus host;
	dap::Config cfg;
	dap::DapEngine engine{drv, host, cfg};
	drv.eng = &engine;
	uint8_t resp[64];
	const uint8_t conn[] = {0x02, dap::kPortSwd};
	engine.process(conn, resp);
	const uint8_t req[] = {0x05, 0x00, 0x03,
			       0x04, 0x01, 0x00, 0x00, 0x00,
			       0x04, 0x02, 0x00, 0x00, 0x00,
			       0x04, 0x03, 0x00, 0x00, 0x00};
	size_t n = engine.process(req, resp);
	CHECK_EQ(n, 3u);
	CHECK_EQ(resp[1], 1); /* one completed, then aborted */
	/* The write completes (transfer #1), the abort then breaks the loop, but
	 * cmd_transfer still issues its trailing check-write RDBUFF read
	 * (DAP.c:902-908, faithful Task-5 behavior) = transfer #2. */
	CHECK_EQ(drv.transfers.size(), 2u);
}

TEST(block_write_fault_stays_framed_in_batch)
{
	Fixture f; /* connected in ctor */
	f.drv.script.push_back({dap::Ack::Ok, 0});    /* write 1 ok */
	f.drv.script.push_back({dap::Ack::Fault, 0}); /* write 2 faults */
	/* batch: TransferBlock{write addr4, count=3, 3 data words}, then Delay(9us).
	 * The block faults at word 2 but must consume its full declared length
	 * (4 + 3*4 = 16 body bytes) so the Delay still runs. */
	uint8_t resp[64] = {};
	const uint8_t req[] = {0x7F, 0x02,
			       0x06, 0x00, 0x03, 0x00, 0x04,
			       0x01, 0x00, 0x00, 0x00,
			       0x02, 0x00, 0x00, 0x00,
			       0x03, 0x00, 0x00, 0x00,
			       0x09, 0x09, 0x00};
	size_t n = f.engine.process({req, sizeof(req)}, resp);
	const uint8_t want[] = {0x7F, 0x02,
				0x06, 0x01, 0x00, dap::kXferFault, /* 1 write done, fault */
				0x09, dap::kDapOk};
	CHECK_BYTES(std::span<const uint8_t>(resp, n), want);
	CHECK_EQ(f.drv.delays_us, 9u);
}
