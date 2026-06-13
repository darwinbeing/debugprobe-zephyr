#pragma once

#include <cstdint>

namespace probe::usb {

/* usbd class instance name, referenced by usbd_register_class */
inline constexpr const char *kDapClassName = "dap_v2_0";

/* DAP command/response buffer size in bytes.  Deliberately larger than the
 * 64-byte USB full-speed bulk max-packet: openocd packs each DAP_TransferBlock
 * up to this size, so one command carries ~125 words instead of ~15.  That
 * amortizes the fixed per-command USB/usbd-thread overhead (the real SWD
 * throughput bottleneck on this stack) across far more words.  The RP2040 UDC
 * driver reassembles the multi-packet bulk transfer into one net_buf, so the
 * class still sees exactly one request callback per command.  This value is
 * both the net_buf pool buffer size and the DAP_Info packet-size reported to
 * the host. */
inline constexpr uint16_t kDapPacketSize = 512;

/* USB full-speed bulk endpoint max-packet (fixed by the USB spec). */
inline constexpr uint16_t kUsbBulkMaxPacket = 64;

} // namespace probe::usb
