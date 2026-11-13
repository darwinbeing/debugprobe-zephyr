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
	 * memory-access rules must hold, nothing is written to flash or the
	 * flash controller, and the CPU is fed only the instructions FakeTap
	 * allows. A test that passes write_mem a flash address on purpose
	 * checks the flag false right before that write, so that no earlier
	 * flash write goes unnoticed, then checks it set and clears it. */
	~Fx() { CHECK_LOG(faults(), Log{}); }

	/* The names of the fault flags that are set. Checked after each
	 * operation of a sequence, a failure names the operation that set one. */
	Log faults() const
	{
		const std::pair<bool, const char *> flags[] = {
			{t.jmb_protocol_error, "jmb_protocol_error"},
			{t.eem_protocol_error, "eem_protocol_error"},
			{t.password_violation, "password_violation"},
			{t.mem_protocol_error, "mem_protocol_error"},
			{t.flash_write_attempt, "flash_write_attempt"},
			{t.unexpected_instruction, "unexpected_instruction"},
		};
		Log set;
		for (const auto &flag : flags) {
			if (flag.first) {
				set.push_back(flag.second);
			}
		}
		return set;
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
	CHECK(f.scripts_consumed());
	/* The watchdog held first, so it cannot reset the device during the
	 * wait; then FCTL3 read until the 100 ms limit: busy at 0, 30, 60 and
	 * 90 ms, given up at 120 ms. Never written. */
	CHECK_LOG(f.t.log, identify_log() + eem_stop(1, kTb0Zero) + sync_tail(0x6904, 0x0008, 4) +
				   read_word(0x1A04) + clear_triggers() + Log{"ir:28", "dr16:0000"});
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
	/* SyncJtag_AssertPor_SaveContextXv2.c:203-218: the PC to a safe
	 * address, then the flag cleared, the other one kept */
	CHECK_LOG(f.t.log, identify_log() + assert_por_log(0x6904) + set_pc(0x0004) +
				   Log{ir(0xC8), d16(0x0501), "tclk:1", ir(0x21)} +
				   write_word(0x0102, 0x0004) + read_word(0x1A04) + clear_triggers() +
				   Log{"ir:28", "dr16:0000"});
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

namespace {

/* ReadAllCpuRegsXv2.c:58-91, JTAG ID 0x91, then i_ReadJmbOut (edt.c:44-57),
 * which takes a 32-bit message off the mailbox if one is waiting, and the
 * init-state check */
Log read_regs_log(bool mailbox_full)
{
	Log l = {"ir:28"};
	for (unsigned r : {1u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u, 12u, 13u, 14u, 15u}) {
		l = l + read_reg(0x0060u | (r << 8)) + set_pc(0x0004) +
		    Log{ir(0xC8), d16(0x0501), "tclk:1", ir(0x21)};
	}
	l = l + Log{ir(0x86), d16(0)};
	if (mailbox_full) {
		l = l + Log{d16(0x0014), d16(0), d16(0)};
	}
	return l + Log{ir(0x28), d16(0)};
}

} // namespace

TEST(debugger_read_regs_follows_mspds_read_all_cpu_regs)
{
	Fx f;
	f.attached();
	/* SP, R4..R15: low word, high word each */
	f.t.captured = {0x43F0, 0x0000, 0x0004, 0x0000, 0x1234, 0x000F};
	uint32_t regs[16] = {};
	REQUIRE(f.d.read_regs(regs) == Status::Ok);
	CHECK_LOG(f.t.log, read_regs_log(false));
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
	CHECK_LOG(f.t.log, read_regs_log(true));
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

/* Registers, as debugger_memory_access_needs_an_attached_cpu,
 * debugger_breakpoints_need_an_attached_cpu and
 * debugger_run_control_needs_an_attached_cpu check the other operations. PC
 * and SR live in the saved context (no TAP traffic even when attached): only
 * their status shows the check. */
TEST(debugger_register_access_needs_an_attached_cpu)
{
	Fx f;
	uint32_t regs[16] = {};
	CHECK(f.d.read_regs(regs) == Status::BadState);
	CHECK(f.d.write_reg(4, 0) == Status::BadState);
	CHECK(f.d.write_reg(0, 0x4600) == Status::BadState);
	CHECK(f.d.write_reg(2, 0x0008) == Status::BadState);
	CHECK(f.t.log.empty());
	f.attached();
	f.d.detach(DetachAction::Run);
	f.t.log.clear();
	f.t.mem_writes.clear();
	CHECK(f.d.read_regs(regs) == Status::BadState);
	CHECK(f.d.write_reg(4, 0) == Status::BadState);
	CHECK(f.d.write_reg(0, 0x4600) == Status::BadState);
	CHECK(f.d.write_reg(2, 0x0008) == Status::BadState);
	CHECK(f.t.log.empty());
	CHECK(f.t.mem_writes.empty());
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
	 * combination 2 = trigger 2 alone; the value read back; BREAKREACT;
	 * the init-state check */
	const Log want = Log{ir(0xB0)} + eem_write(0x10, 0x14A2C) + eem_write(0x12, 0) +
			 eem_write(0x14, 0) + eem_write(0x16, 0x0004) + eem_read(0x10) +
			 eem_write(0x80, 0x0004) + Log{"ir:28", "dr16:0000"};
	CHECK_LOG(f.t.log, want);
	REQUIRE(f.d.set_breakpoint(0, 0x4400) == Status::Ok);
	CHECK_EQ(f.t.eem[0x80], 0x0005u);
	f.t.log.clear();
	REQUIRE(f.d.clear_breakpoint(2) == Status::Ok);
	/* only the combination register and BREAKREACT, then the check */
	CHECK_LOG(f.t.log, Log{ir(0xB0)} + eem_write(0x16, 0) + eem_write(0x80, 0x0001) +
				   Log{"ir:28", "dr16:0000"});
	CHECK_EQ(f.t.eem[0x16], 0u);
	CHECK_EQ(f.t.eem[0x80], 0x0001u);
	f.t.log.clear();
	REQUIRE(f.d.clear_breakpoint(msp430::kAllBreakpoints) == Status::Ok);
	CHECK_LOG(f.t.log, clear_triggers() + Log{"ir:28", "dr16:0000"});
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

TEST(debugger_set_breakpoint_detects_a_floating_line)
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

TEST(debugger_set_breakpoint_detects_a_missing_trigger_block)
{
	Fx f;
	f.attached();
	f.t.eem_read_as[0x38] = 0; /* no trigger block 7: its value reads 0 */
	CHECK(f.d.set_breakpoint(7, 0x4400) == Status::TargetError);
	const Log want = Log{ir(0xB0)} + eem_write(0x38, 0x4400) + eem_write(0x3A, 0) +
			 eem_write(0x3C, 0) + eem_write(0x3E, 0x0080) + eem_read(0x38) +
			 eem_write(0x3E, 0) + eem_write(0x80, 0);
	CHECK_LOG(f.t.log, want);
	CHECK_EQ(f.t.eem[0x3E], 0u);
	CHECK_EQ(f.t.eem[0x80], 0u);
}

TEST(debugger_set_breakpoint_reads_back_20_bits)
{
	Fx f;
	f.attached();
	f.t.eem_read_as[0x10] = 0xFFF14A2C; /* bits above the 20-bit address */
	REQUIRE(f.d.set_breakpoint(2, 0x14A2C) == Status::Ok);
	CHECK_EQ(f.t.eem[0x80], 0x0004u);
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
	CHECK_LOG(f.t.log, Log{ir(0xB0)} + eem_write(0x06, 0) + eem_write(0x80, 0x00FE) +
				   Log{"ir:28", "dr16:0000"});
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
	CHECK_LOG(f.t.log, Log{ir(0xB0)} + eem_write(0x36, 0) + eem_write(0x80, 0x0010) +
				   Log{"ir:28", "dr16:0000"});
}

TEST(debugger_breakpoints_detect_a_dead_link)
{
	for (const bool bad_id : {true, false}) {
		Fx f;
		f.attached();
		if (bad_id) {
			f.t.jtag_id = 0xFF; /* the final check also tests the JTAG ID */
		} else {
			f.t.cntrl_sig = 0x0000; /* the CPU has left full emulation */
		}
		CHECK(f.d.set_breakpoint(1, 0x4400) == Status::TargetError);
		CHECK_EQ(f.t.eem[0x80], 0x0002u); /* armed before the check */
		/* the breakpoint still counts as set: the next one keeps it in
		 * BREAKREACT */
		CHECK(f.d.set_breakpoint(0, 0x4600) == Status::TargetError);
		CHECK_EQ(f.t.eem[0x80], 0x0003u);
		CHECK(f.d.clear_breakpoint(1) == Status::TargetError);
		CHECK_EQ(f.t.eem[0x0E], 0u);
		CHECK_EQ(f.t.eem[0x80], 0x0001u);
		CHECK(f.d.clear_breakpoint(msp430::kAllBreakpoints) == Status::TargetError);
		CHECK_EQ(f.t.eem[0x06], 0u);
		CHECK_EQ(f.t.eem[0x80], 0u);
	}
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

TEST(debugger_resume_restores_the_context_with_the_eem_armed)
{
	Fx f;
	f.attached();
	REQUIRE(f.d.resume() == Status::Ok);
	/* DebugManagerMSP430::run with breakpoints polled: EEM control 0x0007 */
	CHECK_LOG(f.t.log, restore_release(0x4520, 0x0008, 0x6904, 0x0007));
	CHECK(f.d.attached());
	CHECK(f.d.running());
	CHECK(f.d.stop_reason() == StopReason::None);
	CHECK_EQ(f.d.breakpoint_hit(), msp430::kNoBreakpoint);
	f.t.log.clear();
	CHECK(f.d.resume() == Status::Running);
	CHECK(f.t.log.empty());
}

TEST(debugger_resume_into_a_low_power_mode_backs_up_the_pc)
{
	Fx f;
	f.t.cntrl_sig = 0x0321;
	f.t.eem_control_script = {0x0000};
	f.t.captured = {0x00D8, 0x0000}; /* LPM3 + GIE */
	f.t.mab = 0x0453E;
	REQUIRE(f.d.attach(AttachMode::Running, 250000, f.id) == Status::Ok);
	CHECK(f.scripts_consumed());
	f.t.log.clear();
	f.t.cntrl_sig = 0x0301;
	REQUIRE(f.d.resume() == Status::Ok);
	/* Note 1411: PC - 2 again, SR with CPUOFF and GIE back */
	CHECK_LOG(f.t.log, restore_release(0x453C, 0x00D8, 0x6904, 0x0007));
	CHECK(contains(f.t.log, set_pc(0x453A)));
}

TEST(debugger_release_answers_a_halt_signal)
{
	Fx f;
	f.attached();
	/* the control-signal reads of SetPcXv2 and of the release clock; the
	 * second shows HALT (0x0002) */
	f.t.cntrl_sig_script = {0x0301, 0x0303};
	REQUIRE(f.d.resume() == Status::Ok);
	CHECK(f.scripts_consumed());
	CHECK(contains(f.t.log, Log{ir(0x28), "tclk:0", d16(0), ir(0xC8), d16(0x0403), "tclk:1"}));
	CHECK_LOG(f.t.log, restore_release(0x4520, 0x0008, 0x6904, 0x0007, true));
}

TEST(debugger_poll_while_running)
{
	Fx f;
	f.attached();
	REQUIRE(f.d.resume() == Status::Ok);
	f.t.log.clear();
	bool halted = true;
	REQUIRE(f.d.poll(halted) == Status::Ok);
	CHECK(!halted);
	CHECK_LOG(f.t.log, (Log{"ir:28", "ir:d0", "dr16:0000"})); /* WaitForEem */
	CHECK(f.d.running());
	CHECK(f.d.stop_reason() == StopReason::None);
	/* the DLL's mask is EEM_STOPPED (0x80) alone */
	f.t.log.clear();
	f.t.eem_control_script = {0xFF7F};
	halted = true;
	REQUIRE(f.d.poll(halted) == Status::Ok);
	CHECK(f.scripts_consumed());
	CHECK(!halted);
	CHECK_LOG(f.t.log, (Log{"ir:28", "ir:d0", "dr16:0000"}));
	CHECK(f.d.running());
}

TEST(debugger_poll_sees_a_breakpoint_stop)
{
	Fx f;
	f.attached();
	REQUIRE(f.d.set_breakpoint(3, 0x4600) == Status::Ok);
	REQUIRE(f.d.resume() == Status::Ok);
	f.t.log.clear();
	f.t.eem_control_script = {0x0080, 0x0080}; /* WaitForEem, then the sync's look */
	f.t.captured = {0x0001, 0x0000};
	f.t.mab = 0x4604;
	bool halted = false;
	REQUIRE(f.d.poll(halted) == Status::Ok);
	CHECK(f.scripts_consumed());
	CHECK(halted);
	CHECK(f.d.attached());
	CHECK(!f.d.running());
	CHECK(f.d.stop_reason() == StopReason::Breakpoint);
	CHECK_EQ(f.d.breakpoint_hit(), 3);
	CHECK_EQ(f.d.pc(), 0x4600u);
	/* already stopped by the EEM: no stop request, straight to the sync
	 * (DebugManagerMSP430::runEvent, saveContext on BP_HIT_FLAG) */
	CHECK_LOG(f.t.log, Log{"ir:28", "ir:d0", "dr16:0000", "ir:d0", "dr16:0000"} +
				   sync_tail(0x6904, 0x0001));
	/* halted now: nothing to poll */
	f.t.log.clear();
	halted = false;
	CHECK(f.d.poll(halted) == Status::Ok);
	CHECK(halted);
	CHECK(f.t.log.empty());
}

TEST(debugger_poll_reports_a_lost_target)
{
	/* the target reset or lost power: JTAG is gone. WaitForEem would keep
	 * polling; the debugger lets go. 0x98 and 0x99 are valid MSPDS IDs,
	 * but not the F5529's. */
	for (const uint8_t id : {0xFF, 0x00, 0x98, 0x99}) {
		Fx f;
		f.attached();
		REQUIRE(f.d.resume() == Status::Ok);
		f.t.log.clear();
		f.t.mem_writes.clear();
		f.t.jtag_id = id;
		bool halted = true;
		CHECK(f.d.poll(halted) == Status::TargetError);
		CHECK(!halted);
		CHECK(!f.d.attached());
		CHECK(!f.d.running());
		/* an attempt to turn the EEM off, then released, pins let go */
		CHECK_LOG(f.t.log, Log{"ir:28"} + kAbandon);
		CHECK(f.t.log.back() == "close");
		CHECK(f.t.mem_writes.empty());
	}
}

TEST(debugger_poll_with_a_failed_sync_after_a_stop)
{
	Fx f;
	f.attached();
	REQUIRE(f.d.resume() == Status::Ok);
	f.t.log.clear();
	f.t.mem_writes.clear();
	f.t.eem_control_script = {0x0080, 0x0080};
	f.t.cntrl_sig = 0x0101; /* the sync bit never comes */
	bool halted = true;
	CHECK(f.d.poll(halted) == Status::TargetError);
	CHECK(f.scripts_consumed());
	CHECK(!halted);
	CHECK(!f.d.attached());
	CHECK(!f.d.running());
	CHECK(f.d.stop_reason() == StopReason::None);
	/* the CPU, not in full emulation, is let go with nothing written */
	CHECK(f.t.mem_writes.empty());
	CHECK_LOG(last(f.t.log, kAbandon.size()), kAbandon);
}

TEST(debugger_poll_names_the_breakpoint_at_the_stop_address)
{
	const struct {
		uint32_t stop_pc;
		uint8_t want;
	} cases[] = {
		{0x4800, 7},                     /* the last trigger block */
		{0x4600, 2},                     /* two at one address: the lower index */
		{0x4400, msp430::kNoBreakpoint}, /* cleared: no longer counts */
		{0x4A00, msp430::kNoBreakpoint}, /* none there */
	};
	for (const auto &c : cases) {
		Fx f;
		f.attached();
		REQUIRE(f.d.set_breakpoint(0, 0x4400) == Status::Ok);
		REQUIRE(f.d.set_breakpoint(2, 0x4600) == Status::Ok);
		REQUIRE(f.d.set_breakpoint(5, 0x4600) == Status::Ok);
		REQUIRE(f.d.set_breakpoint(7, 0x4800) == Status::Ok);
		REQUIRE(f.d.clear_breakpoint(0) == Status::Ok);
		REQUIRE(f.d.resume() == Status::Ok);
		f.t.eem_control_script = {0x0080, 0x0080};
		f.t.captured = {0x0008, 0x0000};
		f.t.mab = c.stop_pc + 4;
		bool halted = false;
		REQUIRE(f.d.poll(halted) == Status::Ok);
		CHECK(f.scripts_consumed());
		CHECK(halted);
		CHECK(f.d.stop_reason() == StopReason::Breakpoint);
		CHECK_EQ(f.d.pc(), c.stop_pc);
		CHECK_EQ(f.d.breakpoint_hit(), c.want);
	}
}

TEST(debugger_halt_stops_a_running_cpu)
{
	Fx f;
	f.attached();
	REQUIRE(f.d.resume() == Status::Ok);
	f.t.log.clear();
	/* WaitForEem's look, the sync's look, the stop poll */
	f.t.eem_control_script = {0x0000, 0x0000, 0x0080};
	f.t.captured = {0x0008, 0x0000};
	f.t.mab = 0x4ABC;
	REQUIRE(f.d.halt() == Status::Ok);
	CHECK(f.scripts_consumed());
	CHECK(f.d.attached());
	CHECK(!f.d.running());
	CHECK(f.d.stop_reason() == StopReason::Request);
	CHECK_EQ(f.d.breakpoint_hit(), msp430::kNoBreakpoint);
	CHECK_EQ(f.d.pc(), 0x4AB8u);
	CHECK_LOG(f.t.log, Log{"ir:28", "ir:d0", "dr16:0000"} + eem_stop(1, kTb0Zero) +
				   sync_tail(0x6904, 0x0008));
	f.t.log.clear();
	CHECK(f.d.halt() == Status::Ok); /* already halted: nothing to do */
	CHECK(f.t.log.empty());
	CHECK(f.d.stop_reason() == StopReason::Request);
}

TEST(debugger_halt_in_a_low_power_mode)
{
	Fx f;
	f.attached();
	REQUIRE(f.d.resume() == Status::Ok);
	f.t.log.clear();
	f.t.cntrl_sig = 0x0321; /* CPUOFF: the firmware sleeps */
	f.t.eem_control_script = {0x0000, 0x0000};
	f.t.captured = {0x00D8, 0x0000}; /* LPM3 + GIE */
	f.t.mab = 0x0453E;
	REQUIRE(f.d.halt() == Status::Ok);
	CHECK(f.scripts_consumed());
	CHECK(f.d.stop_reason() == StopReason::Request);
	CHECK_EQ(f.d.pc(), 0x0453Cu); /* MAB - 2 in a low-power mode */
	/* no EEM stop request in a low-power mode: straight to the sync */
	CHECK_LOG(f.t.log, Log{"ir:28", "ir:d0", "dr16:0000", "ir:d0", "dr16:0000", "ir:28",
			       "dr16:0000"} +
				   sync_tail(0x6904, 0x00D8));
	f.t.log.clear();
	REQUIRE(f.d.resume() == Status::Ok);
	/* back to sleep where it was: SR with CPUOFF and GIE, the PC 2 lower
	 * (Note 1411) */
	CHECK_LOG(f.t.log, restore_release(0x453C, 0x00D8, 0x6904, 0x0007));
	CHECK(contains(f.t.log, set_pc(0x453A)));
}

TEST(debugger_halt_finds_a_breakpoint_stop)
{
	/* the EEM stopped the CPU at a breakpoint before the halt request:
	 * WaitForEem's check reports it as such */
	Fx f;
	f.attached();
	REQUIRE(f.d.set_breakpoint(2, 0x4600) == Status::Ok);
	REQUIRE(f.d.resume() == Status::Ok);
	f.t.log.clear();
	f.t.eem_control_script = {0x0080, 0x0080};
	f.t.captured = {0x0008, 0x0000};
	f.t.mab = 0x4604;
	REQUIRE(f.d.halt() == Status::Ok);
	CHECK(f.scripts_consumed());
	CHECK(!f.d.running());
	CHECK(f.d.stop_reason() == StopReason::Breakpoint);
	CHECK_EQ(f.d.breakpoint_hit(), 2);
	CHECK_EQ(f.d.pc(), 0x4600u);
	CHECK_LOG(f.t.log, Log{"ir:28", "ir:d0", "dr16:0000", "ir:d0", "dr16:0000"} +
				   sync_tail(0x6904, 0x0008));
}

TEST(debugger_halt_reports_a_lost_target)
{
	Fx f;
	f.attached();
	REQUIRE(f.d.resume() == Status::Ok);
	f.t.log.clear();
	f.t.mem_writes.clear();
	f.t.jtag_id = 0xFF;
	CHECK(f.d.halt() == Status::TargetError);
	CHECK(!f.d.attached());
	CHECK(!f.d.running());
	CHECK_LOG(f.t.log, Log{"ir:28"} + kAbandon);
	CHECK(f.t.mem_writes.empty());
	f.t.log.clear();
	CHECK(f.d.halt() == Status::BadState);
	CHECK(f.t.log.empty());
}

TEST(debugger_halt_with_a_failed_sync_releases)
{
	Fx f;
	f.attached();
	REQUIRE(f.d.resume() == Status::Ok);
	f.t.log.clear();
	f.t.mem_writes.clear();
	f.t.eem_control_script = {0x0000, 0x0000, 0x0080};
	f.t.cntrl_sig = 0x0101; /* not in a low-power mode; the sync bit never comes */
	CHECK(f.d.halt() == Status::TargetError);
	CHECK(f.scripts_consumed());
	CHECK(!f.d.attached());
	CHECK(!f.d.running());
	CHECK(f.d.stop_reason() == StopReason::None);
	CHECK(f.t.mem_writes.empty());
	/* the EEM stop and the sync attempt, then let go with nothing written */
	const Log start = Log{"ir:28", "ir:d0", "dr16:0000"} + eem_stop(1, kTb0Zero);
	CHECK_LOG(first(f.t.log, start.size()), start);
	CHECK_LOG(last(f.t.log, kAbandon.size()), kAbandon);
}

TEST(debugger_breakpoint_survives_a_halt_that_borrows_trigger_block_0)
{
	Fx f;
	f.attached();
	REQUIRE(f.d.set_breakpoint(0, 0x4600) == Status::Ok);
	REQUIRE(f.d.set_breakpoint(5, 0x4A00) == Status::Ok);
	REQUIRE(f.d.resume() == Status::Ok);
	f.t.log.clear();
	f.t.eem_control_script = {0x0000, 0x0000, 0x0080};
	f.t.captured = {0x0008, 0x0000};
	REQUIRE(f.d.halt() == Status::Ok);
	CHECK(f.scripts_consumed());
	/* read before the borrow, written back after the stop: value, control,
	 * mask, combination 0 and BREAKREACT (breakpoints 0 and 5) */
	const unsigned tb0[5] = {0x4600, 0, 0, 1, 0x21};
	CHECK(contains(f.t.log, eem_stop(1, tb0)));
	CHECK_EQ(f.t.eem[0x00], 0x4600u);
	CHECK_EQ(f.t.eem[0x02], 0u);
	CHECK_EQ(f.t.eem[0x04], 0u);
	CHECK_EQ(f.t.eem[0x06], 1u);
	CHECK_EQ(f.t.eem[0x80], 0x21u);
	/* and the debugger still counts it */
	REQUIRE(f.d.resume() == Status::Ok);
	f.t.eem_control_script = {0x0080, 0x0080};
	f.t.captured = {0x0008, 0x0000};
	f.t.mab = 0x4604;
	bool halted = false;
	REQUIRE(f.d.poll(halted) == Status::Ok);
	CHECK(f.scripts_consumed());
	CHECK(halted);
	CHECK_EQ(f.d.breakpoint_hit(), 0);
}

TEST(debugger_resume_after_a_halt_restores_the_halted_context)
{
	Fx f;
	f.attached();
	REQUIRE(f.d.resume() == Status::Ok);
	f.t.memory[0x015C] = 0x6936; /* the program changed its watchdog meanwhile */
	f.t.eem_control_script = {0x0000, 0x0000, 0x0080};
	f.t.captured = {0x0009, 0x0000}; /* GIE + C */
	f.t.mab = 0x14A2C;
	REQUIRE(f.d.halt() == Status::Ok);
	CHECK(f.scripts_consumed());
	CHECK_EQ(f.d.pc(), 0x14A28u);
	f.t.log.clear();
	f.t.mem_writes.clear();
	REQUIRE(f.d.resume() == Status::Ok);
	CHECK_LOG(f.t.log, restore_release(0x14A28, 0x0009, 0x6936, 0x0007));
	REQUIRE(f.t.mem_writes.size() == 1u);
	CHECK(f.t.mem_writes[0] == (std::pair<uint32_t, uint16_t>{0x015C, 0x5A36}));
}

TEST(debugger_detach_halts_a_running_cpu_first)
{
	Fx f;
	f.attached();
	REQUIRE(f.d.resume() == Status::Ok);
	f.t.log.clear();
	f.t.eem_control_script = {0x0000, 0x0000, 0x0080};
	f.t.captured = {0x0008, 0x0000};
	f.d.detach(DetachAction::Run);
	CHECK(f.scripts_consumed());
	CHECK(!f.d.attached());
	CHECK(!f.d.running());
	CHECK(f.d.stop_reason() == StopReason::None);
	/* as the DLL's Close: stopped (SyncJtag_Conditional_SaveContextXv2), then
	 * run with its context restored and the EEM off */
	CHECK_LOG(f.t.log, Log{"ir:28", "ir:d0", "dr16:0000"} + eem_stop(1, kTb0Zero) +
				   sync_tail(0x6904, 0x0008) + clear_triggers() +
				   restore_release(0x4520, 0x0008, 0x6904, 0x0006) + Log{"close"});
}

TEST(debugger_detach_with_reset_halts_a_running_cpu_first)
{
	Fx f;
	f.attached();
	REQUIRE(f.d.resume() == Status::Ok);
	f.t.log.clear();
	f.t.eem_control_script = {0x0000, 0x0000, 0x0080};
	f.t.captured = {0x0008, 0x0000};
	f.d.detach(DetachAction::Reset);
	CHECK(f.scripts_consumed());
	CHECK(!f.d.attached());
	CHECK(!f.d.running());
	CHECK_LOG(f.t.log, Log{"ir:28", "ir:d0", "dr16:0000"} + eem_stop(1, kTb0Zero) +
				   sync_tail(0x6904, 0x0008) + clear_triggers() + kResetRelease);
}

TEST(debugger_detach_of_a_lost_running_target_only_lets_go)
{
	for (const DetachAction action : {DetachAction::Run, DetachAction::Reset}) {
		Fx f;
		f.attached();
		REQUIRE(f.d.resume() == Status::Ok);
		f.t.log.clear();
		f.t.mem_writes.clear();
		f.t.jtag_id = 0xFF;
		f.d.detach(action);
		CHECK(!f.d.attached());
		CHECK(!f.d.running());
		/* the halt found no target and let go: no trigger clearing, nothing
		 * written to a CPU that is not under JTAG control */
		CHECK_LOG(f.t.log, Log{"ir:28"} + kAbandon);
		CHECK(f.t.mem_writes.empty());
	}
}

/* detach does nothing either, before any attach and a second time */
TEST(debugger_run_control_needs_an_attached_cpu)
{
	Fx f;
	bool halted = false;
	CHECK(f.d.halt() == Status::BadState);
	CHECK(f.d.resume() == Status::BadState);
	CHECK(f.d.poll(halted) == Status::BadState);
	CHECK(halted); /* as documented: true when not attached */
	CHECK(f.d.step() == Status::BadState);
	f.d.detach(DetachAction::Run);
	f.d.detach(DetachAction::Reset);
	CHECK(f.t.log.empty());
	f.attached();
	f.d.detach(DetachAction::Run);
	f.t.log.clear();
	f.t.mem_writes.clear();
	CHECK(f.d.halt() == Status::BadState);
	CHECK(f.d.resume() == Status::BadState);
	halted = false;
	CHECK(f.d.poll(halted) == Status::BadState);
	CHECK(halted);
	CHECK(f.d.step() == Status::BadState);
	f.d.detach(DetachAction::Run);
	f.d.detach(DetachAction::Reset);
	CHECK(f.t.log.empty());
	CHECK(f.t.mem_writes.empty());
	CHECK(!f.d.running());
	CHECK(!f.d.attached());
}

TEST(debugger_halt_refuses_a_watchdog_without_its_password)
{
	/* The sync's watchdog check fails after the CPU reached full emulation:
	 * released at the PC where it stopped (release_unsaved), SR and WDTCTL
	 * unwritten, EEM off. */
	Fx f;
	f.attached();
	REQUIRE(f.d.resume() == Status::Ok);
	f.t.log.clear();
	f.t.mem_writes.clear();
	f.t.memory[0x015C] = 0x1234; /* link fault: not a WDTCTL read */
	f.t.eem_control_script = {0x0000, 0x0000, 0x0080};
	CHECK(f.d.halt() == Status::TargetError);
	CHECK(f.scripts_consumed());
	CHECK(!f.d.attached());
	CHECK(!f.d.running());
	CHECK(f.t.mem_writes.empty());
	CHECK(!contains(f.t.log, Log{"dr20:00144"})); /* no flash-controller wait */
	const Log want = read_word(0x015C) + release_at(0x4520, false, 0x0006) + Log{"close"};
	CHECK_LOG(last(f.t.log, want.size()), want);
}

TEST(debugger_poll_when_the_link_dies_after_the_id_read)
{
	/* The line floats from the EEM control read on: it reads 0xFFFF, which
	 * has EEM_STOPPED set, so the sync is attempted; its ID capture fails and
	 * the CPU is let go (abandon()). Nothing reaches memory or the EEM
	 * registers. */
	Fx f;
	f.attached();
	REQUIRE(f.d.resume() == Status::Ok);
	f.t.log.clear();
	f.t.mem_writes.clear();
	f.t.eem_writes.clear();
	f.t.eem_control_writes.clear();
	f.t.ones_after_shifts = f.t.shifts() + 1;
	bool halted = true;
	CHECK(f.d.poll(halted) == Status::TargetError);
	CHECK(!halted);
	CHECK(!f.d.attached());
	CHECK(!f.d.running());
	CHECK_LOG(f.t.log, (Log{"ir:28", "ir:d0", "dr16:0000", "ir:d0", "dr16:0000",
				"ir:30", "dr16:0005", "ir:c8", "dr16:1501", "ir:28"} +
			    kAbandon));
	CHECK(f.t.eem_control_writes == (std::vector<uint16_t>{0x0005, 0x0006}));
	CHECK(f.t.mem_writes.empty());
	CHECK(f.t.eem_writes.empty());
}

TEST(debugger_halt_racing_a_breakpoint_stop_reports_a_request)
{
	/* A breakpoint stops the CPU between WaitForEem's look (not stopped) and
	 * the macro's own look (stopped): the macro skips the stop request and
	 * syncs, and halt() reports Request. The CPU's state equals that of a
	 * breakpoint stop; only the label differs, as in the DLL, whose polling
	 * has the same window before its stop. */
	Fx f;
	f.attached();
	REQUIRE(f.d.set_breakpoint(2, 0x4600) == Status::Ok);
	REQUIRE(f.d.resume() == Status::Ok);
	f.t.log.clear();
	f.t.eem_control_script = {0x0000, 0x0080};
	f.t.captured = {0x0008, 0x0000};
	f.t.mab = 0x4604;
	REQUIRE(f.d.halt() == Status::Ok);
	CHECK(f.scripts_consumed());
	CHECK(!f.d.running());
	CHECK(f.d.stop_reason() == StopReason::Request);
	CHECK_EQ(f.d.breakpoint_hit(), msp430::kNoBreakpoint);
	CHECK_EQ(f.d.pc(), 0x4600u);
	/* the same transcript as a stop poll() finds, after the one look */
	CHECK_LOG(f.t.log, Log{"ir:28", "ir:d0", "dr16:0000", "ir:d0", "dr16:0000"} +
				   sync_tail(0x6904, 0x0008));
}

/* Task 12's test of the same name, with a breakpoint set and all of them
 * cleared as well */
TEST(debugger_halted_only_operations_refuse_a_running_cpu)
{
	Fx f;
	f.attached();
	REQUIRE(f.d.set_breakpoint(1, 0x4600) == Status::Ok);
	REQUIRE(f.d.resume() == Status::Ok);
	f.t.log.clear();
	f.t.mem_writes.clear();
	uint32_t regs[16] = {};
	uint8_t buf[2] = {};
	CHECK(f.d.read_regs(regs) == Status::Running);
	CHECK(f.d.write_reg(4, 0) == Status::Running);
	CHECK(f.d.read_mem(0x2400, 1, buf) == Status::Running);
	CHECK(f.d.write_mem(0x2400, 1, buf) == Status::Running);
	CHECK(f.d.step() == Status::Running);
	CHECK(f.d.set_breakpoint(0, 0x4400) == Status::Running);
	CHECK(f.d.clear_breakpoint(1) == Status::Running);
	CHECK(f.d.clear_breakpoint(msp430::kAllBreakpoints) == Status::Running);
	CHECK(f.t.log.empty());
	CHECK(f.t.mem_writes.empty());
	CHECK(f.d.running());
}

TEST(debugger_step_follows_mspds_single_step)
{
	Fx f;
	f.attached();
	f.t.eem[0x00] = 0x4600; /* a breakpoint lives in trigger block 0 */
	f.t.eem[0x06] = 0x0001;
	f.t.eem[0x80] = 0x0001;
	f.t.eem_control_script = {0x0000, 0x0080, 0x0080};
	f.t.captured = {0x0008, 0x0000};
	f.t.mab = 0x4526;
	REQUIRE(f.d.step() == Status::Ok);
	const Log want = Log{ir(0xB0)} + eem_read(0x00) + eem_read(0x02) + eem_read(0x04) +
			 eem_read(0x06) + eem_read(0x80) + eem_write(0x02, 0) +
			 eem_write(0x04, 0xFFFFF) + eem_write(0x06, 1) + eem_write(0x80, 1) +
			 restore_release(0x4520, 0x0008, 0x6904, 0x0007) +
			 Log{ir(0xD0), d16(0), d16(0)} +
			 /* stopped: the sync sees EEM_STOPPED and does not request a stop */
			 Log{ir(0xD0), d16(0)} + sync_tail(0x6904, 0x0008) + Log{ir(0xB0)} +
			 eem_write(0x00, 0x4600) + eem_write(0x02, 0) + eem_write(0x04, 0) +
			 eem_write(0x06, 1) + eem_write(0x80, 1);
	CHECK_LOG(f.t.log, want);
	CHECK(f.d.stop_reason() == StopReason::Step);
	CHECK_EQ(f.d.pc(), 0x4522u);
	CHECK(!f.d.running());
}

TEST(debugger_step_in_a_low_power_mode_without_interrupt_uses_no_trigger)
{
	Fx f;
	f.t.cntrl_sig = 0x0321;
	f.t.eem_control_script = {0x0000};
	f.t.captured = {0x00D8, 0x0000};
	REQUIRE(f.d.attach(AttachMode::Running, 250000, f.id) == Status::Ok);
	f.t.log.clear();
	f.t.eem_writes.clear();
	f.t.eem_control_script = {0x0000};
	f.t.captured = {0x00D8, 0x0000};
	REQUIRE(f.d.step() == Status::Ok);
	CHECK(f.t.eem_writes.empty());
	REQUIRE(f.t.log.size() > 2u);
	CHECK(f.t.log[0] == "tclk:0"); /* straight to RestoreContext */
	CHECK(contains(f.t.log, Log{"ir:a8", "ir:28", "ir:d0", "dr16:0000", "ir:28", "dr16:0000"}));
	CHECK(f.d.stop_reason() == StopReason::Step);
	/* SingleStepXv2.c:94-96, 147-156 in full: released with the EEM on
	 * (Note 1411: the PC 2 lower), the JTAG ID checked, the CPU taken again
	 * with no stop request, as it is off */
	CHECK(f.scripts_consumed());
	CHECK_LOG(f.t.log, restore_release(0x4522, 0x00D8, 0x6904, 0x0007) + Log{ir(0x28)} +
				   Log{ir(0xD0), d16(0), ir(0x28), d16(0)} + sync_tail(0x6904, 0x00D8));
	CHECK_EQ(f.d.pc(), 0x4522u); /* MAB - 2 */
	CHECK(f.d.attached());
	CHECK(!f.d.running());
}

TEST(debugger_step_that_does_not_stop_halts_and_times_out)
{
	Fx f;
	f.attached();
	f.t.eem_control = 0x0000; /* never stops by itself */
	f.t.eem_control_script.clear();
	/* after 496 polls: the sync's stop request, which stops at once */
	for (int i = 0; i < 496 + 1; ++i) {
		f.t.eem_control_script.push_back(0x0000);
	}
	f.t.eem_control_script.push_back(0x0080);
	f.t.captured = {0x0008, 0x0000};
	CHECK(f.d.step() == Status::Timeout);
	CHECK(!f.d.running());
	CHECK(f.d.attached());
	CHECK(f.d.stop_reason() == StopReason::Request);
	/* trigger block 0 restored last */
	REQUIRE(f.t.eem_writes.size() >= 5u);
	CHECK_EQ(f.t.eem_writes.back().first, 0x80);
}

namespace {

/* SingleStepXv2.c:88-93 with the MyIn table (lines 62-72): trigger block 0
 * and BREAKREACT read, then set to stop on the next instruction fetch. Unlike
 * the sync's EEM stop, the value register and GENCLKCTRL are not written. */
Log step_borrow()
{
	return Log{ir(0xB0)} + eem_read(0x00) + eem_read(0x02) + eem_read(0x04) +
	       eem_read(0x06) + eem_read(0x80) + eem_write(0x02, 0) +
	       eem_write(0x04, 0xFFFFF) + eem_write(0x06, 1) + eem_write(0x80, 1);
}

/* SingleStepXv2.c:101-106: the stop poll, `reads` control reads */
Log step_poll(unsigned reads)
{
	Log l = {ir(0xD0)};
	for (unsigned i = 0; i < reads; ++i) {
		l.push_back(d16(0));
	}
	return l;
}

/* SingleStepXv2.c:114-144: trigger block 0 and BREAKREACT written back */
Log step_give_back(const unsigned tb0[5])
{
	return Log{ir(0xB0)} + eem_write(0x00, tb0[0]) + eem_write(0x02, tb0[1]) +
	       eem_write(0x04, tb0[2]) + eem_write(0x06, tb0[3]) + eem_write(0x80, tb0[4]);
}

/* A breakpoint in trigger block 0 with a different value in each of its
 * five registers (value, control, mask, combination 0, BREAKREACT), so that
 * each must come back from its own read. */
constexpr unsigned kTb0Bp[5] = {0x14600, 0x0040, 0x00003, 0x0005, 0x00A1};

/* kTb0Bp under the step trigger (the MyIn table leaves the value register
 * alone): what the sync's own EEM stop after a step timeout finds in trigger
 * block 0 and puts back. */
constexpr unsigned kTb0StepTrigger[5] = {kTb0Bp[0], 0, 0xFFFFF, 1, 1};

void set_tb0(FakeTap &t, const unsigned tb0[5])
{
	t.eem[0x00] = tb0[0];
	t.eem[0x02] = tb0[1];
	t.eem[0x04] = tb0[2];
	t.eem[0x06] = tb0[3];
	t.eem[0x80] = tb0[4];
}

bool tb0_holds(FakeTap &t, const unsigned tb0[5])
{
	return t.eem[0x00] == tb0[0] && t.eem[0x02] == tb0[1] && t.eem[0x04] == tb0[2] &&
	       t.eem[0x06] == tb0[3] && t.eem[0x80] == tb0[4];
}

size_t ir_shifts(const Log &l)
{
	return static_cast<size_t>(std::count_if(l.begin(), l.end(), [](const std::string &s) {
		return s.compare(0, 3, "ir:") == 0;
	}));
}

/* Attached to a CPU in LPM3 (CPUOFF, GIE) with no interrupt pending; the
 * transcript and recordings cleared. PC 0x4522 (MAB - 2). */
void attached_in_lpm3(Fx &f)
{
	f.t.cntrl_sig = 0x0321;
	f.t.eem_control_script = {0x0000};
	f.t.captured = {0x00D8, 0x0000};
	REQUIRE(f.d.attach(AttachMode::Running, 250000, f.id) == Status::Ok);
	REQUIRE(f.scripts_consumed());
	f.t.log.clear();
	f.t.mem_writes.clear();
	f.t.eem_writes.clear();
	f.t.eem_control_writes.clear();
}

} // namespace

TEST(debugger_step_gives_back_each_register_of_trigger_block_0)
{
	Fx f;
	f.attached();
	set_tb0(f.t, kTb0Bp);
	f.t.eem_control_script = {0x0080, 0x0080}; /* stopped at the first read; the sync's look */
	f.t.captured = {0x0008, 0x0000};
	REQUIRE(f.d.step() == Status::Ok);
	CHECK(f.scripts_consumed());
	CHECK_LOG(f.t.log, step_borrow() + restore_release(0x4520, 0x0008, 0x6904, 0x0007) +
				   step_poll(1) + Log{ir(0xD0), d16(0)} + sync_tail(0x6904, 0x0008) +
				   step_give_back(kTb0Bp));
	CHECK(tb0_holds(f.t, kTb0Bp));
	CHECK(f.d.stop_reason() == StopReason::Step);
	CHECK_EQ(f.d.breakpoint_hit(), msp430::kNoBreakpoint);
}

TEST(debugger_breakpoint_0_survives_a_step)
{
	Fx f;
	f.attached();
	REQUIRE(f.d.set_breakpoint(0, 0x4600) == Status::Ok);
	REQUIRE(f.d.set_breakpoint(5, 0x4A00) == Status::Ok);
	f.t.eem_control_script = {0x0080, 0x0080};
	f.t.captured = {0x0008, 0x0000};
	REQUIRE(f.d.step() == Status::Ok);
	CHECK(f.scripts_consumed());
	/* breakpoints 0 and 5 armed in BREAKREACT again */
	const unsigned tb0[5] = {0x4600, 0, 0, 1, 0x21};
	CHECK(tb0_holds(f.t, tb0));
	/* and the debugger still counts it */
	REQUIRE(f.d.resume() == Status::Ok);
	f.t.eem_control_script = {0x0080, 0x0080};
	f.t.captured = {0x0008, 0x0000};
	f.t.mab = 0x4604;
	bool halted = false;
	REQUIRE(f.d.poll(halted) == Status::Ok);
	CHECK(f.scripts_consumed());
	CHECK(halted);
	CHECK_EQ(f.d.breakpoint_hit(), 0);
}

TEST(debugger_step_counts_a_stop_seen_by_the_495th_read)
{
	/* SingleStepXv2.c:101-108: the counter starts at 4 (left there by the
	 * loop over stream_in_sync) and the poll ends when it reaches 500: 496
	 * reads at most, and the step counts only if the stop showed while the
	 * counter was below 500, by the 495th read. Either way the stopped CPU
	 * is taken without a stop request; only the result differs. */
	for (const unsigned reads : {495u, 496u}) {
		Fx f;
		f.attached();
		set_tb0(f.t, kTb0Bp);
		f.t.eem_control_script.assign(reads - 1, 0x0000);
		f.t.eem_control_script.push_back(0x0080); /* stopped at this read */
		f.t.eem_control_script.push_back(0x0080); /* the sync's look */
		f.t.captured = {0x0008, 0x0000};
		const Status st = f.d.step();
		CHECK(f.scripts_consumed());
		CHECK_LOG(f.t.log, step_borrow() + restore_release(0x4520, 0x0008, 0x6904, 0x0007) +
					   step_poll(reads) + Log{ir(0xD0), d16(0)} +
					   sync_tail(0x6904, 0x0008) + step_give_back(kTb0Bp));
		CHECK(f.d.attached());
		CHECK(!f.d.running());
		CHECK(tb0_holds(f.t, kTb0Bp));
		if (reads == 495u) {
			CHECK(st == Status::Ok);
			CHECK(f.d.stop_reason() == StopReason::Step);
		} else {
			CHECK(st == Status::Timeout);
			CHECK(f.d.stop_reason() == StopReason::Request);
		}
	}
}

TEST(debugger_step_timeout_halts_through_the_eem_and_gives_trigger_block_0_back)
{
	/* Not in MSPDS, which returns an error and leaves the CPU running with
	 * the step trigger set: the sync stops it as halt() does, borrowing
	 * trigger block 0 in turn (it finds the step trigger there and puts it
	 * back), then the step gives back breakpoint 0. */
	Fx f;
	f.attached();
	set_tb0(f.t, kTb0Bp);
	f.t.eem_control_script.assign(496 + 1, 0x0000); /* the poll, then the sync's look */
	f.t.eem_control_script.push_back(0x0080);         /* the sync's stop request */
	f.t.captured = {0x0009, 0x0000};
	f.t.mab = 0x4A04;
	CHECK(f.d.step() == Status::Timeout);
	CHECK(f.scripts_consumed());
	CHECK_LOG(f.t.log, step_borrow() + restore_release(0x4520, 0x0008, 0x6904, 0x0007) +
				   step_poll(496) + eem_stop(1, kTb0StepTrigger) + sync_tail(0x6904, 0x0009) +
				   step_give_back(kTb0Bp));
	CHECK(tb0_holds(f.t, kTb0Bp));
	CHECK(f.d.attached());
	CHECK(!f.d.running());
	CHECK(f.d.stop_reason() == StopReason::Request);
	CHECK_EQ(f.d.breakpoint_hit(), msp430::kNoBreakpoint);
	CHECK_EQ(f.d.pc(), 0x4A00u);
	/* halted with the EEM off (the sync's last control write) */
	REQUIRE(!f.t.eem_control_writes.empty());
	CHECK_EQ(f.t.eem_control_writes.back(), 0x000E);
}

TEST(debugger_step_into_a_low_power_mode_times_out_halted_there)
{
	/* The instruction sends the CPU to sleep: no further fetch, the trigger
	 * never fires. The sync finds the CPU off and takes it without a stop
	 * request; the next resume puts it back to sleep. */
	Fx f;
	f.attached();
	set_tb0(f.t, kTb0Bp);
	f.t.eem_control_script.assign(496 + 1, 0x0000);
	f.t.has_cntrl_sig_after_release = true;
	f.t.cntrl_sig_after_release = 0x0321; /* CPUOFF */
	f.t.captured = {0x00D8, 0x0000};      /* LPM3 + GIE */
	f.t.mab = 0x4526;
	CHECK(f.d.step() == Status::Timeout);
	CHECK(f.scripts_consumed());
	CHECK_LOG(f.t.log, step_borrow() + restore_release(0x4520, 0x0008, 0x6904, 0x0007) +
				   step_poll(496) + Log{ir(0xD0), d16(0), ir(0x28), d16(0)} +
				   sync_tail(0x6904, 0x00D8) + step_give_back(kTb0Bp));
	CHECK(tb0_holds(f.t, kTb0Bp));
	CHECK(f.d.attached());
	CHECK(!f.d.running());
	CHECK(f.d.stop_reason() == StopReason::Request);
	CHECK_EQ(f.d.pc(), 0x4524u); /* MAB - 2 */
	f.t.log.clear();
	REQUIRE(f.d.resume() == Status::Ok);
	CHECK_LOG(f.t.log, restore_release(0x4524, 0x00D8, 0x6904, 0x0007));
}

TEST(debugger_step_with_an_interrupt_pending_in_a_low_power_mode_uses_the_trigger)
{
	/* SingleStepXv2.c:88: the CPU is off, but the sync saw an interrupt
	 * request (CNTRL_SIG 0x0004): it wakes up and runs, so the trigger is
	 * set up as for a running CPU */
	Fx f;
	f.t.cntrl_sig = 0x0325; /* CPUOFF + INTREQ */
	f.t.eem_control_script = {0x0000};
	f.t.captured = {0x00D8, 0x0000};
	REQUIRE(f.d.attach(AttachMode::Running, 250000, f.id) == Status::Ok);
	REQUIRE(f.scripts_consumed());
	CHECK_EQ(f.d.pc(), 0x4522u);
	f.t.log.clear();
	f.t.cntrl_sig = 0x0301; /* awake, in the interrupt handler */
	f.t.eem_control_script = {0x0080, 0x0080};
	f.t.captured = {0x0000, 0x0000};
	f.t.mab = 0x4A04;
	REQUIRE(f.d.step() == Status::Ok);
	CHECK(f.scripts_consumed());
	/* released as it was saved: SR with CPUOFF, the PC 2 lower (Note 1411) */
	CHECK_LOG(f.t.log, step_borrow() + restore_release(0x4522, 0x00D8, 0x6904, 0x0007) +
				   step_poll(1) + Log{ir(0xD0), d16(0)} + sync_tail(0x6904, 0x0000) +
				   step_give_back(kTb0Zero));
	CHECK(f.d.stop_reason() == StopReason::Step);
	CHECK_EQ(f.d.pc(), 0x4A00u);
}

TEST(debugger_step_from_a_low_power_mode_ends_the_session_on_a_lost_jtag_id)
{
	/* SingleStepXv2.c:149-161: after a release without a trigger MSPDS
	 * checks the JTAG ID and, if it is not valid, takes the device for gone
	 * into LPMx.5 and returns success. Here, as in poll(), an ID other than
	 * 0x91 (0x98 and 0x99 are valid for MSPDS, not the F5529's) ends the
	 * session: EEM off, released, pins let go; nothing written after the
	 * release. */
	for (const uint8_t id : {0xFF, 0x00, 0x98, 0x99}) {
		Fx f;
		attached_in_lpm3(f);
		const Log release = restore_release(0x4522, 0x00D8, 0x6904, 0x0007);
		f.t.jtag_id_script.assign(ir_shifts(release), 0x91);
		f.t.jtag_id_script.push_back(id);
		CHECK(f.d.step() == Status::TargetError);
		CHECK(f.t.jtag_id_script.empty());
		CHECK(!f.d.attached());
		CHECK(!f.d.running());
		CHECK(f.d.stop_reason() == StopReason::None);
		CHECK_LOG(f.t.log, release + Log{ir(0x28)} + kAbandon);
		CHECK(f.t.mem_writes.size() == 1u); /* the release's WDTCTL */
		CHECK(f.t.eem_writes.empty());
	}
}

TEST(debugger_step_from_a_low_power_mode_with_a_failed_sync)
{
	Fx f;
	attached_in_lpm3(f);
	f.t.has_cntrl_sig_after_release = true;
	f.t.cntrl_sig_after_release = 0x0121; /* still off; the sync bit never comes */
	f.t.eem_control_script = {0x0000};
	CHECK(f.d.step() == Status::TargetError);
	CHECK(f.scripts_consumed());
	CHECK(!f.d.attached());
	CHECK(!f.d.running());
	CHECK(f.d.stop_reason() == StopReason::None);
	/* the CPU, not in full emulation, is let go with nothing written */
	CHECK_LOG(last(f.t.log, kAbandon.size()), kAbandon);
	CHECK(f.t.mem_writes.size() == 1u); /* the release's WDTCTL */
	CHECK(f.t.eem_writes.empty());
}

TEST(debugger_step_with_a_failed_sync_gives_trigger_block_0_back)
{
	/* MSPDS restores trigger block 0 after the sync whatever the sync
	 * returned; here a failed sync lets the CPU go, so trigger block 0 goes
	 * back first, after the EEM is turned off */
	Fx f;
	f.attached();
	set_tb0(f.t, kTb0Bp);
	f.t.eem_control_script = {0x0080, 0x0080};
	f.t.has_cntrl_sig_after_release = true;
	f.t.cntrl_sig_after_release = 0x0101; /* running; the sync bit never comes */
	CHECK(f.d.step() == Status::TargetError);
	CHECK(f.scripts_consumed());
	CHECK(!f.d.attached());
	CHECK(!f.d.running());
	CHECK(f.d.stop_reason() == StopReason::None);
	CHECK(tb0_holds(f.t, kTb0Bp));
	const Log tail = Log{ir(0x30), d16(0x0006)} + step_give_back(kTb0Bp) + Log{ir(0xA8), "close"};
	CHECK_LOG(last(f.t.log, tail.size()), tail);
	CHECK(f.t.mem_writes.size() == 1u); /* the release's WDTCTL */
}

TEST(debugger_step_with_a_failed_init_state_check_gives_trigger_block_0_back)
{
	Fx f;
	f.attached();
	set_tb0(f.t, kTb0Bp);
	f.t.eem_control_script = {0x0080, 0x0080};
	/* control-signal reads: the release (SetPcXv2, the release clock), the
	 * sync, the wait check, CPUSUSP, i_SaveContext; the init-state check
	 * then sees none of the bits 0x0301 */
	f.t.cntrl_sig_script = {0x0301, 0x0301, 0x0301, 0x0301, 0x0301, 0x0301, 0x0000};
	CHECK(f.d.step() == Status::TargetError);
	CHECK(f.scripts_consumed());
	CHECK(!f.d.attached());
	CHECK(!f.d.running());
	CHECK(f.d.stop_reason() == StopReason::None);
	CHECK(tb0_holds(f.t, kTb0Bp));
	/* in full emulation with the EEM off (0x000E): trigger block 0 back,
	 * then released at the PC where it stopped, SR and WDTCTL unwritten */
	const Log tail = Log{ir(0x30), d16(0x000E), ir(0x28), d16(0)} + step_give_back(kTb0Bp) +
			 release_at(0x4520, false, 0x0006) + Log{"close"};
	CHECK_LOG(last(f.t.log, tail.size()), tail);
	CHECK(f.t.mem_writes.size() == 1u); /* the release's WDTCTL */
}

TEST(debugger_step_timeout_with_a_failed_sync_gives_trigger_block_0_back)
{
	Fx f;
	f.attached();
	set_tb0(f.t, kTb0Bp);
	f.t.eem_control_script.assign(496 + 1, 0x0000);
	f.t.eem_control_script.push_back(0x0080); /* the sync's stop request */
	f.t.has_cntrl_sig_after_release = true;
	f.t.cntrl_sig_after_release = 0x0101; /* running; the sync bit never comes */
	CHECK(f.d.step() == Status::TargetError);
	CHECK(f.scripts_consumed());
	CHECK(!f.d.attached());
	CHECK(!f.d.running());
	CHECK(f.d.stop_reason() == StopReason::None);
	CHECK(tb0_holds(f.t, kTb0Bp));
	CHECK(contains(f.t.log, eem_stop(1, kTb0StepTrigger)));
	const Log tail = Log{ir(0x30), d16(0x0006)} + step_give_back(kTb0Bp) + Log{ir(0xA8), "close"};
	CHECK_LOG(last(f.t.log, tail.size()), tail);
}

TEST(debugger_step_after_a_breakpoint_stop_reports_the_step)
{
	Fx f;
	f.attached();
	REQUIRE(f.d.set_breakpoint(2, 0x4600) == Status::Ok);
	REQUIRE(f.d.resume() == Status::Ok);
	f.t.eem_control_script = {0x0080, 0x0080};
	f.t.captured = {0x0008, 0x0000};
	f.t.mab = 0x4604;
	bool halted = false;
	REQUIRE(f.d.poll(halted) == Status::Ok);
	REQUIRE(halted);
	REQUIRE(f.d.breakpoint_hit() == 2);
	f.t.eem_control_script = {0x0080, 0x0080};
	f.t.captured = {0x0008, 0x0000};
	f.t.mab = 0x4608;
	REQUIRE(f.d.step() == Status::Ok);
	CHECK(f.scripts_consumed());
	CHECK(f.d.stop_reason() == StopReason::Step);
	CHECK_EQ(f.d.breakpoint_hit(), msp430::kNoBreakpoint);
	CHECK_EQ(f.d.pc(), 0x4604u);
}

TEST(debugger_failure_after_a_step_leaves_trigger_block_0_alone)
{
	/* Trigger block 0 went back at the end of the step; a later failure must
	 * not write the values the step saved over a breakpoint set since. */
	Fx f;
	f.attached();
	set_tb0(f.t, kTb0Bp);
	f.t.eem_control_script = {0x0080, 0x0080};
	f.t.captured = {0x0008, 0x0000};
	REQUIRE(f.d.step() == Status::Ok);
	REQUIRE(f.d.set_breakpoint(0, 0x4800) == Status::Ok);
	REQUIRE(f.d.resume() == Status::Ok);
	f.t.log.clear();
	f.t.eem_writes.clear();
	f.t.jtag_id = 0xFF; /* the target is lost */
	bool halted = true;
	CHECK(f.d.poll(halted) == Status::TargetError);
	CHECK_LOG(f.t.log, Log{"ir:28"} + kAbandon);
	CHECK(f.t.eem_writes.empty());
	CHECK_EQ(f.t.eem[0x00], 0x4800u);
}

TEST(debugger_step_after_an_sr_write_clearing_cpuoff_uses_the_trigger)
{
	/* SingleStepXv2.c:88 looks at the SR the release writes back (the DLL
	 * passes its register cache): a CPU saved in LPM3 whose SR was written
	 * without CPUOFF wakes up on the release and is stepped with the
	 * trigger, released at its PC (no Note 1411 correction) */
	Fx f;
	attached_in_lpm3(f);
	REQUIRE(f.d.write_reg(2, 0x00C8) == Status::Ok); /* SCG1 + SCG0 + GIE */
	CHECK(f.t.log.empty());
	f.t.cntrl_sig = 0x0301;
	f.t.eem_control_script = {0x0080, 0x0080};
	f.t.captured = {0x00C8, 0x0000};
	REQUIRE(f.d.step() == Status::Ok);
	CHECK(f.scripts_consumed());
	CHECK_LOG(f.t.log, step_borrow() + restore_release(0x4522, 0x00C8, 0x6904, 0x0007) +
				   step_poll(1) + Log{ir(0xD0), d16(0)} + sync_tail(0x6904, 0x00C8) +
				   step_give_back(kTb0Zero));
	CHECK(f.d.stop_reason() == StopReason::Step);
	CHECK_EQ(f.d.pc(), 0x4520u);
}

/* attach() on an open session ends it as detach(Run) does, a running CPU
 * halted first (its trigger blocks cleared, its context restored, the pins
 * released); the new session then starts from the identify step. */
TEST(debugger_attach_detaches_an_open_session_first)
{
	const Log detach_halted = clear_triggers() +
				  restore_release(0x4520, 0x0008, 0x6904, 0x0006) + Log{"close"};
	const Log new_session = identify_log() + eem_stop(1, kTb0Zero) +
				sync_tail(0x6904, 0x0008) + read_word(0x1A04) + clear_triggers() +
				Log{"ir:28", "dr16:0000"};
	for (const bool running : {false, true}) {
		Fx f;
		f.attached();
		REQUIRE(f.d.set_breakpoint(3, 0x4600) == Status::Ok);
		Log halt;
		if (running) {
			REQUIRE(f.d.resume() == Status::Ok);
			/* the halt: WaitForEem's look, the sync's look, the stop poll;
			 * then the new session's look and stop poll */
			f.t.eem_control_script = {0x0000, 0x0000, 0x0080, 0x0000, 0x0080};
			f.t.captured = {0x0008, 0x0000, 0x0008, 0x0000};
			const unsigned tb0[5] = {0, 0, 0, 0, 0x0008}; /* BREAKREACT: breakpoint 3 */
			halt = Log{"ir:28", "ir:d0", "dr16:0000"} + eem_stop(1, tb0) +
			       sync_tail(0x6904, 0x0008);
		} else {
			f.running_cpu();
		}
		f.t.log.clear();
		REQUIRE(f.d.attach(AttachMode::Running, 250000, f.id) == Status::Ok);
		CHECK(f.scripts_consumed());
		CHECK_LOG(f.t.log, halt + detach_halted + new_session);
		CHECK(f.d.attached());
		CHECK(!f.d.running());
		CHECK(f.d.stop_reason() == StopReason::Request);
	}
}

/* The debugger never starts a funclet and never writes flash or the flash
 * controller. Every operation, on each of the branches below, run against
 * the fake: writes memory only at WDTCTL, SFRIFG1 and the RAM the test asks
 * for; feeds the CPU only the instructions FakeTap allows (cpu_words: the
 * only store is MOVA Rn, &0x0FF6); uses only the JTAG instructions listed
 * at the end; feeds nothing to the mailbox. The fault flags are checked
 * after each operation (the fixture checks them at the end of every
 * test). */
TEST(debugger_never_writes_flash)
{
	using Write = std::pair<uint32_t, uint16_t>;
	Fx f;
	f.t.memory[0x0102] = 0x0008; /* SFRIFG1.VMAIFG: reset and halt clears it */
	/* firmware words in main flash, copied to RAM below */
	f.t.memory[0x4400] = 0x4031;
	f.t.memory[0x4402] = 0x4400;
	f.t.memory[0x4404] = 0x40B2;
	f.t.memory[0x4406] = 0x5A80;

	/* reset and halt, detach */
	REQUIRE(f.d.attach(AttachMode::Reset, 250000, f.id) == Status::Ok);
	CHECK_LOG(f.faults(), Log{});
	f.d.detach(DetachAction::Run);
	CHECK_LOG(f.faults(), Log{});

	/* attach to a running CPU while the flash controller is busy: the wait
	 * gives up after four reads */
	f.t.memory[0x0144] = 0x9659; /* FCTL3.BUSY */
	f.t.millis_step = 30;
	REQUIRE(f.attach() == Status::Ok);
	CHECK_LOG(f.faults(), Log{});
	f.t.memory[0x0144] = 0x9658;
	f.t.millis_step = 1;
	/* registers, with a message waiting in the mailbox */
	f.t.push_jmb_out(0x12345678);
	uint32_t regs[16] = {};
	CHECK(f.d.read_regs(regs) == Status::Ok);
	CHECK(f.t.jmb_out.empty());
	CHECK_LOG(f.faults(), Log{});
	CHECK(f.d.write_reg(5, 0x1234) == Status::Ok);
	CHECK_LOG(f.faults(), Log{});
	CHECK(f.d.write_reg(0, 0x4600) == Status::Ok);
	CHECK_LOG(f.faults(), Log{});
	/* memory: flash copied to RAM; the watchdog written through the
	 * saved context */
	uint8_t buf[8] = {1, 2, 3, 4, 5, 6, 7, 8};
	CHECK(f.d.read_mem(0x4400, 4, buf) == Status::Ok);
	CHECK_LOG(f.faults(), Log{});
	CHECK(f.d.write_mem(0x2400, 4, buf) == Status::Ok);
	CHECK_LOG(f.faults(), Log{});
	const uint8_t wdt[] = {0x36, 0x5A};
	CHECK(f.d.write_mem(0x015C, 1, wdt) == Status::Ok);
	REQUIRE(!f.t.mem_writes.empty());
	CHECK(f.t.mem_writes.back() == (Write{0x015C, 0x5AB6})); /* kept held */
	CHECK_LOG(f.faults(), Log{});
	/* breakpoints */
	CHECK(f.d.set_breakpoint(1, 0x4600) == Status::Ok);
	CHECK_LOG(f.faults(), Log{});
	CHECK(f.d.set_breakpoint(2, 0x4800) == Status::Ok);
	CHECK_LOG(f.faults(), Log{});
	/* a step through trigger block 0 */
	f.t.eem_control_script = {0x0000, 0x0080, 0x0080};
	f.t.captured = {0x0008, 0x0000};
	CHECK(f.d.step() == Status::Ok);
	CHECK_LOG(f.faults(), Log{});
	/* a step that does not stop: halted through the EEM */
	f.t.eem_control_script.assign(496 + 1, 0x0000);
	f.t.eem_control_script.push_back(0x0080);
	f.t.captured = {0x0008, 0x0000};
	CHECK(f.d.step() == Status::Timeout);
	CHECK_LOG(f.faults(), Log{});
	/* run; a poll while running, one that finds a breakpoint stop */
	CHECK(f.d.resume() == Status::Ok);
	CHECK_LOG(f.faults(), Log{});
	bool halted = true;
	CHECK(f.d.poll(halted) == Status::Ok);
	CHECK(!halted);
	CHECK_LOG(f.faults(), Log{});
	f.t.eem_control_script = {0x0080, 0x0080};
	f.t.captured = {0x0008, 0x0000};
	f.t.mab = 0x4804;
	CHECK(f.d.poll(halted) == Status::Ok);
	CHECK(halted);
	CHECK(f.d.stop_reason() == StopReason::Breakpoint);
	CHECK_EQ(f.d.breakpoint_hit(), 2);
	CHECK_LOG(f.faults(), Log{});
	/* run; a halt that stops the CPU through the EEM */
	CHECK(f.d.resume() == Status::Ok);
	CHECK_LOG(f.faults(), Log{});
	f.t.eem_control_script = {0x0000, 0x0000, 0x0080};
	f.t.captured = {0x0008, 0x0000};
	CHECK(f.d.halt() == Status::Ok);
	CHECK(f.d.stop_reason() == StopReason::Request);
	CHECK_LOG(f.faults(), Log{});
	CHECK(f.d.clear_breakpoint(1) == Status::Ok);
	CHECK_LOG(f.faults(), Log{});
	CHECK(f.d.clear_breakpoint(msp430::kAllBreakpoints) == Status::Ok);
	CHECK_LOG(f.faults(), Log{});
	/* run; detach, which halts it first */
	CHECK(f.d.resume() == Status::Ok);
	CHECK_LOG(f.faults(), Log{});
	f.t.eem_control_script = {0x0000, 0x0000, 0x0080};
	f.t.captured = {0x0008, 0x0000};
	f.d.detach(DetachAction::Run);
	CHECK(!f.d.attached());
	CHECK_LOG(f.faults(), Log{});

	/* attach to a CPU in LPM3; a step without a trigger; run; detach with a
	 * reset, which halts it first (in the low-power mode) */
	f.t.cntrl_sig = 0x0321; /* CPUOFF */
	f.t.eem_control_script = {0x0000};
	f.t.captured = {0x00D8, 0x0000};
	REQUIRE(f.d.attach(AttachMode::Running, 250000, f.id) == Status::Ok);
	CHECK_LOG(f.faults(), Log{});
	const size_t eem_writes = f.t.eem_writes.size();
	f.t.eem_control_script = {0x0000};
	f.t.captured = {0x00D8, 0x0000};
	CHECK(f.d.step() == Status::Ok);
	CHECK_EQ(f.t.eem_writes.size(), eem_writes); /* no trigger: the CPU is off */
	CHECK_LOG(f.faults(), Log{});
	CHECK(f.d.resume() == Status::Ok);
	CHECK_LOG(f.faults(), Log{});
	f.t.eem_control_script = {0x0000, 0x0000};
	f.t.captured = {0x00D8, 0x0000};
	f.d.detach(DetachAction::Reset);
	CHECK(!f.d.attached());
	CHECK_LOG(f.faults(), Log{});
	/* every scripted answer used: captured, eem_control_script, mab_script
	 * and cntrl_sig_script */
	CHECK(f.scripts_consumed());

	/* memory written only at WDTCTL, SFRIFG1 and the RAM asked for */
	Log rejected;
	std::vector<Write> ram;
	std::vector<Write> sfr;
	for (const Write &w : f.t.mem_writes) {
		if (w.first >= 0x2400u && w.first < 0x2408u) {
			ram.push_back(w);
		} else if (w.first == 0x0102u) {
			sfr.push_back(w);
		} else if (w.first != 0x015Cu) {
			rejected.push_back(dbglog::f("%05x=", w.first) + dbglog::f("%04x", w.second));
		}
	}
	CHECK_LOG(rejected, Log{});
	/* and those writes did reach the fake: the words read from flash into
	 * RAM, VMAIFG cleared */
	CHECK(ram == (std::vector<Write>{{0x2400, 0x4031}, {0x2402, 0x4400},
					 {0x2404, 0x40B2}, {0x2406, 0x5A80}}));
	CHECK(sfr == (std::vector<Write>{{0x0102, 0x0000}}));

	/* Only the JTAG instructions of debug.cpp and of Cpuxv2::identify,
	 * in_full_emulation and disconnect: none of IR_DATA_QUICK (Phase A's
	 * quick memory access), IR_TEST_REG, IR_TEST_3V_REG or any other. */
	Log allowed;
	for (const uint8_t code :
	     {msp430::kIrAddrCapture, msp430::kIrCntrlSigCapture, msp430::kIrEmexWriteControl,
	      msp430::kIrDataCapture, msp430::kIrData16Bit, msp430::kIrJmbExchange,
	      msp430::kIrDataToAddr, msp430::kIrCntrlSigRelease, msp430::kIrEmexDataExchange32,
	      msp430::kIrAddr16Bit, msp430::kIrCntrlSig16Bit, msp430::kIrEmexReadControl,
	      msp430::kIrDeviceId, msp430::kIrCoreipId}) {
		allowed.push_back(ir(code));
	}
	Log other_irs;
	for (const std::string &e : f.t.log) {
		if (e.compare(0, 3, "ir:") == 0 &&
		    std::find(allowed.begin(), allowed.end(), e) == allowed.end() &&
		    std::find(other_irs.begin(), other_irs.end(), e) == other_irs.end()) {
			other_irs.push_back(e);
		}
	}
	CHECK_LOG(other_irs, Log{});
	CHECK(f.t.jmb_in.empty()); /* nothing fed to a funclet */
}
