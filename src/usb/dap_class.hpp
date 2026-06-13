#pragma once

#include <cstdint>

namespace probe::usb {

/* usbd class instance name, referenced by usbd_register_class */
inline constexpr const char *kDapClassName = "dap_v2_0";

/* DAP command/response buffer size in bytes.  Deliberately larger than the
 * 64-byte USB full-speed bulk max-packet: openocd packs each DAP_TransferBlock
 * up to this size, so one command carries ~255 words instead of ~15.  That
 * amortizes the fixed per-command USB/usbd-thread round-trip (the real SWD
 * throughput bottleneck on this stack) across far more words.  The RP2040 UDC
 * driver reassembles the multi-packet bulk transfer into one net_buf, so the
 * class still sees exactly one request callback per command.  This value is
 * both the net_buf pool buffer size and the DAP_Info packet-size reported to
 * the host.  Measured on rpi_pico/RP2040: 64->512 gave ~2.3x, 512->1024 a
 * further ~1.19x (208 KB/s @15 MHz, +54% vs official); 1024->2048 would yield
 * diminishing returns. A DMA fast path was prototyped and found net-neutral at
 * this packet size (the bottleneck is host round-trips, not on-probe CPU), so
 * it was not adopted. */
inline constexpr uint16_t kDapPacketSize = 1024;

/* USB full-speed bulk endpoint max-packet (fixed by the USB spec). */
inline constexpr uint16_t kUsbBulkMaxPacket = 64;

} // namespace probe::usb
