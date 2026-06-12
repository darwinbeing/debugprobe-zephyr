#pragma once
/* CMSIS-DAP v2 protocol constants.
 * Values follow the ARM reference DAP.h; behavioral quirks follow the
 * Raspberry Pi-patched reference used by the original debugprobe firmware.
 */
#include <cstdint>

namespace dap {

/* Command IDs */
inline constexpr uint8_t kCmdInfo = 0x00;
inline constexpr uint8_t kCmdHostStatus = 0x01;
inline constexpr uint8_t kCmdConnect = 0x02;
inline constexpr uint8_t kCmdDisconnect = 0x03;
inline constexpr uint8_t kCmdTransferConfigure = 0x04;
inline constexpr uint8_t kCmdTransfer = 0x05;
inline constexpr uint8_t kCmdTransferBlock = 0x06;
inline constexpr uint8_t kCmdTransferAbort = 0x07;
inline constexpr uint8_t kCmdWriteAbort = 0x08;
inline constexpr uint8_t kCmdDelay = 0x09;
inline constexpr uint8_t kCmdResetTarget = 0x0A;
inline constexpr uint8_t kCmdSwjPins = 0x10;
inline constexpr uint8_t kCmdSwjClock = 0x11;
inline constexpr uint8_t kCmdSwjSequence = 0x12;
inline constexpr uint8_t kCmdSwdConfigure = 0x13;
inline constexpr uint8_t kCmdJtagSequence = 0x14;
inline constexpr uint8_t kCmdJtagConfigure = 0x15;
inline constexpr uint8_t kCmdJtagIdcode = 0x16;
inline constexpr uint8_t kCmdSwdSequence = 0x1D;
inline constexpr uint8_t kCmdQueueCommands = 0x7E;
inline constexpr uint8_t kCmdExecuteCommands = 0x7F;
inline constexpr uint8_t kCmdVendorFirst = 0x80;
inline constexpr uint8_t kCmdVendorLast = 0x9F;
inline constexpr uint8_t kCmdInvalid = 0xFF;

/* Command status byte */
inline constexpr uint8_t kDapOk = 0x00;
inline constexpr uint8_t kDapError = 0xFF;

/* DAP_Info IDs */
inline constexpr uint8_t kInfoVendor = 0x01;
inline constexpr uint8_t kInfoProduct = 0x02;
inline constexpr uint8_t kInfoSerial = 0x03;
inline constexpr uint8_t kInfoFwVersion = 0x04;
inline constexpr uint8_t kInfoDeviceVendor = 0x05;
inline constexpr uint8_t kInfoDeviceName = 0x06;
inline constexpr uint8_t kInfoCapabilities = 0xF0;
inline constexpr uint8_t kInfoTimestampClock = 0xF1;
inline constexpr uint8_t kInfoSwoBufferSize = 0xFD;
inline constexpr uint8_t kInfoPacketCount = 0xFE;
inline constexpr uint8_t kInfoPacketSize = 0xFF;

/* Transfer request byte bits */
inline constexpr uint8_t kReqApNdp = 1U << 0;
inline constexpr uint8_t kReqRnW = 1U << 1;
inline constexpr uint8_t kReqA2 = 1U << 2;
inline constexpr uint8_t kReqA3 = 1U << 3;
inline constexpr uint8_t kReqMatchValue = 1U << 4;
inline constexpr uint8_t kReqMatchMask = 1U << 5;
inline constexpr uint8_t kReqTimestamp = 1U << 7;

/* DP RDBUFF read request (used to flush posted AP reads) */
inline constexpr uint8_t kReqReadRdbuff = 0x0C | kReqRnW; /* DP addr 0xC, read */

/* Transfer response value: ACK in bits 0-2, then error/mismatch flags */
inline constexpr uint8_t kXferOk = 0x01;
inline constexpr uint8_t kXferWait = 0x02;
inline constexpr uint8_t kXferFault = 0x04;
inline constexpr uint8_t kXferNoAck = 0x07;
inline constexpr uint8_t kXferProtocolError = 0x08;
inline constexpr uint8_t kXferMismatch = 0x10;

/* SWJ pin bit positions */
inline constexpr uint8_t kPinSwclk = 1U << 0;
inline constexpr uint8_t kPinSwdio = 1U << 1;
inline constexpr uint8_t kPinTdi = 1U << 2;
inline constexpr uint8_t kPinTdo = 1U << 3;
inline constexpr uint8_t kPinNtrst = 1U << 5;
inline constexpr uint8_t kPinNreset = 1U << 7;

/* DAP_Connect port values */
inline constexpr uint8_t kPortDefault = 0;
inline constexpr uint8_t kPortSwd = 1;
inline constexpr uint8_t kPortJtag = 2;
inline constexpr uint8_t kPortDisabled = 0; /* response byte on refusal */

} // namespace dap
