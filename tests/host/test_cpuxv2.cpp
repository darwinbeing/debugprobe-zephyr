#include "testing.hpp"
#include "fake_tap.hpp"
#include "msp430/cpuxv2.hpp"

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
	CHECK_LOG(t.log, write_word_log("0015c", "5a80") + write_word_log("0015e", "1234"));
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
			     "tclk:1"};
	CHECK_LOG(t.log, want);
	const uint8_t want_bytes[] = {0x55, 0x29, 0xEF, 0xBE};
	CHECK_BYTES(out, want_bytes);
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
