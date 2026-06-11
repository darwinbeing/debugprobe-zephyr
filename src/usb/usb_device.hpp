#pragma once

namespace probe::usb {

/* Build the USB device (descriptors, configuration, class registration)
 * and enable it. Returns 0 on success.
 */
int init();

} // namespace probe::usb
