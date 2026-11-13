/*
 * MSP430 JTAG over Spy-Bi-Wire: IR/DR shifts, TCLK control, TAP reset and the
 * SBW entry sequences, as per-cycle TMS/TDI vectors. The entry sequences, the
 * shift framing and the 20-bit de-scramble are ported from TI's slau320
 * Replicator430Xv2 (JTAGfunc430Xv2.c, LowLevelFunc430Xv2.c); the TI function
 * is named above each port.
 *
 * Copyright (C) 2016 Texas Instruments Incorporated - http://www.ti.com/
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *    Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 *    Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the
 *    distribution.
 *
 *    Neither the name of Texas Instruments Incorporated nor the names of
 *    its contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *  A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *  OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
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
 * Run-Test/Idle. TDI stays high throughout, so TCLK ends high.
 *
 * TI's ResetTAP leaves TCLK_saved untouched; here the tracked level is set
 * high on purpose, because the last reset cycle enters Run-Test/Idle with
 * TDI = 1, so TCLK really is high afterwards. */
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
 *
 * `nbits` is 1..32. One port call clocks at most 32 cycles, so a longer scan
 * (the 32-bit DR scan has 3 + 32 + 2 = 37 cycles) is split into several
 * calls. Between two calls SBWTCK stays high and the TAP stays in Shift-DR;
 * SBW limits only the low phase of SBWTCK, not the high phase.
 */
uint32_t SbwTap::scan(bool ir, unsigned nbits, uint32_t data)
{
	const unsigned head = ir ? 4u : 3u;
	const unsigned total = head + nbits + 2u;
	uint64_t tms = ir ? 0b0011u : 0b001u;
	uint64_t tdi = (ir ? 0b1110u : 0b110u) | (tclk_ ? 1u : 0u);

	for (unsigned k = 0; k < nbits; ++k) {
		if ((data >> (nbits - 1u - k)) & 1u) {
			tdi |= uint64_t{1} << (head + k);
		}
	}
	tms |= uint64_t{1} << (head + nbits - 1u); /* last data bit: Exit1 */
	tms |= uint64_t{1} << (head + nbits);      /* Update */
	tdi |= uint64_t{1} << (head + nbits);
	if (tclk_) {
		tdi |= uint64_t{1} << (head + nbits + 1u); /* Run-Test/Idle: TDI = TCLK */
	}

	uint64_t tdo = 0;
	for (unsigned done = 0; done < total;) {
		const unsigned n = (total - done > 32u) ? 32u : total - done;
		const uint32_t mask = (n == 32u) ? 0xFFFFFFFFu : ((1u << n) - 1u);
		const uint32_t part = port_.cycles(static_cast<uint32_t>(tms >> done) & mask,
						   static_cast<uint32_t>(tdi >> done) & mask, n);
		tdo |= static_cast<uint64_t>(part & mask) << done;
		done += n;
	}

	uint32_t out = 0;
	for (unsigned k = 0; k < nbits; ++k) {
		out = (out << 1) | static_cast<uint32_t>((tdo >> (head + k)) & 1u);
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

/* MSPDS hil_2w.c, _hil_2w_SetReg_XBits32: same framing, 32 data bits, no
 * de-scramble. */
uint32_t SbwTap::dr_shift32(uint32_t value)
{
	return scan(false, 32, value);
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
