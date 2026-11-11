#pragma once
/* Who drives the two debug pins: the SWD port (PIO0) or the SBW port (PIO1).
 * Pure C++ (host-tested). Every call comes from the DAP request thread, so
 * there is no locking.
 */
namespace probe::pin_owner {

/* SWD is taking the pins. An open SBW session is force-closed first, so a
 * crashed SBW tool cannot lock out an SWD debugger. */
void swd_connect();
void swd_disconnect();

/* SBW asks for the pins. Refused while SWD is connected. */
bool sbw_claim();
void sbw_release();
/* True while SBW holds the pins. */
bool sbw_owns();

/* Hook that closes an open SBW session; installed by the MSP430 backend. */
void set_sbw_force_close(void (*hook)());
/* Hook that returns the pins to their idle state (high-impedance, idle pulls)
 * after SWD lets go of them; installed by the MSP430 backend. */
void set_idle_hook(void (*hook)());

} // namespace probe::pin_owner
