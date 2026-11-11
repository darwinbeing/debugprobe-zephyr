/*
 * MSP430 CPUXv2 device operations: a port of the JTAG control sequences in
 * TI's slau320 Replicator430Xv2 (JTAGfunc430Xv2.c). The sequences are kept
 * call-for-call identical; the TI function is named above each port.
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
#include "msp430/cpuxv2.hpp"

namespace msp430 {

/* "Check Init State": the CPU is under JTAG control and synchronized. */
bool Cpuxv2::in_full_emulation()
{
	tap_.ir_shift(kIrCntrlSigCapture);
	return (tap_.dr_shift16(0) & 0x0301) != 0;
}

/* WriteMem_430Xv2(F_WORD, addr, data) */
bool Cpuxv2::write_word(uint32_t addr, uint16_t data)
{
	if (!in_full_emulation()) {
		return false;
	}
	tap_.clr_tclk();
	tap_.ir_shift(kIrCntrlSig16Bit);
	tap_.dr_shift16(0x0500);
	tap_.ir_shift(kIrAddr16Bit);
	tap_.dr_shift20(addr);
	tap_.set_tclk();
	/* New style: only apply data during the clock high phase */
	tap_.ir_shift(kIrDataToAddr);
	tap_.dr_shift16(data);
	tap_.clr_tclk();
	tap_.ir_shift(kIrCntrlSig16Bit);
	tap_.dr_shift16(0x0501);
	tap_.set_tclk();
	/* one more cycle, so the CPU is driving the correct MAB */
	tap_.clr_tclk();
	tap_.set_tclk();
	return true;
}

/* SetPC_430Xv2(addr): feed "MOVA #imm20, PC" to the CPU. */
bool Cpuxv2::set_pc(uint32_t addr)
{
	const uint16_t mova = static_cast<uint16_t>(0x0080u | ((addr >> 8) & 0x0F00u));
	const uint16_t pc_low = static_cast<uint16_t>(addr & 0xFFFFu);

	if (!in_full_emulation()) {
		return false;
	}
	tap_.clr_tclk();
	/* take over bus control during the clock low phase */
	tap_.ir_shift(kIrData16Bit);
	tap_.set_tclk();
	tap_.dr_shift16(mova);
	tap_.clr_tclk();
	tap_.ir_shift(kIrCntrlSig16Bit);
	tap_.dr_shift16(0x1400);
	tap_.ir_shift(kIrData16Bit);
	tap_.clr_tclk();
	tap_.set_tclk();
	tap_.dr_shift16(pc_low);
	tap_.clr_tclk();
	tap_.set_tclk();
	tap_.dr_shift16(0x4303); /* NOP */
	tap_.clr_tclk();
	tap_.ir_shift(kIrAddrCapture);
	tap_.dr_shift20(0x00000);
	return true;
}

/* WriteMemQuick_430Xv2 */
Status Cpuxv2::write_mem(uint32_t addr, size_t nwords, const uint8_t *src)
{
	for (size_t i = 0; i < nwords; ++i) {
		const uint16_t word = static_cast<uint16_t>(src[2 * i] | (src[2 * i + 1] << 8));
		if (!write_word(addr + 2u * static_cast<uint32_t>(i), word)) {
			return Status::TargetError;
		}
	}
	return Status::Ok;
}

/* ReadMemQuick_430Xv2 (JTAG ID 0x91 path) */
Status Cpuxv2::read_mem(uint32_t addr, size_t nwords, uint8_t *dst)
{
	if (!set_pc(addr)) {
		return Status::TargetError;
	}
	tap_.set_tclk();
	tap_.ir_shift(kIrCntrlSig16Bit);
	tap_.dr_shift16(0x0501);
	tap_.ir_shift(kIrAddrCapture);
	tap_.ir_shift(kIrDataQuick);
	for (size_t i = 0; i < nwords; ++i) {
		tap_.set_tclk();
		tap_.clr_tclk();
		const uint16_t word = tap_.dr_shift16(0);
		dst[2 * i] = static_cast<uint8_t>(word);
		dst[2 * i + 1] = static_cast<uint8_t>(word >> 8);
	}
	tap_.set_tclk();
	return Status::Ok;
}

} // namespace msp430
