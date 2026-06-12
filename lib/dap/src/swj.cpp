/* lib/dap/src/swj.cpp — Connect/Disconnect/ResetTarget/SWJ_Clock/SWJ_Pins
 * implemented in Task 3; SWJ_Sequence/SWD_Configure/SWD_Sequence stubbed
 * here and replaced in Task 4.
 */
#include "dap/constants.hpp"
#include "dap/engine.hpp"

namespace dap {

/* --- Task 3 implementations --- */

DapEngine::Sizes DapEngine::cmd_connect(std::span<const uint8_t> req,
					std::span<uint8_t> resp)
{
	const uint8_t want = req.empty() ? kPortDefault : req[0];
	if (want == kPortDefault || want == kPortSwd) {
		driver_.connect();
		connected_ = true;
		resp[0] = kPortSwd;
	} else {
		resp[0] = kPortDisabled; /* JTAG unavailable */
	}
	return {req.empty() ? 0u : 1u, 1};
}

DapEngine::Sizes DapEngine::cmd_disconnect(std::span<uint8_t> resp)
{
	connected_ = false;
	driver_.disconnect();
	resp[0] = kDapOk;
	return {0, 1};
}

DapEngine::Sizes DapEngine::cmd_reset_target(std::span<uint8_t> resp)
{
	if (resp.size() < 2) {
		resp[0] = kDapError;
		return {0, 1};
	}
	resp[0] = kDapOk;
	resp[1] = 0x00; /* no device-specific reset sequence on this probe */
	return {0, 2};
}

DapEngine::Sizes DapEngine::cmd_swj_clock(std::span<const uint8_t> req,
					  std::span<uint8_t> resp)
{
	if (req.size() < 4) {
		resp[0] = kDapError;
		return {req.size(), 1};
	}
	const uint32_t hz = static_cast<uint32_t>(req[0]) |
			    (static_cast<uint32_t>(req[1]) << 8) |
			    (static_cast<uint32_t>(req[2]) << 16) |
			    (static_cast<uint32_t>(req[3]) << 24);
	if (hz == 0) {
		resp[0] = kDapError;
	} else {
		driver_.set_clock(hz);
		resp[0] = kDapOk;
	}
	return {4, 1};
}

DapEngine::Sizes DapEngine::cmd_swj_pins(std::span<const uint8_t> req,
					 std::span<uint8_t> resp)
{
	if (req.size() < 6) {
		resp[0] = kDapError;
		return {req.size(), 1};
	}
	const uint8_t value = req[0];
	const uint8_t select = req[1];
	const uint32_t wait_us = static_cast<uint32_t>(req[2]) |
				 (static_cast<uint32_t>(req[3]) << 8) |
				 (static_cast<uint32_t>(req[4]) << 16) |
				 (static_cast<uint32_t>(req[5]) << 24);
	driver_.set_pins(select, value, wait_us);
	resp[0] = driver_.read_pins();
	return {6, 1};
}

/* --- Task 4 stubs (replaced in Task 4) --- */

#define DAP_STUB(name)                                                          \
	DapEngine::Sizes DapEngine::name(std::span<const uint8_t> req,         \
					 std::span<uint8_t> resp)               \
	{                                                                       \
		resp[0] = kDapError;                                            \
		return {req.size(), 1};                                         \
	}
DAP_STUB(cmd_swj_sequence)
DAP_STUB(cmd_swd_configure)
DAP_STUB(cmd_swd_sequence)
#undef DAP_STUB

} // namespace dap
