#include "msp430/session.hpp"

namespace msp430 {

namespace {

uint16_t get16(std::span<const uint8_t> b, size_t off)
{
	return static_cast<uint16_t>(b[off] | (b[off + 1] << 8));
}

uint32_t get32(std::span<const uint8_t> b, size_t off)
{
	return static_cast<uint32_t>(b[off]) | (static_cast<uint32_t>(b[off + 1]) << 8) |
	       (static_cast<uint32_t>(b[off + 2]) << 16) |
	       (static_cast<uint32_t>(b[off + 3]) << 24);
}

void put16(std::span<uint8_t> b, size_t off, uint16_t v)
{
	b[off] = static_cast<uint8_t>(v);
	b[off + 1] = static_cast<uint8_t>(v >> 8);
}

void put32(std::span<uint8_t> b, size_t off, uint32_t v)
{
	put16(b, off, static_cast<uint16_t>(v));
	put16(b, off + 2, static_cast<uint16_t>(v >> 16));
}

/* Even address, and [addr, addr + 2 * nwords) inside the 20-bit space. */
bool range_ok(uint32_t addr, uint64_t nwords)
{
	return (addr & 1u) == 0 && addr < kAddressSpace &&
	       addr + 2u * nwords <= kAddressSpace;
}

} // namespace

size_t Session::handle(std::span<const uint8_t> request, std::span<uint8_t> response)
{
	if (request.empty() || request[0] != kVendorCmd || response.size() < 2) {
		return 0;
	}
	response[0] = kVendorCmd;
	if (request.size() < 2) {
		response[1] = static_cast<uint8_t>(Status::BadArg);
		return 2;
	}

	const In args = request.subspan(2);
	const Out out = response.subspan(2);
	size_t n = 0;
	Status st = Status::UnknownOp;

	switch (static_cast<Op>(request[1])) {
	case Op::Info:
		st = op_info(out, n);
		break;
	case Op::Connect:
		st = op_connect(args, out, n);
		break;
	case Op::Disconnect:
		st = op_disconnect();
		break;
	case Op::ReadMem:
		st = op_read_mem(args, out, n);
		break;
	case Op::WriteMem:
		st = op_write_mem(args);
		break;
	case Op::Erase:
		st = op_erase(args);
		break;
	case Op::FlashBegin:
		st = op_flash_begin(args);
		break;
	case Op::FlashData:
		st = op_flash_data(args);
		break;
	case Op::FlashEnd:
		st = op_flash_end();
		break;
	default:
		break;
	}

	/* The target is in an unknown state (lost synchronization). Close the
	 * session so the pins are released; the host must connect again. */
	if (st == Status::TargetError && state_ != State::Idle) {
		abort();
	}

	response[1] = static_cast<uint8_t>(st);
	return 2 + n;
}

void Session::abort()
{
	if (state_ == State::Flashing) {
		cpu_.flash_end(false);
	}
	if (state_ != State::Idle) {
		cpu_.disconnect();
	}
	state_ = State::Idle;
	flash_remaining_ = 0;
}

Status Session::op_info(Out out, size_t &n)
{
	if (out.size() < 8) {
		return Status::BadArg;
	}
	out[0] = kProtocolVersion;
	out[1] = static_cast<uint8_t>(state_);
	put16(out, 2, max_data_bytes_);
	put32(out, 4, kFeatureCpuxv2Flash);
	n = 8;
	return Status::Ok;
}

Status Session::op_connect(In args, Out out, size_t &n)
{
	if (args.size() < 3 || out.size() < 9) {
		return Status::BadArg;
	}
	const uint16_t speed_khz = get16(args, 0);
	const uint8_t mode = args[2];
	if (speed_khz < kMinSpeedKhz || speed_khz > kMaxSpeedKhz || mode > 1) {
		return Status::BadArg;
	}

	abort(); /* a new CONNECT always starts from a clean entry */

	Identity id;
	const Status st = cpu_.connect(static_cast<EntryMode>(mode), speed_khz * 1000u, id);
	if (st == Status::Ok) {
		state_ = State::Connected;
		out[0] = id.jtag_id;
		put16(out, 1, id.core_id);
		put32(out, 3, id.device_id_ptr);
		put16(out, 7, id.device_id);
		n = 9;
	} else if (st == Status::NoTarget) {
		out[0] = id.jtag_id;
		n = 1;
	}
	return st;
}

Status Session::op_disconnect()
{
	if (state_ == State::Idle) {
		return Status::BadState;
	}
	abort();
	return Status::Ok;
}

Status Session::op_read_mem(In args, Out out, size_t &n)
{
	if (state_ != State::Connected) {
		return Status::BadState;
	}
	if (args.size() < 6) {
		return Status::BadArg;
	}
	const uint32_t addr = get32(args, 0);
	const size_t nwords = get16(args, 4);
	const size_t nbytes = 2 * nwords;
	if (nwords == 0 || !range_ok(addr, nwords) || nbytes > max_data_bytes_ ||
	    nbytes > out.size()) {
		return Status::BadArg;
	}
	const Status st = cpu_.read_mem(addr, nwords, out.data());
	if (st == Status::Ok) {
		n = nbytes;
	}
	return st;
}

Status Session::op_write_mem(In args)
{
	if (state_ != State::Connected) {
		return Status::BadState;
	}
	if (args.size() < 4) {
		return Status::BadArg;
	}
	const uint32_t addr = get32(args, 0);
	const In data = args.subspan(4);
	if (data.empty() || (data.size() & 1u) != 0 || data.size() > max_data_bytes_ ||
	    !range_ok(addr, data.size() / 2)) {
		return Status::BadArg;
	}
	return cpu_.write_mem(addr, data.size() / 2, data.data());
}

Status Session::op_erase(In args)
{
	if (state_ != State::Connected) {
		return Status::BadState;
	}
	if (args.size() < 9) {
		return Status::BadArg;
	}
	const uint8_t mode = args[0];
	const uint32_t addr = get32(args, 1);
	const uint32_t ram = get32(args, 5);
	if (mode > 2 || !range_ok(addr, 1) || !range_ok(ram, 1)) {
		return Status::BadArg;
	}
	return cpu_.erase(static_cast<EraseMode>(mode), addr, ram);
}

Status Session::op_flash_begin(In args)
{
	if (state_ != State::Connected) {
		return Status::BadState;
	}
	if (args.size() < 12) {
		return Status::BadArg;
	}
	const uint32_t addr = get32(args, 0);
	const uint32_t nwords = get32(args, 4);
	const uint32_t ram = get32(args, 8);
	if (nwords == 0 || !range_ok(addr, nwords) || !range_ok(ram, 1)) {
		return Status::BadArg;
	}
	const Status st = cpu_.flash_begin(addr, nwords, ram);
	if (st == Status::Ok) {
		state_ = State::Flashing;
		flash_remaining_ = nwords;
	}
	return st;
}

Status Session::op_flash_data(In args)
{
	if (state_ != State::Flashing) {
		return Status::BadState;
	}
	const size_t nwords = args.size() / 2;
	if (args.empty() || (args.size() & 1u) != 0 || args.size() > max_data_bytes_ ||
	    nwords > flash_remaining_) {
		/* The funclet cannot be resynchronized: abandon the write. */
		cpu_.flash_end(false);
		state_ = State::Connected;
		return Status::BadArg;
	}
	const Status st = cpu_.flash_data(nwords, args.data());
	if (st == Status::Ok) {
		flash_remaining_ -= static_cast<uint32_t>(nwords);
	} else {
		state_ = State::Connected; /* Cpuxv2 already cleaned up */
	}
	return st;
}

Status Session::op_flash_end()
{
	if (state_ != State::Flashing) {
		return Status::BadState;
	}
	const bool complete = flash_remaining_ == 0;
	const Status st = cpu_.flash_end(complete);
	state_ = State::Connected;
	if (!complete && st == Status::Ok) {
		return Status::BadState;
	}
	return st;
}

} // namespace msp430
