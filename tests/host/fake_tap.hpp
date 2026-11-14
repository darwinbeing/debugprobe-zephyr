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
	/* EEM registers (even address) that read as this value instead of the
	 * stored one: a register the chip lacks, or bits it adds. Writes are
	 * stored as before. */
	std::map<uint8_t, uint32_t> eem_read_as;
	/* Consumed first, one per EEM register read, in place of what the
	 * register reads (eem_read_as, eem). */
	std::deque<uint32_t> eem_read_script;

	/* --- debug-layer model (msp430::Debugger) --- */
	static constexpr uint32_t kWdtctl = 0x015C;
	static constexpr uint16_t kWdtctlRead = 0x6900; /* WDTCTL reads 0x69xx */
	/* Word memory read through IR_DATA_CAPTURE right after an address was
	 * loaded with IR_ADDR_16BIT (ReadMemWordXv2); writes through
	 * IR_DATA_TO_ADDR update it. Missing addresses read 0. */
	std::map<uint32_t, uint16_t> memory = {
		{kWdtctl, 0x6904}, /* WDTCTL after reset (password 0x69 on read) */
		{0x0144, 0x9658},  /* FCTL3: locked, not busy */
		{0x1A04, 0x2955},  /* TLV device ID of the F5529 */
		{0xFFFE, 0x441A},  /* reset vector */
	};
	/* DR16 under IR_DATA_CAPTURE when no address is pending: the two words
	 * (low, high) of every CPU register read (ReadCpuRegXv2). Default 0. */
	std::deque<uint16_t> captured;
	/* DR20 under IR_ADDR_CAPTURE. Every such shift consumes one mab_script
	 * entry, dummy captures included (SetPcXv2 and WriteCpuRegXv2 end with
	 * one); then mab. */
	uint32_t mab = 0x04524;
	std::deque<uint32_t> mab_script;
	/* DR16 under IR_EMEX_READ_CONTROL. Every read consumes one
	 * eem_control_script entry; then eem_control. Writes do not change it. */
	uint16_t eem_control = 0x0000;
	std::deque<uint16_t> eem_control_script;

	/* --- recordings --- */
	std::vector<std::string> log;
	std::vector<std::pair<uint8_t, uint32_t>> eem_writes; /* (address, value) */
	std::vector<uint16_t> eem_control_writes; /* DR16 under IR_EMEX_WRITE_CONTROL */
	std::vector<std::pair<uint32_t, uint16_t>> mem_writes; /* (address, data) */
	std::vector<uint16_t> jmb_in;         /* words the probe wrote to the mailbox */
	bool jmb_protocol_error = false;      /* unexpected mailbox control word */
	/* EEM exchange out of order: an IR shift between address and data, an
	 * address above one byte, or a 16/20-bit shift under the EEM IR. */
	bool eem_protocol_error = false;
	/* A write to WDTCTL without 0x5A, or to PMMCTL0, FCTL1, FCTL3 or FCTL4
	 * without 0xA5, in the high byte. On the chip this causes a PUC and
	 * nothing is written: the write is recorded in mem_writes, memory keeps
	 * its contents. */
	bool password_violation = false;
	/* A memory access out of step with TCLK and the control signal's R/W bit
	 * (ReadMemWordXv2, WriteMemWordXv2): a read with no TCLK rise since the
	 * address load, with TCLK high at the capture, or with the CPU set to
	 * write; a write with TCLK low or with the CPU set to read. Also a 20-
	 * or 32-bit shift under IR_DATA_TO_ADDR, whose data register is 16 bits
	 * (not recorded as a write). */
	bool mem_protocol_error = false;
	/* Defence in depth: a memory write (IR_DATA_TO_ADDR) to the F5529's BSL,
	 * information memory and device descriptors (0x1000-0x1BFF), main flash
	 * (from 0x4400) or the flash controller (0x0140-0x014F). Recorded and
	 * carried out as any other write. */
	bool flash_write_attempt = false;
	/* Words fed to the CPU, in order: every DR shift under IR_DATA_16BIT
	 * drives the data bus for the CPU's next read (MSPDS shifts under it
	 * only to feed the CPU; its captures use IR_DATA_CAPTURE). They are
	 * decoded as the instruction stream they form, each instruction
	 * followed by its extension word. Allowed, as debug.cpp feeds them
	 * through the MSPDS helpers (MSPDS 3.15.1.1 Bios/src/hil/edt.h) and as
	 * Phase A's Cpuxv2::set_pc feeds SLAU320's SetPC_430Xv2:
	 *  - MOVA #imm20, Rn: (w & 0xF0F0) == 0x0080, then imm15:0, any value:
	 *    WriteCpuRegXv2 (edt.h:651-675), SetPcXv2 (edt.h:728-749);
	 *  - MOVA Rn, &0x0FF6: (w & 0xF0FF) == 0x0060 (abs19:16 = 0), then
	 *    0x0FF6 only: ReadCpuRegXv2 with altRomAddressForCpuRead
	 *    (edt.h:541-589), the register captured from the data bus;
	 *  - JMP $-4, 0x3FFD, which MSPDS feeds after those MOVAs (edt.h:565,
	 *    663), and NOP (MOV #0, R3), 0x4303, which SetPcXv2 feeds after its
	 *    MOVA (edt.h:745): both accepted wherever an instruction is due, not
	 *    only there;
	 *  - SAFE_PC_ADDRESS 0x0004 (Bios/src/hal/JTAG_defs.h:118) as the first
	 *    word after a power-on reset through the control signal (POR bit
	 *    0x0800), which the CPU takes for its PC
	 *    (SyncJtag_AssertPor_SaveContextXv2.c:114-119); there any other
	 *    word is decoded as an instruction.
	 * Anything else, a 20- or 32-bit shift under IR_DATA_16BIT, or a
	 * release (IR_CNTRL_SIG_RELEASE) while an extension word is still due
	 * (the CPU would fetch it from memory) sets unexpected_instruction: the
	 * CPU would execute something the debugger never feeds, such as a store
	 * to flash or to the flash controller.
	 * The decoder works per shift and ignores TCLK and CPUSUSP: it cannot
	 * see the CPU run from memory between fed words, nor which word a clock
	 * edge latches (in MSPDS a word is shifted first and the clock that
	 * follows latches it). A sequence with clocks added or dropped, such as
	 * two more TCLK cycles under IR_ADDR_CAPTURE with the control signal at
	 * 0x1401, or no TCLK cycle between the 0x0FF6 and 0x3FFD shifts, passes
	 * it; the exact transcripts of test_debugger.cpp catch those (see
	 * debugger_never_writes_flash). */
	std::vector<uint16_t> cpu_words;
	bool unexpected_instruction = false;

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
		if (eem_addr_pending_) {
			eem_protocol_error = true; /* the data shift never came */
		}
		eem_addr_pending_ = false;
		/* ReadMemWordXv2 captures right after the address load; any other
		 * instruction ends that read, so the next capture is a register word. */
		if (ir != msp430::kIrDataCapture) {
			addr_pending_ = false;
		}
		if (ir == msp430::kIrCntrlSigRelease && has_cntrl_sig_after_release) {
			cntrl_sig = cntrl_sig_after_release;
		}
		if (ir == msp430::kIrCntrlSigRelease) {
			if (feed_ == Feed::ImmWord || feed_ == Feed::AbsWord) {
				unexpected_instruction = true; /* fetched from memory instead */
			}
			feed_ = Feed::Instruction;
		}
		log.push_back(fmt("ir:%02x", ir));
		return pop(jtag_id_script, jtag_id);
	}

	uint16_t dr_shift16_(uint16_t value)
	{
		millis_now += millis_per_shift;
		log.push_back(fmt("dr16:%04x", value));
		if (ir_ == msp430::kIrEmexDataExchange32) {
			eem_protocol_error = true;
		}
		switch (ir_) {
		case msp430::kIrCntrlSigCapture:
			return pop(cntrl_sig_script, cntrl_sig);
		case msp430::kIrCoreipId:
			return core_id;
		case msp430::kIrDataQuick:
			return pop(quick_words, 0xFFFF);
		case msp430::kIrJmbExchange:
			return jmb_shift(value);
		case msp430::kIrCntrlSig16Bit:
			cntrl_read_ = (value & 0x0001u) != 0; /* R/W: 1 = read */
			if ((value & 0x0800u) != 0) {         /* POR */
				feed_ = Feed::AfterPor;
			}
			return 0;
		case msp430::kIrData16Bit:
			cpu_words.push_back(value);
			feed_cpu(value);
			return 0;
		case msp430::kIrDataToAddr:
			mem_writes.push_back({addr_, value});
			if (!tclk_ || cntrl_read_) {
				mem_protocol_error = true;
			}
			if (is_flash_or_controller(addr_)) {
				flash_write_attempt = true;
			}
			if (!password_ok(addr_, value)) {
				password_violation = true; /* a PUC on the chip: no write */
				return 0;
			}
			/* WDTCTL reads back with its read password; WDTCNTCL (bit 3)
			 * reads 0 */
			memory[addr_] = addr_ == kWdtctl ? static_cast<uint16_t>(kWdtctlRead | (value & 0xF7))
							  : value;
			return 0;
		case msp430::kIrDataCapture:
			if (addr_pending_) {
				addr_pending_ = false;
				if (!addr_clocked_ || tclk_ || !cntrl_read_) {
					mem_protocol_error = true;
				}
				const auto it = memory.find(addr_);
				return it != memory.end() ? it->second : 0;
			}
			return pop(captured, 0);
		case msp430::kIrEmexReadControl:
			return pop(eem_control_script, eem_control);
		case msp430::kIrEmexWriteControl:
			eem_control_writes.push_back(value);
			return 0;
		default:
			return 0;
		}
	}

	uint32_t dr_shift20_(uint32_t value)
	{
		log.push_back(fmt("dr20:%05x", value));
		wrong_width();
		if (ir_ == msp430::kIrAddr16Bit) {
			addr_ = value;
			addr_pending_ = true;
			addr_clocked_ = false;
		}
		if (ir_ == msp430::kIrAddrCapture) {
			return pop(mab_script, mab);
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
			wrong_width();
			return 0;
		}
		if (!eem_addr_pending_) {
			if (value > 0xFFu) {
				eem_protocol_error = true; /* EEM addresses are one byte */
			}
			eem_addr_ = static_cast<uint8_t>(value);
			eem_addr_pending_ = true;
			return 0;
		}
		eem_addr_pending_ = false;
		const uint8_t reg = static_cast<uint8_t>(eem_addr_ & 0xFEu);
		if ((eem_addr_ & 1u) != 0) {
			if (!eem_read_script.empty()) {
				return pop(eem_read_script, 0);
			}
			const auto as = eem_read_as.find(reg);
			if (as != eem_read_as.end()) {
				return as->second;
			}
			const auto it = eem.find(reg);
			return it != eem.end() ? it->second : 0;
		}
		eem[reg] = value;
		eem_writes.push_back({reg, value});
		return 0;
	}

public:
	void set_tclk() override
	{
		log.push_back("tclk:1");
		tclk_ = true;
		addr_clocked_ = true;
	}
	void clr_tclk() override
	{
		log.push_back("tclk:0");
		tclk_ = false;
	}
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
	static bool is_flash_or_controller(uint32_t addr)
	{
		return (addr >= 0x1000 && addr <= 0x1BFF) || addr >= 0x4400 ||
		       (addr >= 0x0140 && addr <= 0x014F);
	}
	/* the key in the high byte of a password-protected register */
	static bool password_ok(uint32_t addr, uint16_t value)
	{
		const unsigned key = value >> 8;
		switch (addr) {
		case kWdtctl:
			return key == 0x5A;
		case 0x0120: /* PMMCTL0 */
		case 0x0140: /* FCTL1 */
		case 0x0144: /* FCTL3 */
		case 0x0146: /* FCTL4 */
			return key == 0xA5;
		default:
			return true;
		}
	}
	/* A 20- or 32-bit shift where the debugger shifts 16 bits: under the
	 * EEM exchange (32 bits; dr_shift32 does not call this for it), memory
	 * write data, a word fed to the CPU. */
	void wrong_width()
	{
		switch (ir_) {
		case msp430::kIrEmexDataExchange32:
			eem_protocol_error = true;
			break;
		case msp430::kIrDataToAddr:
			mem_protocol_error = true;
			break;
		case msp430::kIrData16Bit:
			unexpected_instruction = true; /* a word the decoder cannot see */
			break;
		default:
			break;
		}
	}

	/* What the next word fed to the CPU is (see cpu_words). */
	enum class Feed { Instruction, ImmWord, AbsWord, AfterPor };

	void feed_cpu(uint16_t w)
	{
		const Feed at = feed_;
		feed_ = Feed::Instruction;
		switch (at) {
		case Feed::ImmWord: /* imm15:0 of MOVA #imm20, Rn */
			return;
		case Feed::AbsWord: /* abs15:0 of MOVA Rn, &abs20 */
			if (w != 0x0FF6) {
				unexpected_instruction = true;
			}
			return;
		case Feed::AfterPor:
			if (w == 0x0004) { /* SAFE_PC_ADDRESS */
				return;
			}
			break;
		case Feed::Instruction:
			break;
		}
		if ((w & 0xF0F0u) == 0x0080u) { /* MOVA #imm20, Rn */
			feed_ = Feed::ImmWord;
		} else if ((w & 0xF0FFu) == 0x0060u) { /* MOVA Rn, &abs20, abs19:16 = 0 */
			feed_ = Feed::AbsWord;
		} else if (w != 0x3FFD && w != 0x4303) { /* JMP $-4, NOP */
			unexpected_instruction = true;
		}
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
	bool addr_pending_ = false;
	bool tclk_ = false;         /* TCLK level */
	bool addr_clocked_ = false; /* set_tclk since the address load (even if TCLK was high) */
	bool cntrl_read_ = true;    /* R/W bit of the last IR_CNTRL_SIG_16BIT word */
	Feed feed_ = Feed::Instruction;
};
