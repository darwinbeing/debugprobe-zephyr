#include "usb/dap_class.hpp"

#include <zephyr/kernel.h>
#include <zephyr/usb/usbd.h>
/* udc_get_buf_info() lives in udc.h (not udc_buf.h which does not exist in 4.4) */
#include <zephyr/drivers/usb/udc.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/logging/log.h>

#include <cstring>

LOG_MODULE_REGISTER(dap_usb, LOG_LEVEL_INF);

namespace {

constexpr uint16_t kPacketSize = 64;

struct DapDesc {
	struct usb_if_descriptor if0;
	struct usb_ep_descriptor if0_out_ep;
	struct usb_ep_descriptor if0_in_ep;
	struct usb_desc_header nil_desc;
} __packed;

struct DapData {
	DapDesc *desc;
	const struct usb_desc_header **fs_desc;
	struct usbd_desc_node *if_str;
};

} // namespace

/* Count 2 is load-bearing: during reconfiguration the cancelled OUT buffer's
 * completion event can still hold one buffer while enable() arms a fresh one.
 * Phase 2 (IN responses from this pool) will need >= 3.
 */
UDC_BUF_POOL_DEFINE(dap_buf_pool, 2, kPacketSize,
		    sizeof(struct udc_buf_info), nullptr);

USBD_DESC_STRING_DEFINE(dap_if_str_desc, "CMSIS-DAP v2 Interface",
			USBD_DUT_STRING_INTERFACE);

static DapDesc dap_desc = {
	.if0 = {
		.bLength = sizeof(struct usb_if_descriptor),
		.bDescriptorType = USB_DESC_INTERFACE,
		.bInterfaceNumber = 0,
		.bAlternateSetting = 0,
		.bNumEndpoints = 2,
		.bInterfaceClass = USB_BCC_VENDOR,
		.bInterfaceSubClass = 0,
		.bInterfaceProtocol = 0,
		.iInterface = 0,
	},
	.if0_out_ep = {
		.bLength = sizeof(struct usb_ep_descriptor),
		.bDescriptorType = USB_DESC_ENDPOINT,
		.bEndpointAddress = 0x01,
		.bmAttributes = USB_EP_TYPE_BULK,
		.wMaxPacketSize = sys_cpu_to_le16(kPacketSize),
		.bInterval = 0,
	},
	.if0_in_ep = {
		.bLength = sizeof(struct usb_ep_descriptor),
		.bDescriptorType = USB_DESC_ENDPOINT,
		.bEndpointAddress = 0x81,
		.bmAttributes = USB_EP_TYPE_BULK,
		.wMaxPacketSize = sys_cpu_to_le16(kPacketSize),
		.bInterval = 0,
	},
	.nil_desc = {
		.bLength = 0,
		.bDescriptorType = 0,
	},
};

static const struct usb_desc_header *dap_fs_desc[] = {
	reinterpret_cast<struct usb_desc_header *>(&dap_desc.if0),
	reinterpret_cast<struct usb_desc_header *>(&dap_desc.if0_out_ep),
	reinterpret_cast<struct usb_desc_header *>(&dap_desc.if0_in_ep),
	&dap_desc.nil_desc,
};

static DapData dap_data = {
	.desc = &dap_desc,
	.fs_desc = dap_fs_desc,
	.if_str = &dap_if_str_desc,
};

static uint8_t dap_bulk_out_ep(struct usbd_class_data *const c_data)
{
	auto *data = static_cast<DapData *>(usbd_class_get_private(c_data));

	return data->desc->if0_out_ep.bEndpointAddress;
}

static struct net_buf *dap_buf_alloc(const uint8_t ep)
{
	struct net_buf *buf = net_buf_alloc(&dap_buf_pool, K_NO_WAIT);

	if (buf == nullptr) {
		return nullptr;
	}

	struct udc_buf_info *bi = udc_get_buf_info(buf);

	std::memset(bi, 0, sizeof(*bi));
	bi->ep = ep;

	return buf;
}

static int dap_arm_out(struct usbd_class_data *const c_data)
{
	struct usbd_context *ctx = usbd_class_get_ctx(c_data);
	struct net_buf *buf = dap_buf_alloc(dap_bulk_out_ep(c_data));

	if (buf == nullptr) {
		LOG_ERR("OUT buffer alloc failed");
		return -ENOMEM;
	}

	int err = usbd_ep_enqueue(c_data, buf);

	if (err) {
		LOG_ERR("OUT enqueue failed: %d", err);
		usbd_ep_buf_free(ctx, buf);
	}

	return err;
}

static int dap_request_handler(struct usbd_class_data *const c_data,
			       struct net_buf *buf, int err)
{
	struct usbd_context *ctx = usbd_class_get_ctx(c_data);
	struct udc_buf_info *bi = udc_get_buf_info(buf);
	const bool is_out = (bi->ep == dap_bulk_out_ep(c_data));

	if (err == 0 && is_out) {
		/* Phase 1 stub: log only, no processing.
		 * Phase 2 plugs the DapEngine in here.
		 */
		/* INF so the phase-1 stub is observable at the default log level;
		 * phase 2 replaces this with the DAP protocol engine.
		 */
		LOG_HEXDUMP_INF(buf->data, buf->len, "DAP OUT (stub)");
	}

	usbd_ep_buf_free(ctx, buf);

	if (is_out && err == 0) {
		/* A re-arm failure is already logged in dap_arm_out(); returning
		 * non-zero here would make the stack publish USBD_MSG_STACK_ERROR
		 * while the endpoint stays dead anyway, so swallow it.
		 */
		(void)dap_arm_out(c_data);
	}

	return 0;
}

static void *dap_get_desc(struct usbd_class_data *const c_data,
			  const enum usbd_speed speed)
{
	auto *data = static_cast<DapData *>(usbd_class_get_private(c_data));

	ARG_UNUSED(speed); /* full-speed only */
	return data->fs_desc;
}

static void dap_enable(struct usbd_class_data *const c_data)
{
	LOG_INF("DAP interface enabled");
	/* No re-entrancy guard: in Zephyr 4.4 usbd, enable() is only invoked from
	 * usbd_config_set(), which always disables (and drains endpoints) first.
	 * TODO(phase 2): handle SET_INTERFACE via an update() callback re-arm.
	 */
	(void)dap_arm_out(c_data);
}

static void dap_disable(struct usbd_class_data *const c_data)
{
	ARG_UNUSED(c_data);
	LOG_INF("DAP interface disabled");
}

static int dap_init(struct usbd_class_data *const c_data)
{
	struct usbd_context *ctx = usbd_class_get_ctx(c_data);
	auto *data = static_cast<DapData *>(usbd_class_get_private(c_data));

	if (usbd_add_descriptor(ctx, data->if_str)) {
		LOG_ERR("Failed to add interface string");
	} else {
		data->desc->if0.iInterface =
			usbd_str_desc_get_idx(data->if_str);
	}

	return 0;
}

/* Member order must follow struct usbd_class_api declaration in usbd.h:
 * feature_halt, update, control_to_dev, control_to_host, request,
 * suspended, resumed, sof, enable, disable, init, shutdown, get_desc.
 * The plan listed only {request, enable, disable, init, get_desc}; the
 * full-order initializer below is required for C++ designated initializers.
 */
static struct usbd_class_api dap_class_api = {
	.request = dap_request_handler,
	.enable = dap_enable,
	.disable = dap_disable,
	.init = dap_init,
	.get_desc = dap_get_desc,
};

USBD_DEFINE_CLASS(dap_v2_0, &dap_class_api, &dap_data, nullptr);
