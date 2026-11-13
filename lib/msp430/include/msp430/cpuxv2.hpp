#pragma once
/* MSP430 CPUXv2 (5xx/6xx flash) device operations over a Tap.
 * Port of TI slau320 Replicator430Xv2/JTAGfunc430Xv2.c.
 */
#include <cstddef>
#include <cstdint>

#include "msp430/protocol.hpp"
#include "msp430/tap.hpp"

namespace msp430 {

struct Identity {
	uint8_t jtag_id = 0;
	uint16_t core_id = 0;
	uint32_t device_id_ptr = 0;
	uint16_t device_id = 0;
};

class Cpuxv2 {
public:
	explicit Cpuxv2(Tap &tap) : tap_(tap) {}

	/* Take the target under JTAG control: entry, ID, lock check, sync,
	 * power-on reset, watchdog hold, read the device ID. On failure the
	 * pins are released. `id.jtag_id` holds the ID that was read, also for
	 * NoTarget; it is 0 when the magic-pattern entry got no mailbox access
	 * and no ID was read. */
	Status connect(EntryMode mode, uint32_t slot_hz, Identity &id);
	/* The first half of connect(): entry, JTAG ID, lock check, core ID and
	 * device-ID pointer (`id.device_id` stays 0). With EntryMode::Normal it
	 * does not touch the CPU (entry with RST high, ID and lock reads only).
	 * MagicPattern resets the device through RST and holds it in the boot
	 * code; debug attach uses Normal only. On Ok the pins stay taken, the
	 * CPU keeps running and is not under JTAG control: read_mem, write_mem
	 * and the flash operations need connect() (or a Debugger sync) first.
	 * To let go without a reset, close the Tap; disconnect() resets the
	 * target. On failure the pins are released (same statuses as
	 * connect()). */
	Status identify(EntryMode mode, uint32_t slot_hz, Identity &id);
	/* "Check Init State": JTAG ID 0x91 captured and any of control-signal
	 * bits 0x0301 set. Detects a dead or floating link; it does not prove
	 * that the CPU is halted or synchronized. Shifts IR and DR, so call it
	 * only while the pins are taken. */
	bool in_full_emulation();
	/* Reset the target through JTAG and release the pins. */
	void disconnect();

	/* Data buffers are little-endian byte pairs, one pair per word.
	 * TargetError is also returned when the CPU is no longer under JTAG
	 * control at the end of the transfer (this includes a data line that
	 * reads constantly high or low); the buffer contents are then
	 * undefined. */
	Status read_mem(uint32_t addr, size_t nwords, uint8_t *dst);
	/* Same TargetError rule as read_mem. RAM and peripheral registers only;
	 * it cannot program flash. */
	Status write_mem(uint32_t addr, size_t nwords, const uint8_t *src);

	/* Flash operations. The caller validates addresses and lengths (even,
	 * inside the device's memory). `ram_addr` is where the funclet is loaded
	 * (178 bytes for erase, 240 for write); that RAM is overwritten.
	 * flash_begin must succeed before flash_data / flash_end (otherwise
	 * BadState). After any failure of erase, flash_begin or flash_data the
	 * funclet has already been cleaned up and no flash_end is needed. Any of
	 * these may return TargetError when the funclet cleanup that follows
	 * every erase and write cannot re-synchronize the target; that outranks
	 * the operation's own result.
	 * flash_end(false) abandons a write. With the Info-A key locked, erasing
	 * or writing Info segment A is silently ignored by the device and still
	 * reports Ok. During an active write only flash_data, flash_end, connect,
	 * identify and disconnect are allowed; the last three clear the write
	 * state. */
	Status erase(EraseMode mode, uint32_t addr, uint32_t ram_addr);
	Status flash_begin(uint32_t addr, uint32_t nwords, uint32_t ram_addr);
	Status flash_data(size_t nwords, const uint8_t *src);
	Status flash_end(bool complete);

private:
	bool write_word(uint32_t addr, uint16_t data);
	bool set_pc(uint32_t addr);
	bool execute_por();
	bool sync_por();
	bool release_to(uint32_t addr);
	bool is_locked();
	bool jmb_write16(uint16_t value);
	bool jmb_read32(uint32_t &value);
	bool jmb_wait(uint32_t expected, uint32_t timeout_ms);
	bool start_funclet(const uint16_t *code, size_t nwords, uint32_t ram_addr,
			   const uint16_t *params, size_t nparams);
	bool funclet_cleanup();
	Status finish_funclet(Status result);

	Tap &tap_;
	uint32_t funclet_ram_ = 0;
	size_t funclet_words_ = 0;
	bool write_active_ = false; /* the write funclet is running and waiting for data */
};

} // namespace msp430
