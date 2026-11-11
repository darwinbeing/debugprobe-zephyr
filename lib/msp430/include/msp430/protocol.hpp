#pragma once
/* Wire-level constants of the MSP430 SBW vendor command and the JTAG
 * instruction set of CPUXv2 devices. No dependencies.
 */
#include <cstdint>

namespace msp430 {

/* CMSIS-DAP vendor command that carries this protocol (ID_DAP_Vendor16). */
inline constexpr uint8_t kVendorCmd = 0x90;
inline constexpr uint8_t kProtocolVersion = 1;
inline constexpr uint32_t kFeatureCpuxv2Flash = 1u << 0;

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
};

enum class State : uint8_t { Idle = 0, Connected = 1, Flashing = 2 };
enum class EntryMode : uint8_t { Normal = 0, MagicPattern = 1 };
enum class EraseMode : uint8_t { Segment = 0, Bank = 1, Main = 2 };

inline constexpr uint16_t kMinSpeedKhz = 100;
inline constexpr uint16_t kMaxSpeedKhz = 1000;
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

inline constexpr uint8_t kJtagId91 = 0x91;

/* JTAG mailbox control bits. */
inline constexpr uint16_t kJmbIn0Rdy = 0x0001;
inline constexpr uint16_t kJmbInReq = 0x0001;
inline constexpr uint16_t kJmbOutReq = 0x0004;
inline constexpr uint16_t kJmbOut1Rdy = 0x0008;
inline constexpr uint16_t kJmb32Bit = 0x0010;

} // namespace msp430
