#include "testing.hpp"
#include "fake_tap.hpp"

#include <cstdint>
#include <utility>
#include <vector>

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

TEST(fake_tap_eem_read_as_replaces_what_a_register_reads)
{
	FakeTap t;
	t.eem_read_as[0x10] = 0xFFF14A2C;
	t.ir_shift(msp430::kIrEmexDataExchange32);
	t.dr_shift32(0x10); /* the write is stored as before */
	t.dr_shift32(0x00014A2C);
	t.dr_shift32(0x11);
	CHECK_EQ(t.dr_shift32(0), 0xFFF14A2Cu);
	t.dr_shift32(0x12); /* other registers read what was written */
	t.dr_shift32(0x00000005);
	t.dr_shift32(0x13);
	CHECK_EQ(t.dr_shift32(0), 0x00000005u);
	CHECK_EQ(t.eem[0x10], 0x00014A2Cu);
	CHECK_EQ(t.eem_writes.size(), 2u);
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

namespace {

/* WriteMemWordXv2 (MSPDS hil/edt.h) */
void write_word(FakeTap &t, uint32_t addr, uint16_t data)
{
	t.clr_tclk();
	t.ir_shift(msp430::kIrCntrlSig16Bit);
	t.dr_shift16(0x0500);
	t.ir_shift(msp430::kIrAddr16Bit);
	t.dr_shift20(addr);
	t.set_tclk();
	t.ir_shift(msp430::kIrDataToAddr);
	t.dr_shift16(data);
	t.clr_tclk();
	t.ir_shift(msp430::kIrCntrlSig16Bit);
	t.dr_shift16(0x0501);
	t.set_tclk();
	t.clr_tclk();
	t.set_tclk();
}

/* ReadMemWordXv2 (MSPDS hil/edt.h) */
uint16_t read_word(FakeTap &t, uint32_t addr)
{
	t.clr_tclk();
	t.ir_shift(msp430::kIrAddr16Bit);
	t.dr_shift20(addr);
	t.set_tclk();
	t.clr_tclk();
	t.ir_shift(msp430::kIrDataCapture);
	const uint16_t data = t.dr_shift16(0);
	t.set_tclk();
	t.clr_tclk();
	t.set_tclk();
	return data;
}

} // namespace

TEST(fake_tap_reads_memory_after_an_address_load)
{
	FakeTap t;
	t.memory[0x2400] = 0x1234;
	t.captured = {0xAAAA};
	CHECK_EQ(read_word(t, 0x2400), 0x1234);
	/* no address pending: the next capture is a CPU register word */
	CHECK_EQ(t.dr_shift16(0), 0xAAAA);
	CHECK_EQ(t.dr_shift16(0), 0x0000);
	CHECK(!t.mem_protocol_error);
}

TEST(fake_tap_an_instruction_between_address_and_capture_cancels_the_read)
{
	FakeTap t;
	t.memory[0x2400] = 0x1234;
	t.captured = {0xAAAA};
	t.clr_tclk();
	t.ir_shift(msp430::kIrAddr16Bit);
	t.dr_shift20(0x2400);
	t.set_tclk();
	t.clr_tclk();
	t.ir_shift(msp430::kIrCntrlSigCapture);
	t.ir_shift(msp430::kIrDataCapture);
	CHECK_EQ(t.dr_shift16(0), 0xAAAA); /* a register word, not memory */
	CHECK(!t.mem_protocol_error);
}

TEST(fake_tap_memory_writes_and_the_watchdog_password)
{
	FakeTap t;
	write_word(t, 0x015C, 0x5A80);
	CHECK_EQ(read_word(t, 0x015C), 0x6980); /* WDTCTL reads back with 0x69 */
	write_word(t, 0x015C, 0x5A88);
	CHECK_EQ(read_word(t, 0x015C), 0x6980); /* WDTCNTCL reads 0 */
	write_word(t, 0x2400, 0xBEEF);
	CHECK_EQ(read_word(t, 0x2400), 0xBEEF);
	REQUIRE(t.mem_writes.size() == 3u);
	CHECK(t.mem_writes[0] == (std::pair<uint32_t, uint16_t>{0x015C, 0x5A80}));
	CHECK(!t.password_violation);
	CHECK(!t.mem_protocol_error);
}

TEST(fake_tap_flags_a_write_without_its_password)
{
	/* the keys: 0x5A for WDTCTL, 0xA5 for PMMCTL0, FCTL1, FCTL3, FCTL4 */
	FakeTap ok;
	write_word(ok, 0x015C, 0x5A80);
	write_word(ok, 0x0120, 0xA500);
	write_word(ok, 0x0140, 0xA500);
	write_word(ok, 0x0144, 0xA518);
	write_word(ok, 0x0146, 0xA500);
	write_word(ok, 0x2400, 0x6980); /* other addresses take any value */
	CHECK(!ok.password_violation);

	/* Each written back as it reads (0x69, 0x96): a PUC on the chip, so
	 * the write is recorded but memory keeps its contents. */
	const uint32_t addrs[] = {0x015C, 0x0120, 0x0140, 0x0144, 0x0146};
	for (const uint32_t addr : addrs) {
		FakeTap t;
		const auto before = t.memory;
		write_word(t, addr, addr == 0x015C ? 0x6980 : 0x9658);
		CHECK(t.password_violation);
		CHECK(t.memory == before);
		REQUIRE(t.mem_writes.size() == 1u);
		CHECK_EQ(t.mem_writes[0].first, addr);
	}
}

TEST(fake_tap_flags_a_write_to_flash_or_the_flash_controller)
{
	/* BSL, information memory and device descriptors 0x1000-0x1BFF, main
	 * flash from 0x4400, the flash controller 0x0140-0x014F; each range at
	 * both ends */
	const uint32_t flash[] = {0x1000, 0x1BFE, 0x4400, 0x243FE, 0xFFFFE, 0x0140, 0x014E};
	for (const uint32_t addr : flash) {
		FakeTap t;
		write_word(t, addr, 0xA500);
		CHECK(t.flash_write_attempt);
	}
	/* RAM (USB RAM from 0x1C00, RAM up to 0x43FF), WDTCTL, SFRIFG1 and the
	 * words next to the ranges */
	const uint32_t other[] = {0x2400, 0x43FE, 0x1C00, 0x015C, 0x0102, 0x0FFE, 0x013E, 0x0150};
	for (const uint32_t addr : other) {
		FakeTap t;
		write_word(t, addr, 0x5A80);
		CHECK(!t.flash_write_attempt);
	}
	/* reads never set it */
	FakeTap r;
	read_word(r, 0x4400);
	read_word(r, 0x0144);
	CHECK(!r.flash_write_attempt);
}

TEST(fake_tap_flags_memory_access_out_of_step_with_tclk)
{
	FakeTap ok;
	write_word(ok, 0x2400, 0x1234);
	CHECK_EQ(read_word(ok, 0x2400), 0x1234);
	CHECK(!ok.mem_protocol_error);

	/* read: no TCLK rise after the address load */
	FakeTap a;
	a.clr_tclk();
	a.ir_shift(msp430::kIrAddr16Bit);
	a.dr_shift20(0x2400);
	a.ir_shift(msp430::kIrDataCapture);
	a.dr_shift16(0);
	CHECK(a.mem_protocol_error);

	/* read: TCLK still high at the capture */
	FakeTap b;
	b.clr_tclk();
	b.ir_shift(msp430::kIrAddr16Bit);
	b.dr_shift20(0x2400);
	b.set_tclk();
	b.ir_shift(msp430::kIrDataCapture);
	b.dr_shift16(0);
	CHECK(b.mem_protocol_error);

	/* read: the CPU is set to write */
	FakeTap c;
	c.ir_shift(msp430::kIrCntrlSig16Bit);
	c.dr_shift16(0x0500);
	read_word(c, 0x2400);
	CHECK(c.mem_protocol_error);

	/* write: TCLK low while the data is applied */
	FakeTap d;
	d.clr_tclk();
	d.ir_shift(msp430::kIrCntrlSig16Bit);
	d.dr_shift16(0x0500);
	d.ir_shift(msp430::kIrAddr16Bit);
	d.dr_shift20(0x2400);
	d.ir_shift(msp430::kIrDataToAddr);
	d.dr_shift16(0x1234);
	CHECK(d.mem_protocol_error);

	/* write: the CPU is still set to read */
	FakeTap e;
	e.clr_tclk();
	e.ir_shift(msp430::kIrAddr16Bit);
	e.dr_shift20(0x2400);
	e.set_tclk();
	e.ir_shift(msp430::kIrDataToAddr);
	e.dr_shift16(0x1234);
	CHECK(e.mem_protocol_error);
}

TEST(fake_tap_eem_control_read_and_write)
{
	FakeTap t;
	t.eem_control_script = {0x0080};
	t.ir_shift(msp430::kIrEmexReadControl);
	CHECK_EQ(t.dr_shift16(0), 0x0080);
	CHECK_EQ(t.dr_shift16(0), 0x0000);
	t.ir_shift(msp430::kIrEmexWriteControl);
	t.dr_shift16(0x0007);
	REQUIRE(t.eem_control_writes.size() == 1u);
	CHECK_EQ(t.eem_control_writes[0], 0x0007);
	/* a read returns the status, not the value written */
	t.ir_shift(msp430::kIrEmexReadControl);
	CHECK_EQ(t.dr_shift16(0), 0x0000);
}

TEST(fake_tap_captures_the_mab)
{
	FakeTap t;
	t.mab_script = {0x14A2C};
	t.ir_shift(msp430::kIrAddrCapture);
	CHECK_EQ(t.dr_shift20(0), 0x14A2Cu);
	CHECK_EQ(t.dr_shift20(0), 0x04524u);
}

TEST(fake_tap_flags_a_20_or_32_bit_shift_under_data_to_addr)
{
	/* write mode, an address loaded, TCLK high: a 16-bit shift would be a
	 * proper write */
	for (const bool wide32 : {false, true}) {
		FakeTap t;
		t.clr_tclk();
		t.ir_shift(msp430::kIrCntrlSig16Bit);
		t.dr_shift16(0x0500);
		t.ir_shift(msp430::kIrAddr16Bit);
		t.dr_shift20(0x2400);
		t.set_tclk();
		t.ir_shift(msp430::kIrDataToAddr);
		if (wide32) {
			t.dr_shift32(0x00001234);
		} else {
			t.dr_shift20(0x01234);
		}
		CHECK(t.mem_protocol_error);
		CHECK(t.mem_writes.empty());
	}
}

/* The words fed to the CPU under IR_DATA_16BIT, decoded as the instruction
 * stream the debugger feeds (FakeTap::cpu_words). */

namespace {

/* `words` under IR_DATA_16BIT with a TCLK cycle before each shift. The
 * decoder ignores TCLK, so the order does not matter here; in MSPDS the
 * word is shifted first and the clock that follows latches it. */
void feed(FakeTap &t, const std::vector<uint16_t> &words)
{
	t.ir_shift(msp430::kIrData16Bit);
	for (const uint16_t w : words) {
		t.clr_tclk();
		t.set_tclk();
		t.dr_shift16(w);
	}
}

/* a power-on reset through the control signal, as
 * SyncJtag_AssertPor_SaveContextXv2 and Cpuxv2::disconnect apply it */
void por(FakeTap &t)
{
	t.ir_shift(msp430::kIrCntrlSig16Bit);
	t.dr_shift16(0x0C01);
	t.dr_shift16(0x0401);
}

} // namespace

TEST(fake_tap_accepts_the_instructions_the_debugger_feeds)
{
	FakeTap t;
	/* WriteCpuRegXv2: MOVA #0xFFFFF, R15; JMP $-4 */
	feed(t, {0x0F8F, 0xFFFF, 0x3FFD});
	/* ReadCpuRegXv2: MOVA SR, &0x0FF6, its extension word after an IR
	 * change (the control signal 0x1401); JMP $-4 */
	feed(t, {0x0260});
	t.ir_shift(msp430::kIrCntrlSig16Bit);
	t.dr_shift16(0x1401);
	feed(t, {0x0FF6, 0x3FFD});
	/* SetPcXv2: MOVA #0x14A2C, PC; NOP. An extension word takes any value. */
	feed(t, {0x0180, 0x4A2C, 0x4303});
	feed(t, {0x0080, 0x40B2, 0x4303});
	/* SAFE_PC_ADDRESS after a POR, or an instruction there (Phase A's
	 * SetPC after its POR) */
	por(t);
	feed(t, {0x0004});
	por(t);
	feed(t, {0x0080, 0x0004, 0x4303});
	t.ir_shift(msp430::kIrCntrlSigRelease); /* nothing due */
	CHECK(!t.unexpected_instruction);
	CHECK(t.cpu_words == (std::vector<uint16_t>{0x0F8F, 0xFFFF, 0x3FFD, 0x0260, 0x0FF6, 0x3FFD,
						    0x0180, 0x4A2C, 0x4303, 0x0080, 0x40B2, 0x4303,
						    0x0004, 0x0080, 0x0004, 0x4303}));
}

TEST(fake_tap_flags_any_other_word_fed_to_the_cpu)
{
	const struct {
		bool after_por;
		std::vector<uint16_t> words;
	} cases[] = {
		{false, {0x40B2, 0xA500, 0x0144}}, /* MOV #0xA500, &FCTL3 */
		{true, {0x40B2, 0xA500, 0x0144}},  /* the same after a POR */
		{false, {0x0560, 0x0FF8}},         /* MOVA R5, &0x00FF8 */
		{false, {0x0561, 0x0FF6}},         /* MOVA R5, &0x10FF6 */
		{false, {0x0560, 0x4400}},         /* MOVA R5, &0x04400 */
		{false, {0x0004}},                 /* SAFE_PC_ADDRESS without a POR */
		{false, {0x3FFF}},                 /* JMP $ */
		{false, {0x0080, 0x0000, 0x40B2}}, /* after an extension word: an instruction */
	};
	for (const auto &c : cases) {
		FakeTap t;
		if (c.after_por) {
			por(t);
		}
		feed(t, c.words);
		CHECK(t.unexpected_instruction);
		CHECK(t.cpu_words == c.words);
	}
	/* released with an extension word due: the CPU fetches it from memory */
	for (const uint16_t mova : {0x0080, 0x0560}) {
		FakeTap t;
		feed(t, {mova});
		t.ir_shift(msp430::kIrCntrlSigRelease);
		CHECK(t.unexpected_instruction);
	}
	/* a 20- or 32-bit shift under IR_DATA_16BIT: a word the decoder cannot
	 * see */
	FakeTap a;
	a.ir_shift(msp430::kIrData16Bit);
	a.dr_shift20(0x040B2);
	CHECK(a.unexpected_instruction);
	FakeTap b;
	b.ir_shift(msp430::kIrData16Bit);
	b.dr_shift32(0x000040B2);
	CHECK(b.unexpected_instruction);
}
