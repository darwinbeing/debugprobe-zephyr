/*
 * swdp_block.cpp — PIO TX stream builder + RX response parser for SWD read
 * blocks. Zero hardware / Zephyr dependencies; compiled into both firmware
 * and host-test binaries.
 *
 * SPDX-License-Identifier: MIT
 */
#include "swdp_block.hpp"

namespace probe::swdp_block {

/* ---------------------------------------------------------------------------
 * Internal helpers
 * --------------------------------------------------------------------------*/

/* fmt — pack a PIO command word (mirrors fmt_probe_command in swdp_pio.c).
 *   bits[7:0]  = bit_count - 1
 *   bit[8]     = out_en
 *   bits[31:9] = cmd_addr
 */
static inline uint32_t fmt(uint32_t bit_count, bool out_en, uint16_t cmd_addr)
{
	return ((bit_count - 1u) & 0xffu)
	     | ((out_en ? 1u : 0u) << 8u)
	     | (static_cast<uint32_t>(cmd_addr) << 9u);
}

/* build_prq — build the 8-bit SWD request packet from a CMSIS request byte.
 * Mirrors the packet-build block in sw_transfer (swdp_pio.c:249-258).
 */
static uint32_t build_prq(uint8_t request)
{
	uint8_t prq     = 1u;    /* start bit */
	uint8_t parity  = 0u;
	for (unsigned n = 1u; n < 5u; ++n) {
		uint8_t bit = (request >> (n - 1u)) & 1u;
		prq |= static_cast<uint8_t>(bit << n);
		parity += bit;
	}
	prq |= static_cast<uint8_t>((parity & 1u) << 5u); /* parity bit */
	/* stop bit 6 = 0, park bit 7 = 1 */
	prq |= static_cast<uint8_t>(1u << 7u);
	return prq;
}

/* emit_transfer — write the 7 TX words for one read transfer into out[].
 * Caller must guarantee at least 7 words of capacity.
 */
static void emit_transfer(const StreamCfg& c, uint8_t req_byte, uint32_t* out)
{
	uint8_t t = c.turnaround;
	out[0] = fmt(8u,       true,  c.cmd_write);  /* write-cmd, 8 bits */
	out[1] = build_prq(req_byte);                /* request packet */
	out[2] = fmt(t + 3u,  false, c.cmd_read);   /* ACK capture */
	out[3] = fmt(32u,     false, c.cmd_read);   /* data capture */
	out[4] = fmt(1u,      false, c.cmd_read);   /* parity capture */
	out[5] = fmt(t,       false, c.cmd_turn);   /* trailing turnaround */
	out[6] = 0u;                                  /* turnaround data word */
}

/* ---------------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------------*/

size_t read_rx_count(bool is_ap, uint16_t count)
{
	/* Each transfer produces 3 RX words (ACK, data, parity).
	 * AP reads inject one extra post transfer at the start. */
	uint32_t transfers = static_cast<uint32_t>(count) + (is_ap ? 1u : 0u);
	return transfers * 3u;
}

size_t build_read_stream(const StreamCfg& c, uint8_t request_byte,
			 uint8_t rdbuff_byte, bool is_ap, uint16_t count,
			 uint32_t* out, size_t out_cap)
{
	/* Total transfers = count (DP) or count+1 (AP: one leading post). */
	size_t total = static_cast<size_t>(count) + (is_ap ? 1u : 0u);
	size_t words = total * 7u; /* 7 TX words per transfer */

	if (words > out_cap) {
		return 0u;
	}

	if (is_ap) {
		/* Post transfer: uses request_byte; its RX words are discarded. */
		emit_transfer(c, request_byte, out);
		out += 7u;

		/* Subsequent transfers: all use request_byte except the very last,
		 * which uses rdbuff_byte to flush the AP pipeline. */
		for (uint16_t i = 0u; i < count; ++i) {
			uint8_t req = (i == count - 1u) ? rdbuff_byte : request_byte;
			emit_transfer(c, req, out);
			out += 7u;
		}
	} else {
		/* DP: every transfer uses the same request_byte. */
		for (uint16_t i = 0u; i < count; ++i) {
			emit_transfer(c, request_byte, out);
			out += 7u;
		}
	}

	return words;
}

ReadParse parse_read_response(const StreamCfg& c, bool is_ap, uint16_t count,
			      const uint32_t* rx, size_t rx_len,
			      uint8_t* out_le, size_t out_cap)
{
	ReadParse result{0u, 1u}; /* ack starts OK */
	uint8_t t = c.turnaround;

	/* AP reads: the first 3 RX words belong to the post transfer and are
	 * discarded.  The data words come from RX indices 3..3*(count+1)-1. */
	size_t rx_offset = is_ap ? 3u : 0u;

	for (uint16_t i = 0u; i < count; ++i) {
		size_t base = rx_offset + static_cast<size_t>(i) * 3u;
		if (base + 3u > rx_len) {
			/* Truncated RX — stop */
			break;
		}

		/* ACK: (t+3)-bit capture, MSB-aligned.
		 * Right-align the t+3 bits, then drop the t turnaround bits. */
		uint32_t raw_ack  = rx[base + 0u];
		uint32_t ack_word = raw_ack >> (32u - (t + 3u));
		uint8_t  ack      = static_cast<uint8_t>((ack_word >> t) & 0x7u);

		if (ack != 1u) { /* not DAP_TRANSFER_OK */
			result.ack = ack;
			break;
		}

		/* Data: 32-bit capture, no shift needed. */
		uint32_t data = rx[base + 1u];

		/* Parity: 1-bit capture, MSB-aligned. */
		uint8_t par_got  = static_cast<uint8_t>((rx[base + 2u] >> 31u) & 1u);
		uint8_t par_calc = static_cast<uint8_t>(__builtin_popcount(data) & 1u);

		if (par_got != par_calc) {
			/* Parity error: report DAP_TRANSFER_ERROR (BIT(3)) like the
			 * CPU path (sw_transfer), and stop without storing. */
			result.ack = 0x08u;
			break;
		}

		/* Capacity guard for the output buffer. */
		size_t out_off = static_cast<size_t>(result.completed) * 4u;
		if (out_off + 4u > out_cap) {
			break;
		}

		/* Store little-endian. */
		out_le[out_off + 0u] = static_cast<uint8_t>(data);
		out_le[out_off + 1u] = static_cast<uint8_t>(data >>  8u);
		out_le[out_off + 2u] = static_cast<uint8_t>(data >> 16u);
		out_le[out_off + 3u] = static_cast<uint8_t>(data >> 24u);

		++result.completed;
	}

	return result;
}

} /* namespace probe::swdp_block */
