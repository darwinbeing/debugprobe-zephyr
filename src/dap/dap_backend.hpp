#pragma once
/* Firmware-side ownership of the CMSIS-DAP protocol engine.
 *
 * Owns the singleton dap::DapEngine wired to a NullSwdDriver and a
 * probe::LedHostStatus that drives real LEDs (src/leds.cpp). The USB DAP
 * class request handler forwards OUT packets here and ships the returned
 * response.
 */
#include <cstddef>
#include <cstdint>
#include <span>

#include "dap/interfaces.hpp"

namespace probe::dap_backend {

/* Process one CMSIS-DAP request packet; returns response length.
 * Thread-context only (called from the DAP class request handler).
 */
size_t process(std::span<const uint8_t> request, std::span<uint8_t> response);

/* Transport hook for DAP_TransferAbort (0x07) packets. */
void request_abort();

/* Install the handler for CMSIS-DAP vendor commands (0x80..0x9F). */
void set_vendor_handler(dap::VendorHandler *handler);

/* The DAP USB interface was disabled (bus reset, deconfiguration). */
void on_transport_disabled();

/* Called when the DAP USB interface is disabled, i.e. bus reset or
 * deconfiguration; runs on the USB thread. */
void set_transport_disabled_hook(void (*hook)());

} // namespace probe::dap_backend
