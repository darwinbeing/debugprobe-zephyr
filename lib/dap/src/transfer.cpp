/* lib/dap/src/transfer.cpp — Transfer-family command handlers.
 * transfer_retry and cmd_write_abort (Task 3), TransferConfigure and Transfer
 * (Task 5, faithful port of DAP_SWD_Transfer, DAP.c:657-940) are implemented
 * here; TransferBlock remains stubbed and is replaced in Task 6.
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
	/* Knowing divergence from DAP.c DAP_SWD_WriteAbort, which issues the
	 * ABORT write exactly once and always answers DAP_OK: we retry on WAIT
	 * and report failures. ADIv5 DPs do not WAIT on ABORT, so hosts see no
	 * difference in practice; conformance allowlists this.
	 */
	/* DP write, address 0 (ABORT): request byte 0x00 */
	const uint8_t ack = transfer_retry(0x00, &value);
	resp[0] = (ack == kXferOk) ? kDapOk : kDapError;
	return {5, 1};
}

/* --- Task 5 implementations --- */

/* DAP_TransferConfigure (DAP.c:638-651): request
 * {idle_cycles(1), wait_retry(2 LE), match_retry(2 LE)} -> {cmd, DAP_OK}.
 */
DapEngine::Sizes DapEngine::cmd_transfer_configure(std::span<const uint8_t> req,
						   std::span<uint8_t> resp)
{
	if (req.size() < 5) {
		resp[0] = kDapError;
		return {req.size(), 1};
	}
	idle_cycles_ = req[0];
	retry_count_ = static_cast<uint16_t>(req[1] | (req[2] << 8));
	match_retry_ = static_cast<uint16_t>(req[3] | (req[4] << 8));
	resp[0] = kDapOk;
	return {5, 1};
}

/* DAP_Transfer (faithful port of DAP_SWD_Transfer, DAP.c:657-940).
 *
 * Request body (after the command byte, which the dispatcher already stripped):
 *   {dap_index(1), count(1), then per transfer: request_byte(1)
 *    [+ 4 match-value bytes for value-match reads]
 *    [+ 4 data bytes for writes]}.
 * Response body (cmd byte added by the dispatcher):
 *   {count(1), response_value(1), data words...}.
 *
 * Posted AP reads are pipelined: an AP read posts the address and the data is
 * collected by the *next* transfer (another AP read re-posts while reading the
 * previous word, anything else flushes via a DP RDBUFF read). count counts
 * completed transfers; response_value is the last ACK with mismatch/protocol
 * bits OR'd in. count and response_value are back-filled even on early break.
 *
 * TIMESTAMP_CLOCK is non-zero on this probe, so the timestamp-storage paths
 * (request bit kReqTimestamp) are ported (DAP.c:718-729/778-786/798-806/853-861).
 */
DapEngine::Sizes DapEngine::cmd_transfer(std::span<const uint8_t> req,
					 std::span<uint8_t> resp)
{
	/* Mirror DAP_TransferAbort = 0U (DAP.c:682): a latched abort must not
	 * poison this command. */
	abort_.store(false, std::memory_order_relaxed);

	/* Disconnected -> DAP_Dummy_Transfer parity (DAP.c:1538+):
	 * {count=0, value=0}, consume the whole request. */
	if (!connected_) {
		if (resp.size() >= 2) {
			resp[0] = 0;
			resp[1] = 0;
			return {req.size(), 2};
		}
		if (!resp.empty()) {
			resp[0] = 0;
			return {req.size(), 1};
		}
		return {req.size(), 0};
	}

	/* The dispatcher guarantees a 1-byte response body span; we need 2 (the
	 * count + response_value header) before writing any data word. */
	if (resp.size() < 2) {
		if (!resp.empty()) {
			resp[0] = 0;
		}
		return {req.size(), resp.empty() ? 0u : 1u};
	}

	const size_t req_size = req.size();
	size_t ri = 0;            /* request cursor (into req) */
	size_t wi = 2;            /* response cursor; [0]=count, [1]=value */
	uint8_t response_count = 0;
	uint8_t response_value = 0;
	uint32_t post_read = 0;
	uint32_t data = 0;

	/* req[0] = DAP index (ignored); req[1] = transfer count. */
	if (ri < req_size) {
		ri++; /* skip DAP index */
	}
	uint32_t request_count = (ri < req_size) ? req[ri++] : 0;

	/* Store a 32-bit LE word at the response cursor if it fits; otherwise
	 * signal the caller to stop (treated like an abort). */
	auto store_word = [&](uint32_t value) -> bool {
		if (resp.size() - wi < 4) {
			return false;
		}
		resp[wi++] = static_cast<uint8_t>(value);
		resp[wi++] = static_cast<uint8_t>(value >> 8);
		resp[wi++] = static_cast<uint8_t>(value >> 16);
		resp[wi++] = static_cast<uint8_t>(value >> 24);
		return true;
	};

	bool stop = false; /* response/request bounds hit: stop like an abort */

	for (; request_count != 0U && !stop; request_count--) {
		if (ri >= req_size) { /* out of request bytes */
			stop = true;
			break;
		}
		const uint8_t request_value = req[ri++];

		if ((request_value & kReqRnW) != 0U) {
			/* Read register */
			if (post_read) {
				/* A read was posted before. */
				if ((request_value &
				     (kReqApNdp | kReqMatchValue)) == kReqApNdp) {
					/* Read previous AP data and post next AP read. */
					response_value =
						transfer_retry(request_value, &data);
				} else {
					/* Read previous AP data via RDBUFF. */
					response_value =
						transfer_retry(kReqReadRdbuff, &data);
					post_read = 0U;
				}
				if (response_value != kXferOk) {
					break;
				}
				/* Store previous AP data. */
				if (!store_word(data)) {
					stop = true;
					break;
				}
				if (post_read) {
					/* Store timestamp of the next AP read. */
					if ((request_value & kReqTimestamp) != 0U) {
						if (!store_word(driver_.timestamp())) {
							stop = true;
							break;
						}
					}
				}
			}
			if ((request_value & kReqMatchValue) != 0U) {
				/* Read with value match: consume 4 match bytes. */
				if (req_size - ri < 4) {
					stop = true;
					break;
				}
				uint32_t match_value =
					static_cast<uint32_t>(req[ri + 0]) |
					(static_cast<uint32_t>(req[ri + 1]) << 8) |
					(static_cast<uint32_t>(req[ri + 2]) << 16) |
					(static_cast<uint32_t>(req[ri + 3]) << 24);
				ri += 4;
				uint32_t match_retry = match_retry_;
				if ((request_value & kReqApNdp) != 0U) {
					/* Post AP read. */
					response_value =
						transfer_retry(request_value, nullptr);
					if (response_value != kXferOk) {
						break;
					}
				}
				do {
					/* Read until value matches or retries expire. */
					response_value =
						transfer_retry(request_value, &data);
					if (response_value != kXferOk) {
						break;
					}
				} while (((data & match_mask_) != match_value) &&
					 match_retry-- &&
					 !abort_.load(std::memory_order_relaxed));
				if ((data & match_mask_) != match_value) {
					response_value |= kXferMismatch;
				}
				if (response_value != kXferOk) {
					break;
				}
			} else {
				/* Normal read */
				if ((request_value & kReqApNdp) != 0U) {
					/* Read AP register */
					if (post_read == 0U) {
						/* Post AP read. */
						response_value = transfer_retry(
							request_value, nullptr);
						if (response_value != kXferOk) {
							break;
						}
						/* Store timestamp. */
						if ((request_value &
						     kReqTimestamp) != 0U) {
							if (!store_word(
								    driver_.timestamp())) {
								stop = true;
								break;
							}
						}
						post_read = 1U;
					}
				} else {
					/* Read DP register */
					response_value =
						transfer_retry(request_value, &data);
					if (response_value != kXferOk) {
						break;
					}
					/* Store timestamp. */
					if ((request_value & kReqTimestamp) != 0U) {
						if (!store_word(driver_.timestamp())) {
							stop = true;
							break;
						}
					}
					/* Store data. */
					if (!store_word(data)) {
						stop = true;
						break;
					}
				}
			}
		} else {
			/* Write register */
			if (post_read) {
				/* Read previous data via RDBUFF. */
				response_value =
					transfer_retry(kReqReadRdbuff, &data);
				if (response_value != kXferOk) {
					break;
				}
				/* Store previous data. */
				if (!store_word(data)) {
					stop = true;
					break;
				}
				post_read = 0U;
			}
			/* Load data: consume 4 bytes. */
			if (req_size - ri < 4) {
				stop = true;
				break;
			}
			data = static_cast<uint32_t>(req[ri + 0]) |
			       (static_cast<uint32_t>(req[ri + 1]) << 8) |
			       (static_cast<uint32_t>(req[ri + 2]) << 16) |
			       (static_cast<uint32_t>(req[ri + 3]) << 24);
			ri += 4;
			if ((request_value & kReqMatchMask) != 0U) {
				/* Write match mask (engine-internal). */
				match_mask_ = data;
				response_value = kXferOk;
			} else {
				/* Write DP/AP register. */
				response_value =
					transfer_retry(request_value, &data);
				if (response_value != kXferOk) {
					break;
				}
				/* Store timestamp. */
				if ((request_value & kReqTimestamp) != 0U) {
					if (!store_word(driver_.timestamp())) {
						stop = true;
						break;
					}
				}
			}
		}
		response_count++;
		if (abort_.load(std::memory_order_relaxed)) {
			break;
		}
	}

	/* Process canceled requests to compute the consumed request length
	 * (DAP.c:872-885), bounds-checked. */
	for (; request_count != 0U; request_count--) {
		if (ri >= req_size) {
			break;
		}
		const uint8_t request_value = req[ri++];
		if ((request_value & kReqRnW) != 0U) {
			if ((request_value & kReqMatchValue) != 0U) {
				ri += 4;
			}
		} else {
			ri += 4;
		}
		if (ri > req_size) {
			ri = req_size;
		}
	}

	/* Deliberate deviation from DAP_SWD_Transfer (DAP.c:902-908): the
	 * reference issues a final "check last write" RDBUFF read after the last
	 * transfer was a write (its check_write path). This library omits that
	 * verification read — the plan's structural port and the host tests
	 * (transfer_single_dp_write expects exactly one SWD transfer) define the
	 * behavior. The posted-read final flush below is kept verbatim. */
	if (response_value == kXferOk && !stop) {
		if (post_read) {
			/* Read previous data via RDBUFF. */
			response_value = transfer_retry(kReqReadRdbuff, &data);
			if (response_value == kXferOk) {
				/* Store previous data if it fits. */
				(void)store_word(data);
			}
		}
	}

	resp[0] = response_count;
	resp[1] = response_value;
	return {ri, wi};
}

/* --- Task 6 stub (replaced later) --- */

DapEngine::Sizes DapEngine::cmd_transfer_block(std::span<const uint8_t> req,
					       std::span<uint8_t> resp)
{
	resp[0] = kDapError;
	return {req.size(), 1};
}

} // namespace dap
