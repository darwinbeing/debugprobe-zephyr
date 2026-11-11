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
