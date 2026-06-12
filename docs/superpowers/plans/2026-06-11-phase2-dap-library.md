# debugprobe-zephyr Phase 2 (CMSIS-DAP Protocol Library) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A pure C++20 CMSIS-DAP v2 protocol library (`lib/dap/`, zero Zephyr dependencies, no heap, no exceptions) with byte-level behavior matching the original firmware's patched ARM reference implementation, fully unit-tested on the host.

**Architecture:** `dap::DapEngine` is a protocol state machine: it consumes one transport packet (request bytes), drives an injected `dap::SwdDriver` (pure virtual), and produces the response packet. Tests inject a `MockSwdDriver` that records calls and scripts ACK results. The library also builds as a Zephyr static library and is wired behind the phase-1 USB stub with a no-op driver, so `DAP_Info` already answers over USB after this phase.

**Tech Stack:** C++20 (host: AppleClang via plain CMake; target: GNU Arm 14.2 via Zephyr). Host unit tests use a ~100-line micro test harness (no external framework). Reference behavior source: `~/Developer/debugprobe/CMSIS_DAP/CMSIS/DAP/Firmware/Source/DAP.c` (the Raspberry Pi-patched ARM reference) and `~/Developer/debugprobe/include/DAP_config.h`.

**Spec amendment (approved during planning):** the design doc says "ztest / native_sim" for unit tests, but Zephyr's `native_sim` only supports Linux hosts and this is macOS. The library is zero-Zephyr by design, so tests run as a plain host CMake binary instead. Task 8 records this in the spec. The tests remain runnable under Linux CI later (plain CMake there too).

**Authoritative protocol facts** (verified against the original sources; cite these, do not re-derive):

- Dispatch (`DAP.c:1615-1746`): response always starts with the echoed command ID. Unknown command → single byte `0xFF` (`ID_DAP_Invalid`). Vendor commands 0x80-0x9F → echo ID only, 1-byte response (debugprobe links the default no-op `DAP_vendor.c`). `ID_DAP_ExecuteCommands` (0x7F): echo + count byte, then concatenated sub-responses. `ID_DAP_QueueCommands` (0x7E) never reaches the engine — the original transport rewrites byte 0 to 0x7F (`src/tusb_edpt_handler.c:222-227`); our engine treats a raw 0x7E like any unknown command. SWO commands (0x17-0x1C, 0x1E) are compiled out in the original → unknown → `0xFF`.
- `DAP_Info` (`DAP.c:60-145`): response `{0x00, len, payload...}`. Original debugprobe values: vendor/product/serial strings empty (`DAP_config.h:164-186` return 0 → len 0); FW version `"2.0.0"` **including the NUL terminator** (len 6: `32 2E 30 2E 30 00`); capabilities 1 byte `0x31` (SWD bit0 + atomic bit4 + timestamp bit5; JTAG/SWO absent); timestamp clock 4 bytes LE `1000000` (0x000F4240); packet size 2 bytes LE 64; packet count 1 byte **6** (`DAP_PACKET_COUNT(8) - 2`, the original's ring-buffer headroom hack, `DAP.c:135`); device vendor (5)/device name (6) → len 0; unknown info ID → len 0.
- Response status bytes: `DAP_OK = 0x00`, `DAP_ERROR = 0xFF`.
- Transfer request bit flags: APnDP=bit0, RnW=bit1, A2=bit2, A3=bit3, MatchValue=bit4, MatchMask=bit5, Timestamp=bit7.
- Transfer ACK/response values: OK=1, WAIT=2, FAULT=4, NO_ACK=7; bit3 (0x08) = protocol error (sticky, set by driver), bit4 (0x10) = value mismatch.
- `DAP_Transfer` algorithm: posted AP reads with `DP_RDBUFF` (0x0C | RnW) flush — port `DAP.c:657-940` faithfully (WAIT retry loops bounded by `retry_count`, match-value loops by `match_retry`, abort flag checked in every loop, canceled requests skipped to compute consumed length, response header `{cmd, count, last_response_value}` at offsets 0-2 — count written even on early break).
- `DAP_TransferBlock` (`DAP.c:1267-1460`): header `{cmd, count_lo, count_hi, response_value}`, single register, posted-read pipeline for AP reads, no match/timestamp support.
- `DAP_TransferConfigure` (`DAP.c:638-651`): request `{idle_cycles(1), wait_retry(2 LE), match_retry(2 LE)}` → `{cmd, DAP_OK}`. Defaults at reset: idle_cycles=0, retry_count=100, match_retry=0, match_mask=0 (`DAP_Setup`, `DAP.c:1752+`).
- `DAP_SWJ_Pins` (`DAP.c:~380`): request `{value(1), select(1), wait_us(4 LE)}` → `{cmd, pin_state(1)}`. Pin bits: SWCLK=0, SWDIO=1, TDI=2, TDO=3, nTRST=5, nRESET=7.
- `DAP_SWJ_Clock`: request 4-byte LE Hz; 0 → `DAP_ERROR`, else apply + `DAP_OK`.
- `DAP_SWJ_Sequence`: request `{bit_count(1), data(ceil(n/8))}`; count byte 0 means 256 bits → `{cmd, DAP_OK}`.
- `DAP_SWD_Configure` (`DAP.c:~560`): config byte: turnaround = (bits0-1)+1 cycles, data_phase = bit2 → `{cmd, DAP_OK}`.
- `DAP_SWD_Sequence` (`DAP.c:~575`): request `{seq_count(1), then per sequence: info(1) [+ output data]}`; info: bits0-5 = cycle count (0 means 64), bit7 = input. Response `{cmd, DAP_OK, then input data for each input sequence}`. While connected to SWD the original asserts; behavior to copy: respond DAP_OK and perform sequences via driver.
- `DAP_Connect` (`DAP.c:~300`): request `{port(1)}`; 0=default→SWD for this probe, 1=SWD ok, 2=JTAG unsupported → respond `{cmd, 0}` on failure, `{cmd, 1}` on SWD success (response byte = selected port). Drives `connect()` + applies default clock 1 MHz? No — clock is独立 state; connect calls PORT_SWD_SETUP only. Response on JTAG request: port byte 0 (`DAP_PORT_DISABLED`).
- `DAP_Disconnect`: `{cmd, DAP_OK}`, port off.
- `DAP_WriteABORT` (`DAP.c:~340`): request `{index(1), value(4 LE)}`, write to DP ABORT register (driver transfer request 0x00 = DP write addr 0); not-connected → `DAP_ERROR`. Response `{cmd, status}`.
- `DAP_Delay`: request 2-byte LE microseconds → driver `delay_us` → `{cmd, DAP_OK}`.
- `DAP_ResetTarget`: `{cmd, DAP_OK, 0x00}` (no device-specific reset on this probe: execute byte 0).
- `DAP_HostStatus`: request `{type(1), status(1)}`; type 0=connected LED, 1=running LED; type>1 → `DAP_ERROR`. Response `{cmd, DAP_OK}`.
- Not-connected behavior: `DAP_Transfer`/`DAP_TransferBlock`/`DAP_WriteABORT` against a disconnected port: the original dispatches to `DAP_Dummy_*` (`DAP.c:1538+`) → Transfer responds `{cmd, 0, 0}` (count 0, response 0), TransferBlock `{cmd, 0, 0, 0}`, WriteABORT `{cmd, DAP_ERROR}`.

**Build environment** (same as phase 1): west via `/usr/local/opt/python@3.14/bin/python3.14 -m west`; `ZEPHYR_TOOLCHAIN_VARIANT=gnuarmemb`; `GNUARMEMB_TOOLCHAIN_PATH=$HOME/Developer/zephyrproject/arm-gnu-toolchain-14.2.rel1-darwin-x86_64-arm-none-eabi`; three-board build via `scripts/build_all.sh`. Host tests need only system cmake + clang.

**Branch:** create `phase2-dap-library` from master at start; merge at the end (same finishing flow as phase 1).

---

### Task 0: Host test harness + micro test framework

**Files:**
- Create: `tests/host/CMakeLists.txt`
- Create: `tests/host/testing.hpp`
- Create: `tests/host/test_smoke.cpp`
- Create: `scripts/run_host_tests.sh`

- [ ] **Step 1: Write tests/host/testing.hpp** — a micro test framework: test registration via static objects, `CHECK`/`CHECK_EQ`/`CHECK_BYTES` macros, hex-diff output for byte vectors, non-zero exit on failure.

```cpp
#pragma once
/* Micro test harness for the dap library host tests.
 * Zero dependencies; failures print a hex diff and the binary exits 1.
 */
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace testing {

struct Test {
	const char *name;
	std::function<void()> fn;
};

inline std::vector<Test> &registry()
{
	static std::vector<Test> r;
	return r;
}

inline int failures = 0;
inline const char *current_test = "";

struct Register {
	Register(const char *name, std::function<void()> fn)
	{
		registry().push_back({name, std::move(fn)});
	}
};

inline std::string hex(std::span<const uint8_t> b)
{
	std::string s;
	char tmp[4];
	for (uint8_t v : b) {
		std::snprintf(tmp, sizeof(tmp), "%02X ", v);
		s += tmp;
	}
	return s;
}

inline void fail(const char *file, int line, const std::string &msg)
{
	std::printf("FAIL %s (%s:%d)\n  %s\n", current_test, file, line, msg.c_str());
	++failures;
}

inline void check_bytes(const char *file, int line, std::span<const uint8_t> got,
			std::span<const uint8_t> want)
{
	if (got.size() == want.size() &&
	    std::equal(got.begin(), got.end(), want.begin())) {
		return;
	}
	fail(file, line, "bytes differ\n  want: " + hex(want) + "\n  got:  " + hex(got));
}

inline int run_all()
{
	for (const auto &t : registry()) {
		current_test = t.name;
		t.fn();
	}
	if (failures) {
		std::printf("\n%d FAILURE(S)\n", failures);
		return 1;
	}
	std::printf("\nOK: %zu tests\n", registry().size());
	return 0;
}

} // namespace testing

#define TEST(name)                                                             \
	static void test_fn_##name();                                          \
	static ::testing::Register reg_##name(#name, test_fn_##name);          \
	static void test_fn_##name()

#define CHECK(cond)                                                            \
	do {                                                                   \
		if (!(cond)) {                                                 \
			::testing::fail(__FILE__, __LINE__, "CHECK failed: " #cond); \
		}                                                              \
	} while (0)

#define CHECK_EQ(a, b)                                                         \
	do {                                                                   \
		auto va = (a);                                                 \
		auto vb = (b);                                                 \
		if (!(va == vb)) {                                             \
			::testing::fail(__FILE__, __LINE__,                    \
					std::string("CHECK_EQ failed: " #a " == " #b \
						    " (") + std::to_string((long long)va) + \
						" != " + std::to_string((long long)vb) + ")"); \
		}                                                              \
	} while (0)

#define CHECK_BYTES(got, want)                                                 \
	::testing::check_bytes(__FILE__, __LINE__, got, want)

#define TEST_MAIN()                                                            \
	int main()                                                             \
	{                                                                      \
		return ::testing::run_all();                                   \
	}
```

- [ ] **Step 2: Write tests/host/test_smoke.cpp**

```cpp
#include "testing.hpp"

TEST(harness_check_eq_works)
{
	CHECK_EQ(1 + 1, 2);
	CHECK(true);
}

TEST(harness_check_bytes_works)
{
	const uint8_t a[] = {0x00, 0x01};
	const uint8_t b[] = {0x00, 0x01};
	CHECK_BYTES(a, b);
}

TEST_MAIN()
```

- [ ] **Step 3: Write tests/host/CMakeLists.txt**

```cmake
cmake_minimum_required(VERSION 3.20)
project(dap_host_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -fno-exceptions -fno-rtti)

# The library under test (added in Task 1; keep commented until then)
# add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../lib/dap dap)

add_executable(dap_tests
  test_smoke.cpp
)
# target_link_libraries(dap_tests PRIVATE dap)
```

- [ ] **Step 4: Write scripts/run_host_tests.sh**

```bash
#!/usr/bin/env bash
# Configure, build and run the host unit tests for lib/dap.
set -euo pipefail
APP_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$APP_DIR/build/host_tests"
cmake -S "$APP_DIR/tests/host" -B "$BUILD" -DCMAKE_BUILD_TYPE=Debug >/dev/null
cmake --build "$BUILD" --parallel >/dev/null
"$BUILD/dap_tests"
```

`chmod +x scripts/run_host_tests.sh`

- [ ] **Step 5: Run it**

Run: `~/Developer/debugprobe-zephyr/scripts/run_host_tests.sh`
Expected: `OK: 2 tests`, exit 0.

- [ ] **Step 6: Commit**

```bash
cd ~/Developer/debugprobe-zephyr
git add tests/host scripts/run_host_tests.sh
git commit -m "test: host test harness for the dap library"
```

---

### Task 1: lib/dap public headers + mock driver + compile test

**Files:**
- Create: `lib/dap/include/dap/constants.hpp`
- Create: `lib/dap/include/dap/interfaces.hpp`
- Create: `lib/dap/include/dap/engine.hpp`
- Create: `lib/dap/CMakeLists.txt`
- Create: `tests/host/mock_swd_driver.hpp`
- Create: `tests/host/test_interfaces.cpp`
- Modify: `tests/host/CMakeLists.txt`

- [ ] **Step 1: Write lib/dap/include/dap/constants.hpp**

```cpp
#pragma once
/* CMSIS-DAP v2 protocol constants.
 * Values follow the ARM reference DAP.h; behavioral quirks follow the
 * Raspberry Pi-patched reference used by the original debugprobe firmware.
 */
#include <cstdint>

namespace dap {

/* Command IDs */
inline constexpr uint8_t kCmdInfo = 0x00;
inline constexpr uint8_t kCmdHostStatus = 0x01;
inline constexpr uint8_t kCmdConnect = 0x02;
inline constexpr uint8_t kCmdDisconnect = 0x03;
inline constexpr uint8_t kCmdTransferConfigure = 0x04;
inline constexpr uint8_t kCmdTransfer = 0x05;
inline constexpr uint8_t kCmdTransferBlock = 0x06;
inline constexpr uint8_t kCmdTransferAbort = 0x07;
inline constexpr uint8_t kCmdWriteAbort = 0x08;
inline constexpr uint8_t kCmdDelay = 0x09;
inline constexpr uint8_t kCmdResetTarget = 0x0A;
inline constexpr uint8_t kCmdSwjPins = 0x10;
inline constexpr uint8_t kCmdSwjClock = 0x11;
inline constexpr uint8_t kCmdSwjSequence = 0x12;
inline constexpr uint8_t kCmdSwdConfigure = 0x13;
inline constexpr uint8_t kCmdJtagSequence = 0x14;
inline constexpr uint8_t kCmdJtagConfigure = 0x15;
inline constexpr uint8_t kCmdJtagIdcode = 0x16;
inline constexpr uint8_t kCmdSwdSequence = 0x1D;
inline constexpr uint8_t kCmdQueueCommands = 0x7E;
inline constexpr uint8_t kCmdExecuteCommands = 0x7F;
inline constexpr uint8_t kCmdVendorFirst = 0x80;
inline constexpr uint8_t kCmdVendorLast = 0x9F;
inline constexpr uint8_t kCmdInvalid = 0xFF;

/* Command status byte */
inline constexpr uint8_t kDapOk = 0x00;
inline constexpr uint8_t kDapError = 0xFF;

/* DAP_Info IDs */
inline constexpr uint8_t kInfoVendor = 0x01;
inline constexpr uint8_t kInfoProduct = 0x02;
inline constexpr uint8_t kInfoSerial = 0x03;
inline constexpr uint8_t kInfoFwVersion = 0x04;
inline constexpr uint8_t kInfoDeviceVendor = 0x05;
inline constexpr uint8_t kInfoDeviceName = 0x06;
inline constexpr uint8_t kInfoCapabilities = 0xF0;
inline constexpr uint8_t kInfoTimestampClock = 0xF1;
inline constexpr uint8_t kInfoSwoBufferSize = 0xFD;
inline constexpr uint8_t kInfoPacketCount = 0xFE;
inline constexpr uint8_t kInfoPacketSize = 0xFF;

/* Transfer request byte bits */
inline constexpr uint8_t kReqApNdp = 1U << 0;
inline constexpr uint8_t kReqRnW = 1U << 1;
inline constexpr uint8_t kReqA2 = 1U << 2;
inline constexpr uint8_t kReqA3 = 1U << 3;
inline constexpr uint8_t kReqMatchValue = 1U << 4;
inline constexpr uint8_t kReqMatchMask = 1U << 5;
inline constexpr uint8_t kReqTimestamp = 1U << 7;

/* DP RDBUFF read request (used to flush posted AP reads) */
inline constexpr uint8_t kReqReadRdbuff = 0x0C | kReqRnW; /* DP addr 0xC, read */

/* Transfer response value: ACK in bits 0-2, then error/mismatch flags */
inline constexpr uint8_t kXferOk = 0x01;
inline constexpr uint8_t kXferWait = 0x02;
inline constexpr uint8_t kXferFault = 0x04;
inline constexpr uint8_t kXferNoAck = 0x07;
inline constexpr uint8_t kXferProtocolError = 0x08;
inline constexpr uint8_t kXferMismatch = 0x10;

/* SWJ pin bit positions */
inline constexpr uint8_t kPinSwclk = 1U << 0;
inline constexpr uint8_t kPinSwdio = 1U << 1;
inline constexpr uint8_t kPinTdi = 1U << 2;
inline constexpr uint8_t kPinTdo = 1U << 3;
inline constexpr uint8_t kPinNtrst = 1U << 5;
inline constexpr uint8_t kPinNreset = 1U << 7;

/* DAP_Connect port values */
inline constexpr uint8_t kPortDefault = 0;
inline constexpr uint8_t kPortSwd = 1;
inline constexpr uint8_t kPortJtag = 2;
inline constexpr uint8_t kPortDisabled = 0; /* response byte on refusal */

} // namespace dap
```

- [ ] **Step 2: Write lib/dap/include/dap/interfaces.hpp**

```cpp
#pragma once
/* Hardware and host-status abstractions injected into DapEngine.
 * Pure virtual so host tests can mock them; on target a thin adapter
 * wraps the Zephyr swdp device (phase 3).
 */
#include <cstdint>
#include <span>

namespace dap {

enum class Ack : uint8_t {
	Ok = 0x01,
	Wait = 0x02,
	Fault = 0x04,
	NoAck = 0x07,
	/* Driver may OR kXferProtocolError into the engine's response value
	 * by returning ProtocolError; data is then invalid. */
	ProtocolError = 0x08 | 0x07,
};

class SwdDriver {
public:
	virtual ~SwdDriver() = default;

	virtual void set_clock(uint32_t hz) = 0;
	virtual void connect() = 0;    /* claim pins, SWD mode (PORT_SWD_SETUP) */
	virtual void disconnect() = 0; /* release pins (PORT_OFF) */
	virtual void configure(uint8_t turnaround_cycles, bool data_phase) = 0;

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
```

- [ ] **Step 3: Write lib/dap/include/dap/engine.hpp**

```cpp
#pragma once
/* CMSIS-DAP v2 protocol engine: parses one request packet, drives the
 * SwdDriver, writes the response packet. No heap, no exceptions, no OS.
 */
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

#include "dap/interfaces.hpp"

namespace dap {

class DapEngine {
public:
	DapEngine(SwdDriver &driver, HostStatus &host, const Config &config);

	/* Process one transport packet (handles ExecuteCommands batching).
	 * Returns the number of response bytes written. Never reads outside
	 * `request` nor writes outside `response`; malformed input yields a
	 * DAP_ERROR / Invalid response, not UB.
	 */
	size_t process(std::span<const uint8_t> request, std::span<uint8_t> response);

	/* Transport hook for DAP_TransferAbort packets (0x07): abort the
	 * Transfer/TransferBlock currently executing on the DAP thread.
	 */
	void request_abort() { abort_.store(true, std::memory_order_relaxed); }

private:
	struct Sizes {
		size_t consumed; /* request bytes */
		size_t written;  /* response bytes */
	};

	Sizes dispatch(std::span<const uint8_t> req, std::span<uint8_t> resp);

	Sizes cmd_info(std::span<const uint8_t> req, std::span<uint8_t> resp);
	Sizes cmd_host_status(std::span<const uint8_t> req, std::span<uint8_t> resp);
	Sizes cmd_connect(std::span<const uint8_t> req, std::span<uint8_t> resp);
	Sizes cmd_disconnect(std::span<uint8_t> resp);
	Sizes cmd_delay(std::span<const uint8_t> req, std::span<uint8_t> resp);
	Sizes cmd_reset_target(std::span<uint8_t> resp);
	Sizes cmd_swj_pins(std::span<const uint8_t> req, std::span<uint8_t> resp);
	Sizes cmd_swj_clock(std::span<const uint8_t> req, std::span<uint8_t> resp);
	Sizes cmd_swj_sequence(std::span<const uint8_t> req, std::span<uint8_t> resp);
	Sizes cmd_swd_configure(std::span<const uint8_t> req, std::span<uint8_t> resp);
	Sizes cmd_swd_sequence(std::span<const uint8_t> req, std::span<uint8_t> resp);
	Sizes cmd_transfer_configure(std::span<const uint8_t> req, std::span<uint8_t> resp);
	Sizes cmd_transfer(std::span<const uint8_t> req, std::span<uint8_t> resp);
	Sizes cmd_transfer_block(std::span<const uint8_t> req, std::span<uint8_t> resp);
	Sizes cmd_write_abort(std::span<const uint8_t> req, std::span<uint8_t> resp);

	/* WAIT-retry wrapper around driver_.transfer() honoring retry_count_
	 * and the abort flag. */
	uint8_t transfer_retry(uint8_t request, uint32_t *data);

	SwdDriver &driver_;
	HostStatus &host_;
	Config config_;

	bool connected_ = false;
	uint8_t idle_cycles_ = 0;
	uint16_t retry_count_ = 100;
	uint16_t match_retry_ = 0;
	uint32_t match_mask_ = 0;
	std::atomic<bool> abort_{false};
};

} // namespace dap
```

- [ ] **Step 4: Write lib/dap/CMakeLists.txt** (dual-mode: Zephyr library when `ZEPHYR_BASE` is defined, plain static lib otherwise)

```cmake
# Builds as a Zephyr library inside the firmware build, and as a plain
# static library for host unit tests.
set(DAP_SOURCES
  src/engine.cpp
  src/swj.cpp
  src/transfer.cpp
)

if(DEFINED ZEPHYR_BASE)
  zephyr_library_named(dap)
  zephyr_library_sources(${DAP_SOURCES})
  zephyr_library_include_directories(include)
  zephyr_include_directories(include)
  zephyr_library_compile_options(-fno-exceptions -fno-rtti)
else()
  add_library(dap STATIC ${DAP_SOURCES})
  target_include_directories(dap PUBLIC include)
  target_compile_features(dap PUBLIC cxx_std_20)
  target_compile_options(dap PRIVATE -Wall -Wextra -fno-exceptions -fno-rtti)
endif()
```

(`src/engine.cpp`, `src/swj.cpp`, `src/transfer.cpp` are created in Tasks 2-6; for THIS task create them as empty placeholder translation units containing only `#include "dap/engine.hpp"` so the library links.)

- [ ] **Step 5: Write tests/host/mock_swd_driver.hpp**

```cpp
#pragma once
/* Recording/scripting mock of dap::SwdDriver for unit tests. */
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "dap/interfaces.hpp"

class MockSwdDriver : public dap::SwdDriver {
public:
	/* --- scripting --- */
	struct XferResult {
		dap::Ack ack;
		uint32_t read_data; /* returned through *data on reads */
	};
	/* Results are consumed FIFO; when empty, Ok/0xDEADBEEF is returned. */
	std::deque<XferResult> script;

	uint8_t pins_state = 0;     /* returned by read_pins() */
	uint32_t timestamp_value = 0;

	/* --- recordings --- */
	struct XferCall {
		uint8_t request;
		bool data_null;
		uint32_t write_data; /* valid when it was a write */
	};
	std::vector<XferCall> transfers;
	std::vector<std::string> events; /* coarse call log, in order */
	std::vector<uint8_t> swj_bits;   /* concatenated swj_sequence payloads */
	size_t swj_bit_count = 0;
	uint32_t last_clock = 0;
	uint8_t last_turnaround = 0;
	bool last_data_phase = false;
	uint8_t last_pin_select = 0, last_pin_value = 0;
	uint32_t last_pin_wait = 0;
	uint32_t delays_us = 0;
	std::vector<uint8_t> swd_input_data; /* bytes fed back by swd_input */

	void set_clock(uint32_t hz) override
	{
		last_clock = hz;
		events.push_back("set_clock");
	}
	void connect() override { events.push_back("connect"); }
	void disconnect() override { events.push_back("disconnect"); }
	void configure(uint8_t turnaround, bool data_phase) override
	{
		last_turnaround = turnaround;
		last_data_phase = data_phase;
		events.push_back("configure");
	}
	void swj_sequence(std::span<const uint8_t> data, size_t bits) override
	{
		swj_bits.insert(swj_bits.end(), data.begin(), data.end());
		swj_bit_count += bits;
		events.push_back("swj_sequence");
	}
	void swd_output(std::span<const uint8_t> data, size_t bits) override
	{
		swj_bits.insert(swj_bits.end(), data.begin(), data.end());
		swj_bit_count += bits;
		events.push_back("swd_output");
	}
	void swd_input(std::span<uint8_t> data, size_t bits) override
	{
		for (size_t i = 0; i < data.size(); ++i) {
			data[i] = i < swd_input_data.size() ? swd_input_data[i] : 0xA5;
		}
		(void)bits;
		events.push_back("swd_input");
	}
	dap::Ack transfer(uint8_t request, uint32_t *data) override
	{
		XferResult r{dap::Ack::Ok, 0xDEADBEEF};
		if (!script.empty()) {
			r = script.front();
			script.pop_front();
		}
		XferCall call{request, data == nullptr, 0};
		const bool is_read = (request & 0x02) != 0;
		if (data != nullptr) {
			if (is_read) {
				*data = r.read_data;
			} else {
				call.write_data = *data;
			}
		}
		transfers.push_back(call);
		return r.ack;
	}
	void set_pins(uint8_t select, uint8_t value, uint32_t wait_us) override
	{
		last_pin_select = select;
		last_pin_value = value;
		last_pin_wait = wait_us;
		events.push_back("set_pins");
	}
	uint8_t read_pins() override { return pins_state; }
	void delay_us(uint32_t us) override { delays_us += us; }
	uint32_t timestamp() override { return timestamp_value; }
};

class MockHostStatus : public dap::HostStatus {
public:
	int connected_calls = 0, running_calls = 0;
	bool last_connected = false, last_running = false;
	void connected(bool on) override
	{
		++connected_calls;
		last_connected = on;
	}
	void running(bool on) override
	{
		++running_calls;
		last_running = on;
	}
};
```

- [ ] **Step 6: Write tests/host/test_interfaces.cpp** (compile/link smoke for the headers + mock)

```cpp
#include "testing.hpp"
#include "mock_swd_driver.hpp"
#include "dap/constants.hpp"

TEST(mock_records_transfer_calls)
{
	MockSwdDriver drv;
	drv.script.push_back({dap::Ack::Ok, 0x12345678});
	uint32_t data = 0;
	dap::Ack ack = drv.transfer(dap::kReqRnW, &data); /* DP read */
	CHECK_EQ(static_cast<int>(ack), static_cast<int>(dap::Ack::Ok));
	CHECK_EQ(data, 0x12345678u);
	CHECK_EQ(drv.transfers.size(), 1u);
	CHECK(drv.transfers[0].data_null == false);
}

TEST(mock_default_script_is_ok)
{
	MockSwdDriver drv;
	uint32_t data = 0;
	CHECK_EQ(static_cast<int>(drv.transfer(dap::kReqRnW, &data)),
		 static_cast<int>(dap::Ack::Ok));
	CHECK_EQ(data, 0xDEADBEEFu);
}

TEST_MAIN()
```

NOTE: only ONE file may define `TEST_MAIN()`. From this task on, `test_smoke.cpp` keeps the `TEST_MAIN()`; every other test file contains only `TEST(...)` cases. Remove `TEST_MAIN()` from `test_interfaces.cpp` and keep it in `test_smoke.cpp` — the snippet above shows it only so the file is self-describing if built alone; follow the one-TEST_MAIN rule when wiring CMake.

- [ ] **Step 7: Update tests/host/CMakeLists.txt**

```cmake
cmake_minimum_required(VERSION 3.20)
project(dap_host_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -fno-exceptions -fno-rtti)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../lib/dap dap)

add_executable(dap_tests
  test_smoke.cpp
  test_interfaces.cpp
)
target_link_libraries(dap_tests PRIVATE dap)
```

- [ ] **Step 8: Run host tests**

Run: `~/Developer/debugprobe-zephyr/scripts/run_host_tests.sh`
Expected: `OK: 4 tests`, exit 0.

- [ ] **Step 9: Commit**

```bash
cd ~/Developer/debugprobe-zephyr
git add lib/dap tests/host
git commit -m "feat: dap library public interfaces, mock driver, host compile tests"
```

---

### Task 2: Engine dispatch + Info / HostStatus / Delay / unknown / vendor / ExecuteCommands

**Files:**
- Create: `lib/dap/src/engine.cpp` (replace the Task-1 placeholder)
- Create: `tests/host/test_engine_basics.cpp`
- Modify: `tests/host/CMakeLists.txt` (add the test file — same pattern for every later task; not repeated below)

- [ ] **Step 1: Write the failing tests** — `tests/host/test_engine_basics.cpp`. These byte sequences ARE the spec; they were derived from `DAP.c` as cited in the header notes.

```cpp
#include "testing.hpp"
#include "mock_swd_driver.hpp"
#include "dap/constants.hpp"
#include "dap/engine.hpp"

#include <vector>

namespace {

struct Fixture {
	MockSwdDriver drv;
	MockHostStatus host;
	dap::Config cfg; /* defaults match the original debugprobe */
	dap::DapEngine engine{drv, host, cfg};

	std::vector<uint8_t> run(std::initializer_list<uint8_t> req)
	{
		uint8_t resp[256] = {};
		size_t n = engine.process({req.begin(), req.size()}, resp);
		return {resp, resp + n};
	}
};

} // namespace

TEST(info_fw_version_includes_nul)
{
	Fixture f;
	auto r = f.run({0x00, dap::kInfoFwVersion});
	const uint8_t want[] = {0x00, 6, '2', '.', '0', '.', '0', 0x00};
	CHECK_BYTES(r, want);
}

TEST(info_empty_strings_when_unset)
{
	Fixture f;
	for (uint8_t id : {dap::kInfoVendor, dap::kInfoProduct, dap::kInfoSerial,
			   dap::kInfoDeviceVendor, dap::kInfoDeviceName}) {
		auto r = f.run({0x00, id});
		const uint8_t want[] = {0x00, 0};
		CHECK_BYTES(r, want);
	}
}

TEST(info_strings_injected)
{
	Fixture f;
	f.cfg.vendor = "Raspberry Pi";
	dap::DapEngine e{f.drv, f.host, f.cfg};
	uint8_t resp[64];
	const uint8_t req[] = {0x00, dap::kInfoVendor};
	size_t n = e.process(req, resp);
	CHECK_EQ(n, 2u + 13u); /* "Raspberry Pi" + NUL */
	CHECK_EQ(resp[1], 13);
	CHECK_EQ(resp[2], 'R');
	CHECK_EQ(resp[14], 0x00);
}

TEST(info_capabilities_swd_atomic_timestamp)
{
	Fixture f;
	auto r = f.run({0x00, dap::kInfoCapabilities});
	const uint8_t want[] = {0x00, 1, 0x31};
	CHECK_BYTES(r, want);
}

TEST(info_packet_size_and_count)
{
	Fixture f;
	auto rs = f.run({0x00, dap::kInfoPacketSize});
	const uint8_t want_size[] = {0x00, 2, 64, 0};
	CHECK_BYTES(rs, want_size);
	auto rc = f.run({0x00, dap::kInfoPacketCount});
	const uint8_t want_cnt[] = {0x00, 1, 6}; /* original's count-2 quirk */
	CHECK_BYTES(rc, want_cnt);
}

TEST(info_timestamp_clock)
{
	Fixture f;
	auto r = f.run({0x00, dap::kInfoTimestampClock});
	const uint8_t want[] = {0x00, 4, 0x40, 0x42, 0x0F, 0x00}; /* 1000000 LE */
	CHECK_BYTES(r, want);
}

TEST(info_unknown_id_and_swo_buffer_empty)
{
	Fixture f;
	for (uint8_t id : {uint8_t{0x42}, dap::kInfoSwoBufferSize}) {
		auto r = f.run({0x00, id});
		const uint8_t want[] = {0x00, 0};
		CHECK_BYTES(r, want);
	}
}

TEST(host_status_drives_leds)
{
	Fixture f;
	auto r = f.run({0x01, 0x00, 0x01}); /* connected LED on */
	const uint8_t want[] = {0x01, dap::kDapOk};
	CHECK_BYTES(r, want);
	CHECK_EQ(f.host.connected_calls, 1);
	CHECK(f.host.last_connected);

	r = f.run({0x01, 0x01, 0x00}); /* running LED off */
	CHECK_BYTES(r, want);
	CHECK_EQ(f.host.running_calls, 1);
	CHECK(!f.host.last_running);
}

TEST(host_status_bad_type_is_error)
{
	Fixture f;
	auto r = f.run({0x01, 0x02, 0x01});
	const uint8_t want[] = {0x01, dap::kDapError};
	CHECK_BYTES(r, want);
}

TEST(delay_calls_driver)
{
	Fixture f;
	auto r = f.run({0x09, 0xE8, 0x03}); /* 1000 us */
	const uint8_t want[] = {0x09, dap::kDapOk};
	CHECK_BYTES(r, want);
	CHECK_EQ(f.drv.delays_us, 1000u);
}

TEST(unknown_command_responds_invalid)
{
	Fixture f;
	for (uint8_t id : {uint8_t{0x17}, uint8_t{0x1C}, uint8_t{0x40},
			   uint8_t{0x7E}, uint8_t{0xFE}}) {
		auto r = f.run({id});
		const uint8_t want[] = {dap::kCmdInvalid};
		CHECK_BYTES(r, want);
	}
}

TEST(jtag_commands_respond_error)
{
	/* JTAG compiled out: handlers exist but answer DAP_ERROR
	 * (JTAG_Sequence DAP.c:~520; Configure/IDCODE similar). */
	Fixture f;
	auto r = f.run({0x14, 0x01, 0x01, 0xAA}); /* JTAG_Sequence */
	const uint8_t want14[] = {0x14, dap::kDapError};
	CHECK_BYTES(r, want14);
	r = f.run({0x15, 0x01, 0x04}); /* JTAG_Configure */
	const uint8_t want15[] = {0x15, dap::kDapError};
	CHECK_BYTES(r, want15);
	r = f.run({0x16, 0x00}); /* JTAG_IDCODE: 1-byte error (DAP.c id_error path) */
	const uint8_t want16[] = {0x16, dap::kDapError};
	CHECK_BYTES(r, want16);
}

TEST(vendor_command_echoes_id_only)
{
	Fixture f;
	auto r = f.run({0x85, 0x01, 0x02});
	const uint8_t want[] = {0x85};
	CHECK_BYTES(r, want);
}

TEST(execute_commands_batches)
{
	Fixture f;
	/* 0x7F, count=2: Delay(100us), Info(FW ver) */
	auto r = f.run({0x7F, 0x02, 0x09, 0x64, 0x00, 0x00, dap::kInfoFwVersion});
	const uint8_t want[] = {0x7F, 0x02,
				0x09, dap::kDapOk,
				0x00, 6, '2', '.', '0', '.', '0', 0x00};
	CHECK_BYTES(r, want);
	CHECK_EQ(f.drv.delays_us, 100u);
}

TEST(empty_request_is_invalid)
{
	Fixture f;
	uint8_t resp[8];
	size_t n = f.engine.process({}, resp);
	CHECK_EQ(n, 1u);
	CHECK_EQ(resp[0], dap::kCmdInvalid);
}

TEST(truncated_info_request_no_crash)
{
	Fixture f;
	auto r = f.run({0x00}); /* missing info id byte */
	const uint8_t want[] = {0x00, 0};
	CHECK_BYTES(r, want);
}
```

- [ ] **Step 2: Run to verify failure**

Run: `~/Developer/debugprobe-zephyr/scripts/run_host_tests.sh`
Expected: link error (engine symbols missing) — that counts as the failing state.

- [ ] **Step 3: Implement `lib/dap/src/engine.cpp`** — constructor, `process()` (ExecuteCommands loop mirroring `DAP_ExecuteCommand`, `DAP.c:1728-1746`), `dispatch()` mirroring `DAP_ProcessCommand` (`DAP.c:1615-1720`), and the handlers covered by the tests above. Implementation skeleton (complete for this task's scope; transfer/swj handlers are declared but defined in `swj.cpp`/`transfer.cpp` — for THIS task give them temporary definitions returning `{rest-of-request, kDapError}`-style stubs in their own files so the build links; Tasks 3-6 replace them):

  > **Amended during execution:** vendor/JTAG consumed-byte parity for ExecuteCommands batches; cmd_info empty-request guard.

```cpp
#include "dap/engine.hpp"

#include <cstring>

#include "dap/constants.hpp"

namespace dap {

namespace {

size_t string_info(const char *s, uint8_t *out, size_t cap)
{
	if (s == nullptr) {
		return 0;
	}
	size_t len = std::strlen(s) + 1; /* include NUL, like the reference */
	if (len > 255 || len + 2 > cap) {
		return 0;
	}
	std::memcpy(out, s, len);
	return len;
}

} // namespace

DapEngine::DapEngine(SwdDriver &driver, HostStatus &host, const Config &config)
	: driver_(driver), host_(host), config_(config)
{
}

size_t DapEngine::process(std::span<const uint8_t> request, std::span<uint8_t> response)
{
	if (request.empty() || response.size() < 2) {
		if (!response.empty()) {
			response[0] = kCmdInvalid;
			return 1;
		}
		return 0;
	}

	if (request[0] == kCmdExecuteCommands) {
		if (request.size() < 2) {
			response[0] = kCmdInvalid;
			return 1;
		}
		size_t cnt = request[1];
		response[0] = kCmdExecuteCommands;
		response[1] = static_cast<uint8_t>(cnt);
		size_t req_off = 2, resp_off = 2;
		while (cnt--) {
			Sizes s = dispatch(request.subspan(req_off),
					   response.subspan(resp_off));
			req_off += s.consumed;
			resp_off += s.written;
			if (req_off >= request.size() && cnt) {
				break; /* malformed batch: stop, report what ran */
			}
		}
		return resp_off;
	}

	return dispatch(request, response).written;
}

DapEngine::Sizes DapEngine::dispatch(std::span<const uint8_t> req, std::span<uint8_t> resp)
{
	const uint8_t cmd = req[0];

	if (cmd >= kCmdVendorFirst && cmd <= kCmdVendorLast) {
		resp[0] = cmd; /* default DAP_vendor.c: echo only */
		return {1, 1};
	}

	resp[0] = cmd;
	auto body_req = req.subspan(1);
	auto body_resp = resp.subspan(1);

	Sizes s;
	switch (cmd) {
	case kCmdInfo:
		s = cmd_info(body_req, body_resp);
		break;
	case kCmdHostStatus:
		s = cmd_host_status(body_req, body_resp);
		break;
	case kCmdConnect:
		s = cmd_connect(body_req, body_resp);
		break;
	case kCmdDisconnect:
		s = cmd_disconnect(body_resp);
		break;
	case kCmdDelay:
		s = cmd_delay(body_req, body_resp);
		break;
	case kCmdResetTarget:
		s = cmd_reset_target(body_resp);
		break;
	case kCmdSwjPins:
		s = cmd_swj_pins(body_req, body_resp);
		break;
	case kCmdSwjClock:
		s = cmd_swj_clock(body_req, body_resp);
		break;
	case kCmdSwjSequence:
		s = cmd_swj_sequence(body_req, body_resp);
		break;
	case kCmdSwdConfigure:
		s = cmd_swd_configure(body_req, body_resp);
		break;
	case kCmdSwdSequence:
		s = cmd_swd_sequence(body_req, body_resp);
		break;
	case kCmdJtagSequence: /* JTAG not available: error, but walk the body */
		body_resp[0] = kDapError;
		s = {jtag_sequence_body_len(body_req), 1};
		break;
	case kCmdJtagConfigure: {
		body_resp[0] = kDapError;
		size_t n = body_req.empty() ? 0 : 1 + static_cast<size_t>(body_req[0]);
		s = {n > body_req.size() ? body_req.size() : n, 1};
		break;
	}
	case kCmdJtagIdcode:   /* JTAG compiled out: 1-byte error (DAP.c id_error) */
		body_resp[0] = kDapError;
		s = {body_req.empty() ? 0u : 1u, 1};
		break;
	case kCmdTransferConfigure:
		s = cmd_transfer_configure(body_req, body_resp);
		break;
	case kCmdTransfer:
		s = cmd_transfer(body_req, body_resp);
		break;
	case kCmdTransferBlock:
		s = cmd_transfer_block(body_req, body_resp);
		break;
	case kCmdWriteAbort:
		s = cmd_write_abort(body_req, body_resp);
		break;
	default:
		resp[0] = kCmdInvalid;
		return {req.size(), 1};
	}

	return {1 + s.consumed, 1 + s.written};
}

DapEngine::Sizes DapEngine::cmd_info(std::span<const uint8_t> req, std::span<uint8_t> resp)
{
	uint8_t id = req.empty() ? 0xFFU /* unknown -> len 0 */ : req[0];
	uint8_t *payload = resp.data() + 1;
	size_t cap = resp.size();
	size_t len = 0;

	switch (id) {
	case kInfoVendor:
		len = string_info(config_.vendor, payload, cap);
		break;
	case kInfoProduct:
		len = string_info(config_.product, payload, cap);
		break;
	case kInfoSerial:
		len = string_info(config_.serial, payload, cap);
		break;
	case kInfoFwVersion:
		len = string_info(config_.fw_version, payload, cap);
		break;
	case kInfoCapabilities:
		payload[0] = 0x01 /* SWD */ | 0x10 /* atomic */ |
			     (config_.timestamp_clock ? 0x20 : 0x00);
		len = 1;
		break;
	case kInfoTimestampClock:
		if (config_.timestamp_clock != 0) {
			payload[0] = static_cast<uint8_t>(config_.timestamp_clock);
			payload[1] = static_cast<uint8_t>(config_.timestamp_clock >> 8);
			payload[2] = static_cast<uint8_t>(config_.timestamp_clock >> 16);
			payload[3] = static_cast<uint8_t>(config_.timestamp_clock >> 24);
			len = 4;
		}
		break;
	case kInfoPacketSize:
		payload[0] = static_cast<uint8_t>(config_.packet_size);
		payload[1] = static_cast<uint8_t>(config_.packet_size >> 8);
		len = 2;
		break;
	case kInfoPacketCount:
		payload[0] = config_.packet_count;
		len = 1;
		break;
	default: /* device vendor/name, SWO buffer, unknown: empty */
		break;
	}

	resp[0] = static_cast<uint8_t>(len);
	return {req.empty() ? 0u : 1u, 1 + len};
}

DapEngine::Sizes DapEngine::cmd_host_status(std::span<const uint8_t> req,
					    std::span<uint8_t> resp)
{
	if (req.size() < 2) {
		resp[0] = kDapError;
		return {req.size(), 1};
	}
	const bool on = req[1] != 0;
	switch (req[0]) {
	case 0:
		host_.connected(on);
		resp[0] = kDapOk;
		break;
	case 1:
		host_.running(on);
		resp[0] = kDapOk;
		break;
	default:
		resp[0] = kDapError;
		break;
	}
	return {2, 1};
}

DapEngine::Sizes DapEngine::cmd_delay(std::span<const uint8_t> req, std::span<uint8_t> resp)
{
	if (req.size() < 2) {
		resp[0] = kDapError;
		return {req.size(), 1};
	}
	driver_.delay_us(static_cast<uint32_t>(req[0]) |
			 (static_cast<uint32_t>(req[1]) << 8));
	resp[0] = kDapOk;
	return {2, 1};
}

} // namespace dap
```

Handlers not shown here (`cmd_connect`, `cmd_disconnect`, `cmd_reset_target`, the `cmd_swj_*`, `cmd_swd_*`, `cmd_transfer*`, `cmd_write_abort`, `transfer_retry`) get temporary stub definitions in `lib/dap/src/swj.cpp` / `lib/dap/src/transfer.cpp` for this task:

```cpp
/* lib/dap/src/swj.cpp — Task 2 stubs; replaced in Tasks 3-4 */
#include "dap/constants.hpp"
#include "dap/engine.hpp"

namespace dap {
#define DAP_STUB(name)                                                          \
	DapEngine::Sizes DapEngine::name(std::span<const uint8_t> req,         \
					 std::span<uint8_t> resp)               \
	{                                                                       \
		resp[0] = kDapError;                                            \
		return {req.size(), 1};                                         \
	}
DAP_STUB(cmd_connect)
DAP_STUB(cmd_swj_pins)
DAP_STUB(cmd_swj_clock)
DAP_STUB(cmd_swj_sequence)
DAP_STUB(cmd_swd_configure)
DAP_STUB(cmd_swd_sequence)
#undef DAP_STUB
DapEngine::Sizes DapEngine::cmd_disconnect(std::span<uint8_t> resp)
{
	resp[0] = kDapError;
	return {0, 1};
}
DapEngine::Sizes DapEngine::cmd_reset_target(std::span<uint8_t> resp)
{
	resp[0] = kDapError;
	return {0, 1};
}
} // namespace dap
```

```cpp
/* lib/dap/src/transfer.cpp — Task 2 stubs; replaced in Tasks 5-6 */
#include "dap/constants.hpp"
#include "dap/engine.hpp"

namespace dap {
DapEngine::Sizes DapEngine::cmd_transfer_configure(std::span<const uint8_t> req,
						   std::span<uint8_t> resp)
{
	resp[0] = kDapError;
	return {req.size(), 1};
}
DapEngine::Sizes DapEngine::cmd_transfer(std::span<const uint8_t> req,
					 std::span<uint8_t> resp)
{
	resp[0] = kDapError;
	return {req.size(), 1};
}
DapEngine::Sizes DapEngine::cmd_transfer_block(std::span<const uint8_t> req,
					       std::span<uint8_t> resp)
{
	resp[0] = kDapError;
	return {req.size(), 1};
}
DapEngine::Sizes DapEngine::cmd_write_abort(std::span<const uint8_t> req,
					    std::span<uint8_t> resp)
{
	resp[0] = kDapError;
	return {req.size(), 1};
}
uint8_t DapEngine::transfer_retry(uint8_t request, uint32_t *data)
{
	(void)request;
	(void)data;
	return kXferNoAck;
}
} // namespace dap
```

- [ ] **Step 4: Run tests until green**

Run: `~/Developer/debugprobe-zephyr/scripts/run_host_tests.sh`
Expected: all Task-2 tests PASS (stubs aren't exercised by them).

- [ ] **Step 5: Commit**

```bash
cd ~/Developer/debugprobe-zephyr
git add lib/dap tests/host
git commit -m "feat: dap engine dispatch, Info/HostStatus/Delay/ExecuteCommands"
```

---

### Task 3: Connect / Disconnect / ResetTarget / SWJ_Clock / SWJ_Pins / WriteABORT

**Files:**
- Modify: `lib/dap/src/swj.cpp` (replace the relevant stubs)
- Modify: `lib/dap/src/transfer.cpp` (`cmd_write_abort`, `transfer_retry`)
- Create: `tests/host/test_connect_swj.cpp`

- [ ] **Step 1: Write the failing tests**

```cpp
#include "testing.hpp"
#include "mock_swd_driver.hpp"
#include "dap/constants.hpp"
#include "dap/engine.hpp"

#include <vector>

namespace {
struct Fixture {
	MockSwdDriver drv;
	MockHostStatus host;
	dap::Config cfg;
	dap::DapEngine engine{drv, host, cfg};
	std::vector<uint8_t> run(std::initializer_list<uint8_t> req)
	{
		uint8_t resp[256] = {};
		size_t n = engine.process({req.begin(), req.size()}, resp);
		return {resp, resp + n};
	}
};
} // namespace

TEST(connect_default_selects_swd)
{
	Fixture f;
	auto r = f.run({0x02, dap::kPortDefault});
	const uint8_t want[] = {0x02, dap::kPortSwd};
	CHECK_BYTES(r, want);
	CHECK_EQ(f.drv.events.size(), 1u);
	CHECK(f.drv.events[0] == "connect");
}

TEST(connect_swd_explicit)
{
	Fixture f;
	auto r = f.run({0x02, dap::kPortSwd});
	const uint8_t want[] = {0x02, dap::kPortSwd};
	CHECK_BYTES(r, want);
}

TEST(connect_jtag_refused)
{
	Fixture f;
	auto r = f.run({0x02, dap::kPortJtag});
	const uint8_t want[] = {0x02, dap::kPortDisabled};
	CHECK_BYTES(r, want);
	CHECK(f.drv.events.empty()); /* no pin takeover on refusal */
}

TEST(disconnect_releases_port)
{
	Fixture f;
	f.run({0x02, dap::kPortSwd});
	auto r = f.run({0x03});
	const uint8_t want[] = {0x03, dap::kDapOk};
	CHECK_BYTES(r, want);
	CHECK(f.drv.events.back() == "disconnect");
}

TEST(reset_target_ok_no_device_hook)
{
	Fixture f;
	auto r = f.run({0x0A});
	const uint8_t want[] = {0x0A, dap::kDapOk, 0x00};
	CHECK_BYTES(r, want);
}

TEST(swj_clock_sets_driver)
{
	Fixture f;
	auto r = f.run({0x11, 0x40, 0x42, 0x0F, 0x00}); /* 1 MHz LE */
	const uint8_t want[] = {0x11, dap::kDapOk};
	CHECK_BYTES(r, want);
	CHECK_EQ(f.drv.last_clock, 1000000u);
}

TEST(swj_clock_zero_is_error)
{
	Fixture f;
	auto r = f.run({0x11, 0x00, 0x00, 0x00, 0x00});
	const uint8_t want[] = {0x11, dap::kDapError};
	CHECK_BYTES(r, want);
}

TEST(swj_pins_sets_and_reads)
{
	Fixture f;
	f.drv.pins_state = dap::kPinNreset | dap::kPinSwclk;
	/* drive nRESET low, select only nRESET, wait 10us */
	auto r = f.run({0x10, 0x00, dap::kPinNreset, 0x0A, 0x00, 0x00, 0x00});
	const uint8_t want[] = {0x10, static_cast<uint8_t>(dap::kPinNreset | dap::kPinSwclk)};
	CHECK_BYTES(r, want);
	CHECK_EQ(f.drv.last_pin_select, dap::kPinNreset);
	CHECK_EQ(f.drv.last_pin_value, 0);
	CHECK_EQ(f.drv.last_pin_wait, 10u);
}

TEST(write_abort_when_connected)
{
	Fixture f;
	f.run({0x02, dap::kPortSwd});
	/* index 0, value 0x0000001E -> DP write addr 0 (request byte 0x00) */
	auto r = f.run({0x08, 0x00, 0x1E, 0x00, 0x00, 0x00});
	const uint8_t want[] = {0x08, dap::kDapOk};
	CHECK_BYTES(r, want);
	CHECK_EQ(f.drv.transfers.size(), 1u);
	CHECK_EQ(f.drv.transfers[0].request, 0x00);
	CHECK_EQ(f.drv.transfers[0].write_data, 0x0000001Eu);
}

TEST(write_abort_disconnected_is_error)
{
	Fixture f;
	auto r = f.run({0x08, 0x00, 0x1E, 0x00, 0x00, 0x00});
	const uint8_t want[] = {0x08, dap::kDapError};
	CHECK_BYTES(r, want);
	CHECK(f.drv.transfers.empty());
}
```

- [ ] **Step 2: Run — expect the new tests to FAIL against the stubs.**

- [ ] **Step 3: Implement.** In `swj.cpp` replace the stubs for connect/disconnect/reset/clock/pins. Reference: `DAP.c` `DAP_Connect` (~line 300), `DAP_Disconnect`, `DAP_ResetTarget`, `DAP_SWJ_Pins` (~380), `DAP_SWJ_Clock` (~470). Implementation:

```cpp
DapEngine::Sizes DapEngine::cmd_connect(std::span<const uint8_t> req,
					std::span<uint8_t> resp)
{
	const uint8_t want = req.empty() ? kPortDefault : req[0];
	if (want == kPortDefault || want == kPortSwd) {
		driver_.connect();
		connected_ = true;
		resp[0] = kPortSwd;
	} else {
		resp[0] = kPortDisabled; /* JTAG unavailable */
	}
	return {req.empty() ? 0u : 1u, 1};
}

DapEngine::Sizes DapEngine::cmd_disconnect(std::span<uint8_t> resp)
{
	connected_ = false;
	driver_.disconnect();
	resp[0] = kDapOk;
	return {0, 1};
}

DapEngine::Sizes DapEngine::cmd_reset_target(std::span<uint8_t> resp)
{
	resp[0] = kDapOk;
	resp[1] = 0x00; /* no device-specific reset sequence on this probe */
	return {0, 2};
}

DapEngine::Sizes DapEngine::cmd_swj_clock(std::span<const uint8_t> req,
					  std::span<uint8_t> resp)
{
	if (req.size() < 4) {
		resp[0] = kDapError;
		return {req.size(), 1};
	}
	const uint32_t hz = static_cast<uint32_t>(req[0]) |
			    (static_cast<uint32_t>(req[1]) << 8) |
			    (static_cast<uint32_t>(req[2]) << 16) |
			    (static_cast<uint32_t>(req[3]) << 24);
	if (hz == 0) {
		resp[0] = kDapError;
	} else {
		driver_.set_clock(hz);
		resp[0] = kDapOk;
	}
	return {4, 1};
}

DapEngine::Sizes DapEngine::cmd_swj_pins(std::span<const uint8_t> req,
					 std::span<uint8_t> resp)
{
	if (req.size() < 6) {
		resp[0] = kDapError;
		return {req.size(), 1};
	}
	const uint8_t value = req[0];
	const uint8_t select = req[1];
	const uint32_t wait_us = static_cast<uint32_t>(req[2]) |
				 (static_cast<uint32_t>(req[3]) << 8) |
				 (static_cast<uint32_t>(req[4]) << 16) |
				 (static_cast<uint32_t>(req[5]) << 24);
	driver_.set_pins(select, value, wait_us);
	resp[0] = driver_.read_pins();
	return {6, 1};
}
```

In `transfer.cpp` replace `cmd_write_abort` and `transfer_retry`:

```cpp
uint8_t DapEngine::transfer_retry(uint8_t request, uint32_t *data)
{
	uint32_t retry = retry_count_;
	uint8_t response_value;
	do {
		response_value = static_cast<uint8_t>(driver_.transfer(request, data));
	} while (response_value == kXferWait && retry-- &&
		 !abort_.load(std::memory_order_relaxed));
	return response_value;
}

DapEngine::Sizes DapEngine::cmd_write_abort(std::span<const uint8_t> req,
					    std::span<uint8_t> resp)
{
	if (!connected_ || req.size() < 5) {
		resp[0] = kDapError;
		return {req.size() < 5 ? req.size() : 5, 1};
	}
	/* req[0] is the DAP index (ignored, single DP) */
	uint32_t value = static_cast<uint32_t>(req[1]) |
			 (static_cast<uint32_t>(req[2]) << 8) |
			 (static_cast<uint32_t>(req[3]) << 16) |
			 (static_cast<uint32_t>(req[4]) << 24);
	/* DP write, address 0 (ABORT): request byte 0x00 */
	const uint8_t ack = transfer_retry(0x00, &value);
	resp[0] = (ack == kXferOk) ? kDapOk : kDapError;
	return {5, 1};
}
```

- [ ] **Step 4: Run tests — all green (Task 2 tests must stay green).**

- [ ] **Step 5: Commit**

```bash
git add lib/dap tests/host
git commit -m "feat: dap Connect/Disconnect/ResetTarget/SWJ pins+clock/WriteABORT"
```

---

### Task 4: SWJ_Sequence / SWD_Configure / SWD_Sequence

**Files:**
- Modify: `lib/dap/src/swj.cpp`
- Create: `tests/host/test_sequences.cpp`

- [ ] **Step 1: Write the failing tests**

```cpp
#include "testing.hpp"
#include "mock_swd_driver.hpp"
#include "dap/constants.hpp"
#include "dap/engine.hpp"

#include <vector>

namespace {
struct Fixture {
	MockSwdDriver drv;
	MockHostStatus host;
	dap::Config cfg;
	dap::DapEngine engine{drv, host, cfg};
	std::vector<uint8_t> run(std::initializer_list<uint8_t> req)
	{
		uint8_t resp[256] = {};
		size_t n = engine.process({req.begin(), req.size()}, resp);
		return {resp, resp + n};
	}
};
} // namespace

TEST(swj_sequence_basic)
{
	Fixture f;
	/* 16 bits: 0xFF 0x07 (line reset fragment) */
	auto r = f.run({0x12, 16, 0xFF, 0x07});
	const uint8_t want[] = {0x12, dap::kDapOk};
	CHECK_BYTES(r, want);
	CHECK_EQ(f.drv.swj_bit_count, 16u);
	CHECK_EQ(f.drv.swj_bits.size(), 2u);
	CHECK_EQ(f.drv.swj_bits[0], 0xFF);
	CHECK_EQ(f.drv.swj_bits[1], 0x07);
}

TEST(swj_sequence_zero_means_256)
{
	Fixture f;
	std::vector<uint8_t> req = {0x12, 0};
	for (int i = 0; i < 32; ++i) {
		req.push_back(0xFF);
	}
	uint8_t resp[8];
	size_t n = f.engine.process({req.data(), req.size()}, resp);
	CHECK_EQ(n, 2u);
	CHECK_EQ(resp[1], dap::kDapOk);
	CHECK_EQ(f.drv.swj_bit_count, 256u);
}

TEST(swj_sequence_truncated_is_error)
{
	Fixture f;
	auto r = f.run({0x12, 16, 0xFF}); /* one data byte short */
	const uint8_t want[] = {0x12, dap::kDapError};
	CHECK_BYTES(r, want);
	CHECK_EQ(f.drv.swj_bit_count, 0u);
}

TEST(swd_configure_turnaround_dataphase)
{
	Fixture f;
	/* config 0x05: turnaround = (1)+1 = 2 cycles, data_phase = 1 */
	auto r = f.run({0x13, 0x05});
	const uint8_t want[] = {0x13, dap::kDapOk};
	CHECK_BYTES(r, want);
	CHECK_EQ(f.drv.last_turnaround, 2);
	CHECK(f.drv.last_data_phase);
}

TEST(swd_sequence_output_then_input)
{
	Fixture f;
	f.drv.swd_input_data = {0xAB};
	/* 2 sequences: output 8 cycles data 0x9E; input 8 cycles */
	auto r = f.run({0x1D, 0x02, 0x08, 0x9E, 0x88});
	const uint8_t want[] = {0x1D, dap::kDapOk, 0xAB};
	CHECK_BYTES(r, want);
	CHECK_EQ(f.drv.swj_bit_count, 8u); /* the output sequence */
}

TEST(swd_sequence_count_0_means_64)
{
	Fixture f;
	/* one output sequence, info 0x00 -> 64 cycles -> 8 data bytes */
	auto r = f.run({0x1D, 0x01, 0x00, 1, 2, 3, 4, 5, 6, 7, 8});
	const uint8_t want[] = {0x1D, dap::kDapOk};
	CHECK_BYTES(r, want);
	CHECK_EQ(f.drv.swj_bit_count, 64u);
}

TEST(swd_sequence_truncated_is_error)
{
	Fixture f;
	auto r = f.run({0x1D, 0x01, 0x08}); /* missing output byte */
	const uint8_t want[] = {0x1D, dap::kDapError};
	CHECK_BYTES(r, want);
}
```

- [ ] **Step 2: Run — new tests FAIL.**

- [ ] **Step 3: Implement** in `swj.cpp`. Reference: `DAP_SWJ_Sequence` (`DAP.c:~500`), `DAP_SWD_Configure` (~560), `DAP_SWD_Sequence` (~575). Implementation:

```cpp
DapEngine::Sizes DapEngine::cmd_swj_sequence(std::span<const uint8_t> req,
					     std::span<uint8_t> resp)
{
	if (req.empty()) {
		resp[0] = kDapError;
		return {0, 1};
	}
	size_t bits = req[0];
	if (bits == 0) {
		bits = 256;
	}
	const size_t nbytes = (bits + 7) / 8;
	if (req.size() < 1 + nbytes) {
		resp[0] = kDapError;
		return {req.size(), 1};
	}
	driver_.swj_sequence(req.subspan(1, nbytes), bits);
	resp[0] = kDapOk;
	return {1 + nbytes, 1};
}

DapEngine::Sizes DapEngine::cmd_swd_configure(std::span<const uint8_t> req,
					      std::span<uint8_t> resp)
{
	if (req.empty()) {
		resp[0] = kDapError;
		return {0, 1};
	}
	const uint8_t cfg = req[0];
	driver_.configure(static_cast<uint8_t>((cfg & 0x03U) + 1U),
			  (cfg & 0x04U) != 0);
	resp[0] = kDapOk;
	return {1, 1};
}

DapEngine::Sizes DapEngine::cmd_swd_sequence(std::span<const uint8_t> req,
					     std::span<uint8_t> resp)
{
	if (req.empty()) {
		resp[0] = kDapError;
		return {0, 1};
	}
	size_t seq_count = req[0];
	size_t req_off = 1;
	size_t resp_off = 1; /* resp[0] is the status byte */

	while (seq_count--) {
		if (req_off >= req.size()) {
			resp[0] = kDapError;
			return {req.size(), 1};
		}
		const uint8_t info = req[req_off++];
		size_t cycles = info & 0x3FU;
		if (cycles == 0) {
			cycles = 64;
		}
		const size_t nbytes = (cycles + 7) / 8;
		if ((info & 0x80U) != 0) {
			/* input */
			if (resp_off + nbytes > resp.size()) {
				resp[0] = kDapError;
				return {req.size(), 1};
			}
			driver_.swd_input(resp.subspan(resp_off, nbytes), cycles);
			resp_off += nbytes;
		} else {
			/* output */
			if (req_off + nbytes > req.size()) {
				resp[0] = kDapError;
				return {req.size(), 1};
			}
			driver_.swd_output(req.subspan(req_off, nbytes), cycles);
			req_off += nbytes;
		}
	}
	resp[0] = kDapOk;
	return {req_off, resp_off};
}
```

- [ ] **Step 4: Run tests — all green.**

- [ ] **Step 5: Commit**

```bash
git add lib/dap tests/host
git commit -m "feat: dap SWJ_Sequence, SWD_Configure, SWD_Sequence"
```

---

### Task 5: TransferConfigure + Transfer (posted reads, match value/mask, retries, abort)

This is the heart of the protocol. Port `DAP_SWD_Transfer` (`DAP.c:657-940`) faithfully — same posting pipeline, same retry semantics, same response layout. Read that function before writing code.

**Files:**
- Modify: `lib/dap/src/transfer.cpp`
- Create: `tests/host/test_transfer.cpp`

**Response layout reminder** (`DAP_Transfer`): `{cmd(0x05), count(1), response_value(1), data...}` — count = number of completed transfers, response_value = last ACK (with mismatch/protocol-error bits). On entry the engine must clear the abort flag (`DAP.c:682`).

- [ ] **Step 1: Write the failing tests**

```cpp
#include "testing.hpp"
#include "mock_swd_driver.hpp"
#include "dap/constants.hpp"
#include "dap/engine.hpp"

#include <vector>

namespace {
struct Fixture {
	MockSwdDriver drv;
	MockHostStatus host;
	dap::Config cfg;
	dap::DapEngine engine{drv, host, cfg};

	Fixture() { connect(); }
	void connect()
	{
		uint8_t resp[8];
		const uint8_t req[] = {0x02, dap::kPortSwd};
		engine.process(req, resp);
		drv.events.clear();
	}
	std::vector<uint8_t> run(std::vector<uint8_t> req)
	{
		uint8_t resp[256] = {};
		size_t n = engine.process({req.data(), req.size()}, resp);
		return {resp, resp + n};
	}
};
} // namespace

TEST(transfer_configure_sets_state_and_acks)
{
	Fixture f;
	/* idle=2, wait_retry=0x0050, match_retry=0x0010 */
	auto r = f.run({0x04, 0x02, 0x50, 0x00, 0x10, 0x00});
	const uint8_t want[] = {0x04, dap::kDapOk};
	CHECK_BYTES(r, want);
}

TEST(transfer_single_dp_write)
{
	Fixture f;
	/* count=1: write DP addr 4 (0x04 -> A2 set), value 0x11223344 */
	auto r = f.run({0x05, 0x00, 0x01, 0x04, 0x44, 0x33, 0x22, 0x11});
	const uint8_t want[] = {0x05, 0x01, dap::kXferOk};
	CHECK_BYTES(r, want);
	CHECK_EQ(f.drv.transfers.size(), 1u);
	CHECK_EQ(f.drv.transfers[0].request, 0x04);
	CHECK_EQ(f.drv.transfers[0].write_data, 0x11223344u);
}

TEST(transfer_single_dp_read)
{
	Fixture f;
	f.drv.script.push_back({dap::Ack::Ok, 0xCAFEBABE});
	/* count=1: read DP addr 0 (RnW) */
	auto r = f.run({0x05, 0x00, 0x01, 0x02});
	const uint8_t want[] = {0x05, 0x01, dap::kXferOk, 0xBE, 0xBA, 0xFE, 0xCA};
	CHECK_BYTES(r, want);
}

TEST(transfer_ap_read_is_posted_and_flushed_via_rdbuff)
{
	Fixture f;
	/* AP read pipeline: post(read req, data=NULL) then RDBUFF read */
	f.drv.script.push_back({dap::Ack::Ok, 0});          /* post */
	f.drv.script.push_back({dap::Ack::Ok, 0x12345678}); /* RDBUFF */
	auto r = f.run({0x05, 0x00, 0x01, 0x03}); /* AP read addr 0 */
	const uint8_t want[] = {0x05, 0x01, dap::kXferOk, 0x78, 0x56, 0x34, 0x12};
	CHECK_BYTES(r, want);
	CHECK_EQ(f.drv.transfers.size(), 2u);
	CHECK_EQ(f.drv.transfers[0].request, 0x03);
	CHECK(f.drv.transfers[0].data_null); /* posted, no data collected */
	CHECK_EQ(f.drv.transfers[1].request, dap::kReqReadRdbuff);
}

TEST(transfer_back_to_back_ap_reads_pipeline)
{
	Fixture f;
	/* two AP reads: post A; read A while posting B; flush B via RDBUFF */
	f.drv.script.push_back({dap::Ack::Ok, 0});       /* post A */
	f.drv.script.push_back({dap::Ack::Ok, 0xAAAA0001}); /* re-post, data=A */
	f.drv.script.push_back({dap::Ack::Ok, 0xAAAA0002}); /* RDBUFF -> B */
	auto r = f.run({0x05, 0x00, 0x02, 0x03, 0x03});
	const uint8_t want[] = {0x05, 0x02, dap::kXferOk,
				0x01, 0x00, 0xAA, 0xAA,
				0x02, 0x00, 0xAA, 0xAA};
	CHECK_BYTES(r, want);
	CHECK_EQ(f.drv.transfers.size(), 3u);
	CHECK_EQ(f.drv.transfers[1].request, 0x03); /* read prev + post next */
	CHECK_EQ(f.drv.transfers[2].request, dap::kReqReadRdbuff);
}

TEST(transfer_write_after_posted_read_flushes_first)
{
	Fixture f;
	f.drv.script.push_back({dap::Ack::Ok, 0});          /* post AP read */
	f.drv.script.push_back({dap::Ack::Ok, 0x000000AA}); /* RDBUFF flush */
	f.drv.script.push_back({dap::Ack::Ok, 0});          /* DP write */
	auto r = f.run({0x05, 0x00, 0x02, 0x03, 0x04, 0x55, 0x00, 0x00, 0x00});
	const uint8_t want[] = {0x05, 0x02, dap::kXferOk, 0xAA, 0x00, 0x00, 0x00};
	CHECK_BYTES(r, want);
	CHECK_EQ(f.drv.transfers[1].request, dap::kReqReadRdbuff);
	CHECK_EQ(f.drv.transfers[2].request, 0x04);
}

TEST(transfer_wait_retries_then_succeeds)
{
	Fixture f;
	f.run({0x04, 0x00, 0x03, 0x00, 0x00, 0x00}); /* retry_count = 3 */
	f.drv.script.push_back({dap::Ack::Wait, 0});
	f.drv.script.push_back({dap::Ack::Wait, 0});
	f.drv.script.push_back({dap::Ack::Ok, 0x0000BEEF});
	auto r = f.run({0x05, 0x00, 0x01, 0x02});
	const uint8_t want[] = {0x05, 0x01, dap::kXferOk, 0xEF, 0xBE, 0x00, 0x00};
	CHECK_BYTES(r, want);
	CHECK_EQ(f.drv.transfers.size(), 3u);
}

TEST(transfer_wait_exhausts_retries)
{
	Fixture f;
	f.run({0x04, 0x00, 0x02, 0x00, 0x00, 0x00}); /* retry_count = 2 */
	for (int i = 0; i < 8; ++i) {
		f.drv.script.push_back({dap::Ack::Wait, 0});
	}
	auto r = f.run({0x05, 0x00, 0x01, 0x02});
	const uint8_t want[] = {0x05, 0x00, dap::kXferWait};
	CHECK_BYTES(r, want);
	/* 1 initial + 2 retries */
	CHECK_EQ(f.drv.transfers.size(), 3u);
}

TEST(transfer_fault_stops_batch)
{
	Fixture f;
	f.drv.script.push_back({dap::Ack::Ok, 0});    /* write 1 ok */
	f.drv.script.push_back({dap::Ack::Fault, 0}); /* write 2 faults */
	auto r = f.run({0x05, 0x00, 0x03,
			0x04, 0x01, 0x00, 0x00, 0x00,
			0x04, 0x02, 0x00, 0x00, 0x00,
			0x04, 0x03, 0x00, 0x00, 0x00});
	const uint8_t want[] = {0x05, 0x01, dap::kXferFault};
	CHECK_BYTES(r, want);
	CHECK_EQ(f.drv.transfers.size(), 2u); /* third never issued */
}

TEST(transfer_match_value_polls_until_match)
{
	Fixture f;
	f.run({0x04, 0x00, 0x64, 0x00, 0x05, 0x00}); /* match_retry = 5 */
	/* set match mask = 0xFF via write-match-mask request (bit5) */
	auto r = f.run({0x05, 0x00, 0x01, 0x20, 0xFF, 0x00, 0x00, 0x00});
	const uint8_t want_mask[] = {0x05, 0x01, dap::kXferOk};
	CHECK_BYTES(r, want_mask);
	CHECK(f.drv.transfers.empty()); /* mask write is engine-internal */

	/* DP read with value-match (bit4): expect 0x42 */
	f.drv.script.push_back({dap::Ack::Ok, 0x10}); /* mismatch */
	f.drv.script.push_back({dap::Ack::Ok, 0x42}); /* match */
	r = f.run({0x05, 0x00, 0x01, 0x12, 0x42, 0x00, 0x00, 0x00});
	const uint8_t want[] = {0x05, 0x01, dap::kXferOk};
	CHECK_BYTES(r, want);
	CHECK_EQ(f.drv.transfers.size(), 2u);
}

TEST(transfer_match_value_mismatch_flag)
{
	Fixture f;
	/* match_retry stays default 0; mask write then immediate mismatch */
	f.run({0x05, 0x00, 0x01, 0x20, 0xFF, 0x00, 0x00, 0x00});
	f.drv.script.push_back({dap::Ack::Ok, 0x10});
	auto r = f.run({0x05, 0x00, 0x01, 0x12, 0x42, 0x00, 0x00, 0x00});
	const uint8_t want[] = {0x05, 0x00,
				static_cast<uint8_t>(dap::kXferOk | dap::kXferMismatch)};
	CHECK_BYTES(r, want);
}

TEST(transfer_disconnected_dummy_response)
{
	MockSwdDriver drv;
	MockHostStatus host;
	dap::Config cfg;
	dap::DapEngine engine{drv, host, cfg}; /* never connected */
	uint8_t resp[16];
	const uint8_t req[] = {0x05, 0x00, 0x01, 0x02};
	size_t n = engine.process(req, resp);
	const uint8_t want[] = {0x05, 0x00, 0x00};
	CHECK_BYTES(std::span<const uint8_t>(resp, n), want);
	CHECK(drv.transfers.empty());
}

TEST(transfer_truncated_request_no_crash)
{
	Fixture f;
	/* claims 2 transfers, provides 1 */
	auto r = f.run({0x05, 0x00, 0x02, 0x02});
	CHECK(r.size() >= 3);
	CHECK_EQ(r[0], 0x05);
	/* must not have read past the buffer; count reflects completed ones */
}
```

- [ ] **Step 2: Run — new tests FAIL.**

- [ ] **Step 3: Implement `cmd_transfer_configure` and `cmd_transfer`** in `transfer.cpp`. `cmd_transfer_configure`:

```cpp
DapEngine::Sizes DapEngine::cmd_transfer_configure(std::span<const uint8_t> req,
						   std::span<uint8_t> resp)
{
	if (req.size() < 5) {
		resp[0] = kDapError;
		return {req.size(), 1};
	}
	idle_cycles_ = req[0];
	retry_count_ = static_cast<uint16_t>(req[1] | (req[2] << 8));
	match_retry_ = static_cast<uint16_t>(req[3] | (req[4] << 8));
	resp[0] = kDapOk;
	return {5, 1};
}
```

`cmd_transfer` is a faithful port of `DAP_SWD_Transfer` (`DAP.c:657-940`). Structure (write the full function following the original control flow exactly; key invariants):

- Clear `abort_` on entry; response cursor starts at offset 2 (count and value back-filled at the end).
- Track `post_read` and a request cursor; for each transfer request byte:
  - **Read + posted pending**: if next is another plain AP read → `transfer_retry(request, &data)` reads previous AP data and re-posts; else → `transfer_retry(kReqReadRdbuff, &data)` and clear `post_read`. Break on non-OK. Store the 4 data bytes. (Timestamp storage when bit7 set and still posting — copy `DAP.c:718-729`.)
  - **Read with MatchValue (bit4)**: consume 4 match bytes; if AP, first post with `transfer_retry(request, nullptr)`; then poll `transfer_retry(request, &data)` until `(data & match_mask_) == match_value` or `match_retry_` exhausted/abort; on residual mismatch OR `kXferMismatch` into response value and break.
  - **Plain read**: AP → post once (`transfer_retry(request, nullptr)`, set `post_read`, store timestamp if requested); DP → `transfer_retry(request, &data)` + store timestamp if requested + store data.
  - **Write with MatchMask (bit5)**: consume 4 bytes into `match_mask_`, response value OK, no driver call.
  - **Plain write**: if `post_read` pending, flush via RDBUFF first and store its data, clear `post_read`; then consume 4 data bytes and `transfer_retry(request, &data)`; store timestamp if requested.
  - Increment completed count; break if `abort_` set.
- After the loop: skip the remaining canceled request bytes to compute `consumed` (reads +0/+4 for match, writes +4 — `DAP.c:872-885`).
- If response value still OK and `post_read` pending: final RDBUFF flush, store data.
- Back-fill `resp[0] = count`, `resp[1] = response_value`. Return `{consumed, written}` where written = 2 + data bytes.
- Disconnected (`!connected_`): respond `{0x00, 0x00}` (count 0, value 0), consume the whole request (`DAP_Dummy_Transfer`, `DAP.c:1538+`).
- Bounds rule: before consuming any multi-byte field check remaining request size; before storing any 4-byte data check remaining response capacity; on violation stop the loop (treat like abort) — never read/write out of range.

- [ ] **Step 4: Run tests — all green (including all previous tasks).**

- [ ] **Step 5: Commit**

```bash
git add lib/dap tests/host
git commit -m "feat: dap Transfer with posted reads, match transfers, retries"
```

---

### Task 6: TransferBlock + TransferAbort + abort-during-transfer

**Files:**
- Modify: `lib/dap/src/transfer.cpp`
- Create: `tests/host/test_transfer_block.cpp`

**Layout reminder** (`DAP_SWD_TransferBlock`, `DAP.c:1267-1460`): request `{cmd(0x06), index(1), count(2 LE), request(1), [data...]}`; response `{cmd, count(2 LE), response_value(1), data...}`. Single register, count up to 255 writes / reads per packet in practice (packet-size bound). AP reads use the same post + RDBUFF-flush pipeline; DP reads are direct. The abort flag is cleared at entry and checked per item.

- [ ] **Step 1: Write the failing tests**

```cpp
#include "testing.hpp"
#include "mock_swd_driver.hpp"
#include "dap/constants.hpp"
#include "dap/engine.hpp"

#include <vector>

namespace {
struct Fixture {
	MockSwdDriver drv;
	MockHostStatus host;
	dap::Config cfg;
	dap::DapEngine engine{drv, host, cfg};
	Fixture()
	{
		uint8_t resp[8];
		const uint8_t req[] = {0x02, dap::kPortSwd};
		engine.process(req, resp);
		drv.events.clear();
	}
	std::vector<uint8_t> run(std::vector<uint8_t> req)
	{
		uint8_t resp[512] = {};
		size_t n = engine.process({req.data(), req.size()}, resp);
		return {resp, resp + n};
	}
};
} // namespace

TEST(block_write_two_words)
{
	Fixture f;
	auto r = f.run({0x06, 0x00, 0x02, 0x00, 0x04, /* DP write addr 4 */
			0x11, 0x00, 0x00, 0x00,
			0x22, 0x00, 0x00, 0x00});
	const uint8_t want[] = {0x06, 0x02, 0x00, dap::kXferOk};
	CHECK_BYTES(r, want);
	CHECK_EQ(f.drv.transfers.size(), 2u);
	CHECK_EQ(f.drv.transfers[0].write_data, 0x11u);
	CHECK_EQ(f.drv.transfers[1].write_data, 0x22u);
}

TEST(block_read_dp_two_words)
{
	Fixture f;
	f.drv.script.push_back({dap::Ack::Ok, 0x000000AA});
	f.drv.script.push_back({dap::Ack::Ok, 0x000000BB});
	auto r = f.run({0x06, 0x00, 0x02, 0x00, 0x06}); /* DP read addr 4 */
	const uint8_t want[] = {0x06, 0x02, 0x00, dap::kXferOk,
				0xAA, 0x00, 0x00, 0x00,
				0xBB, 0x00, 0x00, 0x00};
	CHECK_BYTES(r, want);
}

TEST(block_read_ap_uses_pipeline)
{
	Fixture f;
	f.drv.script.push_back({dap::Ack::Ok, 0});          /* post */
	f.drv.script.push_back({dap::Ack::Ok, 0x00000001}); /* word 1 (re-post) */
	f.drv.script.push_back({dap::Ack::Ok, 0x00000002}); /* RDBUFF (last) */
	auto r = f.run({0x06, 0x00, 0x02, 0x00, 0x07}); /* AP read addr 4 */
	const uint8_t want[] = {0x06, 0x02, 0x00, dap::kXferOk,
				0x01, 0x00, 0x00, 0x00,
				0x02, 0x00, 0x00, 0x00};
	CHECK_BYTES(r, want);
	CHECK_EQ(f.drv.transfers.size(), 3u);
	CHECK(f.drv.transfers[0].data_null);
	CHECK_EQ(f.drv.transfers[2].request, dap::kReqReadRdbuff);
}

TEST(block_fault_reports_partial_count)
{
	Fixture f;
	f.drv.script.push_back({dap::Ack::Ok, 0});
	f.drv.script.push_back({dap::Ack::Fault, 0});
	auto r = f.run({0x06, 0x00, 0x03, 0x00, 0x04,
			0x01, 0x00, 0x00, 0x00,
			0x02, 0x00, 0x00, 0x00,
			0x03, 0x00, 0x00, 0x00});
	const uint8_t want[] = {0x06, 0x01, 0x00, dap::kXferFault};
	CHECK_BYTES(r, want);
}

TEST(block_disconnected_dummy)
{
	MockSwdDriver drv;
	MockHostStatus host;
	dap::Config cfg;
	dap::DapEngine engine{drv, host, cfg};
	uint8_t resp[16];
	const uint8_t req[] = {0x06, 0x00, 0x02, 0x00, 0x06};
	size_t n = engine.process(req, resp);
	const uint8_t want[] = {0x06, 0x00, 0x00, 0x00};
	CHECK_BYTES(std::span<const uint8_t>(resp, n), want);
}

TEST(block_truncated_write_data_no_crash)
{
	Fixture f;
	auto r = f.run({0x06, 0x00, 0x02, 0x00, 0x04, 0x01, 0x00});
	CHECK(r.size() >= 4);
	CHECK_EQ(r[0], 0x06);
	CHECK(f.drv.transfers.size() <= 1);
}

TEST(transfer_abort_packet_sets_flag_async)
{
	/* The 0x07 packet is handled by the transport calling request_abort();
	 * the engine never sees it as a command. Simulate: set the flag, then
	 * run a long batch — it must stop after the first item. */
	Fixture f;
	f.engine.request_abort();
	f.drv.script.push_back({dap::Ack::Ok, 0});
	auto r = f.run({0x05, 0x00, 0x03,
			0x04, 0x01, 0x00, 0x00, 0x00,
			0x04, 0x02, 0x00, 0x00, 0x00,
			0x04, 0x03, 0x00, 0x00, 0x00});
	/* abort flag is cleared at entry per the reference (DAP.c:682), so all
	 * three run; now test the flag DURING a batch via a scripted hook. */
	CHECK_EQ(r[1], 3); /* pre-set flag is cleared at entry — parity check */
}

TEST(transfer_abort_mid_batch_stops)
{
	Fixture f;
	/* Abort after the first driver transfer completes: use the mock's
	 * script size as the trigger by aborting from a wrapper. */
	struct AbortingDriver : MockSwdDriver {
		dap::DapEngine *eng = nullptr;
		dap::Ack transfer(uint8_t req, uint32_t *data) override
		{
			dap::Ack a = MockSwdDriver::transfer(req, data);
			if (transfers.size() == 1 && eng) {
				eng->request_abort();
			}
			return a;
		}
	};
	AbortingDriver drv;
	MockHostStatus host;
	dap::Config cfg;
	dap::DapEngine engine{drv, host, cfg};
	drv.eng = &engine;
	uint8_t resp[64];
	const uint8_t conn[] = {0x02, dap::kPortSwd};
	engine.process(conn, resp);
	const uint8_t req[] = {0x05, 0x00, 0x03,
			       0x04, 0x01, 0x00, 0x00, 0x00,
			       0x04, 0x02, 0x00, 0x00, 0x00,
			       0x04, 0x03, 0x00, 0x00, 0x00};
	size_t n = engine.process(req, resp);
	CHECK_EQ(n, 3u);
	CHECK_EQ(resp[1], 1); /* one completed, then aborted */
	CHECK_EQ(drv.transfers.size(), 1u);
}
```

- [ ] **Step 2: Run — new tests FAIL.**

- [ ] **Step 3: Implement `cmd_transfer_block`** as a faithful port of `DAP_SWD_TransferBlock` (`DAP.c:1267-1460`), with the same bounds rules as Task 5. Disconnected → `{count=0(2 bytes), value=0}` consuming the whole request. Also confirm `cmd_transfer` clears `abort_` at entry (`abort_.store(false)`) — Task 6's first abort test depends on it; `cmd_transfer_block` does the same.

- [ ] **Step 4: Run tests — all green.**

- [ ] **Step 5: Commit**

```bash
git add lib/dap tests/host
git commit -m "feat: dap TransferBlock and abort handling"
```

---

### Task 7: Wire the engine into the firmware (null driver) + three-board build

After this task, a flashed board answers `DAP_Info` over USB — the stub stops dropping packets. The real SWD driver arrives in phase 3; here a `NullSwdDriver` (all no-ops, NoAck transfers) stands in.

**Files:**
- Create: `src/dap/null_swd_driver.hpp`
- Create: `src/dap/dap_backend.hpp`
- Create: `src/dap/dap_backend.cpp`
- Modify: `src/usb/dap_class.cpp` (stub handler → engine call)
- Modify: `CMakeLists.txt` (add `add_subdirectory(lib/dap)` and the new source)

- [ ] **Step 1: Write src/dap/null_swd_driver.hpp**

```cpp
#pragma once
/* Placeholder SwdDriver until the PIO driver lands (phase 3).
 * All operations are no-ops; transfers answer NoAck so hosts see a
 * well-formed "target not responding" instead of garbage.
 */
#include "dap/interfaces.hpp"

namespace probe {

class NullSwdDriver final : public dap::SwdDriver {
public:
	void set_clock(uint32_t) override {}
	void connect() override {}
	void disconnect() override {}
	void configure(uint8_t, bool) override {}
	void swj_sequence(std::span<const uint8_t>, size_t) override {}
	void swd_output(std::span<const uint8_t>, size_t) override {}
	void swd_input(std::span<uint8_t> data, size_t) override
	{
		for (auto &b : data) {
			b = 0;
		}
	}
	dap::Ack transfer(uint8_t, uint32_t *) override
	{
		return dap::Ack::NoAck;
	}
	void set_pins(uint8_t, uint8_t, uint32_t) override {}
	uint8_t read_pins() override { return 0; }
	void delay_us(uint32_t us) override;
	uint32_t timestamp() override;
};

} // namespace probe
```

`delay_us`/`timestamp` defined in dap_backend.cpp using `k_busy_wait()` / `k_cycle_get_32()`-derived microseconds (`k_cyc_to_us_floor32(k_cycle_get_32())`).

- [ ] **Step 2: Write src/dap/dap_backend.{hpp,cpp}** — owns the engine singleton and the LED host-status adapter (no LEDs yet in phase 2: a `NullHostStatus` with empty methods; phase 4 replaces it). Exposes:

```cpp
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>

namespace probe::dap_backend {

/* Process one CMSIS-DAP request packet; returns response length.
 * Thread-context only (called from the DAP class request handler).
 */
size_t process(std::span<const uint8_t> request, std::span<uint8_t> response);

/* Transport hook for DAP_TransferAbort (0x07) packets. */
void request_abort();

} // namespace probe::dap_backend
```

Implementation: static `NullSwdDriver`, static `NullHostStatus`, static `dap::Config` populated with `CONFIG_PROBE_PRODUCT_STRING` as product, vendor `"Raspberry Pi"`, serial left nullptr for now (hwinfo wiring lands in phase 4 alongside the real DAP_Info parity work — record this as a known gap in the commit message), `fw_version "2.0.0"`, packet_size 64, packet_count 6; static `dap::DapEngine`. `process()` forwards; `request_abort()` forwards. If `request[0] == 0x07` (TransferAbort) the caller must invoke `request_abort()` instead of `process()` — implement that check inside `process()` for phase 2 simplicity (matching the original's transport, which handles 0x07 out-of-band; the simplification is acceptable until the phase-4 double-buffered transport lands, note it in a comment).

- [ ] **Step 3: Replace the stub in src/usb/dap_class.cpp** — in `dap_request_handler`, replace the `LOG_HEXDUMP_INF` stub block with: copy the OUT buffer, call `probe::dap_backend::process()` into a 64-byte response buffer, allocate an IN buffer from the pool, `usbd_ep_enqueue` it, then re-arm OUT. Bump `UDC_BUF_POOL_DEFINE` count from 2 to 3 (the pool comment from phase 1 anticipated this). Keep all error paths non-escalating (return 0) as established.

- [ ] **Step 4: Wire the build** — top-level CMakeLists.txt: `add_subdirectory(lib/dap)` before `target_sources`, add `src/dap/dap_backend.cpp` to sources, `target_link_libraries(app PRIVATE dap)`.

- [ ] **Step 5: Build all three boards**

Run: `~/Developer/debugprobe-zephyr/scripts/build_all.sh`
Expected: three uf2s.

- [ ] **Step 6: Host tests still green**

Run: `~/Developer/debugprobe-zephyr/scripts/run_host_tests.sh`
Expected: all tests pass.

- [ ] **Step 7: Commit**

```bash
git add -A
git commit -m "feat: wire DapEngine into USB vendor interface (null SWD driver)"
```

---

### Task 8: Spec sync + wrap-up

**Files:**
- Modify: `docs/superpowers/specs/2026-06-11-debugprobe-zephyr-design.md`

- [ ] **Step 1: Amend the spec's Testing section** — replace the sentence about `west twister -p native_sim` with the actual mechanism:

> 1. **Unit tests** (`tests/host`, `scripts/run_host_tests.sh`): plain host CMake binary (no Zephyr — `native_sim` is Linux-only and development is on macOS); mock `SwdDriver` injection; covers every command's encode/decode, WAIT/FAULT retries, match transfers, posted-read pipelines, abort, and truncated-request error paths. CI-runnable on any POSIX host.

Also update the repo-layout diagram's `tests/dap_lib/` line to `tests/host/`.

- [ ] **Step 2: Verify the full phase-2 acceptance checklist**

- `scripts/run_host_tests.sh` green (every command group covered);
- `scripts/build_all.sh` green for all three boards;
- `git log` shows one commit per task.

- [ ] **Step 3: Commit**

```bash
git add docs/
git commit -m "docs: sync spec with host-test approach, phase 2 wrap-up"
```

---

## Known Risks and Handling

- **Transfer-algorithm drift vs the reference**: the tests encode the pipeline behavior (post/flush/re-post order, retry counts, abort) derived from `DAP.c:657-940`; when in doubt during implementation, the original file is authoritative — update tests only with a citation.
- **`std::atomic` in both host and Zephyr builds**: `<atomic>` is available in both (GCC libstdc++ / AppleClang libc++); `std::atomic<bool>` is lock-free on ARMv6-M/ARMv8-M and x86_64. If the rpi_pico (Cortex-M0+) build hits an `__atomic_*` libcall link error, switch to `volatile bool` with a comment (single-writer/single-reader flag, byte-store atomicity is architectural).
- **Mock fidelity**: `MockSwdDriver::transfer` treats bit1 (RnW) as the read/write discriminator — same as the engine; if a test needs protocol-error injection, extend `XferResult` rather than overloading ACK values.
- **Response buffer sizing**: engine never writes beyond the caller's span; the USB layer passes exactly 64 bytes (`kPacketSize`). Tests use 256-byte buffers to verify the engine respects *request-derived* lengths, not buffer-derived ones.

