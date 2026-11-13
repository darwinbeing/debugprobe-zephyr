#include "testing.hpp"
#include "msp430/protocol.hpp"

using namespace msp430;

/* The values of the design spec's vendor-command table (phase B). */
TEST(protocol_debug_values_match_the_spec)
{
	CHECK_EQ(static_cast<int>(Op::DbgAttach), 0x09);
	CHECK_EQ(static_cast<int>(Op::DbgDetach), 0x0A);
	CHECK_EQ(static_cast<int>(Op::DbgHalt), 0x0B);
	CHECK_EQ(static_cast<int>(Op::DbgResume), 0x0C);
	CHECK_EQ(static_cast<int>(Op::DbgStatus), 0x0D);
	CHECK_EQ(static_cast<int>(Op::DbgStep), 0x0E);
	CHECK_EQ(static_cast<int>(Op::DbgReadRegs), 0x0F);
	CHECK_EQ(static_cast<int>(Op::DbgWriteReg), 0x10);
	CHECK_EQ(static_cast<int>(Op::DbgBpSet), 0x11);
	CHECK_EQ(static_cast<int>(Op::DbgBpClear), 0x12);
	CHECK_EQ(static_cast<int>(Status::Protected), 0x09);
	CHECK_EQ(static_cast<int>(Status::Running), 0x0A);
	CHECK_EQ(static_cast<int>(State::DebugHalted), 3);
	CHECK_EQ(static_cast<int>(State::DebugRunning), 4);
	CHECK_EQ(static_cast<int>(StopReason::Request), 1);
	CHECK_EQ(static_cast<int>(StopReason::Breakpoint), 2);
	CHECK_EQ(static_cast<int>(StopReason::Step), 3);
	CHECK_EQ(static_cast<int>(StopReason::Reset), 4);
	CHECK_EQ(kFeatureDebug, 2u);
	CHECK_EQ(kHwBreakpoints, 8u);
	/* MSPDS JTAG_defs.h, bit-reversed like the other IR constants */
	CHECK_EQ(kIrEmexDataExchange32, 0xB0);
	CHECK_EQ(kIrEmexReadControl, 0xD0);
	CHECK_EQ(kIrEmexWriteControl, 0x30);
}
