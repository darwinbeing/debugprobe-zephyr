/*
 * MSP430 CPUXv2 debugging: a port of the CPUXv2 macros of TI's MSP Debug
 * Stack, MSPDS-OPEN-SOURCE 3.15.1.1 (MSPDebugStack_OS_Package_3_15_1_1.zip),
 * Bios/src/hal/macros and the helpers they inline from Bios/src/hil/edt.h.
 * The macro or helper is named above each port, with its lines. The JTAG ID
 * 0x91 paths are taken; the device parameters are those of the MSP430F5529
 * in TI's device database (MSPDS 3.3.1.3 TemplateDeviceDb): no LPMx.5 test
 * register settings (DisableLpmx5() does nothing), altRomAddressForCpuRead
 * set (feature "_1377"), "large" EEM with 8 memory-bus triggers.
 *
 * Deliberate differences from MSPDS:
 *  - a sync that does not complete (SyncJtagXv2, wait_for_synch), a CPU pipe
 *    that never empties, or a JTAG ID other than 0x91 in the IR capture of
 *    SyncJtagXv2 or wait_for_synch (MSPDS discards it there) or at the
 *    start of SyncJtag_AssertPor_SaveContextXv2 (MSPDS takes the other
 *    devices' paths there) ends the operation with TargetError; the CPU,
 *    not yet in full emulation, is then released without anything written
 *    to the target's memory (MSPDS carries on, except that a failed
 *    wait_for_synch returns an error without releasing the CPU). When
 *    SyncJtag_AssertPor_SaveContextXv2 has already switched the system
 *    clocks to JCK (GENCLKCTRL), they are switched back first (lines
 *    198-201, as its success path ends);
 *  - WDTCTL is checked for its read password 0x69 before it is written, not
 *    afterwards as the DLL does (WatchdogControl::checkRead);
 *  - "check for Init State" after a sync is Cpuxv2::in_full_emulation(),
 *    which also checks the JTAG ID; read_mem, write_mem, read_regs and
 *    write_reg end with the same check;
 *  - when the init-state check or the WDTCTL password check fails during a
 *    sync (MSPDS checks neither, in both sync macros), the CPU is already in
 *    full emulation and the operation ends with TargetError. After
 *    SyncJtag_Conditional_SaveContextXv2 the CPU is released at the PC
 *    where it stopped (SetPcXv2 and the release of
 *    RestoreContext_ReleaseJtagXv2, the PC 2 lower if the control signal
 *    shows CPUOFF), SR and WDTCTL unwritten. In
 *    SyncJtag_AssertPor_SaveContextXv2 the power-on reset has left the CPU
 *    at SAFE_PC_ADDRESS (0x0004, vacant memory), or after a failed
 *    init-state check its state is unknown, and the reset vector is not
 *    read yet: the system clocks are switched back (GENCLKCTRL 0x000E,
 *    lines 198-201), then the CPU gets another power-on reset (EEM off,
 *    ReleaseDevice_430Xv2(V_RESET), as detach(Reset)) and starts at its
 *    reset vector;
 *  - the PC derived from the MAB (MAB - 4, or - 2 in a low-power mode) is
 *    kept to 20 bits; MSPDS computes it in 32 bits, so a MAB below 4 wraps
 *    to 0xFFFFFFFx there and to 0xFFFFx here;
 *  - that PC is replaced by the reset vector read from 0xFFFE only when it
 *    is 0x0FFFE or 0xFFFFE (Note 1495, 1637); MSPDS replaces any PC ending
 *    in FFFE, which includes 0x1FFFE in the F5529's main flash;
 *  - after the watchdog is held and before SR is saved,
 *    SyncJtag_Conditional_SaveContextXv2 waits up to 100 ms for the flash
 *    controller to finish an erase or write the firmware had started
 *    (FCTL3.BUSY; not in MSPDS). The design spec (item 14) originally put
 *    this wait before the hold; the Task 5 review changed both. Holding
 *    first keeps the watchdog from resetting the device during the wait.
 *    The reset-and-halt sync does not wait: after its power-on reset no
 *    flash operation can be running;
 *  - SyncJtag_AssertPor_SaveContextXv2 leaves out the per-module clock
 *    control (ETKEYSEL/ETCLKSEL), whose module codes come from the DLL's
 *    device database; the modules keep their reset defaults. GENCLKCTRL ends
 *    at 0x000E as in MSPDS: MCLK, SMCLK and ACLK stop while the CPU is
 *    halted. It also ends there when the macro fails after switching the
 *    clocks to JCK (0x6D8E); MSPDS then returns with them on JCK;
 *  - read_regs keeps each value to 20 bits; ReadAllCpuRegsXv2 passes 24
 *    bits of the capture on. write_reg writes one register (the macro
 *    writes R1 and R4-R15 together) and refuses a value above 20 bits, an
 *    odd PC or an SR above 16 bits with BadArg; the macro drops bits 20-23;
 *  - SingleStepXv2 polls up to 496 times as MSPDS does (its counter starts
 *    at 4); if the CPU has not stopped by then, it is stopped as halt()
 *    does, trigger block 0 is restored and Timeout returned (MSPDS returns
 *    an error and leaves trigger block 0 set up for the step);
 *  - breakpoints: one trigger block per breakpoint, set up as the trigger
 *    SyncJtag_Conditional_SaveContextXv2 uses for "next instruction fetch",
 *    with the exact address; the value register is read back to confirm
 *    that the trigger block exists. All trigger blocks are cleared on
 *    attach and detach.
 *
 * Copyright (C) 2012 Texas Instruments Incorporated - http://www.ti.com/
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
#include "msp430/debug.hpp"

namespace msp430 {

namespace {

/* Device registers (F5xx) */
constexpr uint16_t kWdtCtl = 0x015C;
constexpr uint16_t kWdtHold = 0x5A80;    /* WDTPW | WDTHOLD (DLL WatchdogControl) */
constexpr uint16_t kWdtPw = 0x5A00;
constexpr uint8_t kWdtReadPw = 0x69;
constexpr uint16_t kFctl3 = 0x0144;
constexpr uint16_t kFctl3Busy = 0x0001;
constexpr uint32_t kFlashIdleTimeoutMs = 100;
constexpr uint16_t kSfrIfg1 = 0x0102;
constexpr uint16_t kVmaIfg = 0x0008;
constexpr uint16_t kSafePc = 0x0004;      /* SAFE_PC_ADDRESS (JTAG_defs.h) */
constexpr uint16_t kAltRomAddr = 0x0FF6;  /* ReadCpuRegXv2, altRomAddressForCpuRead */
constexpr uint16_t kResetVector = 0xFFFE;
constexpr uint16_t kSrCpuOff = 0x0010;
constexpr uint16_t kSrKeepMask = 0xFFE7;  /* SR with CPUOFF and GIE cleared */

/* Control signal register bits (JTAG_defs.h) */
constexpr uint16_t kSigHalt = 0x0002;
constexpr uint16_t kSigIntReq = 0x0004;
constexpr uint16_t kSigWait = 0x0008;     /* "Note 805: check if wait is set" */
constexpr uint16_t kSigCpuOff = 0x0020;
constexpr uint16_t kSigCpuSusp = 0x0100;
constexpr uint16_t kSigSync = 0x0200;

/* EEM (EEM_defs.h) */
constexpr uint16_t kEemEn = 0x0001;
constexpr uint16_t kClearStop = 0x0002;
constexpr uint16_t kEmuClkEn = 0x0004;
constexpr uint16_t kEmuFeatEn = 0x0008;
constexpr uint16_t kEemStopped = 0x0080;
constexpr uint8_t kMbTrigVal = 0x00;
constexpr uint8_t kMbTrigCtl = 0x02;
constexpr uint8_t kMbTrigMsk = 0x04;
constexpr uint8_t kMbTrigCmb = 0x06;
constexpr uint8_t kTriggerBlockSize = 0x08;
constexpr uint8_t kBreakReact = 0x80;
constexpr uint8_t kGenClkCtrl = 0x88;
constexpr uint8_t kEemRead = 0x01;
constexpr uint32_t kCmpEqualFetchMab = 0x0000; /* CMP_EQUAL + TRIG_0 + MAB */
constexpr uint32_t kMaskAll = 0xFFFFF;
constexpr uint32_t kNoMask = 0x00000;
constexpr uint32_t kStopClocks = 0x000E;    /* *_SEL0 + STOP_MCLK + STOP_SMCLK + STOP_ACLK */
constexpr uint32_t kJclkStopClocks = 0x6D8E; /* *_SEL3 + STOP_MCLK + STOP_SMCLK + STOP_ACLK */

/* Loop limits, as in MSPDS */
constexpr unsigned kEemStopPolls = 3000;
constexpr unsigned kWaitPolls = 30000;
constexpr unsigned kPipePolls = 10000;
constexpr unsigned kSyncPolls = 50;
constexpr unsigned kStepPolls = 496;
constexpr uint32_t kPorDelayMs = 40;

constexpr uint16_t mova_to_abs(unsigned reg) /* MOVA Rn, &abs */
{
	return static_cast<uint16_t>(0x0060u | ((reg << 8) & 0x0F00u));
}

constexpr uint16_t mova_imm(unsigned reg, uint32_t value) /* MOVA #imm20, Rn */
{
	return static_cast<uint16_t>(0x0080u | ((value >> 8) & 0x0F00u) | (reg & 0x000Fu));
}

} // namespace

/* ---- JTAG helpers (Bios/src/hil/edt.h) ---- */

/* ReadMemWordXv2, edt.h:369-383 */
uint16_t Debugger::read_word(uint32_t addr)
{
	tap_.clr_tclk();
	tap_.ir_shift(kIrAddr16Bit);
	tap_.dr_shift20(addr & 0xFFFFFu);
	tap_.set_tclk();
	tap_.clr_tclk();
	tap_.ir_shift(kIrDataCapture);
	const uint16_t data = tap_.dr_shift16(0);
	tap_.set_tclk();
	tap_.clr_tclk();
	tap_.set_tclk();
	return data;
}

/* WriteMemWordXv2, edt.h:416-432 */
void Debugger::write_word(uint32_t addr, uint16_t data)
{
	tap_.clr_tclk();
	tap_.ir_shift(kIrCntrlSig16Bit);
	tap_.dr_shift16(0x0500);
	tap_.ir_shift(kIrAddr16Bit);
	tap_.dr_shift20(addr);
	tap_.set_tclk();
	tap_.ir_shift(kIrDataToAddr);
	tap_.dr_shift16(data);
	tap_.clr_tclk();
	tap_.ir_shift(kIrCntrlSig16Bit);
	tap_.dr_shift16(0x0501);
	tap_.set_tclk();
	tap_.clr_tclk();
	tap_.set_tclk();
}

/* ReadCpuRegXv2, edt.h:541-589, with altRomAddressForCpuRead set: the CPU
 * executes MOVA Rn, &0x0FF6 and the value is captured from the data bus. */
uint32_t Debugger::read_cpu_reg(uint16_t mova)
{
	tap_.ir_shift(kIrCntrlSigCapture); /* jtagId: selects the JMB address, unused here */
	tap_.clr_tclk();
	tap_.ir_shift(kIrData16Bit);
	tap_.set_tclk();
	tap_.dr_shift16(mova);
	tap_.ir_shift(kIrCntrlSig16Bit);
	tap_.dr_shift16(0x1401);
	tap_.ir_shift(kIrData16Bit);
	tap_.clr_tclk();
	tap_.set_tclk();
	tap_.dr_shift16(kAltRomAddr);
	tap_.clr_tclk();
	tap_.set_tclk();
	tap_.dr_shift16(0x3FFD);
	tap_.clr_tclk();
	tap_.ir_shift(kIrCntrlSig16Bit);
	tap_.dr_shift16(0x0501);
	tap_.ir_shift(kIrDataCapture);
	tap_.set_tclk();
	const uint16_t low = tap_.dr_shift16(0);
	tap_.clr_tclk();
	tap_.set_tclk();
	const uint16_t high = tap_.dr_shift16(0);
	for (int i = 0; i < 3; ++i) {
		tap_.clr_tclk();
		tap_.set_tclk();
	}
	tap_.clr_tclk();
	tap_.ir_shift(kIrDataCapture);
	tap_.set_tclk();
	return (static_cast<uint32_t>(high) << 16) + low;
}

/* WriteCpuRegXv2, edt.h:651-675 */
void Debugger::write_cpu_reg(uint16_t mova, uint16_t data)
{
	tap_.clr_tclk();
	tap_.ir_shift(kIrData16Bit);
	tap_.set_tclk();
	tap_.dr_shift16(mova);
	tap_.ir_shift(kIrCntrlSig16Bit);
	tap_.dr_shift16(0x1401);
	tap_.ir_shift(kIrData16Bit);
	tap_.clr_tclk();
	tap_.set_tclk();
	tap_.dr_shift16(data);
	tap_.clr_tclk();
	tap_.set_tclk();
	tap_.dr_shift16(0x3FFD);
	tap_.clr_tclk();
	tap_.set_tclk();
	tap_.clr_tclk();
	tap_.ir_shift(kIrAddrCapture);
	tap_.dr_shift20(0x00000);
	tap_.set_tclk();
	tap_.ir_shift(kIrCntrlSig16Bit);
	tap_.dr_shift16(0x0501);
	tap_.clr_tclk();
	tap_.set_tclk();
	tap_.clr_tclk();
	tap_.ir_shift(kIrDataCapture);
	tap_.set_tclk();
}

/* SetPcXv2(0x0080 | (pc >> 8 & 0x0F00), pc & 0xFFFF), edt.h:728-749 */
void Debugger::set_pc(uint32_t pc)
{
	tap_.ir_shift(kIrCntrlSigCapture);
	tap_.dr_shift16(0x0000);
	tap_.clr_tclk();
	tap_.ir_shift(kIrData16Bit);
	tap_.set_tclk();
	tap_.dr_shift16(mova_imm(0, pc));
	tap_.clr_tclk();
	tap_.ir_shift(kIrCntrlSig16Bit);
	tap_.dr_shift16(0x1400);
	tap_.ir_shift(kIrData16Bit);
	tap_.clr_tclk();
	tap_.set_tclk();
	tap_.dr_shift16(static_cast<uint16_t>(pc & 0xFFFFu));
	tap_.clr_tclk();
	tap_.set_tclk();
	tap_.dr_shift16(0x4303);
	tap_.clr_tclk();
	tap_.ir_shift(kIrAddrCapture);
	tap_.dr_shift20(0x00000);
}

/* SyncJtagXv2, edt.h:804-816. MSPDS ignores a sync that never comes. */
bool Debugger::sync_jtag()
{
	tap_.ir_shift(kIrCntrlSig16Bit);
	tap_.dr_shift16(0x1501);
	if (tap_.ir_shift(kIrCntrlSigCapture) != kJtagId91) {
		return false;
	}
	for (unsigned i = 0; i < kSyncPolls; ++i) {
		const uint16_t sig = tap_.dr_shift16(0x0000);
		if (sig != 0xFFFF && (sig & kSigSync) != 0) {
			return true;
		}
	}
	return false;
}

/* wait_for_synch, edt.h:819-830 (50 polls; MSPDS reads up to 51 times but
 * fails on the 51st anyway). MSPDS discards the JTAG ID of the capture. */
bool Debugger::wait_for_sync()
{
	if (tap_.ir_shift(kIrCntrlSigCapture) != kJtagId91) {
		return false;
	}
	for (unsigned i = 0; i < kSyncPolls; ++i) {
		if ((tap_.dr_shift16(0) & kSigSync) != 0) {
			return true;
		}
	}
	return false;
}

/* SyncJtag_AssertPor_SaveContextXv2.c:198-201: switch the system clocks back
 * to their own sources but keep them stopped while the CPU is halted. */
void Debugger::restore_system_clocks()
{
	tap_.ir_shift(kIrEmexDataExchange32);
	eem_write(kGenClkCtrl, kStopClocks);
}

/* eem_read_control() + SetReg_16Bits(0) */
uint16_t Debugger::read_control()
{
	tap_.ir_shift(kIrEmexReadControl);
	return tap_.dr_shift16(0x0000);
}

/* eem_write_control() + SetReg_16Bits(value) */
void Debugger::write_control(uint16_t value)
{
	tap_.ir_shift(kIrEmexWriteControl);
	tap_.dr_shift16(value);
}

/* EemDataExchangeXv2.c:66-86, one read; IR_EMEX_DATA_EXCHANGE32 must be
 * selected. */
uint32_t Debugger::eem_read(uint8_t reg)
{
	tap_.dr_shift32(static_cast<uint32_t>(reg | kEemRead));
	return tap_.dr_shift32(0);
}

/* EemDataExchangeXv2.c:88-106, one write */
void Debugger::eem_write(uint8_t reg, uint32_t value)
{
	tap_.dr_shift32(reg);
	tap_.dr_shift32(value);
}

/* ---- context save and restore ---- */

/* Not in MSPDS: release a CPU that has not reached full emulation, without
 * writing anything to the target's memory, and let go of the pins. EEM
 * registers only: the EEM is turned off (and on the reset path the caller
 * has first switched the system clocks back, restore_system_clocks()). */
void Debugger::abandon()
{
	write_control(kEmuClkEn | kClearStop);
	tap_.ir_shift(kIrCntrlSigRelease);
	tap_.close();
	attached_ = false;
	running_ = false;
}

/* Not in MSPDS: release a CPU in full emulation whose context was not saved.
 * SR and WDTCTL have not been written, so only the PC is restored, with the
 * same low-power correction as a normal release (here from the control
 * signal's CPUOFF, as SR was not read); EEM off. Lets go of the pins. */
void Debugger::release_unsaved(uint32_t pc, bool cpuoff)
{
	release_at(pc, cpuoff, kEmuClkEn | kClearStop);
	tap_.close();
	attached_ = false;
	running_ = false;
}

/* EEM off, then ReleaseDevice_430Xv2(V_RESET) (Cpuxv2::disconnect): a
 * power-on reset, after which the CPU starts at its reset vector. Lets go of
 * the pins. */
void Debugger::release_with_reset()
{
	write_control(kEmuClkEn | kClearStop);
	cpu_.disconnect();
	attached_ = false;
	running_ = false;
}

/* Not in MSPDS (see the file header). */
void Debugger::wait_flash_idle()
{
	const uint32_t start = tap_.millis();
	while ((read_word(kFctl3) & kFctl3Busy) != 0) {
		if (tap_.millis() - start >= kFlashIdleTimeoutMs) {
			break;
		}
	}
}

/* SyncJtag_Conditional_SaveContextXv2.c:302-308 /
 * SyncJtag_AssertPor_SaveContextXv2.c:154-159: hold the watchdog, keep its
 * other bits. */
bool Debugger::hold_watchdog()
{
	const uint16_t wdt = read_word(kWdtCtl);
	if ((wdt >> 8) != kWdtReadPw) {
		return false;
	}
	write_word(kWdtCtl, static_cast<uint16_t>(kWdtHold | (wdt & 0x00FFu)));
	ctx_.wdtctl = wdt;
	return true;
}

/* SyncJtag_Conditional_SaveContextXv2.c:310-324: save SR, then clear CPUOFF
 * and GIE in the CPU so that it executes JTAG-fed instructions. */
void Debugger::save_status_register()
{
	ctx_.sr = static_cast<uint16_t>(read_cpu_reg(mova_to_abs(2)));
	write_cpu_reg(mova_imm(2, 0), static_cast<uint16_t>(ctx_.sr & kSrKeepMask));
}

/* SyncJtag_Conditional_SaveContextXv2.c:70-329 */
Status Debugger::sync_conditional()
{
	/* DisableLpmx5(): no LPMx.5 test-register settings on the F5529. */
	if ((read_control() & kEemStopped) == 0) {
		tap_.ir_shift(kIrCntrlSigCapture);
		if ((tap_.dr_shift16(0x0000) & kSigCpuOff) == 0) {
			/* Not in a low-power mode: let the EEM stop the CPU on the
			 * next instruction fetch, using trigger block 0. */
			tap_.ir_shift(kIrEmexDataExchange32);
			const uint32_t tb_val = eem_read(kMbTrigVal);
			const uint32_t tb_ctl = eem_read(kMbTrigCtl);
			const uint32_t tb_msk = eem_read(kMbTrigMsk);
			const uint32_t tb_cmb = eem_read(kMbTrigCmb);
			const uint32_t tb_break = eem_read(kBreakReact);
			eem_write(kMbTrigCtl, kCmpEqualFetchMab);
			eem_write(kMbTrigMsk, kMaskAll);
			eem_write(kMbTrigCmb, 0x0001); /* EN0 */
			eem_write(kBreakReact, 0x0001);
			eem_write(kGenClkCtrl, kStopClocks);
			write_control(kEmuClkEn | kClearStop | kEemEn);
			for (unsigned i = 0; i < kEemStopPolls; ++i) {
				if ((read_control() & kEemStopped) != 0) {
					break;
				}
			}
			tap_.ir_shift(kIrEmexDataExchange32);
			eem_write(kMbTrigVal, tb_val);
			eem_write(kMbTrigCtl, tb_ctl);
			eem_write(kMbTrigMsk, tb_msk);
			eem_write(kMbTrigCmb, tb_cmb);
			eem_write(kBreakReact, tb_break);
		}
	}

	write_control(kEmuClkEn | kEemEn); /* clock control before sync */
	if (!sync_jtag()) {
		abandon();
		return Status::TargetError;
	}
	/* reset the CPU stop reaction: the CPU is under JTAG control now */
	tap_.ir_shift(kIrEmexWriteControl);
	tap_.dr_shift16(kEmuClkEn | kClearStop | kEemEn);
	tap_.dr_shift16(kEmuClkEn | kClearStop);

	tap_.ir_shift(kIrCntrlSig16Bit);
	tap_.dr_shift16(0x1501);
	tap_.ir_shift(kIrCntrlSigCapture);
	uint16_t sig = tap_.dr_shift16(0);
	for (unsigned n = 0; (sig & kSigWait) != 0 && n < kWaitPolls; ++n) {
		tap_.clr_tclk();
		tap_.set_tclk();
		tap_.ir_shift(kIrCntrlSigCapture);
		sig = tap_.dr_shift16(0);
	}

	/* clock the CPU into full emulation state: wait for CPUSUSP */
	tap_.ir_shift(kIrCntrlSigCapture);
	bool pipe_empty = false;
	for (unsigned n = 0; !pipe_empty && n < kPipePolls; ++n) {
		tap_.clr_tclk();
		pipe_empty = (tap_.dr_shift16(0x0000) & kSigCpuSusp) != 0;
		tap_.set_tclk();
	}
	if (!pipe_empty) {
		abandon();
		return Status::TargetError;
	}
	tap_.ir_shift(kIrCntrlSig16Bit);
	tap_.dr_shift16(0x0501);

	/* i_SaveContext */
	tap_.clr_tclk();
	tap_.set_tclk();
	tap_.ir_shift(kIrAddrCapture);
	const uint32_t mab = tap_.dr_shift20(0);
	tap_.ir_shift(kIrCntrlSigCapture);
	sig = tap_.dr_shift16(0);
	const bool cpuoff = (sig & kSigCpuOff) != 0;
	ctx_.irq = static_cast<uint16_t>(sig & kSigIntReq);
	uint32_t pc = (mab - (cpuoff ? 2u : 4u)) & 0xFFFFFu;
	if (pc == 0x0FFFEu || pc == 0xFFFFEu) { /* Note 1495, 1637; see the file header */
		pc = read_word(kResetVector);
	}

	write_control(kEmuFeatEn | kEmuClkEn | kClearStop);
	if (!cpu_.in_full_emulation()) { /* MSPDS: "check for Init State", unchecked */
		release_unsaved(pc, cpuoff);
		return Status::TargetError;
	}
	if (!hold_watchdog()) {
		release_unsaved(pc, cpuoff);
		return Status::TargetError;
	}
	wait_flash_idle(); /* after the hold: the watchdog cannot bite meanwhile */
	ctx_.pc = pc;
	save_status_register();
	return Status::Ok;
}

/* SyncJtag_AssertPor_SaveContextXv2.c:68-228, JTAG ID 0x91 */
Status Debugger::sync_assert_por()
{
	if (tap_.ir_shift(kIrCntrlSigCapture) != kJtagId91) {
		abandon();
		return Status::TargetError;
	}
	/* switch all functional clocks to JCK = 1 and stop them */
	tap_.ir_shift(kIrEmexDataExchange32);
	eem_write(kGenClkCtrl, kJclkStopClocks);
	write_control(kEmuClkEn | kEemEn);
	tap_.ir_shift(kIrCntrlSig16Bit);
	tap_.dr_shift16(0x1501);
	if (!wait_for_sync()) {
		restore_system_clocks(); /* as MSPDS leaves them on success */
		abandon();
		return Status::TargetError;
	}
	tap_.clr_tclk(); /* one more clock to empty the pipe */
	tap_.set_tclk();
	tap_.ir_shift(kIrCntrlSig16Bit);
	tap_.dr_shift16(0x0C01); /* release CPUFLUSH, apply POR */
	tap_.delay_ms(kPorDelayMs);
	tap_.dr_shift16(0x0401); /* release POR */
	/* force the PC to a safe address (JMP $) */
	tap_.ir_shift(kIrData16Bit);
	tap_.clr_tclk();
	tap_.set_tclk();
	tap_.clr_tclk();
	tap_.set_tclk();
	tap_.dr_shift16(kSafePc);
	tap_.clr_tclk();
	tap_.set_tclk();
	tap_.clr_tclk(); /* JTAG ID 0x91: one more */
	tap_.set_tclk();
	tap_.ir_shift(kIrDataCapture);
	/* two more to release the CPU-internal POR delay signals */
	tap_.clr_tclk();
	tap_.set_tclk();
	tap_.clr_tclk();
	tap_.set_tclk();
	tap_.ir_shift(kIrCntrlSig16Bit);
	tap_.dr_shift16(0x0501); /* set CPUFLUSH */
	tap_.clr_tclk();
	tap_.set_tclk();
	write_control(kEmuFeatEn | kEmuClkEn | kClearStop);
	/* "check that sequence exits on Init State" (unchecked in MSPDS), then
	 * hold the watchdog. On failure the POR has left the CPU at SAFE_PC, or
	 * (failed init-state check) its state is unknown: it is not let go
	 * there but, its clocks switched back, reset again; see the file
	 * header. No flash wait: after the POR no flash operation runs. */
	if (!cpu_.in_full_emulation() || !hold_watchdog()) {
		restore_system_clocks();
		release_with_reset();
		return Status::TargetError;
	}
	tap_.ir_shift(kIrAddrCapture); /* MAB; JTAG ID 0x91 takes the reset vector */
	tap_.dr_shift20(0);
	ctx_.pc = read_word(kResetVector);
	ctx_.sr = 0; /* always 0 after a POR */
	ctx_.irq = 0;
	/* module clock control (ETKEYSEL/ETCLKSEL): not ported, see the header */
	restore_system_clocks();
	/* JTAG ID 0x91: reset the vacant-memory flag in SFRIFG1 */
	const uint16_t sfr = read_word(kSfrIfg1);
	if ((sfr & kVmaIfg) != 0) {
		set_pc(kSafePc);
		tap_.ir_shift(kIrCntrlSig16Bit);
		tap_.dr_shift16(0x0501);
		tap_.set_tclk();
		tap_.ir_shift(kIrAddrCapture);
		write_word(kSfrIfg1, static_cast<uint16_t>(sfr & ~kVmaIfg));
	}
	return Status::Ok;
}

/* RestoreContext_ReleaseJtagXv2.c:69-196 with mdb = 0 (no software
 * breakpoints), releaseJtag = 0 and ulpDebug = 0. */
void Debugger::restore_and_release(uint16_t eem_control)
{
	write_cpu_reg(mova_imm(2, 0), ctx_.sr);
	write_word(kWdtCtl, static_cast<uint16_t>(kWdtPw | (ctx_.wdtctl & 0x00FFu)));
	release_at(ctx_.pc, (ctx_.sr & kSrCpuOff) != 0, eem_control);
}

/* RestoreContext_ReleaseJtagXv2.c:98-189, the part after the SR and WDTCTL
 * writes, with mdb = 0 and ulpDebug = 0: set the PC and release the CPU.
 * `cpuoff`: the CPU is in a low-power mode (SR.CPUOFF in the macro). */
void Debugger::release_at(uint32_t pc, bool cpuoff, uint16_t eem_control)
{
	if (cpuoff) { /* Note 1411 */
		pc = (pc - 2u) & 0xFFFFFu;
	}
	set_pc(pc);
	tap_.set_tclk();
	tap_.ir_shift(kIrCntrlSig16Bit);
	tap_.dr_shift16(0x0401);
	tap_.ir_shift(kIrAddrCapture);
	/* one more clock to advance the CPU, or enabling the EEM stops it again */
	tap_.ir_shift(kIrCntrlSigCapture);
	tap_.clr_tclk();
	if ((tap_.dr_shift16(0) & kSigHalt) != 0) {
		tap_.ir_shift(kIrCntrlSig16Bit);
		tap_.dr_shift16(0x0403);
	}
	tap_.set_tclk();
	write_control(eem_control);
	tap_.ir_shift(kIrCntrlSigCapture); /* id; DisableLpmx5() does nothing here */
	tap_.ir_shift(kIrCntrlSigRelease);
}

void Debugger::clear_triggers()
{
	tap_.ir_shift(kIrEmexDataExchange32);
	for (unsigned i = 0; i < kHwBreakpoints; ++i) {
		eem_write(static_cast<uint8_t>(i * kTriggerBlockSize + kMbTrigCmb), 0);
	}
	eem_write(kBreakReact, 0);
	bp_used_ = 0;
}

/* ---- public operations ---- */

Status Debugger::attach(AttachMode mode, uint32_t slot_hz, Identity &id)
{
	detach(DetachAction::Run);
	const Status st = cpu_.identify(EntryMode::Normal, slot_hz, id);
	if (st != Status::Ok) {
		return st;
	}
	attached_ = true;
	running_ = false;
	const Status sync = (mode == AttachMode::Reset) ? sync_assert_por() : sync_conditional();
	if (sync != Status::Ok) {
		return sync;
	}
	id.device_id = read_word(id.device_id_ptr + 4u);
	clear_triggers();
	if (!cpu_.in_full_emulation()) {
		detach(DetachAction::Run);
		return Status::TargetError;
	}
	reason_ = (mode == AttachMode::Reset) ? StopReason::Reset : StopReason::Request;
	bp_hit_ = kNoBreakpoint;
	return Status::Ok;
}

void Debugger::detach(DetachAction action)
{
	if (!attached_) {
		return;
	}
	clear_triggers();
	if (action == DetachAction::Reset) {
		release_with_reset();
	} else {
		restore_and_release(kEmuClkEn | kClearStop); /* DLL run(): EEM off */
		tap_.close();
	}
	attached_ = false;
	running_ = false;
	reason_ = StopReason::None;
}

/* i_ReadJmbOut, Bios/src/hil/edt.c:44-57 */
void Debugger::clear_jmb_out()
{
	tap_.ir_shift(kIrJmbExchange);
	if ((tap_.dr_shift16(0x0000) & kJmbOut1Rdy) != 0) {
		tap_.dr_shift16(kJmb32Bit | kJmbOutReq);
		tap_.dr_shift16(0);
		tap_.dr_shift16(0);
	}
}

/* ReadAllCpuRegsXv2.c:58-91, JTAG ID 0x91 */
Status Debugger::read_regs(uint32_t regs[16])
{
	if (!attached_) {
		return Status::BadState;
	}
	if (running_) {
		return Status::Running;
	}
	regs[0] = ctx_.pc;
	regs[2] = ctx_.sr;
	regs[3] = 0;
	tap_.ir_shift(kIrCntrlSigCapture); /* id: 0x91 takes the safe-PC step below */
	for (unsigned r = 1; r < 16; ++r) {
		if (r == 2) {
			r += 2;
		}
		regs[r] = read_cpu_reg(mova_to_abs(r)) & 0xFFFFFu;
		/* JTAG ID 0x91: set the PC to a safe address */
		set_pc(kSafePc);
		tap_.ir_shift(kIrCntrlSig16Bit);
		tap_.dr_shift16(0x0501);
		tap_.set_tclk();
		tap_.ir_shift(kIrAddrCapture);
	}
	clear_jmb_out();
	return cpu_.in_full_emulation() ? Status::Ok : Status::TargetError;
}

/* WriteAllCpuRegsXv2.c:60-85, for one register */
Status Debugger::write_reg(unsigned index, uint32_t value)
{
	if (!attached_) {
		return Status::BadState;
	}
	if (running_) {
		return Status::Running;
	}
	if (index > 15 || index == 3 || value >= kAddressSpace ||
	    (index == 0 && (value & 1u) != 0) || (index == 2 && value > 0xFFFFu)) {
		return Status::BadArg;
	}
	if (index == 0) {
		ctx_.pc = value;
		return Status::Ok;
	}
	if (index == 2) {
		ctx_.sr = static_cast<uint16_t>(value);
		return Status::Ok;
	}
	write_cpu_reg(mova_imm(index, value), static_cast<uint16_t>(value));
	return cpu_.in_full_emulation() ? Status::Ok : Status::TargetError;
}

} // namespace msp430
