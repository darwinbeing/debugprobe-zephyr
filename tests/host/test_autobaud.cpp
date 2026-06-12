#include "testing.hpp"
#include "autobaud/estimator.hpp"

#include <cstdint>

namespace {
/* raw such that (UINT32_MAX - raw)*2 == cycles */
uint32_t raw_for_cycles(uint32_t cycles)
{
	return UINT32_MAX - (cycles / 2);
}
/* PIO cycles for one bit at `baud` (PIO clock 125 MHz). */
uint32_t bit_cycles(uint32_t baud) { return 125000000u / baud; }
} // namespace

/* The estimator reports a *valid* result only on the sample where the baud
 * estimate first changes meaningfully (mirroring autobaud.c's
 * xQueueOverwrite, which fires on a change). Once converged, later samples
 * at the same bit-time leave baud unchanged, so `valid` is false again. The
 * consumer therefore latches the most recent valid estimate -- so do the
 * tests, capturing the latched result rather than the last per-call flag.
 * (Test-harness capture only; the synthesized input is unchanged.) */
TEST(autobaud_converges_to_115200)
{
	autobaud::Estimator est(125000000u);
	uint32_t bc = bit_cycles(115200); /* ~1085 */
	autobaud::Result latched{};
	for (int i = 0; i < 200; i++) {
		/* mostly 1-bit periods, a few 2-3 bit runs (longer lows) */
		uint32_t cyc = (i % 7 == 0) ? bc * 2 : bc;
		if (i % 13 == 0) cyc = bc * 3;
		autobaud::Result r = est.add_sample(raw_for_cycles(cyc));
		if (r.valid) latched = r;
	}
	CHECK(latched.valid);
	/* within 0.5% of 115200 */
	CHECK(latched.baud > 114624 && latched.baud < 115776);
}

TEST(autobaud_converges_to_9600)
{
	autobaud::Estimator est(125000000u);
	uint32_t bc = bit_cycles(9600);
	autobaud::Result latched{};
	for (int i = 0; i < 300; i++) {
		uint32_t cyc = (i % 5 == 0) ? bc * 2 : bc;
		autobaud::Result r = est.add_sample(raw_for_cycles(cyc));
		if (r.valid) latched = r;
	}
	CHECK(latched.valid);
	CHECK(latched.baud > 9552 && latched.baud < 9648);
}

TEST(autobaud_noise_only_is_invalid)
{
	autobaud::Estimator est(125000000u);
	bool ever_valid = false;
	/* random-ish scattered periods, no dominant bit-time */
	uint32_t periods[] = {500, 1700, 30, 9000, 220, 4400, 77, 12000};
	for (int i = 0; i < 50; i++) {
		autobaud::Result r = est.add_sample(raw_for_cycles(periods[i % 8] + i));
		if (r.valid) ever_valid = true;
	}
	CHECK(!ever_valid);
}
