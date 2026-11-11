#pragma once
/* CMSIS-DAP v2 protocol engine: parses one request packet, drives the
 * SwdDriver, writes the response packet. No heap, no exceptions, no OS.
 */
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

#include "dap/interfaces.hpp"

namespace dap {

class DapEngine {
public:
	DapEngine(SwdDriver &driver, HostStatus &host, const Config &config);

	/* Process one transport packet (handles ExecuteCommands batching).
	 * Returns the number of response bytes written. Never reads outside
	 * `request` nor writes outside `response`; malformed input yields a
	 * DAP_ERROR / Invalid response, not UB.
	 */
	size_t process(std::span<const uint8_t> request, std::span<uint8_t> response);

	/* Transport hook for DAP_TransferAbort packets (0x07): abort the
	 * Transfer/TransferBlock currently executing on the DAP thread.
	 */
	void request_abort() { abort_.store(true, std::memory_order_relaxed); }

	/* Install (or clear with nullptr) the handler for vendor commands. */
	void set_vendor_handler(VendorHandler *handler) { vendor_ = handler; }

private:
	struct Sizes {
		size_t consumed; /* request bytes */
		size_t written;  /* response bytes */
	};

	Sizes dispatch(std::span<const uint8_t> req, std::span<uint8_t> resp);

	Sizes cmd_info(std::span<const uint8_t> req, std::span<uint8_t> resp);
	Sizes cmd_host_status(std::span<const uint8_t> req, std::span<uint8_t> resp);
	Sizes cmd_connect(std::span<const uint8_t> req, std::span<uint8_t> resp);
	Sizes cmd_disconnect(std::span<uint8_t> resp);
	Sizes cmd_delay(std::span<const uint8_t> req, std::span<uint8_t> resp);
	Sizes cmd_reset_target(std::span<uint8_t> resp);
	Sizes cmd_swj_pins(std::span<const uint8_t> req, std::span<uint8_t> resp);
	Sizes cmd_swj_clock(std::span<const uint8_t> req, std::span<uint8_t> resp);
	Sizes cmd_swj_sequence(std::span<const uint8_t> req, std::span<uint8_t> resp);
	Sizes cmd_swd_configure(std::span<const uint8_t> req, std::span<uint8_t> resp);
	Sizes cmd_swd_sequence(std::span<const uint8_t> req, std::span<uint8_t> resp);
	Sizes cmd_transfer_configure(std::span<const uint8_t> req, std::span<uint8_t> resp);
	Sizes cmd_transfer(std::span<const uint8_t> req, std::span<uint8_t> resp);
	Sizes cmd_transfer_block(std::span<const uint8_t> req, std::span<uint8_t> resp);
	Sizes cmd_write_abort(std::span<const uint8_t> req, std::span<uint8_t> resp);

	/* WAIT-retry wrapper around driver_.transfer() honoring retry_count_
	 * and the abort flag. */
	uint8_t transfer_retry(uint8_t request, uint32_t *data);

	SwdDriver &driver_;
	HostStatus &host_;
	VendorHandler *vendor_ = nullptr;
	Config config_;

	bool connected_ = false;
	uint8_t idle_cycles_ = 0; /* set by TransferConfigure and forwarded to the driver via set_idle_cycles() */
	uint16_t retry_count_ = 100;
	uint16_t match_retry_ = 0;
	uint32_t match_mask_ = 0;
	std::atomic<bool> abort_{false};
};

} // namespace dap
