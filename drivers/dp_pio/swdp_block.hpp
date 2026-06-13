#pragma once
/*
 * swdp_block — hardware-independent PIO command-word stream builder and
 * RX response parser for SWD register read blocks.
 *
 * Used by the DMA-based SWD driver to prepare the TX FIFO word stream for a
 * read block, and to decode the corresponding RX words, without any Zephyr or
 * hardware dependencies.
 *
 * SPDX-License-Identifier: MIT
 */
#include <cstdint>
#include <cstddef>

namespace probe::swdp_block {

struct StreamCfg {
	uint16_t cmd_write;   /* cmd_addr for write op */
	uint16_t cmd_read;    /* cmd_addr for read op */
	uint16_t cmd_turn;    /* cmd_addr for turnaround op */
	uint8_t  turnaround;  /* t (>=1) */
};

/* build_read_stream — build the TX word stream for a register read block.
 *
 * request_byte : CMSIS request byte (RnW set) for all but the last AP transfer.
 * rdbuff_byte  : CMSIS request byte used for the final AP transfer (RDBUFF).
 * is_ap        : true for AP read (inserts a post transfer; last uses rdbuff).
 * count        : number of data words to read.
 * out          : destination buffer (cap = out_cap words).
 *
 * Returns the number of words written, or 0 if the buffer is too small.
 */
size_t build_read_stream(const StreamCfg& c, uint8_t request_byte,
			 uint8_t rdbuff_byte, bool is_ap, uint16_t count,
			 uint32_t* out, size_t out_cap);

/* read_rx_count — number of RX words the stream produced by build_read_stream
 * will generate (3 per transfer, with 1 extra post transfer for AP reads).
 */
size_t read_rx_count(bool is_ap, uint16_t count);

struct ReadParse {
	uint16_t completed; /* number of successfully parsed data words */
	uint8_t  ack;       /* ACK of the first bad transfer, or OK(1) if all good */
};

/* parse_read_response — parse the captured RX words for a read block.
 *
 * Writes up to `count` 32-bit data words little-endian into out_le.
 * Stops at the first transfer whose ACK != OK(1) or whose parity is wrong;
 * completed reflects how many data words were verified before the stop.
 */
ReadParse parse_read_response(const StreamCfg& c, bool is_ap, uint16_t count,
			      const uint32_t* rx, size_t rx_len,
			      uint8_t* out_le, size_t out_cap);

} /* namespace probe::swdp_block */
