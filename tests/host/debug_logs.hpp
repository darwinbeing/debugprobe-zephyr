#pragma once
/* Expected FakeTap transcripts of the MSPDS CPUXv2 macros, written from
 * TI's sources (MSPDS 3.15.1.1, Bios/src/hil/edt.h and
 * Bios/src/hal/macros/<macro>Xv2.c), for the JTAG ID 0x91 paths and the
 * MSP430F5529's device settings (altRomAddressForCpuRead = 1, no LPMx.5 test
 * registers). File and lines are named above each builder. */
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "testing.hpp"

namespace dbglog {

using Log = std::vector<std::string>;

inline Log operator+(Log a, const Log &b)
{
	a.insert(a.end(), b.begin(), b.end());
	return a;
}

inline std::string f(const char *fmt, unsigned v)
{
	char buf[32];
	std::snprintf(buf, sizeof(buf), fmt, v);
	return buf;
}
inline std::string ir(unsigned v) { return f("ir:%02x", v); }
inline std::string d16(unsigned v) { return f("dr16:%04x", v); }
inline std::string d20(unsigned v) { return f("dr20:%05x", v); }
inline std::string d32(unsigned v) { return f("dr32:%08x", v); }
inline const Log kTclk = {"tclk:0", "tclk:1"}; /* IHIL_TCLK() */

inline void check_log(const char *file, int line, const Log &got, const Log &want)
{
	if (got == want) {
		return;
	}
	size_t i = 0;
	while (i < got.size() && i < want.size() && got[i] == want[i]) {
		++i;
	}
	std::string msg = "TAP transcript differs at entry " + std::to_string(i) + "\n  want:";
	for (size_t k = i; k < want.size() && k < i + 12; ++k) {
		msg += " " + want[k];
	}
	msg += "\n  got: ";
	for (size_t k = i; k < got.size() && k < i + 12; ++k) {
		msg += " " + got[k];
	}
	::testing::fail(file, line, msg);
}
/* Variadic, so that a braced list with commas needs no extra parentheses. */
#define CHECK_LOG(got, ...) dbglog::check_log(__FILE__, __LINE__, got, (__VA_ARGS__))

inline bool contains(const Log &log, const Log &sub)
{
	for (size_t i = 0; i + sub.size() <= log.size(); ++i) {
		if (std::equal(sub.begin(), sub.end(), log.begin() + static_cast<long>(i))) {
			return true;
		}
	}
	return false;
}

/* Everything after the first occurrence of `marker`. */
inline Log after(const Log &log, const Log &marker)
{
	for (size_t i = 0; i + marker.size() <= log.size(); ++i) {
		if (std::equal(marker.begin(), marker.end(), log.begin() + static_cast<long>(i))) {
			return Log(log.begin() + static_cast<long>(i + marker.size()), log.end());
		}
	}
	return {};
}

/* Cpuxv2::identify: GetCoreID, IsLockKeyProgrammed, GetCoreipIdXv2 */
inline Log identify_log()
{
	return {"speed:250000", "open", "enter:high", "ir:28",
		"ir:28", "dr16:aaaa", "ir:28", "dr16:aaaa", "ir:28", "dr16:aaaa",
		"ir:e8", "dr16:0000", "ir:e1", "dr20:00000"};
}

/* ReadMemWordXv2, edt.h:369-383 */
inline Log read_word(unsigned addr)
{
	return {"tclk:0", ir(0xC1), d20(addr), "tclk:1", "tclk:0", ir(0x42), d16(0),
		"tclk:1", "tclk:0", "tclk:1"};
}

/* WriteMemWordXv2, edt.h:416-432 */
inline Log write_word(unsigned addr, unsigned data)
{
	return {"tclk:0", ir(0xC8), d16(0x0500), ir(0xC1), d20(addr), "tclk:1",
		ir(0xA1), d16(data), "tclk:0", ir(0xC8), d16(0x0501), "tclk:1",
		"tclk:0", "tclk:1"};
}

/* ReadCpuRegXv2, edt.h:541-589, altRomAddressForCpuRead = 1 */
inline Log read_reg(unsigned mova)
{
	return Log{ir(0x28), "tclk:0", ir(0x82), "tclk:1", d16(mova), ir(0xC8), d16(0x1401),
		   ir(0x82)} + kTclk + Log{d16(0x0FF6)} + kTclk +
	       Log{d16(0x3FFD), "tclk:0", ir(0xC8), d16(0x0501), ir(0x42), "tclk:1", d16(0)} +
	       kTclk + Log{d16(0)} + kTclk + kTclk + kTclk + Log{"tclk:0", ir(0x42), "tclk:1"};
}

/* WriteCpuRegXv2, edt.h:651-675 */
inline Log write_reg(unsigned mova, unsigned data)
{
	return Log{"tclk:0", ir(0x82), "tclk:1", d16(mova), ir(0xC8), d16(0x1401), ir(0x82)} +
	       kTclk + Log{d16(data)} + kTclk + Log{d16(0x3FFD)} + kTclk +
	       Log{"tclk:0", ir(0x21), d20(0), "tclk:1", ir(0xC8), d16(0x0501)} + kTclk +
	       Log{"tclk:0", ir(0x42), "tclk:1"};
}

/* SetPcXv2, edt.h:728-749 */
inline Log set_pc(unsigned pc)
{
	const unsigned mova = 0x0080u | ((pc >> 8) & 0x0F00u);
	return {ir(0x28), d16(0), "tclk:0", ir(0x82), "tclk:1", d16(mova), "tclk:0",
		ir(0xC8), d16(0x1400), ir(0x82), "tclk:0", "tclk:1", d16(pc & 0xFFFFu),
		"tclk:0", "tclk:1", d16(0x4303), "tclk:0", ir(0x21), d20(0)};
}

/* EemDataExchangeXv2: one read / one write (IR selected by the caller) */
inline Log eem_read(unsigned reg) { return {d32(reg | 1u), d32(0)}; }
inline Log eem_write(unsigned reg, unsigned value) { return {d32(reg), d32(value)}; }

/* The EEM part of SyncJtag_Conditional_SaveContextXv2.c:95-168 when the CPU
 * is running and not in a low-power mode: trigger block 0 is saved, set to
 * "next instruction fetch", the EEM stops the CPU (`polls` control reads
 * until EEM_STOPPED), trigger block 0 is restored with `tb0` (VAL, CTL, MSK,
 * CMB, BREAKREACT). */
inline Log eem_stop(unsigned polls, const unsigned tb0[5])
{
	Log l = {ir(0xD0), d16(0), ir(0x28), d16(0), ir(0xB0)};
	l = l + eem_read(0x00) + eem_read(0x02) + eem_read(0x04) + eem_read(0x06) +
	    eem_read(0x80) + eem_write(0x02, 0) + eem_write(0x04, 0xFFFFF) +
	    eem_write(0x06, 1) + eem_write(0x80, 1) + eem_write(0x88, 0x000E) +
	    Log{ir(0x30), d16(0x0007)};
	for (unsigned i = 0; i < polls; ++i) {
		l = l + Log{ir(0xD0), d16(0)};
	}
	return l + Log{ir(0xB0)} + eem_write(0x00, tb0[0]) + eem_write(0x02, tb0[1]) +
	       eem_write(0x04, tb0[2]) + eem_write(0x06, tb0[3]) + eem_write(0x80, tb0[4]);
}

/* SyncJtag_Conditional_SaveContextXv2.c:172-329 from "enable clock control
 * before sync" on, with sync, wait bit and CPUSUSP present at the first
 * look, followed by the FCTL3 check, the watchdog hold and the SR save.
 * `wdt` is the WDTCTL value read, `sr` the SR value read. */
inline Log sync_tail(unsigned wdt, unsigned sr)
{
	return Log{ir(0x30), d16(0x0005),
		   /* SyncJtagXv2 */
		   ir(0xC8), d16(0x1501), ir(0x28), d16(0),
		   ir(0x30), d16(0x0007), d16(0x0006),
		   ir(0xC8), d16(0x1501), ir(0x28), d16(0),
		   /* pipe empty on the first clock */
		   ir(0x28), "tclk:0", d16(0), "tclk:1",
		   ir(0xC8), d16(0x0501)} +
	       kTclk + Log{ir(0x21), d20(0), ir(0x28), d16(0), ir(0x30), d16(0x000E),
			   ir(0x28), d16(0)} +
	       read_word(0x0144) + read_word(0x015C) +
	       write_word(0x015C, 0x5A80u | (wdt & 0xFFu)) + read_reg(0x0260) +
	       write_reg(0x0082, sr & 0xFFE7u);
}

/* Clearing all 8 trigger blocks: CMB = 0 each, BREAKREACT = 0 */
inline Log clear_triggers()
{
	Log l = {ir(0xB0)};
	for (unsigned i = 0; i < 8; ++i) {
		l = l + eem_write(i * 8 + 6, 0);
	}
	return l + eem_write(0x80, 0);
}

/* RestoreContext_ReleaseJtagXv2.c:69-196, mdb = 0, ulpDebug = 0, CPU not
 * in the HALT state at the release clock. */
inline Log restore_release(unsigned pc, unsigned sr, unsigned wdt, unsigned eem_control)
{
	return write_reg(0x0082, sr) + write_word(0x015C, 0x5A00u | (wdt & 0xFFu)) +
	       set_pc((sr & 0x0010u) ? pc - 2 : pc) +
	       Log{"tclk:1", ir(0xC8), d16(0x0401), ir(0x21), ir(0x28), "tclk:0", d16(0),
		   "tclk:1", ir(0x30), d16(eem_control), ir(0x28), ir(0xA8)};
}

} // namespace dbglog
