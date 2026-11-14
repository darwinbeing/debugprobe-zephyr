#pragma once
/* Wire-level constants of the MSP430 SBW vendor command and the JTAG
 * instruction set of CPUXv2 devices. No dependencies.
 */
#include <cstdint>

namespace msp430 {

/* CMSIS-DAP vendor command that carries this protocol (ID_DAP_Vendor16). */
inline constexpr uint8_t kVendorCmd = 0x90;
inline constexpr uint8_t kProtocolVersion = 2;
inline constexpr uint32_t kFeatureCpuxv2Flash = 1u << 0;
inline constexpr uint32_t kFeatureDebug = 1u << 1;

enum class Op : uint8_t {
	Info = 0x00,
	Connect = 0x01,
	Disconnect = 0x02,
	ReadMem = 0x03,
	WriteMem = 0x04,
	Erase = 0x05,
	FlashBegin = 0x06,
	FlashData = 0x07,
	FlashEnd = 0x08,
	DbgAttach = 0x09,
	DbgDetach = 0x0A,
	DbgHalt = 0x0B,
	DbgResume = 0x0C,
	DbgStatus = 0x0D,
	DbgStep = 0x0E,
	DbgReadRegs = 0x0F,
	DbgWriteReg = 0x10,
	DbgBpSet = 0x11,
	DbgBpClear = 0x12,
	/* Outside a debug session: a read-only check of the 32-bit DR path
	 * (design spec correction 25). */
	DbgEemPeek = 0x13,
};

enum class Status : uint8_t {
	Ok = 0x00,
	BadArg = 0x01,
	BadState = 0x02,
	Busy = 0x03,
	NoTarget = 0x04,
	Locked = 0x05,
	TargetError = 0x06,
	Timeout = 0x07,
	UnknownOp = 0x08,
	/* refused in a read-only debug session: a write outside the
	 * peripheral/RAM window, ERASE, FLASH_BEGIN */
	Protected = 0x09,
	Running = 0x0A, /* the operation needs a halted CPU */
};

enum class State : uint8_t {
	Idle = 0,
	Connected = 1,
	Flashing = 2,
	DebugHalted = 3,
	DebugRunning = 4,
};
enum class EntryMode : uint8_t { Normal = 0, MagicPattern = 1 };
enum class EraseMode : uint8_t { Segment = 0, Bank = 1, Main = 2 };

/* Debugging (protocol version 2). */
enum class AttachMode : uint8_t { Running = 0, Reset = 1 };
enum class DetachAction : uint8_t { Run = 0, Reset = 1 };
enum class StopReason : uint8_t { None = 0, Request = 1, Breakpoint = 2, Step = 3, Reset = 4 };
/* EEM memory-bus triggers used as breakpoints. The MSP430F5529 has the
 * "large" EEM: 8 memory-bus triggers (MSPDS 3.3.1.3 TemplateDeviceDb,
 * MSP430Defaults.h: LargeTrigger; MSP430F55xx.cpp: LargeEemMode). */
inline constexpr unsigned kHwBreakpoints = 8;
inline constexpr uint8_t kNoBreakpoint = 0xFF; /* DBG_STATUS breakpoint_index: no breakpoint hit */
inline constexpr uint8_t kAllBreakpoints = 0xFF; /* DBG_BP_CLEAR index: every breakpoint */
static_assert(kHwBreakpoints < kNoBreakpoint, "a real breakpoint index is never 0xFF");
/* DBG_EEM_PEEK reads only the even EEM addresses 0x00 to this one: the value,
 * control, mask and combination registers of trigger blocks 0-7, whose reads
 * have no side effects. */
inline constexpr uint8_t kEemPeekLastAddr = 0x3E;

/* SBW clock (slot rate). The F552x accepts 0-20 MHz with a low phase of at
 * least 25 ns (datasheet, JTAG and Spy-Bi-Wire interface); 10 MHz keeps half
 * of that margin and is reachable by sbw.pio on both RP2040 and RP2350 (see
 * SBW_MAX_SLOT_HZ). What a board's RST/SBWTDIO wiring carries reliably is
 * found on the board (design spec, timing rules). */
inline constexpr uint16_t kMinSpeedKhz = 100;
inline constexpr uint16_t kMaxSpeedKhz = 10000;
inline constexpr uint32_t kAddressSpace = 0x100000; /* 20-bit addresses */

/* JTAG instructions, already bit-reversed so they can be shifted MSB first
 * (TI JTAGfunc430Xv2.h; the "original value" there is the datasheet value). */
inline constexpr uint8_t kIrCntrlSig16Bit = 0xC8;
inline constexpr uint8_t kIrCntrlSigCapture = 0x28;
inline constexpr uint8_t kIrCntrlSigRelease = 0xA8;
inline constexpr uint8_t kIrData16Bit = 0x82;
inline constexpr uint8_t kIrDataQuick = 0xC2;
inline constexpr uint8_t kIrAddr16Bit = 0xC1;
inline constexpr uint8_t kIrAddrCapture = 0x21;
inline constexpr uint8_t kIrDataToAddr = 0xA1;
inline constexpr uint8_t kIrDataCapture = 0x42;
inline constexpr uint8_t kIrCoreipId = 0xE8;
inline constexpr uint8_t kIrDeviceId = 0xE1;
inline constexpr uint8_t kIrJmbExchange = 0x86;
inline constexpr uint8_t kIrTestReg = 0x54;
inline constexpr uint8_t kIrTest3vReg = 0xF4;
/* EEM access (MSPDS Bios/src/hal/JTAG_defs.h, same bit order). */
inline constexpr uint8_t kIrEmexDataExchange32 = 0xB0;
inline constexpr uint8_t kIrEmexReadControl = 0xD0;
inline constexpr uint8_t kIrEmexWriteControl = 0x30;

inline constexpr uint8_t kJtagId91 = 0x91;

/* JTAG mailbox control bits. */
inline constexpr uint16_t kJmbIn0Rdy = 0x0001;
inline constexpr uint16_t kJmbInReq = 0x0001;
inline constexpr uint16_t kJmbOutReq = 0x0004;
inline constexpr uint16_t kJmbOut1Rdy = 0x0008;
inline constexpr uint16_t kJmb32Bit = 0x0010;

} // namespace msp430
