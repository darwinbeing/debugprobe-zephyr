#include "uart_bridge.hpp"
#include "leds.hpp"
#include "autobaud/autobaud.hpp"

#include <zephyr/drivers/uart.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(uart_bridge, LOG_LEVEL_INF);

namespace probe {

namespace {

/* Modeled exactly on Zephyr's drivers/serial/uart_bridge.c: each device owns a
 * ring into which data received FROM that device is written; that ring is
 * drained out the PEER device's TX. The two-index `own`/peer scheme keeps the
 * two directions from crossing. Here the two devices are fixed (cdc_acm_uart0
 * and uart1) so the DT `peers`/`get_idx` machinery is replaced by direct
 * lookups, but the RX/TX/pause/resume mechanics are identical.
 */

constexpr size_t kRingSize = 256;
/* uart_bridge.c uses RING_BUF_SIZE/3; keep the same one-third threshold so the
 * pause/resume hysteresis behaves the same. */
constexpr size_t kFullThreshold = kRingSize / 3;

const struct device *const cdc_dev = DEVICE_DT_GET(DT_NODELABEL(cdc_acm_uart0));
const struct device *const uart_dev = DEVICE_DT_GET(DT_NODELABEL(uart1));

struct Peer {
	const struct device *dev;  /* the device this ring belongs to */
	const struct device *peer; /* where this ring's data is drained OUT */
	struct ring_buf rb;
	bool paused;               /* RX was disabled because the ring filled up */
	uint8_t buf[kRingSize];
};

/* g_cdc holds data received FROM the CDC, drained OUT to uart1.
 * g_uart holds data received FROM uart1, drained OUT to the CDC. */
Peer g_cdc;
Peer g_uart;

/* The ring OWNED by `dev` (where data read from `dev` is stored). */
Peer &own_ring(const struct device *dev)
{
	return (dev == cdc_dev) ? g_cdc : g_uart;
}

/* The ring whose data should be sent OUT through `dev` (the peer's own ring).
 * If `dev` is the CDC, that is the data received from uart1 (g_uart), and vice
 * versa. This mirrors uart_bridge.c's `peer[get_idx(dev, own=false)]`. */
Peer &out_ring(const struct device *dev)
{
	return (dev == cdc_dev) ? g_uart : g_cdc;
}

/* RX-ready on `dev`: read FROM `dev` into ITS OWN ring; enable the PEER's TX so
 * the data is flushed out the other side. Mirrors uart_bridge_handle_rx(). */
void handle_rx(const struct device *dev)
{
	Peer &self = own_ring(dev);

	if (ring_buf_space_get(&self.rb) < kFullThreshold) {
		/* Ring nearly full: stop reading this side until the peer drains
		 * it (resumed from handle_tx once space recovers). */
		uart_irq_rx_disable(dev);
		self.paused = true;
		return;
	}

	uint8_t *recv_buf = nullptr;
	uint32_t rb_len = ring_buf_put_claim(&self.rb, &recv_buf, kRingSize);
	if (rb_len == 0) {
		LOG_WRN("%s: ring_buf full", dev->name);
		return;
	}

	int recv_len = uart_fifo_read(dev, recv_buf, rb_len);
	if (recv_len < 0) {
		ring_buf_put_finish(&self.rb, 0);
		LOG_ERR("%s: rx error: %d", dev->name, recv_len);
		return;
	}

	if (ring_buf_put_finish(&self.rb, recv_len) < 0) {
		LOG_ERR("%s: ring_buf_put_finish error", dev->name);
		return;
	}

	/* Spurious RX-ready IRQs (e.g. CDC-ACM) can yield recv_len == 0; skip
	 * the LED blink and peer TX-enable when no bytes were actually read. */
	if (recv_len == 0) {
		return;
	}

	/* Activity LEDs: data read FROM the CDC (host→target) = TX; data read
	 * FROM uart1 (target→host) = RX. Mirrors original cdc_uart.c blink. */
	if (dev == cdc_dev) {
		probe::leds_uart_tx_activity();
	} else {
		probe::leds_uart_rx_activity();
	}

	uart_irq_tx_enable(self.peer);
}

/* TX-ready on `dev`: `dev` is a TX target, so drain the ring whose data should
 * go OUT through `dev` (the peer device's received-data ring) into `dev`'s
 * FIFO. If empty, disable `dev`'s TX. If the source side was paused on a full
 * ring and space has recovered, re-enable its RX. Mirrors
 * uart_bridge_handle_tx(). */
void handle_tx(const struct device *dev)
{
	Peer &src = out_ring(dev);

	uint8_t *send_buf = nullptr;
	uint32_t rb_len = ring_buf_get_claim(&src.rb, &send_buf, kRingSize);
	if (rb_len == 0) {
		uart_irq_tx_disable(dev);
		return;
	}

	int sent_len = uart_fifo_fill(dev, send_buf, rb_len);
	if (sent_len < 0) {
		ring_buf_get_finish(&src.rb, 0);
		LOG_ERR("%s: tx error: %d", dev->name, sent_len);
		return;
	}

	if (ring_buf_get_finish(&src.rb, sent_len) < 0) {
		LOG_ERR("%s: ring_buf_get_finish error", dev->name);
		return;
	}

	if (src.paused && ring_buf_space_get(&src.rb) > kFullThreshold) {
		/* Source ring drained enough: let it receive again. src.dev is
		 * the device that fills this ring (the peer of `dev`). */
		uart_irq_rx_enable(src.dev);
		src.paused = false;
	}
}

/* Drain RX of `dev` into its ring; push the peer's ring into `dev`'s TX.
 * Mirrors uart_bridge.c interrupt_handler. */
void isr(const struct device *dev, void *user_data)
{
	ARG_UNUSED(user_data);

	while (uart_irq_update(dev) && uart_irq_is_pending(dev)) {
		if (uart_irq_rx_ready(dev)) {
			handle_rx(dev);
		}
		if (uart_irq_tx_ready(dev)) {
			handle_tx(dev);
		}
	}
}

} // namespace

void uart_bridge_init()
{
	if (!device_is_ready(cdc_dev) || !device_is_ready(uart_dev)) {
		LOG_ERR("bridge devices not ready");
		return;
	}

	g_cdc.dev = cdc_dev;
	g_cdc.peer = uart_dev;
	g_cdc.paused = false;
	g_uart.dev = uart_dev;
	g_uart.peer = cdc_dev;
	g_uart.paused = false;

	ring_buf_init(&g_cdc.rb, kRingSize, g_cdc.buf);
	ring_buf_init(&g_uart.rb, kRingSize, g_uart.buf);

	uart_irq_callback_user_data_set(cdc_dev, isr, nullptr);
	uart_irq_callback_user_data_set(uart_dev, isr, nullptr);
	uart_irq_rx_enable(cdc_dev);
	uart_irq_rx_enable(uart_dev);
}

void uart_bridge_line_coding(const struct device *cdc)
{
	struct uart_config cfg;
	if (uart_config_get(cdc, &cfg) != 0) {
		return;
	}
	if (cfg.baudrate == probe::kMagicBaud) {
		/* MAGIC_BAUD is a host-side sentinel, not a real rate: start
		 * autobaud and do NOT push it to the target UART. */
		probe::autobaud_start();
		return;
	}
	/* A real baud cancels any running autobaud (it restores the RX pin). */
	probe::autobaud_stop();
	uart_configure(uart_dev, &cfg);
}

} // namespace probe
