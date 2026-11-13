#pragma once
/* Parses the MSP430 SBW vendor command and keeps the session state.
 * Never reads outside the request nor writes outside the response; malformed
 * input yields an error status.
 *
 * Debug sessions (protocol version 2) are read-only. What that guarantees:
 * for the host, the probe never writes flash, the registers of the PMM, the
 * flash controller, the DMA controller or the SYS module, and never moves the
 * PC outside the device's main flash. WRITE_MEM may write only RAM and the
 * peripherals below 0x1000 minus those four register blocks (F5529: PMM
 * 0x0120-0x013F, FCTL 0x0140-0x014F, SYS 0x0180-0x019F, DMA 0x0500-0x053F;
 * the host's device table, tools/msp430/devices.py, mirrors them);
 * DBG_WRITE_REG may set the PC only to an address in main flash or to its
 * current value; ERASE and FLASH_BEGIN are refused. On a device the probe
 * does not know, no RAM and no peripheral may be written and the PC stays
 * where it is. It does not stop the firmware's own code from doing what it
 * normally does when it runs, nor code the user places by other means (a
 * return address changed on the stack, for example). This class is the only
 * barrier between the host and the Debugger's write_mem and write_reg, which
 * write any address and any PC they are given.
 */
#include <cstddef>
#include <cstdint>
#include <span>

#include "msp430/cpuxv2.hpp"
#include "msp430/debug.hpp"
#include "msp430/protocol.hpp"

namespace msp430 {

struct KnownDevice; /* session.cpp */

class Session {
public:
	/* max_data_bytes: the transport's packet size minus the longest header,
	 * kept even: WRITE_MEM has 2 + 4 header bytes, so a 1024-byte packet
	 * allows at most 1018. It must also satisfy 2 + max_data_bytes <=
	 * response capacity for READ_MEM. */
	Session(Cpuxv2 &cpu, Debugger &dbg, uint16_t max_data_bytes)
		: cpu_(cpu), dbg_(dbg), max_data_bytes_(max_data_bytes)
	{
	}

	/* `request` starts at the vendor command ID. Returns the response
	 * length, or 0 if the request is not addressed to this handler or the
	 * response buffer has fewer than 2 bytes. Fixed-size requests with
	 * missing or extra bytes are BAD_ARG. DISCONNECT in Idle is BAD_STATE
	 * (clients that disconnect as cleanup ignore it). DBG_ATTACH is
	 * BAD_STATE in Connected and Flashing; in a debug session it ends that
	 * session as DBG_DETACH with DetachAction::Run does, then attaches as
	 * from Idle (a malformed one is BAD_ARG and leaves the session as it
	 * was).
	 * A command that ends with Status::TargetError while a session is open
	 * closes the session (the target is in an unknown state and the pins are
	 * released); the host has to connect again. In a debug session the
	 * Debugger may already have let the CPU go and released the pins; then
	 * nothing more is sent. */
	size_t handle(std::span<const uint8_t> request, std::span<uint8_t> response);

	/* Close an open session (USB loss, SWD takeover, a new CONNECT). Safe
	 * when idle. Must be called on the thread that calls handle(). When a
	 * write is in progress it re-synchronizes and wipes the funclet RAM
	 * first, which takes a few hundred milliseconds. A debug session is
	 * detached with DetachAction::Run: a running CPU is halted first, then
	 * the program continues where it stopped. */
	void abort();

	/* Callers that mirror the session state (LED, pin ownership) must read
	 * it after every handle(), because a TARGET_ERROR closes the session
	 * internally. */
	State state() const { return state_; }

private:
	using In = std::span<const uint8_t>;
	using Out = std::span<uint8_t>;

	Status op_info(In args, Out out, size_t &n);
	Status op_connect(In args, Out out, size_t &n);
	Status op_disconnect(In args);
	Status op_read_mem(In args, Out out, size_t &n);
	Status op_write_mem(In args);
	Status op_erase(In args);
	Status op_flash_begin(In args);
	Status op_flash_data(In args);
	Status op_flash_end(In args);
	Status op_dbg_attach(In args, Out out, size_t &n);
	Status op_dbg_detach(In args);
	Status op_dbg_halt(In args, Out out, size_t &n);
	Status op_dbg_resume(In args);
	Status op_dbg_status(In args, Out out, size_t &n);
	Status op_dbg_step(In args, Out out, size_t &n);
	Status op_dbg_read_regs(In args, Out out, size_t &n);
	Status op_dbg_write_reg(In args);
	Status op_dbg_bp_set(In args);
	Status op_dbg_bp_clear(In args);

	bool debugging() const
	{
		return state_ == State::DebugHalted || state_ == State::DebugRunning;
	}
	/* Read-only debug sessions: may [addr, addr + nbytes) be written? No
	 * when nbytes is 0. */
	bool writable(uint32_t addr, uint32_t nbytes) const;
	/* Forget the RAM window and the device's limits. */
	void clear_windows();

	Cpuxv2 &cpu_;
	Debugger &dbg_;
	uint16_t max_data_bytes_;
	State state_ = State::Idle;
	uint32_t flash_remaining_ = 0; /* words still expected by the funclet */
	uint32_t ram_lo_ = 0;          /* writable RAM in a debug session, */
	uint32_t ram_hi_ = 0;          /* [ram_lo_, ram_hi_); empty when equal */
	/* In a debug session: the identified device's RAM, main flash and
	 * peripheral holes (session.cpp); nullptr for a device the probe does
	 * not know. */
	const KnownDevice *device_ = nullptr;
};

} // namespace msp430
