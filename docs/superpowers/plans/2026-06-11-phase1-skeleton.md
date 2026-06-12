# debugprobe-zephyr Phase 1 (Skeleton) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Establish the west workspace structure plus a C++20 Zephyr application that builds for all three boards (rpi_debug_probe / rpi_pico / rpi_pico2) and enumerates over USB with descriptors aligned to the original debugprobe (VID/PID, interface layout, strings, BOS + MS OS 2.0).

**Architecture:** The application is a standalone repository (T2 topology; west.yml pins Zephyr v4.4.0), while local builds reuse the existing `~/Developer/zephyrproject` workspace. USB uses the usbd-next stack: a custom vendor bulk class (interface 0, "CMSIS-DAP v2 Interface", a receive-and-drop stub in this phase) plus the built-in CDC-ACM class (interfaces 1/2). BOS + MS OS 2.0 descriptors are adapted from the Zephyr dap sample with the original firmware's parameters.

**Tech Stack:** Zephyr 4.4.0 (local `~/Developer/zephyrproject/zephyr`), usbd-next, C++20 (GNU Arm toolchain 14.2, `gnuarmemb` variant), pyusb (descriptor verification).

**Known acceptable differences vs. the original** (goes into the conformance tool allowlist):

- Endpoint addresses: the original uses EP 0x04/0x85 for DAP and 0x81/0x02/0x83 for CDC; Zephyr usbd assigns addresses automatically in registration order (DAP will get 0x01/0x81). CMSIS-DAP v2 hosts (openocd/pyocd/probe-rs) discover endpoints from descriptors, so compatibility is unaffected.
- MS OS 2.0 descriptor set: the original includes a configuration subset header (total length 0xB2); this implementation follows the Zephyr sample structure (set header + function subset + compatible ID + registry property). Windows accepts both layouts; the key content (WINUSB compatible ID, DeviceInterfaceGUID `{CDB3B5AD-293B-4663-AA36-1AAE46463776}`) is identical.
- `bcdDevice` is tentatively 0x0231 (same as the original v2.31); version management is decided in phase 5.
- CDC ACM management functional descriptor `bmCapabilities`: the original patches it to 0x06 (adds SendBreak support); Zephyr's CDC-ACM class emits 0x02. Recorded for the phase-5 conformance allowlist; phase 4 decides whether break support is needed for UART-bridge parity.

**Reference files** (consult during implementation):

- Original descriptors: `~/Developer/debugprobe/src/usb_descriptors.c`
- Zephyr vendor class template: `~/Developer/zephyrproject/zephyr/subsys/dap/dap_backend_usb.c`
- usbd init template: `~/Developer/zephyrproject/zephyr/samples/subsys/usb/common/sample_usbd_init.c`
- MS OS 2.0 template: `~/Developer/zephyrproject/zephyr/samples/subsys/dap/src/msosv2.h`

---

### Task 0: Build environment smoke test

Confirm the existing workspace can build for rpi_pico (no Zephyr SDK was found on this machine; expect the `gnuarmemb` variant with the GNU Arm toolchain inside the workspace).

**Files:** none (environment verification only)

- [ ] **Step 1: Smoke-build hello_world**

```bash
export ZEPHYR_TOOLCHAIN_VARIANT=gnuarmemb
export GNUARMEMB_TOOLCHAIN_PATH=$HOME/Developer/zephyrproject/arm-gnu-toolchain-14.2.rel1-darwin-x86_64-arm-none-eabi
cd ~/Developer/zephyrproject
west build -p always -b rpi_pico -d /tmp/zsmoke zephyr/samples/hello_world
```

Expected: build succeeds, ending with `Generating files from /tmp/zsmoke/zephyr/zephyr.elf` and the memory region report; `ls /tmp/zsmoke/zephyr/zephyr.uf2` exists.

- [ ] **Step 2: Verify rpi_pico2 and rpi_debug_probe also build**

```bash
cd ~/Developer/zephyrproject
west build -p always -b rpi_pico2/rp2350a/m33 -d /tmp/zsmoke2 zephyr/samples/hello_world
west build -p always -b rpi_debug_probe -d /tmp/zsmokedp zephyr/samples/hello_world
rm -rf /tmp/zsmoke /tmp/zsmoke2 /tmp/zsmokedp
```

Expected: both builds succeed.

If Step 1 fails with a missing toolchain: check `ls $GNUARMEMB_TOOLCHAIN_PATH/bin/arm-none-eabi-gcc`; if Python dependencies are missing, run `pip install -r ~/Developer/zephyrproject/zephyr/scripts/requirements.txt`. All subsequent `west build` commands assume these two environment variables are exported.

---

### Task 1: Repository base files

**Files:**
- Create: `~/Developer/debugprobe-zephyr/.gitignore`
- Create: `~/Developer/debugprobe-zephyr/west.yml`
- Create: `~/Developer/debugprobe-zephyr/README.md`

- [ ] **Step 1: Write .gitignore**

```gitignore
build/
twister-out*/
__pycache__/
*.pyc
.venv/
.DS_Store
```

- [ ] **Step 2: Write west.yml (T2 manifest pinning Zephyr v4.4.0)**

```yaml
manifest:
  remotes:
    - name: zephyrproject-rtos
      url-base: https://github.com/zephyrproject-rtos

  projects:
    - name: zephyr
      remote: zephyrproject-rtos
      revision: v4.4.0
      import:
        name-allowlist:
          - cmsis_6
          - hal_rpi_pico

  self:
    path: debugprobe-zephyr
```

- [ ] **Step 3: Write README.md**

````markdown
# debugprobe-zephyr

A Zephyr RTOS + C++20 rewrite of the Raspberry Pi Debug Probe firmware.
Design documents live in `docs/superpowers/specs/`.

## Building (reusing an existing Zephyr workspace)

```bash
export ZEPHYR_TOOLCHAIN_VARIANT=gnuarmemb
export GNUARMEMB_TOOLCHAIN_PATH=$HOME/Developer/zephyrproject/arm-gnu-toolchain-14.2.rel1-darwin-x86_64-arm-none-eabi
cd ~/Developer/zephyrproject
west build -p always -b rpi_debug_probe -d ~/Developer/debugprobe-zephyr/build/rpi_debug_probe ~/Developer/debugprobe-zephyr
```

Board targets: `rpi_debug_probe`, `rpi_pico`, `rpi_pico2/rp2350a/m33`.
Build all three at once: `scripts/build_all.sh`.
Artifacts: `build/<board>/zephyr/zephyr.uf2` — drag onto the BOOTSEL drive to flash.

## Fresh machine setup (T2 topology)

```bash
mkdir debugprobe-ws && cd debugprobe-ws
west init -m <this repository URL>
west update
```
````

- [ ] **Step 4: Commit**

```bash
cd ~/Developer/debugprobe-zephyr
git add .gitignore west.yml README.md
git commit -m "chore: repository base files (west manifest, README)"
```

---

### Task 2: Minimal C++20 application building for all three boards

**Files:**
- Create: `~/Developer/debugprobe-zephyr/CMakeLists.txt`
- Create: `~/Developer/debugprobe-zephyr/Kconfig`
- Create: `~/Developer/debugprobe-zephyr/prj.conf`
- Create: `~/Developer/debugprobe-zephyr/src/main.cpp`

- [ ] **Step 1: Write CMakeLists.txt**

```cmake
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(debugprobe_zephyr LANGUAGES C CXX)

target_sources(app PRIVATE
  src/main.cpp
)
target_include_directories(app PRIVATE src)
```

- [ ] **Step 2: Write Kconfig (app option: per-board product string, matching the original board_*_config.h)**

```kconfig
mainmenu "debugprobe-zephyr"

config PROBE_PRODUCT_STRING
	string "USB product string"
	default "Debug Probe (CMSIS-DAP)" if BOARD_RPI_DEBUG_PROBE
	default "Debugprobe on Pico (CMSIS-DAP)" if BOARD_RPI_PICO || BOARD_RPI_PICO2
	default "Debugprobe (CMSIS-DAP)"

source "Kconfig.zephyr"
```

- [ ] **Step 3: Write prj.conf (C++20; full libstdc++ headers for std::span)**

```ini
CONFIG_CPP=y
CONFIG_STD_CPP20=y
# GLIBCXX_LIBCPP (full libstdc++) depends on NEWLIB_LIBC || PICOLIBC;
# the gnuarmemb toolchain only provides newlib, so select it explicitly.
CONFIG_GLIBCXX_LIBCPP=y
CONFIG_NEWLIB_LIBC=y
CONFIG_LOG=y
```

- [ ] **Step 4: Write src/main.cpp (with a C++20 feature smoke assertion)**

```cpp
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <array>
#include <span>

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

namespace {

/* C++20 smoke check: if concepts + std::span compile, the toolchain and
 * Kconfig selections are correct.
 */
template <typename T>
concept ByteBuffer = requires(T t) {
	{ t.size() } -> std::convertible_to<size_t>;
	{ t.data() };
};

static_assert(ByteBuffer<std::array<uint8_t, 4>>);
static_assert(ByteBuffer<std::span<const uint8_t>>);

} // namespace

int main()
{
	LOG_INF("debugprobe-zephyr starting");
	return 0;
}
```

- [ ] **Step 5: Build for all three boards**

```bash
cd ~/Developer/zephyrproject
APP=~/Developer/debugprobe-zephyr
west build -p always -b rpi_debug_probe       -d $APP/build/rpi_debug_probe $APP
west build -p always -b rpi_pico              -d $APP/build/rpi_pico        $APP
west build -p always -b rpi_pico2/rp2350a/m33 -d $APP/build/rpi_pico2       $APP
```

Expected: all three succeed; each build directory contains `zephyr/zephyr.uf2`.

- [ ] **Step 6: Commit**

```bash
cd ~/Developer/debugprobe-zephyr
git add CMakeLists.txt Kconfig prj.conf src/main.cpp
git commit -m "feat: minimal C++20 Zephyr app building for all three boards"
```

---

### Task 3: USB device + CDC-ACM enumeration (device-level descriptors aligned with the original)

Goal: the device enumerates as VID 0x2E8A / PID 0x000C, bcdUSB 2.10, bcdDevice 2.31, manufacturer "Raspberry Pi", per-board product string, serial number from the flash unique ID (hwinfo). Only the CDC-ACM function exists for now.

**Files:**
- Create: `~/Developer/debugprobe-zephyr/src/usb/usb_device.hpp`
- Create: `~/Developer/debugprobe-zephyr/src/usb/usb_device.cpp`
- Create: `~/Developer/debugprobe-zephyr/boards/rpi_debug_probe.overlay`
- Create: `~/Developer/debugprobe-zephyr/boards/rpi_pico.overlay`
- Create: `~/Developer/debugprobe-zephyr/boards/rpi_pico2_rp2350a_m33.overlay`
- Modify: `~/Developer/debugprobe-zephyr/prj.conf`
- Modify: `~/Developer/debugprobe-zephyr/CMakeLists.txt`
- Modify: `~/Developer/debugprobe-zephyr/src/main.cpp`

- [ ] **Step 1: Add a CDC-ACM instance to all three overlays (identical content)**

```dts
&zephyr_udc0 {
	cdc_acm_uart0: cdc_acm_uart0 {
		compatible = "zephyr,cdc-acm-uart";
	};
};
```

- [ ] **Step 2: Append USB configuration to prj.conf**

```ini
CONFIG_USB_DEVICE_STACK_NEXT=y
CONFIG_USBD_CDC_ACM_CLASS=y
CONFIG_SERIAL=y
CONFIG_UART_LINE_CTRL=y
CONFIG_HWINFO=y
```

- [ ] **Step 3: Write src/usb/usb_device.hpp**

```cpp
#pragma once

namespace probe::usb {

/* Build the USB device (descriptors, configuration, class registration)
 * and enable it. Returns 0 on success.
 */
int init();

} // namespace probe::usb
```

- [ ] **Step 4: Write src/usb/usb_device.cpp**

```cpp
#include "usb/usb_device.hpp"

#include <zephyr/device.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(usbdev, LOG_LEVEL_INF);

/* The usbd macros rely on linker sections and C linkage conventions;
 * keep them at global scope.
 */
USBD_DEVICE_DEFINE(probe_usbd,
		   DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)),
		   0x2E8A, 0x000C);

USBD_DESC_LANG_DEFINE(probe_lang);
USBD_DESC_MANUFACTURER_DEFINE(probe_mfr, "Raspberry Pi");
USBD_DESC_PRODUCT_DEFINE(probe_product, CONFIG_PROBE_PRODUCT_STRING);
USBD_DESC_SERIAL_NUMBER_DEFINE(probe_sn);

USBD_DESC_CONFIG_DEFINE(probe_fs_cfg_desc, "");
/* bmAttributes=0 (bus powered), bMaxPower=50 (=100 mA),
 * same as the original configuration descriptor.
 */
USBD_CONFIGURATION_DEFINE(probe_fs_config, 0, 50, &probe_fs_cfg_desc);

namespace probe::usb {

int init()
{
	int err;

	err = usbd_add_descriptor(&probe_usbd, &probe_lang);
	if (err) {
		return err;
	}
	err = usbd_add_descriptor(&probe_usbd, &probe_mfr);
	if (err) {
		return err;
	}
	err = usbd_add_descriptor(&probe_usbd, &probe_product);
	if (err) {
		return err;
	}
	err = usbd_add_descriptor(&probe_usbd, &probe_sn);
	if (err) {
		return err;
	}

	/* bcdUSB 2.10 (required for BOS) and bcdDevice 2.31, as in the original */
	err = usbd_device_set_bcd_usb(&probe_usbd, USBD_SPEED_FS, 0x0210);
	if (err) {
		return err;
	}
	err = usbd_device_set_bcd_device(&probe_usbd, 0x0231);
	if (err) {
		return err;
	}

	err = usbd_add_configuration(&probe_usbd, USBD_SPEED_FS, &probe_fs_config);
	if (err) {
		return err;
	}

	/* Registration order determines interface numbering. Task 4 inserts the
	 * DAP vendor class before CDC so DAP becomes interface 0 (old Keil MDK
	 * only looks at interface 0, matching the original).
	 */
	err = usbd_register_class(&probe_usbd, "cdc_acm_0", USBD_SPEED_FS, 1);
	if (err) {
		return err;
	}

	/* Keep the device-level class triple 0/0/0 like the original
	 * (each interface declares its own class).
	 */
	err = usbd_device_set_code_triple(&probe_usbd, USBD_SPEED_FS, 0, 0, 0);
	if (err) {
		return err;
	}

	err = usbd_init(&probe_usbd);
	if (err) {
		return err;
	}

	err = usbd_enable(&probe_usbd);
	if (err) {
		return err;
	}

	LOG_INF("USB device enabled");
	return 0;
}

} // namespace probe::usb
```

- [ ] **Step 5: Call USB init from main.cpp**

Replace the `main` function in `src/main.cpp` with:

```cpp
#include "usb/usb_device.hpp"

int main()
{
	LOG_INF("debugprobe-zephyr starting");

	if (int err = probe::usb::init(); err != 0) {
		LOG_ERR("USB init failed: %d", err);
		return err;
	}

	return 0;
}
```

(Add `#include "usb/usb_device.hpp"` to the include block at the top; everything else stays.)

- [ ] **Step 6: Append the source file to CMakeLists.txt**

```cmake
target_sources(app PRIVATE
  src/main.cpp
  src/usb/usb_device.cpp
)
```

- [ ] **Step 7: Build for all three boards**

```bash
cd ~/Developer/zephyrproject
APP=~/Developer/debugprobe-zephyr
west build -p always -b rpi_debug_probe       -d $APP/build/rpi_debug_probe $APP
west build -p always -b rpi_pico              -d $APP/build/rpi_pico        $APP
west build -p always -b rpi_pico2/rp2350a/m33 -d $APP/build/rpi_pico2       $APP
```

Expected: all three boards build.

- [ ] **Step 8: On-target enumeration check (any board; Pico recommended)**

Plug in while holding BOOTSEL, copy `build/rpi_pico/zephyr/zephyr.uf2` onto the RPI-RP2 volume; after re-enumeration:

```bash
system_profiler SPUSBDataType | grep -A 12 -i "debugprobe\|debug probe"
```

Expected: shows `Product ID: 0x000c`, `Vendor ID: 0x2e8a`, `Manufacturer: Raspberry Pi`, product name `Debugprobe on Pico (CMSIS-DAP)`, a 16-hex-digit serial number, and a `/dev/tty.usbmodem*` device (CDC) appears.

- [ ] **Step 9: Commit**

```bash
cd ~/Developer/debugprobe-zephyr
git add -A
git commit -m "feat: USB device skeleton + CDC-ACM, device descriptors aligned with original"
```

---

### Task 4: DAP v2 vendor bulk interface stub (interface 0)

A custom usbd class: vendor-specific interface, 64-byte bulk OUT/IN, interface string "CMSIS-DAP v2 Interface". In this phase the request handler only logs and re-arms the OUT buffer (the protocol engine plugs in here in phase 2). Structure follows `subsys/dap/dap_backend_usb.c`.

**Files:**
- Create: `~/Developer/debugprobe-zephyr/src/usb/dap_class.hpp`
- Create: `~/Developer/debugprobe-zephyr/src/usb/dap_class.cpp`
- Modify: `~/Developer/debugprobe-zephyr/src/usb/usb_device.cpp`
- Modify: `~/Developer/debugprobe-zephyr/CMakeLists.txt`

- [ ] **Step 1: Write src/usb/dap_class.hpp**

```cpp
#pragma once

namespace probe::usb {

/* usbd class instance name, referenced by usbd_register_class */
inline constexpr const char *kDapClassName = "dap_v2_0";

} // namespace probe::usb
```

- [ ] **Step 2: Write src/usb/dap_class.cpp**

```cpp
#include "usb/dap_class.hpp"

#include <zephyr/kernel.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/drivers/usb/udc_buf.h>
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
		LOG_HEXDUMP_DBG(buf->data, buf->len, "DAP OUT (stub)");
	}

	usbd_ep_buf_free(ctx, buf);

	if (is_out && err == 0) {
		return dap_arm_out(c_data);
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

static struct usbd_class_api dap_class_api = {
	.request = dap_request_handler,
	.enable = dap_enable,
	.disable = dap_disable,
	.init = dap_init,
	.get_desc = dap_get_desc,
};

USBD_DEFINE_CLASS(dap_v2_0, &dap_class_api, &dap_data, nullptr);
```

Note: the member order of `usbd_class_api` must follow its definition in `zephyr/include/zephyr/usb/usbd.h` — C++ designated initializers require declaration order; if compilation fails, reorder the members to match the header.

- [ ] **Step 3: Register the DAP class before CDC in usb_device.cpp**

Insert before `usbd_register_class(&probe_usbd, "cdc_acm_0", ...)`:

```cpp
	err = usbd_register_class(&probe_usbd, probe::usb::kDapClassName,
				  USBD_SPEED_FS, 1);
	if (err) {
		return err;
	}
```

and add `#include "usb/dap_class.hpp"` at the top of the file.

- [ ] **Step 4: Append src/usb/dap_class.cpp to CMakeLists.txt**

```cmake
target_sources(app PRIVATE
  src/main.cpp
  src/usb/usb_device.cpp
  src/usb/dap_class.cpp
)
```

- [ ] **Step 5: Build for all three boards (same commands as Task 3 Step 7)**

Expected: all three boards build.

- [ ] **Step 6: Verify the interface layout on target**

After flashing:

```bash
system_profiler SPUSBDataType | grep -B 2 -A 20 "0x000c"
```

Expected: the device exposes 3 interfaces; confirm interface 0 is vendor class (0xFF) with string "CMSIS-DAP v2 Interface" using `ioreg -p IOUSB -l -w0 | grep -i "cmsis"` or the pyusb tool from Task 6.

- [ ] **Step 7: Commit**

```bash
cd ~/Developer/debugprobe-zephyr
git add -A
git commit -m "feat: DAP v2 vendor bulk interface stub (interface 0)"
```

---

### Task 5: BOS + MS OS 2.0 descriptors

Adapted from the sample's msosv2.h with the original firmware's parameters: vendor code 0x01 (original `TUD_BOS_MS_OS_20_DESCRIPTOR(..., 1)`), GUID `{CDB3B5AD-293B-4663-AA36-1AAE46463776}`, function subset pointing at interface 0.

**Files:**
- Create: `~/Developer/debugprobe-zephyr/src/usb/msosv2.hpp`
- Create: `~/Developer/debugprobe-zephyr/src/usb/msosv2.cpp`
- Modify: `~/Developer/debugprobe-zephyr/src/usb/usb_device.cpp`
- Modify: `~/Developer/debugprobe-zephyr/CMakeLists.txt`

- [ ] **Step 1: Write src/usb/msosv2.hpp**

```cpp
#pragma once

struct usbd_context;

namespace probe::usb {

/* Attach the BOS (MS OS 2.0 platform capability + vendor request)
 * to the device context.
 */
int register_msosv2(struct usbd_context *uds_ctx);

} // namespace probe::usb
```

- [ ] **Step 2: Write src/usb/msosv2.cpp**

```cpp
#include "usb/msosv2.hpp"

#include <zephyr/kernel.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/usb/bos.h>
#include <zephyr/usb/msos_desc.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(msosv2, LOG_LEVEL_INF);

namespace {

/* Same as the original: bMS_VendorCode = 0x01 */
constexpr uint8_t kVendorCode = 0x01;
/* Windows 8.1 and later (the original uses 0x06030000) */
constexpr uint32_t kWindowsVersion = 0x06030000;

/* {CDB3B5AD-293B-4663-AA36-1AAE46463776}, UTF-16LE, same as the original */
#define CMSIS_DAP_V2_GUID \
	'{', 0x00, 'C', 0x00, 'D', 0x00, 'B', 0x00, '3', 0x00, 'B', 0x00, \
	'5', 0x00, 'A', 0x00, 'D', 0x00, '-', 0x00, '2', 0x00, '9', 0x00, \
	'3', 0x00, 'B', 0x00, '-', 0x00, '4', 0x00, '6', 0x00, '6', 0x00, \
	'3', 0x00, '-', 0x00, 'A', 0x00, 'A', 0x00, '3', 0x00, '6', 0x00, \
	'-', 0x00, '1', 0x00, 'A', 0x00, 'A', 0x00, 'E', 0x00, '4', 0x00, \
	'6', 0x00, '4', 0x00, '6', 0x00, '3', 0x00, '7', 0x00, '7', 0x00, \
	'6', 0x00, '}', 0x00, 0x00, 0x00, 0x00, 0x00

struct Msosv2Descriptor {
	struct msosv2_descriptor_set_header header;
	struct msosv2_function_subset_header subset_header;
	struct msosv2_compatible_id compatible_id;
	struct msosv2_guids_property guids_property;
} __packed;

Msosv2Descriptor msosv2_desc = {
	.header = {
		.wLength = sizeof(struct msosv2_descriptor_set_header),
		.wDescriptorType = MS_OS_20_SET_HEADER_DESCRIPTOR,
		.dwWindowsVersion = sys_cpu_to_le32(kWindowsVersion),
		.wTotalLength = sizeof(Msosv2Descriptor),
	},
	.subset_header = {
		.wLength = sizeof(struct msosv2_function_subset_header),
		.wDescriptorType = MS_OS_20_SUBSET_HEADER_FUNCTION,
		.bFirstInterface = 0, /* DAP v2 interface */
		.wSubsetLength = sizeof(Msosv2Descriptor) -
				 sizeof(struct msosv2_descriptor_set_header),
	},
	.compatible_id = {
		.wLength = sizeof(struct msosv2_compatible_id),
		.wDescriptorType = MS_OS_20_FEATURE_COMPATIBLE_ID,
		.CompatibleID = {'W', 'I', 'N', 'U', 'S', 'B', 0x00, 0x00},
	},
	.guids_property = {
		.wLength = sizeof(struct msosv2_guids_property),
		.wDescriptorType = MS_OS_20_FEATURE_REG_PROPERTY,
		.wPropertyDataType = MS_OS_20_PROPERTY_DATA_REG_MULTI_SZ,
		.wPropertyNameLength = 42,
		.PropertyName = {DEVICE_INTERFACE_GUIDS_PROPERTY_NAME},
		.wPropertyDataLength = 80,
		.bPropertyData = {CMSIS_DAP_V2_GUID},
	},
};

struct BosMsosv2 {
	struct usb_bos_platform_descriptor platform;
	struct usb_bos_capability_msos cap;
} __packed;

BosMsosv2 bos_msosv2_desc = {
	.platform = {
		.bLength = sizeof(BosMsosv2),
		.bDescriptorType = USB_DESC_DEVICE_CAPABILITY,
		.bDevCapabilityType = USB_BOS_CAPABILITY_PLATFORM,
		.bReserved = 0,
		/* MS OS 2.0 platform capability UUID
		 * D8DD60DF-4589-4CC7-9CD2-659D9E648A9F
		 */
		.PlatformCapabilityUUID = {
			0xDF, 0x60, 0xDD, 0xD8,
			0x89, 0x45,
			0xC7, 0x4C,
			0x9C, 0xD2,
			0x65, 0x9D, 0x9E, 0x64, 0x8A, 0x9F,
		},
	},
	.cap = {
		.dwWindowsVersion = sys_cpu_to_le32(kWindowsVersion),
		.wMSOSDescriptorSetTotalLength =
			sys_cpu_to_le16(sizeof(msosv2_desc)),
		.bMS_VendorCode = kVendorCode,
		.bAltEnumCode = 0,
	},
};

int msosv2_to_host_cb(const struct usbd_context *const ctx,
		      const struct usb_setup_packet *const setup,
		      struct net_buf *const buf)
{
	ARG_UNUSED(ctx);

	if (setup->bRequest == kVendorCode &&
	    setup->wIndex == MS_OS_20_DESCRIPTOR_INDEX) {
		net_buf_add_mem(buf, &msosv2_desc,
				MIN(net_buf_tailroom(buf),
				    sizeof(msosv2_desc)));
		return 0;
	}

	return -ENOTSUP;
}

} // namespace

USBD_DESC_BOS_VREQ_DEFINE(bos_vreq_msosv2, sizeof(bos_msosv2_desc),
			  &bos_msosv2_desc, kVendorCode,
			  msosv2_to_host_cb, nullptr);

namespace probe::usb {

int register_msosv2(struct usbd_context *uds_ctx)
{
	return usbd_add_descriptor(uds_ctx, &bos_vreq_msosv2);
}

} // namespace probe::usb
```

Note: if the macro requires external linkage for `msosv2_to_host_cb` and compilation fails, move the callback and the two descriptor structs out of the anonymous namespace.

- [ ] **Step 3: Attach the BOS in usb_device.cpp**

Insert after `usbd_add_configuration` and before class registration:

```cpp
	err = probe::usb::register_msosv2(&probe_usbd);
	if (err) {
		return err;
	}
```

and add `#include "usb/msosv2.hpp"` at the top of the file.

- [ ] **Step 4: Append src/usb/msosv2.cpp to CMakeLists.txt; build all three boards (same commands as Task 3 Step 7)**

Expected: all three boards build.

- [ ] **Step 5: Commit**

```bash
cd ~/Developer/debugprobe-zephyr
git add -A
git commit -m "feat: BOS + MS OS 2.0 descriptors (WinUSB auto-binding, params match original)"
```

---

### Task 6: Descriptor verification tool + on-target check

A pyusb script verifies that a flashed device's descriptors match the original firmware's key fields (this is the "test" of this phase and the seed of the phase 5 conformance tool).

**Files:**
- Create: `~/Developer/debugprobe-zephyr/tools/conformance/check_descriptors.py`
- Create: `~/Developer/debugprobe-zephyr/tools/conformance/requirements.txt`

- [ ] **Step 1: Write requirements.txt**

```
pyusb>=1.2
```

- [ ] **Step 2: Write check_descriptors.py**

```python
#!/usr/bin/env python3
"""Verify debugprobe-zephyr USB descriptors are aligned with the original debugprobe.

Usage: python3 check_descriptors.py
Needs: pip install pyusb; on macOS: brew install libusb
"""
import sys

import usb.core
import usb.util

VID, PID = 0x2E8A, 0x000C
EXPECTED_PRODUCTS = {
    "Debug Probe (CMSIS-DAP)",
    "Debugprobe on Pico (CMSIS-DAP)",
}
MSOS_PLATFORM_UUID = bytes((
    0xDF, 0x60, 0xDD, 0xD8, 0x89, 0x45, 0xC7, 0x4C,
    0x9C, 0xD2, 0x65, 0x9D, 0x9E, 0x64, 0x8A, 0x9F,
))
DAP_GUID = "{CDB3B5AD-293B-4663-AA36-1AAE46463776}"

failures = []


def check(name, cond, detail=""):
    status = "PASS" if cond else "FAIL"
    print(f"[{status}] {name}" + (f" ({detail})" if detail else ""))
    if not cond:
        failures.append(name)


def main():
    dev = usb.core.find(idVendor=VID, idProduct=PID)
    if dev is None:
        print(f"Device {VID:04x}:{PID:04x} not found", file=sys.stderr)
        return 2

    check("bcdUSB == 2.10", dev.bcdUSB == 0x0210, f"got {dev.bcdUSB:#06x}")
    check("bcdDevice == 2.31", dev.bcdDevice == 0x0231,
          f"got {dev.bcdDevice:#06x}")
    check("bDeviceClass == 0", dev.bDeviceClass == 0)
    check("manufacturer", usb.util.get_string(dev, dev.iManufacturer)
          == "Raspberry Pi")
    product = usb.util.get_string(dev, dev.iProduct)
    check("product string", product in EXPECTED_PRODUCTS, product)
    serial = usb.util.get_string(dev, dev.iSerialNumber)
    check("serial is 16 uppercase hex digits",
          len(serial) == 16 and all(c in "0123456789ABCDEF" for c in serial),
          serial)

    cfg = dev.get_active_configuration()
    intfs = list(cfg)
    check("interface count == 3", len(intfs) == 3, f"got {len(intfs)}")

    if0 = cfg[(0, 0)]
    check("interface 0 is vendor class", if0.bInterfaceClass == 0xFF)
    check("interface 0 named CMSIS-DAP v2 Interface",
          usb.util.get_string(dev, if0.iInterface)
          == "CMSIS-DAP v2 Interface")
    eps = list(if0)
    check("interface 0 has 2 bulk endpoints",
          len(eps) == 2 and all(usb.util.endpoint_type(e.bmAttributes)
                                == usb.util.ENDPOINT_TYPE_BULK for e in eps))
    check("interface 1 is CDC control", cfg[(1, 0)].bInterfaceClass == 0x02)
    check("interface 2 is CDC data", cfg[(2, 0)].bInterfaceClass == 0x0A)

    # BOS: GET_DESCRIPTOR(BOS), check for the MS OS 2.0 platform capability
    bos = dev.ctrl_transfer(0x80, 0x06, (0x0F << 8), 0, 256)
    check("BOS contains MS OS 2.0 platform UUID",
          MSOS_PLATFORM_UUID in bytes(bos))

    # MS OS 2.0 descriptor set: vendor request (bRequest=0x01, wIndex=7)
    msos = bytes(dev.ctrl_transfer(0xC0, 0x01, 0, 7, 512))
    check("MS OS 2.0 contains WINUSB compatible ID", b"WINUSB" in msos)
    guid_utf16 = DAP_GUID.encode("utf-16-le")
    check("MS OS 2.0 contains DAP DeviceInterfaceGUID", guid_utf16 in msos)

    print()
    if failures:
        print(f"FAILED: {len(failures)} mismatches: {failures}")
        return 1
    print("OK: descriptors aligned with the original")
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 3: Run against a flashed board**

```bash
cd ~/Developer/debugprobe-zephyr/tools/conformance
python3 -m venv .venv && .venv/bin/pip install -q -r requirements.txt
.venv/bin/python check_descriptors.py
```

Expected: all `[PASS]`, ending with `OK: descriptors aligned with the original`, exit code 0.
(If libusb is missing: `brew install libusb`.)

- [ ] **Step 4: Commit**

```bash
cd ~/Developer/debugprobe-zephyr
git add tools/
git commit -m "test: USB descriptor alignment checker (pyusb)"
```

---

### Task 7: Three-board build script and phase wrap-up

**Files:**
- Create: `~/Developer/debugprobe-zephyr/scripts/build_all.sh`

- [ ] **Step 1: Write scripts/build_all.sh**

```bash
#!/usr/bin/env bash
# Build all three boards; artifacts land in build/<board>/zephyr/zephyr.uf2
set -euo pipefail

APP_DIR="$(cd "$(dirname "$0")/.." && pwd)"
WS="${ZEPHYR_WORKSPACE:-$HOME/Developer/zephyrproject}"
# Zephyr 4.4 needs Python >= 3.12; override WEST if your `west` already
# runs under a new enough interpreter.
WEST="${WEST:-/usr/local/opt/python@3.14/bin/python3.14 -m west}"
export ZEPHYR_TOOLCHAIN_VARIANT="${ZEPHYR_TOOLCHAIN_VARIANT:-gnuarmemb}"
export GNUARMEMB_TOOLCHAIN_PATH="${GNUARMEMB_TOOLCHAIN_PATH:-$WS/arm-gnu-toolchain-14.2.rel1-darwin-x86_64-arm-none-eabi}"

for board in rpi_debug_probe rpi_pico rpi_pico2/rp2350a/m33; do
    build_dir="$APP_DIR/build/$(echo "$board" | cut -d/ -f1)"
    (cd "$WS" && $WEST build -p always -b "$board" -d "$build_dir" "$APP_DIR")
done

echo
ls -l "$APP_DIR"/build/*/zephyr/zephyr.uf2
```

- [ ] **Step 2: Make executable and run**

```bash
chmod +x ~/Developer/debugprobe-zephyr/scripts/build_all.sh
~/Developer/debugprobe-zephyr/scripts/build_all.sh
```

Expected: all three boards build; three `zephyr.uf2` files listed.

- [ ] **Step 3: Confirm the phase acceptance checklist**

- `build_all.sh` green for all three boards;
- at least one physical board enumerates with correct VID/PID/strings (Task 3 Step 8);
- `check_descriptors.py` all PASS (Task 6 Step 3);
- `/dev/tty.usbmodem*` (CDC) appears.

- [ ] **Step 4: Commit**

```bash
cd ~/Developer/debugprobe-zephyr
git add scripts/
git commit -m "chore: three-board build script, phase 1 wrap-up"
```

---

## Known Risks and Handling

- **Zephyr macros in C++**: `USBD_DEFINE_CLASS` / `UDC_BUF_POOL_DEFINE` etc. are used in C++ translation units; designated initializers must follow struct declaration order. On compile errors, reorder members per `usbd.h`, and move definitions to global scope / named variables if needed.
- **`usbd_class_api` member order**: the initializer order in Task 4 Step 2 is a best guess; the header's actual order is authoritative.
- **rpi_debug_probe board console occupies uart1**: the upstream board sets `zephyr,console = &uart1`, which conflicts with the future UART bridge; harmless this phase, fixed via the overlay's `chosen` in phase 4.
- **pyusb on macOS has limited access to CDC interfaces**: the script only does control transfers and descriptor reads, which work fine; run with `sudo` if permissions block it.
