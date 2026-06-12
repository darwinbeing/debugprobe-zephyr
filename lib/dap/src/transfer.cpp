/* lib/dap/src/transfer.cpp — transfer_retry and cmd_write_abort implemented
 * in Task 3; TransferConfigure/Transfer/TransferBlock stubbed here and
 * replaced in Tasks 5-6.
 */
#include "dap/constants.hpp"
#include "dap/engine.hpp"

namespace dap {

/* --- Task 3 implementations --- */

uint8_t DapEngine::transfer_retry(uint8_t request, uint32_t *data)
{
	uint32_t retry = retry_count_;
	uint8_t response_value;
	do {
		response_value = static_cast<uint8_t>(driver_.transfer(request, data));
	} while (response_value == kXferWait && retry-- &&
		 !abort_.load(std::memory_order_relaxed));
	return response_value;
}

DapEngine::Sizes DapEngine::cmd_write_abort(std::span<const uint8_t> req,
					    std::span<uint8_t> resp)
{
	if (!connected_ || req.size() < 5) {
		resp[0] = kDapError;
		return {req.size() < 5 ? req.size() : 5, 1};
	}
	/* req[0] is the DAP index (ignored, single DP) */
	uint32_t value = static_cast<uint32_t>(req[1]) |
			 (static_cast<uint32_t>(req[2]) << 8) |
			 (static_cast<uint32_t>(req[3]) << 16) |
			 (static_cast<uint32_t>(req[4]) << 24);
	/* DP write, address 0 (ABORT): request byte 0x00 */
	const uint8_t ack = transfer_retry(0x00, &value);
	resp[0] = (ack == kXferOk) ? kDapOk : kDapError;
	return {5, 1};
}

/* --- Task 5-6 stubs (replaced later) --- */

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

} // namespace dap
