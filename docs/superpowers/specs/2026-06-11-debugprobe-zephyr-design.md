# debugprobe-zephyr 总体设计

日期：2026-06-11
状态：已评审通过（设计阶段）

## 背景与目标

将 Raspberry Pi 官方 [debugprobe](https://github.com/raspberrypi/debugprobe) 固件
（pico-sdk + FreeRTOS + TinyUSB + ARM CMSIS-DAP 参考实现，C 语言）
在 Zephyr RTOS 上用 C++20 完整重写，做到**功能与性能对等的替代固件**。

参考实现位于 `~/Developer/debugprobe`（v2.3.1 附近，master 分支）。

### 需求决议（已与用户确认）

| 决策点 | 选择 |
|---|---|
| SWD 实现 | PIO 加速，性能与原版对等（SWD 时钟可达 ~25 MHz 量级） |
| 目标硬件 | Debug Probe 配件（RP2040）、Pico 1（RP2040）、Pico 2（RP2350）三种一步到位 |
| 功能范围 | 完整功能对等：CMSIS-DAP v2 + CDC UART 桥 + PIO 自动波特率 + LED + USB 看门狗 |
| C++ 程度 | 全部 C++ 重写，**包括 CMSIS-DAP 协议层**（不复用 ARM C 参考实现） |
| 代码位置 | 独立新仓库 `~/Developer/debugprobe-zephyr` |
| 测试策略 | 主机单元测试 + 真机验证 + 新旧固件协议一致性对比 |

### 非目标

- JTAG 支持（原版固件也只编译了 SWD，`DAP_JTAG_DEV_CNT = 0`）
- CMSIS-DAP UART 扩展命令（`DAP_UART_*`，原版未启用；UART 走 CDC）
- USB MSC / DFU 等额外 USB 功能
- 除三种目标硬件外的板卡

## 既有可复用资产（Zephyr 4.4.0，本地工作区 `~/Developer/zephyrproject`）

- **板定义全部存在于上游**：`rpi_debug_probe`（含全部 LED 与 pinctrl）、`rpi_pico`、`rpi_pico2/rp2350a/m33`。无需自定义 board，只需各板 overlay。
- **`swdp` 驱动接口**（`include/zephyr/drivers/swdp.h`，`swdp_api`）：SWD 端口驱动抽象
  （output/input sequence、transfer、set clock、pin 控制）。本项目的 PIO 驱动实现该接口。
- **`zephyr,swdp-gpio` bitbang 驱动**（`drivers/dp/swdp_bitbang.c`）：阶段 3 用作协议库
  真机验证的过渡驱动，最终被 PIO 驱动替换（保留 Kconfig 可选）。
- **usbd-next 新 USB 设备栈**：CDC-ACM 类、BOS/MS OS 2.0 描述符支持；
  `samples/subsys/dap` 演示了 CMSIS-DAP v2 vendor bulk + WinUSB 描述符的完整写法。
- **PIO 基础设施**：`raspberrypi,pico-pio` 设备 + `pio_rpi_pico` misc 驱动 +
  pioasm CMake 集成（参考 `drivers/serial/uart_rpi_pico_pio.c` 的用法）。

## 总体架构（方案 A：分层库架构）

```
debugprobe-zephyr/                      west T2 拓扑（app 仓库即 manifest 仓库）
├── west.yml                            # 锁定 zephyr v4.4.0 + hal_rpi_pico 等模块
├── CMakeLists.txt
├── Kconfig                             # 应用级选项（USB 看门狗开关等）
├── prj.conf                            # CONFIG_CPP=y, CONFIG_STD_CPP20=y, usbd-next
├── lib/dap/                            # ① 纯 C++20 CMSIS-DAP 协议库（零 Zephyr 依赖）
│   ├── include/dap/...                 #    公共头：DapEngine、SwdDriver、Pins 等
│   └── src/...
├── drivers/
│   ├── dp_pio/                         # ② PIO SWD 驱动，compatible: raspberrypi,swdp-pio
│   │   └── probe.pio / probe_oen.pio   #    自原版移植（RP2040 / RP2350 变体）
│   └── freqcount_pio/                  #    PIO 频率计（autobaud 用），自 autobaud.pio 移植
├── dts/bindings/                       # 自定义 DT binding（swdp-pio、freqcount-pio）
├── src/                                # ③ 应用层（全部 C++）
│   ├── main.cpp                        #    初始化与线程拉起
│   ├── usb/                            #    描述符、vendor bulk 类、MS OS 2.0
│   ├── dap_thread.cpp                  #    DAP 请求/响应泵 + SwdDriver 适配器
│   ├── uart_bridge.{hpp,cpp}           #    CDC ↔ 物理 UART 桥
│   ├── autobaud.{hpp,cpp}              #    自动波特率
│   ├── leds.{hpp,cpp}
│   └── usb_watchdog.cpp                #    RP2040 SOF 停滞检测（条件编译）
├── boards/                             # 三板 overlay + .conf
│   ├── rpi_debug_probe.overlay/.conf
│   ├── rpi_pico.overlay/.conf
│   └── rpi_pico2_rp2350a_m33.overlay/.conf
├── tests/dap_lib/                      # ztest（native_sim）协议库单元测试
├── tools/conformance/                  # Python(pyusb) 新旧固件协议一致性对比
└── docs/superpowers/specs/             # 本文档及后续实施计划
```

依赖方向：`src/` → `lib/dap/` + 驱动；`lib/dap/` 不依赖任何下层。
构建：`west build -b rpi_debug_probe|rpi_pico|rpi_pico2/rp2350a/m33` 产出三个 UF2。

## 组件设计

### ① C++ CMSIS-DAP 协议库（`lib/dap/`）

全新实现 CMSIS-DAP v2 命令集（与原版固件实际启用范围一致）：

- 通用：`DAP_Info`、`DAP_HostStatus`、`DAP_Connect`、`DAP_Disconnect`、
  `DAP_Delay`、`DAP_ResetTarget`、`DAP_ExecuteCommands`、`DAP_QueueCommands`
- SWJ：`DAP_SWJ_Pins`、`DAP_SWJ_Clock`、`DAP_SWJ_Sequence`
- SWD：`DAP_SWD_Configure`、`DAP_SWD_Sequence`
- 传输：`DAP_TransferConfigure`、`DAP_Transfer`、`DAP_TransferBlock`、`DAP_TransferAbort`
- JTAG 命令返回 `DAP_ERROR`（同原版行为）

核心抽象（纯虚接口，主机测试用 mock 注入）：

```cpp
namespace dap {

class SwdDriver {                     // SWD 端口硬件抽象
public:
  virtual void set_clock(uint32_t hz) = 0;
  virtual void output_sequence(std::span<const uint8_t> bits, size_t nbits) = 0;
  virtual void input_sequence(std::span<uint8_t> bits, size_t nbits) = 0;
  virtual Ack  transfer(Request req, uint32_t &data) = 0;   // 单次 DAP 传输（含 turnaround/parity）
  virtual void set_pins(uint8_t mask, uint8_t value, uint32_t wait_us) = 0; // SWJ_Pins
  virtual uint8_t read_pins() = 0;
  virtual void connect() = 0;         // 引脚接管/释放（对应原版 PORT_SWD_SETUP / PORT_OFF）
  virtual void disconnect() = 0;
  virtual ~SwdDriver() = default;
};

class HostStatus {                    // LED 等宿主状态指示
public:
  virtual void connected(bool) = 0;
  virtual void running(bool) = 0;
  virtual ~HostStatus() = default;
};

class DapEngine {                     // 协议状态机
public:
  DapEngine(SwdDriver &, HostStatus &, const InfoStrings &);
  // 解析一个完整请求包，写出响应包，返回响应长度
  size_t process(std::span<const uint8_t> req, std::span<uint8_t> resp);
};

} // namespace dap
```

约束：

- 不依赖 Zephyr 头文件、不使用堆分配（固定大小缓冲，包大小上限 = USB bulk 包配置，
  与原版 `DAP_PACKET_SIZE` 对齐）、不使用异常与 RTTI（`-fno-exceptions -fno-rtti`）。
- ACK 重试（WAIT retry）、match mask/value 传输、abort 标志位等行为
  以 ARM CMSIS-DAP 规范 + 原版实测行为为准。
- `DAP_Info` 返回的 Vendor/Product/SerialNumber/FirmwareVersion 由应用层注入。

### ② PIO SWD 驱动（`drivers/dp_pio/`）

- 移植原版 `probe.pio`；RP2350 使用 `probe_oen.pio` 变体（输出使能极性差异）。
- devicetree：`compatible = "raspberrypi,swdp-pio"`，属性含 `pio` 句柄、
  `clk-gpios`/`dio-gpios`（或 pin 序号）、可选 `dir-gpios`（Debug Probe 的电平转换方向控制）。
- 实现 Zephyr `swdp_api`（与生态一致，可被上游 `subsys/dap` 复用）；
  `lib/dap` 的 `SwdDriver` 在应用层由薄 C++ 适配器包装 swdp 设备。
- 时钟分频计算、写/读位序列、DIO 三态翻转时序与原版 `sw_dp_pio.c` 逐条对齐；
  `transfer()` 快速路径在驱动内完成（ACK 读取 + 数据 + parity），避免逐 bit 往返。
- `drivers/freqcount_pio/`：移植 `autobaud.pio` 为 PIO 频率计驱动，
  供 autobaud 模块测目标 TX 引脚跳变周期。

### ③ USB 与应用层（`src/`）

USB（usbd-next）：

- 接口 0：CDC-ACM（UART 桥）；接口 2：DAP v2 vendor 类（bulk out/in），
  接口名含 "CMSIS-DAP"（主机工具按此识别）。顺序与原版描述符一致。
- BOS + MS OS 2.0 描述符（WinUSB 自动绑定），vendor request 处理同原版。
- VID/PID 统一为 `0x2E8A`/`0x000C`（三种硬件相同，仅产品字符串区分，同原版）、
  字符串、序列号（flash 唯一 ID，`hwinfo` API 读取）与原版一致，
  保证 openocd / probe-rs / pyocd 现有配置零修改可用。

应用模块：

- `DapThread`：bulk out 收包 → `DapEngine::process` → bulk in 发回；
  双缓冲流水（原版 `tusb_edpt_handler.c` 的请求/响应队列语义）。
- `UartBridge`：CDC line coding（波特率/数据位/停止位/校验）下发到物理 UART
  （`uart_configure`）、DTR/RTS 透传、硬件流控（Debug Probe 板 CTS/RTS 引脚）、
  双向环形缓冲 + TX/RX LED 活动指示。
- `Autobaud`：仅在主机连接后未显式设置波特率时启用；用频率计驱动测目标 TX
  最短脉宽推算波特率，命中常见波特率表后套用（算法与原版 `autobaud.c` 一致）。
- `UsbWatchdog`（仅 RP2040，条件编译）：周期采样 SOF 计数器，三次不变判定
  USB 控制器卡死，断开重连 USB（usbd disable/enable）。RP2040 errata 对策。
- 线程：`dap`、`bridge`、`autobaud` 三个 Zephyr 线程 + 系统工作队列；
  优先级关系沿用原版（UART 桥 > USB > DAP）。USB 挂起/恢复时挂起/恢复各线程。

### 板级差异（devicetree overlay）

| | rpi_debug_probe | rpi_pico | rpi_pico2 |
|---|---|---|---|
| SWD CLK/DIO | GP12/GP14（带方向控制 GP13/GP15） | GP3/GP2 | GP3/GP2 |
| 目标 UART | uart1 GP4/GP5 + CTS/RTS GP6/GP7 | uart1 GP4/GP5 | uart1 GP4/GP5 |
| LED | 红(运行)/绿×2(UART)/黄×2(DAP) | 单板载 LED | 单板载 LED |
| PIO 程序 | probe.pio | probe.pio | probe_oen.pio |
| USB 看门狗 | 启用 | 启用 | 不需要 |

（引脚以原版 `include/board_*_config.h` 为准，实施时逐项核对。）

## 测试与验收

1. **单元测试**（`tests/dap_lib`，`west twister -p native_sim`）：
   mock `SwdDriver` 注入，覆盖全部命令的编解码、WAIT/FAULT 重试、match 传输、
   越界/截断请求等错误路径。CI 可跑（无硬件依赖）。
2. **协议一致性**（`tools/conformance/`）：Python + pyusb，向旧固件与新固件
   发送同一组 DAP 请求序列（含随机生成的 Transfer 批次），逐字节比对响应；
   `DAP_Info` 的版本/序列号字符串走白名单豁免。
3. **真机验收**（三板各自执行）：
   - openocd + gdb 对真实 ARM 目标：connect → halt → 读写内存/寄存器 →
     flash 烧录 → reset → 断点运行，全流程通过；
   - SWD 时钟设到原版同档位（如 24 MHz）烧录耗时与原版相差 < 10%；
   - UART 桥 115200 与 921600 双向压测无丢字节；autobaud 对常见波特率收敛；
   - LED 行为、USB 挂起/恢复、拔插重连与原版一致。

## 错误处理原则

- 协议层：畸形/越界请求返回 `DAP_ERROR` 响应而非崩溃；所有缓冲访问有界。
- 驱动层：PIO 资源申请失败、devicetree 缺项在 init 阶段报错并使设备 not-ready。
- 应用层：USB 断开/挂起走显式状态机（对应原版 mount/unmount/suspend 回调链）；
  UART 桥缓冲满时按原版策略丢弃并计数（debug log 可见）。
- 禁用异常；错误经由返回值/`expected` 风格传递。

## 分阶段实施

每阶段单独产出实施计划（writing-plans），独立验收：

1. **骨架**：west workspace + 空 C++ app 三板编译通过；USB 枚举且描述符
   与原版对齐（接口布局、VID/PID、MS OS 2.0）。
2. **协议库 + 单元测试**：`lib/dap` 全命令实现；native_sim 测试全绿。
3. **SWD 落地**：先以上游 `zephyr,swdp-gpio` bitbang 驱动真机验证协议库
   （openocd 全流程通），再实现 `dp_pio` PIO 驱动替换之，达到性能对等。
4. **UART 桥 + autobaud + LED + USB 看门狗**。
5. **一致性对比 + 三板真机验收**：conformance 工具跑通，验收清单逐项过。

## 风险与对策

- **usbd-next vendor 类 + CDC 组合的描述符顺序控制**：以上游 dap sample 为底，
  阶段 1 即用 USB 描述符 dump 与原版逐字段比对，提前暴露问题。
- **PIO 驱动在 RP2350 上的差异**（GPIO coupling / OEN 极性）：原版已有
  `probe_oen.pio` 解法，按板卡条件选择 PIO 程序。
- **协议层重写引入行为偏差**：一致性对比工具是主要防线；对规范含糊处
  以原版（ARM 参考实现）实测行为为准。
- **Zephyr CDC-ACM 对非常用波特率/流控的支持限制**：阶段 4 实测，必要时
  绕过 CDC-ACM UART 抽象直接用 usbd CDC 类回调。
