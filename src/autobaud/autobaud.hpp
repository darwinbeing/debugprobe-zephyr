#pragma once
/* Firmware autobaud: runs the PIO frequency counter on the target UART RX
 * pin, feeds the Estimator, and applies the detected baud to the bridged
 * UART. Triggered when the host sets MAGIC_BAUD.
 */
#include <cstdint>

namespace probe {

inline constexpr uint32_t kMagicBaud = 9728; /* autobaud.h MAGIC_BAUD 0x2600 */

/* Begin measuring; on a confident estimate, reconfigure uart1 to the
 * detected baud and stop. Idempotent. */
void autobaud_start();
/* Abort measuring and restore uart1's RX pin to the UART function. */
void autobaud_stop();

} // namespace probe
