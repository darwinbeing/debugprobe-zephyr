#pragma once
/* Hardware and host-status abstractions injected into DapEngine.
 * Pure virtual so host tests can mock them; on target a thin adapter
 * wraps the Zephyr swdp device (phase 3).
 */
#include <cstddef>
#include <cstdint>
#include <span>

namespace dap {

enum class Ack : uint8_t {
	Ok = 0x01,
	Wait = 0x02,
	Fault = 0x04,
	NoAck = 0x07,
	/* Sentinel for protocol errors: casts to 0x0F = protocol-error bit (0x08) | NoAck bits; the engine compares the cast value against kXfer* masks. */
	ProtocolError = 0x08 | 0x07,
};

class SwdDriver {
public:
	virtual ~SwdDriver() = default;

	virtual void set_clock(uint32_t hz) = 0;
	virtual void connect() = 0;    /* claim pins, SWD mode (PORT_SWD_SETUP) */
	virtual void disconnect() = 0; /* release pins (PORT_OFF) */
	virtual void configure(uint8_t turnaround_cycles, bool data_phase) = 0;
	/* Idle cycles clocked after each transfer; set by DAP_TransferConfigure. */
	virtual void set_idle_cycles(uint8_t cycles) = 0;

	/* Clock out bit_count bits from data (LSB of data[0] first) on SWDIO. */
	virtual void swj_sequence(std::span<const uint8_t> data, size_t bit_count) = 0;
	/* SWD_Sequence primitives: drive or capture SWDIO for bit_count cycles. */
	virtual void swd_output(std::span<const uint8_t> data, size_t bit_count) = 0;
	virtual void swd_input(std::span<uint8_t> data, size_t bit_count) = 0;

	/* One DAP transfer (header + ACK + data + parity + turnaround + idle).
	 * request: CMSIS transfer request byte (APnDP/RnW/A2/A3 bits relevant).
	 * data: in for writes, out for reads; may be nullptr to post a read
	 * without collecting data. Returns the ACK.
	 */
	virtual Ack transfer(uint8_t request, uint32_t *data) = 0;

	/* Optional DMA fast path for a register READ block. Writes up to
	 * out_cap/4 little-endian data words to `out`, sets *completed to the
	 * number of OK transfers (stops at the first WAIT/FAULT) and *ack to the
	 * final ACK. Returns true if it handled the block (out/completed/ack
	 * valid); false => the caller must use the normal per-word path. The
	 * default does not handle it, so non-DMA drivers and host mocks are
	 * unaffected. */
	virtual bool transfer_block_read(uint8_t request, uint16_t count,
					 uint8_t *out, size_t out_cap,
					 uint16_t *completed, uint8_t *ack)
	{
		(void)request;
		(void)count;
		(void)out;
		(void)out_cap;
		(void)completed;
		(void)ack;
		return false;
	}

	virtual void set_pins(uint8_t select, uint8_t value, uint32_t wait_us) = 0;
	virtual uint8_t read_pins() = 0;
	virtual void delay_us(uint32_t us) = 0;
	/* Timestamp counter value (kInfoTimestampClock Hz domain). */
	virtual uint32_t timestamp() = 0;
};

class HostStatus {
public:
	virtual ~HostStatus() = default;
	virtual void connected(bool on) = 0;
	virtual void running(bool on) = 0;
};

struct Config {
	/* DAP_Info string payloads; nullptr -> zero-length info (the original
	 * debugprobe returns empty vendor/product/serial). Strings are sent
	 * INCLUDING their NUL terminator, as the reference implementation does.
	 */
	const char *vendor = nullptr;
	const char *product = nullptr;
	const char *serial = nullptr;
	const char *fw_version = "2.0.0";
	uint16_t packet_size = 64;
	/* The original reports DAP_PACKET_COUNT(8) - 2 to keep ring headroom. */
	uint8_t packet_count = 6;
	uint32_t timestamp_clock = 1000000;
};

} // namespace dap
