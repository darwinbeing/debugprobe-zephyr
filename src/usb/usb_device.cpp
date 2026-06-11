#include "usb/usb_device.hpp"

#include <zephyr/device.h>
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

namespace probe::usb {

int init()
{
	int err;

	err = usbd_add_descriptor(&probe_usbd, &probe_lang);
	if (err) {
		return err;
	}
	err = usbd_add_descriptor(&probe_usbd, &probe_mfr);
	if (err) {
		return err;
	}
	err = usbd_add_descriptor(&probe_usbd, &probe_product);
	if (err) {
		return err;
	}
	err = usbd_add_descriptor(&probe_usbd, &probe_sn);
	if (err) {
		return err;
	}

	/* bcdUSB 2.10 (required for BOS) and bcdDevice 2.31, as in the original */
	err = usbd_device_set_bcd_usb(&probe_usbd, USBD_SPEED_FS, 0x0210);
	if (err) {
		return err;
	}
	err = usbd_device_set_bcd_device(&probe_usbd, 0x0231);
	if (err) {
		return err;
	}

	err = usbd_add_configuration(&probe_usbd, USBD_SPEED_FS, &probe_fs_config);
	if (err) {
		return err;
	}

	/* Registration order determines interface numbering. Task 4 inserts the
	 * DAP vendor class before CDC so DAP becomes interface 0 (old Keil MDK
	 * only looks at interface 0, matching the original).
	 */
	err = usbd_register_class(&probe_usbd, "cdc_acm_0", USBD_SPEED_FS, 1);
	if (err) {
		return err;
	}

	/* Keep the device-level class triple 0/0/0 like the original
	 * (each interface declares its own class).
	 */
	err = usbd_device_set_code_triple(&probe_usbd, USBD_SPEED_FS, 0, 0, 0);
	if (err) {
		return err;
	}

	err = usbd_init(&probe_usbd);
	if (err) {
		return err;
	}

	err = usbd_enable(&probe_usbd);
	if (err) {
		return err;
	}

	LOG_INF("USB device enabled");
	return 0;
}

} // namespace probe::usb
