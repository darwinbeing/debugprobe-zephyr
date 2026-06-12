/* lib/dap/src/swj.cpp — Task 2 stubs; replaced in Tasks 3-4 */
#include "dap/constants.hpp"
#include "dap/engine.hpp"

namespace dap {
#define DAP_STUB(name)                                                          \
	DapEngine::Sizes DapEngine::name(std::span<const uint8_t> req,         \
					 std::span<uint8_t> resp)               \
	{                                                                       \
		resp[0] = kDapError;                                            \
		return {req.size(), 1};                                         \
	}
DAP_STUB(cmd_connect)
DAP_STUB(cmd_swj_pins)
DAP_STUB(cmd_swj_clock)
DAP_STUB(cmd_swj_sequence)
DAP_STUB(cmd_swd_configure)
DAP_STUB(cmd_swd_sequence)
#undef DAP_STUB
DapEngine::Sizes DapEngine::cmd_disconnect(std::span<uint8_t> resp)
{
	resp[0] = kDapError;
	return {0, 1};
}
DapEngine::Sizes DapEngine::cmd_reset_target(std::span<uint8_t> resp)
{
	resp[0] = kDapError;
	return {0, 1};
}
} // namespace dap
