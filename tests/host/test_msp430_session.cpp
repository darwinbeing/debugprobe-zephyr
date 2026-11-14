#include "testing.hpp"
#include "fake_tap.hpp"
#include "debug_logs.hpp"
#include "msp430/session.hpp"

#include <utility>
#include <vector>

using msp430::State;
using dbglog::Log;

namespace {

using Bytes = std::vector<uint8_t>;

struct Fx {
	FakeTap tap;
	msp430::Cpuxv2 cpu{tap};
	msp430::Debugger dbg{tap, cpu};
	msp430::Session s{cpu, dbg, 1016};

	/* Whatever the Session does, the fake's mailbox, EEM exchange, password
	 * and memory-access rules must hold and the CPU is fed only the
	 * instructions FakeTap allows. flash_write_attempt is not checked here:
	 * Phase A's session_range_rules writes 0xFFFFE on purpose; every test of
	 * a debug session (test_msp430_session_debug.cpp) checks it too. */
	~Fx() { CHECK_LOG(faults(), Log{}); }

	Log faults() const
	{
		const std::pair<bool, const char *> flags[] = {
			{tap.jmb_protocol_error, "jmb_protocol_error"},
			{tap.eem_protocol_error, "eem_protocol_error"},
			{tap.password_violation, "password_violation"},
			{tap.mem_protocol_error, "mem_protocol_error"},
			{tap.unexpected_instruction, "unexpected_instruction"},
		};
		Log set;
		for (const auto &flag : flags) {
			if (flag.first) {
				set.push_back(flag.second);
			}
		}
		return set;
	}

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
const Bytes kTargetError = {0x90, 0x06};
const Bytes kTimeout = {0x90, 0x07};
const Bytes kIdentity = {0x90, 0x00, 0x91, 0x06, 0x01, 0x00, 0x1A, 0x00, 0x00, 0x55, 0x29};

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
	/* version 2, state Idle, max_data_bytes 1016 = 0x03F8, features bits 0
	 * (flash) and 1 (debugging), 8 hardware breakpoints */
	const Bytes want = {0x90, 0x00, 0x02, 0x00, 0xF8, 0x03, 0x03, 0x00, 0x00, 0x00, 0x08};
	CHECK_BYTES(f.run({0x90, 0x00}), want);
}

TEST(session_connect_returns_identity)
{
	Fx f;
	f.tap.quick_words = {0x2955};
	CHECK_BYTES(f.run({0x90, 0x01, 0xFA, 0x00, 0x00}), kIdentity);
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

namespace {

void le32(Bytes &b, uint32_t v)
{
	for (int i = 0; i < 4; ++i) {
		b.push_back(static_cast<uint8_t>(v >> (8 * i)));
	}
}

Bytes read_req(uint32_t addr, uint16_t nwords)
{
	Bytes b = {0x90, 0x03};
	le32(b, addr);
	b.push_back(static_cast<uint8_t>(nwords));
	b.push_back(static_cast<uint8_t>(nwords >> 8));
	return b;
}

Bytes write_req(uint32_t addr, size_t nbytes)
{
	Bytes b = {0x90, 0x04};
	le32(b, addr);
	for (size_t i = 0; i < nbytes; ++i) {
		b.push_back(static_cast<uint8_t>(i));
	}
	return b;
}

Bytes erase_req(uint8_t mode, uint32_t addr, uint32_t ram)
{
	Bytes b = {0x90, 0x05, mode};
	le32(b, addr);
	le32(b, ram);
	return b;
}

Bytes begin_req(uint32_t addr, uint32_t nwords, uint32_t ram)
{
	Bytes b = {0x90, 0x06};
	le32(b, addr);
	le32(b, nwords);
	le32(b, ram);
	return b;
}

Bytes data_req(size_t nbytes)
{
	Bytes b = {0x90, 0x07};
	for (size_t i = 0; i < nbytes; ++i) {
		b.push_back(static_cast<uint8_t>(i));
	}
	return b;
}

const Bytes kConnectReq = {0x90, 0x01, 0xFA, 0x00, 0x00};

size_t count_wipes(const FakeTap &t)
{
	size_t n = 0;
	for (const auto &w : t.mem_writes) {
		if (w.second == 0x3FFF) {
			++n;
		}
	}
	return n;
}

bool log_has(const FakeTap &t, const char *entry)
{
	for (const auto &l : t.log) {
		if (l == entry) {
			return true;
		}
	}
	return false;
}

} // namespace

TEST(session_ops_need_a_connection)
{
	Fx f;
	CHECK_BYTES(f.run(read_req(0x4400, 1)), kBadState);
	CHECK_BYTES(f.run(write_req(0x2400, 2)), kBadState);
	CHECK_BYTES(f.run(erase_req(2, 0x4400, 0x2400)), kBadState);
	CHECK_BYTES(f.run(begin_req(0x4400, 2, 0x2400)), kBadState);
	CHECK_BYTES(f.run(data_req(2)), kBadState);
	CHECK_BYTES(f.run({0x90, 0x08}), kBadState);
	CHECK(f.tap.log.empty());
	CHECK(f.s.state() == State::Idle);
}

TEST(session_small_response_buffers)
{
	Fx f;
	uint8_t r10[10] = {};
	CHECK_EQ(f.s.handle(Bytes{0x90, 0x00}, r10), 2u);
	CHECK_EQ(r10[1], 0x01);
	uint8_t r11[11] = {};
	CHECK_EQ(f.s.handle(Bytes{0x90, 0x00}, r11), 11u);
	CHECK_EQ(r11[1], 0x00);

	uint8_t c10[10] = {};
	CHECK_EQ(f.s.handle(kConnectReq, c10), 2u);
	CHECK_EQ(c10[1], 0x01);
	CHECK(f.tap.log.empty());
	uint8_t c11[11] = {};
	f.tap.quick_words = {0x2955};
	CHECK_EQ(f.s.handle(kConnectReq, c11), 11u);
	CHECK_EQ(c11[1], 0x00);
}

TEST(session_requests_one_byte_short)
{
	Fx f;
	f.connect();
	CHECK_BYTES(f.run({0x90, 0x03, 0x00, 0x44, 0x00, 0x00, 0x01}), kBadArg);
	CHECK_BYTES(f.run({0x90, 0x04, 0x00, 0x24, 0x00}), kBadArg);
	CHECK_BYTES(f.run({0x90, 0x05, 0x02, 0x00, 0x44, 0x00, 0x00, 0x00, 0x24, 0x00}), kBadArg);
	CHECK_BYTES(f.run({0x90, 0x06, 0x00, 0x44, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x24, 0x00}),
		    kBadArg);
	CHECK(f.tap.log.empty());
}

TEST(session_trailing_bytes_are_rejected)
{
	Fx f;
	CHECK_BYTES(f.run({0x90, 0x00, 0x00}), kBadArg);
	CHECK_BYTES(f.run({0x90, 0x01, 0xFA, 0x00, 0x00, 0x00}), kBadArg);
	CHECK(f.tap.log.empty());
	f.connect();
	Bytes r = read_req(0x4400, 1);
	r.push_back(0);
	CHECK_BYTES(f.run(r), kBadArg);
	Bytes e = erase_req(2, 0x4400, 0x2400);
	e.push_back(0);
	CHECK_BYTES(f.run(e), kBadArg);
	Bytes b = begin_req(0x4400, 2, 0x2400);
	b.push_back(0);
	CHECK_BYTES(f.run(b), kBadArg);
	CHECK_BYTES(f.run({0x90, 0x02, 0x00}), kBadArg);
	CHECK(f.s.state() == State::Connected);
	CHECK(f.tap.log.empty());

	f.tap.push_jmb_out(0xABADBABE);
	CHECK_BYTES(f.run(begin_req(0x4400, 2, 0x2400)), kOk);
	CHECK_BYTES(f.run({0x90, 0x08, 0x00}), kBadArg);
	CHECK(f.s.state() == State::Flashing);
}

TEST(session_range_rules)
{
	Fx f;
	f.connect();
	CHECK_BYTES(f.run(write_req(0x2401, 2)), kBadArg);
	CHECK_BYTES(f.run(write_req(0xFFFFE, 4)), kBadArg);
	CHECK(f.tap.log.empty());
	CHECK_BYTES(f.run({0x90, 0x04, 0xFE, 0xFF, 0x0F, 0x00, 0x34, 0x12}), kOk);
	REQUIRE(!f.tap.mem_writes.empty());
	CHECK_EQ(f.tap.mem_writes.back().first, 0xFFFFEu);
	CHECK_EQ(f.tap.mem_writes.back().second, 0x1234);
	f.tap.log.clear();
	/* the last word can be read; the fake's quick read returns what the
	 * write above stored there (on the chip a flash word would not change) */
	CHECK_BYTES(f.run(read_req(0xFFFFE, 1)), (Bytes{0x90, 0x00, 0x34, 0x12}));
	f.tap.log.clear();

	CHECK_BYTES(f.run(erase_req(0, 0x4401, 0x2400)), kBadArg);
	CHECK_BYTES(f.run(erase_req(0, 0x100000, 0x2400)), kBadArg);
	CHECK_BYTES(f.run(erase_req(0, 0x4400, 0x2401)), kBadArg);
	CHECK_BYTES(f.run(erase_req(0, 0x4400, 0xFFFFE)), kBadArg);
	CHECK_BYTES(f.run(erase_req(0, 0x4400, 0xFFF50)), kBadArg);
	CHECK(f.tap.log.empty());
	f.tap.push_jmb_out(0xCAFEBABE);
	CHECK_BYTES(f.run(erase_req(0, 0x4400, 0xFFF4E)), kOk);
	f.tap.log.clear();

	CHECK_BYTES(f.run(begin_req(0x4401, 2, 0x2400)), kBadArg);
	CHECK_BYTES(f.run(begin_req(0, 0x80000000u, 0x2400)), kBadArg);
	CHECK_BYTES(f.run(begin_req(0xFFFFE, 2, 0x2400)), kBadArg);
	CHECK_BYTES(f.run(begin_req(0x4400, 2, 0xFFFFE)), kBadArg);
	CHECK_BYTES(f.run(begin_req(0x4400, 2, 0xFFF12)), kBadArg);
	CHECK(f.tap.log.empty());
	f.tap.push_jmb_out(0xABADBABE);
	CHECK_BYTES(f.run(begin_req(0x4400, 2, 0xFFF10)), kOk);
	CHECK(f.s.state() == State::Flashing);
}

TEST(session_data_size_limits)
{
	Fx f;
	f.connect();
	const Bytes rd = f.run(read_req(0x4400, 508));
	REQUIRE(rd.size() == 2 + 1016u);
	CHECK_EQ(rd[1], 0x00);
	CHECK_BYTES(f.run(write_req(0x2400, 1016)), kOk);
	CHECK_EQ(f.tap.mem_writes.size(), 508u);
	CHECK_BYTES(f.run(write_req(0x2400, 1018)), kBadArg);

	f.tap.push_jmb_out(0xABADBABE);
	CHECK_BYTES(f.run(begin_req(0x4400, 2000, 0x2400)), kOk);
	CHECK_BYTES(f.run(data_req(1016)), kOk);
	CHECK(f.s.state() == State::Flashing);
	CHECK_BYTES(f.run(data_req(1018)), kBadArg);
	CHECK(f.s.state() == State::Connected);
}

TEST(session_flash_data_framing_errors_abandon_the_write)
{
	Fx f;
	f.connect();
	f.tap.push_jmb_out(0xABADBABE);
	CHECK_BYTES(f.run(begin_req(0x4400, 2, 0x2400)), kOk);
	f.tap.mem_writes.clear();
	CHECK_BYTES(f.run(data_req(0)), kBadArg);
	CHECK(f.s.state() == State::Connected);
	CHECK_EQ(count_wipes(f.tap), 120u);

	f.tap.push_jmb_out(0xABADBABE);
	CHECK_BYTES(f.run(begin_req(0x4400, 2, 0x2400)), kOk);
	CHECK_BYTES(f.run(data_req(3)), kBadArg);
	CHECK(f.s.state() == State::Connected);
}

TEST(session_flash_data_timeout_returns_to_connected)
{
	Fx f;
	f.connect();
	f.tap.push_jmb_out(0xABADBABE);
	CHECK_BYTES(f.run(begin_req(0x4400, 2, 0x2400)), kOk);
	f.tap.jmb_in_ready = false;
	CHECK_BYTES(f.run(data_req(2)), kTimeout);
	CHECK(f.s.state() == State::Connected);
	CHECK_BYTES(f.run({0x90, 0x08}), kBadState);
}

TEST(session_closing_while_flashing_abandons_the_write)
{
	for (int how = 0; how < 3; ++how) {
		Fx f;
		f.connect();
		f.tap.push_jmb_out(0xABADBABE);
		CHECK_BYTES(f.run(begin_req(0x4400, 2, 0x2400)), kOk);
		f.tap.mem_writes.clear();
		if (how == 0) {
			f.s.abort();
			CHECK(f.s.state() == State::Idle);
		} else if (how == 1) {
			CHECK_BYTES(f.run({0x90, 0x02}), kOk);
			CHECK(f.s.state() == State::Idle);
		} else {
			f.tap.quick_words = {0x2955};
			CHECK_BYTES(f.run(kConnectReq), kIdentity);
			CHECK(f.s.state() == State::Connected);
		}
		CHECK_EQ(count_wipes(f.tap), 120u);
		CHECK(log_has(f.tap, "close"));
	}
}

TEST(session_failed_cleanup_is_a_target_error)
{
	{
		Fx f;
		f.connect();
		f.tap.has_cntrl_sig_after_release = true;
		f.tap.cntrl_sig_after_release = 0x0101;
		f.tap.push_jmb_out(0xABADBABE);
		CHECK_BYTES(f.run(begin_req(0x4400, 1, 0x2400)), kOk);
		CHECK_BYTES(f.run(data_req(4)), kTargetError);
		CHECK(f.s.state() == State::Idle);
	}
	{
		Fx f;
		f.connect();
		f.tap.has_cntrl_sig_after_release = true;
		f.tap.cntrl_sig_after_release = 0x0101;
		f.tap.push_jmb_out(0xABADBABE);
		CHECK_BYTES(f.run(begin_req(0x4400, 2, 0x2400)), kOk);
		CHECK_BYTES(f.run(data_req(2)), kOk);
		CHECK_BYTES(f.run({0x90, 0x08}), kTargetError);
		CHECK(f.s.state() == State::Idle);
	}
	{
		Fx f;
		f.connect();
		f.tap.has_cntrl_sig_after_release = true;
		f.tap.cntrl_sig_after_release = 0x0101;
		CHECK_BYTES(f.run(erase_req(2, 0x4400, 0x2400)), kTargetError);
		CHECK(f.s.state() == State::Idle);
	}
}

TEST(session_connect_speed_bounds)
{
	{
		Fx f;
		f.tap.quick_words = {0x2955};
		CHECK_BYTES(f.run({0x90, 0x01, 0x64, 0x00, 0x00}), kIdentity);
		CHECK(f.s.state() == State::Connected);
		REQUIRE(!f.tap.log.empty());
		CHECK(f.tap.log.front() == "speed:100000");
	}
	{
		Fx f;
		f.tap.quick_words = {0x2955};
		CHECK_BYTES(f.run({0x90, 0x01, 0xE8, 0x03, 0x00}), kIdentity);
		CHECK(f.s.state() == State::Connected);
		REQUIRE(!f.tap.log.empty());
		CHECK(f.tap.log.front() == "speed:1000000");
	}
}

TEST(session_state_error_wins_over_size_error)
{
	Fx f;
	CHECK_BYTES(f.run({0x90, 0x02, 0x00}), kBadState);
	Bytes r = read_req(0x4400, 1);
	r.push_back(0);
	CHECK_BYTES(f.run(r), kBadState);
	f.connect();
	CHECK_BYTES(f.run({0x90, 0x08, 0x00}), kBadState);
}

TEST(session_read_mem_reports_a_dead_link)
{
	Fx f;
	f.connect();
	f.tap.cntrl_sig = 0x0000;
	f.tap.cntrl_sig_script = {0x0301};
	CHECK_BYTES(f.run({0x90, 0x03, 0x00, 0x44, 0x00, 0x00, 0x02, 0x00}), kTargetError);
	CHECK(f.s.state() == State::Idle);
}

TEST(session_read_mem_reports_a_floating_data_line)
{
	Fx f;
	f.connect();
	f.tap.ones_after_shifts = f.tap.shifts() + 30;
	CHECK_BYTES(f.run({0x90, 0x03, 0x00, 0x44, 0x00, 0x00, 0x40, 0x00}), kTargetError);
	CHECK(f.s.state() == State::Idle);
}
