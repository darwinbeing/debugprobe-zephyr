/*
 * TX word layout of the SBW PIO program (drivers/sbw_pio/sbw.pio).
 * Pure C with no dependencies so it can be unit-tested on the host.
 */
#ifndef SBW_PACK_H_
#define SBW_PACK_H_

#include <stdint.h>

/* Cycles carried by one TX word: 4 count bits + 9 * 3 cycle bits = 31. */
#define SBW_CYCLES_PER_WORD 9U
/* Cycles per driver call: 4 words fill the TX FIFO, 4 results the RX FIFO. */
#define SBW_MAX_CYCLES 32U

/* Pack cycles [first, first + n) of the per-cycle bit vectors into one word.
 * Preconditions: n must be 1..SBW_CYCLES_PER_WORD and first + n must not
 * exceed 32. Outside that range the result is undefined (n = 0 would even
 * encode 16 cycles). */
static inline uint32_t sbw_pack_word(uint32_t tms, uint32_t mid, uint32_t tdi,
				     unsigned int first, unsigned int n)
{
	uint32_t word = (n - 1U) & 0xFU;

	for (unsigned int i = 0; i < n; i++) {
		const unsigned int c = first + i;
		const uint32_t bits = ((tms >> c) & 1U) | (((mid >> c) & 1U) << 1) |
				      (((tdi >> c) & 1U) << 2);

		word |= bits << (4U + 3U * i);
	}
	return word;
}

#endif /* SBW_PACK_H_ */
