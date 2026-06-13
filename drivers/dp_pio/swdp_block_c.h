#pragma once
/* C-callable wrappers over probe::swdp_block (swdp_block.cpp), so the C PIO
 * driver can build the DMA TX command stream and parse the RX capture without
 * including the C++ header. Pure logic; no hardware. */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Build the TX command-word stream for a register READ block; see
 * probe::swdp_block::build_read_stream. Returns words written, 0 if it
 * does not fit. cmd_* are offset+probe_offset_*_cmd; t = turnaround. */
size_t swdp_block_build_read(uint16_t cmd_write, uint16_t cmd_read,
			     uint16_t cmd_turn, uint8_t t, uint8_t request_byte,
			     uint8_t rdbuff_byte, int is_ap, uint16_t count,
			     uint32_t *out, size_t out_cap);

/* RX words the above stream produces. */
size_t swdp_block_read_rx_count(int is_ap, uint16_t count);

/* Parse the RX capture into little-endian data words; see
 * probe::swdp_block::parse_read_response. Returns completed count; *ack gets
 * the final ACK (0x01 OK, else WAIT/FAULT/0x08 on parity error). */
uint16_t swdp_block_parse_read(uint8_t t, int is_ap, uint16_t count,
			       const uint32_t *rx, size_t rx_len,
			       uint8_t *out_le, size_t out_cap, uint8_t *ack);

#ifdef __cplusplus
}
#endif
