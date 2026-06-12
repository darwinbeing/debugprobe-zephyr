#include "dap/engine.hpp"

#include <cstring>

#include "dap/constants.hpp"

namespace dap {

namespace {

size_t string_info(const char *s, uint8_t *out, size_t cap)
{
	if (s == nullptr) {
		return 0;
	}
	size_t len = std::strlen(s) + 1; /* include NUL, like the reference */
	if (len > 255 || len + 2 > cap) {
		return 0;
	}
	std::memcpy(out, s, len);
	return len;
}

} // namespace

DapEngine::DapEngine(SwdDriver &driver, HostStatus &host, const Config &config)
	: driver_(driver), host_(host), config_(config)
{
}

size_t DapEngine::process(std::span<const uint8_t> request, std::span<uint8_t> response)
{
	if (request.empty() || response.size() < 2) {
		if (!response.empty()) {
			response[0] = kCmdInvalid;
			return 1;
		}
		return 0;
	}

	if (request[0] == kCmdExecuteCommands) {
		if (request.size() < 2) {
			response[0] = kCmdInvalid;
			return 1;
		}
		size_t cnt = request[1];
		response[0] = kCmdExecuteCommands;
		response[1] = static_cast<uint8_t>(cnt);
		size_t req_off = 2, resp_off = 2;
		while (cnt--) {
			Sizes s = dispatch(request.subspan(req_off),
					   response.subspan(resp_off));
			req_off += s.consumed;
			resp_off += s.written;
			if (req_off >= request.size() && cnt) {
				break; /* malformed batch: stop, report what ran */
			}
		}
		return resp_off;
	}

	return dispatch(request, response).written;
}

DapEngine::Sizes DapEngine::dispatch(std::span<const uint8_t> req, std::span<uint8_t> resp)
{
	const uint8_t cmd = req[0];

	if (cmd >= kCmdVendorFirst && cmd <= kCmdVendorLast) {
		resp[0] = cmd; /* default DAP_vendor.c: echo only */
		return {req.size(), 1};
	}

	resp[0] = cmd;
	auto body_req = req.subspan(1);
	auto body_resp = resp.subspan(1);

	Sizes s;
	switch (cmd) {
	case kCmdInfo:
		s = cmd_info(body_req, body_resp);
		break;
	case kCmdHostStatus:
		s = cmd_host_status(body_req, body_resp);
		break;
	case kCmdConnect:
		s = cmd_connect(body_req, body_resp);
		break;
	case kCmdDisconnect:
		s = cmd_disconnect(body_resp);
		break;
	case kCmdDelay:
		s = cmd_delay(body_req, body_resp);
		break;
	case kCmdResetTarget:
		s = cmd_reset_target(body_resp);
		break;
	case kCmdSwjPins:
		s = cmd_swj_pins(body_req, body_resp);
		break;
	case kCmdSwjClock:
		s = cmd_swj_clock(body_req, body_resp);
		break;
	case kCmdSwjSequence:
		s = cmd_swj_sequence(body_req, body_resp);
		break;
	case kCmdSwdConfigure:
		s = cmd_swd_configure(body_req, body_resp);
		break;
	case kCmdSwdSequence:
		s = cmd_swd_sequence(body_req, body_resp);
		break;
	case kCmdJtagSequence: /* JTAG not available on this probe */
		body_resp[0] = kDapError;
		s = {body_req.size(), 1};
		break;
	case kCmdJtagConfigure:
		body_resp[0] = kDapError;
		s = {body_req.size(), 1};
		break;
	case kCmdJtagIdcode: /* JTAG compiled out: 1-byte error (DAP.c id_error) */
		body_resp[0] = kDapError;
		s = {body_req.size(), 1};
		break;
	case kCmdTransferConfigure:
		s = cmd_transfer_configure(body_req, body_resp);
		break;
	case kCmdTransfer:
		s = cmd_transfer(body_req, body_resp);
		break;
	case kCmdTransferBlock:
		s = cmd_transfer_block(body_req, body_resp);
		break;
	case kCmdWriteAbort:
		s = cmd_write_abort(body_req, body_resp);
		break;
	default:
		resp[0] = kCmdInvalid;
		return {req.size(), 1};
	}

	return {1 + s.consumed, 1 + s.written};
}

DapEngine::Sizes DapEngine::cmd_info(std::span<const uint8_t> req, std::span<uint8_t> resp)
{
	uint8_t *payload = resp.data() + 1;
	size_t cap = resp.size();
	size_t len = 0;

	if (req.empty()) {
		/* Truncated request: return empty info (len=0). */
		resp[0] = 0;
		return {0u, 1};
	}

	uint8_t id = req[0];
	switch (id) {
	case kInfoVendor:
		len = string_info(config_.vendor, payload, cap);
		break;
	case kInfoProduct:
		len = string_info(config_.product, payload, cap);
		break;
	case kInfoSerial:
		len = string_info(config_.serial, payload, cap);
		break;
	case kInfoFwVersion:
		len = string_info(config_.fw_version, payload, cap);
		break;
	case kInfoCapabilities:
		payload[0] = 0x01 /* SWD */ | 0x10 /* atomic */ |
			     (config_.timestamp_clock ? 0x20 : 0x00);
		len = 1;
		break;
	case kInfoTimestampClock:
		if (config_.timestamp_clock != 0) {
			payload[0] = static_cast<uint8_t>(config_.timestamp_clock);
			payload[1] = static_cast<uint8_t>(config_.timestamp_clock >> 8);
			payload[2] = static_cast<uint8_t>(config_.timestamp_clock >> 16);
			payload[3] = static_cast<uint8_t>(config_.timestamp_clock >> 24);
			len = 4;
		}
		break;
	case kInfoPacketSize:
		payload[0] = static_cast<uint8_t>(config_.packet_size);
		payload[1] = static_cast<uint8_t>(config_.packet_size >> 8);
		len = 2;
		break;
	case kInfoPacketCount:
		payload[0] = config_.packet_count;
		len = 1;
		break;
	default: /* device vendor/name, SWO buffer, unknown: empty */
		break;
	}

	resp[0] = static_cast<uint8_t>(len);
	return {1u, 1 + len};
}

DapEngine::Sizes DapEngine::cmd_host_status(std::span<const uint8_t> req,
					    std::span<uint8_t> resp)
{
	if (req.size() < 2) {
		resp[0] = kDapError;
		return {req.size(), 1};
	}
	const bool on = req[1] != 0;
	switch (req[0]) {
	case 0:
		host_.connected(on);
		resp[0] = kDapOk;
		break;
	case 1:
		host_.running(on);
		resp[0] = kDapOk;
		break;
	default:
		resp[0] = kDapError;
		break;
	}
	return {2, 1};
}

DapEngine::Sizes DapEngine::cmd_delay(std::span<const uint8_t> req, std::span<uint8_t> resp)
{
	if (req.size() < 2) {
		resp[0] = kDapError;
		return {req.size(), 1};
	}
	driver_.delay_us(static_cast<uint32_t>(req[0]) |
			 (static_cast<uint32_t>(req[1]) << 8));
	resp[0] = kDapOk;
	return {2, 1};
}

} // namespace dap
