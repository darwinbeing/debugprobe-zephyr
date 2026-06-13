# SWD PIO DMA Backend — Design

Status: approved for spec review (2026-06-12). Branch: `swd-pio-dma`.

## Background and Goal

After raising the DAP packet size to 512 bytes (commit `905138d`), SWD read
throughput on rpi_pico/PIO is 56 / 134 / 176 KB/s at 1 / 5 / 15 MHz adapter
speed — already 21–39% faster than the official debugprobe v2.3.1. Decomposing
the per-word cost into fixed CPU + on-wire time (fits all three measured points
within 0.1 µs) shows a **constant ~19 µs/word of CPU cost** in the per-word SWD
loop:

| adapter speed | per-word | SWD wire | **CPU** |
|---|---|---|---|
| 1 MHz | 69.7 µs | 50.8 µs | ~19 µs |
| 5 MHz | 29.1 µs | 10.2 µs | ~19 µs |
| 15 MHz | 22.2 µs | 3.4 µs | ~19 µs |

At 15 MHz, 85% of each word is CPU spent interleaving `pio_sm_put_blocking` /
`pio_sm_get_blocking` calls with per-word ACK/parity checks
(`drivers/dp_pio/swdp_pio.c` `sw_transfer`). The bus is mostly idle waiting on
the CPU.

**Goal:** stream a whole `DAP_TransferBlock` through the PIO using DMA so the CPU
leaves the per-word loop. Projected ~270–300 KB/s at 15 MHz (~1.5–1.7× current).

### Requirement Decisions (confirmed with the user)

1. **Scope: full DMA backend** — both read-block and write-block paths.
2. **Fault handling: optimistic DMA + post-verify** — DMA the whole block
   assuming all ACKs are OK; verify after; fall back to the CPU path on any
   non-OK ACK.
3. **`probe.pio` is NOT modified** — it stays byte-identical to the upstream
   generated program. DMA only changes who feeds/drains the FIFOs.

### Non-goals

- No change to single `DAP_Transfer`, SWJ sequences, or any non-block command —
  they stay on the proven CPU `sw_transfer` path.
- No change to the on-the-wire SWD protocol — conformance stays identical.
- No `probe.pio` / pioasm changes.
- Bitbang and null backends gain only a trivial loop implementation of the new
  block API (no DMA).

## Overall Architecture

A new **block** entry point on the SWD driver API replaces the engine's
per-word loop for `DAP_TransferBlock`:

```
DapEngine::cmd_transfer_block
        │  (request, data[], count, idle)
        ▼
swdp_api.transfer_block(dev, request, data[], count, idle, response[]) -> completed
        │
        ├── PIO driver:   DMA fast path  ──fault──►  CPU sw_transfer fallback (from word k)
        ├── bitbang/null: loop over sw_transfer  (no DMA)
```

The DMA fast path is built from two **pure, host-testable** units plus thin
hardware glue:

```
build_block_stream(request, data[], count, cfg) ──► TX command-word array
                 (start RX DMA, start TX DMA, wait RX done)
RX capture words ──► parse_block_response(rx, count, cfg) ──► {data[], first_fault, ack}
```

## Component Design

### ① Block stream builder (pure C++, host-tested)

`build_block_stream` produces exactly the 32-bit words `probe_write_bits` /
`probe_read_bits` / `probe_hiz_clocks` would have pushed to the TX FIFO, for an
entire block:

- **Read word** (per transfer): `write-cmd(8)+prq`, `read-cmd(turnaround+3)`
  (ACK), `read-cmd(32)` (data), `read-cmd(1)` (parity), `turnaround-cmd+0`.
  `prq` (request packet) is identical for every word in the block, so the read
  template is a constant repeated `count` times.
- **Write word** (per transfer): `write-cmd(8)+prq`, `read-cmd(turnaround+3)`
  (ACK), `turnaround-cmd+0`, `write-cmd(32)+data[k]`, `write-cmd(1)+parity`,
  idle cycles. The payload `data[k]` is interleaved per word.

Command-word encoding is the existing `fmt_probe_command` packing
(`| cmd_addr | out_en | bit_count-1 |`); the builder reuses it so the PIO sees
identical commands.

### ② Block response parser (pure C++, host-tested)

`parse_block_response` consumes the RX capture buffer:

- Reads produce **3 RX words/transfer**: ACK, data, parity. Writes produce
  **1 RX word/transfer**: ACK.
- Per transfer: extract ACK (`>> turnaround`), compare to OK; for reads extract
  data (MSB-aligned, matching `probe_read_bits`) and verify parity.
- Returns `data[]`, the index of the **first non-OK transfer** (`count` if none),
  and that transfer's ACK byte.

### ③ DMA glue in the PIO driver (`drivers/dp_pio/`)

- **Init:** claim two DMA channels (TX, RX). TX: `mem → PIO TX FIFO`, paced by
  the SM's TX-not-full DREQ. RX: `PIO RX FIFO → mem`, paced by RX-not-empty
  DREQ. Use the RP2040 SDK DMA HAL already available through `hal_rpi_pico`.
- **Per block:**
  1. `build_block_stream` into a static TX buffer.
  2. Configure RX DMA: `read_count = 3·count` (reads) or `count` (writes) into
     a static RX buffer.
  3. Start RX DMA, then TX DMA.
  4. Block on RX DMA completion (the SM stalls naturally if RX FIFO fills; RX
     DMA keeps it drained).
  5. `parse_block_response`.
- **Buffers:** static, sized for the maximum block from a 512 B packet (~127
  transfers): TX ≈ 3.6 KB, RX ≈ 1.5 KB. (RAM budget: firmware currently ~11%.)

### ④ Optimistic fault recovery

If `parse_block_response` reports `first_fault = k < count`:

1. Transfers `0..k-1` (all OK) are valid → keep their data, `completed = k`.
2. Words `k+1..` were clocked optimistically and are meaningless (and may have
   desynced the bus). **Re-synchronize:** drive idle clocks / line reset as
   needed and clear sticky errors via `DAP_WriteABORT`.
3. **Redo from word `k`** on the CPU `sw_transfer` path, which performs the
   real WAIT-retry / FAULT handling. Its result (further completed count + final
   ACK) is appended, producing the same `{count, ack, data...}` the all-CPU path
   would have. Matches `DAP_TransferBlock` partial-count semantics
   (DAP.c:1267-1460).

Healthy RAM block reads almost never fault, so the fast path dominates; the
fallback exists purely for correctness parity.

## Testing and Acceptance

- **Host (TDD):** `build_block_stream` and `parse_block_response` —
  read-template correctness, write-payload interleave, all-OK parse,
  fault-at-word-k detection, parity-error detection. Pure, compiled into the
  host test binary like `estimator.cpp` / the engine.
- **Regression:** existing 77 host tests stay green; engine block semantics
  unchanged (the engine just calls the new API).
- **On-target (rpi_pico/PIO vs live RP2040):**
  - `scripts/swd_throughput_bench.sh` A/B — expect a clear jump over 176 KB/s.
  - Correctness: write/readback at 5 MHz; two 128 KB reads byte-identical at
    15 MHz (already passing, must stay passing).
  - **Forced fault:** read an unmapped/parked AP so an ACK is non-OK; confirm
    the fallback produces the same partial count + ACK as a CPU-only build.
- **3-board build** stays clean (only the PIO driver gains DMA; bitbang/null get
  the loop fallback).

## Error Handling Principles

- A DMA timeout (SM stalled, RX never completes) aborts the channels, leaves the
  bus, and falls back to the full CPU path for that block — never wedges.
- The on-wire behavior on any non-OK ACK is identical to the current CPU path
  after fallback, so no host-visible semantic change.

## Phased Implementation

1. **Read-block DMA** — builder + parser (read), DMA glue, engine wiring for
   reads. Prove on the benchmark + integrity checks. (Highest value, provable.)
2. **Write-block DMA** — extend builder/parser for the write template and the
   per-word payload interleave; reuse the same DMA machinery.
3. **Fault fallback hardening** — re-sync + CPU redo from word k; forced-fault
   acceptance.

Each phase is independently testable; read-block alone already captures the
benchmark win.

## Risks and Mitigations

- **Bus desync after an optimistic fault** → always re-sync (line reset + ABORT)
  before the CPU redo; covered by the forced-fault acceptance test.
- **RX/TX DMA word-count mismatch vs what the SM emits** → the parser asserts
  the expected RX count; builder/parser share the per-word word-count constants,
  host-tested together.
- **DMA channel contention** (USB/other peripherals) → claim dedicated channels
  at init; the SWD block path is the only user.
- **`hal_rpi_pico` DMA API availability** → verify the SDK DMA headers are
  reachable from the driver before phase 1 (same HAL that already exposes PIO).
- **Effort/complexity exceeds payoff** → phase 1 (read-block) is the gate; if the
  measured gain or risk isn't worth it, stop after phase 1 with the win banked.
