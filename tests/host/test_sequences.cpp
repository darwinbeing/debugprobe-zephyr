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

TEST(swj_sequence_basic)
{
	Fixture f;
	/* 16 bits: 0xFF 0x07 (line reset fragment) */
	auto r = f.run({0x12, 16, 0xFF, 0x07});
	const uint8_t want[] = {0x12, dap::kDapOk};
	CHECK_BYTES(r, want);
	CHECK_EQ(f.drv.swj_bit_count, 16u);
	CHECK_EQ(f.drv.swj_bits.size(), 2u);
	if (!f.drv.swj_bits.empty()) {
		CHECK_EQ(f.drv.swj_bits[0], 0xFF);
	}
	if (f.drv.swj_bits.size() >= 2) {
		CHECK_EQ(f.drv.swj_bits[1], 0x07);
	}
}

TEST(swj_sequence_zero_means_256)
{
	Fixture f;
	std::vector<uint8_t> req = {0x12, 0};
	for (int i = 0; i < 32; ++i) {
		req.push_back(0xFF);
	}
	uint8_t resp[8];
	size_t n = f.engine.process({req.data(), req.size()}, resp);
	CHECK_EQ(n, 2u);
	CHECK_EQ(resp[1], dap::kDapOk);
	CHECK_EQ(f.drv.swj_bit_count, 256u);
}

TEST(swj_sequence_truncated_is_error)
{
	Fixture f;
	auto r = f.run({0x12, 16, 0xFF}); /* one data byte short */
	const uint8_t want[] = {0x12, dap::kDapError};
	CHECK_BYTES(r, want);
	CHECK_EQ(f.drv.swj_bit_count, 0u);
}

TEST(swd_configure_turnaround_dataphase)
{
	Fixture f;
	/* config 0x05: turnaround = (1)+1 = 2 cycles, data_phase = 1 */
	auto r = f.run({0x13, 0x05});
	const uint8_t want[] = {0x13, dap::kDapOk};
	CHECK_BYTES(r, want);
	CHECK_EQ(f.drv.last_turnaround, 2);
	CHECK(f.drv.last_data_phase);
}

TEST(swd_sequence_output_then_input)
{
	Fixture f;
	f.drv.swd_input_data = {0xAB};
	/* 2 sequences: output 8 cycles data 0x9E; input 8 cycles */
	auto r = f.run({0x1D, 0x02, 0x08, 0x9E, 0x88});
	const uint8_t want[] = {0x1D, dap::kDapOk, 0xAB};
	CHECK_BYTES(r, want);
	CHECK_EQ(f.drv.swj_bit_count, 8u); /* the output sequence */
}

TEST(swd_sequence_count_0_means_64)
{
	Fixture f;
	/* one output sequence, info 0x00 -> 64 cycles -> 8 data bytes */
	auto r = f.run({0x1D, 0x01, 0x00, 1, 2, 3, 4, 5, 6, 7, 8});
	const uint8_t want[] = {0x1D, dap::kDapOk};
	CHECK_BYTES(r, want);
	CHECK_EQ(f.drv.swj_bit_count, 64u);
}

TEST(swd_sequence_truncated_is_error)
{
	Fixture f;
	auto r = f.run({0x1D, 0x01, 0x08}); /* missing output byte */
	const uint8_t want[] = {0x1D, dap::kDapError};
	CHECK_BYTES(r, want);
}
