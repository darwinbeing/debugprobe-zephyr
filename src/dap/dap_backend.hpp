#pragma once
/* Firmware-side ownership of the CMSIS-DAP protocol engine.
 *
 * Owns the singleton dap::DapEngine wired to a NullSwdDriver (phase 2 has no
 * real SWD hardware yet) and a NullHostStatus (no LEDs yet). The USB DAP class
 * request handler forwards OUT packets here and ships the returned response.
 */
#include <cstddef>
#include <cstdint>
#include <span>

namespace probe::dap_backend {

/* Process one CMSIS-DAP request packet; returns response length.
 * Thread-context only (called from the DAP class request handler).
 */
size_t process(std::span<const uint8_t> request, std::span<uint8_t> response);

/* Transport hook for DAP_TransferAbort (0x07) packets. */
void request_abort();

} // namespace probe::dap_backend
