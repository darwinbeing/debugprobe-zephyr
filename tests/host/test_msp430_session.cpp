#include "testing.hpp"
#include "fake_tap.hpp"
#include "msp430/session.hpp"

#include <vector>

using msp430::State;

namespace {

using Bytes = std::vector<uint8_t>;

struct Fx {
	FakeTap tap;
	msp430::Cpuxv2 cpu{tap};
	msp430::Session s{cpu, 1016};

	Bytes run(const Bytes &req)
	{
		uint8_t resp[1024] = {};
		const size_t n = s.handle(req, resp);
		return {resp, resp + n};
	}

	/* CONNECT at 250 kHz, normal entry; then forget the transcript. */
	void connect()
	{
		tap.quick_words = {0x2955};
		run({0x90, 0x01, 0xFA, 0x00, 0x00});
		tap.log.clear();
		tap.mem_writes.clear();
	}
};

const Bytes kBadArg = {0x90, 0x01};
const Bytes kBadState = {0x90, 0x02};
const Bytes kOk = {0x90, 0x00};

} // namespace

TEST(session_declines_other_vendor_ids)
{
	Fx f;
	CHECK_EQ(f.run({0x85, 0x01}).size(), 0u);
	CHECK_EQ(f.run({}).size(), 0u);
}

TEST(session_truncated_and_unknown_ops)
{
	Fx f;
	CHECK_BYTES(f.run({0x90}), kBadArg);
	const Bytes unknown = {0x90, 0x08};
	CHECK_BYTES(f.run({0x90, 0x7F}), unknown);
}

TEST(session_info_reports_version_state_limits)
{
	Fx f;
	/* version 1, state Idle, max_data_bytes 1016 = 0x03F8, features bit 0 */
	const Bytes want = {0x90, 0x00, 0x01, 0x00, 0xF8, 0x03, 0x01, 0x00, 0x00, 0x00};
	CHECK_BYTES(f.run({0x90, 0x00}), want);
}

TEST(session_connect_returns_identity)
{
	Fx f;
	f.tap.quick_words = {0x2955};
	const Bytes want = {0x90, 0x00, 0x91, 0x06, 0x01, 0x00, 0x1A, 0x00, 0x00, 0x55, 0x29};
	CHECK_BYTES(f.run({0x90, 0x01, 0xFA, 0x00, 0x00}), want);
	CHECK(f.s.state() == State::Connected);
	REQUIRE(!f.tap.log.empty());
	CHECK(f.tap.log.front() == "speed:250000");
}

TEST(session_connect_validates_arguments)
{
	Fx f;
	CHECK_BYTES(f.run({0x90, 0x01, 0x63, 0x00, 0x00}), kBadArg); /* 99 kHz */
	CHECK_BYTES(f.run({0x90, 0x01, 0xE9, 0x03, 0x00}), kBadArg); /* 1001 kHz */
	CHECK_BYTES(f.run({0x90, 0x01, 0xFA, 0x00, 0x02}), kBadArg); /* entry mode 2 */
	CHECK_BYTES(f.run({0x90, 0x01, 0xFA, 0x00}), kBadArg);       /* truncated */
	CHECK(f.tap.log.empty());
	CHECK(f.s.state() == State::Idle);
}

TEST(session_connect_no_target_carries_the_id)
{
	Fx f;
	f.tap.jtag_id = 0xFF;
	const Bytes want = {0x90, 0x04, 0xFF};
	CHECK_BYTES(f.run({0x90, 0x01, 0xFA, 0x00, 0x00}), want);
	CHECK(f.s.state() == State::Idle);
}

TEST(session_connect_while_connected_closes_first)
{
	Fx f;
	f.connect();
	f.tap.quick_words = {0x2955};
	f.run({0x90, 0x01, 0xFA, 0x00, 0x00});
	const std::vector<std::string> head = {"ir:c8", "dr16:0c01", "dr16:0401",
					       "ir:a8", "close", "speed:250000"};
	REQUIRE(f.tap.log.size() > head.size());
	CHECK(std::vector<std::string>(f.tap.log.begin(), f.tap.log.begin() + 6) == head);
	CHECK(f.s.state() == State::Connected);
}

TEST(session_disconnect)
{
	Fx f;
	CHECK_BYTES(f.run({0x90, 0x02}), kBadState);
	f.connect();
	CHECK_BYTES(f.run({0x90, 0x02}), kOk);
	CHECK(f.s.state() == State::Idle);
	REQUIRE(!f.tap.log.empty());
	CHECK(f.tap.log.back() == "close");
}

TEST(session_read_mem)
{
	Fx f;
	f.connect();
	f.tap.quick_words = {0x1234, 0xABCD};
	const Bytes want = {0x90, 0x00, 0x34, 0x12, 0xCD, 0xAB};
	CHECK_BYTES(f.run({0x90, 0x03, 0x00, 0x44, 0x00, 0x00, 0x02, 0x00}), want);
}

TEST(session_read_mem_validates_arguments)
{
	Fx f;
	CHECK_BYTES(f.run({0x90, 0x03, 0x00, 0x44, 0x00, 0x00, 0x01, 0x00}), kBadState);
	f.connect();
	CHECK_BYTES(f.run({0x90, 0x03, 0x01, 0x44, 0x00, 0x00, 0x01, 0x00}), kBadArg); /* odd */
	CHECK_BYTES(f.run({0x90, 0x03, 0x00, 0x44, 0x00, 0x00, 0x00, 0x00}), kBadArg); /* 0 words */
	CHECK_BYTES(f.run({0x90, 0x03, 0xFE, 0xFF, 0x0F, 0x00, 0x02, 0x00}), kBadArg); /* past 1 MB */
	CHECK_BYTES(f.run({0x90, 0x03, 0x00, 0x44, 0x00, 0x00, 0xFD, 0x01}), kBadArg); /* 509 words */
	CHECK_BYTES(f.run({0x90, 0x03, 0x00, 0x44, 0x00}), kBadArg);                   /* truncated */
	CHECK(f.tap.log.empty());
}

TEST(session_write_mem)
{
	Fx f;
	f.connect();
	CHECK_BYTES(f.run({0x90, 0x04, 0x00, 0x24, 0x00, 0x00, 0x34, 0x12}), kOk);
	REQUIRE(f.tap.mem_writes.size() == 1u);
	CHECK_EQ(f.tap.mem_writes[0].first, 0x2400u);
	CHECK_EQ(f.tap.mem_writes[0].second, 0x1234);
	CHECK_BYTES(f.run({0x90, 0x04, 0x00, 0x24, 0x00, 0x00, 0x34}), kBadArg); /* half a word */
	CHECK_BYTES(f.run({0x90, 0x04, 0x00, 0x24, 0x00, 0x00}), kBadArg);       /* no data */
}

TEST(session_erase)
{
	Fx f;
	f.connect();
	f.tap.push_jmb_out(0xCAFEBABE);
	/* mode 2 (main), address 0x4400, funclet RAM 0x2400 */
	CHECK_BYTES(f.run({0x90, 0x05, 0x02, 0x00, 0x44, 0x00, 0x00, 0x00, 0x24, 0x00, 0x00}), kOk);
	REQUIRE(f.tap.mem_writes.size() > 4);
	CHECK_EQ(f.tap.mem_writes[4].second, 0xA506);

	CHECK_BYTES(f.run({0x90, 0x05, 0x03, 0x00, 0x44, 0x00, 0x00, 0x00, 0x24, 0x00, 0x00}), kBadArg);
	CHECK_BYTES(f.run({0x90, 0x05, 0x02, 0x00, 0x44, 0x00, 0x00}), kBadArg);

	const Bytes timeout = {0x90, 0x07};
	CHECK_BYTES(f.run({0x90, 0x05, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00, 0x24, 0x00, 0x00}), timeout);
	CHECK(f.s.state() == State::Connected);
}

TEST(session_flash_flow)
{
	Fx f;
	f.connect();
	f.tap.push_jmb_out(0xABADBABE);
	/* address 0x4400, 2 words, funclet RAM 0x2400 */
	CHECK_BYTES(f.run({0x90, 0x06, 0x00, 0x44, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
			   0x00, 0x24, 0x00, 0x00}),
		    kOk);
	CHECK(f.s.state() == State::Flashing);
	const Bytes info = f.run({0x90, 0x00});
	REQUIRE(info.size() > 3);
	CHECK_EQ(info[3], 2); /* INFO reports Flashing */

	CHECK_BYTES(f.run({0x90, 0x07, 0x34, 0x12}), kOk);
	CHECK_BYTES(f.run({0x90, 0x07, 0x78, 0x56}), kOk);

	f.tap.push_jmb_out(0xCAFEBABE);
	CHECK_BYTES(f.run({0x90, 0x08}), kOk);
	CHECK(f.s.state() == State::Connected);
}

TEST(session_flash_state_and_argument_errors)
{
	Fx f;
	f.connect();
	CHECK_BYTES(f.run({0x90, 0x07, 0x34, 0x12}), kBadState); /* data without begin */
	CHECK_BYTES(f.run({0x90, 0x08}), kBadState);             /* end without begin */
	/* zero words */
	CHECK_BYTES(f.run({0x90, 0x06, 0x00, 0x44, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			   0x00, 0x24, 0x00, 0x00}),
		    kBadArg);
	CHECK(f.s.state() == State::Connected);
}

TEST(session_flash_data_overrun_aborts_the_write)
{
	Fx f;
	f.connect();
	f.tap.push_jmb_out(0xABADBABE);
	f.run({0x90, 0x06, 0x00, 0x44, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x24, 0x00, 0x00});
	const size_t writes = f.tap.mem_writes.size();
	CHECK_BYTES(f.run({0x90, 0x07, 0x34, 0x12, 0x78, 0x56}), kBadArg); /* 2 words, 1 announced */
	CHECK(f.s.state() == State::Connected);
	CHECK(f.tap.mem_writes.size() > writes); /* funclet RAM was wiped */
}

TEST(session_flash_end_early_is_bad_state)
{
	Fx f;
	f.connect();
	f.tap.push_jmb_out(0xABADBABE);
	f.run({0x90, 0x06, 0x00, 0x44, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x24, 0x00, 0x00});
	f.run({0x90, 0x07, 0x34, 0x12});
	CHECK_BYTES(f.run({0x90, 0x08}), kBadState);
	CHECK(f.s.state() == State::Connected);
}

TEST(session_other_ops_refused_while_flashing)
{
	Fx f;
	f.connect();
	f.tap.push_jmb_out(0xABADBABE);
	f.run({0x90, 0x06, 0x00, 0x44, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x24, 0x00, 0x00});
	CHECK_BYTES(f.run({0x90, 0x03, 0x00, 0x44, 0x00, 0x00, 0x01, 0x00}), kBadState);
	CHECK_BYTES(f.run({0x90, 0x05, 0x02, 0x00, 0x44, 0x00, 0x00, 0x00, 0x24, 0x00, 0x00}), kBadState);
	CHECK(f.s.state() == State::Flashing);
}

TEST(session_abort)
{
	Fx f;
	f.s.abort(); /* idle: nothing happens */
	CHECK(f.tap.log.empty());

	f.connect();
	f.tap.push_jmb_out(0xABADBABE);
	f.run({0x90, 0x06, 0x00, 0x44, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x24, 0x00, 0x00});
	f.s.abort();
	CHECK(f.s.state() == State::Idle);
	REQUIRE(!f.tap.log.empty());
	CHECK(f.tap.log.back() == "close");
}

TEST(session_response_buffer_too_small)
{
	Fx f;
	f.connect();
	f.tap.quick_words = {0x1234, 0xABCD};
	uint8_t resp[5] = {}; /* room for the header and one word only */
	const Bytes req = {0x90, 0x03, 0x00, 0x44, 0x00, 0x00, 0x02, 0x00};
	const size_t n = f.s.handle(req, resp);
	CHECK_EQ(n, 2u);
	CHECK_EQ(resp[1], 0x01); /* BAD_ARG, nothing written past the header */

	uint8_t tiny[1] = {};
	CHECK_EQ(f.s.handle(req, tiny), 0u);
}

TEST(session_target_error_closes_the_session)
{
	Fx f;
	f.connect();
	f.tap.cntrl_sig = 0x0000; /* CPU no longer in full-emulation state */
	const Bytes target_error = {0x90, 0x06};
	CHECK_BYTES(f.run({0x90, 0x03, 0x00, 0x44, 0x00, 0x00, 0x01, 0x00}), target_error);
	CHECK(f.s.state() == State::Idle);
	REQUIRE(!f.tap.log.empty());
	CHECK(f.tap.log.back() == "close"); /* pins released */
	CHECK_BYTES(f.run({0x90, 0x03, 0x00, 0x44, 0x00, 0x00, 0x01, 0x00}), kBadState);
}

TEST(session_erase_cleanup_failure_closes_the_session)
{
	Fx f;
	f.connect();
	f.tap.push_jmb_out(0xCAFEBABE);
	f.tap.has_cntrl_sig_after_release = true;
	f.tap.cntrl_sig_after_release = 0x0101; /* sync never returns after the funclet ran */
	const Bytes target_error = {0x90, 0x06};
	CHECK_BYTES(f.run({0x90, 0x05, 0x02, 0x00, 0x44, 0x00, 0x00, 0x00, 0x24, 0x00, 0x00}),
		    target_error);
	CHECK(f.s.state() == State::Idle);
}
