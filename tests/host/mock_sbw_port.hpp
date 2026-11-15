#pragma once
/* Recording mock of msp430::SbwPort, plus a JTAG TAP state decoder. */
#include <cstdint>
#include <cstdio>
#include <deque>
#include <string>
#include <vector>

#include "msp430/sbw_port.hpp"

class MockSbwPort : public msp430::SbwPort {
public:
	enum TapState {
		TLR, RTI, SelDR, CapDR, ShDR, Ex1DR, PauDR, Ex2DR, UpdDR,
		SelIR, CapIR, ShIR, Ex1IR, PauIR, Ex2IR, UpdIR,
	};

	struct CycleCall {
		uint32_t tms, tdi;
		unsigned count;
	};
	struct Scan {
		bool ir;
		unsigned nbits;
		uint32_t value; /* first shifted bit is the MSB */
		bool operator==(const Scan &o) const
		{
			return ir == o.ir && nbits == o.nbits && value == o.value;
		}
	};

	/* --- scripting --- */
	bool acquire_ok = true;
	std::deque<uint32_t> tdo_script; /* one entry per cycles() call; default 0 */
	uint32_t millis_now = 0;
	uint32_t millis_step = 1; /* added on every millis() call */

	/* --- recordings --- */
	std::vector<std::string> log; /* every call, in order */
	std::vector<CycleCall> cycle_calls;
	std::vector<Scan> scans;      /* decoded IR/DR scans */
	TapState state = TLR;
	bool tclk_outside_rti = false; /* set if tclk_cycle ran outside Run-Test/Idle */
	uint32_t clock_hz = 0;
	bool bad_count = false;   /* cycles() called with count outside 1..32 */
	bool tclk = true;         /* TCLK level as the target sees it */
	bool tclk_glitch = false; /* TCLK changed while leaving or re-entering Run-Test/Idle */

	bool acquire() override
	{
		log.push_back("acquire");
		return acquire_ok;
	}
	void release() override { log.push_back("release"); }
	void set_clock(uint32_t hz) override
	{
		clock_hz = hz;
		log.push_back("clock:" + std::to_string(hz));
	}
	void set_tck(bool level) override
	{
		log.push_back(level ? "tck:1" : "tck:0");
		if (!level) {
			/* TEST low resets the target's test logic, so the TAP is
			 * in Test-Logic-Reset. */
			state = TLR;
		}
	}
	void set_tdio(bool level) override { log.push_back(level ? "tdio:1" : "tdio:0"); }
	void tck_pulse() override { log.push_back("pulse"); }

	uint32_t cycles(uint32_t tms, uint32_t tdi, unsigned count) override
	{
		char buf[48];
		std::snprintf(buf, sizeof(buf), "cycles:%x:%x:%u", (unsigned)tms,
			      (unsigned)tdi, count);
		log.push_back(buf);
		cycle_calls.push_back({tms, tdi, count});
		if (count == 0 || count > 32) {
			bad_count = true;
		}
		for (unsigned i = 0; i < count; ++i) {
			step((tms >> i) & 1u, (tdi >> i) & 1u);
		}
		uint32_t tdo = 0;
		if (!tdo_script.empty()) {
			tdo = tdo_script.front();
			tdo_script.pop_front();
		}
		return tdo;
	}

	void tclk_cycle(bool was_high, bool level) override
	{
		log.push_back(std::string("tclk:") + (was_high ? "1" : "0") + ">" +
			      (level ? "1" : "0"));
		if (state != RTI) {
			tclk_outside_rti = true;
		}
		step(false, level);
	}

	void delay_us(uint32_t us) override { log.push_back("delay:" + std::to_string(us)); }
	uint32_t millis() override
	{
		millis_now += millis_step;
		return millis_now;
	}

private:
	static TapState next(TapState s, bool tms)
	{
		switch (s) {
		case TLR:   return tms ? TLR : RTI;
		case RTI:   return tms ? SelDR : RTI;
		case SelDR: return tms ? SelIR : CapDR;
		case CapDR: return tms ? Ex1DR : ShDR;
		case ShDR:  return tms ? Ex1DR : ShDR;
		case Ex1DR: return tms ? UpdDR : PauDR;
		case PauDR: return tms ? Ex2DR : PauDR;
		case Ex2DR: return tms ? UpdDR : ShDR;
		case UpdDR: return tms ? SelDR : RTI;
		case SelIR: return tms ? TLR : CapIR;
		case CapIR: return tms ? Ex1IR : ShIR;
		case ShIR:  return tms ? Ex1IR : ShIR;
		case Ex1IR: return tms ? UpdIR : PauIR;
		case PauIR: return tms ? Ex2IR : PauIR;
		case Ex2IR: return tms ? UpdIR : ShIR;
		case UpdIR: return tms ? SelDR : RTI;
		}
		return TLR;
	}

	void step(bool tms, bool tdi)
	{
		const TapState before = state;
		if (state == ShDR || state == ShIR) {
			shift_value_ = (shift_value_ << 1) | (tdi ? 1u : 0u);
			++shift_bits_;
		}
		state = next(state, tms);
		if (before == RTI && state != RTI && tdi != tclk) {
			tclk_glitch = true; /* left Run-Test/Idle with TDI != TCLK */
		}
		if (state == RTI) {
			if ((before == UpdDR || before == UpdIR) && tdi != tclk) {
				tclk_glitch = true; /* came back from a scan with TDI != TCLK */
			}
			tclk = tdi; /* in Run-Test/Idle the TDI slot is TCLK */
		}
		if (state == UpdDR || state == UpdIR) {
			scans.push_back({state == UpdIR, shift_bits_, shift_value_});
			shift_value_ = 0;
			shift_bits_ = 0;
		}
	}

	uint32_t shift_value_ = 0;
	unsigned shift_bits_ = 0;
};
