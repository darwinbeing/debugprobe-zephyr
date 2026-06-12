/* Faithful port of debugprobe src/autobaud.c (lines 91-263) into a pure-C++,
 * heap-free class. The algorithm, constants, and control flow mirror the
 * original; only the storage (fixed open-addressing table instead of a
 * malloc'd HashTable) and the I/O shape (one sample in, a Result out instead
 * of a DMA-buffer scan pushing to a FreeRTOS queue) differ. Line references
 * below point at the original autobaud.c.
 */
#include "autobaud/estimator.hpp"

#include <cmath>

namespace autobaud {

namespace {
/* autobaud.c:22 -- minimum sample occurrence ratio to treat a value as
 * signal rather than noise. */
constexpr float kMinFrequency = 0.05f;

/* autobaud.c:91-96 -- integer hash (two rounds of multiply/xor), reduced
 * modulo the table size. */
uint32_t hash(uint32_t x, size_t size)
{
	x = ((x >> 16) ^ x) * 0x45d9f3bu;
	x = ((x >> 16) ^ x) * 0x45d9f3bu;
	x = (x >> 16) ^ x;
	return x % size;
}

/* autobaud.c:195-203 -- compare rounded integer parts of baud rates with a
 * 0.5% tolerance between detected and the previously reported value. */
bool baud_changed(float new_baud, float baud)
{
	uint32_t hi = (uint32_t)(baud * 1.005f);
	uint32_t lo = (uint32_t)(baud * 0.995f);
	uint32_t nb = (uint32_t)new_baud;

	return (nb > hi || nb < lo);
}
} // namespace

Estimator::Estimator(uint32_t pio_clock_hz) : pio_clock_(pio_clock_hz)
{
	reset();
}

void Estimator::reset()
{
	/* autobaud.c:300-308 -- reset of the accumulators between runs. */
	for (auto &e : table_) {
		e.key = 0;
		e.count = 0;
	}
	total_samples_ = 0;
	min_cycles_ = UINT32_MAX;
	max_cycles_ = 0;
	bit_time_sum_ = 0;
	bit_time_count_ = 0;
	outlier_count_ = 0;
	baud_ = 0.0f;
}

/* autobaud.c:110-132 -- open-addressing insert + count. Key 0 marks an empty
 * slot (the original calloc'd table relies on the same convention); a real
 * cycle count is (UINT32_MAX - raw)*2, always even and non-zero in practice.
 * Returns the occurrence count of `cycles` after this insertion. */
uint32_t Estimator::insert_and_count(uint32_t cycles)
{
	uint32_t idx = hash(cycles, kTableSize);
	while (table_[idx].key != 0) {
		if (table_[idx].key == cycles) {
			table_[idx].count++;
			return table_[idx].count;
		}
		idx = (idx + 1) % kTableSize;
	}
	table_[idx].key = cycles;
	table_[idx].count = 1;
	return 1;
}

/* Port of estimate_baud_rate()'s per-sample body, autobaud.c:216-260. */
Result Estimator::add_sample(uint32_t raw)
{
	Result result{};
	result.baud = (uint32_t)roundf(baud_);
	result.validity = 0.0f;
	result.valid = false;

	/* autobaud.c:217-218 */
	uint32_t curr_cycles_count = (UINT32_MAX - raw) * 2;
	uint32_t count = insert_and_count(curr_cycles_count);

	/* autobaud.c:221-223 */
	total_samples_++;
	if (curr_cycles_count > max_cycles_)
		max_cycles_ = curr_cycles_count;

	/* autobaud.c:224-227 -- ignore values seen below the noise floor. */
	float freq = (float)count / (float)total_samples_;
	if (freq < kMinFrequency)
		return result;

	/* autobaud.c:228-234 -- a new minimum resets the bit-time accumulators. */
	if (curr_cycles_count < min_cycles_) {
		min_cycles_ = curr_cycles_count;
		bit_time_sum_ = 0;
		bit_time_count_ = 0;
		outlier_count_ = 0;
		return result;
	}

	/* autobaud.c:235-236 -- within +10% of min: treat as a "1-bit period". */
	if ((curr_cycles_count - min_cycles_) < ((float)min_cycles_ * 0.1f)) {
		/* autobaud.c:237-241 */
		bit_time_sum_ += curr_cycles_count;
		bit_time_count_++;
		/* 1-bit period should not be < 1/9th of the longest period. */
		if (curr_cycles_count < (max_cycles_ / 9))
			outlier_count_++;

		/* autobaud.c:242-244 -- baud from the average 1-bit time. */
		float avg_bit_time = (float)bit_time_sum_ / (float)bit_time_count_;
		float new_baud = (float)pio_clock_ / avg_bit_time;

		/* autobaud.c:246-258 */
		if (baud_changed(new_baud, baud_)) {
			float completeness = 1.0f - expf(-(float)total_samples_ / 40.0f);
			float noise_ratio = (float)outlier_count_ / (float)bit_time_count_;
			float consistency = 1.0f - fminf(noise_ratio * 2.0f, 1.0f);
			float validity = completeness * consistency;

			result.validity = validity;
			if (validity > 0.6f) {
				baud_ = new_baud;
				result.baud = (uint32_t)roundf(baud_);
				result.valid = true;
			}
		}
	}

	return result;
}

} // namespace autobaud
