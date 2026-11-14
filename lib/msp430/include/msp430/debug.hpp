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
 * given; keeping it off flash, the PMM, the flash controller, the SYS and
 * DMA registers is Session's job.
 *
 * "The link check" below is Cpuxv2::in_full_emulation(): the JTAG ID that an
 * IR shift captures is 0x91, and the control signal then read has at least
 * one of the bits 0x0301 set. It tests the Spy-Bi-Wire link (a dead link
 * reads all ones or all zeros), not what the CPU is doing. Not in MSPDS.
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

	/* An open session is first ended as detach(Run) does. Then take the CPU
	 * under JTAG control (normal entry: RST stays high).
	 * Running: SyncJtag_Conditional_SaveContextXv2, the program stops where
	 * it is (stop reason Request). Reset: SyncJtag_AssertPor_SaveContextXv2,
	 * the CPU stops at the reset vector (stop reason Reset). Then the device
	 * ID is read, all EEM triggers are cleared and the EEM sequencer is
	 * turned off (as the DLL's EEM_Init leaves it). Statuses as
	 * Cpuxv2::connect. On failure the pins are released and the CPU is let
	 * go: before it reached full emulation with nothing written to its
	 * memory; in full emulation before its context was saved, at the PC
	 * where it stopped (SR and WDTCTL unwritten), except that with Reset,
	 * whose power-on reset left the CPU at a safe address (or, after a
	 * failed init-state check, in an unknown state), it gets another
	 * power-on reset as detach(Reset) does; after that with its context
	 * restored, as detach(Run) does. Before the CPU gets its PC or its
	 * context back, the link check is made again; if it fails, nothing is
	 * fed over the link: before the context was saved, an attempt is made
	 * to turn the EEM off, and the CPU is released where it is with nothing
	 * written to it; after, detach(Run) resets it instead (correction 26).
	 * With Reset, system clocks already switched to the JTAG clock are
	 * switched back before any release. */
	Status attach(AttachMode mode, uint32_t slot_hz, Identity &id);
	/* A running CPU is halted first (halt()); if that fails, the CPU has
	 * been let go as halt() describes and nothing more is done: with Reset,
	 * no reset happens and the program keeps running as it is; TargetError.
	 * Then the triggers are cleared and the EEM sequencer is turned off, and
	 * Run: the link check, then the context (SR, WDTCTL, PC) is restored and
	 * the program continues where it stopped, EEM off; Ok. If the link check
	 * fails twice in a row (after a TargetError from a memory, register or
	 * breakpoint access, for example; one bad capture does not count),
	 * nothing is restored over the link: the EEM is
	 * turned off and the CPU gets a power-on reset through the control
	 * signal (as with Reset; nothing written to its memory, no instruction
	 * fed), so the firmware restarts instead of running on from wherever
	 * JTAG left the PC with its watchdog held; TargetError (design spec
	 * correction 26). Reset: power-on reset and run; Ok. Releases the pins.
	 * BadState, nothing sent, when not attached. */
	Status detach(DetachAction action);

	bool attached() const { return attached_; }
	bool running() const { return running_; }
	/* Valid while attached and halted. */
	uint32_t pc() const { return ctx_.pc; }
	StopReason stop_reason() const { return reason_; }
	/* After a breakpoint stop: the index of the breakpoint whose address is
	 * the stop PC (the lowest if several), else kNoBreakpoint. */
	uint8_t breakpoint_hit() const { return bp_hit_; }

	/* Stop a running CPU. poll()'s look first: a CPU the EEM has already
	 * stopped is taken as poll() takes it (stop reason Breakpoint).
	 * Otherwise SyncJtag_Conditional_SaveContextXv2 stops it through the
	 * EEM (at once in a low-power mode) and saves its context (stop reason
	 * Request). Ok, nothing sent, when already halted. BadState when not
	 * attached. TargetError as poll() reports it, or when the sync fails:
	 * the CPU is let go as attach(Running) describes for its failures and
	 * the debugger is detached. */
	Status halt();
	/* Restore the context (SR, WDTCTL, PC) and let the CPU run with the EEM
	 * on and the breakpoints armed (EEM control 0x0007). BadState when not
	 * attached, Running (nothing sent) when it already runs. Nothing is
	 * checked (MSPDS checks nothing here): a lost target shows at the next
	 * poll(). */
	Status resume();
	/* While running: has the EEM stopped the CPU (WaitForEem)? If so its
	 * context is saved (stop reason Breakpoint, see breakpoint_hit()).
	 * `halted` reports the state afterwards. Ok, nothing sent, `halted`
	 * true, when already halted. BadState when not attached (`halted`
	 * true). TargetError, `halted` false and the debugger detached: the JTAG
	 * ID is not 0x91 (the target reset or lost power): an attempt is made to
	 * turn the EEM off, the CPU is released where it is with nothing written
	 * to it, the pins are released; or the sync after a stop failed: the
	 * CPU is let go as attach(Running) describes for its failures. */
	Status poll(bool &halted);
	/* Execute one instruction (SingleStepXv2). Unless the CPU is off (SR
	 * CPUOFF) with no interrupt pending at the last sync, trigger block 0
	 * and BREAKREACT are borrowed to stop it on the next instruction fetch;
	 * the context is restored and the CPU released with the EEM on (as
	 * resume()); once the EEM has stopped it, its context is saved again
	 * (SyncJtag_Conditional_SaveContextXv2) and trigger block 0 and
	 * BREAKREACT are written back as they were. With GIE set and an
	 * interrupt pending, the CPU takes it after the instruction and the
	 * step ends at the handler's first instruction, as in MSPDS and the
	 * DLL. A CPU that is off with no interrupt pending executes nothing: it
	 * is released and taken again without a trigger; an interrupt that
	 * arrives during that brief release runs until the sync stops it, and
	 * the result is still Step. The CPU is released with the firmware's own
	 * WDTCTL, so the watchdog counts during each step (as in the DLL).
	 * Ok: stop reason Step. BadState when not attached, Running when it
	 * runs (nothing sent). Timeout: the stop did not show within 495 reads
	 * of the EEM control register; the CPU is stopped and its context saved
	 * as halt() does, trigger block 0 written back, stop reason Request. A
	 * step over an instruction that enters a low-power mode ends so,
	 * although the instruction executed (design spec item 17): the CPU
	 * fetches nothing more. TargetError, the debugger detached: after a
	 * release without a trigger the JTAG ID is not 0x91 (the target reset,
	 * lost power or went into LPMx.5): the EEM is turned off, the CPU
	 * released where it is with nothing more written to it, the pins
	 * released; or the sync after the release failed: the CPU is let go as
	 * attach(Running) describes for its failures, trigger block 0 written
	 * back before. */
	Status step();

	/* R0 (PC) and R2 (SR) come from the saved context, R3 reads 0. Values
	 * are 20 bits wide (SR 16). BadState when not attached, Running while
	 * the CPU runs; TargetError if the link check afterwards fails (`regs`
	 * is filled anyway). */
	Status read_regs(uint32_t regs[16]);
	/* R0 and R2 change the saved context, which is written back on release;
	 * the others are written to the CPU, SP with bit 0 cleared (as the DLL
	 * does). BadState and Running as read_regs; BadArg for an index above
	 * 15, R3, a value above 20 bits, an odd PC or an SR above 16 bits;
	 * TargetError if the link check after a write to the CPU fails. Any
	 * even 20-bit PC is accepted; in a read-only session, keeping the PC
	 * inside main flash is Session's job. */
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
	 * past 0xFFFFF; no words: Ok, no TAP traffic. TargetError if the link
	 * check afterwards fails: read_mem fills `dst` anyway; write_mem has
	 * sent every word, but those after the link died did not arrive.
	 * write_mem makes the link check once before its first write too (not
	 * before each word) and returns TargetError without writing anything if
	 * it fails. Other
	 * address checks are the caller's: write_mem writes any address it is
	 * given, flash, the PMM, the flash controller, the SYS and DMA
	 * registers included; keeping it off them is Session's job. */
	Status read_mem(uint32_t addr, size_t nwords, uint8_t *dst);
	Status write_mem(uint32_t addr, size_t nwords, const uint8_t *src);

	/* Breakpoint `index` (< kHwBreakpoints) on the instruction fetch at
	 * `addr`: EEM trigger block `index` compares the MAB, combination
	 * `index` holds it alone, BREAKREACT bit `index` arms it; it stops the
	 * CPU once resume() enables the EEM. Setting an index again moves it.
	 * BadState when not attached, Running while the CPU runs, BadArg (no
	 * TAP traffic) for an index of kHwBreakpoints or more, an odd address
	 * or one above 20 bits. TargetError if the trigger block's value does
	 * not read back (no such trigger block, or a dead link): an attempt is
	 * made to disarm it, and a breakpoint this index held before no longer
	 * counts as set; or if the link check afterwards fails: the breakpoint
	 * is armed and counts as set (detach clears every trigger block). */
	Status set_breakpoint(unsigned index, uint32_t addr);
	/* Disarm breakpoint `index` (its combination register and BREAKREACT
	 * bit cleared), Ok also when it was not set; kAllBreakpoints disarms
	 * every trigger block and turns the EEM sequencer off. BadState and
	 * Running as set_breakpoint; BadArg (no TAP traffic) for any other
	 * index of kHwBreakpoints or more. TargetError if the link check
	 * afterwards fails: the writes have been sent, and the breakpoint no
	 * longer counts as set. */
	Status clear_breakpoint(unsigned index);

	/* Not in MSPDS: a read-only check of the 32-bit DR shift (design spec
	 * correction 25), outside a debug session. Cpuxv2::identify() with the
	 * normal entry, as attach() runs it before its sync; then
	 * IR_EMEX_DATA_EXCHANGE32 and the EEM register `reg` read twice with
	 * EemDataExchangeXv2's read framing (each read shifts `reg` with the read
	 * bit set, then 32 bits out; the register is not written), into
	 * values[0] and values[1]; then one more IR scan, IR_CNTRL_SIG_CAPTURE,
	 * whose captured JTAG ID shows that the link survived the 32-bit scans;
	 * then the pins are released as a failed identify() releases them. No
	 * TCLK, no control-signal write, no EEM register or control write, no
	 * memory access: the CPU keeps running untouched. `reg` must be even and
	 * at most kEemPeekLastAddr (the trigger blocks' value, control, mask and
	 * combination registers), else BadArg with no TAP traffic. BadState,
	 * nothing sent, while attached. Statuses of identify() (NoTarget with
	 * `id.jtag_id`, Locked, Busy, TargetError), each with the pins released
	 * by identify(); TargetError, pins released, if the JTAG ID captured in
	 * front of the reads or after them is not 0x91 (no 32-bit scan when the
	 * first is wrong). `values` is 0 0 unless Ok. */
	Status eem_peek(uint8_t reg, uint32_t slot_hz, Identity &id, uint32_t values[2]);

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
	void give_back_trigger_block_0();
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
	/* While step() has borrowed trigger block 0: its value, control, mask
	 * and combination registers and BREAKREACT as step() found them. */
	uint32_t tb0_[5] = {};
	bool tb0_borrowed_ = false;
};

} // namespace msp430
