#pragma once
/* Scripted msp430::Tap. Logs every call as a short string and answers DR
 * shifts according to the instruction that is currently loaded. */
#include <cstdint>
#include <cstdio>
#include <deque>
#include <map>
#include <string>
#include <type_traits>
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
	/* The device-descriptor pointer the device holds. Under IR_DEVICE_ID a
	 * real device shifts it out in plain order, and Tap::dr_shift20 applies
	 * TI's 20-bit de-scramble to every result, so the caller sees it
	 * scrambled (measured on an MSP430F5529: 0x001A0 for 0x01A00). */
	uint32_t device_id_ptr = 0x1A00;
	std::deque<uint16_t> quick_words; /* DR under IR_DATA_QUICK, default 0xFFFF */
	std::deque<uint8_t> jtag_id_script;   /* consumed first, one per ir_shift */
	bool jmb_in_ready = true;             /* IN0RDY in the mailbox status */
	uint32_t jmb_hidden_polls = 0;  /* this many mailbox status reads report "nothing ready" first */
	uint32_t millis_per_shift = 0;  /* added to the clock by every dr_shift16 (a slow poll) */
	std::deque<uint32_t> jmb_out;         /* values the target has posted */
	/* When set, replaces cntrl_sig once IR_CNTRL_SIG_RELEASE is shifted
	 * (the CPU was released: sync may never come back). */
	bool has_cntrl_sig_after_release = false;
	uint16_t cntrl_sig_after_release = 0;
	uint32_t millis_now = 0;
	uint32_t millis_step = 1;

	/* EEM registers behind IR_EMEX_DATA_EXCHANGE32, by even address. A
	 * 32-bit shift carries an address (bit 0 set: read); the next shift
	 * returns the register (read) or stores its value (write). */
	std::map<uint8_t, uint32_t> eem;

	/* --- recordings --- */
	std::vector<std::string> log;
	std::vector<std::pair<uint8_t, uint32_t>> eem_writes; /* (address, value) */
	std::vector<std::pair<uint32_t, uint16_t>> mem_writes; /* (address, data) */
	std::vector<uint16_t> jmb_in;         /* words the probe wrote to the mailbox */
	bool jmb_protocol_error = false;      /* unexpected mailbox control word */

	/* Post a 32-bit value in the target's mailbox output. */
	void push_jmb_out(uint32_t value) { jmb_out.push_back(value); }

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

	/* When >= 0: after this many shifts (IR and DR together) the data line
	 * floats high, so every later shift reads all ones. */
	int ones_after_shifts = -1;
	int shifts() const { return shifts_; }

	uint8_t ir_shift(uint8_t ir) override
	{
		const uint8_t v = ir_shift_(ir);
		return floating() ? 0xFF : v;
	}
	uint16_t dr_shift16(uint16_t value) override
	{
		const uint16_t v = dr_shift16_(value);
		return floating() ? 0xFFFF : v;
	}
	uint32_t dr_shift20(uint32_t value) override
	{
		const uint32_t v = dr_shift20_(value);
		return floating() ? 0xFFFFF : v;
	}
	uint32_t dr_shift32(uint32_t value) override
	{
		const uint32_t v = dr_shift32_(value);
		return floating() ? 0xFFFFFFFFu : v;
	}

private:
	/* Counts one shift; true if this one reads all ones. */
	bool floating()
	{
		const int before = shifts_++;
		return ones_after_shifts >= 0 && before >= ones_after_shifts;
	}

	uint8_t ir_shift_(uint8_t ir)
	{
		ir_ = ir;
		jmb_phase_ = JmbPhase::Control;
		eem_addr_pending_ = false;
		if (ir == msp430::kIrCntrlSigRelease && has_cntrl_sig_after_release) {
			cntrl_sig = cntrl_sig_after_release;
		}
		log.push_back(fmt("ir:%02x", ir));
		return pop(jtag_id_script, jtag_id);
	}

	uint16_t dr_shift16_(uint16_t value)
	{
		millis_now += millis_per_shift;
		log.push_back(fmt("dr16:%04x", value));
		switch (ir_) {
		case msp430::kIrCntrlSigCapture:
			return pop(cntrl_sig_script, cntrl_sig);
		case msp430::kIrCoreipId:
			return core_id;
		case msp430::kIrDataQuick:
			return pop(quick_words, 0xFFFF);
		case msp430::kIrJmbExchange:
			return jmb_shift(value);
		case msp430::kIrDataToAddr:
			mem_writes.push_back({addr_, value});
			return 0;
		default:
			return 0;
		}
	}

	uint32_t dr_shift20_(uint32_t value)
	{
		log.push_back(fmt("dr20:%05x", value));
		if (ir_ == msp430::kIrAddr16Bit) {
			addr_ = value;
		}
		if (ir_ != msp430::kIrDeviceId) {
			return 0;
		}
		return ((device_id_ptr << 16) + (device_id_ptr >> 4)) & 0xFFFFFu;
	}

	uint32_t dr_shift32_(uint32_t value)
	{
		log.push_back(fmt("dr32:%08x", value));
		if (ir_ != msp430::kIrEmexDataExchange32) {
			return 0;
		}
		if (!eem_addr_pending_) {
			eem_addr_ = static_cast<uint8_t>(value);
			eem_addr_pending_ = true;
			return 0;
		}
		eem_addr_pending_ = false;
		const uint8_t reg = static_cast<uint8_t>(eem_addr_ & 0xFEu);
		if ((eem_addr_ & 1u) != 0) {
			return eem.count(reg) != 0 ? eem[reg] : 0;
		}
		eem[reg] = value;
		eem_writes.push_back({reg, value});
		return 0;
	}

public:
	void set_tclk() override { log.push_back("tclk:1"); }
	void clr_tclk() override { log.push_back("tclk:0"); }
	void delay_ms(uint32_t ms) override { log.push_back("delay:" + std::to_string(ms)); }
	uint32_t millis() override
	{
		millis_now += millis_step;
		return millis_now;
	}

private:
	int shifts_ = 0;
	static std::string fmt(const char *f, unsigned v)
	{
		char buf[32];
		std::snprintf(buf, sizeof(buf), f, v);
		return buf;
	}
	template <typename T>
	static T pop(std::deque<T> &q, std::type_identity_t<T> fallback)
	{
		if (q.empty()) {
			return fallback;
		}
		const T v = q.front();
		q.pop_front();
		return v;
	}

	enum class JmbPhase { Control, OutLow, OutHigh, InData };

	/* JTAG mailbox as i_ReadJmbOut / i_WriteJmbIn16 use it: a control word,
	 * then either two data reads (32-bit output) or one data write. */
	uint16_t jmb_shift(uint16_t value)
	{
		switch (jmb_phase_) {
		case JmbPhase::Control:
			if (value == 0x0000) { /* status read */
				if (jmb_hidden_polls > 0) {
					--jmb_hidden_polls;
					return 0;
				}
				return static_cast<uint16_t>(
					(jmb_in_ready ? msp430::kJmbIn0Rdy : 0) |
					(jmb_out.empty() ? 0 : msp430::kJmbOut1Rdy));
			}
			if (value == (msp430::kJmb32Bit | msp430::kJmbOutReq) && !jmb_out.empty()) {
				jmb_phase_ = JmbPhase::OutLow;
				return 0;
			}
			if (value == msp430::kJmbInReq && jmb_in_ready) {
				jmb_phase_ = JmbPhase::InData;
				return 0;
			}
			jmb_protocol_error = true;
			return 0;
		case JmbPhase::OutLow:
			jmb_phase_ = JmbPhase::OutHigh;
			return static_cast<uint16_t>(jmb_out.front());
		case JmbPhase::OutHigh: {
			const uint16_t high = static_cast<uint16_t>(jmb_out.front() >> 16);
			jmb_out.pop_front();
			jmb_phase_ = JmbPhase::Control;
			return high;
		}
		case JmbPhase::InData:
			jmb_in.push_back(value);
			jmb_phase_ = JmbPhase::Control;
			return 0;
		}
		return 0;
	}

	JmbPhase jmb_phase_ = JmbPhase::Control;
	bool eem_addr_pending_ = false;
	uint8_t eem_addr_ = 0;
	uint8_t ir_ = 0;
	uint32_t addr_ = 0;
};
