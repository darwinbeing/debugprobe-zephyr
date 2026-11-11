#include "msp430/sbw_tap.hpp"

namespace msp430 {

/* StopJtag + ConnectJTAG (JTAGfunc430Xv2.c): release the pins so the
 * target's test logic resets, then drive SBWTCK low and SBWTDIO high. */
bool SbwTap::open()
{
	port_.release();
	delay_ms(15);
	if (!port_.acquire()) {
		return false;
	}
	delay_ms(15);
	return true;
}

/* EntrySequences_RstHigh_SBW / EntrySequences_RstLow_SBW followed by
 * ResetTAP. SBWTCK is the target's TEST pin, SBWTDIO its RST pin. The one
 * timing-critical step is the short low pulse on TEST ("phase 2..4"), which
 * the port generates. */
void SbwTap::enter(Entry entry)
{
	if (entry == Entry::RstHigh) {
		port_.set_tck(false);
		delay_ms(4); /* reset TEST logic */
		port_.set_tdio(true);
		port_.set_tck(true);
		delay_ms(20); /* activate TEST logic */
		port_.set_tdio(true);
		port_.delay_us(60);
		port_.tck_pulse();
		port_.delay_us(60);
		delay_ms(5);
	} else {
		port_.set_tck(false);
		delay_ms(1);
		port_.set_tdio(false); /* hold the device in reset */
		delay_ms(50);
		port_.set_tck(true);
		delay_ms(100);
		port_.set_tdio(true);
		port_.delay_us(40);
		port_.tck_pulse();
		port_.delay_us(40);
		delay_ms(5);
	}
	reset();
}
/* ResetTAP (JTAGfunc430Xv2.c): six cycles with TMS high reach
 * Test-Logic-Reset from any state, one cycle with TMS low enters
 * Run-Test/Idle. TDI stays high throughout, so TCLK ends high. */
void SbwTap::reset()
{
	port_.cycles(0x3F, 0x7F, 7);
	tclk_ = true;
}

/* One complete scan, as IR_Shift / DR_Shift16 / DR_Shift20 + AllShifts do:
 *
 *   IR: cycle 0 (TMS 1, TDI = TCLK)  Run-Test/Idle -> Select-DR
 *       cycle 1 (1, 1)               -> Select-IR
 *       cycle 2 (0, 1)               -> Capture-IR
 *       cycle 3 (0, 1)               -> Shift-IR
 *   DR: cycle 0 (TMS 1, TDI = TCLK)  Run-Test/Idle -> Select-DR
 *       cycle 1 (0, 1)               -> Capture-DR
 *       cycle 2 (0, 1)               -> Shift-DR
 *   then `nbits` data cycles, MSB first, TMS high on the last one (Exit1),
 *   then (1, 1) Update and (0, TDI = TCLK) back to Run-Test/Idle.
 *
 * TDI follows the saved TCLK level on the first and last cycle so that TCLK
 * does not change while the TAP leaves and re-enters Run-Test/Idle.
 */
uint32_t SbwTap::scan(bool ir, unsigned nbits, uint32_t data)
{
	const unsigned head = ir ? 4u : 3u;
	uint32_t tms = ir ? 0b0011u : 0b001u;
	uint32_t tdi = (ir ? 0b1110u : 0b110u) | (tclk_ ? 1u : 0u);

	for (unsigned k = 0; k < nbits; ++k) {
		if ((data >> (nbits - 1u - k)) & 1u) {
			tdi |= 1u << (head + k);
		}
	}
	tms |= 1u << (head + nbits - 1u); /* last data bit: Exit1 */
	tms |= 1u << (head + nbits);      /* Update */
	tdi |= 1u << (head + nbits);
	if (tclk_) {
		tdi |= 1u << (head + nbits + 1u); /* Run-Test/Idle: TDI = TCLK */
	}

	const uint32_t tdo = port_.cycles(tms, tdi, head + nbits + 2u);

	uint32_t out = 0;
	for (unsigned k = 0; k < nbits; ++k) {
		out = (out << 1) | ((tdo >> (head + k)) & 1u);
	}
	return out;
}

uint8_t SbwTap::ir_shift(uint8_t ir)
{
	return static_cast<uint8_t>(scan(true, 8, ir));
}

uint16_t SbwTap::dr_shift16(uint16_t value)
{
	return static_cast<uint16_t>(scan(false, 16, value));
}

uint32_t SbwTap::dr_shift20(uint32_t value)
{
	const uint32_t v = scan(false, 20, value & 0xFFFFFu);
	/* AllShifts, F_ADDR: de-scramble the 20-bit result. */
	return ((v << 16) + (v >> 4)) & 0xFFFFFu;
}

/* SetTCLK_sbw / ClrTCLK_sbw: one Run-Test/Idle cycle whose TDI slot carries
 * the new TCLK level; the TMS slot uses TMSLDH when TCLK is currently high. */
void SbwTap::set_tclk()
{
	port_.tclk_cycle(tclk_, true);
	tclk_ = true;
}

void SbwTap::clr_tclk()
{
	port_.tclk_cycle(tclk_, false);
	tclk_ = false;
}

} // namespace msp430
