#pragma once
/* Parses the MSP430 SBW vendor command and keeps the session state.
 * Never reads outside the request nor writes outside the response; malformed
 * input yields an error status.
 *
 * Debug sessions (protocol version 2) are read-only: WRITE_MEM may write only
 * the peripheral space without the flash controller, and RAM; ERASE and
 * FLASH_BEGIN are refused. This class is the only barrier between the host
 * and the Debugger's write_mem, which writes any address it is given.
 */
#include <cstddef>
#include <cstdint>
#include <span>

#include "msp430/cpuxv2.hpp"
#include "msp430/debug.hpp"
#include "msp430/protocol.hpp"

namespace msp430 {

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
	/* Read-only debug sessions: may [addr, addr + nbytes) be written? */
	bool writable(uint32_t addr, uint32_t nbytes) const;

	Cpuxv2 &cpu_;
	Debugger &dbg_;
	uint16_t max_data_bytes_;
	State state_ = State::Idle;
	uint32_t flash_remaining_ = 0; /* words still expected by the funclet */
	uint32_t ram_lo_ = 0;          /* writable RAM in a debug session, */
	uint32_t ram_hi_ = 0;          /* [ram_lo_, ram_hi_); empty when equal */
};

} // namespace msp430
