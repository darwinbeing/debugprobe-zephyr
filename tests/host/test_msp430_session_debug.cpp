#include "testing.hpp"
#include "fake_tap.hpp"
#include "debug_logs.hpp"
#include "msp430/session.hpp"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

using msp430::State;
using namespace dbglog;

namespace {

using Bytes = std::vector<uint8_t>;

const Bytes kOk = {0x90, 0x00};
const Bytes kBadArg = {0x90, 0x01};
const Bytes kBadState = {0x90, 0x02};
const Bytes kTargetError = {0x90, 0x06};
const Bytes kTimeout = {0x90, 0x07};
const Bytes kProtected = {0x90, 0x09};
const Bytes kRunning = {0x90, 0x0A};

const Bytes kConnectReq = {0x90, 0x01, 0xFA, 0x00, 0x00};

void le32(Bytes &b, uint32_t v)
{
	for (int i = 0; i < 4; ++i) {
		b.push_back(static_cast<uint8_t>(v >> (8 * i)));
	}
}

/* DBG_ATTACH: mode, flags, 250 kHz, the F5529's RAM */
Bytes attach_req(uint8_t mode = 0, uint8_t flags = 0, uint32_t ram_lo = 0x1C00,
		 uint32_t ram_hi = 0x4400, uint16_t khz = 250)
{
	Bytes b = {0x90, 0x09, mode, flags, static_cast<uint8_t>(khz), static_cast<uint8_t>(khz >> 8)};
	le32(b, ram_lo);
	le32(b, ram_hi);
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

Bytes read_req(uint32_t addr, uint16_t nwords)
{
	Bytes b = {0x90, 0x03};
	le32(b, addr);
	b.push_back(static_cast<uint8_t>(nwords));
	b.push_back(static_cast<uint8_t>(nwords >> 8));
	return b;
}

Bytes bp_req(uint8_t index, uint32_t addr)
{
	Bytes b = {0x90, 0x11, index};
	le32(b, addr);
	return b;
}

Bytes reg_req(uint8_t index, uint32_t value)
{
	Bytes b = {0x90, 0x10, index};
	le32(b, value);
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

bool log_has(const FakeTap &t, const char *entry)
{
	for (const auto &l : t.log) {
		if (l == entry) {
			return true;
		}
	}
	return false;
}

size_t count_log(const FakeTap &t, const char *entry)
{
	size_t n = 0;
	for (const auto &l : t.log) {
		if (l == entry) {
			++n;
		}
	}
	return n;
}

struct Fx {
	FakeTap tap;
	msp430::Cpuxv2 cpu{tap};
	msp430::Debugger dbg{tap, cpu};
	msp430::Session s{cpu, dbg, 1016};

	/* Every test here drives a debug session: whatever the Session does,
	 * nothing may be written to flash or the flash controller, the fake's
	 * mailbox, EEM exchange, password and memory-access rules must hold, and
	 * the CPU is fed only the instructions FakeTap allows. */
	~Fx() { CHECK_LOG(faults(), Log{}); }

	/* The names of the fault flags that are set. */
	Log faults() const
	{
		const std::pair<bool, const char *> flags[] = {
			{tap.jmb_protocol_error, "jmb_protocol_error"},
			{tap.eem_protocol_error, "eem_protocol_error"},
			{tap.password_violation, "password_violation"},
			{tap.mem_protocol_error, "mem_protocol_error"},
			{tap.flash_write_attempt, "flash_write_attempt"},
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

	/* The next sync finds a running CPU that the EEM stops at once. */
	void cpu_runs()
	{
		tap.eem_control_script = {0x0000, 0x0080};
		tap.captured = {0x0008, 0x0000};
	}

	/* DBG_ATTACH without reset; then forget the transcript. */
	void attach()
	{
		cpu_runs();
		REQUIRE(run(attach_req())[1] == 0x00);
		tap.log.clear();
		tap.mem_writes.clear();
	}

	/* DBG_RESUME; then forget the transcript. */
	void resume()
	{
		REQUIRE(run({0x90, 0x0C})[1] == 0x00);
		REQUIRE(s.state() == State::DebugRunning);
		tap.log.clear();
	}

	/* A debug session with breakpoint 5 at 0x4600, halted or running; then
	 * forget the transcript. */
	void open_session(bool running)
	{
		attach();
		REQUIRE(run(bp_req(5, 0x4600))[1] == 0x00);
		if (running) {
			resume();
		}
		tap.log.clear();
		tap.mem_writes.clear();
	}
};

} // namespace

TEST(session_dbg_attach_returns_identity_pc_and_breakpoints)
{
	Fx f;
	f.cpu_runs();
	const Bytes want = {0x90, 0x00,
			    0x91, 0x06, 0x01, 0x00, 0x1A, 0x00, 0x00, 0x55, 0x29, /* identity */
			    0x20, 0x45, 0x00, 0x00,                               /* PC 0x4520 */
			    0x01,                                                 /* stopped on request */
			    0x08};                                                /* 8 breakpoints */
	CHECK_BYTES(f.run(attach_req()), want);
	CHECK(f.s.state() == State::DebugHalted);
	const Bytes info = f.run({0x90, 0x00});
	REQUIRE(info.size() > 3u);
	CHECK_EQ(info[3], 3); /* INFO reports DebugHalted */
	CHECK(!log_has(f.tap, "dr16:0c01")); /* no reset */
}

TEST(session_dbg_attach_with_reset)
{
	Fx f;
	const Bytes r = f.run(attach_req(1));
	REQUIRE(r.size() == 17u);
	CHECK_EQ(r[1], 0x00);
	CHECK_EQ(r[11] | (r[12] << 8), 0x441A); /* reset vector */
	CHECK_EQ(r[13] | (r[14] << 8), 0);
	CHECK_EQ(r[15], 4);                      /* stop reason: reset */
	CHECK_EQ(r[16], 8);
	CHECK(log_has(f.tap, "dr16:0c01"));
	CHECK(f.s.state() == State::DebugHalted);
}

TEST(session_dbg_attach_validates_arguments)
{
	Fx f;
	CHECK_BYTES(f.run(attach_req(2)), kBadArg);                          /* mode */
	CHECK_BYTES(f.run(attach_req(0, 1)), kBadArg);                       /* full mode: B2 */
	CHECK_BYTES(f.run(attach_req(0, 2)), kBadArg);                       /* unknown flag */
	CHECK_BYTES(f.run(attach_req(0, 0x80)), kBadArg);                    /* unknown flag */
	CHECK_BYTES(f.run(attach_req(0, 0, 0x1C01)), kBadArg);               /* odd RAM start */
	CHECK_BYTES(f.run(attach_req(0, 0, 0x4400, 0x1C00)), kBadArg);       /* empty RAM range */
	CHECK_BYTES(f.run(attach_req(0, 0, 0x2400, 0x2400)), kBadArg);       /* empty RAM range */
	CHECK_BYTES(f.run(attach_req(0, 0, 0x1C00, 0x100002)), kBadArg);     /* past 1 MB */
	CHECK_BYTES(f.run(attach_req(0, 0, 0x1C00, 0x4400, 99)), kBadArg);   /* speed */
	CHECK_BYTES(f.run(attach_req(0, 0, 0x1C00, 0x4400, 10001)), kBadArg); /* speed */
	Bytes shortreq = attach_req();
	shortreq.pop_back();
	CHECK_BYTES(f.run(shortreq), kBadArg);
	Bytes longreq = attach_req();
	longreq.push_back(0);
	CHECK_BYTES(f.run(longreq), kBadArg);
	CHECK(f.tap.log.empty());
	CHECK(f.s.state() == State::Idle);

	uint8_t small[16] = {}; /* 2 + 15 bytes needed */
	CHECK_EQ(f.s.handle(attach_req(), small), 2u);
	CHECK_EQ(small[1], 0x01);
	CHECK(f.tap.log.empty());
	uint8_t enough[17] = {};
	f.cpu_runs();
	CHECK_EQ(f.s.handle(attach_req(), enough), 17u);
	CHECK_EQ(enough[1], 0x00);
	CHECK(f.s.state() == State::DebugHalted);
}

TEST(session_dbg_attach_no_target_carries_the_id)
{
	Fx f;
	f.tap.jtag_id = 0xFF;
	CHECK_BYTES(f.run(attach_req()), (Bytes{0x90, 0x04, 0xFF}));
	CHECK(f.s.state() == State::Idle);
	REQUIRE(!f.tap.log.empty());
	CHECK(f.tap.log.back() == "close");
}

/* Cpuxv2::identify() with the normal entry does not stop a Phase A funclet
 * that may still be running on the target, so DBG_ATTACH is refused in
 * Connected and Flashing, with nothing sent. The state error wins over the
 * size error. */
TEST(session_dbg_attach_refused_while_connected_or_flashing)
{
	{
		Fx f; /* Connected */
		f.tap.quick_words = {0x2955};
		REQUIRE(f.run(kConnectReq)[1] == 0x00);
		f.tap.log.clear();
		CHECK_BYTES(f.run(attach_req()), kBadState);
		CHECK_BYTES(f.run({0x90, 0x09, 0x00}), kBadState);
		CHECK(f.tap.log.empty());
		CHECK(f.s.state() == State::Connected);
	}
	{
		Fx f; /* Flashing: the write funclet waits for data */
		f.tap.quick_words = {0x2955};
		REQUIRE(f.run(kConnectReq)[1] == 0x00);
		f.tap.push_jmb_out(0xABADBABE);
		REQUIRE(f.run(begin_req(0x4400, 2, 0x2400))[1] == 0x00);
		REQUIRE(f.s.state() == State::Flashing);
		f.tap.log.clear();
		f.tap.mem_writes.clear();
		CHECK_BYTES(f.run(attach_req()), kBadState);
		CHECK_BYTES(f.run({0x90, 0x09, 0x00}), kBadState);
		CHECK(f.tap.log.empty());
		CHECK(f.tap.mem_writes.empty());
		CHECK(f.s.state() == State::Flashing);
	}
}

/* DBG_ATTACH in a debug session ends it as DBG_DETACH 0 does (a running CPU
 * is halted first; the program continues where it stopped), then attaches as
 * from Idle, with or without reset: the transcript is DBG_DETACH 0's followed
 * by a fresh DBG_ATTACH's, the RAM window comes from the new request, and the
 * old session's breakpoints are gone. */
TEST(session_dbg_attach_in_a_debug_session_detaches_it_first)
{
	/* the reply and DBG_STATUS for attach mode 0 (PC 0x4520, stopped on
	 * request) and mode 1 (the reset vector 0x441A, reset) */
	const Bytes want[2] = {
		{0x90, 0x00, 0x91, 0x06, 0x01, 0x00, 0x1A, 0x00, 0x00, 0x55, 0x29, /* identity */
		 0x20, 0x45, 0x00, 0x00, 0x01, 0x08},                               /* 8 breakpoints */
		{0x90, 0x00, 0x91, 0x06, 0x01, 0x00, 0x1A, 0x00, 0x00, 0x55, 0x29,
		 0x1A, 0x44, 0x00, 0x00, 0x04, 0x08},
	};
	const Bytes status[2] = {
		{0x90, 0x00, 0x01, 0x01, 0xFF, 0x20, 0x45, 0, 0},
		{0x90, 0x00, 0x01, 0x04, 0xFF, 0x1A, 0x44, 0, 0},
	};
	for (const uint8_t mode : {0, 1}) {
		const Bytes req = attach_req(mode, 0, 0x2400, 0x3000);
		for (const bool running : {false, true}) {
			/* The reference: DBG_DETACH 0 from the same session, then
			 * DBG_ATTACH from Idle. */
			Log detach;
			Log fresh;
			{
				Fx g;
				g.open_session(running);
				if (running) {
					g.cpu_runs();
					g.tap.eem_control_script.push_front(0x0000); /* the halt's own look */
				}
				REQUIRE(g.run({0x90, 0x0A, 0x00}) == kOk);
				detach = g.tap.log;
				g.tap.log.clear();
				if (mode == 0) {
					g.cpu_runs();
				}
				CHECK_BYTES(g.run(req), want[mode]);
				fresh = g.tap.log;
			}
			REQUIRE(!detach.empty());
			CHECK(detach.back() == "close");
			CHECK(std::find(detach.begin(), detach.end(), "ir:a8") != detach.end());
			CHECK(std::find(detach.begin(), detach.end(), "dr16:0c01") == detach.end());
			REQUIRE(!fresh.empty());
			CHECK(fresh.front() == "speed:250000");

			Fx f;
			f.open_session(running);
			if (running) {
				/* the halt: WaitForEem's look, the sync's look, the stop poll */
				f.tap.eem_control_script = {0x0000, 0x0000, 0x0080};
				f.tap.captured = {0x0008, 0x0000};
			}
			if (mode == 0) {
				/* the new session's look and stop poll */
				f.tap.eem_control_script.insert(f.tap.eem_control_script.end(),
								{0x0000, 0x0080});
				f.tap.captured.insert(f.tap.captured.end(), {0x0008, 0x0000});
			}
			CHECK_BYTES(f.run(req), want[mode]);
			CHECK_LOG(f.tap.log, detach + fresh); /* the old session's release first */
			CHECK(f.tap.eem_control_script.empty());
			CHECK(f.tap.captured.empty());
			CHECK(f.s.state() == State::DebugHalted);
			CHECK_BYTES(f.run({0x90, 0x0D}), status[mode]);

			/* the RAM window of the new request, not the old session's */
			CHECK_BYTES(f.run(write_req(0x2400, 2)), kOk);
			CHECK_BYTES(f.run(write_req(0x2FFE, 2)), kOk);
			CHECK_BYTES(f.run(write_req(0x1C00, 2)), kProtected);
			CHECK_BYTES(f.run(write_req(0x3000, 2)), kProtected);

			/* breakpoint 5 is gone: a stop at its address reports no
			 * breakpoint (the fake's EEM stops the CPU there anyway) */
			CHECK_BYTES(f.run({0x90, 0x0C}), kOk);
			f.tap.eem_control_script = {0x0080, 0x0080};
			f.tap.captured = {0x0008, 0x0000};
			f.tap.mab = 0x4604;
			CHECK_BYTES(f.run({0x90, 0x0D}),
				    (Bytes{0x90, 0x00, 0x01, 0x02, 0xFF, 0x00, 0x46, 0, 0}));
		}
	}
}

/* A re-attach that fails after the old session's release leaves Idle: the
 * Debugger has released the pins, nothing more is sent. */
TEST(session_dbg_attach_failing_in_a_debug_session_leaves_idle)
{
	Fx f;
	f.open_session(false);
	f.tap.jtag_id = 0xFF;
	CHECK_BYTES(f.run(attach_req()), (Bytes{0x90, 0x04, 0xFF}));
	CHECK(f.s.state() == State::Idle);
	CHECK(log_has(f.tap, "ir:a8"));         /* the old session's release */
	CHECK_EQ(count_log(f.tap, "close"), 2u); /* the release's and the failed entry's */
	REQUIRE(!f.tap.log.empty());
	CHECK(f.tap.log.back() == "close");
	f.tap.log.clear();
	CHECK_BYTES(f.run({0x90, 0x0D}), kBadState);
	CHECK(f.tap.log.empty());
}

/* A malformed DBG_ATTACH in a debug session is BAD_ARG before anything is
 * sent: the open session stays as it was (state, RAM window, breakpoints). */
TEST(session_dbg_attach_bad_arg_leaves_the_debug_session_untouched)
{
	Bytes shortreq = attach_req();
	shortreq.pop_back();
	Bytes longreq = attach_req();
	longreq.push_back(0);
	const Bytes bad[] = {
		attach_req(2),                          /* mode */
		attach_req(0, 1),                       /* full mode: B2 */
		attach_req(0, 2),                       /* unknown flag */
		attach_req(0, 0x80),                    /* unknown flag */
		attach_req(0, 0, 0x1C01),               /* odd RAM start */
		attach_req(0, 0, 0x4400, 0x1C00),       /* empty RAM range */
		attach_req(0, 0, 0x2400, 0x2400),       /* empty RAM range */
		attach_req(0, 0, 0x1C00, 0x100002),     /* past 1 MB */
		attach_req(0, 0, 0x2400, 0x3000, 99),   /* speed, narrower RAM */
		attach_req(0, 0, 0x1C00, 0x4400, 10001), /* speed */
		shortreq,
		longreq,
		{0x90, 0x09},                           /* no arguments */
	};
	for (const bool running : {false, true}) {
		Fx f;
		f.open_session(running);
		for (const Bytes &req : bad) {
			CHECK_BYTES(f.run(req), kBadArg);
		}
		uint8_t small[16] = {}; /* 2 + 15 bytes needed */
		CHECK_EQ(f.s.handle(attach_req(), small), 2u);
		CHECK_EQ(small[1], 0x01);
		CHECK(f.tap.log.empty());
		CHECK(f.tap.mem_writes.empty());
		CHECK(f.s.state() == (running ? State::DebugRunning : State::DebugHalted));

		if (!running) {
			CHECK_BYTES(f.run({0x90, 0x0D}),
				    (Bytes{0x90, 0x00, 0x01, 0x01, 0xFF, 0x20, 0x45, 0, 0}));
			CHECK_BYTES(f.run({0x90, 0x0C}), kOk);
		}
		/* breakpoint 5 still set: a stop at its address reports it */
		f.tap.eem_control_script = {0x0080, 0x0080};
		f.tap.captured = {0x0008, 0x0000};
		f.tap.mab = 0x4604;
		CHECK_BYTES(f.run({0x90, 0x0D}), (Bytes{0x90, 0x00, 0x01, 0x02, 0x05, 0x00, 0x46, 0, 0}));
		/* the RAM window of the first attach */
		CHECK_BYTES(f.run(write_req(0x1C00, 2)), kOk);
		CHECK_BYTES(f.run(write_req(0x43FE, 2)), kOk);
	}
}

TEST(session_debug_ops_need_a_debug_session)
{
	Fx f;
	CHECK_BYTES(f.run({0x90, 0x0A, 0x00}), kBadState);
	CHECK_BYTES(f.run({0x90, 0x0B}), kBadState);
	CHECK_BYTES(f.run({0x90, 0x0C}), kBadState);
	CHECK_BYTES(f.run({0x90, 0x0D}), kBadState);
	CHECK_BYTES(f.run({0x90, 0x0E}), kBadState);
	CHECK_BYTES(f.run({0x90, 0x0F}), kBadState);
	CHECK_BYTES(f.run({0x90, 0x10, 0x04, 0, 0, 0, 0}), kBadState);
	CHECK_BYTES(f.run(bp_req(0, 0x4400)), kBadState);
	CHECK_BYTES(f.run({0x90, 0x12, 0xFF}), kBadState);
	CHECK(f.tap.log.empty());
	f.tap.quick_words = {0x2955};
	REQUIRE(f.run(kConnectReq)[1] == 0x00); /* Phase A CONNECT */
	f.tap.log.clear();
	CHECK_BYTES(f.run({0x90, 0x0A, 0x00}), kBadState);
	CHECK_BYTES(f.run({0x90, 0x0B}), kBadState);
	CHECK_BYTES(f.run({0x90, 0x0C}), kBadState);
	CHECK_BYTES(f.run({0x90, 0x0D}), kBadState);
	CHECK_BYTES(f.run({0x90, 0x0E}), kBadState);
	CHECK_BYTES(f.run({0x90, 0x0F}), kBadState);
	CHECK_BYTES(f.run({0x90, 0x10, 0x04, 0, 0, 0, 0}), kBadState);
	CHECK_BYTES(f.run(bp_req(0, 0x4400)), kBadState);
	CHECK_BYTES(f.run({0x90, 0x12, 0xFF}), kBadState);
	CHECK(f.tap.log.empty());
	CHECK(f.s.state() == State::Connected);

	f.tap.push_jmb_out(0xABADBABE); /* Flashing: the write funclet waits for data */
	REQUIRE(f.run(begin_req(0x4400, 2, 0x2400))[1] == 0x00);
	REQUIRE(f.s.state() == State::Flashing);
	f.tap.log.clear();
	f.tap.mem_writes.clear();
	CHECK_BYTES(f.run({0x90, 0x0A, 0x00}), kBadState);
	CHECK_BYTES(f.run({0x90, 0x0B}), kBadState);
	CHECK_BYTES(f.run({0x90, 0x0C}), kBadState);
	CHECK_BYTES(f.run({0x90, 0x0D}), kBadState);
	CHECK_BYTES(f.run({0x90, 0x0E}), kBadState);
	CHECK_BYTES(f.run({0x90, 0x0F}), kBadState);
	CHECK_BYTES(f.run({0x90, 0x10, 0x04, 0, 0, 0, 0}), kBadState);
	CHECK_BYTES(f.run(bp_req(0, 0x4400)), kBadState);
	CHECK_BYTES(f.run({0x90, 0x12, 0xFF}), kBadState);
	CHECK(f.tap.log.empty());
	CHECK(f.tap.mem_writes.empty());
	CHECK(f.s.state() == State::Flashing);
}

/* The Session's own state checks, not the Debugger's: a wrong size in the
 * wrong state is BAD_STATE, not BAD_ARG. */
TEST(session_debug_state_error_wins_over_size_error)
{
	const Bytes wrong_size[] = {
		{0x90, 0x0A},                         /* DBG_DETACH without its action */
		{0x90, 0x0B, 0x00},                   /* DBG_HALT with a byte too many */
		{0x90, 0x0C, 0x00},                   /* DBG_RESUME */
		{0x90, 0x0D, 0x00},                   /* DBG_STATUS */
		{0x90, 0x0E, 0x00},                   /* DBG_STEP */
		{0x90, 0x0F, 0x00},                   /* DBG_READ_REGS */
		{0x90, 0x10, 0x04, 0x00},             /* DBG_WRITE_REG short */
		{0x90, 0x11, 0x00, 0x00, 0x44},       /* DBG_BP_SET short */
		{0x90, 0x12},                         /* DBG_BP_CLEAR short */
		{0x90, 0x03, 0x00, 0x24, 0x00, 0x00}, /* READ_MEM short */
		{0x90, 0x04, 0x00, 0x24, 0x00},       /* WRITE_MEM short */
	};
	{
		Fx f; /* Idle */
		for (const Bytes &req : wrong_size) {
			CHECK_BYTES(f.run(req), kBadState);
		}
		CHECK(f.tap.log.empty());
	}
	{
		Fx f; /* running: the halted-only operations answer RUNNING */
		f.attach();
		f.resume();
		CHECK_BYTES(f.run({0x90, 0x0C, 0x00}), kRunning);
		CHECK_BYTES(f.run({0x90, 0x0E, 0x00}), kRunning);
		CHECK_BYTES(f.run({0x90, 0x0F, 0x00}), kRunning);
		CHECK_BYTES(f.run({0x90, 0x10, 0x04, 0x00}), kRunning);
		CHECK_BYTES(f.run({0x90, 0x11, 0x00, 0x00, 0x44}), kRunning);
		CHECK_BYTES(f.run({0x90, 0x12}), kRunning);
		CHECK_BYTES(f.run({0x90, 0x03, 0x00, 0x24, 0x00, 0x00}), kRunning);
		CHECK_BYTES(f.run({0x90, 0x04, 0x00, 0x24, 0x00}), kRunning);
		/* the operations allowed while running still check their size */
		CHECK_BYTES(f.run({0x90, 0x0B, 0x00}), kBadArg);
		CHECK_BYTES(f.run({0x90, 0x0D, 0x00}), kBadArg);
		CHECK_BYTES(f.run({0x90, 0x0A}), kBadArg);
		CHECK(f.tap.log.empty());
		CHECK(f.s.state() == State::DebugRunning);
	}
}

TEST(session_dbg_run_status_halt)
{
	Fx f;
	f.attach();
	CHECK_BYTES(f.run({0x90, 0x0C}), kOk);
	CHECK(f.s.state() == State::DebugRunning);
	const Bytes info = f.run({0x90, 0x00});
	REQUIRE(info.size() > 3u);
	CHECK_EQ(info[3], 4); /* INFO reports DebugRunning */
	/* still running: state 0, no reason, no breakpoint, PC 0 */
	CHECK_BYTES(f.run({0x90, 0x0D}), (Bytes{0x90, 0x00, 0x00, 0x00, 0xFF, 0, 0, 0, 0}));
	CHECK(f.s.state() == State::DebugRunning);
	f.cpu_runs();
	f.tap.eem_control_script.push_front(0x0000); /* the halt's own look first */
	f.tap.mab = 0x4ABC;
	CHECK_BYTES(f.run({0x90, 0x0B}), (Bytes{0x90, 0x00, 0xB8, 0x4A, 0x00, 0x00}));
	CHECK(f.s.state() == State::DebugHalted);
	CHECK_BYTES(f.run({0x90, 0x0D}), (Bytes{0x90, 0x00, 0x01, 0x01, 0xFF, 0xB8, 0x4A, 0, 0}));
	f.tap.log.clear();
	CHECK_BYTES(f.run({0x90, 0x0B}), (Bytes{0x90, 0x00, 0xB8, 0x4A, 0x00, 0x00})); /* again: Ok */
	CHECK(f.tap.log.empty()); /* nothing sent */
	CHECK(f.s.state() == State::DebugHalted);
}

TEST(session_dbg_status_reports_a_breakpoint_stop)
{
	Fx f;
	f.attach();
	CHECK_BYTES(f.run(bp_req(5, 0x4600)), kOk);
	CHECK_BYTES(f.run({0x90, 0x0C}), kOk);
	f.tap.eem_control_script = {0x0080, 0x0080};
	f.tap.captured = {0x0008, 0x0000};
	f.tap.mab = 0x4604;
	CHECK_BYTES(f.run({0x90, 0x0D}), (Bytes{0x90, 0x00, 0x01, 0x02, 0x05, 0x00, 0x46, 0, 0}));
	CHECK(f.s.state() == State::DebugHalted);
	/* a halt right after the EEM stopped the CPU takes it as a breakpoint */
	CHECK_BYTES(f.run({0x90, 0x0C}), kOk);
	f.tap.eem_control_script = {0x0080, 0x0080};
	f.tap.captured = {0x0008, 0x0000};
	CHECK_BYTES(f.run({0x90, 0x0B}), (Bytes{0x90, 0x00, 0x00, 0x46, 0x00, 0x00}));
	CHECK_BYTES(f.run({0x90, 0x0D}), (Bytes{0x90, 0x00, 0x01, 0x02, 0x05, 0x00, 0x46, 0, 0}));
}

TEST(session_dbg_halted_only_ops_answer_running)
{
	Fx f;
	f.attach();
	CHECK_BYTES(f.run({0x90, 0x0C}), kOk);
	f.tap.log.clear();
	f.tap.mem_writes.clear(); /* the release wrote WDTCTL back */
	CHECK_BYTES(f.run({0x90, 0x0C}), kRunning);
	CHECK_BYTES(f.run({0x90, 0x0E}), kRunning);
	CHECK_BYTES(f.run({0x90, 0x0F}), kRunning);
	CHECK_BYTES(f.run({0x90, 0x10, 0x04, 0, 0, 0, 0}), kRunning);
	CHECK_BYTES(f.run(bp_req(0, 0x4400)), kRunning);
	CHECK_BYTES(f.run({0x90, 0x12, 0xFF}), kRunning);
	CHECK_BYTES(f.run(read_req(0x2400, 1)), kRunning);
	CHECK_BYTES(f.run(write_req(0x2400, 2)), kRunning);
	CHECK_BYTES(f.run(write_req(0x4400, 2)), kRunning); /* RUNNING before PROTECTED */
	CHECK(f.tap.log.empty());
	CHECK(f.tap.mem_writes.empty());
	CHECK(f.s.state() == State::DebugRunning);
}

TEST(session_dbg_registers)
{
	Fx f;
	f.attach();
	f.tap.captured = {0x43F0, 0x0000};
	const Bytes r = f.run({0x90, 0x0F});
	REQUIRE(r.size() == 2u + 64u);
	CHECK_EQ(r[1], 0x00);
	CHECK_EQ(r[2] | (r[3] << 8), 0x4520); /* R0 */
	CHECK_EQ(r[6] | (r[7] << 8), 0x43F0); /* R1 */
	CHECK_EQ(r[10], 0x08);                /* R2 */
	CHECK_EQ(r[14], 0x00);                /* R3 */
	CHECK_BYTES(f.run({0x90, 0x10, 0x04, 0x34, 0x12, 0x0F, 0x00}), kOk);
	f.tap.log.clear();
	CHECK_BYTES(f.run({0x90, 0x10, 0x03, 0x00, 0x00, 0x00, 0x00}), kBadArg); /* R3 */
	CHECK_BYTES(f.run({0x90, 0x10, 0x10, 0x00, 0x00, 0x00, 0x00}), kBadArg); /* R16 */
	CHECK_BYTES(f.run({0x90, 0x10, 0x04, 0x00, 0x00, 0x10, 0x00}), kBadArg); /* 21 bits */
	CHECK_BYTES(f.run({0x90, 0x10, 0x00, 0x01, 0x00, 0x00, 0x00}), kBadArg); /* odd PC */
	CHECK_BYTES(f.run({0x90, 0x10, 0x04, 0x00}), kBadArg);
	CHECK_BYTES(f.run({0x90, 0x10, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00}), kBadArg);
	CHECK_BYTES(f.run({0x90, 0x0F, 0x00}), kBadArg);
	CHECK(f.tap.log.empty());
	CHECK(f.s.state() == State::DebugHalted);

	/* the new PC is reported by DBG_STATUS, not written until the release */
	CHECK_BYTES(f.run({0x90, 0x10, 0x00, 0x00, 0x46, 0x00, 0x00}), kOk);
	CHECK(f.tap.log.empty());
	CHECK_BYTES(f.run({0x90, 0x0D}), (Bytes{0x90, 0x00, 0x01, 0x01, 0xFF, 0x00, 0x46, 0, 0}));

	uint8_t small[65] = {}; /* 2 + 64 bytes needed */
	CHECK_EQ(f.s.handle(Bytes{0x90, 0x0F}, small), 2u);
	CHECK_EQ(small[1], 0x01);
	CHECK(f.tap.log.empty());
}

/* DBG_WRITE_REG moves the PC only into the device's main flash (the F5529's
 * [0x4400, 0x24400)) or leaves it where it is (GDB's G packet writes every
 * register back); any other PC is PROTECTED and the saved PC keeps its
 * value. Nothing is sent either way (R0 lives in the saved context). An odd
 * PC stays BAD_ARG: the form is checked first, as for WRITE_MEM. */
TEST(session_dbg_write_reg_moves_the_pc_only_into_main_flash)
{
	const Bytes at_4520 = {0x90, 0x00, 0x01, 0x01, 0xFF, 0x20, 0x45, 0, 0}; /* DBG_STATUS */
	{
		Fx f;
		f.attach();
		/* the BSL, information memory, USB RAM, RAM, past main flash, the
		 * peripherals, vacant memory, above 20 bits */
		for (const uint32_t pc : {0x1000u, 0x1800u, 0x1C00u, 0x2400u, 0x43FEu, 0x24400u,
					  0x0004u, 0x0000u, 0x0FFEu, 0xFFFFEu, 0x100000u, 0xFFFFFFFEu}) {
			CHECK_BYTES(f.run(reg_req(0, pc)), kProtected);
		}
		CHECK_BYTES(f.run(reg_req(0, 0x2401)), kBadArg);
		CHECK_BYTES(f.run(reg_req(0, 0x4401)), kBadArg);
		CHECK_BYTES(f.run({0x90, 0x0D}), at_4520);
		CHECK_BYTES(f.run(reg_req(0, 0x4520)), kOk); /* unchanged */
		CHECK_BYTES(f.run(reg_req(0, 0x4400)), kOk);
		CHECK_BYTES(f.run({0x90, 0x0D}), (Bytes{0x90, 0x00, 0x01, 0x01, 0xFF, 0x00, 0x44, 0, 0}));
		CHECK_BYTES(f.run(reg_req(0, 0x243FE)), kOk);
		CHECK_BYTES(f.run({0x90, 0x0D}), (Bytes{0x90, 0x00, 0x01, 0x01, 0xFF, 0xFE, 0x43, 0x02, 0}));
		CHECK(f.tap.log.empty());
		CHECK_BYTES(f.run(reg_req(0, 0x4520)), kOk); /* back: main flash */
		CHECK_BYTES(f.run(reg_req(4, 0x2400)), kOk); /* other registers: any value */
		CHECK(f.s.state() == State::DebugHalted);
	}
	{
		Fx f; /* halted in code the firmware runs from RAM */
		f.tap.mab = 0x2404;
		f.attach();
		CHECK_BYTES(f.run({0x90, 0x0D}), (Bytes{0x90, 0x00, 0x01, 0x01, 0xFF, 0x00, 0x24, 0, 0}));
		CHECK_BYTES(f.run(reg_req(0, 0x2400)), kOk); /* unchanged */
		CHECK_BYTES(f.run(reg_req(0, 0x2402)), kProtected);
		CHECK_BYTES(f.run(reg_req(0, 0x4400)), kOk);        /* out into main flash */
		CHECK_BYTES(f.run(reg_req(0, 0x2400)), kProtected); /* not back */
		CHECK_BYTES(f.run({0x90, 0x0D}), (Bytes{0x90, 0x00, 0x01, 0x01, 0xFF, 0x00, 0x44, 0, 0}));
		CHECK(f.tap.log.empty());
	}
	{
		Fx f; /* an unknown device: the PC stays where it is */
		f.tap.memory[0x1A04] = 0x1234;
		f.attach();
		CHECK_BYTES(f.run(reg_req(0, 0x4520)), kOk);
		for (const uint32_t pc : {0x4400u, 0x4522u, 0x243FEu, 0x2400u, 0x1000u}) {
			CHECK_BYTES(f.run(reg_req(0, pc)), kProtected);
		}
		CHECK_BYTES(f.run({0x90, 0x0D}), at_4520);
		CHECK(f.tap.log.empty());
	}
	{
		Fx f; /* the main flash is set anew by every attach */
		f.attach();
		CHECK_BYTES(f.run(reg_req(0, 0x4400)), kOk);
		f.cpu_runs();
		f.tap.memory[0x1A04] = 0x1234;
		REQUIRE(f.run(attach_req())[1] == 0x00); /* re-attach: an unknown device */
		f.tap.log.clear();
		CHECK_BYTES(f.run(reg_req(0, 0x4400)), kProtected);
		CHECK_BYTES(f.run({0x90, 0x0D}), at_4520);
		CHECK(f.tap.log.empty());
	}
}

/* The PC DBG_WRITE_REG may leave unchanged is the one the CPU stopped at
 * last: after a step, a halt and a breakpoint stop, not only after the
 * attach. Here the CPU runs from RAM, outside main flash, so only that
 * exception lets R0 be written back. */
TEST(session_dbg_write_reg_current_pc_follows_every_stop)
{
	Fx f;
	f.tap.mab = 0x2404;
	f.attach(); /* PC 0x2400 */

	f.cpu_runs(); /* DBG_STEP to 0x2404 */
	f.tap.eem_control_script = {0x0080, 0x0080};
	f.tap.mab = 0x2408;
	CHECK_BYTES(f.run({0x90, 0x0E}), (Bytes{0x90, 0x00, 0x04, 0x24, 0x00, 0x00}));
	f.tap.log.clear();
	CHECK_BYTES(f.run(reg_req(0, 0x2404)), kOk);
	CHECK_BYTES(f.run(reg_req(0, 0x2400)), kProtected);
	CHECK(f.tap.log.empty());

	CHECK_BYTES(f.run({0x90, 0x0C}), kOk); /* DBG_RESUME, DBG_HALT at 0x2408 */
	f.cpu_runs();
	f.tap.eem_control_script.push_front(0x0000); /* the halt's own look */
	f.tap.mab = 0x240C;
	CHECK_BYTES(f.run({0x90, 0x0B}), (Bytes{0x90, 0x00, 0x08, 0x24, 0x00, 0x00}));
	f.tap.log.clear();
	CHECK_BYTES(f.run(reg_req(0, 0x2408)), kOk);
	CHECK_BYTES(f.run(reg_req(0, 0x2404)), kProtected);
	CHECK(f.tap.log.empty());

	CHECK_BYTES(f.run({0x90, 0x0C}), kOk); /* DBG_RESUME, a stop DBG_STATUS sees at 0x240C */
	f.tap.eem_control_script = {0x0080, 0x0080};
	f.tap.captured = {0x0008, 0x0000};
	f.tap.mab = 0x2410;
	CHECK_BYTES(f.run({0x90, 0x0D}), (Bytes{0x90, 0x00, 0x01, 0x02, 0xFF, 0x0C, 0x24, 0, 0}));
	f.tap.log.clear();
	CHECK_BYTES(f.run(reg_req(0, 0x240C)), kOk);
	CHECK_BYTES(f.run(reg_req(0, 0x2408)), kProtected);
	CHECK(f.tap.log.empty());
	CHECK_BYTES(f.run({0x90, 0x0D}), (Bytes{0x90, 0x00, 0x01, 0x02, 0xFF, 0x0C, 0x24, 0, 0}));
}

TEST(session_dbg_step_returns_the_new_pc)
{
	Fx f;
	f.attach();
	f.cpu_runs();
	f.tap.eem_control_script = {0x0080, 0x0080};
	f.tap.mab = 0x4526;
	CHECK_BYTES(f.run({0x90, 0x0E}), (Bytes{0x90, 0x00, 0x22, 0x45, 0x00, 0x00}));
	CHECK(f.s.state() == State::DebugHalted);
	CHECK_BYTES(f.run({0x90, 0x0D}), (Bytes{0x90, 0x00, 0x01, 0x03, 0xFF, 0x22, 0x45, 0, 0}));
}

TEST(session_dbg_step_timeout_leaves_the_cpu_halted)
{
	Fx f;
	f.attach();
	f.tap.eem_control = 0x0000; /* never stops by itself */
	f.tap.eem_control_script.clear();
	/* after 496 polls: the sync's stop request, which stops at once */
	for (int i = 0; i < 496 + 1; ++i) {
		f.tap.eem_control_script.push_back(0x0000);
	}
	f.tap.eem_control_script.push_back(0x0080);
	f.tap.captured = {0x0008, 0x0000};
	CHECK_BYTES(f.run({0x90, 0x0E}), kTimeout);
	CHECK(f.s.state() == State::DebugHalted);
	CHECK(f.tap.eem_control_script.empty());
	/* halted at the stop request's PC, reported as a request */
	CHECK_BYTES(f.run({0x90, 0x0D}), (Bytes{0x90, 0x00, 0x01, 0x01, 0xFF, 0x20, 0x45, 0, 0}));
}

TEST(session_dbg_breakpoint_arguments)
{
	Fx f;
	f.attach();
	CHECK_BYTES(f.run(bp_req(8, 0x4400)), kBadArg);
	CHECK_BYTES(f.run(bp_req(0xFF, 0x4400)), kBadArg);
	CHECK_BYTES(f.run(bp_req(0, 0x4401)), kBadArg);
	CHECK_BYTES(f.run(bp_req(0, 0x100000)), kBadArg);
	CHECK_BYTES(f.run({0x90, 0x11, 0x00, 0x00, 0x44}), kBadArg);             /* short */
	CHECK_BYTES(f.run({0x90, 0x11, 0x00, 0x00, 0x44, 0x00, 0x00, 0x00}), kBadArg); /* long */
	CHECK_BYTES(f.run({0x90, 0x12, 0x08}), kBadArg);
	CHECK_BYTES(f.run({0x90, 0x12, 0xFE}), kBadArg);
	CHECK_BYTES(f.run({0x90, 0x12}), kBadArg);
	CHECK_BYTES(f.run({0x90, 0x12, 0x00, 0x00}), kBadArg);
	CHECK(f.tap.log.empty());
	CHECK_BYTES(f.run(bp_req(7, 0x24400 - 2)), kOk);
	CHECK_BYTES(f.run({0x90, 0x12, 0x07}), kOk);
	CHECK_BYTES(f.run({0x90, 0x12, 0x07}), kOk); /* not set: still Ok */
	CHECK_BYTES(f.run({0x90, 0x12, 0xFF}), kOk);
	CHECK(f.s.state() == State::DebugHalted);
}

TEST(session_dbg_read_mem_keeps_the_context)
{
	Fx f;
	f.attach();
	f.tap.memory[0x4400] = 0xBEEF;
	CHECK_BYTES(f.run(read_req(0x4400, 1)), (Bytes{0x90, 0x00, 0xEF, 0xBE}));
	/* main flash: ReadMemQuickXv2 (correction 28); it moves the CPU's PC, not
	 * the saved one, which DBG_STATUS reports */
	CHECK(log_has(f.tap, "ir:c2"));
	CHECK(f.s.state() == State::DebugHalted);
	CHECK_BYTES(f.run({0x90, 0x0D}), (Bytes{0x90, 0x00, 0x01, 0x01, 0xFF, 0x20, 0x45, 0, 0}));
	/* reads have no whitelist: flash, information memory, the controller */
	f.tap.log.clear();
	f.tap.memory[0x1800] = 0x0000; /* FakeTap's quick read gives 0xFFFF for unset words */
	CHECK_BYTES(f.run(read_req(0x1800, 1)), (Bytes{0x90, 0x00, 0x00, 0x00}));
	CHECK_BYTES(f.run(read_req(0x0144, 1)), (Bytes{0x90, 0x00, 0x58, 0x96}));
	CHECK_BYTES(f.run(read_req(0xFFFFE, 1)), (Bytes{0x90, 0x00, 0x00, 0x00}));
	CHECK(f.tap.mem_writes.empty());
	/* the Phase A argument rules still hold */
	f.tap.log.clear();
	CHECK_BYTES(f.run(read_req(0x4401, 1)), kBadArg);
	CHECK_BYTES(f.run(read_req(0x4400, 0)), kBadArg);
	CHECK_BYTES(f.run(read_req(0xFFFFE, 2)), kBadArg);
	CHECK_BYTES(f.run(read_req(0x4400, 509)), kBadArg);
	CHECK(f.tap.log.empty());
}

/* READ_MEM and WRITE_MEM in a debug session go through the Debugger
 * (test_debugger.cpp has these transcripts): its WriteMemWordsXv2 port, its
 * ReadMemQuickXv2 port for memory and ReadMemWordsXv2 for peripherals
 * (correction 28), not through Phase A's quick read and write. */
TEST(session_dbg_mem_ops_go_through_the_debugger)
{
	Fx f;
	f.attach();
	CHECK_BYTES(f.run(write_req(0x2400, 4)), kOk);
	CHECK_LOG(f.tap.log, Log{"ir:28", "dr16:0000"} + write_word(0x2400, 0x0100) +
				     write_word(0x2402, 0x0302) + Log{"ir:28", "dr16:0000"});
	CHECK_EQ(f.tap.memory[0x2400], 0x0100);
	CHECK_EQ(f.tap.memory[0x2402], 0x0302);
	f.tap.log.clear();
	f.tap.memory[0x2400] = 0x1234;
	CHECK_BYTES(f.run(read_req(0x2400, 1)), (Bytes{0x90, 0x00, 0x34, 0x12}));
	CHECK_LOG(f.tap.log, quick_read(0x2400, 1) + Log{"ir:28", "dr16:0000"});
	f.tap.log.clear();
	f.tap.memory[0x0200] = 0x5678;
	CHECK_BYTES(f.run(read_req(0x0200, 1)), (Bytes{0x90, 0x00, 0x78, 0x56}));
	CHECK_LOG(f.tap.log, Log{"tclk:0", ir(0xC8), d16(0x0501), ir(0xC1), d20(0x0200), "tclk:1",
				 "tclk:0", ir(0x42), d16(0), "tclk:1", "tclk:0", "tclk:1", "ir:28",
				 "dr16:0000"});
	CHECK(f.s.state() == State::DebugHalted);
}

TEST(session_dbg_write_mem_whitelist)
{
	Fx f;
	f.attach();
	/* allowed: RAM, USB RAM, the F5529's peripheral registers 0x0100-0x093F
	 * outside its PMM, flash controller, SYS module and DMA controller, each
	 * edge included */
	CHECK_BYTES(f.run(write_req(0x2400, 4)), kOk);
	CHECK_BYTES(f.run(write_req(0x1C00, 2)), kOk);
	CHECK_BYTES(f.run(write_req(0x43FE, 2)), kOk);
	CHECK_BYTES(f.run(write_req(0x0200, 2)), kOk);
	CHECK_BYTES(f.run(write_req(0x0100, 2)), kOk); /* SFRIE1: the window's first word */
	CHECK_BYTES(f.run(write_req(0x093E, 2)), kOk); /* USBIFG: the window's last word */
	CHECK_BYTES(f.run(write_req(0x011E, 2)), kOk); /* below PMMCTL0 */
	CHECK_BYTES(f.run(write_req(0x0150, 2)), kOk); /* above FCTL4 */
	CHECK_BYTES(f.run(write_req(0x017E, 2)), kOk); /* below SYSCTL */
	CHECK_BYTES(f.run(write_req(0x01A0, 2)), kOk); /* above SYSRSTIV */
	CHECK_BYTES(f.run(write_req(0x04FE, 2)), kOk); /* below DMACTL0 */
	CHECK_BYTES(f.run(write_req(0x0540, 2)), kOk); /* above DMA2SZ */
	CHECK_BYTES(f.run(write_req(0x05C0, 2)), kOk); /* UCA0CTLW0 */
	CHECK_BYTES(f.run(write_req(0x05FE, 2)), kOk); /* UCB0IV */
	CHECK_BYTES(f.run(write_req(0x0600, 2)), kOk);
	CHECK_BYTES(f.run(write_req(0x1C00, 1016)), kOk); /* the largest write */
	const size_t writes = f.tap.mem_writes.size();
	CHECK_EQ(writes, 16u + 508u);
	f.tap.log.clear();
	/* refused: main flash, information memory, BSL, vacant memory (below
	 * and above the peripheral registers too), the PMM, the flash
	 * controller, the SYS module, the DMA controller, the top of the address
	 * space, a write that runs off the end of RAM, of the peripheral
	 * registers or into one of the four */
	for (uint32_t addr : {0x4400u, 0x4402u, 0xFFFEu, 0x10000u, 0x1FFFEu, 0x243FEu, 0x24400u,
			      0xFFFFEu, 0x1800u, 0x1980u, 0x19FEu, 0x1A00u, 0x1B00u, 0x1BFEu,
			      0x1000u, 0x17FEu, 0x1C00u - 2u, 0x1C00u - 0x200u, 0x0120u, 0x0122u,
			      0x0124u, 0x012Cu, 0x0130u, 0x013Eu, 0x0140u, 0x0142u,
			      0x0144u, 0x0146u, 0x0148u, 0x014Eu, 0x0180u, 0x0182u, 0x0186u,
			      0x019Eu, 0x0500u, 0x0510u, 0x053Au, 0x053Eu, 0x0000u, 0x00FEu,
			      0x0940u, 0x0FFEu}) {
		CHECK_BYTES(f.run(write_req(addr, 2)), kProtected);
	}
	CHECK_BYTES(f.run(write_req(0x43FE, 4)), kProtected);    /* RAM into flash */
	CHECK_BYTES(f.run(write_req(0x4010, 1016)), kProtected); /* RAM into flash */
	CHECK_BYTES(f.run(write_req(0x00FE, 4)), kProtected);    /* vacant into SFRIE1 */
	CHECK_BYTES(f.run(write_req(0x00F0, 0x20)), kProtected); /* across SFRIE1's start */
	CHECK_BYTES(f.run(write_req(0x093E, 4)), kProtected);    /* USBIFG into vacant */
	CHECK_BYTES(f.run(write_req(0x0930, 0x20)), kProtected); /* across the window's end */
	CHECK_BYTES(f.run(write_req(0x011E, 4)), kProtected);    /* into PMMCTL0 */
	CHECK_BYTES(f.run(write_req(0x0110, 0x20)), kProtected); /* across PMMCTL0 */
	CHECK_BYTES(f.run(write_req(0x013E, 4)), kProtected);    /* PMM into FCTL1 */
	CHECK_BYTES(f.run(write_req(0x014E, 4)), kProtected);    /* out of FCTL4 */
	CHECK_BYTES(f.run(write_req(0x0100, 0x50)), kProtected); /* across PMM and FCTL */
	CHECK_BYTES(f.run(write_req(0x017E, 4)), kProtected);    /* into SYSCTL */
	CHECK_BYTES(f.run(write_req(0x019E, 4)), kProtected);    /* out of SYSRSTIV */
	CHECK_BYTES(f.run(write_req(0x0170, 0x40)), kProtected); /* across the SYS module */
	CHECK_BYTES(f.run(write_req(0x04FE, 4)), kProtected);    /* into DMACTL0 */
	CHECK_BYTES(f.run(write_req(0x053E, 4)), kProtected);    /* out of the DMA controller */
	CHECK_BYTES(f.run(write_req(0x0400, 0x300)), kProtected); /* across the DMA controller */
	CHECK_BYTES(f.run(write_req(0x0FFE, 4)), kProtected);   /* vacant into the BSL */
	CHECK_BYTES(f.run(write_req(0x0FF0, 0x3F8)), kProtected); /* vacant into the BSL */
	CHECK_BYTES(f.run(write_req(0x1BFE, 4)), kProtected);   /* descriptors into RAM */
	CHECK_BYTES(f.run(write_req(0x1BFE, 0x100)), kProtected);
	CHECK_EQ(f.tap.mem_writes.size(), writes);
	CHECK(f.tap.log.empty());
	CHECK(!f.tap.flash_write_attempt);
	CHECK(f.s.state() == State::DebugHalted);
}

/* The write may be refused on its form (BAD_ARG: odd address, odd length, no
 * data, more than max_data_bytes) or on its address (PROTECTED); either way
 * nothing reaches the TAP. A range that leaves the 20-bit space lies outside
 * every window, so it is PROTECTED, unlike Phase A's BAD_ARG. */
TEST(session_dbg_write_mem_malformed_is_bad_arg_and_sends_nothing)
{
	Fx f;
	f.attach();
	CHECK_BYTES(f.run(write_req(0x2401, 2)), kBadArg);     /* odd address in RAM */
	CHECK_BYTES(f.run(write_req(0x4401, 2)), kBadArg);     /* odd address in flash */
	CHECK_BYTES(f.run(write_req(0x0141, 2)), kBadArg);     /* odd address in the controller */
	CHECK_BYTES(f.run(write_req(0x2400, 3)), kBadArg);     /* odd length */
	CHECK_BYTES(f.run(write_req(0x2400, 1)), kBadArg);     /* half a word */
	CHECK_BYTES(f.run(write_req(0x4400, 1)), kBadArg);
	CHECK_BYTES(f.run(write_req(0x2400, 0)), kBadArg);     /* no data */
	CHECK_BYTES(f.run(write_req(0x1C00, 1018)), kBadArg);  /* more than max_data_bytes */
	CHECK_BYTES(f.run({0x90, 0x04, 0x00, 0x24, 0x00}), kBadArg); /* short */
	CHECK_BYTES(f.run(write_req(0xFFFFE, 4)), kProtected);  /* wraps around 0xFFFFF */
	CHECK_BYTES(f.run(write_req(0xFFFFFFFEu, 2)), kProtected);
	CHECK_BYTES(f.run(write_req(0x100000, 2)), kProtected); /* above 20 bits */
	CHECK(f.tap.log.empty());
	CHECK(f.tap.mem_writes.empty());
	CHECK(!f.tap.flash_write_attempt);
	CHECK(f.s.state() == State::DebugHalted);
}

TEST(session_dbg_write_mem_ram_is_the_intersection_with_the_known_device)
{
	{
		Fx f; /* the host passes a narrower range: only that */
		f.cpu_runs();
		REQUIRE(f.run(attach_req(0, 0, 0x2400, 0x3000))[1] == 0x00);
		CHECK_BYTES(f.run(write_req(0x2400, 2)), kOk);
		CHECK_BYTES(f.run(write_req(0x2FFE, 2)), kOk);
		CHECK_BYTES(f.run(write_req(0x1C00, 2)), kProtected);
		CHECK_BYTES(f.run(write_req(0x23FE, 2)), kProtected);
		CHECK_BYTES(f.run(write_req(0x3000, 2)), kProtected);
		CHECK_BYTES(f.run(write_req(0x2FFE, 4)), kProtected);
		CHECK_BYTES(f.run(write_req(0x0200, 2)), kOk); /* peripherals: not from the host */
	}
	{
		Fx f; /* a wrong host table that covers flash cannot open it */
		f.cpu_runs();
		REQUIRE(f.run(attach_req(0, 0, 0x1C00, 0x24400))[1] == 0x00);
		CHECK_BYTES(f.run(write_req(0x43FE, 2)), kOk);
		CHECK_BYTES(f.run(write_req(0x4400, 2)), kProtected);
		CHECK_BYTES(f.run(write_req(0x10000, 2)), kProtected);
		CHECK(!f.tap.flash_write_attempt);
	}
	{
		Fx f; /* a host table that starts in the BSL or information memory */
		f.cpu_runs();
		REQUIRE(f.run(attach_req(0, 0, 0x1000, 0x4400))[1] == 0x00);
		CHECK_BYTES(f.run(write_req(0x1000, 2)), kProtected);
		CHECK_BYTES(f.run(write_req(0x1800, 2)), kProtected);
		CHECK_BYTES(f.run(write_req(0x1BFE, 2)), kProtected);
		CHECK_BYTES(f.run(write_req(0x1C00, 2)), kOk);
	}
	{
		Fx f; /* the host's range covers the whole address space */
		f.cpu_runs();
		REQUIRE(f.run(attach_req(0, 0, 0x0000, 0x100000))[1] == 0x00);
		CHECK_BYTES(f.run(write_req(0x1BFE, 2)), kProtected);
		CHECK_BYTES(f.run(write_req(0x1C00, 2)), kOk);
		CHECK_BYTES(f.run(write_req(0x43FE, 2)), kOk);
		CHECK_BYTES(f.run(write_req(0x4400, 2)), kProtected);
		CHECK_BYTES(f.run(write_req(0x0140, 2)), kProtected);
	}
	{
		Fx f; /* a host range that misses the device's RAM entirely: no RAM */
		f.cpu_runs();
		REQUIRE(f.run(attach_req(0, 0, 0x4400, 0x24400))[1] == 0x00);
		CHECK_BYTES(f.run(write_req(0x4400, 2)), kProtected);
		CHECK_BYTES(f.run(write_req(0x2400, 2)), kProtected);
		CHECK_BYTES(f.run(write_req(0x0200, 2)), kOk);
	}
	{
		/* an unknown device: no RAM nor peripheral writes at all (where its
		 * PMM, flash controller, SYS module and DMA controller lie is not
		 * known) */
		Fx f;
		f.cpu_runs();
		f.tap.memory[0x1A04] = 0x1234;
		REQUIRE(f.run(attach_req())[1] == 0x00);
		f.tap.log.clear();
		f.tap.mem_writes.clear();
		for (const uint32_t addr : {0x2400u, 0x1C00u, 0x43FEu, 0x4400u, 0x0000u, 0x0200u,
					    0x013Eu, 0x0140u, 0x0600u, 0x0FFEu}) {
			CHECK_BYTES(f.run(write_req(addr, 2)), kProtected);
		}
		CHECK(f.tap.log.empty());
		CHECK(f.tap.mem_writes.empty());
	}
	{
		Fx f; /* the window is set anew by every attach */
		f.cpu_runs();
		REQUIRE(f.run(attach_req(0, 0, 0x1C00, 0x4400))[1] == 0x00);
		CHECK_BYTES(f.run(write_req(0x1C00, 2)), kOk);
		CHECK_BYTES(f.run({0x90, 0x0A, 0x00}), kOk);
		f.cpu_runs();
		f.tap.memory[0x1A04] = 0x1234;
		REQUIRE(f.run(attach_req(0, 0, 0x1C00, 0x4400))[1] == 0x00);
		CHECK_BYTES(f.run(write_req(0x1C00, 2)), kProtected);
	}
}

TEST(session_flash_ops_are_locked_in_debug_states)
{
	Fx f;
	f.attach();
	const Bytes erase = erase_req(2, 0x4400, 0x2400);
	const Bytes begin = begin_req(0x4400, 2, 0x2400);
	CHECK_BYTES(f.run(erase), kProtected);
	CHECK_BYTES(f.run(erase_req(0, 0x1800, 0x2400)), kProtected);
	CHECK_BYTES(f.run(begin), kProtected);
	CHECK_BYTES(f.run({0x90, 0x05, 0x02}), kProtected); /* even malformed */
	CHECK_BYTES(f.run({0x90, 0x06}), kProtected);
	CHECK_BYTES(f.run({0x90, 0x07, 0x34, 0x12}), kBadState);
	CHECK_BYTES(f.run({0x90, 0x08}), kBadState);
	CHECK_BYTES(f.run({0x90, 0x02}), kBadState); /* DISCONNECT: use DBG_DETACH */
	CHECK_BYTES(f.run({0x90, 0x02, 0x00}), kBadState);
	CHECK(f.tap.log.empty());
	CHECK(f.tap.mem_writes.empty());
	CHECK(f.s.state() == State::DebugHalted);
	CHECK_BYTES(f.run({0x90, 0x0C}), kOk);
	f.tap.log.clear();
	f.tap.mem_writes.clear(); /* the release wrote WDTCTL back */
	CHECK_BYTES(f.run(erase), kProtected);
	CHECK_BYTES(f.run(begin), kProtected);
	CHECK_BYTES(f.run({0x90, 0x07, 0x34, 0x12}), kBadState);
	CHECK_BYTES(f.run({0x90, 0x08}), kBadState);
	CHECK_BYTES(f.run({0x90, 0x02}), kBadState);
	CHECK(f.tap.log.empty());
	CHECK(f.tap.mem_writes.empty());
	CHECK(f.s.state() == State::DebugRunning);
}

TEST(session_dbg_detach)
{
	{
		Fx f;
		f.attach();
		CHECK_BYTES(f.run({0x90, 0x0A, 0x00}), kOk);
		CHECK(f.s.state() == State::Idle);
		CHECK(log_has(f.tap, "ir:a8"));
		CHECK(!log_has(f.tap, "dr16:0c01")); /* restore and run, no reset */
		REQUIRE(!f.tap.log.empty());
		CHECK(f.tap.log.back() == "close");
		const Bytes info = f.run({0x90, 0x00});
		REQUIRE(info.size() > 3u);
		CHECK_EQ(info[3], 0);
		CHECK_BYTES(f.run({0x90, 0x0A, 0x00}), kBadState); /* again: no session */
	}
	{
		Fx f;
		f.attach();
		CHECK_BYTES(f.run({0x90, 0x0A, 0x01}), kOk);
		CHECK(log_has(f.tap, "dr16:0c01")); /* reset and run */
		CHECK(f.s.state() == State::Idle);
		REQUIRE(!f.tap.log.empty());
		CHECK(f.tap.log.back() == "close");
	}
	{
		Fx f;
		f.attach();
		CHECK_BYTES(f.run({0x90, 0x0A, 0x02}), kBadArg);
		CHECK_BYTES(f.run({0x90, 0x0A}), kBadArg);
		CHECK_BYTES(f.run({0x90, 0x0A, 0x00, 0x00}), kBadArg);
		CHECK(f.tap.log.empty());
		CHECK(f.s.state() == State::DebugHalted);
	}
	{
		Fx f; /* running: halted first, then restored and released */
		f.attach();
		f.resume();
		f.cpu_runs();
		f.tap.eem_control_script.push_front(0x0000);
		CHECK_BYTES(f.run({0x90, 0x0A, 0x00}), kOk);
		CHECK(f.s.state() == State::Idle);
		CHECK(log_has(f.tap, "dr16:1501"));
		CHECK(log_has(f.tap, "ir:a8"));
		CHECK(!log_has(f.tap, "dr16:0c01"));
		REQUIRE(!f.tap.log.empty());
		CHECK(f.tap.log.back() == "close");
	}
	{
		/* the link check before the restore fails: the probe resets the
		 * target instead (its firmware restarts) and answers TARGET_ERROR,
		 * so the host does not report a program that continues where it
		 * stopped (design spec correction 26); the session is over */
		Fx f;
		f.attach();
		f.tap.cpu_words.clear();
		f.tap.cntrl_sig = 0x0000;
		CHECK_BYTES(f.run({0x90, 0x0A, 0x00}), kTargetError);
		CHECK(f.s.state() == State::Idle);
		CHECK_LOG(f.tap.log, clear_triggers() + link_check() + link_check() + kResetRelease);
		CHECK(f.tap.mem_writes.empty()); /* no WDTCTL restore */
		CHECK(f.tap.cpu_words.empty());  /* no SR or PC fed */
		f.tap.log.clear();
		CHECK_BYTES(f.run({0x90, 0x0A, 0x00}), kBadState);
		CHECK(f.tap.log.empty());
	}
	{
		/* with action 1 a reset is what was asked for: OK, no link check */
		Fx f;
		f.attach();
		f.tap.cntrl_sig = 0x0000;
		CHECK_BYTES(f.run({0x90, 0x0A, 0x01}), kOk);
		CHECK(f.s.state() == State::Idle);
		CHECK_LOG(f.tap.log, clear_triggers() + kResetRelease);
	}
}

/* One glitched capture during DBG_DETACH 0 on a healthy link: the second
 * link check passes, the context is restored, OK, no reset. */
TEST(session_dbg_detach_restores_the_context_after_one_glitched_check)
{
	for (const bool id_glitch : {false, true}) {
		Fx f;
		f.attach();
		if (id_glitch) {
			f.tap.jtag_id_script = {0x91, 0x48}; /* clear_triggers' IR scan, the check */
		} else {
			f.tap.cntrl_sig_script = {0x0000};
		}
		CHECK_BYTES(f.run({0x90, 0x0A, 0x00}), kOk);
		CHECK(f.s.state() == State::Idle);
		CHECK(f.tap.jtag_id_script.empty());
		CHECK(f.tap.cntrl_sig_script.empty());
		CHECK(!log_has(f.tap, "dr16:0c01"));
		CHECK_LOG(f.tap.log, clear_triggers() + link_check(!id_glitch) + link_check() +
					     restore_release(0x4520, 0x0008, 0x6904, 0x0006) +
					     Log{"close"});
	}
}

TEST(session_abort_detaches_a_debug_session)
{
	{
		Fx f;
		f.attach();
		f.s.abort();
		CHECK(f.s.state() == State::Idle);
		CHECK(log_has(f.tap, "ir:a8"));
		CHECK(!log_has(f.tap, "dr16:0c01"));
		REQUIRE(!f.tap.log.empty());
		CHECK(f.tap.log.back() == "close");
		f.tap.log.clear();
		f.s.abort(); /* idle again: nothing happens */
		CHECK(f.tap.log.empty());
	}
	{
		Fx f; /* running: halted first, then restored and released */
		f.attach();
		REQUIRE(f.run({0x90, 0x0C})[1] == 0x00);
		f.tap.log.clear();
		f.cpu_runs();
		f.tap.eem_control_script.push_front(0x0000);
		f.s.abort();
		CHECK(f.s.state() == State::Idle);
		CHECK(log_has(f.tap, "dr16:1501"));
		CHECK(log_has(f.tap, "ir:a8"));
		CHECK(!log_has(f.tap, "dr16:0c01"));
		REQUIRE(!f.tap.log.empty());
		CHECK(f.tap.log.back() == "close");
	}
}

TEST(session_connect_ends_a_debug_session_first)
{
	{
		Fx f;
		f.attach();
		f.tap.quick_words = {0x2955};
		REQUIRE(f.run(kConnectReq)[1] == 0x00);
		CHECK(f.s.state() == State::Connected);
		CHECK(log_has(f.tap, "ir:a8"));
		/* the debug session's release before Phase A's entry */
		REQUIRE(!f.tap.log.empty());
		CHECK(f.tap.log.front() != "speed:250000");
		CHECK(log_has(f.tap, "speed:250000"));
	}
	{
		Fx f; /* running: halted first */
		f.attach();
		f.resume();
		f.cpu_runs();
		f.tap.eem_control_script.push_front(0x0000);
		f.tap.quick_words = {0x2955};
		REQUIRE(f.run(kConnectReq)[1] == 0x00);
		CHECK(f.s.state() == State::Connected);
		CHECK(log_has(f.tap, "dr16:1501"));
	}
}

/* A TargetError from poll(), halt() or step() means the Debugger has already
 * ended the session (EEM off, CPU let go, pins released): the Session goes
 * Idle and sends nothing more. */
TEST(session_target_error_from_a_lost_running_target_sends_nothing_more)
{
	const Log abandon = {"ir:28", "ir:30", "dr16:0006", "ir:a8", "close"};
	{
		Fx f;
		f.attach();
		f.resume();
		f.tap.jtag_id = 0xFF; /* target lost while running */
		CHECK_BYTES(f.run({0x90, 0x0D}), kTargetError);
		CHECK(f.s.state() == State::Idle);
		CHECK_LOG(f.tap.log, abandon);
		CHECK_BYTES(f.run({0x90, 0x0D}), kBadState);
	}
	{
		Fx f;
		f.attach();
		f.resume();
		f.tap.jtag_id = 0xFF;
		CHECK_BYTES(f.run({0x90, 0x0B}), kTargetError);
		CHECK(f.s.state() == State::Idle);
		CHECK_LOG(f.tap.log, abandon);
	}
	{
		/* DBG_DETACH of a lost running target: TARGET_ERROR (the program
		 * does not continue where it stopped), nothing more sent */
		Fx f;
		f.attach();
		f.resume();
		f.tap.jtag_id = 0xFF;
		CHECK_BYTES(f.run({0x90, 0x0A, 0x00}), kTargetError);
		CHECK(f.s.state() == State::Idle);
		CHECK_LOG(f.tap.log, abandon);
	}
	{
		Fx f; /* abort() of a lost running target */
		f.attach();
		f.resume();
		f.tap.jtag_id = 0xFF;
		f.s.abort();
		CHECK(f.s.state() == State::Idle);
		CHECK_LOG(f.tap.log, abandon);
	}
	{
		Fx f; /* DBG_STEP whose sync fails after the release */
		f.attach();
		f.tap.eem_control_script = {0x0080, 0x0080};
		f.tap.has_cntrl_sig_after_release = true;
		f.tap.cntrl_sig_after_release = 0x0101; /* running; the sync bit never comes */
		CHECK_BYTES(f.run({0x90, 0x0E}), kTargetError);
		CHECK(f.tap.eem_control_script.empty());
		CHECK(f.s.state() == State::Idle);
		/* the Debugger's own release is the last thing sent */
		const Log tail = {"ir:a8", "close"};
		REQUIRE(f.tap.log.size() >= tail.size());
		CHECK_LOG(Log(f.tap.log.end() - 2, f.tap.log.end()), tail);
		CHECK_EQ(count_log(f.tap, "close"), 1u);
		CHECK(!log_has(f.tap, "dr16:0c01"));
		f.tap.log.clear();
		CHECK_BYTES(f.run({0x90, 0x0D}), kBadState);
		CHECK(f.tap.log.empty());
	}
}

/* A TargetError from a memory, register or breakpoint operation leaves the
 * Debugger attached (the link check failed afterwards): the Session detaches
 * with Run, as Debugger::attach does for the same failure, so the pins are
 * released and the CPU is not left halted. Here the link stays dead, so the
 * link check before the restore fails as well and the CPU gets a power-on
 * reset instead of its context (correction 26). */
TEST(session_target_error_while_halted_ends_the_session)
{
	{
		Fx f;
		f.attach();
		f.tap.cntrl_sig = 0x0000; /* the CPU is no longer under JTAG control */
		CHECK_BYTES(f.run(read_req(0x2400, 1)), kTargetError);
		CHECK(f.s.state() == State::Idle);
		CHECK(log_has(f.tap, "ir:a8"));
		CHECK(log_has(f.tap, "dr16:0c01")); /* the reset in place of the restore */
		REQUIRE(!f.tap.log.empty());
		CHECK(f.tap.log.back() == "close");
		CHECK_EQ(count_log(f.tap, "close"), 1u);
		CHECK_BYTES(f.run(read_req(0x2400, 1)), kBadState);
	}
	{
		Fx f;
		f.attach();
		f.tap.cntrl_sig = 0x0000;
		CHECK_BYTES(f.run(write_req(0x2400, 2)), kTargetError);
		/* checked before the first write, and the link check before the
		 * release fails as well: nothing written, not even WDTCTL */
		CHECK(f.tap.mem_writes.empty());
		CHECK(f.s.state() == State::Idle);
		REQUIRE(!f.tap.log.empty());
		CHECK(f.tap.log.back() == "close");
	}
	{
		Fx f;
		f.attach();
		f.tap.cntrl_sig = 0x0000;
		CHECK_BYTES(f.run({0x90, 0x0F}), kTargetError);
		CHECK(f.s.state() == State::Idle);
		REQUIRE(!f.tap.log.empty());
		CHECK(f.tap.log.back() == "close");
	}
	{
		Fx f;
		f.attach();
		f.tap.cntrl_sig = 0x0000;
		CHECK_BYTES(f.run(bp_req(0, 0x4400)), kTargetError);
		CHECK(f.s.state() == State::Idle);
		REQUIRE(!f.tap.log.empty());
		CHECK(f.tap.log.back() == "close");
	}
	{
		Fx f; /* a register written to the CPU */
		f.attach();
		f.tap.cntrl_sig = 0x0000;
		CHECK_BYTES(f.run(reg_req(4, 0x1234)), kTargetError);
		CHECK(f.s.state() == State::Idle);
		CHECK(log_has(f.tap, "ir:a8"));
		CHECK(log_has(f.tap, "dr16:0c01")); /* the reset in place of the restore */
		REQUIRE(!f.tap.log.empty());
		CHECK(f.tap.log.back() == "close");
		CHECK_EQ(count_log(f.tap, "close"), 1u);
		CHECK_BYTES(f.run({0x90, 0x0D}), kBadState);
	}
	{
		Fx f;
		f.attach();
		REQUIRE(f.run(bp_req(5, 0x4600))[1] == 0x00);
		f.tap.log.clear();
		f.tap.cntrl_sig = 0x0000;
		CHECK_BYTES(f.run({0x90, 0x12, 0x05}), kTargetError);
		CHECK(f.s.state() == State::Idle);
		CHECK(log_has(f.tap, "ir:a8"));
		CHECK(log_has(f.tap, "dr16:0c01")); /* the reset in place of the restore */
		REQUIRE(!f.tap.log.empty());
		CHECK(f.tap.log.back() == "close");
		CHECK_EQ(count_log(f.tap, "close"), 1u);
		CHECK_BYTES(f.run({0x90, 0x0D}), kBadState);
	}
}

/* The detach that follows such a TargetError makes the link check again
 * before it restores SR, WDTCTL and PC: on a dead link it feeds nothing and
 * writes nothing to memory but resets the CPU (EEM off, power-on reset
 * through the control signal, released, pins released; correction 26); on a
 * healthy link (the access's own check alone failed) it restores the context
 * as before. */
TEST(session_target_error_on_a_dead_link_resets_instead_of_restoring)
{
	for (const bool id_lost : {true, false}) {
		Fx f;
		f.attach();
		f.tap.cpu_words.clear();
		if (id_lost) {
			f.tap.jtag_id = 0xFF; /* the data line reads all ones */
		} else {
			f.tap.cntrl_sig = 0x0000; /* the data line reads all zeros */
		}
		CHECK_BYTES(f.run(read_req(0x0200, 1)), kTargetError);
		CHECK(f.s.state() == State::Idle);
		CHECK(f.tap.mem_writes.empty());
		CHECK(f.tap.cpu_words.empty()); /* no SR or PC fed */
		const Log tail = clear_triggers() + link_check(!id_lost) + link_check(!id_lost) +
				 kResetRelease;
		CHECK_LOG(last(f.tap.log, tail.size()), tail);
		CHECK_EQ(count_log(f.tap, "close"), 1u);
		f.tap.log.clear();
		CHECK_BYTES(f.run(read_req(0x0200, 1)), kBadState);
		CHECK(f.tap.log.empty());
	}
	{
		Fx f; /* healthy link: the restore of today */
		f.attach();
		f.tap.cntrl_sig_script = {0x0000}; /* READ_MEM's check alone fails */
		CHECK_BYTES(f.run(read_req(0x0200, 1)), kTargetError);
		CHECK(f.tap.cntrl_sig_script.empty());
		CHECK(f.s.state() == State::Idle);
		const Log tail = detach_run(0x4520, 0x0008, 0x6904);
		CHECK_LOG(last(f.tap.log, tail.size()), tail);
		REQUIRE(f.tap.mem_writes.size() == 1u);
		CHECK(f.tap.mem_writes[0] == (std::pair<uint32_t, uint16_t>{0x015C, 0x5A04}));
	}
}

TEST(session_dbg_fixed_size_requests)
{
	Fx f;
	f.attach();
	CHECK_BYTES(f.run({0x90, 0x0B, 0x00}), kBadArg);
	CHECK_BYTES(f.run({0x90, 0x0D, 0x00}), kBadArg);
	CHECK_BYTES(f.run({0x90, 0x0E, 0x00}), kBadArg);
	CHECK_BYTES(f.run({0x90, 0x0C, 0x00}), kBadArg);
	CHECK_BYTES(f.run({0x90, 0x0F, 0x00}), kBadArg);
	CHECK(f.s.state() == State::DebugHalted);
	CHECK(f.tap.log.empty());

	uint8_t small[5] = {}; /* DBG_HALT, DBG_STEP: 2 + 4 bytes needed */
	CHECK_EQ(f.s.handle(Bytes{0x90, 0x0B}, small), 2u);
	CHECK_EQ(small[1], 0x01);
	CHECK_EQ(f.s.handle(Bytes{0x90, 0x0E}, small), 2u);
	CHECK_EQ(small[1], 0x01);
	uint8_t small7[8] = {}; /* DBG_STATUS: 2 + 7 bytes needed */
	CHECK_EQ(f.s.handle(Bytes{0x90, 0x0D}, small7), 2u);
	CHECK_EQ(small7[1], 0x01);
	CHECK(f.tap.log.empty());
	CHECK(f.s.state() == State::DebugHalted);
}

/* ---- DBG_EEM_PEEK (0x13, design spec correction 25) ---- */

namespace {

/* DBG_EEM_PEEK: u16 speed_khz, u8 eem_addr */
Bytes peek_req(uint8_t addr = 0x00, uint16_t khz = 250)
{
	return {0x90, 0x13, static_cast<uint8_t>(khz), static_cast<uint8_t>(khz >> 8), addr};
}

} // namespace

TEST(session_dbg_eem_peek_returns_the_jtag_id_and_both_reads)
{
	Fx f;
	f.tap.eem_read_script = {0x89ABCDEF, 0x01234567};
	CHECK_BYTES(f.run(peek_req(0x06, 500)),
		    (Bytes{0x90, 0x00, 0x91,             /* JTAG ID */
			   0xEF, 0xCD, 0xAB, 0x89,       /* first read */
			   0x67, 0x45, 0x23, 0x01}));    /* second read */
	CHECK(f.s.state() == State::Idle);
	CHECK_LOG(f.tap.log, Log{"speed:500000", "open", "enter:high", "ir:28",
				 "ir:28", "dr16:aaaa", "ir:28", "dr16:aaaa", "ir:28", "dr16:aaaa",
				 "ir:e8", "dr16:0000", "ir:e1", "dr20:00000",
				 "ir:b0", "dr32:00000007", "dr32:00000000", "dr32:00000007",
				 "dr32:00000000", "ir:28", "close"});
	CHECK(f.tap.mem_writes.empty());
	CHECK(f.tap.eem_writes.empty());
	CHECK(f.tap.eem_control_writes.empty());
	CHECK(f.tap.cpu_words.empty());
	CHECK(!log_has(f.tap, "ir:c8")); /* no control-signal write */
	/* INFO still reports Idle; a debug session can follow */
	const Bytes info = f.run({0x90, 0x00});
	REQUIRE(info.size() > 3u);
	CHECK_EQ(info[3], 0);
	f.cpu_runs();
	CHECK(f.run(attach_req())[1] == 0x00);
	CHECK(f.s.state() == State::DebugHalted);
}

TEST(session_dbg_eem_peek_validates_arguments)
{
	Fx f;
	CHECK_BYTES(f.run(peek_req(0x00, 99)), kBadArg);   /* speed */
	CHECK_BYTES(f.run(peek_req(0x00, 10001)), kBadArg); /* speed */
	CHECK_BYTES(f.run(peek_req(0x00, 0)), kBadArg);    /* speed */
	CHECK_BYTES(f.run(peek_req(0x01)), kBadArg);       /* odd: the read bit is the probe's */
	CHECK_BYTES(f.run(peek_req(0x3F)), kBadArg);
	CHECK_BYTES(f.run(peek_req(0x40)), kBadArg);       /* past trigger block 7 */
	CHECK_BYTES(f.run(peek_req(0x80)), kBadArg);       /* BREAKREACT */
	CHECK_BYTES(f.run(peek_req(0x88)), kBadArg);       /* GENCLKCTRL */
	CHECK_BYTES(f.run(peek_req(0xFE)), kBadArg);
	Bytes shortreq = peek_req();
	shortreq.pop_back();
	CHECK_BYTES(f.run(shortreq), kBadArg);
	Bytes longreq = peek_req();
	longreq.push_back(0);
	CHECK_BYTES(f.run(longreq), kBadArg);
	CHECK_BYTES(f.run({0x90, 0x13}), kBadArg);
	uint8_t small[10] = {}; /* 2 + 9 bytes needed */
	CHECK_EQ(f.s.handle(peek_req(), small), 2u);
	CHECK_EQ(small[1], 0x01);
	CHECK(f.tap.log.empty());
	CHECK(f.s.state() == State::Idle);

	/* the limits */
	uint8_t enough[11] = {};
	CHECK_EQ(f.s.handle(peek_req(0x3E, 100), enough), 11u);
	CHECK_EQ(enough[1], 0x00);
	REQUIRE(!f.tap.log.empty());
	CHECK(f.tap.log.front() == "speed:100000");
	CHECK(log_has(f.tap, "dr32:0000003f"));
	f.tap.log.clear();
	CHECK_BYTES(f.run(peek_req(0x00, 10000)),
		    (Bytes{0x90, 0x00, 0x91, 0, 0, 0, 0, 0, 0, 0, 0}));
	REQUIRE(!f.tap.log.empty());
	CHECK(f.tap.log.front() == "speed:10000000");
}

/* The identify phase's statuses as DBG_ATTACH reports them (NO_TARGET with
 * the ID), and TARGET_ERROR when the JTAG ID around the reads is wrong. The
 * pins are released and the session stays Idle every time. */
TEST(session_dbg_eem_peek_failures_leave_idle)
{
	{
		Fx f;
		f.tap.jtag_id = 0xFF;
		CHECK_BYTES(f.run(peek_req()), (Bytes{0x90, 0x04, 0xFF}));
		CHECK(f.s.state() == State::Idle);
		REQUIRE(!f.tap.log.empty());
		CHECK(f.tap.log.back() == "close");
	}
	{
		Fx f;
		f.tap.cntrl_sig_script = {0x5555};
		CHECK_BYTES(f.run(peek_req()), (Bytes{0x90, 0x05}));
		CHECK(f.s.state() == State::Idle);
		REQUIRE(!f.tap.log.empty());
		CHECK(f.tap.log.back() == "close");
	}
	{
		Fx f;
		f.tap.open_ok = false;
		CHECK_BYTES(f.run(peek_req()), (Bytes{0x90, 0x03}));
		CHECK(f.s.state() == State::Idle);
	}
	{
		Fx f;
		f.tap.ones_after_shifts = 16; /* the capture after the reads floats */
		CHECK_BYTES(f.run(peek_req()), kTargetError);
		CHECK(f.s.state() == State::Idle);
		REQUIRE(!f.tap.log.empty());
		CHECK(f.tap.log.back() == "close");
		CHECK_EQ(count_log(f.tap, "close"), 1u); /* nothing more after the release */
	}
}

/* Only in Idle: in Connected, Flashing, DebugHalted and DebugRunning it is
 * BAD_STATE (before the size check) and nothing is sent; the session stays
 * as it was. */
TEST(session_dbg_eem_peek_only_in_idle)
{
	const Bytes reqs[] = {peek_req(), {0x90, 0x13}, peek_req(0x01)};
	{
		Fx f; /* Connected */
		f.tap.quick_words = {0x2955};
		REQUIRE(f.run(kConnectReq)[1] == 0x00);
		f.tap.log.clear();
		for (const Bytes &req : reqs) {
			CHECK_BYTES(f.run(req), kBadState);
		}
		CHECK(f.tap.log.empty());
		CHECK(f.s.state() == State::Connected);

		f.tap.push_jmb_out(0xABADBABE); /* Flashing */
		REQUIRE(f.run(begin_req(0x4400, 2, 0x2400))[1] == 0x00);
		REQUIRE(f.s.state() == State::Flashing);
		f.tap.log.clear();
		f.tap.mem_writes.clear();
		for (const Bytes &req : reqs) {
			CHECK_BYTES(f.run(req), kBadState);
		}
		CHECK(f.tap.log.empty());
		CHECK(f.tap.mem_writes.empty());
		CHECK(f.s.state() == State::Flashing);
	}
	for (const bool running : {false, true}) {
		Fx f;
		f.open_session(running);
		for (const Bytes &req : reqs) {
			CHECK_BYTES(f.run(req), kBadState);
		}
		CHECK(f.tap.log.empty());
		CHECK(f.tap.mem_writes.empty());
		CHECK(f.s.state() == (running ? State::DebugRunning : State::DebugHalted));
		if (!running) {
			CHECK_BYTES(f.run({0x90, 0x0D}),
				    (Bytes{0x90, 0x00, 0x01, 0x01, 0xFF, 0x20, 0x45, 0, 0}));
		}
	}
}

/* The device ID comes from the quick read (correction 27): the probe knows
 * the F5529, so its RAM and peripheral windows are open, and READ_MEM of the
 * bootcode area returns its words (the address register would read
 * 0x3FFF there). */
TEST(session_dbg_attach_identifies_the_f5529_with_the_quick_read)
{
	Fx f;
	f.cpu_runs();
	const Bytes r = f.run(attach_req());
	REQUIRE(r.size() == 17u);
	CHECK_EQ(r[1], 0x00);
	CHECK_EQ(r[9], 0x55); /* device ID bytes 55h 29h */
	CHECK_EQ(r[10], 0x29);
	CHECK(log_has(f.tap, "ir:c2"));
	CHECK_BYTES(f.run(write_req(0x2400, 2)), kOk); /* RAM */
	CHECK_BYTES(f.run(write_req(0x43FE, 2)), kOk);
	CHECK_BYTES(f.run(write_req(0x0200, 2)), kOk); /* a peripheral register */
	CHECK_BYTES(f.run(read_req(0x1A04, 1)), (Bytes{0x90, 0x00, 0x55, 0x29}));
}
