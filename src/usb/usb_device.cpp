#include "usb/usb_device.hpp"
#include "usb/dap_class.hpp"
#include "usb/msosv2.hpp"
#include "leds.hpp"
#include "uart_bridge.hpp"

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(usbdev, LOG_LEVEL_INF);

/* The usbd macros rely on linker sections and C linkage conventions;
 * keep them at global scope.
 */
USBD_DEVICE_DEFINE(probe_usbd,
		   DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)),
		   0x2E8A, 0x000C);

USBD_DESC_LANG_DEFINE(probe_lang);
USBD_DESC_MANUFACTURER_DEFINE(probe_mfr, "Raspberry Pi");
USBD_DESC_PRODUCT_DEFINE(probe_product, CONFIG_PROBE_PRODUCT_STRING);
USBD_DESC_SERIAL_NUMBER_DEFINE(probe_sn);

USBD_DESC_CONFIG_DEFINE(probe_fs_cfg_desc, "");
/* bmAttributes=0 (bus powered), bMaxPower=50 (=100 mA),
 * same as the original configuration descriptor.
 */
USBD_CONFIGURATION_DEFINE(probe_fs_config, 0, 50, &probe_fs_cfg_desc);

/* Handle USB device state changes and CDC line-coding updates.
 *
 * Drives the USB-connected LED from real enumeration events so it goes off on
 * suspend/reset/VBUS removal instead of staying on until reboot.
 * Also propagates CDC line coding / control line state onto the hardware UART
 * so uart1 follows the host's baud/parity instead of being stuck at the DTS
 * default. Referenced by name in init(), so keep it at file scope.
 */
static void usb_msg_cb(struct usbd_context *const ctx, const struct usbd_msg *msg)
{
	ARG_UNUSED(ctx);
	switch (msg->type) {
	case USBD_MSG_CDC_ACM_LINE_CODING:
	case USBD_MSG_CDC_ACM_CONTROL_LINE_STATE:
		probe::uart_bridge_line_coding(msg->dev);
		break;
	case USBD_MSG_CONFIGURATION:
	case USBD_MSG_RESUME:
		probe::leds_usb_connected(true);
		break;
	case USBD_MSG_SUSPEND:
	case USBD_MSG_RESET:
	case USBD_MSG_VBUS_REMOVED:
		probe::leds_usb_connected(false);
		break;
	default:
		break;
	}
}

namespace probe::usb {

static int check(int err, const char *what)
{
	if (err) {
		LOG_ERR("%s failed: %d", what, err);
	}
	return err;
}

int init()
{
	int err;

	err = check(usbd_add_descriptor(&probe_usbd, &probe_lang),
		    "add lang descriptor");
	if (err) {
		return err;
	}
	err = check(usbd_add_descriptor(&probe_usbd, &probe_mfr),
		    "add manufacturer descriptor");
	if (err) {
		return err;
	}
	err = check(usbd_add_descriptor(&probe_usbd, &probe_product),
		    "add product descriptor");
	if (err) {
		return err;
	}
	err = check(usbd_add_descriptor(&probe_usbd, &probe_sn),
		    "add serial number descriptor");
	if (err) {
		return err;
	}

	/* bcdUSB 2.10 (required for BOS) and bcdDevice 2.31, as in the original */
	err = check(usbd_device_set_bcd_usb(&probe_usbd, USBD_SPEED_FS, 0x0210),
		    "set bcdUSB");
	if (err) {
		return err;
	}
	err = check(usbd_device_set_bcd_device(&probe_usbd, 0x0231),
		    "set bcdDevice");
	if (err) {
		return err;
	}

	err = check(usbd_add_configuration(&probe_usbd, USBD_SPEED_FS,
					   &probe_fs_config),
		    "add FS configuration");
	if (err) {
		return err;
	}

	err = check(register_msosv2(&probe_usbd), "register MS OS 2.0 BOS");
	if (err) {
		return err;
	}

	/* Registration order determines interface numbering. DAP is registered
	 * first so it becomes interface 0 (old Keil MDK only looks at interface
	 * 0, matching the original firmware).
	 */
	err = check(usbd_register_class(&probe_usbd, probe::usb::kDapClassName,
					USBD_SPEED_FS, 1),
		    "register DAP class");
	if (err) {
		return err;
	}

	err = check(usbd_register_class(&probe_usbd, "cdc_acm_0",
					USBD_SPEED_FS, 1),
		    "register CDC-ACM class");
	if (err) {
		return err;
	}

	/* Keep the device-level class triple 0/0/0 like the original
	 * (each interface declares its own class).
	 */
	err = check(usbd_device_set_code_triple(&probe_usbd, USBD_SPEED_FS,
						0, 0, 0),
		    "set code triple");
	if (err) {
		return err;
	}

	err = check(usbd_init(&probe_usbd), "usbd_init");
	if (err) {
		return err;
	}

	err = check(usbd_msg_register_cb(&probe_usbd, usb_msg_cb),
		    "register usbd msg cb");
	if (err) {
		return err;
	}

	err = check(usbd_enable(&probe_usbd), "usbd_enable");
	if (err) {
		return err;
	}

	/* USB-connected LED is now driven by usb_msg_cb (USBD_MSG_CONFIGURATION
	 * fires on enumeration; USBD_MSG_SUSPEND/RESET/VBUS_REMOVED turn it off).
	 * No explicit set here — the callback will fire when the host enumerates.
	 */

	LOG_INF("USB device enabled");
	return 0;
}

} // namespace probe::usb
