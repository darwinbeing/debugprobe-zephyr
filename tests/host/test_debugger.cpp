#include "testing.hpp"
#include "fake_tap.hpp"
#include "debug_logs.hpp"
#include "msp430/debug.hpp"

#include <algorithm>
#include <string>
#include <vector>

using msp430::AttachMode;
using msp430::Debugger;
using msp430::DetachAction;
using msp430::Status;
using msp430::StopReason;
using namespace dbglog;

namespace {

struct Fx {
	FakeTap t;
	msp430::Cpuxv2 cpu{t};
	Debugger d{t, cpu};
	msp430::Identity id;

	/* No test here provokes a protocol error on purpose: whatever the
	 * Debugger does, the fake's mailbox, EEM exchange, password and
	 * memory-access rules must hold. */
	~Fx()
	{
		CHECK(!t.jmb_protocol_error);
		CHECK(!t.eem_protocol_error);
		CHECK(!t.password_violation);
		CHECK(!t.mem_protocol_error);
	}

	/* The CPU runs and is not in a low-power mode: the first EEM control
	 * read says "not stopped", the one after the stop request "stopped".
	 * The SR read during the sync returns `sr`. */
	void running_cpu(uint16_t sr = 0x0008)
	{
		t.eem_control_script = {0x0000, 0x0080};
		t.captured = {sr, 0x0000};
	}

	/* Every scripted answer was used. */
	bool scripts_consumed() const
	{
		return t.captured.empty() && t.eem_control_script.empty() &&
		       t.cntrl_sig_script.empty() && t.mab_script.empty();
	}

	Status attach()
	{
		running_cpu();
		return d.attach(AttachMode::Running, 250000, id);
	}

	/* attach(), then forget the transcript and the recordings */
	void attached()
	{
		REQUIRE(attach() == Status::Ok);
		REQUIRE(scripts_consumed());
		t.log.clear();
		t.mem_writes.clear();
		t.eem_writes.clear();
		t.eem_control_writes.clear();
	}
};

const unsigned kTb0Zero[5] = {0, 0, 0, 0, 0};

} // namespace

TEST(debugger_attach_running_follows_mspds_conditional_sync)
{
	Fx f;
	CHECK(f.attach() == Status::Ok);
	const Log want = identify_log() + eem_stop(1, kTb0Zero) + sync_tail(0x6904, 0x0008) +
			 read_word(0x1A04) + clear_triggers() + Log{"ir:28", "dr16:0000"};
	CHECK_LOG(f.t.log, want);
	CHECK(f.d.attached());
	CHECK(!f.d.running());
	CHECK(f.d.stop_reason() == StopReason::Request);
	CHECK_EQ(f.id.device_id, 0x2955);
}

TEST(debugger_attach_saves_pc_sr_and_watchdog)
{
	Fx f;
	f.t.mab = 0x14A2C; /* the MAB points 4 bytes past the next instruction */
	f.t.memory[0x015C] = 0x6936;
	f.running_cpu(0x0009); /* GIE + C */
	REQUIRE(f.d.attach(AttachMode::Running, 250000, f.id) == Status::Ok);
	CHECK_EQ(f.d.pc(), 0x14A28u);
	/* watchdog held, its other bits kept; SR written back without GIE */
	REQUIRE(f.t.mem_writes.size() == 1u); /* SR goes through MOVA, not a memory write */
	CHECK_EQ(f.t.mem_writes[0].first, 0x015Cu);
	CHECK_EQ(f.t.mem_writes[0].second, 0x5AB6);
	CHECK(contains(f.t.log, write_reg(0x0082, 0x0001)));
}

TEST(debugger_attach_restores_trigger_block_0)
{
	Fx f;
	f.t.eem[0x00] = 0x04400;
	f.t.eem[0x02] = 0x0000;
	f.t.eem[0x04] = 0x00000;
	f.t.eem[0x06] = 0x0001;
	f.t.eem[0x80] = 0x0001;
	REQUIRE(f.attach() == Status::Ok);
	const unsigned tb0[5] = {0x04400, 0, 0, 1, 1};
	CHECK(contains(f.t.log, eem_stop(1, tb0)));
}

TEST(debugger_attach_in_each_low_power_mode)
{
	/* LPM0..LPM4: CPUOFF plus SCG0, SCG1, OSCOFF as the mode requires, GIE set */
	for (uint16_t sr : {0x0018, 0x0058, 0x0098, 0x00D8, 0x00F8}) {
		Fx f;
		f.t.cntrl_sig = 0x0321; /* CPUOFF: the CPU is off */
		f.t.eem_control_script = {0x0000};
		f.t.captured = {sr, 0x0000};
		f.t.mab = 0x0453E;
		REQUIRE(f.d.attach(AttachMode::Running, 250000, f.id) == Status::Ok);
		CHECK(f.scripts_consumed());
		/* no EEM stop: straight to the sync */
		const Log want = identify_log() + Log{"ir:d0", "dr16:0000", "ir:28", "dr16:0000"} +
				 sync_tail(0x6904, sr);
		REQUIRE(f.t.log.size() >= want.size());
		CHECK_LOG(Log(f.t.log.begin(), f.t.log.begin() + static_cast<long>(want.size())), want);
		CHECK_EQ(f.d.pc(), 0x0453Cu); /* MAB - 2 in a low-power mode */
		/* the CPU gets SR without CPUOFF and GIE; the context keeps all bits
		 * and gives them back on release, with the PC 2 lower (Note 1411) */
		CHECK(contains(f.t.log, write_reg(0x0082, sr & 0xFFE7u)));
		f.t.log.clear();
		f.d.detach(DetachAction::Run);
		CHECK(contains(f.t.log, write_reg(0x0082, sr)));
		CHECK(contains(f.t.log, set_pc(0x453A)));
	}
}

TEST(debugger_attach_when_the_eem_has_already_stopped_the_cpu)
{
	Fx f;
	f.t.eem_control_script = {0x0080};
	f.t.captured = {0x0008, 0x0000};
	REQUIRE(f.d.attach(AttachMode::Running, 250000, f.id) == Status::Ok);
	CHECK(f.scripts_consumed());
	const Log want = identify_log() + Log{"ir:d0", "dr16:0000"} + sync_tail(0x6904, 0x0008);
	REQUIRE(f.t.log.size() >= want.size());
	CHECK_LOG(Log(f.t.log.begin(), f.t.log.begin() + static_cast<long>(want.size())), want);
	CHECK(f.t.eem_writes.size() == 9u); /* only the trigger clearing */
}

TEST(debugger_attach_clocks_through_the_wait_signal)
{
	Fx f;
	f.running_cpu();
	/* Note 805: the control-signal read after 0x1501 shows "wait" twice.
	 * The first five reads are the lock check (3), the CPUOFF check and
	 * the sync. */
	f.t.cntrl_sig_script = {0x0301, 0x0301, 0x0301, 0x0301, 0x0301,
				0x0309, 0x0309, 0x0301};
	REQUIRE(f.d.attach(AttachMode::Running, 250000, f.id) == Status::Ok);
	CHECK(f.scripts_consumed());
	CHECK(contains(f.t.log, Log{"ir:c8", "dr16:1501", "ir:28", "dr16:0000",
				    "tclk:0", "tclk:1", "ir:28", "dr16:0000",
				    "tclk:0", "tclk:1", "ir:28", "dr16:0000",
				    "ir:28", "tclk:0", "dr16:0000", "tclk:1"}));
}

TEST(debugger_attach_reads_the_pc_from_the_reset_vector_at_fffe)
{
	/* MAB - 4 = 0xFFFFE (wrapped below 0) or 0x0FFFE: Note 1495, 1637 */
	for (uint32_t mab : {0x00002u, 0x10002u}) {
		Fx f;
		f.t.mab = mab;
		REQUIRE(f.attach() == Status::Ok);
		CHECK_EQ(f.d.pc(), 0x441Au);
		CHECK(contains(f.t.log, Log{"ir:21", "dr20:00000", "ir:28", "dr16:0000"} +
						read_word(0xFFFE)));
	}
}

TEST(debugger_attach_keeps_a_pc_ending_in_fffe_outside_the_vector)
{
	/* 0x1FFFE is in the F5529's main flash; MSPDS would read the reset
	 * vector for any PC ending in FFFE. */
	{
		Fx f;
		f.t.mab = 0x20002;
		REQUIRE(f.attach() == Status::Ok);
		CHECK_EQ(f.d.pc(), 0x1FFFEu);
		CHECK(!contains(f.t.log, read_word(0xFFFE)));
	}
	{
		Fx f; /* low-power mode: PC = MAB - 2 */
		f.t.cntrl_sig = 0x0321;
		f.t.eem_control_script = {0x0000};
		f.t.captured = {0x0018, 0x0000};
		f.t.mab = 0x20000;
		REQUIRE(f.d.attach(AttachMode::Running, 250000, f.id) == Status::Ok);
		CHECK_EQ(f.d.pc(), 0x1FFFEu);
		CHECK(!contains(f.t.log, read_word(0xFFFE)));
	}
}

TEST(debugger_attach_waits_for_the_flash_controller)
{
	Fx f;
	f.running_cpu();
	f.t.memory[0x0144] = 0x9659; /* BUSY */
	f.t.millis_step = 30;
	REQUIRE(f.d.attach(AttachMode::Running, 250000, f.id) == Status::Ok);
	/* polled until the 100 ms limit, never written */
	size_t reads = 0;
	for (size_t i = 0; i + 2 < f.t.log.size(); ++i) {
		if (f.t.log[i] == "dr20:00144") {
			++reads;
		}
	}
	CHECK(reads >= 3u);
	for (const auto &w : f.t.mem_writes) {
		CHECK(w.first != 0x0144u);
	}
	/* the watchdog is held first, so it cannot reset the device during
	 * the wait */
	const Log hold = write_word(0x015C, 0x5A84);
	const auto hold_at = std::search(f.t.log.begin(), f.t.log.end(), hold.begin(), hold.end());
	const auto first_poll = std::find(f.t.log.begin(), f.t.log.end(), "dr20:00144");
	REQUIRE(hold_at != f.t.log.end());
	CHECK(hold_at < first_poll);
}

TEST(debugger_attach_refuses_a_watchdog_without_its_password)
{
	Fx f;
	f.running_cpu();
	f.t.memory[0x015C] = 0x0000; /* link fault: not a WDTCTL read */
	CHECK(f.d.attach(AttachMode::Running, 250000, f.id) == Status::TargetError);
	CHECK(f.t.mem_writes.empty());
	CHECK(!f.d.attached());
	/* The CPU is in full emulation: it is let go at the PC where it stopped,
	 * with SR and WDTCTL untouched. The flash controller is not polled. */
	const Log want = read_word(0x015C) + release_at(0x4520, false, 0x0006) + Log{"close"};
	CHECK_LOG(last(f.t.log, want.size()), want);
	CHECK(!contains(f.t.log, Log{"dr20:00144"}));
}

TEST(debugger_attach_with_a_failed_init_state_check_releases_at_the_pc)
{
	Fx f;
	f.running_cpu();
	/* Eight control-signal reads up to the context save (lock check 3,
	 * CPUOFF check, sync, wait check, CPUSUSP, i_SaveContext); the
	 * init-state check then sees none of the bits 0x0301. */
	f.t.cntrl_sig_script = {0x0301, 0x0301, 0x0301, 0x0301, 0x0301,
				0x0301, 0x0301, 0x0301, 0x0000};
	CHECK(f.d.attach(AttachMode::Running, 250000, f.id) == Status::TargetError);
	CHECK(f.t.cntrl_sig_script.empty());
	CHECK(f.t.mem_writes.empty());
	CHECK(!f.d.attached());
	const Log want = Log{"ir:30", "dr16:000e", "ir:28", "dr16:0000"} +
			 release_at(0x4520, false, 0x0006) + Log{"close"};
	CHECK_LOG(last(f.t.log, want.size()), want);
}

TEST(debugger_attach_failure_in_a_low_power_mode_releases_at_pc_minus_2)
{
	Fx f;
	f.t.cntrl_sig = 0x0321; /* CPUOFF */
	f.t.eem_control_script = {0x0000};
	f.t.mab = 0x0453E;
	f.t.memory[0x015C] = 0x0000;
	CHECK(f.d.attach(AttachMode::Running, 250000, f.id) == Status::TargetError);
	CHECK(f.t.mem_writes.empty());
	/* PC = MAB - 2, released 2 lower as a normal release does (Note 1411) */
	const Log want = read_word(0x015C) + release_at(0x453C, true, 0x0006) + Log{"close"};
	CHECK_LOG(last(f.t.log, want.size()), want);
}

TEST(debugger_attach_without_sync_releases_and_writes_nothing)
{
	Fx f;
	f.running_cpu();
	f.t.cntrl_sig = 0x0101; /* sync bit never set */
	CHECK(f.d.attach(AttachMode::Running, 250000, f.id) == Status::TargetError);
	CHECK(f.t.mem_writes.empty());
	CHECK(!f.d.attached());
	REQUIRE(f.t.log.size() >= 4u);
	CHECK_LOG(Log(f.t.log.end() - 4, f.t.log.end()),
		  (Log{"ir:30", "dr16:0006", "ir:a8", "close"}));
}

TEST(debugger_attach_when_the_pipe_never_empties)
{
	Fx f;
	f.running_cpu();
	f.t.cntrl_sig = 0x0201; /* synchronized, CPUSUSP never set */
	CHECK(f.d.attach(AttachMode::Running, 250000, f.id) == Status::TargetError);
	CHECK(f.t.mem_writes.empty());
	REQUIRE(!f.t.log.empty());
	CHECK(f.t.log.back() == "close");
}

TEST(debugger_attach_passes_on_identify_failures)
{
	{
		Fx f;
		f.t.jtag_id = 0xFF;
		CHECK(f.d.attach(AttachMode::Running, 250000, f.id) == Status::NoTarget);
		CHECK(!f.d.attached());
	}
	{
		Fx f;
		f.t.cntrl_sig_script = {0x5555};
		CHECK(f.d.attach(AttachMode::Running, 250000, f.id) == Status::Locked);
		CHECK(!f.d.attached());
		CHECK(f.t.mem_writes.empty());
	}
}

TEST(debugger_detach_restores_the_context_and_lets_the_program_run)
{
	Fx f;
	f.attached();
	f.d.detach(DetachAction::Run);
	CHECK_LOG(f.t.log, clear_triggers() + restore_release(0x4520, 0x0008, 0x6904, 0x0006) +
				   Log{"close"});
	CHECK(!f.d.attached());
	/* the watchdog gets its original bits back, without HOLD */
	REQUIRE(!f.t.mem_writes.empty());
	CHECK_EQ(f.t.mem_writes.back().second, 0x5A04);
}

TEST(debugger_detach_with_reset)
{
	Fx f;
	f.attached();
	f.d.detach(DetachAction::Reset);
	CHECK_LOG(f.t.log, clear_triggers() + Log{"ir:30", "dr16:0006", "ir:c8", "dr16:0c01",
						  "dr16:0401", "ir:a8", "close"});
	CHECK(!f.d.attached());
}
