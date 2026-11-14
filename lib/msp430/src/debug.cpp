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
 *    which also checks the JTAG ID; read_mem, write_mem, read_regs,
 *    write_reg, set_breakpoint and clear_breakpoint end with the same check
 *    (EemDataExchangeXv2 checks nothing). write_mem also makes it once
 *    before its first write and writes nothing if it fails, but not before
 *    each word as Phase A's Cpuxv2::write_word does (WriteMem_430Xv2 with
 *    the check in front): a link that dies during the transfer shows in the
 *    check after the last word. ReadMemWordsXv2 and WriteMemWordsXv2 check
 *    nothing;
 *  - read_mem sets the CPU to read (control signal 0x0501) before the
 *    first address, as SLAU320's ReadMem_430Xv2 and Phase A's
 *    Cpuxv2::read_mem do; ReadMemWordsXv2 relies on the operation before
 *    it having left the CPU in read mode, and in write mode its bus cycles
 *    could be writes;
 *  - read_mem and write_mem refuse an odd address, an address above 20 bits
 *    or words past 0xFFFFF with BadArg before any TAP traffic, and do
 *    nothing for no words. The macros take any address and wrap at 20 bits
 *    (ReadMemWordsXv2 also clocks TCLK for no words); the DLL widens an odd
 *    start or end to whole words (RegisterAccessBase.cpp:104-121);
 *  - read_mem and write_mem keep WDTCTL (0x015C) in step with the saved
 *    context as TI's DLL does (RegisterAccessBase.cpp:67-79 and 93-146,
 *    WatchdogControl.cpp), not the macros: read_mem still reads the word
 *    on the bus but returns 0x69 and the saved low byte, the value the
 *    program gets back on release (the DLL patches only the low byte);
 *    write_mem saves the written low byte for the release and writes it
 *    with the password 0x5A and the hold bit, so the watchdog stays held
 *    while the CPU is halted and a written high byte without the password
 *    causes no PUC;
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
 *  - before the CPU is let go with anything fed to it, the init-state check
 *    (Cpuxv2::in_full_emulation(), JTAG ID 0x91 and a control-signal bit of
 *    0x0301: a link check) is made once more: in detach(Run) before the
 *    context (SR, WDTCTL, PC) is restored, and before the release at the PC
 *    after a failed sync (above). If it fails, nothing is fed over the
 *    link: EEM control 0x0006, IR_CNTRL_SIG_RELEASE and the pins released,
 *    nothing written to the target's memory (abandon(), as for a lost
 *    target below). Session::abort ends a debug session with detach(Run)
 *    after a memory, register or breakpoint access returned TargetError, so
 *    the link that failed there gets no restore fed over it. MSPDS and the
 *    DLL check nothing before RestoreContext_ReleaseJtagXv2;
 *  - detach(Run) of a CPU that is already halted disarms every trigger
 *    block and turns the sequencer off, restores SR, WDTCTL and the PC and
 *    releases the CPU with EEM control 0x0006, as the DLL's run() does
 *    without breakpoints to poll (DebugManagerMSP430.cpp:291). The DLL's
 *    Close does that only for a CPU that was running (State with stop, then
 *    Run(FREE_RUN), DLL430_OldApiV3.cpp:556-567); for a halted one it only
 *    moves the PC to a safe place, 0x0004 with SR 0
 *    (DLL430_OldApiV3.cpp:568-574, DebugManagerMSP430::setPCtoSafeLocation,
 *    DebugManagerMSP430.cpp:873-891), before it stops JTAG;
 *  - after the reset-and-halt sync the interrupt-request flag that step()
 *    passes on as SingleStepXv2's irRequest is 0, as SR is. The DLL keeps
 *    the value of its last conditional sync or step
 *    (DebugManagerMSP430.cpp:622, 739): its reset
 *    (DeviceHandleMSP430::reset, DeviceHandleMSP430.cpp:296-301) takes only
 *    PC and SR from SyncJtag_AssertPor_SaveContextXv2;
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
 *  - registers: read_regs captures the JTAG ID as ReadAllCpuRegsXv2 does
 *    but does not use it; it always takes the safe-PC step, which MSPDS
 *    takes for 0x91, 0x98 and 0x99 only. read_regs and write_reg end with
 *    the init-state check (above), which also checks the ID; MSPDS checks
 *    nothing there. read_regs keeps each value to 20 bits; the macro passes
 *    24 bits of the capture on. write_reg writes one register (the macro
 *    writes R1 and R4-R15 together) and refuses a value above 20 bits, an
 *    odd PC or an SR above 16 bits with BadArg; the macro drops bits 20-23.
 *    It clears bit 0 of SP, which the DLL does (CpuMemoryAccess::write)
 *    and the macro does not;
 *  - SingleStepXv2 reads the EEM control register up to 496 times as MSPDS
 *    does: its counter starts at 4 and ends the poll at 500, and its
 *    `i < 500` fails then even if that last read showed the stop, as here.
 *    Without the stop MSPDS returns an error and leaves the CPU running
 *    with trigger block 0 set up for the step; here
 *    SyncJtag_Conditional_SaveContextXv2 stops the CPU as halt() does (at
 *    once if the instruction put it to sleep), trigger block 0 is restored
 *    and Timeout returned (design spec item 17). MSPDS restores trigger
 *    block 0 after the sync whatever the sync returned; here a failed sync
 *    lets the CPU go (above), so trigger block 0 goes back as part of that
 *    release, with the EEM off. After a release without a trigger (CPU
 *    off, no interrupt pending) MSPDS takes a JTAG ID it does not know for
 *    LPMx.5 and returns success; here an ID other than 0x91 ends the
 *    session with TargetError as in poll() (below);
 *  - breakpoints: one trigger block per breakpoint, set up as the trigger
 *    SyncJtag_Conditional_SaveContextXv2 uses for "next instruction fetch"
 *    (lines 130-137), with the exact address and no mask, as the DLL's code
 *    breakpoint (DLL430_OldApiV3.cpp:2536, AddressCondition430,
 *    Trigger430::setMask). Combination i holds trigger i alone; the DLL
 *    assigns combinations from 0 up (TriggerConfigurator430). The value
 *    register is read back to confirm that the trigger block exists; if it
 *    does not read back, an attempt is made to disarm the block and
 *    TargetError is returned.
 *    Clearing writes the combination register and BREAKREACT only; the DLL
 *    also zeroes value, control and mask (Trigger430::reset). The DLL
 *    writes EEM registers through a cache (EemRegisterAccess430.cpp):
 *    CCNT1_REACT, STOR_REACT and the sequencer control SEQ_CTL go out in
 *    full only at its first configuration, EEM_Init's writeConfiguration
 *    (TriggerManager430::writeTriggerReactions, Sequencer430), then only on
 *    change. Here CCNT1_REACT and STOR_REACT are left as found (neither
 *    stops the CPU); SEQ_CTL is written 0 whenever all trigger blocks are
 *    cleared (attach, detach, clear_breakpoint(kAllBreakpoints)): on the
 *    F5529 (TriggerManager430(8, 2, 10, 7)) the sequencer's output is
 *    reaction bit 7, which a sequencer left on by an earlier session would
 *    drive in place of breakpoint 7. Breakpoints change only while the CPU
 *    is halted (the DLL also changes hardware breakpoints of a running
 *    CPU), and the index and address are checked (BadArg; the DLL checks
 *    neither);
 *  - stop detection: WaitForEem checks the JTAG ID (jtagIdIsValid, lines
 *    84-87) and, if it is not valid, reports no event; the DLL polls on.
 *    poll() takes an ID other than 0x91 for a lost target (a reset or a
 *    power loss ends JTAG access) and ends the session with TargetError: an
 *    attempt to turn the EEM off (EEM control 0x0006), IR_CNTRL_SIG_RELEASE,
 *    the pins released, nothing written to the target's memory. halt() and
 *    detach() of a running CPU go through poll() and do the same;
 *  - halt() makes WaitForEem's check before
 *    SyncJtag_Conditional_SaveContextXv2, so that a CPU the EEM has already
 *    stopped is reported as stopped at a breakpoint. The DLL's stop
 *    (DLL430_OldApiV3::State with stop, DebugManagerMSP430::stop) runs the
 *    macro alone, whose own look at EEM_STOPPED then skips the stop request
 *    as here. After a breakpoint stop, the breakpoint whose address is the
 *    stop PC is named; the DLL reports only that one was hit;
 *  - eem_peek() (DBG_EEM_PEEK, design spec correction 25) is no MSPDS
 *    macro: identify(), then EemDataExchangeXv2's read of one trigger-block
 *    register, twice, with the JTAG ID checked before and after, and no
 *    sync; a read-only check of the 32-bit scans.
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
constexpr uint8_t kSeqCtl = 0xA6; /* the DLL's sequencer control (Sequencer430.cpp), 0 = off */
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

/* Not in MSPDS (see the file header): an even address and `nwords` words
 * that end at or below 0xFFFFF. */
constexpr bool word_range_ok(uint32_t addr, size_t nwords)
{
	return (addr & 1u) == 0 && addr < kAddressSpace && nwords <= (kAddressSpace - addr) / 2u;
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

/* EemDataExchangeXv2.c:83-84, one read; IR_EMEX_DATA_EXCHANGE32 must be
 * selected (line 70). */
uint32_t Debugger::eem_read(uint8_t reg)
{
	tap_.dr_shift32(static_cast<uint32_t>(reg | kEemRead));
	return tap_.dr_shift32(0);
}

/* EemDataExchangeXv2.c:104-105, one write; IR_EMEX_DATA_EXCHANGE32 must be
 * selected (line 70). */
void Debugger::eem_write(uint8_t reg, uint32_t value)
{
	tap_.dr_shift32(reg);
	tap_.dr_shift32(value);
}

/* ---- context save and restore ---- */

/* Not in MSPDS: release a CPU that is not in full emulation (not yet
 * synchronized, or running when its JTAG ID was lost), or one whose link
 * check failed just before a release, without feeding it anything or
 * writing anything to the target's memory, and let go of the pins. EEM
 * registers only: the EEM is turned off, then trigger block 0 goes back if
 * step() has borrowed it (once the reset path has switched the system
 * clocks to JCK, the caller switches them back first,
 * restore_system_clocks()). */
void Debugger::abandon()
{
	write_control(kEmuClkEn | kClearStop);
	give_back_trigger_block_0();
	tap_.ir_shift(kIrCntrlSigRelease);
	tap_.close();
	attached_ = false;
	running_ = false;
}

/* Not in MSPDS: release a CPU in full emulation whose context was not saved.
 * SR and WDTCTL have not been written, so only the PC is restored, with the
 * same low-power correction as a normal release (here from the control
 * signal's CPUOFF, as SR was not read); EEM off. Trigger block 0 goes back
 * first if step() has borrowed it (the sync has turned the EEM off).
 * Lets go of the pins. The link check comes first: if it fails, nothing is
 * fed over the link and the CPU is let go as abandon() does. */
void Debugger::release_unsaved(uint32_t pc, bool cpuoff)
{
	if (!cpu_.in_full_emulation()) {
		abandon();
		return;
	}
	give_back_trigger_block_0();
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

/* Every trigger block disarmed, then the sequencer off as the DLL's EEM_Init
 * leaves it (see the header). */
void Debugger::clear_triggers()
{
	tap_.ir_shift(kIrEmexDataExchange32);
	for (unsigned i = 0; i < kHwBreakpoints; ++i) {
		eem_write(static_cast<uint8_t>(i * kTriggerBlockSize + kMbTrigCmb), 0);
	}
	eem_write(kBreakReact, 0);
	eem_write(kSeqCtl, 0);
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

/* A running CPU is stopped first and then let go with its context restored,
 * as the DLL's Close does (DLL430_OldApiV3.cpp:556-567: State with stop,
 * then Run(FREE_RUN), EEM control 0x0006). A halted one is let go the same
 * way, and every trigger is disarmed; neither is in the DLL's Close (see the
 * header). Before the restore, the link check (not in MSPDS, see the
 * header): after a TargetError the link may have failed. */
void Debugger::detach(DetachAction action)
{
	if (!attached_) {
		return;
	}
	if (running_ && halt() != Status::Ok) {
		return; /* halt() failed and has let the CPU go */
	}
	clear_triggers();
	if (action == DetachAction::Reset) {
		release_with_reset();
	} else if (!cpu_.in_full_emulation()) {
		abandon(); /* nothing fed over a failed link */
	} else {
		/* EEM off (0x0006), as the DLL's run() without breakpoints to poll,
		 * DebugManagerMSP430.cpp:291 */
		restore_and_release(kEmuClkEn | kClearStop);
		tap_.close();
	}
	attached_ = false;
	running_ = false;
	reason_ = StopReason::None;
}

/* Not in MSPDS (design spec correction 25): the kind of EEM read that
 * SyncJtag_Conditional_SaveContextXv2 makes first when it attaches to a
 * running CPU (lines 112-124: trigger block 0's value, control, mask and
 * combination registers, after the same entry and identify), on one register
 * and alone, with the JTAG ID checked before and after the reads. For the
 * addresses allowed here EemDataExchangeXv2 has no special case (only
 * MODCLKCTRL0, 0x8A, is answered from a cache). */
Status Debugger::eem_peek(uint8_t reg, uint32_t slot_hz, Identity &id, uint32_t values[2])
{
	values[0] = 0;
	values[1] = 0;
	if (attached_) {
		return Status::BadState;
	}
	if ((reg & 1u) != 0 || reg > kEemPeekLastAddr) {
		return Status::BadArg;
	}
	const Status st = cpu_.identify(EntryMode::Normal, slot_hz, id);
	if (st != Status::Ok) {
		return st; /* identify() has released the pins */
	}
	/* EemDataExchangeXv2.c:70, then lines 83-84 once per read */
	bool link_ok = tap_.ir_shift(kIrEmexDataExchange32) == kJtagId91;
	if (link_ok) {
		const uint32_t first = eem_read(reg);
		const uint32_t second = eem_read(reg);
		link_ok = tap_.ir_shift(kIrCntrlSigCapture) == kJtagId91;
		if (link_ok) {
			values[0] = first;
			values[1] = second;
		}
	}
	tap_.close();
	return link_ok ? Status::Ok : Status::TargetError;
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
	/* id, unchecked: MSPDS takes the safe-PC step for 0x91/0x98/0x99; the
	 * final in_full_emulation() checks the ID. */
	tap_.ir_shift(kIrCntrlSigCapture);
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
	/* With the alternate ROM address nothing was moved to the mailbox;
	 * MSPDS clears it anyway, which discards a 32-bit message the firmware
	 * posted. */
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
	if (index == 1) { /* as the DLL does, CpuMemoryAccess.cpp:106-110 */
		value &= ~1u;
	}
	write_cpu_reg(mova_imm(index, value), static_cast<uint16_t>(value));
	return cpu_.in_full_emulation() ? Status::Ok : Status::TargetError;
}

/* ReadMemWordsXv2.c:57-83. No SetPcXv2: the saved PC stays valid. The
 * read mode, range checks and WDTCTL are not in MSPDS (see the header). */
Status Debugger::read_mem(uint32_t addr, size_t nwords, uint8_t *dst)
{
	if (!attached_) {
		return Status::BadState;
	}
	if (running_) {
		return Status::Running;
	}
	if (!word_range_ok(addr, nwords)) {
		return Status::BadArg;
	}
	if (nwords == 0) {
		return Status::Ok;
	}
	tap_.clr_tclk();
	tap_.ir_shift(kIrCntrlSig16Bit); /* read mode; not in MSPDS, see the header */
	tap_.dr_shift16(0x0501);
	for (size_t i = 0; i < nwords; ++i) {
		const uint32_t a = addr + 2u * static_cast<uint32_t>(i);
		tap_.ir_shift(kIrAddr16Bit);
		tap_.dr_shift20(a);
		tap_.set_tclk();
		tap_.clr_tclk();
		tap_.ir_shift(kIrDataCapture);
		uint16_t word = tap_.dr_shift16(0);
		if (a == kWdtCtl) { /* the DLL, RegisterAccessBase.cpp:67-79 */
			word = static_cast<uint16_t>((kWdtReadPw << 8) | (ctx_.wdtctl & 0x00FFu));
		}
		dst[2 * i] = static_cast<uint8_t>(word);
		dst[2 * i + 1] = static_cast<uint8_t>(word >> 8);
	}
	tap_.set_tclk();
	tap_.clr_tclk(); /* one more cycle, so the CPU drives the correct MAB */
	tap_.set_tclk();
	return cpu_.in_full_emulation() ? Status::Ok : Status::TargetError;
}

/* WriteMemWordsXv2.c:58-104; its loop body is WriteMemWordXv2. The range
 * checks, the check before the first write and WDTCTL are not in MSPDS (see
 * the header). */
Status Debugger::write_mem(uint32_t addr, size_t nwords, const uint8_t *src)
{
	if (!attached_) {
		return Status::BadState;
	}
	if (running_) {
		return Status::Running;
	}
	if (!word_range_ok(addr, nwords)) {
		return Status::BadArg;
	}
	if (nwords == 0) {
		return Status::Ok;
	}
	if (!cpu_.in_full_emulation()) {
		return Status::TargetError;
	}
	for (size_t i = 0; i < nwords; ++i) {
		const uint32_t a = addr + 2u * static_cast<uint32_t>(i);
		uint16_t word = static_cast<uint16_t>(src[2 * i] | (src[2 * i + 1] << 8));
		if (a == kWdtCtl) { /* the DLL, RegisterAccessBase.cpp:93-146 */
			ctx_.wdtctl = static_cast<uint16_t>((kWdtReadPw << 8) | (word & 0x00FFu));
			word = static_cast<uint16_t>(kWdtHold | (word & 0x00FFu));
		}
		write_word(a, word);
	}
	return cpu_.in_full_emulation() ? Status::Ok : Status::TargetError;
}

/* Trigger block `index` set up through EemDataExchangeXv2.c:104-105 as
 * SyncJtag_Conditional_SaveContextXv2.c:130-137 sets up trigger block 0 for
 * "next instruction fetch", with the exact address (mask none, as the DLL's
 * code breakpoint); combination `index` holds trigger `index` alone. The
 * read-back and the disarming on its failure are not in MSPDS (see the
 * header). */
Status Debugger::set_breakpoint(unsigned index, uint32_t addr)
{
	if (!attached_) {
		return Status::BadState;
	}
	if (running_) {
		return Status::Running;
	}
	if (index >= kHwBreakpoints || addr >= kAddressSpace || (addr & 1u) != 0) {
		return Status::BadArg;
	}
	const uint8_t tb = static_cast<uint8_t>(index * kTriggerBlockSize);
	tap_.ir_shift(kIrEmexDataExchange32);
	eem_write(static_cast<uint8_t>(tb + kMbTrigVal), addr);
	eem_write(static_cast<uint8_t>(tb + kMbTrigCtl), kCmpEqualFetchMab);
	eem_write(static_cast<uint8_t>(tb + kMbTrigMsk), kNoMask);
	eem_write(static_cast<uint8_t>(tb + kMbTrigCmb), 1u << index);
	if ((eem_read(static_cast<uint8_t>(tb + kMbTrigVal)) & 0xFFFFFu) != addr) {
		/* no such trigger block, or a dead link: disarm it, and with it a
		 * breakpoint this index held before */
		bp_used_ &= ~(1u << index);
		eem_write(static_cast<uint8_t>(tb + kMbTrigCmb), 0);
		eem_write(kBreakReact, bp_used_);
		return Status::TargetError;
	}
	bp_addr_[index] = addr;
	bp_used_ |= 1u << index;
	eem_write(kBreakReact, bp_used_);
	/* not in MSPDS (see the header); on failure the breakpoint still counts
	 * as set: it is armed, and detach clears every trigger block */
	return cpu_.in_full_emulation() ? Status::Ok : Status::TargetError;
}

/* The combination register and BREAKREACT only, then the init-state check
 * (see the header). */
Status Debugger::clear_breakpoint(unsigned index)
{
	if (!attached_) {
		return Status::BadState;
	}
	if (running_) {
		return Status::Running;
	}
	if (index == kAllBreakpoints) {
		clear_triggers();
	} else if (index < kHwBreakpoints) {
		bp_used_ &= ~(1u << index);
		tap_.ir_shift(kIrEmexDataExchange32);
		eem_write(static_cast<uint8_t>(index * kTriggerBlockSize + kMbTrigCmb), 0);
		eem_write(kBreakReact, bp_used_);
	} else {
		return Status::BadArg;
	}
	return cpu_.in_full_emulation() ? Status::Ok : Status::TargetError;
}

/* ---- run control ---- */

/* The DLL's stop (DLL430_OldApiV3::State with stop, DebugManagerMSP430::stop
 * and saveContext): SyncJtag_Conditional_SaveContextXv2, which stops the CPU
 * through the EEM. WaitForEem's check first is not in the DLL (see the
 * header). */
Status Debugger::halt()
{
	bool halted = false;
	Status st = poll(halted);
	if (st != Status::Ok || halted) {
		return st;
	}
	st = sync_conditional();
	if (st != Status::Ok) {
		return st; /* the CPU has been let go */
	}
	running_ = false;
	reason_ = StopReason::Request;
	bp_hit_ = kNoBreakpoint;
	return Status::Ok;
}

/* DebugManagerMSP430::run (DebugManagerMSP430.cpp:287-294):
 * RestoreContext_ReleaseJtagXv2 with EEM control 0x0007, as the DLL sends it
 * when breakpoints are polled (controlMask != 0), mdb = 0, releaseJtag = 0,
 * ulpDebug = 0. */
Status Debugger::resume()
{
	if (!attached_) {
		return Status::BadState;
	}
	if (running_) {
		return Status::Running;
	}
	restore_and_release(kEmuClkEn | kClearStop | kEemEn);
	running_ = true;
	reason_ = StopReason::None;
	bp_hit_ = kNoBreakpoint;
	return Status::Ok;
}

/* WaitForEem.c:61-111 with the mask 0x80 the DLL's PollingManager passes
 * (PollingManager.cpp:221, 240-242), then SyncJtag_Conditional_SaveContextXv2
 * as DebugManagerMSP430::runEvent does on BP_HIT_FLAG (saveContext). The JTAG
 * ID check ends the session here (see the header). */
Status Debugger::poll(bool &halted)
{
	halted = !running_;
	if (!attached_) {
		return Status::BadState;
	}
	if (!running_) {
		return Status::Ok;
	}
	if (tap_.ir_shift(kIrCntrlSigCapture) != kJtagId91) { /* jtagIdIsValid */
		abandon();
		return Status::TargetError;
	}
	if ((read_control() & kEemStopped) == 0) {
		return Status::Ok;
	}
	const Status st = sync_conditional();
	if (st != Status::Ok) {
		return st; /* the CPU has been let go */
	}
	running_ = false;
	halted = true;
	reason_ = StopReason::Breakpoint;
	bp_hit_ = kNoBreakpoint;
	for (unsigned i = 0; i < kHwBreakpoints; ++i) { /* not in MSPDS, see the header */
		if ((bp_used_ & (1u << i)) != 0 && bp_addr_[i] == ctx_.pc) {
			bp_hit_ = static_cast<uint8_t>(i);
			break;
		}
	}
	return Status::Ok;
}

/* SingleStepXv2.c:114-144: trigger block 0 and BREAKREACT back as step()
 * found them. Nothing when step() has not borrowed them. */
void Debugger::give_back_trigger_block_0()
{
	if (!tb0_borrowed_) {
		return;
	}
	tb0_borrowed_ = false;
	tap_.ir_shift(kIrEmexDataExchange32);
	eem_write(kMbTrigVal, tb0_[0]);
	eem_write(kMbTrigCtl, tb0_[1]);
	eem_write(kMbTrigMsk, tb0_[2]);
	eem_write(kMbTrigCmb, tb0_[3]);
	eem_write(kBreakReact, tb0_[4]);
}

/* SingleStepXv2.c:57-164 with the parameters the DLL passes
 * (DebugManagerMSP430::singleStep, DebugManagerMSP430.cpp:578-591):
 * RestoreContext_ReleaseJtagXv2 with EEM control 0x0007, mdb = 0,
 * releaseJtag = 0, ulpDebug = 0; irRequest from the last sync. The timeout,
 * the JTAG ID check after a release without a trigger and trigger block 0
 * after a failed sync differ from MSPDS (see the header). */
Status Debugger::step()
{
	if (!attached_) {
		return Status::BadState;
	}
	if (running_) {
		return Status::Running;
	}
	/* lines 88-93: unless the CPU is off with no interrupt pending, trigger
	 * block 0 stops it on the next instruction fetch (the MyIn table) */
	const bool use_eem = (ctx_.sr & kSrCpuOff) == 0 || (ctx_.irq & kSigIntReq) != 0;
	if (use_eem) {
		tap_.ir_shift(kIrEmexDataExchange32);
		tb0_[0] = eem_read(kMbTrigVal);
		tb0_[1] = eem_read(kMbTrigCtl);
		tb0_[2] = eem_read(kMbTrigMsk);
		tb0_[3] = eem_read(kMbTrigCmb);
		tb0_[4] = eem_read(kBreakReact);
		tb0_borrowed_ = true;
		eem_write(kMbTrigCtl, kCmpEqualFetchMab);
		eem_write(kMbTrigMsk, kMaskAll);
		eem_write(kMbTrigCmb, 0x0001); /* EN0 */
		eem_write(kBreakReact, 0x0001);
	}
	restore_and_release(kEmuClkEn | kClearStop | kEemEn); /* lines 94-96 */
	running_ = true;
	reason_ = StopReason::None;
	bp_hit_ = kNoBreakpoint;

	if (!use_eem) {
		/* lines 147-162: the CPU stays off and executes nothing. "First
		 * Check if we did not go into LPMx.5": MSPDS returns success for an
		 * invalid ID; here the target is lost (see the header). */
		if (tap_.ir_shift(kIrCntrlSigCapture) != kJtagId91) {
			abandon();
			return Status::TargetError;
		}
		const Status st = sync_conditional();
		if (st != Status::Ok) {
			return st; /* the CPU has been let go */
		}
		running_ = false;
		reason_ = StopReason::Step;
		return Status::Ok;
	}

	/* lines 98-108: poll for the CPU stop reaction. MSPDS's counter starts
	 * at 4 and ends the poll at 500 (kStepPolls reads); `i < 500` then
	 * fails even if the last read showed the stop. */
	tap_.ir_shift(kIrEmexReadControl);
	unsigned reads = 0;
	bool stopped = false;
	do {
		++reads;
		stopped = (tap_.dr_shift16(0x0000) & kEemStopped) != 0;
	} while (!stopped && reads < kStepPolls);
	const bool stepped = reads < kStepPolls;
	/* lines 110-113; without the stop not in MSPDS: the sync stops the CPU
	 * as halt() does (see the header) */
	const Status st = sync_conditional();
	if (st != Status::Ok) {
		return st; /* the CPU has been let go */
	}
	running_ = false;
	give_back_trigger_block_0();
	if (!stepped) {
		reason_ = StopReason::Request;
		return Status::Timeout;
	}
	reason_ = StopReason::Step;
	return Status::Ok;
}

} // namespace msp430
