#include "testing.hpp"
#include "fake_tap.hpp"
#include "msp430/cpuxv2.hpp"

#include <algorithm>
#include <string>
#include <vector>

using msp430::Cpuxv2;
using msp430::Status;

namespace {

using Log = std::vector<std::string>;

/* Append b to a. */
Log operator+(Log a, const Log &b)
{
	a.insert(a.end(), b.begin(), b.end());
	return a;
}

void check_log(const char *file, int line, const Log &got, const Log &want)
{
	if (got == want) {
		return;
	}
	std::string msg = "TAP transcript differs\n  want:";
	for (const auto &s : want) {
		msg += " " + s;
	}
	msg += "\n  got: ";
	for (const auto &s : got) {
		msg += " " + s;
	}
	::testing::fail(file, line, msg);
}
#define CHECK_LOG(got, want) check_log(__FILE__, __LINE__, got, want)

/* WriteMem_430Xv2(F_WORD, addr, data); addr/data as formatted by FakeTap. */
Log write_word_log(const char *addr20, const char *data16)
{
	return {"ir:28", "dr16:0000", "tclk:0", "ir:c8", "dr16:0500", "ir:c1",
		std::string("dr20:") + addr20, "tclk:1", "ir:a1",
		std::string("dr16:") + data16, "tclk:0", "ir:c8", "dr16:0501",
		"tclk:1", "tclk:0", "tclk:1"};
}

/* SetPC_430Xv2(addr): mova = 0x0080 | ((addr >> 8) & 0x0F00), then the low word. */
Log set_pc_log(const char *mova16, const char *low16)
{
	return {"ir:28", "dr16:0000", "tclk:0", "ir:82", "tclk:1",
		std::string("dr16:") + mova16, "tclk:0", "ir:c8", "dr16:1400",
		"ir:82", "tclk:0", "tclk:1", std::string("dr16:") + low16,
		"tclk:0", "tclk:1", "dr16:4303", "tclk:0", "ir:21", "dr20:00000"};
}

} // namespace

TEST(cpuxv2_write_mem_follows_ti_writemem)
{
	FakeTap t;
	Cpuxv2 cpu{t};
	const uint8_t data[] = {0x80, 0x5A, 0x34, 0x12};
	CHECK(cpu.write_mem(0x015C, 2, data) == Status::Ok);
	CHECK_LOG(t.log, (write_word_log("0015c", "5a80") + write_word_log("0015e", "1234") +
			 Log{"ir:28", "dr16:0000"}));
}

TEST(cpuxv2_read_mem_follows_ti_readmemquick)
{
	FakeTap t;
	Cpuxv2 cpu{t};
	t.quick_words = {0x2955, 0xBEEF};
	uint8_t out[4] = {};
	CHECK(cpu.read_mem(0x11A04, 2, out) == Status::Ok);
	/* ReadMemQuick_430Xv2: SetPC, SetTCLK, CNTRL_SIG 0x0501, ADDR_CAPTURE,
	 * DATA_QUICK, then per word SetTCLK / ClrTCLK / DR_Shift16, final SetTCLK. */
	const Log want = set_pc_log("0180", "1a04") +
			 Log{"tclk:1", "ir:c8", "dr16:0501", "ir:21", "ir:c2",
			     "tclk:1", "tclk:0", "dr16:0000",
			     "tclk:1", "tclk:0", "dr16:0000",
			     "tclk:1", "ir:28", "dr16:0000"};
	CHECK_LOG(t.log, want);
	const uint8_t want_bytes[] = {0x55, 0x29, 0xEF, 0xBE};
	CHECK_BYTES(out, want_bytes);
}

TEST(cpuxv2_connect_reads_the_device_id_at_the_pointer_the_device_holds)
{
	/* A pointer with five different nibbles pins both halves of the
	 * conversion; the F5529's 0x01A00 has zeros in the nibbles that move. */
	FakeTap t;
	t.device_id_ptr = 0xABCD4;
	t.quick_words = {0x2955};
	Cpuxv2 cpu{t};
	msp430::Identity id;
	CHECK(cpu.connect(msp430::EntryMode::Normal, 250000, id) == Status::Ok);
	CHECK_EQ(id.device_id_ptr, 0xABCD4u);
	/* the ID word is read at pointer + 4 = 0xABCD8 */
	const Log pc = set_pc_log("0a80", "bcd8");
	CHECK(std::search(t.log.begin(), t.log.end(), pc.begin(), pc.end()) != t.log.end());
}

TEST(cpuxv2_write_mem_needs_full_emulation_state)
{
	FakeTap t;
	t.cntrl_sig = 0x0000;
	Cpuxv2 cpu{t};
	const uint8_t data[] = {0x00, 0x00};
	CHECK(cpu.write_mem(0x2400, 1, data) == Status::TargetError);
	CHECK_LOG(t.log, (Log{"ir:28", "dr16:0000"}));
}

TEST(cpuxv2_read_mem_needs_full_emulation_state)
{
	FakeTap t;
	t.cntrl_sig = 0x0000;
	Cpuxv2 cpu{t};
	uint8_t out[2] = {};
	CHECK(cpu.read_mem(0x2400, 1, out) == Status::TargetError);
	CHECK_LOG(t.log, (Log{"ir:28", "dr16:0000"}));
}

namespace {

/* ExecutePOR_430Xv2, including the watchdog hold and the final state check. */
Log por_log()
{
	Log l = {"ir:28", "tclk:0", "tclk:1", "ir:c8", "dr16:0c01", "dr16:0401"};
	for (int i = 0; i < 5; ++i) {
		l.push_back("tclk:0");
		l.push_back("tclk:1");
	}
	l = l + Log{"ir:c8", "dr16:0501", "tclk:0", "tclk:1"};
	return l + write_word_log("0015c", "5a80") + Log{"ir:28", "dr16:0000"};
}

/* SyncJtag_AssertPor with the sync bit already set on the first poll. */
Log sync_por_log()
{
	return Log{"ir:c8", "dr16:1501", "ir:28", "dr16:0000"} + por_log();
}

bool contains(const Log &log, const Log &sub)
{
	if (sub.empty() || sub.size() > log.size()) {
		return false;
	}
	for (size_t i = 0; i + sub.size() <= log.size(); ++i) {
		if (std::equal(sub.begin(), sub.end(), log.begin() + static_cast<long>(i))) {
			return true;
		}
	}
	return false;
}

} // namespace

TEST(cpuxv2_connect_follows_ti_getdevice)
{
	FakeTap t;
	t.quick_words = {0x2955};
	Cpuxv2 cpu{t};
	msp430::Identity id;
	CHECK(cpu.connect(msp430::EntryMode::Normal, 250000, id) == Status::Ok);
	CHECK_EQ(id.jtag_id, 0x91);
	CHECK_EQ(id.core_id, 0x0106);
	CHECK_EQ(id.device_id_ptr, 0x1A00u);
	CHECK_EQ(id.device_id, 0x2955);

	const Log want =
		Log{"speed:250000", "open", "enter:high", "ir:28",
		    /* IsLockKeyProgrammed: three tries */
		    "ir:28", "dr16:aaaa", "ir:28", "dr16:aaaa", "ir:28", "dr16:aaaa",
		    /* GetCoreipIdXv2 */
		    "ir:e8", "dr16:0000", "ir:e1", "dr20:00000"} +
		sync_por_log() +
		/* ReadMemQuick(DeviceIdPointer + 4, 1) */
		set_pc_log("0080", "1a04") +
		Log{"tclk:1", "ir:c8", "dr16:0501", "ir:21", "ir:c2",
		    "tclk:1", "tclk:0", "dr16:0000", "tclk:1", "ir:28", "dr16:0000"};
	CHECK_LOG(t.log, want);
}

TEST(cpuxv2_connect_reports_no_target_with_the_id_read)
{
	FakeTap t;
	t.jtag_id = 0xFF; /* data line stuck high: nothing connected */
	Cpuxv2 cpu{t};
	msp430::Identity id;
	CHECK(cpu.connect(msp430::EntryMode::Normal, 250000, id) == Status::NoTarget);
	CHECK_EQ(id.jtag_id, 0xFF);
	CHECK_LOG(t.log, (Log{"speed:250000", "open", "enter:high", "ir:28", "close"}));
}

TEST(cpuxv2_connect_busy_when_pins_unavailable)
{
	FakeTap t;
	t.open_ok = false;
	Cpuxv2 cpu{t};
	msp430::Identity id;
	CHECK(cpu.connect(msp430::EntryMode::Normal, 250000, id) == Status::Busy);
	CHECK_LOG(t.log, (Log{"speed:250000", "open"}));
}

TEST(cpuxv2_connect_refuses_locked_device)
{
	FakeTap t;
	t.cntrl_sig_script = {0x5555};
	Cpuxv2 cpu{t};
	msp430::Identity id;
	CHECK(cpu.connect(msp430::EntryMode::Normal, 250000, id) == Status::Locked);
	REQUIRE(!t.log.empty());
	CHECK(t.log.back() == "close");
}

TEST(cpuxv2_connect_fails_when_sync_never_arrives)
{
	FakeTap t;
	t.cntrl_sig = 0x0101; /* sync bit 0x0200 never set */
	Cpuxv2 cpu{t};
	msp430::Identity id;
	CHECK(cpu.connect(msp430::EntryMode::Normal, 250000, id) == Status::TargetError);
	REQUIRE(!t.log.empty());
	CHECK(t.log.back() == "close");
	/* 50 sync polls after the 0x1501 write */
	size_t polls = 0;
	bool after_sync_cmd = false;
	for (const auto &s : t.log) {
		if (s == "dr16:1501") {
			after_sync_cmd = true;
		} else if (after_sync_cmd && s == "dr16:0000") {
			++polls;
		}
	}
	CHECK_EQ(polls, 50u);
}

TEST(cpuxv2_connect_magic_pattern_entry)
{
	FakeTap t;
	t.quick_words = {0x2955};
	Cpuxv2 cpu{t};
	msp430::Identity id;
	CHECK(cpu.connect(msp430::EntryMode::MagicPattern, 250000, id) == Status::Ok);
	/* magicPattern(): RST-low entry, feed 0xA55A to the mailbox, RST-high
	 * entry, read the ID, reset the LPMx.5 I/O lock. */
	const Log head = {"speed:250000", "open", "enter:low",
			  "ir:86", "dr16:0000", "dr16:0001", "dr16:a55a",
			  "enter:high", "ir:28", "ir:f4", "dr16:4020"};
	REQUIRE(t.log.size() > head.size());
	CHECK_LOG(Log(t.log.begin(), t.log.begin() + static_cast<long>(head.size())), head);
	const std::vector<uint16_t> fed = {0xA55A};
	CHECK(t.jmb_in == fed);
}

TEST(cpuxv2_connect_magic_pattern_without_mailbox_is_no_target)
{
	FakeTap t;
	t.jmb_in_ready = false; /* mailbox input never ready */
	Cpuxv2 cpu{t};
	msp430::Identity id;
	CHECK(cpu.connect(msp430::EntryMode::MagicPattern, 250000, id) == Status::NoTarget);
	CHECK_EQ(id.jtag_id, 0);
	REQUIRE(!t.log.empty());
	CHECK(t.log.back() == "close");
	CHECK(!t.jmb_protocol_error);
}

TEST(cpuxv2_disconnect_resets_and_releases)
{
	FakeTap t;
	Cpuxv2 cpu{t};
	cpu.disconnect();
	/* ReleaseDevice_430Xv2(V_RESET), then the pins */
	CHECK_LOG(t.log, (Log{"ir:c8", "dr16:0c01", "dr16:0401", "ir:a8", "close"}));
}

TEST(cpuxv2_erase_loads_patches_runs_and_cleans_up)
{
	FakeTap t;
	t.push_jmb_out(0xCAFEBABE);
	Cpuxv2 cpu{t};
	CHECK(cpu.erase(msp430::EraseMode::Main, 0x14400, 0x2400) == Status::Ok);
	CHECK(t.jmb_out.empty());
	CHECK(!t.jmb_protocol_error);
	/* i_ReadJmbOut: status, JMB32B|OUTREQ, low, high */
	CHECK(contains(t.log, Log{"ir:86", "dr16:0000", "dr16:0014", "dr16:0000", "dr16:0000"}));

	/* 89 funclet words, the watchdog write of the POR, 89 words of JMP $ */
	REQUIRE(t.mem_writes.size() == 89u + 1u + 89u);
	CHECK_EQ(t.mem_writes[0].first, 0x2400u);
	CHECK_EQ(t.mem_writes[0].second, 0x0016); /* entry offset, unpatched */
	CHECK_EQ(t.mem_writes[2].second, 0x4400); /* address low */
	CHECK_EQ(t.mem_writes[3].second, 0x0001); /* address high */
	CHECK_EQ(t.mem_writes[4].second, 0xA506); /* erase all main memory */
	CHECK_EQ(t.mem_writes[5].second, 0xA548); /* Info A stays locked */
	CHECK_EQ(t.mem_writes[6].second, 0xA508); /* first unpatched word */
	CHECK_EQ(t.mem_writes[88].first, 0x2400u + 2u * 88u);
	CHECK_EQ(t.mem_writes[89].first, 0x015Cu);
	CHECK_EQ(t.mem_writes[90].first, 0x2400u);
	CHECK_EQ(t.mem_writes[90].second, 0x3FFF);
	CHECK_EQ(t.mem_writes[178].first, 0x2400u + 2u * 88u);
	CHECK_EQ(t.mem_writes[178].second, 0x3FFF);

	/* ReleaseDevice_430Xv2(ram + entry offset): PC = 0x2416 */
	CHECK(contains(t.log, set_pc_log("0080", "2416") +
				      Log{"tclk:1", "ir:c8", "dr16:0401", "ir:21", "ir:a8"}));
}

TEST(cpuxv2_erase_segment_and_bank_modes)
{
	for (auto [mode, fctl1] : {std::pair{msp430::EraseMode::Segment, 0xA502},
				   std::pair{msp430::EraseMode::Bank, 0xA504}}) {
		FakeTap t;
		t.push_jmb_out(0xCAFEBABE);
		Cpuxv2 cpu{t};
		CHECK(cpu.erase(mode, 0x1800, 0x2400) == Status::Ok);
		REQUIRE(t.mem_writes.size() > 4u);
		CHECK_EQ(t.mem_writes[4].second, fctl1);
	}
}

TEST(cpuxv2_erase_times_out_but_still_cleans_up)
{
	FakeTap t; /* mailbox never reports completion */
	Cpuxv2 cpu{t};
	const uint32_t before = t.millis_now;
	CHECK(cpu.erase(msp430::EraseMode::Main, 0x4400, 0x2400) == Status::Timeout);
	CHECK_EQ(t.millis_now - before, 201u); /* 200 ms erase wait */
	CHECK_EQ(t.mem_writes.size(), 89u + 1u + 89u);
	CHECK(!t.jmb_protocol_error);
}

TEST(cpuxv2_flash_write_handshake)
{
	FakeTap t;
	Cpuxv2 cpu{t};

	t.push_jmb_out(0xABADBABE);
	CHECK(cpu.flash_begin(0x4400, 2, 0x2400) == Status::Ok);
	REQUIRE(t.mem_writes.size() == 120u);
	CHECK_EQ(t.mem_writes[0].second, 0x001C);
	CHECK_EQ(t.mem_writes[2].second, 0x4400); /* address low */
	CHECK_EQ(t.mem_writes[3].second, 0x0000); /* address high */
	CHECK_EQ(t.mem_writes[4].second, 0x0002); /* length low, in words */
	CHECK_EQ(t.mem_writes[5].second, 0x0000); /* length high */
	CHECK_EQ(t.mem_writes[6].second, 0xA548); /* Info A stays locked */
	CHECK_EQ(t.mem_writes[7].second, 0xA508);

	t.log.clear();
	const uint8_t data[] = {0x34, 0x12, 0x78, 0x56};
	CHECK(cpu.flash_data(2, data) == Status::Ok);
	const std::vector<uint16_t> sent = {0x1234, 0x5678};
	CHECK(t.jmb_in == sent);
	/* i_WriteJmbIn16 per word: wait for IN0RDY, INREQ, data */
	CHECK_LOG(t.log, (Log{"ir:86", "dr16:0000", "dr16:0001", "dr16:1234",
			      "ir:86", "dr16:0000", "dr16:0001", "dr16:5678"}));

	t.push_jmb_out(0xCAFEBABE);
	CHECK(cpu.flash_end(true) == Status::Ok);
	CHECK(t.jmb_out.empty());
	CHECK(!t.jmb_protocol_error);
	REQUIRE(t.mem_writes.size() == 120u + 1u + 120u);
	CHECK_EQ(t.mem_writes.back().second, 0x3FFF);
}

TEST(cpuxv2_flash_begin_times_out_when_funclet_does_not_start)
{
	FakeTap t;
	Cpuxv2 cpu{t};
	const uint32_t before = t.millis_now;
	CHECK(cpu.flash_begin(0x4400, 2, 0x2400) == Status::Timeout);
	CHECK_EQ(t.millis_now - before, 51u); /* 50 ms ready wait */
	CHECK_EQ(t.mem_writes.size(), 120u + 1u + 120u); /* cleaned up */
	CHECK(!t.jmb_protocol_error);
}

TEST(cpuxv2_flash_data_times_out_and_cleans_up)
{
	FakeTap t;
	Cpuxv2 cpu{t};
	t.push_jmb_out(0xABADBABE);
	CHECK(cpu.flash_begin(0x4400, 2, 0x2400) == Status::Ok);
	t.jmb_in_ready = false; /* mailbox input never ready */
	const uint8_t data[] = {0x34, 0x12};
	const uint32_t before = t.millis_now;
	CHECK(cpu.flash_data(1, data) == Status::Timeout);
	CHECK_EQ(t.millis_now - before, 21u); /* 20 ms mailbox wait */
	CHECK_EQ(t.mem_writes.size(), 120u + 1u + 120u);
	CHECK(!t.jmb_protocol_error);
}

TEST(cpuxv2_flash_end_incomplete_skips_the_completion_wait)
{
	FakeTap t;
	Cpuxv2 cpu{t};
	t.push_jmb_out(0xABADBABE);
	CHECK(cpu.flash_begin(0x4400, 2, 0x2400) == Status::Ok);
	t.log.clear();
	CHECK(cpu.flash_end(false) == Status::Ok);
	CHECK(!contains(t.log, Log{"ir:86"}));
	CHECK_EQ(t.mem_writes.size(), 120u + 1u + 120u);
}

TEST(cpuxv2_connect_fails_when_core_id_is_zero)
{
	FakeTap t;
	t.core_id = 0;
	Cpuxv2 cpu{t};
	msp430::Identity id;
	CHECK(cpu.connect(msp430::EntryMode::Normal, 250000, id) == Status::TargetError);
	REQUIRE(!t.log.empty());
	CHECK(t.log.back() == "close");
}

TEST(cpuxv2_connect_fails_when_id_changes_during_sync)
{
	FakeTap t;
	/* the 8th IR shift of a normal connect is the ID check in SyncJtag_AssertPor */
	t.jtag_id_script = {0x91, 0x91, 0x91, 0x91, 0x91, 0x91, 0x91, 0x89};
	Cpuxv2 cpu{t};
	msp430::Identity id;
	CHECK(cpu.connect(msp430::EntryMode::Normal, 250000, id) == Status::TargetError);
	REQUIRE(!t.log.empty());
	CHECK(t.log.back() == "close");
	CHECK(!contains(t.log, Log{"dr16:0c01"})); /* no POR was attempted */
}

TEST(cpuxv2_erase_load_failure_is_target_error)
{
	FakeTap t;
	t.cntrl_sig = 0x0000;
	Cpuxv2 cpu{t};
	CHECK(cpu.erase(msp430::EraseMode::Main, 0x4400, 0x2400) == Status::TargetError);
	CHECK(t.mem_writes.empty());
	CHECK(contains(t.log, Log{"ir:c8", "dr16:1501"})); /* cleanup sync attempted */
}

TEST(cpuxv2_erase_cleanup_sync_failure_is_target_error)
{
	FakeTap t;
	t.push_jmb_out(0xCAFEBABE);
	t.has_cntrl_sig_after_release = true;
	t.cntrl_sig_after_release = 0x0101;
	Cpuxv2 cpu{t};
	CHECK(cpu.erase(msp430::EraseMode::Main, 0x4400, 0x2400) == Status::TargetError);
	CHECK(t.jmb_out.empty()); /* the completion was seen */
	CHECK_EQ(t.mem_writes.size(), 89u); /* RAM not rewritten: sync failed */
}

TEST(cpuxv2_flash_end_times_out_without_completion)
{
	FakeTap t;
	Cpuxv2 cpu{t};
	t.push_jmb_out(0xABADBABE);
	CHECK(cpu.flash_begin(0x4400, 1, 0x2400) == Status::Ok);
	const uint8_t data[] = {0x34, 0x12};
	CHECK(cpu.flash_data(1, data) == Status::Ok);
	const uint32_t before = t.millis_now;
	CHECK(cpu.flash_end(true) == Status::Timeout);
	CHECK_EQ(t.millis_now - before, 101u); /* 100 ms completion wait */
	CHECK_EQ(t.mem_writes.size(), 120u + 1u + 120u);
	CHECK(!t.jmb_protocol_error);
}

TEST(cpuxv2_flash_calls_need_an_active_write)
{
	FakeTap t;
	Cpuxv2 cpu{t};
	const uint8_t data[] = {0x34, 0x12};
	CHECK(cpu.flash_data(1, data) == Status::BadState);
	CHECK(cpu.flash_end(false) == Status::BadState);
	CHECK(cpu.flash_end(true) == Status::BadState);
	CHECK(t.log.empty());

	t.push_jmb_out(0xCAFEBABE);
	CHECK(cpu.erase(msp430::EraseMode::Main, 0x4400, 0x2400) == Status::Ok);
	const size_t writes = t.mem_writes.size();
	CHECK(cpu.flash_end(false) == Status::BadState);
	CHECK_EQ(t.mem_writes.size(), writes);

	t.push_jmb_out(0xABADBABE);
	CHECK(cpu.flash_begin(0x4400, 2, 0x2400) == Status::Ok);
	t.quick_words = {0x2955};
	msp430::Identity id;
	CHECK(cpu.connect(msp430::EntryMode::Normal, 250000, id) == Status::Ok);
	CHECK(cpu.flash_end(false) == Status::BadState); /* new session, no write */
}

TEST(cpuxv2_flash_begin_rejects_zero_words)
{
	FakeTap t;
	Cpuxv2 cpu{t};
	CHECK(cpu.flash_begin(0x4400, 0, 0x2400) == Status::BadArg);
	CHECK(t.log.empty());
}

TEST(cpuxv2_flash_begin_patches_high_words)
{
	FakeTap t;
	Cpuxv2 cpu{t};
	t.push_jmb_out(0xABADBABE);
	CHECK(cpu.flash_begin(0x1C400, 0x10002, 0x2400) == Status::Ok);
	REQUIRE(t.mem_writes.size() > 5u);
	CHECK_EQ(t.mem_writes[2].second, 0xC400); /* address low */
	CHECK_EQ(t.mem_writes[3].second, 0x0001); /* address high */
	CHECK_EQ(t.mem_writes[4].second, 0x0002); /* length low */
	CHECK_EQ(t.mem_writes[5].second, 0x0001); /* length high */
}

TEST(cpuxv2_mailbox_wait_ignores_stray_values)
{
	{
		FakeTap t;
		Cpuxv2 cpu{t};
		t.push_jmb_out(0x12345678);
		t.push_jmb_out(0xCAFEBABE);
		CHECK(cpu.erase(msp430::EraseMode::Main, 0x4400, 0x2400) == Status::Ok);
		CHECK(t.jmb_out.empty());
	}
	{
		FakeTap t;
		Cpuxv2 cpu{t};
		t.push_jmb_out(0x12345678);
		CHECK(cpu.erase(msp430::EraseMode::Main, 0x4400, 0x2400) == Status::Timeout);
		CHECK(t.jmb_out.empty()); /* the stray value was read and rejected */
	}
}

TEST(cpuxv2_write_state_ends_with_the_write)
{
	const uint8_t data[] = {0x34, 0x12};
	{
		FakeTap t;
		Cpuxv2 cpu{t};
		t.push_jmb_out(0xABADBABE);
		CHECK(cpu.flash_begin(0x4400, 1, 0x2400) == Status::Ok);
		CHECK(cpu.flash_data(1, data) == Status::Ok);
		t.push_jmb_out(0xCAFEBABE);
		CHECK(cpu.flash_end(true) == Status::Ok);
		CHECK(cpu.flash_data(1, data) == Status::BadState);
	}
	{
		FakeTap t;
		Cpuxv2 cpu{t};
		t.push_jmb_out(0xABADBABE);
		CHECK(cpu.flash_begin(0x4400, 1, 0x2400) == Status::Ok);
		t.jmb_in_ready = false;
		CHECK(cpu.flash_data(1, data) == Status::Timeout);
		CHECK(cpu.flash_end(false) == Status::BadState);
	}
	{
		FakeTap t;
		Cpuxv2 cpu{t};
		t.push_jmb_out(0xABADBABE);
		CHECK(cpu.flash_begin(0x4400, 1, 0x2400) == Status::Ok);
		cpu.disconnect();
		CHECK(cpu.flash_end(false) == Status::BadState);
	}
}

TEST(cpuxv2_flash_begin_load_failure_is_target_error)
{
	FakeTap t;
	t.cntrl_sig = 0x0000;
	Cpuxv2 cpu{t};
	CHECK(cpu.flash_begin(0x4400, 1, 0x2400) == Status::TargetError);
	CHECK(t.mem_writes.empty());
	CHECK(contains(t.log, Log{"ir:c8", "dr16:1501"}));
	CHECK(cpu.flash_end(false) == Status::BadState);
}

TEST(cpuxv2_flash_end_cleanup_sync_failure_is_target_error)
{
	FakeTap t;
	t.has_cntrl_sig_after_release = true;
	t.cntrl_sig_after_release = 0x0101;
	Cpuxv2 cpu{t};
	t.push_jmb_out(0xABADBABE);
	CHECK(cpu.flash_begin(0x4400, 1, 0x2400) == Status::Ok);
	const uint8_t data[] = {0x34, 0x12};
	CHECK(cpu.flash_data(1, data) == Status::Ok);
	t.push_jmb_out(0xCAFEBABE);
	CHECK(cpu.flash_end(true) == Status::TargetError);
	CHECK(t.jmb_out.empty());
	CHECK_EQ(t.mem_writes.size(), 120u); /* no RAM overwrite after the failed sync */
}

TEST(cpuxv2_other_operations_refused_during_a_write)
{
	FakeTap t;
	Cpuxv2 cpu{t};
	t.push_jmb_out(0xABADBABE);
	CHECK(cpu.flash_begin(0x4400, 1, 0x2400) == Status::Ok);
	t.log.clear();
	uint8_t buf[2] = {};
	const uint8_t data[] = {0x34, 0x12};
	CHECK(cpu.erase(msp430::EraseMode::Main, 0x4400, 0x2400) == Status::BadState);
	CHECK(cpu.flash_begin(0x4400, 1, 0x2400) == Status::BadState);
	CHECK(cpu.read_mem(0x4400, 1, buf) == Status::BadState);
	CHECK(cpu.write_mem(0x2400, 1, data) == Status::BadState);
	CHECK(t.log.empty());
}

TEST(cpuxv2_mailbox_wait_polls_once_more_after_the_deadline)
{
	FakeTap t;
	Cpuxv2 cpu{t};
	t.push_jmb_out(0xCAFEBABE);
	t.jmb_hidden_polls = 1;
	t.millis_per_shift = 250;
	CHECK(cpu.erase(msp430::EraseMode::Main, 0x4400, 0x2400) == Status::Ok);
}

TEST(cpuxv2_mailbox_write_polls_once_more_after_the_deadline)
{
	FakeTap t;
	Cpuxv2 cpu{t};
	t.push_jmb_out(0xABADBABE);
	CHECK(cpu.flash_begin(0x4400, 1, 0x2400) == Status::Ok);
	t.jmb_hidden_polls = 1;
	t.millis_per_shift = 30;
	const uint8_t data[] = {0x34, 0x12};
	CHECK(cpu.flash_data(1, data) == Status::Ok);
	const std::vector<uint16_t> sent = {0x1234};
	CHECK(t.jmb_in == sent);
}

TEST(cpuxv2_failed_cleanup_outranks_the_timeout)
{
	const uint8_t data[] = {0x34, 0x12};
	{
		FakeTap t;
		Cpuxv2 cpu{t};
		t.has_cntrl_sig_after_release = true;
		t.cntrl_sig_after_release = 0x0101;
		CHECK(cpu.erase(msp430::EraseMode::Main, 0x4400, 0x2400) == Status::TargetError);
	}
	{
		FakeTap t;
		Cpuxv2 cpu{t};
		t.has_cntrl_sig_after_release = true;
		t.cntrl_sig_after_release = 0x0101;
		CHECK(cpu.flash_begin(0x4400, 1, 0x2400) == Status::TargetError);
	}
	{
		FakeTap t;
		Cpuxv2 cpu{t};
		t.has_cntrl_sig_after_release = true;
		t.cntrl_sig_after_release = 0x0101;
		t.push_jmb_out(0xABADBABE);
		CHECK(cpu.flash_begin(0x4400, 1, 0x2400) == Status::Ok);
		t.jmb_in_ready = false;
		CHECK(cpu.flash_data(1, data) == Status::TargetError);
	}
	{
		FakeTap t;
		Cpuxv2 cpu{t};
		t.has_cntrl_sig_after_release = true;
		t.cntrl_sig_after_release = 0x0101;
		t.push_jmb_out(0xABADBABE);
		CHECK(cpu.flash_begin(0x4400, 1, 0x2400) == Status::Ok);
		CHECK(cpu.flash_data(1, data) == Status::Ok);
		CHECK(cpu.flash_end(true) == Status::TargetError);
	}
}

TEST(cpuxv2_read_mem_detects_a_link_that_died_mid_read)
{
	FakeTap t;
	t.cntrl_sig = 0x0000;
	t.cntrl_sig_script = {0x0301}; /* only the check in set_pc sees the CPU */
	Cpuxv2 cpu{t};
	uint8_t out[4] = {};
	CHECK(cpu.read_mem(0x4400, 2, out) == Status::TargetError);
}

TEST(cpuxv2_write_mem_detects_a_link_that_died_mid_write)
{
	FakeTap t;
	t.cntrl_sig = 0x0000;
	t.cntrl_sig_script = {0x0301};
	Cpuxv2 cpu{t};
	const uint8_t data[] = {0x34, 0x12};
	CHECK(cpu.write_mem(0x2400, 1, data) == Status::TargetError);
	CHECK_EQ(t.mem_writes.size(), 1u); /* the word itself was sent */
}

TEST(cpuxv2_read_mem_detects_a_floating_data_line)
{
	FakeTap t;
	t.ones_after_shifts = 40;
	Cpuxv2 cpu{t};
	uint8_t out[128] = {};
	CHECK(cpu.read_mem(0x4400, 64, out) == Status::TargetError);
}

TEST(cpuxv2_write_mem_detects_a_floating_data_line)
{
	FakeTap t;
	t.ones_after_shifts = 20;
	Cpuxv2 cpu{t};
	const uint8_t data[16] = {};
	CHECK(cpu.write_mem(0x2400, 8, data) == Status::TargetError);
}

TEST(cpuxv2_connect_detects_a_floating_data_line_after_the_id)
{
	FakeTap t;
	t.ones_after_shifts = 1; /* the first IR shift still returns 0x91 */
	Cpuxv2 cpu{t};
	msp430::Identity id;
	CHECK(cpu.connect(msp430::EntryMode::Normal, 250000, id) != Status::Ok);
	REQUIRE(!t.log.empty());
	CHECK(t.log.back() == "close");
}

TEST(cpuxv2_flash_data_detects_a_floating_data_line)
{
	FakeTap t;
	Cpuxv2 cpu{t};
	t.push_jmb_out(0xABADBABE);
	CHECK(cpu.flash_begin(0x4400, 4, 0x2400) == Status::Ok);
	t.ones_after_shifts = t.shifts(); /* the line floats from now on */
	const uint8_t data[] = {0x34, 0x12, 0x78, 0x56};
	CHECK(cpu.flash_data(2, data) == Status::TargetError);
	CHECK(t.jmb_in.empty());
}

TEST(cpuxv2_erase_detects_a_floating_data_line)
{
	FakeTap t;
	t.ones_after_shifts = 400; /* inside the funclet load */
	Cpuxv2 cpu{t};
	CHECK(cpu.erase(msp430::EraseMode::Main, 0x4400, 0x2400) == Status::TargetError);
}

TEST(cpuxv2_identify_stops_before_the_sync)
{
	FakeTap t;
	Cpuxv2 cpu{t};
	msp430::Identity id;
	CHECK(cpu.identify(msp430::EntryMode::Normal, 250000, id) == Status::Ok);
	CHECK_EQ(id.jtag_id, 0x91);
	CHECK_EQ(id.core_id, 0x0106);
	CHECK_EQ(id.device_id_ptr, 0x1A00u);
	CHECK_EQ(id.device_id, 0);
	/* GetCoreID + IsLockKeyProgrammed + GetCoreipIdXv2, nothing else: no
	 * control-signal write, so the CPU is neither synchronized nor reset. */
	CHECK_LOG(t.log, (Log{"speed:250000", "open", "enter:high", "ir:28",
			      "ir:28", "dr16:aaaa", "ir:28", "dr16:aaaa", "ir:28", "dr16:aaaa",
			      "ir:e8", "dr16:0000", "ir:e1", "dr20:00000"}));
	CHECK(t.mem_writes.empty());
}

TEST(cpuxv2_identify_failures_release_the_pins)
{
	{
		FakeTap t;
		t.jtag_id = 0xFF;
		Cpuxv2 cpu{t};
		msp430::Identity id;
		CHECK(cpu.identify(msp430::EntryMode::Normal, 250000, id) == Status::NoTarget);
		CHECK(t.log.back() == "close");
	}
	{
		FakeTap t;
		t.cntrl_sig_script = {0x5555};
		Cpuxv2 cpu{t};
		msp430::Identity id;
		CHECK(cpu.identify(msp430::EntryMode::Normal, 250000, id) == Status::Locked);
		CHECK(t.log.back() == "close");
	}
	{
		FakeTap t;
		t.core_id = 0;
		Cpuxv2 cpu{t};
		msp430::Identity id;
		CHECK(cpu.identify(msp430::EntryMode::Normal, 250000, id) == Status::TargetError);
		CHECK(t.log.back() == "close");
	}
}

TEST(cpuxv2_in_full_emulation_is_public)
{
	FakeTap t;
	Cpuxv2 cpu{t};
	CHECK(cpu.in_full_emulation());
	t.cntrl_sig = 0x0000;
	CHECK(!cpu.in_full_emulation());
	t.cntrl_sig = 0x0301;
	t.jtag_id = 0x89;
	CHECK(!cpu.in_full_emulation());
}
