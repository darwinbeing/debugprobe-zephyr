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
	/* max_data_bytes: largest data payload of one command, in either
	 * direction (the transport packet size minus the headers). */
	Session(Cpuxv2 &cpu, uint16_t max_data_bytes)
		: cpu_(cpu), max_data_bytes_(max_data_bytes)
	{
	}

	/* `request` starts at the vendor command ID. Returns the response
	 * length, or 0 if the request is not addressed to this handler.
	 * A command that ends with Status::TargetError while a session is open
	 * closes the session (the target is in an unknown state and the pins are
	 * released); the host has to connect again. */
	size_t handle(std::span<const uint8_t> request, std::span<uint8_t> response);

	/* Close an open session (USB loss, SWD takeover). Safe when idle. */
	void abort();

	State state() const { return state_; }

private:
	using In = std::span<const uint8_t>;
	using Out = std::span<uint8_t>;

	Status op_info(Out out, size_t &n);
	Status op_connect(In args, Out out, size_t &n);
	Status op_disconnect();
	Status op_read_mem(In args, Out out, size_t &n);
	Status op_write_mem(In args);
	Status op_erase(In args);
	Status op_flash_begin(In args);
	Status op_flash_data(In args);
	Status op_flash_end();

	Cpuxv2 &cpu_;
	uint16_t max_data_bytes_;
	State state_ = State::Idle;
	uint32_t flash_remaining_ = 0; /* words still expected by the funclet */
};

} // namespace msp430
