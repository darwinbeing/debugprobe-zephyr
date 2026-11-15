#include "usb/dap_class.hpp"

#include "dap/dap_backend.hpp"

#include <zephyr/kernel.h>
#include <zephyr/usb/usbd.h>
/* udc_get_buf_info() lives in udc.h (not udc_buf.h which does not exist in 4.4) */
#include <zephyr/drivers/usb/udc.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/logging/log.h>

#include <cstring>
#include <span>

LOG_MODULE_REGISTER(dap_usb, LOG_LEVEL_INF);

namespace {

using probe::usb::kDapPacketSize;
using probe::usb::kUsbBulkMaxPacket;

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

/* Count 3 is load-bearing: during reconfiguration the cancelled OUT buffer's
 * completion event can still hold one buffer while enable() arms a fresh one;
 * the IN response buffer is drawn from this same pool while the OUT buffer is
 * still in flight, so the pool must hold OUT + IN + the cancelled straggler
 * concurrently.  Each buffer is a full DAP packet (kDapPacketSize), which the
 * UDC driver fills/drains across multiple 64-byte USB packets.
 */
UDC_BUF_POOL_DEFINE(dap_buf_pool, 3, kDapPacketSize,
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
		.wMaxPacketSize = sys_cpu_to_le16(kUsbBulkMaxPacket),
		.bInterval = 0,
	},
	.if0_in_ep = {
		.bLength = sizeof(struct usb_ep_descriptor),
		.bDescriptorType = USB_DESC_ENDPOINT,
		.bEndpointAddress = 0x81,
		.bmAttributes = USB_EP_TYPE_BULK,
		.wMaxPacketSize = sys_cpu_to_le16(kUsbBulkMaxPacket),
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

static uint8_t dap_bulk_in_ep(struct usbd_class_data *const c_data)
{
	auto *data = static_cast<DapData *>(usbd_class_get_private(c_data));

	return data->desc->if0_in_ep.bEndpointAddress;
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
		/* Run the DAP engine straight from the OUT buffer into a fresh IN
		 * buffer.  No stack bounce: buffers are up to kDapPacketSize (512)
		 * which would blow the usbd thread stack as locals.
		 */
		struct net_buf *in_buf = dap_buf_alloc(dap_bulk_in_ep(c_data));

		if (in_buf == nullptr) {
			/* Pool exhausted: drop the response rather than escalate;
			 * the host will time out and retry. */
			LOG_ERR("IN buffer alloc failed");
		} else {
			const size_t resp_len = probe::dap_backend::process(
				std::span<const uint8_t>(buf->data, buf->len),
				std::span<uint8_t>(net_buf_tail(in_buf),
						   net_buf_tailroom(in_buf)));

			/* resp_len == 0 means "no response" (e.g. DAP_TransferAbort,
			 * handled out-of-band): skip the IN transfer entirely.
			 */
			if (resp_len == 0) {
				usbd_ep_buf_free(ctx, in_buf);
			} else {
				net_buf_add(in_buf, resp_len);

				if (usbd_ep_enqueue(c_data, in_buf)) {
					LOG_ERR("IN enqueue failed");
					usbd_ep_buf_free(ctx, in_buf);
				}
			}
		}
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
	probe::dap_backend::on_transport_disabled();
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

/* C++ designated initializers must appear in declaration order
 * (usbd.h: ... request ... enable, disable, init ... get_desc);
 * this subset already is — keep it that way when adding members.
 */
static struct usbd_class_api dap_class_api = {
	.request = dap_request_handler,
	.enable = dap_enable,
	.disable = dap_disable,
	.init = dap_init,
	.get_desc = dap_get_desc,
};

USBD_DEFINE_CLASS(dap_v2_0, &dap_class_api, &dap_data, nullptr);
