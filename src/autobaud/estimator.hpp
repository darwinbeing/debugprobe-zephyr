#pragma once
/* Baud-rate estimator: consumes raw PIO frequency-counter samples and
 * estimates the target's UART baud. Pure C++ (no Zephyr, no heap), a port
 * of the original debugprobe autobaud.c histogram/bit-time/validity logic.
 */
#include <cstddef>
#include <cstdint>

namespace autobaud {

struct Result {
	uint32_t baud = 0;
	float validity = 0.0f;
	bool valid = false; /* validity > 0.6 and baud changed meaningfully */
};

class Estimator {
public:
	explicit Estimator(uint32_t pio_clock_hz);

	/* Feed one raw PIO sample. Returns the current best estimate; the
	 * result is `valid` only when confidence crosses the threshold. */
	Result add_sample(uint32_t raw);

	void reset();

private:
	static constexpr size_t kTableSize = 509; /* prime, open addressing */
	struct Entry { uint32_t key; uint32_t count; };

	uint32_t insert_and_count(uint32_t cycles); /* hash table */

	uint32_t pio_clock_;
	Entry table_[kTableSize];
	uint32_t total_samples_;
	uint32_t min_cycles_;
	uint32_t max_cycles_;
	uint32_t bit_time_sum_;
	uint32_t bit_time_count_;
	uint32_t outlier_count_;
	float baud_;
};

} // namespace autobaud
