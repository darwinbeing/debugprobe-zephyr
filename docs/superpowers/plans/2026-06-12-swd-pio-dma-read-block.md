# SWD PIO DMA — Phase 1 (Read-Block) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** DMA-stream `DAP_TransferBlock` AP/DP **reads** through the PIO so the CPU leaves the per-word loop, lifting rpi_pico/PIO read throughput past today's 176 KB/s (target ~270–300 KB/s @ 15 MHz).

**Architecture:** Add a `transfer_block` operation to the SWD driver stack. The engine delegates the whole block to it; a pure stream-builder/parser pair drives the existing `probe.pio` via two Zephyr DMA channels (TX = mem→PIO FIFO, RX = PIO FIFO→mem). Optimistic streaming: capture all ACK/data, verify after, fall back to the CPU `sw_transfer` pipeline on any non-OK ACK. `probe.pio` is unchanged.

**Tech Stack:** Zephyr 4.4 USB-DAP firmware, C/C++20, RP2040 PIO, Zephyr DMA (`dma_rpi_pico`), host micro-test harness (`tests/host`).

Spec: `docs/superpowers/specs/2026-06-12-swd-pio-dma-design.md`. Branch: `swd-pio-dma`.

---

## File Structure

- `prj.conf` — enable `CONFIG_DMA`, `CONFIG_DMA_RPI_PICO`.
- `boards/*/*.overlay` — enable `&dma { status = "okay"; }`.
- `lib/dap/include/dap/interfaces.hpp` — add `SwdDriver::transfer_block` (virtual, default CPU pipeline).
- `lib/dap/src/transfer_block_loop.cpp` (new) — the default `transfer_block` (faithful per-word pipeline, host-tested).
- `lib/dap/src/transfer.cpp` — refactor `cmd_transfer_block` read path to delegate to `driver_.transfer_block`.
- `drivers/dp_pio/swdp_block.hpp` / `.cpp` (new) — pure `build_read_stream` + `parse_read_response` (host-tested).
- `drivers/dp_pio/swdp_pio.c` — DMA channel init + `sw_transfer_block` (read DMA fast path + CPU fallback); add to `swdp_api`.
- `src/dap/swdp_shim.{c,h}` — `probe_swdp_transfer_block` wrapper.
- `src/dap/swdp_driver.{cpp,hpp}` — `ZephyrSwdpDriver::transfer_block` override.
- `tests/host/test_swdp_block.cpp` (new), `tests/host/CMakeLists.txt` — builder/parser + default-pipeline tests.

---

## Task 1: Enable Zephyr DMA on target

**Files:**
- Modify: `prj.conf`
- Modify: `boards/rpi_pico.overlay` (and rpi_pico2 / rpi_debug_probe overlays — same one-liner)

- [ ] **Step 1: Enable the DMA driver**

Append to `prj.conf`:
```
# SWD block DMA (drivers/dp_pio): Zephyr DMA controller, not the raw SDK.
CONFIG_DMA=y
CONFIG_DMA_RPI_PICO=y
```

- [ ] **Step 2: Enable the dma node (default-disabled in rp2040.dtsi)**

In each board overlay (e.g. `boards/rpi_pico.overlay`), add:
```dts
&dma { status = "okay"; };
```

- [ ] **Step 3: Build + confirm the dma device is present**

Run: `bash scripts/build_all.sh` (expect clean, all three `zephyr.uf2`).
On target, add a one-line `LOG_INF` in `swdp_pio_init` guarded by a TODO:
`device_is_ready(DEVICE_DT_GET(DT_NODELABEL(dma)))` must be true. Flash, check the log shows ready, then remove the temporary log.
Expected: dma device ready.

- [ ] **Step 4: Commit**
```bash
git add prj.conf boards/*.overlay
git commit -m "build: enable Zephyr DMA (dma_rpi_pico) for SWD block DMA"
```

---

## Task 2: Additive `transfer_block_read` hook in the SWD driver + engine

The DMA read path is **purely additive**. Add an opt-in driver method; the engine
*attempts* it and, if it returns `false` (no DMA / ineligible), runs its existing
faithful per-word loop **unchanged**. No pipeline move, no WAIT-retry relocation —
so all existing read-block tests stay green by construction. The DMA writes its
LE data words straight into the response buffer (no large stack array on the 1 KB
usbd thread).

**Files:**
- Modify: `lib/dap/include/dap/interfaces.hpp`
- Modify: `lib/dap/src/transfer.cpp` (read path of `cmd_transfer_block`)
- Modify: `tests/host/mock_swd_driver.hpp`, `tests/host/test_transfer_block.cpp`

- [ ] **Step 1: Green baseline.** Run `bash scripts/run_host_tests.sh` → `OK: 77 tests`.

- [ ] **Step 2: Add the opt-in interface method (default = not handled)**

In `interfaces.hpp` `class SwdDriver`, after `transfer`:
```cpp
/* Optional DMA fast path for a register READ block. Writes up to
 * `out_cap/4` little-endian data words to `out`, sets `*completed` to the
 * number of OK transfers (stops at the first WAIT/FAULT) and `*ack` to the
 * final ACK. Returns true if it handled the block (out/completed/ack valid);
 * false => caller must use the normal per-word path. Default: not handled. */
virtual bool transfer_block_read(uint8_t request, uint16_t count, uint8_t *out,
                                 size_t out_cap, uint16_t *completed,
                                 uint8_t *ack) {
    (void)request; (void)count; (void)out; (void)out_cap;
    (void)completed; (void)ack;
    return false;
}
```

- [ ] **Step 3: Write the failing engine test** (mock returns a DMA block; engine must frame it)

In `mock_swd_driver.hpp` add overridable hooks: a `bool block_enabled=false`, a `std::vector<uint32_t> block_words`, `uint8_t block_ack=dap::kXferOk`, `uint16_t block_completed` (defaults to `block_words.size()`); override `transfer_block_read` to, when `block_enabled`, write `block_completed` LE words from `block_words` into `out` (bounded by `out_cap/4`), set `*completed`/`*ack`, return true; else return false.
In `test_transfer_block.cpp`:
```cpp
TEST(block_read_uses_dma_path_when_available) {
  Fixture f;
  f.drv.block_enabled = true;
  f.drv.block_words = {0xAABBCCDDu, 0x11223344u};
  f.drv.block_completed = 2; f.drv.block_ack = dap::kXferOk;
  auto r = f.run({0x06, 0x00, 0x02, 0x00, 0x06}); /* DP read addr 4, count 2 */
  const uint8_t want[] = {0x06, 0x02, 0x00, dap::kXferOk,
                          0xDD,0xCC,0xBB,0xAA, 0x44,0x33,0x22,0x11};
  CHECK_BYTES(r, want);
}
```

- [ ] **Step 4: Run it — FAIL** (engine ignores the hook). `bash scripts/run_host_tests.sh`.

- [ ] **Step 5: Wire the hook into the engine read branch**

In `cmd_transfer_block` (transfer.cpp), at the very top of the read branch (before the AP post), insert:
```cpp
{
    uint16_t done = 0; uint8_t blk_ack = 0;
    if (request_count > 0 &&
        driver_.transfer_block_read(static_cast<uint8_t>(request_value),
                                    static_cast<uint16_t>(request_count),
                                    resp.data() + wi, resp.size() - wi,
                                    &done, &blk_ack)) {
        size_t avail = (resp.size() - wi) / 4;
        if (done > avail) { done = static_cast<uint16_t>(avail); }
        wi += static_cast<size_t>(done) * 4;
        response_count = done;
        response_value = blk_ack;
        goto end;
    }
}
```
(The existing per-word loop below is untouched and remains the fallback.)

- [ ] **Step 6: Run tests green.** Expected: `OK: 78 tests` (new test passes; the 77 existing — whose mock returns `false` — are byte-identical to before).

- [ ] **Step 7: Commit**
```bash
git add lib/dap tests/host
git commit -m "feat: additive DMA read-block hook in SwdDriver + engine (CPU path unchanged)"
```

---

## Task 3: Pure read-block stream builder (host TDD)

`build_read_stream` emits the exact TX command words `probe_write_bits`/`probe_read_bits`/`probe_hiz_clocks` would push, for `post + (count) reads` where the last read is RDBUFF. Per read word: `write-cmd(8)+prq`, `read-cmd(turnaround+3)`, `read-cmd(32)`, `read-cmd(1)`, `turnaround-cmd+0`.

**Files:**
- Create: `drivers/dp_pio/swdp_block.hpp`, `drivers/dp_pio/swdp_block.cpp`
- Create: `tests/host/test_swdp_block.cpp`; Modify: `tests/host/CMakeLists.txt`

- [ ] **Step 1: Declare the pure API (hardware-independent params)**

`swdp_block.hpp`:
```cpp
#pragma once
#include <cstdint>
#include <cstddef>
namespace probe::swdp_block {
struct StreamCfg { uint16_t cmd_write; uint16_t cmd_read; uint16_t cmd_turn;
                   uint8_t turnaround; }; /* cmd_* = offset+probe_offset_*_cmd */
/* Build the TX word stream for `count` reads of request byte `request_byte`
 * (AP post handled by caller passing is_ap). Returns words written. */
size_t build_read_stream(const StreamCfg &c, uint8_t request_byte, bool is_ap,
                         uint16_t count, uint32_t *out, size_t out_cap);
/* RX words produced by that stream (3 per read + 1 for the AP post's ack). */
size_t read_rx_count(bool is_ap, uint16_t count);
}
```

- [ ] **Step 2: Write the failing test (small, exact)**

`test_swdp_block.cpp` — assert word count and the first read-word's 5-word template (cmd encodings via `fmt`-equivalent packing `((bits-1)&0xff)|(out_en<<8)|(cmd_addr<<9)`):
```cpp
#include "testing.hpp"
#include "swdp_block.hpp"
using namespace probe::swdp_block;
TEST(read_stream_one_word_dp_template) {
  StreamCfg c{ /*cmd_write*/10, /*cmd_read*/13, /*cmd_turn*/12, /*turnaround*/1 };
  uint32_t out[32];
  size_t n = build_read_stream(c, 0x25 /*DP read DRW*/, /*is_ap*/false, 1, out, 32);
  CHECK_EQ(n, 7u);                 /* write(2) + ackread(1) + dataread(1) + parityread(1) + turn(2) */
  CHECK_EQ(read_rx_count(false, 1), 3u);
}
```

- [ ] **Step 3: Run it — fails to compile (no swdp_block)**

Run: `bash scripts/run_host_tests.sh`
Expected: FAIL — `swdp_block.hpp` not found / undefined.

- [ ] **Step 4: Implement `build_read_stream` + `read_rx_count`**

`swdp_block.cpp`: emit `prq` from `request_byte` (same start/parity/stop/park packing as `sw_transfer` swdp_pio.c:249-259), then the 5-word read template ×count (RDBUFF request for the last AP read handled by caller in phase 5; for phase-3 tests the request is constant). `read_rx_count = 3*count (+1 if is_ap)`.

- [ ] **Step 5: Add to host build + run green**

Add `test_swdp_block.cpp` and `${...}/drivers/dp_pio/swdp_block.cpp` to `tests/host/CMakeLists.txt`; add include dir `drivers/dp_pio`.
Run: `bash scripts/run_host_tests.sh`
Expected: `OK: 78 tests`.

- [ ] **Step 6: Commit**
```bash
git add drivers/dp_pio/swdp_block.* tests/host
git commit -m "feat: pure read-block PIO stream builder (host-tested)"
```

---

## Task 4: Pure read-block response parser (host TDD)

`parse_read_response` consumes the RX capture (3 words/read: ack, data, parity; +1 ack for AP post), extracts data, checks ACK==OK and parity, reports the first non-OK index.

**Files:**
- Modify: `drivers/dp_pio/swdp_block.hpp` / `.cpp`, `tests/host/test_swdp_block.cpp`

- [ ] **Step 1: Declare**
```cpp
struct ReadParse { uint16_t completed; uint8_t ack; };
ReadParse parse_read_response(const StreamCfg &c, bool is_ap, uint16_t count,
                              const uint32_t *rx, size_t rx_len, uint32_t *data_out);
```

- [ ] **Step 2: Failing test — all-OK then fault-at-k + parity error**
```cpp
TEST(parse_read_all_ok) {
  StreamCfg c{10,13,12,1};
  /* 2 reads, DP: rx = [ack,data,par]x2; ack OK = 1<<turnaround region; build minimal */
  uint32_t rx[6] = { /*ack0*/ MK_ACK_OK(c), 0x000000AAu, MK_PAR(0xAA),
                     /*ack1*/ MK_ACK_OK(c), 0x000000BBu, MK_PAR(0xBB) };
  uint32_t data[2]; 
  ReadParse r = parse_read_response(c, false, 2, rx, 6, data);
  CHECK_EQ(r.completed, 2u); CHECK_EQ(r.ack, 1u /*OK*/);
  CHECK_EQ(data[0], 0xAAu); CHECK_EQ(data[1], 0xBBu);
}
```
(Define `MK_ACK_OK`/`MK_PAR` test helpers matching the PIO MSB-alignment + `>>turnaround` extraction; include a `parse_read_fault_at_1` and `parse_read_parity_error` case.)

- [ ] **Step 3: Run — FAIL (undefined).** Run: `bash scripts/run_host_tests.sh`.

- [ ] **Step 4: Implement** the extraction mirroring `sw_transfer`: ack `= rx >> turnaround` masked to 3 bits; data MSB-aligned (already 32-bit, no shift); parity via `__builtin_popcount(data) ^ par_bit`. Stop at first non-OK / parity error, set `completed`/`ack`.

- [ ] **Step 5: Run green.** Expected: `OK: 81 tests` (3 new).

- [ ] **Step 6: Commit**
```bash
git add drivers/dp_pio/swdp_block.* tests/host/test_swdp_block.cpp
git commit -m "feat: pure read-block PIO response parser (host-tested)"
```

---

## Task 5: PIO driver DMA glue + adapter wiring (on-target)

**Files:**
- Modify: `drivers/dp_pio/swdp_pio.c` (DMA init in `sw_port_on`/init, `sw_transfer_block`, add to `swdp_api`)
- Modify: `src/dap/swdp_shim.{c,h}`, `src/dap/swdp_driver.{cpp,hpp}`

- [ ] **Step 1: DMA channels + buffers in the driver**

Add to `swdp_pio_data`: `uint32_t dma_tx_ch, dma_rx_ch; struct k_sem dma_done;`. Add static buffers `static uint32_t tx_buf[CMD_WORDS_MAX]; static uint32_t rx_buf[RX_WORDS_MAX];` sized for ~127 transfers. At init/port_on: `const struct device *dma = DEVICE_DT_GET(DT_NODELABEL(dma)); dma_tx_ch = dma_request_channel(dma, NULL); dma_rx_ch = dma_request_channel(dma, NULL);` `k_sem_init(&dma_done,0,1)`.

- [ ] **Step 2: Implement `sw_transfer_block` (read fast path)**

```c
static int sw_transfer_block(const struct device *dev, uint8_t request,
                             uint32_t *data, uint16_t count, uint16_t *completed,
                             uint8_t *ack) {
  /* build TX stream (probe::swdp_block::build_read_stream via extern "C"),
     configure RX (PERIPHERAL_TO_MEMORY, src=&pio->rxf[sm] NO_CHANGE,
       dst=rx_buf INCREMENT, size 4, slot=~pio_get_dreq(pio,sm,false)&0x3F,
       callback -> k_sem_give(&dma_done)),
     configure TX (MEMORY_TO_PERIPHERAL, src=tx_buf INCREMENT,
       dst=&pio->txf[sm] NO_CHANGE, slot=~pio_get_dreq(pio,sm,true)&0x3F),
     dma_start(rx); dma_start(tx); k_sem_take(&dma_done, K_MSEC(50));
     parse_read_response -> *completed, *ack, data[].
     If completed < count (fault) OR sem timeout: dma_stop both, re-sync bus
       (line reset + WriteABORT) and finish via the CPU sw_transfer pipeline
       from word *completed. */
}
```
Provide C-callable wrappers in `swdp_block.cpp` (`extern "C"`) for the builder/parser so the C driver can call them.

- [ ] **Step 3: Add to `swdp_api` + shim + C++ adapter**

- `swdp_pio_api.swdp_transfer_block = sw_transfer_block;`
- swdp.h has no block member → add a local typedef path: call it through the shim by reading `((const struct swdp_api*)dev->api)->swdp_transfer_block`; if NULL return -ENOTSUP. Implement `probe_swdp_transfer_block` in `swdp_shim.c` + declare in `.h`.
- `ZephyrSwdpDriver::transfer_block_read` override → `probe_swdp_transfer_block`; on -ENOTSUP, call the base `SwdDriver::transfer_block_read`.

- [ ] **Step 4: Build all three boards**

Run: `bash scripts/build_all.sh`
Expected: clean, three `zephyr.uf2`, no overflow.

- [ ] **Step 5: Commit**
```bash
git add drivers/dp_pio src/dap
git commit -m "feat: DMA read-block fast path in PIO driver + adapter wiring"
```

---

## Task 6: On-target acceptance (the gate)

**Files:** none (verification).

- [ ] **Step 1: Flash + verify target live** — BOOTSEL, `cp build/rpi_pico/zephyr/zephyr.uf2 /Volumes/RPI-RP2/`; `openocd ... -c "init; halt; mdw 0x20000000 1; shutdown"` shows `DPIDR 0x0bc12477`, both cores.

- [ ] **Step 2: Throughput A/B** — `bash scripts/swd_throughput_bench.sh dma /tmp/bench_dma.txt`. Expected: clearly above the committed 176 KB/s @ 15 MHz (target ~270–300). Record vs `/tmp/bench_pktsize.txt`.

- [ ] **Step 3: Correctness** — write/readback 3 patterns at 5 MHz (must match); two 128 KB reads at 15 MHz `cmp`-identical (must match). Any mismatch = stop, debug (systematic-debugging).

- [ ] **Step 4: Forced-fault fallback** — read a parked/unmapped AP so an ACK is non-OK; confirm openocd gets the same partial result a CPU-only build gives (compare against the committed `905138d` firmware on the same op).

- [ ] **Step 5: Host regression** — `bash scripts/run_host_tests.sh` → all green.

- [ ] **Step 6: Commit results + decide gate**
```bash
git commit --allow-empty -m "test: DMA read-block on-target acceptance (throughput + integrity + fault)"
```
If the gain/risk justifies it, proceed to the Phase 2 (write-block) and Phase 3 (fault-hardening) plans; otherwise stop with read-block banked or revert.

---

## Self-Review Notes

- **Spec coverage:** builder (Task 3), parser (Task 4), Zephyr-DMA glue (Task 5), optimistic+verify+fallback (Task 5 Step 2 + Task 6 Step 4), engine delegation (Task 2), testing (Tasks 2-6). Write-block + fault-hardening are explicitly deferred to follow-on plans (spec phasing).
- **Type consistency:** `transfer_block_read(request, count, data, ack) -> completed` used identically in interfaces.hpp, engine, adapter, shim, driver.
- **Known iteration point:** Task 5 DMA glue is on-target hardware bring-up; the exact `dma_config` field values may need tuning (data size, addr-adj, sem timeout) — the host-tested builder/parser de-risk the logic, leaving only the transport to verify on hardware.
