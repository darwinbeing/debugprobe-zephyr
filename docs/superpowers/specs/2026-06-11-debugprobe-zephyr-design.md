# debugprobe-zephyr Overall Design

Date: 2026-06-11
Status: Implemented — all phases 1–5 complete and merged; hardware-verified on
rpi_pico (CMSIS-DAP conformance: 27/31 responses byte-identical to official
v2.3.1, 3 deliberate DAP_Info-string enhancements, 0 unexpected mismatches). See
`docs/PROJECT_STATUS.md` and `docs/CONFORMANCE_RESULTS.md`.

## Background and Goal

Reimplement the official Raspberry Pi [debugprobe](https://github.com/raspberrypi/debugprobe)
firmware (pico-sdk + FreeRTOS + TinyUSB + ARM CMSIS-DAP reference implementation, C)
on Zephyr RTOS in C++20, producing a **drop-in replacement firmware with feature and
performance parity**.

The reference implementation lives at `~/Developer/debugprobe` (around v2.3.1, master branch).

### Requirement Decisions (confirmed with the user)

| Decision | Choice |
|---|---|
| SWD implementation | PIO-accelerated, performance parity with the original (SWD clock up to ~25 MHz) |
| Target hardware | Debug Probe accessory (RP2040), Pico 1 (RP2040), Pico 2 (RP2350) — all three from the start |
| Feature scope | Full parity: CMSIS-DAP v2 + CDC UART bridge + PIO autobaud + LEDs + USB watchdog |
| C++ extent | Full C++ rewrite, **including the CMSIS-DAP protocol layer** (no reuse of the ARM C reference implementation) |
| Code location | New standalone repository `~/Developer/debugprobe-zephyr` |
| Test strategy | Host unit tests + on-target verification + old-vs-new firmware protocol conformance comparison |

### Non-goals

- JTAG support (the original firmware only compiles SWD; `DAP_JTAG_DEV_CNT = 0`)
- CMSIS-DAP UART extension commands (`DAP_UART_*`; not enabled in the original — UART goes over CDC)
- Extra USB functions such as MSC / DFU
- Boards other than the three targets above

## Reusable Upstream Assets (Zephyr 4.4.0, local workspace `~/Developer/zephyrproject`)

- **All board definitions exist upstream**: `rpi_debug_probe` (with all LEDs and pinctrl),
  `rpi_pico`, `rpi_pico2/rp2350a/m33`. No custom board needed — only per-board overlays.
- **`swdp` driver interface** (`include/zephyr/drivers/swdp.h`, `swdp_api`): an SWD port
  driver abstraction (output/input sequence, transfer, clock, pin control). Our PIO driver
  implements this interface.
- **`zephyr,swdp-gpio` bitbang driver** (`drivers/dp/swdp_bitbang.c`): used in phase 3 as a
  transitional driver to validate the protocol library on hardware, later replaced by the
  PIO driver (kept selectable via Kconfig).
- **usbd-next (new USB device stack)**: CDC-ACM class, BOS / MS OS 2.0 descriptor support;
  `samples/subsys/dap` demonstrates a complete CMSIS-DAP v2 vendor-bulk + WinUSB descriptor setup.
- **PIO infrastructure**: `raspberrypi,pico-pio` device + `pio_rpi_pico` misc driver +
  pioasm CMake integration (see `drivers/serial/uart_rpi_pico_pio.c` for usage).

## Overall Architecture (Option A: layered libraries)

```
debugprobe-zephyr/                      west T2 topology (app repo is the manifest repo)
├── west.yml                            # pins zephyr v4.4.0 + hal_rpi_pico and friends
├── CMakeLists.txt
├── Kconfig                             # app-level options (USB watchdog switch, etc.)
├── prj.conf                            # CONFIG_CPP=y, CONFIG_STD_CPP20=y, usbd-next
├── lib/dap/                            # ① pure C++20 CMSIS-DAP protocol library (zero Zephyr deps) + tests/host
│   ├── include/dap/...                 #    public headers: DapEngine, SwdDriver, Pins, ...
│   └── src/...
├── drivers/
│   ├── dp_pio/                         # ② PIO SWD driver, compatible: raspberrypi,swdp-pio
│   │   └── probe.pio / probe_oen.pio   #    ported from the original (RP2040 / RP2350 variants)
│   └── freqcount_pio/                  #    PIO frequency counter (for autobaud), from autobaud.pio
├── dts/bindings/                       # custom DT bindings (swdp-pio, freqcount-pio)
├── src/                                # ③ application layer (all C++)
│   ├── main.cpp                        #    init and thread bring-up
│   ├── usb/                            #    descriptors, vendor bulk class, MS OS 2.0
│   ├── dap_thread.cpp                  #    DAP request/response pump + SwdDriver adapter
│   ├── uart_bridge.{hpp,cpp}           #    CDC <-> physical UART bridge
│   ├── autobaud.{hpp,cpp}              #    automatic baud rate detection
│   ├── leds.{hpp,cpp}
│   └── usb_watchdog.cpp                #    RP2040 SOF-stall detection (conditional compile)
├── boards/                             # per-board overlays + .conf
│   ├── rpi_debug_probe.overlay/.conf
│   ├── rpi_pico.overlay/.conf
│   └── rpi_pico2_rp2350a_m33.overlay/.conf
├── tests/host/                         # host unit tests (plain CMake, no Zephyr)
├── tools/conformance/                  # Python (pyusb) old-vs-new protocol conformance tool
└── docs/superpowers/specs/             # this document and subsequent plans
```

Dependency direction: `src/` → `lib/dap/` + drivers; `lib/dap/` depends on nothing below it.
Build: `west build -b rpi_debug_probe|rpi_pico|rpi_pico2/rp2350a/m33` produces three UF2s.

## Component Design

### ① C++ CMSIS-DAP protocol library (`lib/dap/`)

A from-scratch implementation of the CMSIS-DAP v2 command set (matching what the original
firmware actually enables):

- General: `DAP_Info`, `DAP_HostStatus`, `DAP_Connect`, `DAP_Disconnect`,
  `DAP_Delay`, `DAP_ResetTarget`, `DAP_ExecuteCommands`, `DAP_QueueCommands`
- SWJ: `DAP_SWJ_Pins`, `DAP_SWJ_Clock`, `DAP_SWJ_Sequence`
- SWD: `DAP_SWD_Configure`, `DAP_SWD_Sequence`
- Transfers: `DAP_TransferConfigure`, `DAP_Transfer`, `DAP_TransferBlock`, `DAP_TransferAbort`
- JTAG commands return `DAP_ERROR` (same behavior as the original)

Core abstractions (pure virtual interfaces; mocks injected for host testing):

```cpp
namespace dap {

class SwdDriver {                     // SWD port hardware abstraction
public:
  virtual void set_clock(uint32_t hz) = 0;
  virtual void output_sequence(std::span<const uint8_t> bits, size_t nbits) = 0;
  virtual void input_sequence(std::span<uint8_t> bits, size_t nbits) = 0;
  virtual Ack  transfer(Request req, uint32_t &data) = 0;   // one DAP transfer (turnaround/parity included)
  virtual void set_pins(uint8_t mask, uint8_t value, uint32_t wait_us) = 0; // SWJ_Pins
  virtual uint8_t read_pins() = 0;
  virtual void connect() = 0;         // pin takeover/release (original PORT_SWD_SETUP / PORT_OFF)
  virtual void disconnect() = 0;
  virtual ~SwdDriver() = default;
};

class HostStatus {                    // host status indication (LEDs)
public:
  virtual void connected(bool) = 0;
  virtual void running(bool) = 0;
  virtual ~HostStatus() = default;
};

class DapEngine {                     // protocol state machine
public:
  DapEngine(SwdDriver &, HostStatus &, const InfoStrings &);
  // Parse one complete request packet, write the response packet, return its length
  size_t process(std::span<const uint8_t> req, std::span<uint8_t> resp);
};

} // namespace dap
```

Constraints:

- No Zephyr headers, no heap allocation (fixed-size buffers; packet size cap equals the
  USB bulk packet configuration, aligned with the original `DAP_PACKET_SIZE`), no
  exceptions or RTTI (`-fno-exceptions -fno-rtti`).
- ACK retry (WAIT retry), match mask/value transfers, abort flag behavior follow the
  ARM CMSIS-DAP specification plus observed behavior of the original firmware.
- The Vendor/Product/SerialNumber/FirmwareVersion strings returned by `DAP_Info` are
  injected by the application layer.

### ② PIO SWD driver (`drivers/dp_pio/`)

**As built (phase 3):**

- The driver implements the Zephyr `swdp_api` (`include/zephyr/drivers/swdp.h`),
  making it ecosystem-consistent and reusable by the upstream `subsys/dap` sample.
- A single `dap::SwdDriver` adapter (`src/dap/swdp_driver.cpp`, plus a small C shim
  `src/dap/swdp_shim.c` that isolates the not-C++-clean `swdp.h`) is shared by **both**
  the bitbang (`zephyr,swdp-gpio`) and PIO (`raspberrypi,swdp-pio`) backends. The active
  backend is selected purely by devicetree node (`swd_pio` / `swd_bitbang`) and
  `CONFIG_PROBE_SWD_{NULL,BITBANG,PIO}` (default: PIO).
- PIO opcodes are generated by pioasm from the original `.pio` sources and committed to
  `drivers/dp_pio/probe_pio.h`; regenerate at any time via `scripts/gen_pio.sh`. The
  opcodes are **never hand-edited** — pioasm reproducibility is the bit-for-bit parity
  guarantee for the part that cannot be verified without hardware.
- **Board/PIO program mapping:** all three boards use **`probe.pio`** (1-bit side-set,
  side-set base = SWCLK). The Debug Probe (RP2040, GP12/GP14) differs only by an
  additional separate SWDI input pin (`din-gpios = GP13`); the PIO program is still
  `probe.pio`. `probe_oen.pio` (2-bit side-set with output-enable) is a latent path
  (`oen-gpios`) kept correct for a hypothetical buffered/example board — it is **not
  used by any of the three shipped boards**.
- The out-of-tree driver is wired into the build as a Zephyr module via
  `zephyr/module.yml` (not `add_subdirectory`), so the device objects are collected
  into `ZEPHYR_LIBS` early enough for the linker's whole-archive group.
- Devicetree: `compatible = "raspberrypi,swdp-pio"`, properties include `piodev`
  (phandle to the PIO instance), `clk-gpios`/`dio-gpios`, optional `din-gpios`
  (Debug Probe separate SWDI input), optional `oen-gpios` (latent buffered-board path),
  optional `reset-gpios`, and plain-int `swclk-pin`/`swdio-pin` for PIO base config.
- Clock divider computation, write/read bit sequences, and DIO tri-state turnaround
  timing are aligned line-by-line with the original `sw_dp_pio.c`; the `transfer()`
  fast path (ACK read + data + parity) is done inside the driver to avoid per-bit
  round trips.
- `drivers/freqcount_pio/`: port `autobaud.pio` as a PIO frequency-counter driver used
  by the autobaud module to measure target TX edge periods.

### ③ USB and application layer (`src/`)

USB (usbd-next):

- **DAP v2 interface**: custom vendor class (bulk in/out) with MS OS 2.0 descriptors
  (automatic WinUSB binding) and BOS. Interface name contains "CMSIS-DAP" (host tools
  match on this).
- **CDC-ACM ↔ uart1 bridge** (as built, phase 4 core → replaced in phase 4b): Phase
  4 core used Zephyr's `zephyr,uart-bridge` (`CONFIG_UART_BRIDGE=y`, DT node
  `uart-bridge0`). **Phase 4b replaced it entirely** with a custom C++ `UartBridge`
  (`src/uart_bridge.cpp`, modeled on Zephyr's `drivers/serial/uart_bridge.c`):
  interrupt-driven, per-direction ring buffers, same `uart_configure()` line-coding
  propagation — plus two taps the closed driver could not offer:
  (1) **TX/RX activity LEDs** (debounced ~40 ms; `leds_uart_tx_activity()` /
  `leds_uart_rx_activity()`; debug-probe GP8/GP7; no-op on Picos via
  `GPIO_DT_SPEC_GET_OR`), and (2) **autobaud** triggered when the host sets
  `MAGIC_BAUD` (9728 / `0x2600`) in the CDC line-coding. `zephyr,uart-bridge` (the
  DT node + `CONFIG_UART_BRIDGE`) was removed. Decision recorded in the phase-4b
  plan: the Zephyr driver exposes no per-byte hook and no autobaud integration point.
  **Autobaud** is implemented as a PIO frequency counter (`drivers/dp_pio/autobaud.pio`
  → `drivers/dp_pio/autobaud_pio.h`, assembled by `scripts/gen_pio.sh`) plus a
  host-tested `autobaud::Estimator` (`src/autobaud/estimator.{hpp,cpp}` — a faithful
  port of the original `autobaud.c` histogram / bit-time / validity logic, no heap).
  The firmware glue (`src/autobaud/autobaud.cpp`) runs a dedicated Zephyr thread that
  drains the PIO FIFO (no DMA — a thread drain is used; may drop samples at very high
  baud, acceptable for a statistical estimate), feeds the estimator, and on a confident
  result reconfigures uart1 to the detected baud. On-target end-to-end verification
  requires a UART source driving the uart1 RX pin at a known baud.
- VID/PID fixed at `0x2E8A`/`0x000C` (same for all three boards; only the product string
  differs, as in the original), strings and serial number (flash unique ID via `hwinfo`)
  identical to the original so existing openocd / probe-rs / pyocd configs work unchanged.

Application modules (as built):

- `DapThread`: bulk-out packet in → `DapEngine::process` → bulk-in response out;
  double-buffered pipelining (the request/response queue semantics of the original
  `tusb_edpt_handler.c`).
- **Status LEDs** (`src/leds.cpp`): `probe::LedHostStatus` implements `dap::HostStatus`;
  GPIO pins are resolved via devicetree aliases (`dap-connected-led`, `dap-running-led`,
  `usb-connected-led`). Any alias absent on a board is a no-op (uses
  `GPIO_DT_SPEC_GET_OR`). Replaces `NullHostStatus` in `dap_backend.cpp`. USB-connected
  LED is lit from the `usbd_msg` callback on `USBD_MSG_CONFIGURATION`. TX/RX
  UART-activity LEDs (`leds_uart_tx_activity` / `leds_uart_rx_activity`) were added in
  phase 4b (see bridge section above); aliases `uart-tx-led` (GP8) / `uart-rx-led`
  (GP7) on the debug-probe board, absent on Picos → no-op.
- **CMSIS-DAP serial** (`src/dap/dap_backend.cpp`): `DAP_Info(SerialNumber)` returns the
  flash unique ID formatted as uppercase hex via `hwinfo_get_device_id()` — the same
  source as `USBD_DESC_SERIAL_NUMBER_DEFINE`, so the DAP serial matches the USB serial
  descriptor. Built once at startup by `build_serial()` / `make_config()`.
- `UsbWatchdog` (RP2040 only, conditional compile): samples the SOF counter periodically;
  three identical readings ⇒ USB controller stuck ⇒ disconnect/reconnect USB
  (usbd disable/enable). RP2040 errata workaround.
- Threads: `dap` Zephyr thread + system workqueue + autobaud thread (phase 4b; drains
  the PIO FIFO and feeds the estimator). The bridge itself runs in interrupt context
  (custom `UartBridge` ISR, no separate bridge thread).

### Board differences (devicetree overlays)

| | rpi_debug_probe | rpi_pico | rpi_pico2 |
|---|---|---|---|
| SWD CLK/DIO | GP12/GP14, SWDI input GP13 | GP2/GP3 | GP2/GP3 |
| Target UART | uart1 GP4/GP5 + CTS/RTS GP6/GP7 | uart1 GP4/GP5 | uart1 GP4/GP5 |
| LEDs | red (run) / 2× green (UART) / 2× yellow (DAP) | single onboard LED | single onboard LED |
| PIO program | probe.pio (din-gpios = GP13) | probe.pio | probe.pio |
| USB watchdog | enabled | enabled | not needed |

(Pin assignments authoritative source: the original `include/board_*_config.h`; verify
item-by-item during implementation.)

## Testing and Acceptance

1. **Unit tests** (`tests/host`, `scripts/run_host_tests.sh`): a plain host CMake binary built with the system compiler (no Zephyr — `native_sim` is Linux-only and development is on macOS); a micro test harness with a mock `SwdDriver` injection covers every command's encode/decode, WAIT/FAULT retries, match transfers, posted-read pipelines, the check-write trailing read, TransferBlock, abort, ExecuteCommands batching/framing, and truncated-request / response-capacity error paths. ASan mode via `SANITIZE=1`. CI-runnable on any POSIX host.
2. **Protocol conformance** (`tools/conformance/`): Python + pyusb sends the same DAP
   request sequences (including randomized Transfer batches) to the old and the new
   firmware and compares responses byte-for-byte; `DAP_Info` version/serial strings go
   through an allowlist.
3. **On-target acceptance** (each of the three boards):
   - openocd + gdb against a real ARM target: connect → halt → memory/register
     read/write → flash program → reset → breakpoint run, full pass;
   - with the SWD clock at the same setting as the original (e.g. 24 MHz), flash
     programming time within 10% of the original;
   - UART bridge bidirectional stress at 115200 and 921600 with no byte loss;
   - Autobaud: host sets MAGIC_BAUD (9728) on the CDC; firmware runs the PIO
     frequency-counter thread-drain + `autobaud::Estimator`; uart1 is reconfigured to
     the detected baud. Statistical estimator is host-tested (`OK: 76 tests`).
     On-target end-to-end verification requires a UART source at a known baud driving
     the uart1 RX pin (controller to arrange);
   - LED behavior, USB suspend/resume, unplug/replug match the original.

## Error Handling Principles

- Protocol layer: malformed/out-of-bounds requests produce a `DAP_ERROR` response, never
  a crash; all buffer accesses are bounds-checked.
- Driver layer: PIO resource allocation failures and missing devicetree properties fail
  at init and mark the device not-ready.
- Application layer: USB disconnect/suspend goes through an explicit state machine
  (mirroring the original mount/unmount/suspend callback chain); when bridge buffers are
  full, drop and count per the original policy (visible in debug logs).
- Exceptions disabled; errors propagate via return values / `expected`-style types.

## Phased Implementation

Each phase gets its own implementation plan (writing-plans) and acceptance:

1. **Skeleton**: west workspace + empty C++ app building for all three boards; USB
   enumerates with descriptors aligned to the original (interface layout, VID/PID,
   MS OS 2.0).
2. **Protocol library + unit tests**: full `lib/dap` command set; native_sim tests green.
3. **SWD bring-up**: first validate the protocol library on hardware with the upstream
   `zephyr,swdp-gpio` bitbang driver (openocd full flow passes), then implement the
   `dp_pio` PIO driver to replace it and reach performance parity.
4. **UART bridge + autobaud + LEDs + USB watchdog**.
5. **Conformance comparison + three-board on-target acceptance** (done): the
   `tools/conformance/dap_conformance.py` capture/compare tool sends a fixed
   31-command DAP sequence and byte-compares responses with a documented
   whitelist; our firmware is byte-identical to official v2.3.1 on all 27
   deterministic probe-level commands, differing only on the 3 deliberately
   populated DAP_Info strings (1 command, `swj_pins_read`, is live-pin-state
   dependent). The full three-board × two-backend openocd acceptance procedure is
   documented in `docs/HARDWARE_BRINGUP.md`; remaining hardware-dependent items
   (autobaud detection happy-path, full memory-R/W on a single-drop target) are
   tracked in `docs/PROJECT_STATUS.md`.

## Risks and Mitigations

- **Descriptor ordering control with usbd-next vendor class + CDC combo**: start from the
  upstream dap sample; in phase 1 dump the descriptors and compare field-by-field with
  the original to surface problems early.
- **PIO driver differences on RP2350** (GPIO coupling / OEN polarity): the original
  already solved this with `probe_oen.pio`; select the PIO program per board.
- **Behavioral drift from rewriting the protocol layer**: the conformance tool is the main
  defense; where the spec is ambiguous, the original (ARM reference implementation)
  observed behavior is authoritative.
- **Zephyr CDC-ACM limitations for unusual baud rates / flow control**: measure in
  phase 4; if needed, bypass the CDC-ACM UART abstraction and use the usbd CDC class
  callbacks directly.
