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

/* The debug-layer model of the test double behaves as the Debugger tests
 * assume. */

TEST(fake_tap_reads_memory_after_an_address_load)
{
	FakeTap t;
	t.memory[0x2400] = 0x1234;
	t.captured = {0xAAAA};
	t.ir_shift(msp430::kIrAddr16Bit);
	t.dr_shift20(0x2400);
	t.ir_shift(msp430::kIrDataCapture);
	CHECK_EQ(t.dr_shift16(0), 0x1234);
	/* no address pending: the next capture is a CPU register word */
	CHECK_EQ(t.dr_shift16(0), 0xAAAA);
	CHECK_EQ(t.dr_shift16(0), 0x0000);
}

TEST(fake_tap_memory_writes_and_the_watchdog_password)
{
	FakeTap t;
	t.ir_shift(msp430::kIrAddr16Bit);
	t.dr_shift20(0x015C);
	t.ir_shift(msp430::kIrDataToAddr);
	t.dr_shift16(0x5A80);
	CHECK_EQ(t.memory[0x015C], 0x6980); /* WDTCTL reads back with 0x69 */
	t.ir_shift(msp430::kIrAddr16Bit);
	t.dr_shift20(0x2400);
	t.ir_shift(msp430::kIrDataToAddr);
	t.dr_shift16(0xBEEF);
	CHECK_EQ(t.memory[0x2400], 0xBEEF);
	REQUIRE(t.mem_writes.size() == 2u);
}

TEST(fake_tap_eem_exchange_and_control)
{
	FakeTap t;
	t.ir_shift(msp430::kIrEmexDataExchange32);
	t.dr_shift32(0x10);       /* write TB2 value */
	t.dr_shift32(0x4600);
	t.dr_shift32(0x11);       /* read it back */
	CHECK_EQ(t.dr_shift32(0), 0x4600u);
	REQUIRE(t.eem_writes.size() == 1u);
	CHECK_EQ(t.eem_writes[0].first, 0x10);
	t.eem_control_script = {0x0080};
	t.ir_shift(msp430::kIrEmexReadControl);
	CHECK_EQ(t.dr_shift16(0), 0x0080);
	CHECK_EQ(t.dr_shift16(0), 0x0000);
	t.ir_shift(msp430::kIrEmexWriteControl);
	t.dr_shift16(0x0007);
	REQUIRE(t.eem_control_writes.size() == 1u);
	CHECK_EQ(t.eem_control_writes[0], 0x0007);
	t.mab_script = {0x14A2C};
	t.ir_shift(msp430::kIrAddrCapture);
	CHECK_EQ(t.dr_shift20(0), 0x14A2Cu);
	CHECK_EQ(t.dr_shift20(0), 0x04524u);
}
