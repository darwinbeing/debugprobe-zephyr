# debugprobe-zephyr Phase 4 (UART Bridge + LEDs + Serial) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the firmware a full-function debug probe: bridge the USB CDC-ACM to the target's hardware UART, drive the status LEDs from the DAP host-status, and report the flash unique ID as the CMSIS-DAP serial — all using Zephyr's own facilities rather than hand-rolled code.

**Architecture:** The CDC↔UART bridge is Zephyr's `zephyr,uart-bridge` driver (a devicetree node bridging `cdc_acm_uart0` ↔ `uart1`, propagating line coding to the physical UART) — no custom bridge code. A small `LedHostStatus` (Zephyr GPIO LED API) replaces `NullHostStatus` so DAP connect/run drive real LEDs. The CMSIS-DAP `DAP_Info` serial is filled from `hwinfo` to match the USB serial descriptor.

**Tech Stack:** Zephyr 4.4.0 `zephyr,uart-bridge` + CDC-ACM + UART + GPIO LED + hwinfo; C++20 application glue. Reference firmware behaviors: `~/Developer/debugprobe/src/cdc_uart.c` (line coding, LEDs) and `include/board_*_config.h` (UART pins GP4/GP5, LED pins).

---

## Scope: this plan is phase-4 CORE; autobaud is deferred to phase 4b

The original debugprobe's phase-4 feature set is: CDC UART bridge, **PIO autobaud**, status LEDs, and hwinfo serial. This plan delivers the **bridge + LEDs + serial** — the parts that map cleanly onto Zephyr facilities and are hardware-verifiable now.

**Autobaud is intentionally NOT in this plan.** It is a much larger, separable effort: a PIO frequency-counter program (`autobaud.pio`) plus a DMA-fed edge-period histogram and baud-estimation algorithm (`autobaud.c`, RP2040-DMA-specific), engaged only when the host has not set a baud rate. It needs its own PIO program (gen_pio.sh pattern, like phase 3), a Zephyr DMA port, and a UART-target-at-unknown-baud test setup. It gets its own plan (phase 4b) so each phase is a working, testable increment. **UART TX/RX activity LEDs** are also deferred to 4b: the Zephyr `uart-bridge` driver is a closed data path, so tapping it for activity blink needs either an upstream hook or the autobaud-era custom bridge — not worth coupling into this plan. The DAP connect/run + USB-connected LEDs (driven by the engine's HostStatus, which we own) ARE in this plan.

## What Zephyr gives us (the Zephyr way)

The upstream `samples/subsys/dap/boards/rpi_debug_probe.overlay` wires the bridge exactly:

```dts
uart-bridge0 {
    compatible = "zephyr,uart-bridge";
    peers = <&cdc_acm_uart0 &uart1>;
};
```

`drivers/serial/uart_bridge.c` copies bytes both ways via interrupt-driven UART AND propagates CDC line-coding to the hardware UART (`uart_configure(peer_dev, &cfg)` with baudrate/parity, `uart_bridge.c:72`). So the bridge core — including "set the target UART's baud from the host's terminal settings" — is **done by Zephyr**; we only provide devicetree.

## USB descriptor note (no change)

We keep ONE CDC-ACM (the existing `cdc_acm_uart0`) and bridge it to `uart1`. The USB layout stays DAP(if0) + CDC(if1,if2) = 3 interfaces, matching the original debugprobe and the existing `check_descriptors.py`. The console moves OFF `uart1` (it must, since `uart1` becomes the bridge target) to the board's `uart0` (or is left as-is on boards whose console is already `uart0`).

## Board pin/LED facts (verify against the Zephyr board DTS during impl)

| Board | uart1 (target) | console default | LEDs (Zephyr aliases / nodes) |
|---|---|---|---|
| rpi_pico | GP4(TX)/GP5(RX) | uart0 (GP0/GP1) — free | `led0` = onboard GP25 |
| rpi_pico2 | GP4/GP5 | uart0 — free | `led0` onboard |
| rpi_debug_probe | GP4/GP5 (uart1_default already in board DTS) | **uart1** — MUST move to uart0 | `red_led`, `green_led_uart`, `yellow_led_uart`, `green_led_dbug`, `yellow_led_dbug` (gpio-leds in board DTS) |

(Original `board_*_config.h`: UART_TX=GP4, UART_RX=GP5, uart1, 115200; debug-probe LEDs USB=GP2, DAP_CONNECTED=GP15, DAP_RUNNING=GP16, UART_RX=GP7, UART_TX=GP8 — the Zephyr board DTS already models these as named LED nodes.)

**Build/flash/test environment** (same as phases 1-3): `ZEPHYR_TOOLCHAIN_VARIANT=gnuarmemb`, `GNUARMEMB_TOOLCHAIN_PATH=$HOME/Developer/zephyrproject/arm-gnu-toolchain-14.2.rel1-darwin-x86_64-arm-none-eabi`, west via `/usr/local/opt/python@3.14/bin/python3.14 -m west`; `scripts/build_all.sh`, `scripts/run_host_tests.sh`. Flashing needs manual BOOTSEL (RPI-RP2 drive). A board is attached for on-target checks.

**Branch:** create `phase4-uart-bridge` from master; merge at the end (finishing-a-development-branch flow).

---

### Task 1: CDC ↔ uart1 bridge via `zephyr,uart-bridge`

**Files:**
- Modify: `boards/rpi_pico.overlay`
- Modify: `boards/rpi_pico2_rp2350a_m33.overlay`
- Modify: `boards/rpi_debug_probe.overlay`
- Modify: `prj.conf`

- [ ] **Step 1: Enable the bridge subsystem in `prj.conf`**

Append:

```ini
CONFIG_UART_BRIDGE=y
CONFIG_UART_INTERRUPT_DRIVEN=y
```

(`zephyr,uart-bridge` is `drivers/serial/uart_bridge.c`, enabled by `CONFIG_UART_BRIDGE`. Confirm the symbol with `grep -n "config UART_BRIDGE" ~/Developer/zephyrproject/zephyr/drivers/serial/Kconfig*` and adjust if different; record it.)

- [ ] **Step 2: rpi_pico / rpi_pico2 overlays — enable uart1 (GP4/GP5) + bridge node**

Add to BOTH `boards/rpi_pico.overlay` and `boards/rpi_pico2_rp2350a_m33.overlay`. First confirm whether the board DTS already provides `&uart1` + a `uart1_default` pinctrl on GP4/GP5; if not, add the pinctrl. For rpi_pico, the bridge wiring is:

```dts
&pinctrl {
	uart1_default: uart1_default {
		group1 {
			pinmux = <UART1_TX_P4>, <UART1_RX_P5>;
		};
	};
};

&uart1 {
	status = "okay";
	current-speed = <115200>;
	pinctrl-0 = <&uart1_default>;
	pinctrl-names = "default";
};

/ {
	uart_bridge0: uart-bridge0 {
		compatible = "zephyr,uart-bridge";
		peers = <&cdc_acm_uart0 &uart1>;
	};
};
```

Verify the pinmux macro names (`UART1_TX_P4`/`UART1_RX_P5`) against `~/Developer/zephyrproject/zephyr/include/zephyr/dt-bindings/pinctrl/rpi-pico-rp2040-pinctrl.h` (rp2040) and `rpi-pico-rp2350a-pinctrl.h` (pico2); use the exact names (they exist — UART1 funcsel). If the board DTS already defines `uart1_default`, reuse it and only add `status="okay"` + the bridge node. Record what each board needed.

- [ ] **Step 3: rpi_debug_probe overlay — move console off uart1, add bridge**

The rpi_debug_probe board DTS sets `zephyr,console = &uart1` and `zephyr,shell-uart = &uart1` and already enables `&uart1` with `uart1_default`. Since uart1 becomes the bridge target, move the console to `uart0` (GP0/GP1). Add to `boards/rpi_debug_probe.overlay`:

```dts
/ {
	chosen {
		zephyr,console = &uart0;
		zephyr,shell-uart = &uart0;
	};

	uart_bridge0: uart-bridge0 {
		compatible = "zephyr,uart-bridge";
		peers = <&cdc_acm_uart0 &uart1>;
	};
};

&uart0 {
	status = "okay";
	current-speed = <115200>;
};
```

If `uart0` needs a pinctrl on GP0/GP1, add it (check the board DTS; `uart0_default` may already exist). `&uart1` is already `okay` in the board DTS — do not redefine its pinctrl, just add the bridge node referencing it. Record details.

- [ ] **Step 4: Build all three boards**

```bash
~/Developer/debugprobe-zephyr/scripts/build_all.sh
```

Expected: three uf2s. Confirm in a generated DTS that `uart_bridge0` is present and `uart1` is `okay` (e.g. `grep -i uart_bridge ~/Developer/debugprobe-zephyr/build/rpi_pico/zephyr/zephyr.dts`).

- [ ] **Step 5: Host tests unaffected**

```bash
~/Developer/debugprobe-zephyr/scripts/run_host_tests.sh
```

Expected: `OK: 73 tests`.

- [ ] **Step 6: Commit**

```bash
cd ~/Developer/debugprobe-zephyr
git add -A
git commit -m "feat: bridge USB CDC-ACM to target uart1 via zephyr,uart-bridge"
```

- [ ] **Step 7: On-target check (controller does this with the attached board, rpi_pico)**

Flash `build/rpi_pico/zephyr/zephyr.uf2`. Wire GP4(TX)→GP5(RX) loopback on the probe, open the CDC port, send bytes, confirm echo. (Recorded as a controller verification step; the implementer notes it pending.)

---

### Task 2: Status LEDs from DAP HostStatus

**Files:**
- Create: `src/leds.hpp`
- Create: `src/leds.cpp`
- Modify: `src/dap/dap_backend.cpp` (replace NullHostStatus with LedHostStatus)
- Modify: `CMakeLists.txt`
- Modify: `boards/rpi_pico.overlay`, `boards/rpi_pico2_rp2350a_m33.overlay` (LED aliases if needed)

The engine calls `HostStatus::connected(bool)` (DAP_HostStatus type 0) and `running(bool)` (type 1). Drive LEDs from these. Use Zephyr LED aliases so each board maps to its own LEDs; absent LEDs are simply not driven.

- [ ] **Step 1: Write `src/leds.hpp`**

```cpp
#pragma once
/* Drives the probe's status LEDs from the DAP engine's HostStatus.
 * Uses devicetree aliases (dap-connected-led, dap-running-led,
 * usb-connected-led); any alias that is absent on a board is a no-op.
 */
#include "dap/interfaces.hpp"

namespace probe {

class LedHostStatus final : public dap::HostStatus {
public:
	void connected(bool on) override;   /* DAP target connected (HostStatus type 0) */
	void running(bool on) override;     /* DAP target running (HostStatus type 1) */
};

/* Turn the USB-connected indicator on once the USB device is configured. */
void leds_usb_connected(bool on);

} // namespace probe
```

- [ ] **Step 2: Write `src/leds.cpp`**

```cpp
#include "leds.hpp"

#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>

namespace probe {

namespace {

/* A LED is present only if the alias resolves to a gpios property. Each entry
 * is compiled out to an empty spec ({0}) when its alias is absent, and the
 * helpers no-op on a null port. */
#define LED_SPEC(alias) \
	GPIO_DT_SPEC_GET_OR(DT_ALIAS(alias), gpios, {0})

const struct gpio_dt_spec dap_connected = LED_SPEC(dap_connected_led);
const struct gpio_dt_spec dap_running = LED_SPEC(dap_running_led);
const struct gpio_dt_spec usb_connected = LED_SPEC(usb_connected_led);

void led_configure(const struct gpio_dt_spec *led)
{
	if (led->port != nullptr && gpio_is_ready_dt(led)) {
		gpio_pin_configure_dt(led, GPIO_OUTPUT_INACTIVE);
	}
}

void led_set(const struct gpio_dt_spec *led, bool on)
{
	if (led->port != nullptr && gpio_is_ready_dt(led)) {
		gpio_pin_set_dt(led, on ? 1 : 0);
	}
}

struct LedInit {
	LedInit()
	{
		led_configure(&dap_connected);
		led_configure(&dap_running);
		led_configure(&usb_connected);
	}
};
const LedInit led_init;

} // namespace

void LedHostStatus::connected(bool on)
{
	led_set(&dap_connected, on);
}

void LedHostStatus::running(bool on)
{
	led_set(&dap_running, on);
}

void leds_usb_connected(bool on)
{
	led_set(&usb_connected, on);
}

} // namespace probe
```

- [ ] **Step 3: Add LED aliases to the board overlays**

rpi_pico / rpi_pico2 (single onboard LED → use it as the USB-connected indicator; no separate DAP LEDs):

```dts
/ {
	aliases {
		usb-connected-led = &led0;
	};
};
```

(`led0` is the board's onboard LED alias; confirm with `grep led0 ~/Developer/zephyrproject/zephyr/boards/raspberrypi/rpi_pico/rpi_pico.dts`.)

rpi_debug_probe — map to the board's named LED nodes (matching the original: DAP_CONNECTED=GP15, DAP_RUNNING=GP16, USB=GP2 ≈ red_led):

```dts
/ {
	aliases {
		dap-connected-led = &green_led_dbug;
		dap-running-led = &yellow_led_dbug;
		usb-connected-led = &red_led;
	};
};
```

(Verify the node labels `green_led_dbug`/`yellow_led_dbug`/`red_led` exist in `rpi_debug_probe.dts`; the phase-1 recon confirmed them. Adjust if the board uses different labels.)

- [ ] **Step 4: Wire `LedHostStatus` into `src/dap/dap_backend.cpp`**

Replace the `NullHostStatus g_host` with `probe::LedHostStatus g_host` and `#include "leds.hpp"`. Keep the engine constructed with `g_host`. (The `dap::HostStatus` interface is unchanged.) Remove the now-unused NullHostStatus only if it's local to dap_backend; if it's a shared type, leave it.

- [ ] **Step 5: Call `leds_usb_connected(true)` when USB configures**

In `src/usb/usb_device.cpp`, the usbd message callback (or after `usbd_enable`) — light the USB-connected LED when the device reaches configured state. If there is a `usbd_msg_cb`, set it on `USBD_MSG_CONFIGURATION`/`USBD_MSG_RESUME` → `probe::leds_usb_connected(true)`, and off on suspend/reset. If no msg callback is wired yet, the simplest correct placement is `leds_usb_connected(true)` right after a successful `usbd_enable()` (it stays lit while running) — record which you did and why. Add `#include "leds.hpp"`.

- [ ] **Step 6: CMakeLists.txt — add `src/leds.cpp`**

```cmake
  src/leds.cpp
```

(append to the existing `target_sources(app PRIVATE ...)` list)

- [ ] **Step 7: Build all three boards + host tests**

```bash
~/Developer/debugprobe-zephyr/scripts/build_all.sh
~/Developer/debugprobe-zephyr/scripts/run_host_tests.sh
```

Expected: three uf2s; `OK: 73 tests`.

- [ ] **Step 8: Commit**

```bash
git add -A
git commit -m "feat: drive status LEDs from DAP HostStatus + USB connected"
```

- [ ] **Step 9: On-target check (controller)** — flash rpi_pico; the onboard LED lights when USB is configured; (on a debug-probe board the DAP LEDs would follow openocd connect/run). Recorded as pending visual check.

---

### Task 3: CMSIS-DAP serial from hwinfo

**Files:**
- Modify: `src/dap/dap_backend.cpp`

`DAP_Info(SerialNumber)` currently returns empty (`Config.serial = nullptr`). Fill it from the flash unique ID (same source as the USB serial descriptor, which already uses `USBD_DESC_SERIAL_NUMBER_DEFINE` + hwinfo). Then host tools report a stable serial via CMSIS-DAP too, matching the original.

- [ ] **Step 1: Build the serial string from hwinfo in `dap_backend.cpp`**

Add near the top of the file (after includes):

```cpp
#include <zephyr/drivers/hwinfo.h>

namespace {
/* Flash unique ID as uppercase hex, same formatting as the USB serial
 * descriptor (USBD_DESC_SERIAL_NUMBER). Built once at startup. */
char g_serial[2 * 8 + 1];

const char *build_serial()
{
	uint8_t id[8];
	ssize_t n = hwinfo_get_device_id(id, sizeof(id));

	if (n <= 0) {
		return nullptr;
	}
	static const char hex[] = "0123456789ABCDEF";
	size_t pos = 0;
	for (ssize_t i = 0; i < n && pos + 2 < sizeof(g_serial); i++) {
		g_serial[pos++] = hex[id[i] >> 4];
		g_serial[pos++] = hex[id[i] & 0xF];
	}
	g_serial[pos] = '\0';
	return g_serial;
}
} // namespace
```

- [ ] **Step 2: Set `Config.serial` at init**

In the `dap::Config g_config{...}` initialization (or wherever the engine/config is set up at startup), set `serial` from `build_serial()`. Because `g_config` is a static initialized aggregate, the cleanest is to assign it in an init function that runs before `process()` is first called — e.g. in `dap_backend`'s init path (there is a static engine; add a small init or set the field in a function the app calls at startup). Concretely, if `dap_backend.cpp` has no init entrypoint, change `Config g_config` so its `serial` is set by a constructor/init-once: declare `Config g_config = make_config();` where `make_config()` returns a `Config` with `serial = build_serial()`. Keep vendor "Raspberry Pi", product `CONFIG_PROBE_PRODUCT_STRING`, fw_version "2.0.0", packet_size 64, packet_count 6 unchanged. Show the final `make_config()`:

```cpp
namespace {
dap::Config make_config()
{
	dap::Config c{};
	c.vendor = "Raspberry Pi";
	c.product = CONFIG_PROBE_PRODUCT_STRING;
	c.serial = build_serial();
	c.fw_version = "2.0.0";
	c.packet_size = 64;
	c.packet_count = 6;
	c.timestamp_clock = 1000000;
	return c;
}
} // namespace
```

and replace the existing aggregate init with `dap::Config g_config = make_config();`. (Verify `dap::Config`'s exact field names/order against `lib/dap/include/dap/interfaces.hpp` — match them; don't reorder.)

- [ ] **Step 3: Build all three boards + host tests**

```bash
~/Developer/debugprobe-zephyr/scripts/build_all.sh
~/Developer/debugprobe-zephyr/scripts/run_host_tests.sh
```

Expected: three uf2s; `OK: 73 tests` (library untouched).

- [ ] **Step 4: Commit**

```bash
git add -A
git commit -m "feat: report flash unique ID as CMSIS-DAP serial (hwinfo)"
```

- [ ] **Step 5: On-target check (controller)** — flash; `openocd ... -c "init"` logs `CMSIS-DAP: SN = <16 hex>` matching the USB serial descriptor (and `tools/conformance/check_descriptors.py` still passes its serial check).

---

### Task 4: Conformance checker update + spec sync + wrap-up

**Files:**
- Modify: `tools/conformance/check_descriptors.py`
- Modify: `docs/superpowers/specs/2026-06-11-debugprobe-zephyr-design.md`

- [ ] **Step 1: Extend the checker to verify the CDC is bridge-functional is out of scope (USB-only tool); instead assert the DAP serial via DAP_Info**

The pyusb checker reads descriptors only. Add a check that the CDC interface is present (already there) and — optionally — a DAP_Info(SerialNumber) round-trip over the vendor bulk endpoint to confirm the serial is non-empty and matches the USB serial string. Add this function and call it in `main()`:

```python
def check_dap_serial(dev):
    # DAP_Info(0x00) SerialNumber(0x03) over the vendor bulk OUT/IN (if0)
    cfg = dev.get_active_configuration()
    if0 = cfg[(0, 0)]
    out_ep = next(e for e in if0 if not (e.bEndpointAddress & 0x80))
    in_ep = next(e for e in if0 if (e.bEndpointAddress & 0x80))
    dev.write(out_ep.bEndpointAddress, bytes([0x00, 0x03]))
    resp = bytes(dev.read(in_ep.bEndpointAddress, 64, timeout=500))
    # resp = {0x00, len, serial bytes...}
    serial = resp[2:2 + resp[1]].rstrip(b"\x00").decode("ascii", "replace")
    usb_serial = usb.util.get_string(dev, dev.iSerialNumber)
    check("DAP_Info serial matches USB serial", serial == usb_serial,
          f"dap={serial!r} usb={usb_serial!r}")
```

(Guard it so a busy/claimed vendor interface doesn't crash the whole tool — wrap the call in try/except and report a soft skip if the endpoint can't be claimed on macOS.)

- [ ] **Step 2: Sync the spec's UART-bridge / LED component sections**

In `docs/superpowers/specs/2026-06-11-debugprobe-zephyr-design.md`, under the application-layer "③" section, record the as-built reality: the CDC↔UART bridge is Zephyr's `zephyr,uart-bridge` (devicetree, propagates line coding), not a hand-written `UartBridge` C++ class; LEDs are driven by `LedHostStatus` via devicetree aliases; the DAP serial comes from hwinfo. Note that **autobaud and UART-activity LEDs are deferred to phase 4b**. Do not rewrite unrelated sections.

- [ ] **Step 3: Verify the phase-4 acceptance checklist (record results)**

- `scripts/build_all.sh` green (3 boards).
- `scripts/run_host_tests.sh` → `OK: 73 tests`.
- On-target (rpi_pico): CDC↔uart1 loopback echoes; onboard LED lights on USB-configured; openocd DAP_Info serial == USB serial; `check_descriptors.py` passes.

- [ ] **Step 4: Commit**

```bash
git add -A
git commit -m "docs+test: conformance DAP serial check; sync spec for phase 4 core"
```

---

## Known Risks and Handling

- **uart1 pinctrl naming**: the `UART1_TX_P4`/`UART1_RX_P5` pinmux macros must match the RP2 pinctrl headers; if absent, use `RP2XXX_PINMUX(4, RP2_PINCTRL_GPIO_FUNC_UART)` / `(5, ...)` directly. Verify per board and record.
- **rpi_debug_probe console move**: the board defaults console to uart1; failing to move it to uart0 means the bridge and console fight over uart1. Task 1 Step 3 moves it; confirm in the generated DTS that `zephyr,console` resolves to `uart0`.
- **`uart_bridge` Kconfig symbol**: confirm `CONFIG_UART_BRIDGE` (Task 1 Step 1) is the right enable; the driver also needs `CONFIG_UART_INTERRUPT_DRIVEN` and the CDC-ACM line-ctrl (`CONFIG_UART_LINE_CTRL`, already set in phase 1).
- **LED polarity**: board LED nodes encode active level (`GPIO_ACTIVE_HIGH/LOW`); `gpio_pin_set_dt(.., 1)` respects it, so "on" is correct regardless. The Pico onboard LED is active-high GP25.
- **hwinfo serial length**: RP2040/RP2350 `hwinfo_get_device_id` returns 8 bytes → 16 hex chars, matching the USB serial descriptor and `check_descriptors.py`'s 16-uppercase-hex assertion.
- **Autobaud / UART-activity LEDs (deferred)**: tracked for phase 4b; the bridge works at the host-set baud (or the 115200 default) without autobaud.
