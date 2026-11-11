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
	 * pins are released. `id.jtag_id` is valid for NoTarget too. */
	Status connect(EntryMode mode, uint32_t slot_hz, Identity &id);
	/* Reset the target through JTAG and release the pins. */
	void disconnect();

	/* Data buffers are little-endian byte pairs, one pair per word. */
	Status read_mem(uint32_t addr, size_t nwords, uint8_t *dst);
	Status write_mem(uint32_t addr, size_t nwords, const uint8_t *src);

	Status erase(EraseMode mode, uint32_t addr, uint32_t ram_addr);
	Status flash_begin(uint32_t addr, uint32_t nwords, uint32_t ram_addr);
	Status flash_data(size_t nwords, const uint8_t *src);
	Status flash_end(bool complete);

private:
	bool in_full_emulation();
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

	Tap &tap_;
	uint32_t funclet_ram_ = 0;
	size_t funclet_words_ = 0;
};

} // namespace msp430
