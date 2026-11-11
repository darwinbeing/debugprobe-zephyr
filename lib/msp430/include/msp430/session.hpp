#pragma once
/* Parses the MSP430 SBW vendor command and keeps the session state.
 * Never reads outside the request nor writes outside the response; malformed
 * input yields an error status.
 */
#include <cstddef>
#include <cstdint>
#include <span>

#include "msp430/cpuxv2.hpp"
#include "msp430/protocol.hpp"

namespace msp430 {

class Session {
public:
	/* max_data_bytes: the transport's packet size minus the longest header,
	 * kept even: WRITE_MEM has 2 + 4 header bytes, so a 1024-byte packet
	 * allows at most 1018. It must also satisfy 2 + max_data_bytes <=
	 * response capacity for READ_MEM. */
	Session(Cpuxv2 &cpu, uint16_t max_data_bytes)
		: cpu_(cpu), max_data_bytes_(max_data_bytes)
	{
	}

	/* `request` starts at the vendor command ID. Returns the response
	 * length, or 0 if the request is not addressed to this handler or the
	 * response buffer has fewer than 2 bytes. Fixed-size requests with
	 * missing or extra bytes are BAD_ARG. DISCONNECT in Idle is BAD_STATE
	 * (clients that disconnect as cleanup ignore it).
	 * A command that ends with Status::TargetError while a session is open
	 * closes the session (the target is in an unknown state and the pins are
	 * released); the host has to connect again. */
	size_t handle(std::span<const uint8_t> request, std::span<uint8_t> response);

	/* Close an open session (USB loss, SWD takeover). Safe when idle. Must
	 * be called on the thread that calls handle(). When a write is in
	 * progress it re-synchronizes and wipes the funclet RAM first, which
	 * takes a few hundred milliseconds. */
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

	Cpuxv2 &cpu_;
	uint16_t max_data_bytes_;
	State state_ = State::Idle;
	uint32_t flash_remaining_ = 0; /* words still expected by the funclet */
};

} // namespace msp430
