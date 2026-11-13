#pragma once
/* MSP430 CPUXv2 debugging over a Tap: attach with or without reset, the
 * saved CPU context, registers, memory while halted, single step, run with
 * EEM breakpoints, stop detection and detach. Port of the CPUXv2 macros of
 * TI's MSP Debug Stack (MSPDS 3.15.1.1, Bios/src/hal/macros). Device
 * parameters are those of the MSP430F5529 (JTAG ID 0x91).
 *
 * The debugger never erases or programs flash: it uses only identify(),
 * in_full_emulation() and disconnect() of Cpuxv2, none of the funclet,
 * erase or flash-write code. write_mem, though, writes any address it is
 * given; keeping it off flash and the flash controller is Session's job.
 */
#include <cstddef>
#include <cstdint>

#include "msp430/cpuxv2.hpp"
#include "msp430/protocol.hpp"
#include "msp430/tap.hpp"

namespace msp430 {

class Debugger {
public:
	Debugger(Tap &tap, Cpuxv2 &cpu) : tap_(tap), cpu_(cpu) {}

	/* Take the CPU under JTAG control (normal entry: RST stays high).
	 * Running: SyncJtag_Conditional_SaveContextXv2, the program stops where
	 * it is (stop reason Request). Reset: SyncJtag_AssertPor_SaveContextXv2,
	 * the CPU stops at the reset vector (stop reason Reset). Then the device
	 * ID is read and all EEM triggers are cleared. Statuses as
	 * Cpuxv2::connect. On failure the pins are released and the CPU is let
	 * go: before it reached full emulation with nothing written to its
	 * memory; in full emulation before its context was saved, at the PC
	 * where it stopped (SR and WDTCTL unwritten), except that with Reset,
	 * whose power-on reset left the CPU at a safe address (or, after a
	 * failed init-state check, in an unknown state), it gets another
	 * power-on reset as detach(Reset) does; after that with its context
	 * restored, as detach(Run) does. With Reset, system clocks already
	 * switched to the JTAG clock are switched back before any release. */
	Status attach(AttachMode mode, uint32_t slot_hz, Identity &id);
	/* Clear the triggers, then Run: restore the context and let the program
	 * continue where it stopped; Reset: power-on reset and run. Releases
	 * the pins. Does nothing when not attached. Expects a halted CPU. */
	void detach(DetachAction action);

	bool attached() const { return attached_; }
	bool running() const { return running_; }
	/* Valid while attached and halted. */
	uint32_t pc() const { return ctx_.pc; }
	StopReason stop_reason() const { return reason_; }
	/* Index of the breakpoint at the stop address, or kNoBreakpoint. */
	uint8_t breakpoint_hit() const { return bp_hit_; }

	/* Stop a running CPU (stop reason Request, or Breakpoint if the EEM had
	 * already stopped it). Ok when already halted. */
	Status halt();
	/* Restore the context and run with the EEM and the breakpoints armed. */
	Status resume();
	/* While running: has the EEM stopped the CPU? If so the context is saved
	 * (stop reason Breakpoint). `halted` reports the state afterwards. */
	Status poll(bool &halted);
	/* Execute one instruction (stop reason Step). Timeout if the CPU did not
	 * stop by itself; it is halted anyway. */
	Status step();

	/* R0 (PC) and R2 (SR) come from the saved context, R3 reads 0. Values
	 * are 20 bits wide (SR 16). BadState when not attached, Running while
	 * the CPU runs; TargetError if the CPU is no longer under JTAG control
	 * afterwards (`regs` is filled anyway). */
	Status read_regs(uint32_t regs[16]);
	/* R0 and R2 change the saved context, which is written back on release;
	 * the others are written to the CPU, SP with bit 0 cleared (as the DLL
	 * does). BadState and Running as read_regs; BadArg for an index above
	 * 15, R3, a value above 20 bits, an odd PC or an SR above 16 bits;
	 * TargetError if the CPU is no longer under JTAG control after a write
	 * to the CPU. */
	Status write_reg(unsigned index, uint32_t value);
	/* `nwords` words from `addr` on, little-endian byte pairs, one per word.
	 * Plain bus accesses: reading interrupt-vector registers, RXBUF or
	 * vacant addresses has its usual effects. The PC is not moved. WDTCTL
	 * (0x015C) goes through the saved context, as TI's DLL does: read_mem
	 * returns 0x69 with the saved low byte, which the program gets back on
	 * release; write_mem saves the written low byte for the release and
	 * writes it with the password 0x5A and the hold bit (the written high
	 * byte is ignored), so the watchdog stays held while the CPU is halted.
	 * BadState when not attached, Running while the CPU runs, BadArg (no
	 * TAP traffic) for an odd address, an address above 20 bits or words
	 * past 0xFFFFF; no words: Ok, no TAP traffic. TargetError if the CPU is
	 * no longer under JTAG control afterwards: read_mem fills `dst` anyway;
	 * write_mem has sent every word, but those after the link died did not
	 * arrive. write_mem makes the same check before its first write and
	 * returns TargetError without writing anything if it fails. Other
	 * address checks are the caller's (Session): write_mem writes any
	 * address it is given, flash and the flash controller included. */
	Status read_mem(uint32_t addr, size_t nwords, uint8_t *dst);
	Status write_mem(uint32_t addr, size_t nwords, const uint8_t *src);

	/* Breakpoint `index` (< kHwBreakpoints) on the instruction fetch at
	 * `addr`: EEM trigger block `index` compares the MAB, combination
	 * `index` holds it alone, BREAKREACT bit `index` arms it; it stops the
	 * CPU once resume() enables the EEM. Setting an index again moves it.
	 * BadState when not attached, Running while the CPU runs, BadArg (no
	 * TAP traffic) for an index of kHwBreakpoints or more, an odd address
	 * or one above 20 bits. TargetError if the trigger block's value does
	 * not read back (no such trigger block, or a dead link): the block is
	 * then disarmed, so a breakpoint this index held before is gone too. */
	Status set_breakpoint(unsigned index, uint32_t addr);
	/* Disarm breakpoint `index` (its combination register and BREAKREACT
	 * bit cleared), Ok also when it was not set; kAllBreakpoints disarms
	 * every trigger block. BadState and Running as set_breakpoint; BadArg
	 * (no TAP traffic) for any other index of kHwBreakpoints or more. Reads
	 * nothing back: a dead link goes unnoticed here. */
	Status clear_breakpoint(unsigned index);

private:
	struct Context {
		uint32_t pc = 0;
		uint16_t sr = 0;     /* as read, before CPUOFF and GIE were cleared */
		uint16_t wdtctl = 0; /* WDTCTL as read (0x69xx) */
		uint16_t irq = 0;    /* CNTRL_SIG_INTREQ at the sync */
	};

	Status sync_conditional();
	Status sync_assert_por();
	bool sync_jtag();
	bool wait_for_sync();
	void restore_system_clocks();
	bool hold_watchdog();
	void save_status_register();
	void wait_flash_idle();
	void restore_and_release(uint16_t eem_control);
	void release_at(uint32_t pc, bool cpuoff, uint16_t eem_control);
	void release_unsaved(uint32_t pc, bool cpuoff);
	void release_with_reset();
	void abandon();
	void clear_triggers();
	void clear_jmb_out();

	uint16_t read_word(uint32_t addr);
	void write_word(uint32_t addr, uint16_t data);
	uint32_t read_cpu_reg(uint16_t mova);
	void write_cpu_reg(uint16_t mova, uint16_t data);
	void set_pc(uint32_t pc);
	uint16_t read_control();
	void write_control(uint16_t value);
	uint32_t eem_read(uint8_t reg);
	void eem_write(uint8_t reg, uint32_t value);

	Tap &tap_;
	Cpuxv2 &cpu_;
	Context ctx_;
	bool attached_ = false;
	bool running_ = false;
	StopReason reason_ = StopReason::None;
	uint8_t bp_hit_ = kNoBreakpoint;
	uint32_t bp_addr_[kHwBreakpoints] = {};
	uint32_t bp_used_ = 0; /* bit i: trigger block i is a breakpoint */
};

} // namespace msp430
