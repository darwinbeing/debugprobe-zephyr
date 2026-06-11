#pragma once

struct usbd_context;

namespace probe::usb {

/* Attach the BOS (MS OS 2.0 platform capability + vendor request)
 * to the device context.
 */
int register_msosv2(struct usbd_context *uds_ctx);

} // namespace probe::usb
