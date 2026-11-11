#pragma once
/* Scripted msp430::Tap. Logs every call as a short string and answers DR
 * shifts according to the instruction that is currently loaded. */
#include <cstdint>
#include <cstdio>
#include <deque>
#include <string>
#include <utility>
#include <vector>

#include "msp430/protocol.hpp"
#include "msp430/tap.hpp"

class FakeTap : public msp430::Tap {
public:
	/* --- scripting --- */
	bool open_ok = true;
	uint8_t jtag_id = 0x91;       /* returned by every ir_shift */
	uint16_t cntrl_sig = 0x0301;  /* DR under IR_CNTRL_SIG_CAPTURE */
	std::deque<uint16_t> cntrl_sig_script; /* consumed first, one per read */
	uint16_t core_id = 0x0106;    /* DR under IR_COREIP_ID */
	uint32_t device_id_ptr = 0x1A00; /* DR under IR_DEVICE_ID */
	std::deque<uint16_t> quick_words; /* DR under IR_DATA_QUICK, default 0xFFFF */
	std::deque<uint16_t> jmb_words;   /* DR under IR_JMB_EXCHANGE */
	uint16_t jmb_default = msp430::kJmbIn0Rdy; /* input ready, no output */
	uint32_t millis_now = 0;
	uint32_t millis_step = 1;

	/* --- recordings --- */
	std::vector<std::string> log;
	std::vector<std::pair<uint32_t, uint16_t>> mem_writes; /* (address, data) */

	/* Queue the four mailbox reads that deliver a 32-bit value. */
	void push_jmb_out(uint32_t value)
	{
		jmb_words.push_back(msp430::kJmbOut1Rdy);
		jmb_words.push_back(0);
		jmb_words.push_back(static_cast<uint16_t>(value));
		jmb_words.push_back(static_cast<uint16_t>(value >> 16));
	}

	void set_speed(uint32_t hz) override { log.push_back("speed:" + std::to_string(hz)); }
	bool open() override
	{
		log.push_back("open");
		return open_ok;
	}
	void enter(msp430::Entry e) override
	{
		log.push_back(e == msp430::Entry::RstHigh ? "enter:high" : "enter:low");
	}
	void close() override { log.push_back("close"); }

	uint8_t ir_shift(uint8_t ir) override
	{
		ir_ = ir;
		log.push_back(fmt("ir:%02x", ir));
		return jtag_id;
	}

	uint16_t dr_shift16(uint16_t value) override
	{
		log.push_back(fmt("dr16:%04x", value));
		switch (ir_) {
		case msp430::kIrCntrlSigCapture:
			return pop(cntrl_sig_script, cntrl_sig);
		case msp430::kIrCoreipId:
			return core_id;
		case msp430::kIrDataQuick:
			return pop(quick_words, 0xFFFF);
		case msp430::kIrJmbExchange:
			return pop(jmb_words, jmb_default);
		case msp430::kIrDataToAddr:
			mem_writes.push_back({addr_, value});
			return 0;
		default:
			return 0;
		}
	}

	uint32_t dr_shift20(uint32_t value) override
	{
		log.push_back(fmt("dr20:%05x", value));
		if (ir_ == msp430::kIrAddr16Bit) {
			addr_ = value;
		}
		return ir_ == msp430::kIrDeviceId ? device_id_ptr : 0;
	}

	void set_tclk() override { log.push_back("tclk:1"); }
	void clr_tclk() override { log.push_back("tclk:0"); }
	void delay_ms(uint32_t ms) override { log.push_back("delay:" + std::to_string(ms)); }
	uint32_t millis() override
	{
		millis_now += millis_step;
		return millis_now;
	}

private:
	static std::string fmt(const char *f, unsigned v)
	{
		char buf[32];
		std::snprintf(buf, sizeof(buf), f, v);
		return buf;
	}
	static uint16_t pop(std::deque<uint16_t> &q, uint16_t fallback)
	{
		if (q.empty()) {
			return fallback;
		}
		const uint16_t v = q.front();
		q.pop_front();
		return v;
	}

	uint8_t ir_ = 0;
	uint32_t addr_ = 0;
};
