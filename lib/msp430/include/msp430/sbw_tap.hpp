#pragma once
/* Tap over Spy-Bi-Wire: builds the per-cycle TMS/TDI vectors that TI's
 * IR_Shift / DR_Shift16 / DR_Shift20 / AllShifts / ResetTAP / SetTCLK_sbw /
 * ClrTCLK_sbw produce, and the StopJtag / ConnectJTAG /
 * EntrySequences_RstHigh_SBW / EntrySequences_RstLow_SBW pin sequences
 * (slau320, LowLevelFunc430Xv2.c, JTAGfunc430Xv2.c).
 */
#include "msp430/sbw_port.hpp"
#include "msp430/tap.hpp"

namespace msp430 {

class SbwTap final : public Tap {
public:
	explicit SbwTap(SbwPort &port) : port_(port) {}

	void set_speed(uint32_t slot_hz) override { port_.set_clock(slot_hz); }
	bool open() override;
	void enter(Entry entry) override;
	void close() override { port_.release(); }

	uint8_t ir_shift(uint8_t ir) override;
	uint16_t dr_shift16(uint16_t value) override;
	uint32_t dr_shift20(uint32_t value) override;
	void set_tclk() override;
	void clr_tclk() override;

	void delay_ms(uint32_t ms) override { port_.delay_us(ms * 1000u); }
	uint32_t millis() override { return port_.millis(); }

	/* 6x TMS=1 to Test-Logic-Reset, then one TMS=0 to Run-Test/Idle. */
	void reset();

private:
	uint32_t scan(bool ir, unsigned nbits, uint32_t data);

	SbwPort &port_;
	bool tclk_ = true; /* TI: TCLK_saved starts high */
};

} // namespace msp430
