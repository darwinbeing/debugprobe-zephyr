#pragma once
/* Firmware-side ownership of the MSP430 SBW objects (port, TAP, device layer,
 * session) and their registration with the DAP engine.
 */
namespace probe::msp430_backend {

/* Register the vendor command, the two pin-ownership hooks and the
 * transport-disabled hook. Call once from main(), before USB is started. */
void init();

/* Close an open SBW session and release the pins. Safe when idle. */
void close_session();

} // namespace probe::msp430_backend
