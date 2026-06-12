/* lib/dap/src/transfer.cpp — Task 2 stubs; replaced in Tasks 5-6 */
#include "dap/constants.hpp"
#include "dap/engine.hpp"

namespace dap {
DapEngine::Sizes DapEngine::cmd_transfer_configure(std::span<const uint8_t> req,
						   std::span<uint8_t> resp)
{
	resp[0] = kDapError;
	return {req.size(), 1};
}
DapEngine::Sizes DapEngine::cmd_transfer(std::span<const uint8_t> req,
					 std::span<uint8_t> resp)
{
	resp[0] = kDapError;
	return {req.size(), 1};
}
DapEngine::Sizes DapEngine::cmd_transfer_block(std::span<const uint8_t> req,
					       std::span<uint8_t> resp)
{
	resp[0] = kDapError;
	return {req.size(), 1};
}
DapEngine::Sizes DapEngine::cmd_write_abort(std::span<const uint8_t> req,
					    std::span<uint8_t> resp)
{
	resp[0] = kDapError;
	return {req.size(), 1};
}
uint8_t DapEngine::transfer_retry(uint8_t request, uint32_t *data)
{
	(void)request;
	(void)data;
	return kXferNoAck;
}
} // namespace dap
