/*
 * Host tests for drivers/dp_pio/swdp_block — pure read-block PIO stream
 * builder + response parser.
 *
 * SPDX-License-Identifier: MIT
 */
#include "testing.hpp"
#include "swdp_block.hpp"

#include <vector>
#include <cstring>

namespace {

/* Known StreamCfg used throughout the tests */
constexpr probe::swdp_block::StreamCfg kCfg{
	.cmd_write  = 10,
	.cmd_read   = 13,
	.cmd_turn   = 12,
	.turnaround = 1,
};

/* fmt() — mirrors fmt_probe_command in swdp_pio.c */
static uint32_t fmt(uint32_t bit_count, bool out_en, uint16_t cmd_addr)
{
	return ((bit_count - 1) & 0xff)
	     | ((out_en ? 1u : 0u) << 8)
	     | (static_cast<uint32_t>(cmd_addr) << 9);
}

/* prq — mirrors the request-packet build from sw_transfer (swdp_pio.c:249-258) */
static uint32_t prq(uint8_t request)
{
	uint8_t p = 0;
	uint8_t v = 1; /* start bit */
	for (int n = 1; n < 5; ++n) {
		uint8_t bit = (request >> (n - 1)) & 1u;
		v |= static_cast<uint8_t>(bit << n);
		p += bit;
	}
	v |= static_cast<uint8_t>((p & 1u) << 5); /* parity */
	/* stop bit 6 = 0, park bit 7 = 1 */
	v |= (1u << 7);
	return v;
}

/* --- RX word construction helpers --- */

/* mk_ack: MSB-align a (t+3)-bit value in a 32-bit word, where the ACK
 * occupies bits [2:0] after shifting right by t turnaround bits.
 * The SM shifts in from MSB; probe_read_bits does >> (32 - bit_count).
 * So the captured bits are left-aligned: bit 31 is the first bit received.
 * Layout received (LSB-first SWD): t turnaround bits, then ACK[0], ACK[1], ACK[2].
 * In the RX word (MSB = first received), bits [31 .. 31-(t+3)+1] hold the data.
 * After >> (32-(t+3)): bit [t+2..0]; then >> t gives ACK in bits[2:0].
 */
static uint32_t mk_ack(uint8_t t, uint8_t ackval)
{
	/* (t+3) bits captured, MSB-aligned.
	 * Simpler derivation:
	 *   raw_shifted = raw >> (32-(t+3))   → t+3 bits right-aligned
	 *   ack = (raw_shifted >> t) & 7
	 *   ⟹ raw_shifted = ackval << t  (turnaround bits = 0)
	 *   ⟹ raw = raw_shifted << (32-(t+3)) = (ackval << t) << (32-(t+3))
	 *          = ackval << (32 - 3)   = ackval << 29
	 * Note: t cancels out, so the result is independent of t.
	 */
	(void)t;
	return (static_cast<uint32_t>(ackval) << 29u);
}

/* mk_data: 32-bit read — bit_count=32, no shift needed.  data = rx_data. */
static uint32_t mk_data(uint32_t word) { return word; }

/* mk_par: 1-bit read, MSB-aligned.  par = (rx_par >> 31) & 1. */
static uint32_t mk_par(uint8_t parbit)
{
	return (static_cast<uint32_t>(parbit & 1u) << 31u);
}

/* Build a correct 3-RX-word tuple for a successful read of `word`. */
static std::vector<uint32_t> good_transfer(uint8_t t, uint32_t word)
{
	uint8_t par = static_cast<uint8_t>(__builtin_popcount(word) & 1u);
	return { mk_ack(t, 1), mk_data(word), mk_par(par) };
}

} // namespace

/* ---- build_read_stream / read_rx_count basic shapes ---- */

TEST(dp_read_1_word_count)
{
	uint32_t buf[32];
	size_t n = probe::swdp_block::build_read_stream(
		kCfg, 0x06, 0x0E, /*is_ap=*/false, 1, buf, 32);
	CHECK_EQ(n, 7u);
}

TEST(dp_read_rx_count_1)
{
	CHECK_EQ(probe::swdp_block::read_rx_count(false, 1), 3u);
}

TEST(ap_read_2_word_count)
{
	uint32_t buf[64];
	size_t n = probe::swdp_block::build_read_stream(
		kCfg, 0x07, 0x0E, /*is_ap=*/true, 2, buf, 64);
	/* 3 transfers (post + 2) × 7 words = 21 */
	CHECK_EQ(n, 21u);
}

TEST(ap_read_rx_count_2)
{
	/* 3 transfers × 3 RX words = 9 */
	CHECK_EQ(probe::swdp_block::read_rx_count(true, 2), 9u);
}

/* ---- Command-word encoding for first transfer ---- */

/* The first transfer (DP or AP post) emits:
 *   [0] write-cmd (8, out_en=1, cmd_write)
 *   [1] prq(request_byte)
 *   [2] read-cmd  (t+3, out_en=0, cmd_read)      ← ACK capture
 *   [3] read-cmd  (32,  out_en=0, cmd_read)       ← data
 *   [4] read-cmd  (1,   out_en=0, cmd_read)       ← parity
 *   [5] turn-cmd  (t,   out_en=0, cmd_turn)       ← trailing turnaround
 *   [6] 0                                         ← turnaround data word
 */
TEST(dp_first_transfer_cmd_words)
{
	constexpr uint8_t REQ = 0x06; /* DP read CTRL/STAT */
	uint32_t buf[32] = {};
	probe::swdp_block::build_read_stream(
		kCfg, REQ, 0x0E, false, 1, buf, 32);

	uint8_t t = kCfg.turnaround;
	CHECK_EQ(buf[0], fmt(8,     true,  kCfg.cmd_write));
	CHECK_EQ(buf[1], static_cast<uint32_t>(prq(REQ)));
	CHECK_EQ(buf[2], fmt(t + 3, false, kCfg.cmd_read));
	CHECK_EQ(buf[3], fmt(32,    false, kCfg.cmd_read));
	CHECK_EQ(buf[4], fmt(1,     false, kCfg.cmd_read));
	CHECK_EQ(buf[5], fmt(t,     false, kCfg.cmd_turn));
	CHECK_EQ(buf[6], 0u);
}

/* ---- AP: last transfer uses rdbuff_byte, others use request_byte ---- */

TEST(ap_rdbuff_on_last_transfer)
{
	constexpr uint8_t REQ   = 0x07; /* AP read */
	constexpr uint8_t RDBU  = 0x0E; /* RDBUFF */
	uint32_t buf[64] = {};
	/* count=2 → 3 transfers: [post][transfer1][transfer2=rdbuff] */
	probe::swdp_block::build_read_stream(
		kCfg, REQ, RDBU, true, 2, buf, 64);

	/* Transfer 0 (post): word 1 = prq(REQ) */
	CHECK_EQ(buf[1], static_cast<uint32_t>(prq(REQ)));
	/* Transfer 1: word 8 = prq(REQ) */
	CHECK_EQ(buf[8], static_cast<uint32_t>(prq(REQ)));
	/* Transfer 2 (last): word 15 = prq(RDBU) */
	CHECK_EQ(buf[15], static_cast<uint32_t>(prq(RDBU)));
}

/* ---- parse_read_response: DP count=2 all-OK ---- */

TEST(dp_parse_2_ok)
{
	constexpr uint8_t T = kCfg.turnaround;
	constexpr uint32_t W0 = 0xDEADBEEFu;
	constexpr uint32_t W1 = 0x12345678u;

	/* Build synthetic RX: 3 words per transfer */
	std::vector<uint32_t> rx;
	auto t0 = good_transfer(T, W0);
	auto t1 = good_transfer(T, W1);
	rx.insert(rx.end(), t0.begin(), t0.end());
	rx.insert(rx.end(), t1.begin(), t1.end());

	uint8_t out[8] = {};
	auto res = probe::swdp_block::parse_read_response(
		kCfg, false, 2, rx.data(), rx.size(), out, sizeof(out));

	CHECK_EQ(res.completed, 2u);
	CHECK_EQ(res.ack, 1u); /* OK */

	/* Little-endian layout */
	uint32_t got0, got1;
	memcpy(&got0, out,     4);
	memcpy(&got1, out + 4, 4);
	CHECK_EQ(got0, W0);
	CHECK_EQ(got1, W1);
}

/* ---- parse_read_response: AP count=2 (post discarded) ---- */

TEST(ap_parse_2_post_discarded)
{
	constexpr uint8_t T = kCfg.turnaround;
	constexpr uint32_t W0 = 0xAAAA0001u;
	constexpr uint32_t W1 = 0xBBBB0002u;

	/* post (discarded) + transfer0 (W0) + transfer1 (W1) */
	std::vector<uint32_t> rx;
	auto post = good_transfer(T, 0xDEAD0000u); /* discarded */
	auto tr0  = good_transfer(T, W0);
	auto tr1  = good_transfer(T, W1);
	rx.insert(rx.end(), post.begin(), post.end());
	rx.insert(rx.end(), tr0.begin(),  tr0.end());
	rx.insert(rx.end(), tr1.begin(),  tr1.end());

	uint8_t out[8] = {};
	auto res = probe::swdp_block::parse_read_response(
		kCfg, true, 2, rx.data(), rx.size(), out, sizeof(out));

	CHECK_EQ(res.completed, 2u);
	CHECK_EQ(res.ack, 1u);

	uint32_t got0, got1;
	memcpy(&got0, out,     4);
	memcpy(&got1, out + 4, 4);
	CHECK_EQ(got0, W0);
	CHECK_EQ(got1, W1);
}

/* ---- parse_read_response: fault at transfer 1 (ACK=WAIT=2) ---- */

TEST(dp_parse_fault_at_transfer1)
{
	constexpr uint8_t T = kCfg.turnaround;
	constexpr uint32_t W0 = 0x11112222u;

	std::vector<uint32_t> rx;
	auto tr0 = good_transfer(T, W0);
	rx.insert(rx.end(), tr0.begin(), tr0.end());
	/* Transfer 1: ACK=WAIT(2) */
	rx.push_back(mk_ack(T, 2)); /* WAIT */
	rx.push_back(mk_data(0));
	rx.push_back(mk_par(0));

	uint8_t out[8] = {};
	auto res = probe::swdp_block::parse_read_response(
		kCfg, false, 2, rx.data(), rx.size(), out, sizeof(out));

	CHECK_EQ(res.completed, 1u);
	CHECK_EQ(res.ack, 2u); /* WAIT */
}

/* ---- parse_read_response: parity error stops parsing ---- */

TEST(dp_parse_parity_error)
{
	constexpr uint8_t T = kCfg.turnaround;
	constexpr uint32_t W0 = 0xAAAAAAAAu; /* popcount=16, par=0 */

	std::vector<uint32_t> rx;
	auto tr0 = good_transfer(T, W0);
	rx.insert(rx.end(), tr0.begin(), tr0.end());
	/* Transfer 1: ACK OK but parity flipped */
	rx.push_back(mk_ack(T, 1));
	rx.push_back(mk_data(0x11111111u)); /* popcount=8, par=0 */
	rx.push_back(mk_par(1));            /* wrong parity → error */

	uint8_t out[8] = {};
	auto res = probe::swdp_block::parse_read_response(
		kCfg, false, 2, rx.data(), rx.size(), out, sizeof(out));

	/* W0 is good, transfer 1 has bad parity — stops there */
	CHECK_EQ(res.completed, 1u);
	/* A parity failure must surface as TRANSFER_ERROR (0x08), matching the
	 * CPU path (sw_transfer sets ack = DAP_TRANSFER_ERROR); reporting OK
	 * would hide data corruption from the host. */
	CHECK_EQ(res.ack, 0x08u);
}

/* ---- capacity guard: build returns 0 if buffer too small ---- */

TEST(build_returns_0_if_no_room)
{
	uint32_t buf[4];
	size_t n = probe::swdp_block::build_read_stream(
		kCfg, 0x06, 0x0E, false, 1, buf, 4); /* need 7, only 4 */
	CHECK_EQ(n, 0u);
}
