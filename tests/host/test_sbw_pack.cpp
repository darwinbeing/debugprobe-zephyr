#include "testing.hpp"

extern "C" {
#include "sbw_pack.h"
}

TEST(sbw_pack_single_cycle)
{
	/* TMS=1, MID=1, TDI=0: count field 0, cycle bits 0b011 at bit 4 */
	CHECK_EQ(sbw_pack_word(0x1, 0x1, 0x0, 0, 1), 0x30u);
	/* TMSLDH: TMS=0, MID=1, TDI=0 */
	CHECK_EQ(sbw_pack_word(0x0, 0x1, 0x0, 0, 1), 0x20u);
	/* TDI only */
	CHECK_EQ(sbw_pack_word(0x0, 0x0, 0x1, 0, 1), 0x40u);
}

TEST(sbw_pack_first_nine_cycles_of_an_ir_shift)
{
	/* IR shift 0x28 with TCLK high: tms 0x1803, tdi 0x314F, mid = tms.
	 * cycles 0..8 -> (tms|mid<<1|tdi<<2) = 7,7,4,4,0,0,4,0,4
	 * word = 8 | 7<<4 | 7<<7 | 4<<10 | 4<<13 | 4<<22 | 4<<28 */
	CHECK_EQ(sbw_pack_word(0x1803, 0x1803, 0x314F, 0, 9), 0x410093F8u);
}

TEST(sbw_pack_remaining_five_cycles_of_an_ir_shift)
{
	/* cycles 9..13 -> 0,0,3,7,4 ; word = 4 | 3<<10 | 7<<13 | 4<<16 */
	CHECK_EQ(sbw_pack_word(0x1803, 0x1803, 0x314F, 9, 5), 0x4EC04u);
}

TEST(sbw_pack_constants)
{
	CHECK_EQ(SBW_CYCLES_PER_WORD, 9u);
	CHECK_EQ(SBW_MAX_CYCLES, 32u);
}

TEST(sbw_pack_nine_cycles_all_ones_uses_31_bits)
{
	/* count field 8, then 27 one-bits: bit 31 stays clear */
	CHECK_EQ(sbw_pack_word(0x1FF, 0x1FF, 0x1FF, 0, 9), 0x7FFFFFF8u);
}

TEST(sbw_pack_window_ending_at_bit_31)
{
	/* cycles 27..31; only cycle 31 has TMS and TDI set -> bits 0b101 at
	 * position 4 + 3*4 = 16; count field 4 */
	CHECK_EQ(sbw_pack_word(0x80000000u, 0x0u, 0x80000000u, 27, 5), 0x00050004u);
}

TEST(sbw_pack_ignores_bits_outside_the_window)
{
	/* cycles 4..6 of all-ones vectors: three cycles of 0b111, count field 2 */
	CHECK_EQ(sbw_pack_word(0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 4, 3), 0x00001FF2u);
	/* the same window of all-zero vectors */
	CHECK_EQ(sbw_pack_word(0x0u, 0x0u, 0x0u, 4, 3), 0x00000002u);
}
