#include "testing.hpp"
#include "fake_tap.hpp"
#include "debug_logs.hpp"
#include "msp430/debug.hpp"

#include <algorithm>
#include <string>
#include <utility>
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
	 * memory-access rules must hold, and nothing is written to flash or the
	 * flash controller. A test that passes write_mem a flash address on
	 * purpose checks the flag false right before that write, so that no
	 * earlier flash write goes unnoticed, then checks it set and clears
	 * it. */
	~Fx()
	{
		CHECK(!t.jmb_protocol_error);
		CHECK(!t.eem_protocol_error);
		CHECK(!t.password_violation);
		CHECK(!t.mem_protocol_error);
		CHECK(!t.flash_write_attempt);
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
	CHECK(f.t.eem_writes.size() == 10u); /* only the trigger clearing */
}

TEST(debugger_attach_and_detach_turn_the_eem_sequencer_off)
{
	/* A sequencer left on by an earlier session drives reaction bit 7 in
	 * place of combination 7 (the F5529's EEM: TriggerManager430(8, 2, 10,
	 * 7)); the DLL's EEM_Init writes SEQ_CTL 0 every session. */
	Fx f;
	f.t.eem[0xA6] = 0x0003; /* SEQ_ENABLE + SEQ_RESET_EN */
	REQUIRE(f.attach() == Status::Ok);
	CHECK_EQ(f.t.eem[0xA6], 0u);
	f.t.eem[0xA6] = 0x0003;
	f.d.detach(DetachAction::Run);
	CHECK_EQ(f.t.eem[0xA6], 0u);
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
	CHECK(!f.d.attached());
	CHECK_LOG(last(f.t.log, 4), (Log{"ir:30", "dr16:0006", "ir:a8", "close"}));
}

TEST(debugger_attach_refuses_a_wrong_jtag_id_in_the_sync)
{
	Fx f;
	f.t.eem_control_script = {0x0080}; /* already stopped: no EEM stop */
	/* The tenth IR shift is the capture in SyncJtagXv2 (identify 6, EEM
	 * control read, clock control write, IR_CNTRL_SIG_16BIT). */
	f.t.jtag_id_script = {0x91, 0x91, 0x91, 0x91, 0x91, 0x91, 0x91, 0x91, 0x91, 0xFF};
	CHECK(f.d.attach(AttachMode::Running, 250000, f.id) == Status::TargetError);
	CHECK(f.scripts_consumed());
	CHECK(f.t.jtag_id_script.empty());
	CHECK(f.t.mem_writes.empty());
	CHECK(!f.d.attached());
	/* no sync poll after the wrong ID: released with nothing written */
	CHECK_LOG(last(f.t.log, 7), (Log{"ir:c8", "dr16:1501", "ir:28", "ir:30", "dr16:0006",
					 "ir:a8", "close"}));
}

TEST(debugger_attach_proceeds_when_the_eem_stop_never_comes)
{
	Fx f;
	f.t.captured = {0x0008, 0x0000}; /* the EEM control register reads 0 throughout */
	REQUIRE(f.d.attach(AttachMode::Running, 250000, f.id) == Status::Ok);
	CHECK(f.scripts_consumed());
	/* MSPDS polls 3000 times, restores trigger block 0 and syncs anyway */
	const Log want = identify_log() + eem_stop(3000, kTb0Zero) + sync_tail(0x6904, 0x0008);
	CHECK_LOG(first(f.t.log, want.size()), want);
}

TEST(debugger_attach_when_the_cpu_enters_a_low_power_mode_before_the_save)
{
	Fx f;
	f.running_cpu(0x0018); /* the SR read finds CPUOFF + GIE */
	/* running at the CPUOFF check (read 4), off from the context save on
	 * (read 8) */
	f.t.cntrl_sig_script = {0x0301, 0x0301, 0x0301, 0x0301, 0x0301, 0x0301, 0x0301};
	f.t.cntrl_sig = 0x0321;
	f.t.mab = 0x0453E;
	REQUIRE(f.d.attach(AttachMode::Running, 250000, f.id) == Status::Ok);
	CHECK(f.scripts_consumed());
	CHECK(contains(f.t.log, eem_stop(1, kTb0Zero)));
	CHECK_EQ(f.d.pc(), 0x0453Cu); /* MAB - 2 */
	f.t.log.clear();
	f.d.detach(DetachAction::Run);
	/* released 2 lower again (Note 1411) */
	CHECK_LOG(f.t.log, clear_triggers() + restore_release(0x453C, 0x0018, 0x6904, 0x0006) +
				   Log{"close"});
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

TEST(debugger_detach_keeps_a_watchdog_the_firmware_holds_held)
{
	Fx f;
	f.t.memory[0x015C] = 0x6984; /* WDTHOLD set by the firmware */
	REQUIRE(f.attach() == Status::Ok);
	f.d.detach(DetachAction::Run);
	/* held on attach, given back on detach with HOLD still set */
	REQUIRE(f.t.mem_writes.size() == 2u);
	for (const auto &w : f.t.mem_writes) {
		CHECK_EQ(w.first, 0x015Cu);
		CHECK_EQ(w.second, 0x5A84);
	}
	CHECK_EQ(f.t.memory[0x015C], 0x6984);
}

TEST(debugger_detach_clocks_a_cpu_in_halt_with_0403)
{
	Fx f;
	f.attached();
	/* the control-signal reads of SetPcXv2 and of the release clock; the
	 * second shows HALT (0x0002) */
	f.t.cntrl_sig_script = {0x0301, 0x0303};
	f.d.detach(DetachAction::Run);
	CHECK(f.t.cntrl_sig_script.empty());
	CHECK_LOG(f.t.log, clear_triggers() +
				   restore_release(0x4520, 0x0008, 0x6904, 0x0006, true) +
				   Log{"close"});
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

namespace {

/* SyncJtag_AssertPor_SaveContextXv2.c:198-201: the system clocks back on
 * their own sources, stopped while the CPU is halted (GENCLKCTRL 0x000E) */
Log restore_clocks()
{
	return Log{ir(0xB0)} + eem_write(0x88, 0x000E);
}

/* SyncJtag_AssertPor_SaveContextXv2.c:96-100: 0x1501, then wait_for_synch
 * with `polls` control-signal reads */
Log sync_wait(unsigned polls)
{
	Log l = {ir(0xC8), d16(0x1501), ir(0x28)};
	for (unsigned i = 0; i < polls; ++i) {
		l.push_back(d16(0));
	}
	return l;
}

/* SyncJtag_AssertPor_SaveContextXv2.c:68-228, JTAG ID 0x91, sync at the
 * first look, SFRIFG1 without the vacant-memory flag. GENCLKCTRL first gets
 * MCLK_SEL3 + SMCLK_SEL3 + ACLK_SEL3 + STOP_MCLK + STOP_SMCLK + STOP_ACLK
 * (line 91) = 0x6000 + 0x0C00 + 0x0180 + 0x0008 + 0x0004 + 0x0002 = 0x6D8E
 * (EEM_defs.h:53-58); the HAL image in Bios/include/UifHalMsp430.h:764 loads
 * the same constant there (MOV #0x6D8E, R12; MOV #0, R13). */
Log assert_por_log(unsigned wdt)
{
	return Log{ir(0x28), ir(0xB0)} + eem_write(0x88, 0x6D8E) + Log{ir(0x30), d16(0x0005)} +
	       sync_wait(1) + kTclk +
	       Log{ir(0xC8), d16(0x0C01), "delay:40", d16(0x0401), ir(0x82)} + kTclk + kTclk +
	       Log{d16(0x0004)} + kTclk + kTclk + Log{ir(0x42)} + kTclk + kTclk +
	       Log{ir(0xC8), d16(0x0501)} + kTclk + Log{ir(0x30), d16(0x000E), ir(0x28), d16(0)} +
	       read_word(0x015C) + write_word(0x015C, 0x5A80u | (wdt & 0xFFu)) +
	       Log{ir(0x21), d20(0)} + read_word(0xFFFE) + restore_clocks() + read_word(0x0102);
}

/* EEM off, then ReleaseDevice_430Xv2(V_RESET) (Cpuxv2::disconnect) */
const Log kResetRelease = {"ir:30", "dr16:0006", "ir:c8", "dr16:0c01", "dr16:0401", "ir:a8",
			   "close"};

/* abandon(): EEM off, released */
const Log kAbandon = {"ir:30", "dr16:0006", "ir:a8", "close"};

} // namespace

TEST(debugger_reset_and_halt_follows_mspds_assert_por)
{
	Fx f;
	REQUIRE(f.d.attach(AttachMode::Reset, 250000, f.id) == Status::Ok);
	const Log want = identify_log() + assert_por_log(0x6904) + read_word(0x1A04) +
			 clear_triggers() + Log{"ir:28", "dr16:0000"};
	CHECK_LOG(f.t.log, want);
	CHECK_EQ(f.d.pc(), 0x441Au); /* the reset vector's target */
	CHECK(f.d.stop_reason() == StopReason::Reset);
	f.t.log.clear();
	f.d.detach(DetachAction::Run);
	CHECK(contains(f.t.log, write_reg(0x0082, 0))); /* SR is 0 after a POR */
	CHECK_EQ(f.t.eem[0x88], 0x000Eu);
}

TEST(debugger_reset_and_halt_clears_the_vacant_memory_flag)
{
	Fx f;
	f.t.memory[0x0102] = 0x000C; /* VMAIFG and another flag */
	REQUIRE(f.d.attach(AttachMode::Reset, 250000, f.id) == Status::Ok);
	CHECK(contains(f.t.log, read_word(0x0102) + set_pc(0x0004) +
					Log{ir(0xC8), d16(0x0501), "tclk:1", ir(0x21)} +
					write_word(0x0102, 0x0004)));
	/* the CPU's PC was moved for the write; the saved one is still the
	 * reset vector's target */
	CHECK_EQ(f.d.pc(), 0x441Au);
	CHECK_EQ(f.t.eem[0x88], 0x000Eu);
}

TEST(debugger_reset_and_halt_sync_wait_succeeds_on_the_50th_poll)
{
	Fx f;
	/* lock check 3, then 49 polls without the sync bit and one with it */
	f.t.cntrl_sig_script = {0x0301, 0x0301, 0x0301};
	f.t.cntrl_sig_script.insert(f.t.cntrl_sig_script.end(), 49, uint16_t{0x0101});
	f.t.cntrl_sig_script.push_back(0x0301);
	REQUIRE(f.d.attach(AttachMode::Reset, 250000, f.id) == Status::Ok);
	CHECK(f.scripts_consumed());
	CHECK(contains(f.t.log, sync_wait(50) + kTclk + Log{ir(0xC8), d16(0x0C01)}));
	CHECK_EQ(f.t.eem[0x88], 0x000Eu);
}

TEST(debugger_reset_and_halt_without_sync_releases)
{
	Fx f;
	f.t.cntrl_sig = 0x0101;
	CHECK(f.d.attach(AttachMode::Reset, 250000, f.id) == Status::TargetError);
	CHECK(!contains(f.t.log, Log{"dr16:0c01"})); /* no POR without sync */
	CHECK(f.t.mem_writes.empty());
	CHECK(!f.d.attached());
	/* 50 polls, then the clocks go back to their sources before the release */
	const Log want = sync_wait(50) + restore_clocks() + kAbandon;
	CHECK_LOG(last(f.t.log, want.size()), want);
	CHECK_EQ(f.t.eem[0x88], 0x000Eu);
}

TEST(debugger_reset_and_halt_refuses_a_wrong_jtag_id_in_the_sync_wait)
{
	Fx f;
	/* The eleventh IR shift is the capture of wait_for_synch (identify 6,
	 * the macro's capture, EEM exchange, EEM control, IR_CNTRL_SIG_16BIT). */
	f.t.jtag_id_script = {0x91, 0x91, 0x91, 0x91, 0x91, 0x91, 0x91, 0x91, 0x91, 0x91, 0xFF};
	CHECK(f.d.attach(AttachMode::Reset, 250000, f.id) == Status::TargetError);
	CHECK(f.t.jtag_id_script.empty());
	CHECK(f.t.mem_writes.empty());
	CHECK(!f.d.attached());
	/* no poll after the wrong ID: clocks back, released with nothing written */
	const Log want = sync_wait(0) + restore_clocks() + kAbandon;
	CHECK_LOG(last(f.t.log, want.size()), want);
	CHECK_EQ(f.t.eem[0x88], 0x000Eu);
}

TEST(debugger_reset_and_halt_refuses_another_jtag_id)
{
	Fx f;
	/* The seventh IR shift is the macro's own capture (identify 6). */
	f.t.jtag_id_script = {0x91, 0x91, 0x91, 0x91, 0x91, 0x91, 0x98};
	CHECK(f.d.attach(AttachMode::Reset, 250000, f.id) == Status::TargetError);
	CHECK(f.t.jtag_id_script.empty());
	CHECK(f.t.mem_writes.empty());
	CHECK(!f.d.attached());
	/* no clock switch, no POR: released at once with nothing written */
	CHECK_LOG(f.t.log, identify_log() + Log{"ir:28"} + kAbandon);
	CHECK(f.t.eem_writes.empty()); /* GENCLKCTRL never written */
}

TEST(debugger_reset_and_halt_with_a_failed_init_state_check_resets_again)
{
	Fx f;
	/* Five control-signal reads up to the init-state check (lock check 3,
	 * wait_for_synch, init state); the check sees none of the bits 0x0301. */
	f.t.cntrl_sig_script = {0x0301, 0x0301, 0x0301, 0x0301, 0x0000};
	CHECK(f.d.attach(AttachMode::Reset, 250000, f.id) == Status::TargetError);
	CHECK(f.scripts_consumed());
	CHECK(f.t.mem_writes.empty());
	CHECK(!f.d.attached());
	/* The POR left the CPU at SAFE_PC (0x0004, vacant memory), or its state
	 * is unknown: it is not let go there. Its clocks go back to their
	 * sources, then it is reset again and starts at its reset vector. */
	const Log want = Log{"ir:30", "dr16:000e", "ir:28", "dr16:0000"} + restore_clocks() +
			 kResetRelease;
	CHECK_LOG(last(f.t.log, want.size()), want);
	CHECK_EQ(f.t.eem[0x88], 0x000Eu);
}

TEST(debugger_reset_and_halt_refuses_a_watchdog_without_its_password)
{
	Fx f;
	f.t.memory[0x015C] = 0x0000; /* link fault: not a WDTCTL read */
	CHECK(f.d.attach(AttachMode::Reset, 250000, f.id) == Status::TargetError);
	CHECK(f.t.mem_writes.empty());
	CHECK(!f.d.attached());
	CHECK(!contains(f.t.log, read_word(0xFFFE)));
	const Log want = read_word(0x015C) + restore_clocks() + kResetRelease;
	CHECK_LOG(last(f.t.log, want.size()), want);
	CHECK_EQ(f.t.eem[0x88], 0x000Eu);
}

TEST(debugger_reset_and_halt_with_a_late_init_state_failure_releases_at_the_reset_vector)
{
	Fx f;
	/* The sixth control-signal read is attach's check after the device ID
	 * and the trigger clearing (lock check 3, wait_for_synch, init state). */
	f.t.cntrl_sig_script = {0x0301, 0x0301, 0x0301, 0x0301, 0x0301, 0x0000};
	CHECK(f.d.attach(AttachMode::Reset, 250000, f.id) == Status::TargetError);
	CHECK(f.scripts_consumed());
	CHECK(!f.d.attached());
	/* the context is saved: released as detach(Run) does, at the reset
	 * vector's target with SR 0 and the watchdog as the POR left it */
	const Log want = Log{"ir:28", "dr16:0000"} + clear_triggers() +
			 restore_release(0x441A, 0x0000, 0x6904, 0x0006) + Log{"close"};
	CHECK_LOG(last(f.t.log, want.size()), want);
	CHECK_EQ(f.t.eem[0x88], 0x000Eu);
}

TEST(debugger_read_regs_follows_mspds_read_all_cpu_regs)
{
	Fx f;
	f.attached();
	/* SP, R4..R15: low word, high word each */
	f.t.captured = {0x43F0, 0x0000, 0x0004, 0x0000, 0x1234, 0x000F};
	uint32_t regs[16] = {};
	REQUIRE(f.d.read_regs(regs) == Status::Ok);
	Log want = {"ir:28"};
	for (unsigned r : {1u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u, 12u, 13u, 14u, 15u}) {
		want = want + read_reg(0x0060u | (r << 8)) + set_pc(0x0004) +
		       Log{ir(0xC8), d16(0x0501), "tclk:1", ir(0x21)};
	}
	want = want + Log{ir(0x86), d16(0), ir(0x28), d16(0)};
	CHECK_LOG(f.t.log, want);
	CHECK_EQ(regs[0], 0x4520u);
	CHECK_EQ(regs[1], 0x43F0u);
	CHECK_EQ(regs[2], 0x0008u);
	CHECK_EQ(regs[3], 0u);
	CHECK_EQ(regs[4], 0x4u);
	CHECK_EQ(regs[5], 0xF1234u); /* 20 bits */
	CHECK_EQ(regs[6], 0u);
}

TEST(debugger_read_regs_clears_a_full_mailbox)
{
	Fx f;
	f.attached();
	f.t.push_jmb_out(0x12345678);
	uint32_t regs[16] = {};
	REQUIRE(f.d.read_regs(regs) == Status::Ok);
	CHECK(f.t.jmb_out.empty());
	CHECK(contains(f.t.log, Log{ir(0x86), d16(0), d16(0x0014), d16(0), d16(0)}));
}

TEST(debugger_write_reg)
{
	Fx f;
	f.attached();
	CHECK(f.d.write_reg(12, 0xA5432) == Status::Ok);
	/* MOVA #0xA5432, R12 */
	CHECK_LOG(f.t.log, (write_reg(0x0A8C, 0x5432) + Log{"ir:28", "dr16:0000"}));
	f.t.log.clear();
	CHECK(f.d.write_reg(0, 0x4600) == Status::Ok); /* PC: context only */
	CHECK(f.d.write_reg(2, 0x0001) == Status::Ok); /* SR: context only */
	CHECK(f.t.log.empty());
	CHECK_EQ(f.d.pc(), 0x4600u);
	uint32_t regs[16] = {};
	REQUIRE(f.d.read_regs(regs) == Status::Ok);
	CHECK_EQ(regs[0], 0x4600u);
	CHECK_EQ(regs[2], 1u);
}

TEST(debugger_write_reg_accepts_the_limits)
{
	Fx f;
	f.attached();
	CHECK(f.d.write_reg(15, 0xFFFFF) == Status::Ok);
	/* MOVA #0xFFFFF, R15 */
	CHECK_LOG(f.t.log, (write_reg(0x0F8F, 0xFFFF) + Log{"ir:28", "dr16:0000"}));
	f.t.log.clear();
	CHECK(f.d.write_reg(0, 0xFFFFE) == Status::Ok);
	CHECK(f.d.write_reg(2, 0xFFFF) == Status::Ok);
	CHECK(f.t.log.empty());
	uint32_t regs[16] = {};
	REQUIRE(f.d.read_regs(regs) == Status::Ok);
	CHECK_EQ(regs[0], 0xFFFFEu);
	CHECK_EQ(regs[2], 0xFFFFu);
}

TEST(debugger_write_reg_clears_bit_0_of_sp)
{
	/* as the DLL does, CpuMemoryAccess.cpp:106-110 */
	Fx f;
	f.attached();
	CHECK(f.d.write_reg(1, 0x43F1) == Status::Ok);
	/* MOVA #0x043F0, SP */
	CHECK_LOG(f.t.log, (write_reg(0x0081, 0x43F0) + Log{"ir:28", "dr16:0000"}));
}

TEST(debugger_written_pc_and_sr_are_restored_on_detach)
{
	Fx f;
	f.attached();
	REQUIRE(f.d.write_reg(0, 0x4600) == Status::Ok);
	REQUIRE(f.d.write_reg(2, 0x0018) == Status::Ok); /* LPM0: CPUOFF + GIE */
	f.d.detach(DetachAction::Run);
	/* SR goes to the CPU as written; with CPUOFF set the PC is released 2
	 * lower (Note 1411) */
	CHECK_LOG(f.t.log, clear_triggers() + restore_release(0x4600, 0x0018, 0x6904, 0x0006) +
				   Log{"close"});
	CHECK(contains(f.t.log, set_pc(0x45FE)));
}

TEST(debugger_read_regs_keeps_20_bits)
{
	Fx f;
	f.attached();
	/* SP, R4, R5: low word, high word each; the high words carry bits above
	 * bit 19 */
	f.t.captured = {0x43F0, 0xFFFF, 0x0004, 0x0000, 0x1234, 0x00FF};
	uint32_t regs[16] = {};
	REQUIRE(f.d.read_regs(regs) == Status::Ok);
	CHECK(f.scripts_consumed());
	CHECK_EQ(regs[1], 0xF43F0u);
	CHECK_EQ(regs[4], 0x4u);
	CHECK_EQ(regs[5], 0xF1234u);
}

TEST(debugger_register_access_detects_a_dead_link)
{
	{
		Fx f;
		f.attached();
		f.t.cntrl_sig = 0x0000; /* the CPU has left full emulation */
		f.t.captured = {0x43F0, 0x0000};
		uint32_t regs[16] = {};
		CHECK(f.d.read_regs(regs) == Status::TargetError);
		/* filled anyway */
		CHECK_EQ(regs[0], 0x4520u);
		CHECK_EQ(regs[1], 0x43F0u);
		CHECK_EQ(regs[2], 0x0008u);
		CHECK(f.d.write_reg(4, 0x1234) == Status::TargetError);
	}
	{
		Fx f;
		f.attached();
		f.t.jtag_id = 0xFF; /* the final check also tests the JTAG ID */
		uint32_t regs[16] = {};
		CHECK(f.d.read_regs(regs) == Status::TargetError);
		CHECK(f.d.write_reg(4, 0x1234) == Status::TargetError);
	}
}

TEST(debugger_write_reg_rejects_bad_arguments)
{
	Fx f;
	f.attached();
	CHECK(f.d.write_reg(3, 0) == Status::BadArg);
	CHECK(f.d.write_reg(16, 0) == Status::BadArg);
	CHECK(f.d.write_reg(4, 0x100000) == Status::BadArg);
	CHECK(f.d.write_reg(0, 0x4601) == Status::BadArg);
	CHECK(f.d.write_reg(2, 0x10000) == Status::BadArg);
	CHECK(f.t.log.empty());
}

TEST(debugger_read_mem_follows_mspds_read_mem_words)
{
	Fx f;
	f.attached();
	f.t.memory[0x2400] = 0x1234;
	f.t.memory[0x2402] = 0xABCD;
	uint8_t out[4] = {};
	REQUIRE(f.d.read_mem(0x2400, 2, out) == Status::Ok);
	/* read mode first (not in ReadMemWordsXv2), then the macro */
	const Log want = Log{"tclk:0", ir(0xC8), d16(0x0501),
			     ir(0xC1), d20(0x2400), "tclk:1", "tclk:0", ir(0x42), d16(0),
			     ir(0xC1), d20(0x2402), "tclk:1", "tclk:0", ir(0x42), d16(0),
			     "tclk:1", "tclk:0", "tclk:1", "ir:28", "dr16:0000"};
	CHECK_LOG(f.t.log, want);
	const uint8_t want_bytes[] = {0x34, 0x12, 0xCD, 0xAB};
	CHECK_BYTES(out, want_bytes);
	CHECK_EQ(f.d.pc(), 0x4520u); /* no SetPC: the context is untouched */
}

TEST(debugger_write_mem_follows_mspds_write_mem_words)
{
	Fx f;
	f.attached();
	const uint8_t data[] = {0x34, 0x12, 0x78, 0x56};
	REQUIRE(f.d.write_mem(0x2400, 2, data) == Status::Ok);
	CHECK_LOG(f.t.log, (Log{"ir:28", "dr16:0000"} + write_word(0x2400, 0x1234) +
			    write_word(0x2402, 0x5678) + Log{"ir:28", "dr16:0000"}));
	CHECK_EQ(f.t.memory[0x2400], 0x1234);
	CHECK_EQ(f.t.memory[0x2402], 0x5678);
}

TEST(debugger_memory_access_detects_a_dead_link)
{
	{
		Fx f;
		f.attached();
		f.t.cntrl_sig = 0x0000; /* the CPU has left full emulation */
		uint8_t buf[2] = {};
		CHECK(f.d.read_mem(0x2400, 1, buf) == Status::TargetError);
		CHECK(f.d.write_mem(0x2400, 1, buf) == Status::TargetError);
		CHECK(f.t.mem_writes.empty()); /* checked before writing */
	}
	{
		Fx f;
		f.attached();
		f.t.jtag_id = 0xFF; /* the check also tests the JTAG ID */
		uint8_t buf[2] = {};
		CHECK(f.d.read_mem(0x2400, 1, buf) == Status::TargetError);
		CHECK(f.d.write_mem(0x2400, 1, buf) == Status::TargetError);
		CHECK(f.t.mem_writes.empty());
	}
}

TEST(debugger_write_mem_reports_a_link_that_dies_during_the_transfer)
{
	Fx f;
	f.attached();
	/* the check before the writes passes, the one after them does not */
	f.t.cntrl_sig_script = {0x0301, 0x0000};
	const uint8_t data[] = {0x34, 0x12};
	CHECK(f.d.write_mem(0x2400, 1, data) == Status::TargetError);
	CHECK(f.scripts_consumed());
	CHECK_LOG(f.t.log, (Log{"ir:28", "dr16:0000"} + write_word(0x2400, 0x1234) +
			    Log{"ir:28", "dr16:0000"}));
}

TEST(debugger_memory_access_needs_an_attached_cpu)
{
	Fx f;
	uint8_t buf[2] = {0x34, 0x12};
	CHECK(f.d.read_mem(0x2400, 1, buf) == Status::BadState);
	CHECK(f.d.write_mem(0x2400, 1, buf) == Status::BadState);
	CHECK(f.t.log.empty());
	f.attached();
	f.d.detach(DetachAction::Run);
	f.t.log.clear();
	f.t.mem_writes.clear();
	CHECK(f.d.read_mem(0x2400, 1, buf) == Status::BadState);
	CHECK(f.d.write_mem(0x2400, 1, buf) == Status::BadState);
	CHECK(f.t.log.empty());
	CHECK(f.t.mem_writes.empty());
}

TEST(debugger_read_mem_sets_read_mode_first)
{
	Fx f;
	f.attached();
	/* The CPU left in write mode (R/W = 0): a bus cycle on the address
	 * would be a write. The fixture checks mem_protocol_error. */
	f.t.ir_shift(msp430::kIrCntrlSig16Bit);
	f.t.dr_shift16(0x0500);
	f.t.log.clear();
	f.t.memory[0x2400] = 0x1234;
	uint8_t out[2] = {};
	REQUIRE(f.d.read_mem(0x2400, 1, out) == Status::Ok);
	CHECK_LOG(first(f.t.log, 3), (Log{"tclk:0", ir(0xC8), d16(0x0501)}));
	const uint8_t want[] = {0x34, 0x12};
	CHECK_BYTES(out, want);
	CHECK(f.t.mem_writes.empty());
}

TEST(debugger_memory_access_rejects_bad_ranges)
{
	Fx f;
	f.attached();
	uint8_t buf[4] = {};
	CHECK(f.d.read_mem(0x2401, 1, buf) == Status::BadArg); /* odd */
	CHECK(f.d.write_mem(0x2401, 1, buf) == Status::BadArg);
	CHECK(f.d.read_mem(0x100000, 1, buf) == Status::BadArg); /* above 20 bits */
	CHECK(f.d.write_mem(0x100000, 1, buf) == Status::BadArg);
	CHECK(f.d.read_mem(0x200000, 1, buf) == Status::BadArg);
	CHECK(f.d.write_mem(0x200000, 1, buf) == Status::BadArg);
	CHECK(f.d.read_mem(0xFFFFE, 2, buf) == Status::BadArg); /* past 0xFFFFF */
	CHECK(f.d.write_mem(0xFFFFE, 2, buf) == Status::BadArg);
	CHECK(f.d.read_mem(0x2400, SIZE_MAX, buf) == Status::BadArg);
	CHECK(f.d.write_mem(0x2400, SIZE_MAX, buf) == Status::BadArg);
	CHECK(f.t.log.empty());
}

TEST(debugger_memory_access_of_no_words_does_nothing)
{
	Fx f;
	f.attached();
	CHECK(f.d.read_mem(0x2400, 0, nullptr) == Status::Ok);
	CHECK(f.d.write_mem(0x2400, 0, nullptr) == Status::Ok);
	CHECK(f.t.log.empty());
}

TEST(debugger_memory_access_reaches_the_last_word)
{
	Fx f;
	f.attached();
	f.t.memory[0xFFFFE] = 0x441A;
	uint8_t out[2] = {};
	REQUIRE(f.d.read_mem(0xFFFFE, 1, out) == Status::Ok);
	CHECK(contains(f.t.log, Log{ir(0xC1), d20(0xFFFFE)}));
	const uint8_t want[] = {0x1A, 0x44};
	CHECK_BYTES(out, want);
	f.t.log.clear();
	CHECK(!f.t.flash_write_attempt); /* nothing before the intended write */
	const uint8_t data[] = {0x34, 0x12};
	REQUIRE(f.d.write_mem(0xFFFFE, 1, data) == Status::Ok);
	CHECK_LOG(f.t.log, (Log{"ir:28", "dr16:0000"} + write_word(0xFFFFE, 0x1234) +
			    Log{"ir:28", "dr16:0000"}));
	/* a flash address on purpose: write_mem passes on what it is given;
	 * keeping it off flash is Session's job */
	CHECK(f.t.flash_write_attempt);
	f.t.flash_write_attempt = false;
}

TEST(debugger_memory_access_crosses_a_64k_boundary)
{
	Fx f;
	f.attached();
	f.t.memory[0x1FFFE] = 0x1111;
	f.t.memory[0x20000] = 0x2222;
	uint8_t out[4] = {};
	REQUIRE(f.d.read_mem(0x1FFFE, 2, out) == Status::Ok);
	CHECK(contains(f.t.log, Log{ir(0xC1), d20(0x1FFFE)}));
	CHECK(contains(f.t.log, Log{ir(0xC1), d20(0x20000)}));
	const uint8_t want[] = {0x11, 0x11, 0x22, 0x22};
	CHECK_BYTES(out, want);
	f.t.log.clear();
	CHECK(!f.t.flash_write_attempt); /* nothing before the intended write */
	const uint8_t data[] = {0x34, 0x12, 0x78, 0x56};
	REQUIRE(f.d.write_mem(0x1FFFE, 2, data) == Status::Ok);
	CHECK_LOG(f.t.log, (Log{"ir:28", "dr16:0000"} + write_word(0x1FFFE, 0x1234) +
			    write_word(0x20000, 0x5678) + Log{"ir:28", "dr16:0000"}));
	/* flash addresses on purpose, as in the test above */
	CHECK(f.t.flash_write_attempt);
	f.t.flash_write_attempt = false;
}

TEST(debugger_read_mem_returns_the_saved_watchdog)
{
	/* TI's DLL, RegisterAccessBase.cpp:67-79: the value the program gets
	 * back on release, not the held one */
	Fx f;
	f.t.memory[0x015C] = 0x6936;
	f.attached();
	CHECK_EQ(f.t.memory[0x015C], 0x69B6); /* held while halted */
	f.t.memory[0x015A] = 0x1111;
	f.t.memory[0x015E] = 0x2222;
	uint8_t out[6] = {};
	REQUIRE(f.d.read_mem(0x015A, 3, out) == Status::Ok);
	CHECK(contains(f.t.log, Log{ir(0xC1), d20(0x015C)})); /* still read on the bus */
	const uint8_t want[] = {0x11, 0x11, 0x36, 0x69, 0x22, 0x22};
	CHECK_BYTES(out, want);
}

TEST(debugger_write_mem_to_the_watchdog_keeps_it_held)
{
	/* TI's DLL, RegisterAccessBase.cpp:93-146: the low byte is kept for the
	 * release; the live write carries the password and the hold bit */
	const struct {
		uint16_t written, live, released;
	} cases[] = {
		{0x0004, 0x5A84, 0x5A04}, /* no password: no PUC */
		{0x5A80, 0x5A80, 0x5A80}, /* held by the program */
	};
	for (const auto &c : cases) {
		Fx f;
		f.t.memory[0x015C] = 0x6936;
		f.attached();
		const uint8_t data[] = {0x11, 0x11, static_cast<uint8_t>(c.written),
					static_cast<uint8_t>(c.written >> 8), 0x22, 0x22};
		REQUIRE(f.d.write_mem(0x015A, 3, data) == Status::Ok);
		REQUIRE(f.t.mem_writes.size() == 3u);
		CHECK(f.t.mem_writes[0] == (std::pair<uint32_t, uint16_t>{0x015A, 0x1111}));
		CHECK(f.t.mem_writes[1] == (std::pair<uint32_t, uint16_t>{0x015C, c.live}));
		CHECK(f.t.mem_writes[2] == (std::pair<uint32_t, uint16_t>{0x015E, 0x2222}));
		uint8_t out[2] = {};
		REQUIRE(f.d.read_mem(0x015C, 1, out) == Status::Ok);
		CHECK_EQ(out[0] | (out[1] << 8), 0x6900 | (c.written & 0xFF));
		f.t.mem_writes.clear();
		f.d.detach(DetachAction::Run);
		REQUIRE(f.t.mem_writes.size() == 1u);
		CHECK(f.t.mem_writes[0] == (std::pair<uint32_t, uint16_t>{0x015C, c.released}));
	}
}

TEST(debugger_set_breakpoint_programs_one_trigger_block)
{
	Fx f;
	f.attached();
	REQUIRE(f.d.set_breakpoint(2, 0x14A2C) == Status::Ok);
	/* value = address, control = CMP_EQUAL + TRIG_0 + MAB, mask none,
	 * combination 2 = trigger 2 alone; the value read back; BREAKREACT */
	const Log want = Log{ir(0xB0)} + eem_write(0x10, 0x14A2C) + eem_write(0x12, 0) +
			 eem_write(0x14, 0) + eem_write(0x16, 0x0004) + eem_read(0x10) +
			 eem_write(0x80, 0x0004);
	CHECK_LOG(f.t.log, want);
	REQUIRE(f.d.set_breakpoint(0, 0x4400) == Status::Ok);
	CHECK_EQ(f.t.eem[0x80], 0x0005u);
	f.t.log.clear();
	REQUIRE(f.d.clear_breakpoint(2) == Status::Ok);
	/* only the combination register and BREAKREACT */
	CHECK_LOG(f.t.log, Log{ir(0xB0)} + eem_write(0x16, 0) + eem_write(0x80, 0x0001));
	CHECK_EQ(f.t.eem[0x16], 0u);
	CHECK_EQ(f.t.eem[0x80], 0x0001u);
	f.t.log.clear();
	REQUIRE(f.d.clear_breakpoint(msp430::kAllBreakpoints) == Status::Ok);
	CHECK_LOG(f.t.log, clear_triggers());
	CHECK_EQ(f.t.eem[0x06], 0u);
	CHECK_EQ(f.t.eem[0x80], 0u);
}

TEST(debugger_set_breakpoint_checks_its_arguments)
{
	Fx f;
	f.attached();
	CHECK(f.d.set_breakpoint(8, 0x4400) == Status::BadArg);
	CHECK(f.d.set_breakpoint(msp430::kAllBreakpoints, 0x4400) == Status::BadArg);
	CHECK(f.d.set_breakpoint(0, 0x4401) == Status::BadArg);
	CHECK(f.d.set_breakpoint(0, 0xFFFFF) == Status::BadArg);
	CHECK(f.d.set_breakpoint(0, 0x100000) == Status::BadArg);
	CHECK(f.d.set_breakpoint(0, 0xFFFFFFFE) == Status::BadArg);
	CHECK(f.d.clear_breakpoint(8) == Status::BadArg);
	CHECK(f.d.clear_breakpoint(0xFE) == Status::BadArg);
	CHECK(f.d.clear_breakpoint(0x100u | msp430::kAllBreakpoints) == Status::BadArg);
	CHECK(f.t.log.empty());
}

TEST(debugger_set_breakpoint_detects_a_missing_trigger_block)
{
	Fx f;
	f.attached();
	f.t.ones_after_shifts = f.t.shifts() + 9; /* the read-back sees a floating line */
	CHECK(f.d.set_breakpoint(7, 0x4400) == Status::TargetError);
	/* the trigger block is left disarmed: combination 7 empty, BREAKREACT
	 * without bit 7 */
	const Log want = Log{ir(0xB0)} + eem_write(0x38, 0x4400) + eem_write(0x3A, 0) +
			 eem_write(0x3C, 0) + eem_write(0x3E, 0x0080) + eem_read(0x38) +
			 eem_write(0x3E, 0) + eem_write(0x80, 0);
	CHECK_LOG(f.t.log, want);
	CHECK_EQ(f.t.eem[0x3E], 0u);
	CHECK_EQ(f.t.eem[0x80], 0u);
}

TEST(debugger_set_breakpoint_failure_disarms_the_breakpoint_it_replaces)
{
	Fx f;
	f.attached();
	REQUIRE(f.d.set_breakpoint(7, 0x4400) == Status::Ok);
	REQUIRE(f.d.set_breakpoint(1, 0x4500) == Status::Ok);
	CHECK_EQ(f.t.eem[0x80], 0x0082u);
	f.t.log.clear();
	/* the new value of trigger block 7 does not read back: its old
	 * breakpoint is gone with it */
	f.t.ones_after_shifts = f.t.shifts() + 9;
	CHECK(f.d.set_breakpoint(7, 0x4600) == Status::TargetError);
	const Log want = Log{ir(0xB0)} + eem_write(0x38, 0x4600) + eem_write(0x3A, 0) +
			 eem_write(0x3C, 0) + eem_write(0x3E, 0x0080) + eem_read(0x38) +
			 eem_write(0x3E, 0) + eem_write(0x80, 0x0002);
	CHECK_LOG(f.t.log, want);
	CHECK_EQ(f.t.eem[0x3E], 0u);
	CHECK_EQ(f.t.eem[0x80], 0x0002u);
	/* link back: the debugger no longer counts breakpoint 7 as set */
	f.t.ones_after_shifts = -1;
	REQUIRE(f.d.set_breakpoint(0, 0x4700) == Status::Ok);
	CHECK_EQ(f.t.eem[0x80], 0x0003u);
}

TEST(debugger_set_breakpoint_uses_all_eight_trigger_blocks)
{
	Fx f;
	f.attached();
	/* the lowest and the highest even address included */
	const uint32_t addr[8] = {0x00000, 0x04400, 0x0FFFE, 0x10000,
				  0x14A2C, 0x02400, 0x45678, 0xFFFFE};
	for (unsigned i = 0; i < 8; ++i) {
		REQUIRE(f.d.set_breakpoint(i, addr[i]) == Status::Ok);
		CHECK_EQ(f.t.eem[0x80], (2u << i) - 1u);
	}
	for (unsigned i = 0; i < 8; ++i) {
		const uint8_t tb = static_cast<uint8_t>(8 * i);
		CHECK_EQ(f.t.eem[tb], addr[i]);
		CHECK_EQ(f.t.eem[static_cast<uint8_t>(tb + 2)], 0u);
		CHECK_EQ(f.t.eem[static_cast<uint8_t>(tb + 4)], 0u);
		CHECK_EQ(f.t.eem[static_cast<uint8_t>(tb + 6)], 1u << i);
	}
	/* nothing beyond trigger block 7 (the register triggers) but BREAKREACT */
	for (const auto &w : f.t.eem_writes) {
		CHECK(w.first < 0x40u || w.first == 0x80u);
	}
	/* the first and the last */
	f.t.log.clear();
	REQUIRE(f.d.clear_breakpoint(0) == Status::Ok);
	CHECK_LOG(f.t.log, Log{ir(0xB0)} + eem_write(0x06, 0) + eem_write(0x80, 0x00FE));
	REQUIRE(f.d.clear_breakpoint(7) == Status::Ok);
	CHECK_EQ(f.t.eem[0x3E], 0u);
	CHECK_EQ(f.t.eem[0x80], 0x007Eu);
	REQUIRE(f.d.clear_breakpoint(msp430::kAllBreakpoints) == Status::Ok);
	for (unsigned i = 0; i < 8; ++i) {
		CHECK_EQ(f.t.eem[static_cast<uint8_t>(8 * i + 6)], 0u);
	}
	CHECK_EQ(f.t.eem[0x80], 0u);
}

TEST(debugger_set_breakpoint_again_moves_it)
{
	Fx f;
	f.attached();
	REQUIRE(f.d.set_breakpoint(3, 0x4400) == Status::Ok);
	REQUIRE(f.d.set_breakpoint(3, 0x4600) == Status::Ok);
	CHECK_EQ(f.t.eem[0x18], 0x4600u);
	CHECK_EQ(f.t.eem[0x1E], 0x0008u);
	CHECK_EQ(f.t.eem[0x80], 0x0008u);
}

TEST(debugger_clear_breakpoint_of_an_unset_index)
{
	Fx f;
	f.attached();
	REQUIRE(f.d.set_breakpoint(4, 0x4400) == Status::Ok);
	f.t.log.clear();
	REQUIRE(f.d.clear_breakpoint(6) == Status::Ok);
	CHECK_LOG(f.t.log, Log{ir(0xB0)} + eem_write(0x36, 0) + eem_write(0x80, 0x0010));
}

TEST(debugger_breakpoints_are_cleared_on_detach_and_attach)
{
	Fx f;
	f.attached();
	REQUIRE(f.d.set_breakpoint(5, 0x4400) == Status::Ok);
	f.t.log.clear();
	f.d.detach(DetachAction::Run);
	CHECK_LOG(first(f.t.log, clear_triggers().size()), clear_triggers());
	CHECK_EQ(f.t.eem[0x2E], 0u);
	CHECK_EQ(f.t.eem[0x80], 0u);
	f.attached();
	REQUIRE(f.d.set_breakpoint(1, 0x4600) == Status::Ok);
	CHECK_EQ(f.t.eem[0x80], 0x0002u); /* breakpoint 5 is gone */
}

TEST(debugger_breakpoints_need_an_attached_cpu)
{
	Fx f;
	CHECK(f.d.set_breakpoint(0, 0x4400) == Status::BadState);
	CHECK(f.d.clear_breakpoint(0) == Status::BadState);
	CHECK(f.d.clear_breakpoint(msp430::kAllBreakpoints) == Status::BadState);
	CHECK(f.t.log.empty());
	f.attached();
	f.d.detach(DetachAction::Run);
	f.t.log.clear();
	CHECK(f.d.set_breakpoint(0, 0x4400) == Status::BadState);
	CHECK(f.d.clear_breakpoint(0) == Status::BadState);
	CHECK(f.d.clear_breakpoint(msp430::kAllBreakpoints) == Status::BadState);
	CHECK(f.t.log.empty());
}
