#include "testing.hpp"
#include "mock_sbw_port.hpp"
#include "msp430/sbw_tap.hpp"

using msp430::SbwTap;

TEST(sbw_tap_reset_reaches_run_test_idle)
{
	MockSbwPort p;
	SbwTap t{p};
	t.reset();
	/* 6 cycles TMS=1,TDI=1 then one cycle TMS=0,TDI=1 */
	REQUIRE(p.cycle_calls.size() == 1u);
	CHECK_EQ(p.cycle_calls[0].tms, 0x3Fu);
	CHECK_EQ(p.cycle_calls[0].tdi, 0x7Fu);
	CHECK_EQ(p.cycle_calls[0].count, 7u);
	CHECK_EQ(p.state, MockSbwPort::RTI);
}

TEST(sbw_tap_ir_shift_vector_with_tclk_high)
{
	MockSbwPort p;
	SbwTap t{p};
	t.reset();
	t.ir_shift(0x28);
	/* cycle: 0 (1,T) 1 (1,1) 2 (0,1) 3 (0,1) | 4..11 data MSB first,
	 * TMS=1 on cycle 11 | 12 (1,1) | 13 (0,T).   T = TCLK = 1
	 * 0x28 = 0010 1000 -> TDI=1 on cycles 6 and 8.
	 * tms = bits 0,1,11,12            = 0x1803
	 * tdi = bits 0,1,2,3,6,8,12,13    = 0x314F */
	REQUIRE(!p.cycle_calls.empty());
	const auto &c = p.cycle_calls.back();
	CHECK_EQ(c.tms, 0x1803u);
	CHECK_EQ(c.tdi, 0x314Fu);
	CHECK_EQ(c.count, 14u);
}

TEST(sbw_tap_ir_shift_vector_with_tclk_low)
{
	MockSbwPort p;
	SbwTap t{p};
	t.reset();
	t.clr_tclk();
	t.ir_shift(0x28);
	/* same as above but TDI=0 on cycles 0 and 13 */
	REQUIRE(!p.cycle_calls.empty());
	const auto &c = p.cycle_calls.back();
	CHECK_EQ(c.tms, 0x1803u);
	CHECK_EQ(c.tdi, 0x114Eu);
	CHECK_EQ(c.count, 14u);
}

TEST(sbw_tap_ir_shift_returns_captured_id_msb_first)
{
	MockSbwPort p;
	SbwTap t{p};
	t.reset();
	/* 0x91 = 1001 0001 arrives on data cycles 4..11 -> bits 4, 7, 11 */
	p.tdo_script.push_back((1u << 4) | (1u << 7) | (1u << 11));
	CHECK_EQ(t.ir_shift(0x28), 0x91);
}

TEST(sbw_tap_dr16_vector)
{
	MockSbwPort p;
	SbwTap t{p};
	t.reset();
	t.dr_shift16(0xA55A);
	/* cycle: 0 (1,T) 1 (0,1) 2 (0,1) | 3..18 data, TMS=1 on 18 | 19 (1,1) | 20 (0,T)
	 * 0xA55A MSB first = 1010 0101 0101 1010 -> TDI=1 on cycles
	 * 3,5,8,10,12,14,15,17 = 0x2D528
	 * tms = bits 0,18,19                    = 0xC0001
	 * tdi = 0x7 + 0x2D528 + bit19 + bit20   = 0x1AD52F */
	REQUIRE(!p.cycle_calls.empty());
	const auto &c = p.cycle_calls.back();
	CHECK_EQ(c.tms, 0xC0001u);
	CHECK_EQ(c.tdi, 0x1AD52Fu);
	CHECK_EQ(c.count, 21u);
}

TEST(sbw_tap_dr16_returns_tdo_msb_first)
{
	MockSbwPort p;
	SbwTap t{p};
	t.reset();
	/* 0x1234 = 0001 0010 0011 0100 on data cycles 3..18 -> cycles 6,9,13,14,16 */
	p.tdo_script.push_back((1u << 6) | (1u << 9) | (1u << 13) | (1u << 14) | (1u << 16));
	CHECK_EQ(t.dr_shift16(0), 0x1234);
}

TEST(sbw_tap_dr20_vector)
{
	MockSbwPort p;
	SbwTap t{p};
	t.reset();
	t.dr_shift20(0x12345);
	/* 20 data cycles 3..22, TMS=1 on 22, update on 23, idle on 24.
	 * 0x12345 = 0001 0010 0011 0100 0101 -> TDI=1 on cycles 6,9,13,14,16,20,22
	 * = 0x516240; tdi = 0x7 + 0x516240 + bit23 + bit24 = 0x1D16247
	 * tms = bits 0,22,23 = 0xC00001 */
	REQUIRE(!p.cycle_calls.empty());
	const auto &c = p.cycle_calls.back();
	CHECK_EQ(c.tms, 0xC00001u);
	CHECK_EQ(c.tdi, 0x1D16247u);
	CHECK_EQ(c.count, 25u);
}

TEST(sbw_tap_dr20_descrambles_result)
{
	MockSbwPort p;
	SbwTap t{p};
	t.reset();
	/* TI's AllShifts de-scrambles every 20-bit result:
	 * ((v << 16) + (v >> 4)) & 0xFFFFF turns raw 0x1A000 into 0x01A00.
	 * raw 0x1A000 = 0001 1010 0000 0000 0000 on cycles 3..22 -> cycles 6,7,9 */
	p.tdo_script.push_back((1u << 6) | (1u << 7) | (1u << 9));
	CHECK_EQ(t.dr_shift20(0), 0x01A00u);
}

TEST(sbw_tap_scans_decode_to_the_shifted_values)
{
	MockSbwPort p;
	SbwTap t{p};
	t.reset();
	t.ir_shift(0xC8);
	t.dr_shift16(0xBEEF);
	t.dr_shift20(0x54321);
	REQUIRE(p.scans.size() == 3u);
	CHECK(p.scans[0] == (MockSbwPort::Scan{true, 8, 0xC8}));
	CHECK(p.scans[1] == (MockSbwPort::Scan{false, 16, 0xBEEF}));
	CHECK(p.scans[2] == (MockSbwPort::Scan{false, 20, 0x54321}));
	CHECK_EQ(p.state, MockSbwPort::RTI);
}

TEST(sbw_tap_tclk_tracks_level_and_uses_ldh_when_high)
{
	MockSbwPort p;
	SbwTap t{p};
	t.reset();
	p.log.clear();
	t.clr_tclk(); /* TCLK starts high -> LDH variant */
	t.set_tclk(); /* now low -> plain */
	t.set_tclk(); /* high again -> LDH variant */
	REQUIRE(p.log.size() == 3u);
	CHECK(p.log[0] == "tclk:1>0");
	CHECK(p.log[1] == "tclk:0>1");
	CHECK(p.log[2] == "tclk:1>1");
	CHECK(!p.tclk_outside_rti);
}

TEST(sbw_tap_shift_keeps_tclk_low_at_both_ends)
{
	MockSbwPort p;
	SbwTap t{p};
	t.reset();
	t.clr_tclk();
	t.dr_shift16(0xFFFF);
	REQUIRE(!p.cycle_calls.empty());
	const auto &c = p.cycle_calls.back();
	CHECK_EQ(c.tdi & 1u, 0u);          /* leaving Run-Test/Idle */
	CHECK_EQ((c.tdi >> 20) & 1u, 0u);  /* re-entering Run-Test/Idle */
}

namespace {

std::vector<std::string> strs(std::initializer_list<const char *> l)
{
	return {l.begin(), l.end()};
}

} // namespace

TEST(sbw_tap_open_releases_then_acquires)
{
	MockSbwPort p;
	SbwTap t{p};
	CHECK(t.open());
	/* StopJtag (release, 15 ms) + ConnectJTAG (drive, 15 ms) */
	CHECK(p.log == strs({"release", "delay:15000", "acquire", "delay:15000"}));
}

TEST(sbw_tap_open_fails_when_pins_are_taken)
{
	MockSbwPort p;
	p.acquire_ok = false;
	SbwTap t{p};
	CHECK(!t.open());
	CHECK(p.log == strs({"release", "delay:15000", "acquire"}));
}

TEST(sbw_tap_enter_rst_high_sequence)
{
	MockSbwPort p;
	SbwTap t{p};
	t.enter(msp430::Entry::RstHigh);
	/* EntrySequences_RstHigh_SBW, then ResetTAP */
	CHECK(p.log == strs({"tck:0", "delay:4000", "tdio:1", "tck:1", "delay:20000",
			     "tdio:1", "delay:60", "pulse", "delay:60", "delay:5000",
			     "cycles:3f:7f:7"}));
	CHECK_EQ(p.state, MockSbwPort::RTI);
}

TEST(sbw_tap_enter_rst_low_sequence)
{
	MockSbwPort p;
	SbwTap t{p};
	t.enter(msp430::Entry::RstLow);
	/* EntrySequences_RstLow_SBW, then ResetTAP */
	CHECK(p.log == strs({"tck:0", "delay:1000", "tdio:0", "delay:50000", "tck:1",
			     "delay:100000", "tdio:1", "delay:40", "pulse", "delay:40",
			     "delay:5000", "cycles:3f:7f:7"}));
}

TEST(sbw_tap_enter_restores_tclk_high)
{
	MockSbwPort p;
	SbwTap t{p};
	t.reset();
	t.clr_tclk();
	t.enter(msp430::Entry::RstHigh);
	p.log.clear();
	t.clr_tclk();
	CHECK(p.log == strs({"tclk:1>0"})); /* TCLK was high again after entry */
	CHECK(!p.tclk_glitch);
}

TEST(sbw_tap_shifts_never_glitch_tclk)
{
	MockSbwPort p;
	SbwTap t{p};
	t.reset();
	for (int pass = 0; pass < 2; ++pass) { /* TCLK high, then low */
		t.ir_shift(0xC8);
		t.dr_shift16(0x1234);
		t.dr_shift20(0xABCDE);
		CHECK(!p.tclk_glitch);
		t.clr_tclk();
	}
	CHECK(!p.bad_count);
	CHECK_EQ(p.state, MockSbwPort::RTI);
}

TEST(sbw_tap_shift_preserves_tracked_tclk)
{
	MockSbwPort p;
	SbwTap t{p};
	t.reset();
	t.clr_tclk();
	t.dr_shift16(0xFFFF);
	p.log.clear();
	t.set_tclk(); /* still low after the shift -> plain TMS slot */
	CHECK(p.log == strs({"tclk:0>1"}));
}

TEST(sbw_tap_dr20_vector_with_tclk_low)
{
	MockSbwPort p;
	SbwTap t{p};
	t.reset();
	t.clr_tclk();
	t.dr_shift20(0x12345);
	/* as sbw_tap_dr20_vector, but TDI = 0 on cycles 0 and 24 */
	REQUIRE(!p.cycle_calls.empty());
	const auto &c = p.cycle_calls.back();
	CHECK_EQ(c.tms, 0xC00001u);
	CHECK_EQ(c.tdi, 0x0D16246u);
	CHECK_EQ(c.count, 25u);
}

TEST(sbw_tap_dr32_is_split_into_two_port_calls)
{
	MockSbwPort p;
	SbwTap t{p};
	t.reset();
	p.cycle_calls.clear();
	t.dr_shift32(0x80000083);
	/* 3 lead-in + 32 data + 2 trailing cycles = 37: one call of 32, one of 5.
	 * cycle: 0 (1,T) 1 (0,1) 2 (0,1) | 3..34 data, TMS=1 on 34 | 35 (1,1) | 36 (0,T)
	 * 0x80000083 MSB first: TDI=1 on cycles 3, 27, 33, 34.
	 * first call (cycles 0..31): tms = bit 0          = 0x00000001
	 *                            tdi = bits 0,1,2,3,27 = 0x0800000F
	 * second call (cycles 32..36): tms = 34, 35 -> bits 2, 3 = 0x0C
	 *                              tdi = 33, 34, 35, 36 -> bits 1..4 = 0x1E */
	REQUIRE(p.cycle_calls.size() == 2u);
	CHECK_EQ(p.cycle_calls[0].count, 32u);
	CHECK_EQ(p.cycle_calls[0].tms, 0x00000001u);
	CHECK_EQ(p.cycle_calls[0].tdi, 0x0800000Fu);
	CHECK_EQ(p.cycle_calls[1].count, 5u);
	CHECK_EQ(p.cycle_calls[1].tms, 0x0Cu);
	CHECK_EQ(p.cycle_calls[1].tdi, 0x1Eu);
	REQUIRE(!p.scans.empty());
	CHECK(p.scans.back() == (MockSbwPort::Scan{false, 32, 0x80000083}));
	CHECK_EQ(p.state, MockSbwPort::RTI);
	CHECK(!p.tclk_glitch);
	CHECK(!p.bad_count);
}

TEST(sbw_tap_dr32_assembles_tdo_across_the_calls)
{
	MockSbwPort p;
	SbwTap t{p};
	t.reset();
	/* 0xC0000003: data bits on cycles 3, 4 (first call) and 33, 34 (second
	 * call, bits 1 and 2). */
	p.tdo_script.push_back((1u << 3) | (1u << 4));
	p.tdo_script.push_back((1u << 1) | (1u << 2));
	CHECK_EQ(t.dr_shift32(0), 0xC0000003u);
	/* 0x0000000C: data bits 3 and 2 on cycles 31 and 32, either side of the
	 * split (bit 31 of the first call, bit 0 of the second). */
	p.tdo_script.push_back(1u << 31);
	p.tdo_script.push_back(1u << 0);
	CHECK_EQ(t.dr_shift32(0), 0x0000000Cu);
	/* The same with ones on every cycle that carries no data (0..2, 35,
	 * 36) and in bits 5..31 of the 5-cycle second call: ignored. */
	p.tdo_script.push_back((1u << 31) | 0x7u);
	p.tdo_script.push_back(0xFFFFFFF9u);
	CHECK_EQ(t.dr_shift32(0), 0x0000000Cu);
}

TEST(sbw_tap_dr32_with_tclk_low_keeps_tclk_low)
{
	MockSbwPort p;
	SbwTap t{p};
	t.reset();
	t.clr_tclk();
	p.cycle_calls.clear();
	t.dr_shift32(0x12345678);
	REQUIRE(p.cycle_calls.size() == 2u);
	CHECK_EQ(p.cycle_calls[0].tdi & 1u, 0u);          /* cycle 0: TDI = TCLK = 0 */
	CHECK_EQ((p.cycle_calls[1].tdi >> 4) & 1u, 0u);   /* cycle 36: TDI = TCLK = 0 */
	CHECK(!p.tclk_glitch);
	CHECK(!p.tclk);
	REQUIRE(!p.scans.empty());
	CHECK_EQ(p.scans.back().value, 0x12345678u);
}
