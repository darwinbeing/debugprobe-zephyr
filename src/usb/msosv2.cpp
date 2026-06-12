#include "usb/msosv2.hpp"

#include <zephyr/kernel.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/usb/bos.h>
#include <zephyr/usb/msos_desc.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(msosv2, LOG_LEVEL_INF);

namespace {

/* Same as the original: bMS_VendorCode = 0x01 */
constexpr uint8_t kVendorCode = 0x01;
/* Windows 8.1 and later (the original uses 0x06030000) */
constexpr uint32_t kWindowsVersion = 0x06030000;

/* {CDB3B5AD-293B-4663-AA36-1AAE46463776}, UTF-16LE, same as the original */
#define CMSIS_DAP_V2_GUID \
	'{', 0x00, 'C', 0x00, 'D', 0x00, 'B', 0x00, '3', 0x00, 'B', 0x00, \
	'5', 0x00, 'A', 0x00, 'D', 0x00, '-', 0x00, '2', 0x00, '9', 0x00, \
	'3', 0x00, 'B', 0x00, '-', 0x00, '4', 0x00, '6', 0x00, '6', 0x00, \
	'3', 0x00, '-', 0x00, 'A', 0x00, 'A', 0x00, '3', 0x00, '6', 0x00, \
	'-', 0x00, '1', 0x00, 'A', 0x00, 'A', 0x00, 'E', 0x00, '4', 0x00, \
	'6', 0x00, '4', 0x00, '6', 0x00, '3', 0x00, '7', 0x00, '7', 0x00, \
	'6', 0x00, '}', 0x00, 0x00, 0x00, 0x00, 0x00

struct Msosv2Descriptor {
	struct msosv2_descriptor_set_header header;
	struct msosv2_function_subset_header subset_header;
	struct msosv2_compatible_id compatible_id;
	struct msosv2_guids_property guids_property;
} __packed;

Msosv2Descriptor msosv2_desc = {
	.header = {
		.wLength = sizeof(struct msosv2_descriptor_set_header),
		.wDescriptorType = MS_OS_20_SET_HEADER_DESCRIPTOR,
		.dwWindowsVersion = sys_cpu_to_le32(kWindowsVersion),
		.wTotalLength = sizeof(Msosv2Descriptor),
	},
	.subset_header = {
		.wLength = sizeof(struct msosv2_function_subset_header),
		.wDescriptorType = MS_OS_20_SUBSET_HEADER_FUNCTION,
		/* DAP v2 is interface 0 because usb_device.cpp registers it
		 * before CDC-ACM (see init() there); keep these in sync.
		 */
		.bFirstInterface = 0,
		.wSubsetLength = sizeof(Msosv2Descriptor) -
				 sizeof(struct msosv2_descriptor_set_header),
	},
	.compatible_id = {
		.wLength = sizeof(struct msosv2_compatible_id),
		.wDescriptorType = MS_OS_20_FEATURE_COMPATIBLE_ID,
		.CompatibleID = {'W', 'I', 'N', 'U', 'S', 'B', 0x00, 0x00},
	},
	.guids_property = {
		.wLength = sizeof(struct msosv2_guids_property),
		.wDescriptorType = MS_OS_20_FEATURE_REG_PROPERTY,
		.wPropertyDataType = MS_OS_20_PROPERTY_DATA_REG_MULTI_SZ,
		.wPropertyNameLength = 42,
		.PropertyName = {DEVICE_INTERFACE_GUIDS_PROPERTY_NAME},
		.wPropertyDataLength = 80,
		.bPropertyData = {CMSIS_DAP_V2_GUID},
	},
};

struct BosMsosv2 {
	struct usb_bos_platform_descriptor platform;
	struct usb_bos_capability_msos cap;
} __packed;

BosMsosv2 bos_msosv2_desc = {
	.platform = {
		.bLength = sizeof(struct usb_bos_platform_descriptor)
			 + sizeof(struct usb_bos_capability_msos),
		.bDescriptorType = USB_DESC_DEVICE_CAPABILITY,
		.bDevCapabilityType = USB_BOS_CAPABILITY_PLATFORM,
		.bReserved = 0,
		/* MS OS 2.0 platform capability UUID
		 * D8DD60DF-4589-4CC7-9CD2-659D9E648A9F
		 */
		.PlatformCapabilityUUID = {
			0xDF, 0x60, 0xDD, 0xD8,
			0x89, 0x45,
			0xC7, 0x4C,
			0x9C, 0xD2,
			0x65, 0x9D, 0x9E, 0x64, 0x8A, 0x9F,
		},
	},
	.cap = {
		.dwWindowsVersion = sys_cpu_to_le32(kWindowsVersion),
		.wMSOSDescriptorSetTotalLength =
			sys_cpu_to_le16(sizeof(msosv2_desc)),
		.bMS_VendorCode = kVendorCode,
		.bAltEnumCode = 0,
	},
};

int msosv2_to_host_cb(const struct usbd_context *const ctx,
		      const struct usb_setup_packet *const setup,
		      struct net_buf *const buf)
{
	ARG_UNUSED(ctx);

	if (setup->bRequest == kVendorCode &&
	    setup->wIndex == MS_OS_20_DESCRIPTOR_INDEX) {
		size_t len = MIN(net_buf_tailroom(buf), sizeof(msosv2_desc));

		net_buf_add_mem(buf, &msosv2_desc, len);
		LOG_DBG("MS OS 2.0 descriptor set sent (%u bytes)", (unsigned)len);
		return 0;
	}

	return -ENOTSUP;
}

} // namespace

USBD_DESC_BOS_VREQ_DEFINE(bos_vreq_msosv2, sizeof(bos_msosv2_desc),
			  &bos_msosv2_desc, kVendorCode,
			  msosv2_to_host_cb, nullptr);

static_assert(sizeof(Msosv2Descriptor) ==
	      sizeof(struct msosv2_descriptor_set_header) +
	      sizeof(struct msosv2_function_subset_header) +
	      sizeof(struct msosv2_compatible_id) +
	      sizeof(struct msosv2_guids_property));

namespace probe::usb {

int register_msosv2(struct usbd_context *uds_ctx)
{
	return usbd_add_descriptor(uds_ctx, &bos_vreq_msosv2);
}

} // namespace probe::usb
