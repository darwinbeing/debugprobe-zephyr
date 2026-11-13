#include "msp430/session.hpp"

#include "msp430/funclets.hpp"

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

/* Read-only debug sessions may write the peripheral space except the flash
 * controller, and RAM. */
constexpr uint32_t kPeripheralEnd = 0x1000;
constexpr uint32_t kFlashCtlStart = 0x0140; /* FCTL1..FCTL4 */
constexpr uint32_t kFlashCtlEnd = 0x0150;

/* RAM of the devices the probe knows, by device ID. A read-only session may
 * write only RAM that lies inside both this range and the one the host
 * passes with DBG_ATTACH, so a wrong host table cannot open flash. */
struct KnownRam {
	uint16_t device_id;
	uint32_t lo, hi; /* [lo, hi) */
};
constexpr KnownRam kKnownRam[] = {
	{0x2955, 0x1C00, 0x4400}, /* MSP430F5529: USB RAM + RAM */
};

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
		st = op_info(args, out, n);
		break;
	case Op::Connect:
		st = op_connect(args, out, n);
		break;
	case Op::Disconnect:
		st = op_disconnect(args);
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
		st = op_flash_end(args);
		break;
	case Op::DbgAttach:
		st = op_dbg_attach(args, out, n);
		break;
	case Op::DbgDetach:
		st = op_dbg_detach(args);
		break;
	case Op::DbgHalt:
		st = op_dbg_halt(args, out, n);
		break;
	case Op::DbgResume:
		st = op_dbg_resume(args);
		break;
	case Op::DbgStatus:
		st = op_dbg_status(args, out, n);
		break;
	case Op::DbgStep:
		st = op_dbg_step(args, out, n);
		break;
	case Op::DbgReadRegs:
		st = op_dbg_read_regs(args, out, n);
		break;
	case Op::DbgWriteReg:
		st = op_dbg_write_reg(args);
		break;
	case Op::DbgBpSet:
		st = op_dbg_bp_set(args);
		break;
	case Op::DbgBpClear:
		st = op_dbg_bp_clear(args);
		break;
	default:
		break;
	}

	/* The debugger may have stopped or lost the CPU during the command. A
	 * TargetError from poll(), halt() or step() means it has already ended
	 * the session (EEM off, CPU let go, pins released): Idle, nothing more
	 * is sent. */
	if (debugging()) {
		if (!dbg_.attached()) {
			state_ = State::Idle;
			ram_lo_ = ram_hi_ = 0;
		} else {
			state_ = dbg_.running() ? State::DebugRunning : State::DebugHalted;
		}
	}
	/* The target is in an unknown state (lost synchronization). Close the
	 * session so the pins are released; the host must connect again. A
	 * debugger still attached here (the CPU left JTAG control during a
	 * memory, register or breakpoint access) is detached with Run, as
	 * Debugger::attach does for the same failure. */
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
	if (debugging()) {
		dbg_.detach(DetachAction::Run);
	} else if (state_ != State::Idle) {
		cpu_.disconnect();
	}
	state_ = State::Idle;
	flash_remaining_ = 0;
	ram_lo_ = ram_hi_ = 0;
}

bool Session::writable(uint32_t addr, uint32_t nbytes) const
{
	const uint64_t end = static_cast<uint64_t>(addr) + nbytes;
	const bool peripheral = end <= kPeripheralEnd &&
				(end <= kFlashCtlStart || addr >= kFlashCtlEnd);
	const bool ram = addr >= ram_lo_ && end <= ram_hi_;
	return peripheral || ram;
}

Status Session::op_info(In args, Out out, size_t &n)
{
	if (!args.empty() || out.size() < 9) {
		return Status::BadArg;
	}
	out[0] = kProtocolVersion;
	out[1] = static_cast<uint8_t>(state_);
	put16(out, 2, max_data_bytes_);
	put32(out, 4, kFeatureCpuxv2Flash | kFeatureDebug);
	out[8] = static_cast<uint8_t>(kHwBreakpoints);
	n = 9;
	return Status::Ok;
}

Status Session::op_connect(In args, Out out, size_t &n)
{
	if (args.size() != 3 || out.size() < 9) {
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

Status Session::op_disconnect(In args)
{
	if (state_ == State::Idle || debugging()) {
		return Status::BadState; /* a debug session ends with DBG_DETACH */
	}
	if (!args.empty()) {
		return Status::BadArg;
	}
	abort();
	return Status::Ok;
}

Status Session::op_read_mem(In args, Out out, size_t &n)
{
	if (state_ == State::DebugRunning) {
		return Status::Running;
	}
	if (state_ != State::Connected && state_ != State::DebugHalted) {
		return Status::BadState;
	}
	if (args.size() != 6) {
		return Status::BadArg;
	}
	const uint32_t addr = get32(args, 0);
	const size_t nwords = get16(args, 4);
	const size_t nbytes = 2 * nwords;
	if (nwords == 0 || !range_ok(addr, nwords) || nbytes > max_data_bytes_ ||
	    nbytes > out.size()) {
		return Status::BadArg;
	}
	const Status st = state_ == State::DebugHalted ? dbg_.read_mem(addr, nwords, out.data())
						       : cpu_.read_mem(addr, nwords, out.data());
	if (st == Status::Ok) {
		n = nbytes;
	}
	return st;
}

Status Session::op_write_mem(In args)
{
	if (state_ == State::DebugRunning) {
		return Status::Running;
	}
	if (state_ != State::Connected && state_ != State::DebugHalted) {
		return Status::BadState;
	}
	if (args.size() < 4) {
		return Status::BadArg;
	}
	const uint32_t addr = get32(args, 0);
	const In data = args.subspan(4);
	if (data.empty() || (data.size() & 1u) != 0 || data.size() > max_data_bytes_ ||
	    (addr & 1u) != 0) {
		return Status::BadArg;
	}
	if (state_ == State::DebugHalted) {
		/* Before the range check: a range that leaves the 20-bit space
		 * lies outside every window, so it is refused as PROTECTED. */
		if (!writable(addr, static_cast<uint32_t>(data.size()))) {
			return Status::Protected;
		}
	}
	if (!range_ok(addr, data.size() / 2)) {
		return Status::BadArg;
	}
	if (state_ == State::DebugHalted) {
		return dbg_.write_mem(addr, data.size() / 2, data.data());
	}
	return cpu_.write_mem(addr, data.size() / 2, data.data());
}

Status Session::op_erase(In args)
{
	if (debugging()) {
		return Status::Protected; /* no flash writes in a debug session */
	}
	if (state_ != State::Connected) {
		return Status::BadState;
	}
	if (args.size() != 9) {
		return Status::BadArg;
	}
	const uint8_t mode = args[0];
	const uint32_t addr = get32(args, 1);
	const uint32_t ram = get32(args, 5);
	if (mode > 2 || !range_ok(addr, 1) || !range_ok(ram, kFlashEraseWords)) {
		return Status::BadArg;
	}
	return cpu_.erase(static_cast<EraseMode>(mode), addr, ram);
}

Status Session::op_flash_begin(In args)
{
	if (debugging()) {
		return Status::Protected; /* no flash writes in a debug session */
	}
	if (state_ != State::Connected) {
		return Status::BadState;
	}
	if (args.size() != 12) {
		return Status::BadArg;
	}
	const uint32_t addr = get32(args, 0);
	const uint32_t nwords = get32(args, 4);
	const uint32_t ram = get32(args, 8);
	if (nwords == 0 || !range_ok(addr, nwords) || !range_ok(ram, kFlashWriteWords)) {
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
		/* Protocol rule: any FLASH_DATA error ends the write (the
		 * funclet itself could have continued). */
		const Status cleanup = cpu_.flash_end(false);
		state_ = State::Connected;
		flash_remaining_ = 0;
		return cleanup == Status::TargetError ? cleanup : Status::BadArg;
	}
	const Status st = cpu_.flash_data(nwords, args.data());
	if (st == Status::Ok) {
		flash_remaining_ -= static_cast<uint32_t>(nwords);
	} else {
		state_ = State::Connected; /* Cpuxv2 already cleaned up */
		flash_remaining_ = 0;
	}
	return st;
}

Status Session::op_flash_end(In args)
{
	if (state_ != State::Flashing) {
		return Status::BadState;
	}
	if (!args.empty()) {
		return Status::BadArg;
	}
	const bool complete = flash_remaining_ == 0;
	const Status st = cpu_.flash_end(complete);
	state_ = State::Connected;
	flash_remaining_ = 0;
	if (!complete && st == Status::Ok) {
		return Status::BadState;
	}
	return st;
}

Status Session::op_dbg_attach(In args, Out out, size_t &n)
{
	/* Not in Connected and Flashing: a Phase A funclet may still be running
	 * on the target, and Cpuxv2::identify() with the normal entry would not
	 * stop it. */
	if (state_ == State::Connected || state_ == State::Flashing) {
		return Status::BadState;
	}
	if (args.size() != 12 || out.size() < 15) {
		return Status::BadArg;
	}
	const uint8_t mode = args[0];
	const uint8_t flags = args[1];
	const uint16_t speed_khz = get16(args, 2);
	const uint32_t ram_lo = get32(args, 4);
	const uint32_t ram_hi = get32(args, 8);
	/* flags bit 0 (full mode) belongs to phase B2 and is refused here. */
	if (mode > 1 || flags != 0 || speed_khz < kMinSpeedKhz || speed_khz > kMaxSpeedKhz ||
	    (ram_lo & 1u) != 0 || ram_lo >= ram_hi || ram_hi > kAddressSpace) {
		return Status::BadArg;
	}

	/* In DebugHalted and DebugRunning, Debugger::attach first ends the open
	 * session with DetachAction::Run (a running CPU is halted first; the
	 * program continues where it stopped), then attaches as from Idle. */
	Identity id;
	const Status st = dbg_.attach(static_cast<AttachMode>(mode), speed_khz * 1000u, id);
	if (st == Status::NoTarget) {
		out[0] = id.jtag_id;
		n = 1;
	}
	if (st != Status::Ok) {
		state_ = State::Idle; /* the debugger has released the pins */
		ram_lo_ = ram_hi_ = 0;
		return st;
	}
	state_ = State::DebugHalted;
	/* The writable RAM: the intersection of the host's range and the RAM
	 * the probe knows for the device; empty for a device it does not know. */
	ram_lo_ = ram_hi_ = 0;
	for (const KnownRam &k : kKnownRam) {
		if (k.device_id == id.device_id) {
			ram_lo_ = ram_lo > k.lo ? ram_lo : k.lo;
			ram_hi_ = ram_hi < k.hi ? ram_hi : k.hi;
		}
	}
	if (ram_lo_ >= ram_hi_) {
		ram_lo_ = ram_hi_ = 0;
	}
	out[0] = id.jtag_id;
	put16(out, 1, id.core_id);
	put32(out, 3, id.device_id_ptr);
	put16(out, 7, id.device_id);
	put32(out, 9, dbg_.pc());
	out[13] = static_cast<uint8_t>(dbg_.stop_reason());
	out[14] = static_cast<uint8_t>(kHwBreakpoints);
	n = 15;
	return Status::Ok;
}

Status Session::op_dbg_detach(In args)
{
	if (!debugging()) {
		return Status::BadState;
	}
	if (args.size() != 1 || args[0] > 1) {
		return Status::BadArg;
	}
	dbg_.detach(static_cast<DetachAction>(args[0]));
	state_ = State::Idle;
	ram_lo_ = ram_hi_ = 0;
	return Status::Ok;
}

Status Session::op_dbg_halt(In args, Out out, size_t &n)
{
	if (!debugging()) {
		return Status::BadState;
	}
	if (!args.empty() || out.size() < 4) {
		return Status::BadArg;
	}
	const Status st = dbg_.halt();
	if (st == Status::Ok) {
		put32(out, 0, dbg_.pc());
		n = 4;
	}
	return st;
}

Status Session::op_dbg_resume(In args)
{
	if (!debugging()) {
		return Status::BadState;
	}
	if (state_ == State::DebugRunning) {
		return Status::Running;
	}
	if (!args.empty()) {
		return Status::BadArg;
	}
	return dbg_.resume();
}

Status Session::op_dbg_status(In args, Out out, size_t &n)
{
	if (!debugging()) {
		return Status::BadState;
	}
	if (!args.empty() || out.size() < 7) {
		return Status::BadArg;
	}
	bool halted = false;
	const Status st = dbg_.poll(halted);
	if (st != Status::Ok) {
		return st; /* `halted` and the stop reason are valid on Ok only */
	}
	out[0] = halted ? 1 : 0;
	out[1] = static_cast<uint8_t>(halted ? dbg_.stop_reason() : StopReason::None);
	out[2] = halted ? dbg_.breakpoint_hit() : kNoBreakpoint;
	put32(out, 3, halted ? dbg_.pc() : 0);
	n = 7;
	return Status::Ok;
}

Status Session::op_dbg_step(In args, Out out, size_t &n)
{
	if (!debugging()) {
		return Status::BadState;
	}
	if (state_ == State::DebugRunning) {
		return Status::Running;
	}
	if (!args.empty() || out.size() < 4) {
		return Status::BadArg;
	}
	const Status st = dbg_.step();
	if (st == Status::Ok) {
		put32(out, 0, dbg_.pc());
		n = 4;
	}
	return st;
}

Status Session::op_dbg_read_regs(In args, Out out, size_t &n)
{
	if (!debugging()) {
		return Status::BadState;
	}
	if (state_ == State::DebugRunning) {
		return Status::Running;
	}
	if (!args.empty() || out.size() < 64) {
		return Status::BadArg;
	}
	uint32_t regs[16] = {};
	const Status st = dbg_.read_regs(regs);
	if (st == Status::Ok) {
		for (size_t i = 0; i < 16; ++i) {
			put32(out, 4 * i, regs[i]);
		}
		n = 64;
	}
	return st;
}

Status Session::op_dbg_write_reg(In args)
{
	if (!debugging()) {
		return Status::BadState;
	}
	if (state_ == State::DebugRunning) {
		return Status::Running;
	}
	if (args.size() != 5) {
		return Status::BadArg;
	}
	return dbg_.write_reg(args[0], get32(args, 1));
}

Status Session::op_dbg_bp_set(In args)
{
	if (!debugging()) {
		return Status::BadState;
	}
	if (state_ == State::DebugRunning) {
		return Status::Running;
	}
	if (args.size() != 5) {
		return Status::BadArg;
	}
	return dbg_.set_breakpoint(args[0], get32(args, 1));
}

Status Session::op_dbg_bp_clear(In args)
{
	if (!debugging()) {
		return Status::BadState;
	}
	if (state_ == State::DebugRunning) {
		return Status::Running;
	}
	if (args.size() != 1) {
		return Status::BadArg;
	}
	return dbg_.clear_breakpoint(args[0]);
}

} // namespace msp430
