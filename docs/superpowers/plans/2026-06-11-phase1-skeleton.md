# debugprobe-zephyr 阶段 1（骨架）实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 建立 west 工作区结构 + C++20 Zephyr 应用，三块板（rpi_debug_probe / rpi_pico / rpi_pico2）编译通过，USB 枚举出与原版 debugprobe 对齐的描述符（VID/PID、接口布局、字符串、BOS + MS OS 2.0）。

**Architecture:** 应用为独立仓库（T2 拓扑，west.yml 锁定 Zephyr v4.4.0），本地构建复用现有 `~/Developer/zephyrproject` 工作区。USB 用 usbd-next 新栈：自定义 vendor bulk 类（接口 0，"CMSIS-DAP v2 Interface"，本阶段为收包即丢的 stub）+ 内置 CDC-ACM 类（接口 1/2），BOS+MS OS 2.0 描述符参照 Zephyr dap sample 改写为原版参数。

**Tech Stack:** Zephyr 4.4.0（本地 `~/Developer/zephyrproject/zephyr`）、usbd-next、C++20（GNU Arm 工具链 14.2，`gnuarmemb` variant）、pyusb（描述符校验）。

**与原版的已知可接受差异**（写入对比工具白名单）：

- 端点地址：原版 DAP 用 EP 0x04/0x85、CDC 用 0x81/0x02/0x83；Zephyr usbd 按注册顺序自动分配（DAP 将是 0x01/0x81）。CMSIS-DAP v2 主机（openocd/pyocd/probe-rs）从描述符动态发现端点，不影响兼容性。
- MS OS 2.0 描述符集：原版含 configuration subset header（总长 0xB2）；本实现按 Zephyr sample 结构（set header + function subset + compatible ID + registry property）。Windows 两种布局都接受，关键内容（WINUSB compatible ID、DeviceInterfaceGUID `{CDB3B5AD-293B-4663-AA36-1AAE46463776}`）一致。
- `bcdDevice` 暂定 0x0231（与原版 v2.31 相同），后续版本管理在阶段 5 决定。

**参考文件**（实施时可对照）：

- 原版描述符：`~/Developer/debugprobe/src/usb_descriptors.c`
- Zephyr vendor 类模板：`~/Developer/zephyrproject/zephyr/subsys/dap/dap_backend_usb.c`
- usbd 初始化模板：`~/Developer/zephyrproject/zephyr/samples/subsys/usb/common/sample_usbd_init.c`
- MS OS 2.0 模板：`~/Developer/zephyrproject/zephyr/samples/subsys/dap/src/msosv2.h`

---

### Task 0: 构建环境冒烟验证

确认现有工作区能为 rpi_pico 构建（之前机器上未发现 Zephyr SDK，预期用 `gnuarmemb` variant + 工作区内的 GNU Arm 工具链）。

**Files:** 无（只验证环境）

- [ ] **Step 1: 用 hello_world 冒烟构建**

```bash
export ZEPHYR_TOOLCHAIN_VARIANT=gnuarmemb
export GNUARMEMB_TOOLCHAIN_PATH=$HOME/Developer/zephyrproject/arm-gnu-toolchain-14.2.rel1-darwin-x86_64-arm-none-eabi
cd ~/Developer/zephyrproject
west build -p always -b rpi_pico -d /tmp/zsmoke zephyr/samples/hello_world
```

Expected: 构建成功，结尾出现 `Generating files from /tmp/zsmoke/zephyr/zephyr.elf` 及 memory region 报表；`ls /tmp/zsmoke/zephyr/zephyr.uf2` 存在。

- [ ] **Step 2: 验证 rpi_pico2 与 rpi_debug_probe 也能构建**

```bash
cd ~/Developer/zephyrproject
west build -p always -b rpi_pico2/rp2350a/m33 -d /tmp/zsmoke2 zephyr/samples/hello_world
west build -p always -b rpi_debug_probe -d /tmp/zsmokedp zephyr/samples/hello_world
rm -rf /tmp/zsmoke /tmp/zsmoke2 /tmp/zsmokedp
```

Expected: 两次构建都成功。

若 Step 1 失败提示找不到工具链：检查 `ls $GNUARMEMB_TOOLCHAIN_PATH/bin/arm-none-eabi-gcc`；若提示缺 Python 依赖则 `pip install -r ~/Developer/zephyrproject/zephyr/scripts/requirements.txt`。后续所有 `west build` 命令都假定这两个环境变量已导出。

---

### Task 1: 仓库基础文件

**Files:**
- Create: `~/Developer/debugprobe-zephyr/.gitignore`
- Create: `~/Developer/debugprobe-zephyr/west.yml`
- Create: `~/Developer/debugprobe-zephyr/README.md`

- [ ] **Step 1: 写 .gitignore**

```gitignore
build/
twister-out*/
__pycache__/
*.pyc
.venv/
.DS_Store
```

- [ ] **Step 2: 写 west.yml（T2 manifest，锁定 Zephyr v4.4.0）**

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

- [ ] **Step 3: 写 README.md**

````markdown
# debugprobe-zephyr

Raspberry Pi Debug Probe 固件的 Zephyr RTOS + C++20 重写版。
设计文档见 `docs/superpowers/specs/`。

## 构建（复用现有 Zephyr 工作区）

```bash
export ZEPHYR_TOOLCHAIN_VARIANT=gnuarmemb
export GNUARMEMB_TOOLCHAIN_PATH=$HOME/Developer/zephyrproject/arm-gnu-toolchain-14.2.rel1-darwin-x86_64-arm-none-eabi
cd ~/Developer/zephyrproject
west build -p always -b rpi_debug_probe -d ~/Developer/debugprobe-zephyr/build/rpi_debug_probe ~/Developer/debugprobe-zephyr
```

板目标：`rpi_debug_probe`、`rpi_pico`、`rpi_pico2/rp2350a/m33`。
一键构建三板：`scripts/build_all.sh`。
产物：`build/<board>/zephyr/zephyr.uf2`，BOOTSEL 模式拖入即可烧录。

## 全新机器初始化（T2 拓扑）

```bash
mkdir debugprobe-ws && cd debugprobe-ws
west init -m <本仓库 URL>
west update
```
````

- [ ] **Step 4: Commit**

```bash
cd ~/Developer/debugprobe-zephyr
git add .gitignore west.yml README.md
git commit -m "chore: 仓库基础文件（west manifest、README）"
```

---

### Task 2: 最小 C++20 应用，三板编译通过

**Files:**
- Create: `~/Developer/debugprobe-zephyr/CMakeLists.txt`
- Create: `~/Developer/debugprobe-zephyr/Kconfig`
- Create: `~/Developer/debugprobe-zephyr/prj.conf`
- Create: `~/Developer/debugprobe-zephyr/src/main.cpp`

- [ ] **Step 1: 写 CMakeLists.txt**

```cmake
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(debugprobe_zephyr LANGUAGES C CXX)

target_sources(app PRIVATE
  src/main.cpp
)
target_include_directories(app PRIVATE src)
```

- [ ] **Step 2: 写 Kconfig（应用选项：每板产品字符串，与原版 board_*_config.h 一致）**

```kconfig
mainmenu "debugprobe-zephyr"

config PROBE_PRODUCT_STRING
	string "USB product string"
	default "Debug Probe (CMSIS-DAP)" if BOARD_RPI_DEBUG_PROBE
	default "Debugprobe on Pico (CMSIS-DAP)" if BOARD_RPI_PICO || BOARD_RPI_PICO2
	default "Debugprobe (CMSIS-DAP)"

source "Kconfig.zephyr"
```

- [ ] **Step 3: 写 prj.conf（C++20，全量 libstdc++ 头以使用 std::span）**

```ini
CONFIG_CPP=y
CONFIG_STD_CPP20=y
CONFIG_GLIBCXX_LIBCPP=y
CONFIG_LOG=y
```

- [ ] **Step 4: 写 src/main.cpp（含 C++20 特性冒烟断言）**

```cpp
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <array>
#include <span>

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

namespace {

/* C++20 冒烟：concepts + std::span 能编译即说明工具链/Kconfig 配置正确 */
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

- [ ] **Step 5: 三板构建验证**

```bash
cd ~/Developer/zephyrproject
APP=~/Developer/debugprobe-zephyr
west build -p always -b rpi_debug_probe       -d $APP/build/rpi_debug_probe $APP
west build -p always -b rpi_pico              -d $APP/build/rpi_pico        $APP
west build -p always -b rpi_pico2/rp2350a/m33 -d $APP/build/rpi_pico2       $APP
```

Expected: 三次都成功，各 build 目录下生成 `zephyr/zephyr.uf2`。

- [ ] **Step 6: Commit**

```bash
cd ~/Developer/debugprobe-zephyr
git add CMakeLists.txt Kconfig prj.conf src/main.cpp
git commit -m "feat: 最小 C++20 Zephyr 应用，三板编译通过"
```

---

### Task 3: USB 设备 + CDC-ACM 枚举（设备级描述符对齐原版）

目标：设备以 VID 0x2E8A / PID 0x000C 枚举，bcdUSB 2.10、bcdDevice 2.31、制造商 "Raspberry Pi"、产品字符串按板、序列号来自 flash 唯一 ID（hwinfo），暂时只有 CDC-ACM 一个功能。

**Files:**
- Create: `~/Developer/debugprobe-zephyr/src/usb/usb_device.hpp`
- Create: `~/Developer/debugprobe-zephyr/src/usb/usb_device.cpp`
- Create: `~/Developer/debugprobe-zephyr/boards/rpi_debug_probe.overlay`
- Create: `~/Developer/debugprobe-zephyr/boards/rpi_pico.overlay`
- Create: `~/Developer/debugprobe-zephyr/boards/rpi_pico2_rp2350a_m33.overlay`
- Modify: `~/Developer/debugprobe-zephyr/prj.conf`
- Modify: `~/Developer/debugprobe-zephyr/CMakeLists.txt`
- Modify: `~/Developer/debugprobe-zephyr/src/main.cpp`

- [ ] **Step 1: 三个 overlay 都加 CDC-ACM 实例（三个文件内容相同）**

```dts
&zephyr_udc0 {
	cdc_acm_uart0: cdc_acm_uart0 {
		compatible = "zephyr,cdc-acm-uart";
	};
};
```

- [ ] **Step 2: prj.conf 追加 USB 配置**

```ini
CONFIG_USB_DEVICE_STACK_NEXT=y
CONFIG_USBD_CDC_ACM_CLASS=y
CONFIG_SERIAL=y
CONFIG_UART_LINE_CTRL=y
CONFIG_HWINFO=y
```

- [ ] **Step 3: 写 src/usb/usb_device.hpp**

```cpp
#pragma once

namespace probe::usb {

/* 构造 USB 设备（描述符、配置、类注册）并使能。成功返回 0。 */
int init();

} // namespace probe::usb
```

- [ ] **Step 4: 写 src/usb/usb_device.cpp**

```cpp
#include "usb/usb_device.hpp"

#include <zephyr/device.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(usbdev, LOG_LEVEL_INF);

/* usbd 宏使用链接器 section 与设计上的 C 链接，放在全局作用域 */
USBD_DEVICE_DEFINE(probe_usbd,
		   DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)),
		   0x2E8A, 0x000C);

USBD_DESC_LANG_DEFINE(probe_lang);
USBD_DESC_MANUFACTURER_DEFINE(probe_mfr, "Raspberry Pi");
USBD_DESC_PRODUCT_DEFINE(probe_product, CONFIG_PROBE_PRODUCT_STRING);
USBD_DESC_SERIAL_NUMBER_DEFINE(probe_sn);

USBD_DESC_CONFIG_DEFINE(probe_fs_cfg_desc, "");
/* bmAttributes=0（bus powered）、bMaxPower=50（=100 mA），同原版配置描述符 */
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

	/* bcdUSB 2.10（BOS 需要）、bcdDevice 2.31，与原版一致 */
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

	/* 注册顺序决定接口编号。Task 4 会在 CDC 之前插入 DAP vendor 类，
	 * 使 DAP 成为接口 0（旧版 Keil MDK 只认接口 0，与原版对齐）。 */
	err = usbd_register_class(&probe_usbd, "cdc_acm_0", USBD_SPEED_FS, 1);
	if (err) {
		return err;
	}

	/* 设备级 class 三元组保持 0/0/0，同原版（接口各自声明类） */
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

- [ ] **Step 5: main.cpp 调用 USB 初始化**

将 `src/main.cpp` 的 `main` 函数替换为：

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

（文件顶部 include 区追加 `#include "usb/usb_device.hpp"`，其余保持不变。）

- [ ] **Step 6: CMakeLists.txt 追加源文件**

```cmake
target_sources(app PRIVATE
  src/main.cpp
  src/usb/usb_device.cpp
)
```

- [ ] **Step 7: 三板构建**

```bash
cd ~/Developer/zephyrproject
APP=~/Developer/debugprobe-zephyr
west build -p always -b rpi_debug_probe       -d $APP/build/rpi_debug_probe $APP
west build -p always -b rpi_pico              -d $APP/build/rpi_pico        $APP
west build -p always -b rpi_pico2/rp2350a/m33 -d $APP/build/rpi_pico2       $APP
```

Expected: 三板构建成功。

- [ ] **Step 8: 真机枚举验证（任选一块板，推荐 Pico）**

BOOTSEL 模式插入，拷贝 `build/rpi_pico/zephyr/zephyr.uf2` 到 RPI-RP2 卷；重新枚举后：

```bash
system_profiler SPUSBDataType | grep -A 12 -i "debugprobe\|debug probe"
```

Expected: 出现 `Product ID: 0x000c`、`Vendor ID: 0x2e8a`、`Manufacturer: Raspberry Pi`、产品名 `Debugprobe on Pico (CMSIS-DAP)`、序列号为 16 位十六进制，且系统出现 `/dev/tty.usbmodem*`（CDC）。

- [ ] **Step 9: Commit**

```bash
cd ~/Developer/debugprobe-zephyr
git add -A
git commit -m "feat: USB 设备骨架 + CDC-ACM，设备描述符对齐原版"
```

---

### Task 4: DAP v2 vendor bulk 接口 stub（接口 0）

自定义 usbd 类：vendor-specific 接口、bulk OUT/IN 各 64 字节、接口字符串 "CMSIS-DAP v2 Interface"。本阶段 request handler 只打印并重新挂出 OUT 缓冲（协议引擎阶段 2 接入）。结构参照 `subsys/dap/dap_backend_usb.c`。

**Files:**
- Create: `~/Developer/debugprobe-zephyr/src/usb/dap_class.hpp`
- Create: `~/Developer/debugprobe-zephyr/src/usb/dap_class.cpp`
- Modify: `~/Developer/debugprobe-zephyr/src/usb/usb_device.cpp`
- Modify: `~/Developer/debugprobe-zephyr/CMakeLists.txt`

- [ ] **Step 1: 写 src/usb/dap_class.hpp**

```cpp
#pragma once

namespace probe::usb {

/* usbd 类实例名，供 usbd_register_class 引用 */
inline constexpr const char *kDapClassName = "dap_v2_0";

} // namespace probe::usb
```

- [ ] **Step 2: 写 src/usb/dap_class.cpp**

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
		/* 阶段 1 stub：只记录，不处理。阶段 2 在此接入 DapEngine。 */
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

	ARG_UNUSED(speed); /* 仅支持 full-speed */
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

注意：`usbd_class_api` 的成员顺序以 `zephyr/include/zephyr/usb/usbd.h` 中的定义为准——C++ 的指定初始化器要求按声明顺序排列，如编译报错请按头文件顺序调整。

- [ ] **Step 3: usb_device.cpp 在 CDC 之前注册 DAP 类**

在 `usbd_register_class(&probe_usbd, "cdc_acm_0", ...)` 之前插入：

```cpp
	err = usbd_register_class(&probe_usbd, probe::usb::kDapClassName,
				  USBD_SPEED_FS, 1);
	if (err) {
		return err;
	}
```

并在文件顶部追加 `#include "usb/dap_class.hpp"`。

- [ ] **Step 4: CMakeLists.txt 追加 src/usb/dap_class.cpp**

```cmake
target_sources(app PRIVATE
  src/main.cpp
  src/usb/usb_device.cpp
  src/usb/dap_class.cpp
)
```

- [ ] **Step 5: 三板构建（命令同 Task 3 Step 7）**

Expected: 三板构建成功。

- [ ] **Step 6: 真机验证接口布局**

烧录后：

```bash
system_profiler SPUSBDataType | grep -B 2 -A 20 "0x000c"
```

Expected: 设备有 3 个接口；用 `ioreg -p IOUSB -l -w0 | grep -i "cmsis"` 或 Task 6 的 pyusb 工具确认接口 0 为 vendor class (0xFF) 且字符串为 "CMSIS-DAP v2 Interface"。

- [ ] **Step 7: Commit**

```bash
cd ~/Developer/debugprobe-zephyr
git add -A
git commit -m "feat: DAP v2 vendor bulk 接口 stub（接口 0）"
```

---

### Task 5: BOS + MS OS 2.0 描述符

参照 sample 的 msosv2.h，参数改为原版值：vendor code 0x01（原版 `TUD_BOS_MS_OS_20_DESCRIPTOR(..., 1)`）、GUID `{CDB3B5AD-293B-4663-AA36-1AAE46463776}`、function subset 指向接口 0。

**Files:**
- Create: `~/Developer/debugprobe-zephyr/src/usb/msosv2.hpp`
- Create: `~/Developer/debugprobe-zephyr/src/usb/msosv2.cpp`
- Modify: `~/Developer/debugprobe-zephyr/src/usb/usb_device.cpp`
- Modify: `~/Developer/debugprobe-zephyr/CMakeLists.txt`

- [ ] **Step 1: 写 src/usb/msosv2.hpp**

```cpp
#pragma once

struct usbd_context;

namespace probe::usb {

/* 向设备上下文挂载 BOS（MS OS 2.0 platform capability + vendor request） */
int register_msosv2(struct usbd_context *uds_ctx);

} // namespace probe::usb
```

- [ ] **Step 2: 写 src/usb/msosv2.cpp**

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

/* 与原版一致：bMS_VendorCode = 0x01 */
constexpr uint8_t kVendorCode = 0x01;
/* Windows 8.1 起（原版用 0x06030000） */
constexpr uint32_t kWindowsVersion = 0x06030000;

/* {CDB3B5AD-293B-4663-AA36-1AAE46463776}，UTF-16LE，同原版 */
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
		.bFirstInterface = 0, /* DAP v2 接口 */
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

注意：匿名 namespace 内的 `msosv2_to_host_cb` 若被宏要求外部链接而报错，把回调和两个描述符结构移出匿名 namespace 即可。

- [ ] **Step 3: usb_device.cpp 挂载 BOS**

在 `usbd_add_configuration` 之后、注册类之前插入：

```cpp
	err = probe::usb::register_msosv2(&probe_usbd);
	if (err) {
		return err;
	}
```

并在文件顶部追加 `#include "usb/msosv2.hpp"`。

- [ ] **Step 4: CMakeLists.txt 追加 src/usb/msosv2.cpp，三板构建（命令同 Task 3 Step 7）**

Expected: 三板构建成功。

- [ ] **Step 5: Commit**

```bash
cd ~/Developer/debugprobe-zephyr
git add -A
git commit -m "feat: BOS + MS OS 2.0 描述符（WinUSB 自动绑定，参数对齐原版）"
```

---

### Task 6: 描述符校验工具 + 真机验证

pyusb 脚本校验已烧录设备的描述符与原版关键字段一致（这是本阶段的"测试"，也是阶段 5 一致性工具的起点）。

**Files:**
- Create: `~/Developer/debugprobe-zephyr/tools/conformance/check_descriptors.py`
- Create: `~/Developer/debugprobe-zephyr/tools/conformance/requirements.txt`

- [ ] **Step 1: 写 requirements.txt**

```
pyusb>=1.2
```

- [ ] **Step 2: 写 check_descriptors.py**

```python
#!/usr/bin/env python3
"""校验 debugprobe-zephyr 的 USB 描述符与原版 debugprobe 对齐。

用法: python3 check_descriptors.py
需要: pip install pyusb; macOS 需 brew install libusb
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
        print(f"未找到设备 {VID:04x}:{PID:04x}", file=sys.stderr)
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
    check("serial 是 16 位大写十六进制",
          len(serial) == 16 and all(c in "0123456789ABCDEF" for c in serial),
          serial)

    cfg = dev.get_active_configuration()
    intfs = list(cfg)
    check("接口数量 == 3", len(intfs) == 3, f"got {len(intfs)}")

    if0 = cfg[(0, 0)]
    check("接口 0 是 vendor class", if0.bInterfaceClass == 0xFF)
    check("接口 0 名为 CMSIS-DAP v2 Interface",
          usb.util.get_string(dev, if0.iInterface)
          == "CMSIS-DAP v2 Interface")
    eps = list(if0)
    check("接口 0 有 2 个 bulk 端点",
          len(eps) == 2 and all(usb.util.endpoint_type(e.bmAttributes)
                                == usb.util.ENDPOINT_TYPE_BULK for e in eps))
    check("接口 1 是 CDC 控制", cfg[(1, 0)].bInterfaceClass == 0x02)
    check("接口 2 是 CDC 数据", cfg[(2, 0)].bInterfaceClass == 0x0A)

    # BOS：GET_DESCRIPTOR(BOS)，检查 MS OS 2.0 platform capability
    bos = dev.ctrl_transfer(0x80, 0x06, (0x0F << 8), 0, 256)
    check("BOS 含 MS OS 2.0 platform UUID",
          MSOS_PLATFORM_UUID in bytes(bos))

    # MS OS 2.0 描述符集：vendor request（bRequest=0x01, wIndex=7）
    msos = bytes(dev.ctrl_transfer(0xC0, 0x01, 0, 7, 512))
    check("MS OS 2.0 含 WINUSB compatible ID", b"WINUSB" in msos)
    guid_utf16 = DAP_GUID.encode("utf-16-le")
    check("MS OS 2.0 含 DAP DeviceInterfaceGUID", guid_utf16 in msos)

    print()
    if failures:
        print(f"FAILED: {len(failures)} 项不一致: {failures}")
        return 1
    print("OK: 描述符与原版对齐")
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 3: 对已烧录的板子运行**

```bash
cd ~/Developer/debugprobe-zephyr/tools/conformance
python3 -m venv .venv && .venv/bin/pip install -q -r requirements.txt
.venv/bin/python check_descriptors.py
```

Expected: 全部 `[PASS]`，结尾 `OK: 描述符与原版对齐`，退出码 0。
（若 libusb 缺失：`brew install libusb`。）

- [ ] **Step 4: Commit**

```bash
cd ~/Developer/debugprobe-zephyr
git add tools/
git commit -m "test: USB 描述符对齐校验工具（pyusb）"
```

---

### Task 7: 三板构建脚本与阶段收尾

**Files:**
- Create: `~/Developer/debugprobe-zephyr/scripts/build_all.sh`

- [ ] **Step 1: 写 scripts/build_all.sh**

```bash
#!/usr/bin/env bash
# 三板全量构建，产物在 build/<board>/zephyr/zephyr.uf2
set -euo pipefail

APP_DIR="$(cd "$(dirname "$0")/.." && pwd)"
WS="${ZEPHYR_WORKSPACE:-$HOME/Developer/zephyrproject}"
export ZEPHYR_TOOLCHAIN_VARIANT="${ZEPHYR_TOOLCHAIN_VARIANT:-gnuarmemb}"
export GNUARMEMB_TOOLCHAIN_PATH="${GNUARMEMB_TOOLCHAIN_PATH:-$WS/arm-gnu-toolchain-14.2.rel1-darwin-x86_64-arm-none-eabi}"

for board in rpi_debug_probe rpi_pico rpi_pico2/rp2350a/m33; do
    build_dir="$APP_DIR/build/$(echo "$board" | cut -d/ -f1)"
    (cd "$WS" && west build -p always -b "$board" -d "$build_dir" "$APP_DIR")
done

echo
ls -l "$APP_DIR"/build/*/zephyr/zephyr.uf2
```

- [ ] **Step 2: 赋权并运行**

```bash
chmod +x ~/Developer/debugprobe-zephyr/scripts/build_all.sh
~/Developer/debugprobe-zephyr/scripts/build_all.sh
```

Expected: 三板构建成功，列出三个 `zephyr.uf2`。

- [ ] **Step 3: 阶段验收清单确认**

- 三板 `build_all.sh` 全绿；
- 至少一块真机：枚举 VID/PID/字符串正确（Task 3 Step 8）；
- `check_descriptors.py` 全 PASS（Task 6 Step 3）；
- `/dev/tty.usbmodem*`（CDC）出现。

- [ ] **Step 4: Commit**

```bash
cd ~/Developer/debugprobe-zephyr
git add scripts/
git commit -m "chore: 三板构建脚本，阶段 1 收尾"
```

---

## 已知风险与处置

- **C++ 中的 Zephyr 宏**：`USBD_DEFINE_CLASS` / `UDC_BUF_POOL_DEFINE` 等宏在 C++ 编译单元使用，指定初始化器必须按结构体声明顺序；编译报错时对照 `usbd.h` 调整成员顺序，必要时把对应定义移到全局作用域/具名变量。
- **`usbd_class_api` 成员顺序**：Task 4 Step 2 的初始化顺序是按常见定义猜测的，以头文件实际顺序为准。
- **rpi_debug_probe 板 console 占用 uart1**：上游板定义 `zephyr,console = &uart1`，与日后 UART 桥冲突；本阶段无影响，阶段 4 在 overlay 中改 chosen。
- **macOS 上 pyusb 读取 CDC 接口受限**：脚本只做控制传输与描述符读取，正常可用；若权限问题用 `sudo` 运行。
