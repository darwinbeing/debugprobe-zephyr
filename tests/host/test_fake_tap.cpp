#include "testing.hpp"
#include "fake_tap.hpp"

#include <cstdint>
#include <utility>

/* The fake's EEM exchange under IR_EMEX_DATA_EXCHANGE32: a one-byte address
 * shift, then the data shift (MSPDS EemDataExchangeXv2.c). Anything else is
 * a protocol error. */

TEST(fake_tap_eem_exchange_reads_and_writes_without_error)
{
	FakeTap t;
	t.ir_shift(msp430::kIrEmexDataExchange32);
	t.dr_shift32(0x88); /* write the EEM control register */
	t.dr_shift32(0x00000007);
	t.dr_shift32(0x89); /* read it back */
	CHECK_EQ(t.dr_shift32(0), 0x00000007u);
	t.dr_shift32(0x01); /* read a register nothing wrote */
	CHECK_EQ(t.dr_shift32(0), 0u);
	t.ir_shift(msp430::kIrCntrlSigCapture); /* every exchange complete */
	t.dr_shift16(0x0000);
	REQUIRE(t.eem_writes.size() == 1u);
	CHECK(t.eem_writes[0] == (std::pair<uint8_t, uint32_t>{0x88, 0x00000007}));
	CHECK(!t.eem_protocol_error);
}

TEST(fake_tap_eem_ir_shift_with_address_pending_is_an_error)
{
	FakeTap t;
	t.ir_shift(msp430::kIrEmexDataExchange32);
	t.dr_shift32(0x89);
	t.ir_shift(msp430::kIrEmexDataExchange32); /* the data shift never came */
	CHECK(t.eem_protocol_error);
	/* The pending address was dropped: the next shift is an address again. */
	t.dr_shift32(0x88);
	t.dr_shift32(0x00000006);
	REQUIRE(t.eem_writes.size() == 1u);
	CHECK(t.eem_writes[0] == (std::pair<uint8_t, uint32_t>{0x88, 0x00000006}));
}

TEST(fake_tap_eem_address_above_one_byte_is_an_error)
{
	FakeTap t;
	t.ir_shift(msp430::kIrEmexDataExchange32);
	t.dr_shift32(0x00000188);
	CHECK(t.eem_protocol_error);
}

TEST(fake_tap_eem_16_or_20_bit_shift_is_an_error)
{
	FakeTap a;
	a.ir_shift(msp430::kIrEmexDataExchange32);
	a.dr_shift16(0x0000);
	CHECK(a.eem_protocol_error);

	FakeTap b;
	b.ir_shift(msp430::kIrEmexDataExchange32);
	b.dr_shift20(0x00000);
	CHECK(b.eem_protocol_error);
}
