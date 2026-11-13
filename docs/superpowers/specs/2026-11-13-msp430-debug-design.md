# MSP430F5529 Spy-Bi-Wire Support — Design (Phase B: Debugging)

Status: design, 2026-11-13. Extends `2026-11-11-msp430-sbw-design.md` (Phase A,
flash programming over SBW), whose transport, TAP and device layers are reused
unchanged.

## Background and Goal

Phase A made the probe erase, program, verify and read an MSP430F5529 over
Spy-Bi-Wire through a CMSIS-DAP vendor command (`0x90`) driven by
`tools/msp430/sbw.py`. Phase B adds source-less and source-level debugging
with GDB: halt, resume, single-step, CPU registers, memory, hardware
breakpoints.

The first target is the user's own F5529 board, which runs a commercial
firmware that exists nowhere else except in the backups made in Phase A. It
must be possible to debug that firmware without any risk of changing its
flash. A second target class, boards whose flash may be rewritten freely, gets
the full feature set afterwards.

### Requirement Decisions (confirmed with the user)

| Decision | Choice |
|---|---|
| Use | Both: first B1, a read-only mode for the existing firmware on the user's board; then B2, a full mode for boards whose flash may be rewritten. |
| Front end | GDB command line: `msp430-elf-gdb` from TI's MSP430-GCC package (to be installed on the Mac with the user's consent). VS Code can later use the same server. |
| Architecture | GDB remote-protocol server in the host tool (`sbw.py gdbserver`), new debug operations under the existing vendor command in the probe firmware. No USB descriptor change. |
| Symbols for the existing firmware | A symbol-only ELF generated from a name table (address, name, size, kind), outside the repository for third-party firmware. |

### Scope

| | B1: read-only (first) | B2: full (second) |
|---|---|---|
| Connect | attach to the running program without reset; or reset and halt at the reset vector | same, plus loading a program |
| Halt, resume, single-step | yes | yes |
| CPU registers | read and write | read and write |
| Memory | read everything; write RAM and peripheral registers; **never write flash** (main, information, BSL) | read and write everything; flash writes go through the Phase A erase/program path |
| Breakpoints | EEM hardware breakpoints only | hardware first, then software breakpoints in flash |
| GDB `load` | refused | allowed |
| Disconnect | restore the context and let the program continue from where it stopped, no reset | continue or reset |

### Non-goals

- 4-wire JTAG; other MSP430 families (the code stays CPUXv2-generic as in
  Phase A, only the F5529 is verified).
- Data watchpoints, trace, state storage, energy measurement, real-time
  variable watch. Watchpoints may follow B2 if the EEM supports them.
- Source-level stepping in the existing firmware: its reconstructed source
  does not have the firmware's code addresses. Instruction-level stepping
  with symbols works.
- A GDB server inside the probe firmware.

## Reference Material

- **TI MSP Debug Stack** (MSPDS, "MSPDS-OPEN-SOURCE", slac460; current release
  3.15.1.1). The firmware part (`Bios/src/hal/macros/`) holds the CPUXv2
  debug sequences this design ports:
  `SyncJtag_Conditional_SaveContextXv2` (take over a running CPU without reset
  and save its context), `SyncJtag_AssertPor_SaveContextXv2` (reset and halt),
  `RestoreContext_ReleaseJtagXv2` (restore and run), `SingleStepXv2`,
  `ReadAllCpuRegsXv2` / `WriteAllCpuRegsXv2`, `ReadMemWordsXv2` /
  `WriteMemWordsXv2` (memory access that preserves the halted context),
  `EemDataExchangeXv2` (EEM register access, breakpoint triggers),
  `WaitForEem` / `PollJStateReg` (stop detection). The files carry TI's
  BSD-3-Clause notice; the implementation checks the notice of every ported
  file against TI's official package and records it in
  `THIRD_PARTY_NOTICES.md`, as Phase A did for SLAU320. Some later parts of
  the MSPDS package are under TI's TSPA licence; nothing from those is used.
- TI SLAU320 (Phase A reference) for the shared JTAG primitives.
- MSP430F5529 datasheet (EEM size: number of breakpoint comparators), TI
  device database in MSPDS (same information, machine-readable).
- GDB Remote Serial Protocol documentation (packets, target description,
  memory map) and `msp430-elf-gdb`'s MSP430X register layout.
- Behaviour reference only, no code taken: `grumat/glossy-msp430` (GPL-2.0,
  an embedded GDB server for MSP430).

## Architecture

```
msp430-elf-gdb ⇄ TCP, GDB remote protocol ⇄ sbw.py gdbserver (tools/msp430/gdbserver.py)
               ⇄ USB, CMSIS-DAP vendor command 0x90 ⇄ msp430::Session
               ⇄ msp430::Debugger ⇄ msp430::Cpuxv2 ⇄ SbwTap ⇄ PIO SBW ⇄ MSP430F5529
```

- **Transport, TAP, PIO** (`drivers/sbw_pio`, `SbwTap`): unchanged.
- **`Cpuxv2`**: unchanged in behaviour; a few primitives it already has
  (`set_pc`, instruction fetch, `in_full_emulation`, word read/write) are made
  usable from the new layer.
- **`msp430::Debugger`** (new, `lib/msp430/src/debug.cpp`,
  `include/msp430/debug.hpp`): the ported MSPDS sequences: attach with or
  without reset, saved context (PC, SR, watchdog control, the registers the
  sequences clobber), register access, memory access while halted, single
  step, release with breakpoints armed, stop detection, EEM breakpoint
  triggers. It never calls a flash erase or write path.
- **`msp430::Session`**: new operations under `0x90`, the debug states, and
  the flash lock (below).
- **Host**: `probe.py` gets client methods; `gdbserver.py` implements the GDB
  remote protocol; `symelf.py` makes symbol-only ELF files; `sbw.py` gets a
  `gdbserver` subcommand.

### Session states

```
Idle ──CONNECT──▶ Connected ──FLASH_BEGIN──▶ Flashing          (Phase A)
Idle ──DBG_ATTACH──▶ DebugHalted ⇄ DebugRunning                 (Phase B)
DebugHalted / DebugRunning ──DBG_DETACH──▶ Idle
```

`CONNECT` (Phase A) keeps its power-on reset. Debugging starts from Idle with
`DBG_ATTACH`, so the attach-without-reset path never passes through Phase A's
reset. Phase A's `ERASE`, `FLASH_*` and `WRITE_MEM` to flash return
`BAD_STATE` or `PROTECTED` in the debug states unless the session was opened
in full mode (B2).

## Vendor Command Extension

Protocol version 2. All fields little-endian; addresses and register values
are 20-bit numbers in 32-bit fields.

| op | Name | Request args | Response data |
|---|---|---|---|
| 0x00 | INFO | – | as before; `version` = 2, new `state` values 3 DebugHalted, 4 DebugRunning; `features` bit 1: debugging; ninth byte `u8 hw_breakpoints` (the F5529's 8 EEM triggers; B1 supports only the F5529) |
| 0x09 | DBG_ATTACH | `u8 mode` (0 attach without reset, 1 reset and halt at the reset vector), `u8 flags` (bit 0: full mode, B2 only), `u16 speed_khz`, `u32 ram_lo`, `u32 ram_hi` (the device's RAM range from the host's device table) | the CONNECT identity block, then `u32 pc`, `u8 stop_reason`, `u8 hw_breakpoints` |
| 0x0A | DBG_DETACH | `u8 action` (0 restore context and run, 1 reset and run) | – |
| 0x0B | DBG_HALT | – | `u32 pc` |
| 0x0C | DBG_RESUME | – | – (breakpoints armed, CPU runs) |
| 0x0D | DBG_STATUS | – | `u8 state` (running / halted), `u8 stop_reason` (request, breakpoint, step, reset), `u8 breakpoint_index`, `u32 pc` (0 while running) |
| 0x0E | DBG_STEP | – | `u32 pc` |
| 0x0F | DBG_READ_REGS | – | 16 × `u32` (R0 PC, R1 SP, R2 SR, R3 CG, R4–R15) |
| 0x10 | DBG_WRITE_REG | `u8 index`, `u32 value` | – |
| 0x11 | DBG_BP_SET | `u8 index`, `u32 address` | – |
| 0x12 | DBG_BP_CLEAR | `u8 index` (0xFF: all) | – |

- `READ_MEM` / `WRITE_MEM` (Phase A ops) work in DebugHalted through the
  debug layer, which preserves the halted context. In DebugRunning they
  return `RUNNING` (correction 6).
- New status `PROTECTED` (0x09): in read-only mode `WRITE_MEM` may only
  touch the peripheral space below 0x1000 **without the flash controller
  registers (0x0140–0x014F)**, and RAM. The RAM window is the **intersection**
  of the range given with `DBG_ATTACH` and the RAM the probe itself knows for
  the device it identified (F5529, device ID bytes 55h 29h: 0x1C00–0x43FF, USB RAM
  included); for a device the probe does not know, the window is empty, and
  so is the peripheral window (the refused ranges below are per device). Any
  other address — main flash, information memory, BSL, vacant memory — is
  refused with `PROTECTED`. A wrong host table can therefore only narrow the
  window, never open flash. Three more peripheral ranges are refused: the DMA
  module (F5529: 0x0500–0x053F), since a DMA transfer could copy into the
  flash controller; the SYS module (0x0180–0x019F: `SYSCTL.SYSRIVECT`
  would move the interrupt vectors into RAM, `SYSBSLC` controls the BSL
  area's protection); and the PMM module (0x0120–0x013F: a byte write to
  `PMMCTL0` becomes a word write with the read-back password 0x96, which
  resets the chip).
- In read-only mode `DBG_WRITE_REG` to the PC is `PROTECTED` unless the new
  PC lies in the identified device's main flash (F5529: 0x4400–0x243FF) or
  equals the current PC (GDB writes all registers back). This keeps jumps
  into the BSL, information memory or host-written RAM out of reach.
- What read-only mode guarantees: the probe never writes flash, the flash
  controller, the PMM, the DMA or the SYS registers for the host, and never moves the PC
  outside main flash. It does not stop the firmware's own code from doing
  what it does when it runs (the firmware may write its own information
  memory), nor code the user places by other means, such as a changed return
  address on the stack.
- New status `RUNNING` (0x0A): an operation that needs a halted CPU while it
  runs.
- Reads (host side): GDB's map leaves out the BSL area, vacant memory
  (0x0000–0x00FF, 0x0940–0x0FFF, 0x1B00–0x1BFF) and everything above main
  flash. The GDB server also refuses, by default, reads of the registers
  whose read has a side effect (interrupt-vector registers, serial receive
  buffers, ADC12MEM, the JTAG mailbox inputs; the list is in the device
  table), because GDB reads memory on its own (stack unwinding, caches) and
  a stray read would make the firmware lose an interrupt or a byte;
  `monitor read` reads one on purpose. The probe itself does not restrict
  reads.

## Component Design

### Probe firmware: `msp430::Debugger`

- **Attach without reset**: entry sequence and JTAG ID as Phase A, lock check
  (a locked device ends the attach with `LOCKED`), then
  `SyncJtag_Conditional_SaveContextXv2`: synchronises with the running CPU,
  handles a CPU in a low-power mode, stops the watchdog and saves `WDTCTL`,
  and saves PC and SR. Stop reason "request".
- **Reset and halt**: `SyncJtag_AssertPor_SaveContextXv2`; PC is the reset
  vector's target.
- **Registers**: `ReadAllCpuRegsXv2` / `WriteAllCpuRegsXv2` on the saved
  context; PC and SR live in the saved context and are written back on
  release.
- **Memory while halted**: the MSPDS word read/write sequences, which keep the
  halted state; byte-granular writes are read-modify-write of words.
- **Single step**: `SingleStepXv2` with the EEM's single-step trigger;
  interrupts pending at the step are handled as MSPDS does.
- **Run and stop detection**: `RestoreContext_ReleaseJtagXv2` with the EEM
  breakpoints armed; `DBG_STATUS` polls the EEM / JTAG state register as
  `WaitForEem` / `PollJStateReg` do, and on a stop re-saves the context.
  `DBG_HALT` stops a running CPU through the EEM.
- **Breakpoints**: EEM trigger blocks programmed through
  `EemDataExchangeXv2`, one instruction-fetch trigger per breakpoint. The
  number of triggers comes from the device (EEM size of the F5529 from its
  datasheet / the MSPDS device database, confirmed on the board).
- **Clocks while halted**: the EEM clock-control setting follows MSPDS's
  default for the device; the chosen value is documented after porting
  (whether timers keep running while halted matters for firmware that keeps
  time with a timer; the acceptance measures Timer0_B).
- **Detach**: clear the triggers, restore the context (including `WDTCTL`) and
  release; with action 1 a POR instead.
- **No flash paths**: `Debugger` contains no call to the funclet, erase or
  flash-write code of `Cpuxv2`. A host test asserts it (below).

### Probe firmware: `msp430::Session`

New handlers, the states above, the `PROTECTED` check on `WRITE_MEM`, the
lock of Phase A's flash ops in debug states, `abort()` extended: an open debug
session is detached with action 0 (restore and run) when USB goes away or SWD
takes over the pins, so a lost host never leaves the target halted.

### Host: `gdbserver.py`

- Listens on a TCP port (default 3333), one GDB client at a time.
- Packets: `qSupported` (`PacketSize`, `qXfer:features:read+`,
  `qXfer:memory-map:read+`), `?`, `g`/`G`, `p`/`P`, `m`/`M`, `X`, `c`, `s`,
  `vCont?`/`vCont;c;s`, `Z1`/`z1`, `Z0`/`z0`, `D`, `k`, `qRcmd` (monitor), the
  interrupt byte `0x03`, and the `qXfer` objects.
- **Target description**: the MSP430X register set as `msp430-elf-gdb`
  expects it (register count, sizes and order verified against the installed
  GDB during implementation).
- **Memory map**: RAM and peripherals as `ram`; main flash and information
  memory as `rom` in read-only mode, `flash` with their segment size in full
  mode. The BSL area (0x1000–0x17FF) is left out of the map and reads there
  are refused (reading it reset the CPU of the user's board in Phase A). With flash marked `rom`, GDB's automatic hardware-breakpoint
  rule makes an ordinary `break` use `Z1`, so users need not type `hbreak`.
- **Read-only enforcement on the host**: `M`/`X` into a flash range,
  `Z0`, `vFlash*` and `load` answer with an error in read-only mode, before
  anything is sent to the probe.
- **Running**: after `c`, poll `DBG_STATUS` every 50 ms; on a stop send
  `T05` (with the PC); `0x03` sends `DBG_HALT`.
- **Monitor commands**: `monitor reset` (reset and halt), `monitor mode
  readonly|full` (full needs `--allow-flash-write` on the command line, B2),
  `monitor speed <kHz>`, `monitor status`.
- **Disconnect**: `D` and a dropped TCP connection detach with action 0.

### Host: `symelf.py`

`symelf.py names.tsv -o symbols.elf [--image backup.hex]` writes an ELF32
(`EM_MSP430`) with a symbol table only (functions as `STT_FUNC`, objects as
`STT_OBJECT`, with sizes). With `--image` it adds the image as section
contents so GDB disassembles without reading the target. The tool is
generic; tables and images of third-party firmware stay outside the
repository (for the user's board: `~/msp430-backup/fw-rebuild/notes/names.tsv`
and `objnames.tsv`).

### Host: `sbw.py gdbserver`

`sbw.py gdbserver [--port 3333] [--attach | --reset] [--speed kHz]
[--allow-flash-write]`. Default: attach without reset, read-only.
`--allow-flash-write` is B2 and is refused for a target whose device ID and
first flash words match a backup marked "do not write" (a list kept by the
user outside the repository, holding the user's board), so the existing
board cannot be put into full mode by mistake.

## Error Handling Principles

- A failed attach leaves the target as it was: before the context is saved
  nothing is changed; after it, a failure releases with the saved context.
- Every operation that can leave the CPU halted has a release path: detach on
  `D`, on a dropped TCP connection, on Ctrl-C of the server, on USB loss
  (`Session::abort`).
- A target that stops answering (power loss, reset by the user) ends the
  debug session with `TargetError` as in Phase A; GDB is told the target
  exited.
- The firmware's watchdog is stopped while halted and restored on release, so
  a halt never ends in a watchdog reset. A single step, like TI's DLL, puts
  the firmware's own `WDTCTL` back for the step's brief release, so the
  watchdog counts during each step and its counter is not cleared; many steps
  in a row with the watchdog running can therefore end in a watchdog reset,
  as with TI's tools.

## Testing and Acceptance

1. **Host C++ tests** (`scripts/run_host_tests.sh`, also under ASan/UBSan):
   - `Debugger` sequences against the scripted fake `Tap`, compared with
     vectors derived from the MSPDS macros, as Phase A did with SLAU320:
     attach without reset (CPU active and in each low-power mode), reset and
     halt, register read/write, step, release, stop detection, breakpoint
     programming, detach.
   - A "no flash write" test: every debug operation run against the fake
     records no write to the flash controller registers (`FCTL1`–`FCTL4`) and
     no funclet start.
   - `Session`: new ops, framing and bounds, every state transition, the
     `PROTECTED` and `RUNNING` statuses, the flash lock, `abort()` detaching.
2. **Python tests**: the fake firmware gets a debug model (halted/running
   state, registers, breakpoints, stop events); `gdbserver.py` packet tests,
   read-only enforcement, polling and interrupt, detach on disconnect;
   `symelf.py` output checked with `readelf`-equivalent parsing; an end-to-end
   test with a scripted `msp430-elf-gdb --batch` session against the fake when
   GDB is installed.
3. **Build**: all three boards with the feature; rpi_pico without it.
4. **On-target acceptance, B1, on the user's board** — read-only throughout;
   the flash is compared with the Phase A backup before and after:
   1. `sbw.py info` shows protocol 2 with the debug feature and the
      breakpoint count.
   2. Attach without reset, read registers, detach: the target keeps running
      (display and keys respond).
   3. Halt and resume 50 times while the target runs; it keeps working.
   4. A hardware breakpoint on a key handler (by symbol from the symbol ELF),
      press the key, GDB stops there; step, inspect variables, continue.
   5. Reset and halt at the reset vector, step through the start-up, continue.
   6. Read-only refusals: `load`, a write into flash, a software breakpoint
      request are refused with a message.
   7. The full flash and information memory read back equal the backup.
5. **B2 acceptance**: on a board whose flash may be rewritten (not the user's
   board): load a test program, software and hardware breakpoints, flash
   write through GDB. Needs such a board; deferred until one is available.

## Delivery

B1 and B2 are separate plans. B1 contains the probe-side debug layer, the
vendor ops, the host server in read-only mode, `symelf.py`, and the B1
acceptance. B2 adds full mode (flash writes through Phase A's path, software
breakpoints, `load`) and its acceptance.

## Risks and Mitigations

| Risk | Mitigation |
|---|---|
| Attaching changes the running firmware's state (watchdog, low-power mode, peripherals) | port MSPDS's conditional sync exactly, test each LPM on the fake, first try on the board with the display visibly running, compare flash afterwards |
| The register layout does not match `msp430-elf-gdb` | verify against the installed GDB before writing the server's register code |
| GDB sends software-breakpoint or flash-write requests anyway | memory map marks flash `rom`; the server refuses `Z0`/`M`/`X`/`vFlash` in read-only mode; the probe refuses flash writes in debug states |
| Too few hardware breakpoints for GDB's internal breakpoints (e.g. on `step` over calls) | report the count to GDB; use `DBG_STEP` for stepping; document the limit |
| Timers keep running while halted and the firmware sees time jump | follow MSPDS's clock-control default; document; it does not endanger flash |
| A halt during the firmware's own flash write (information memory) | the CPU is stalled during a flash write from flash; document; check `FCTL3.BUSY` before saving context and wait |
| Licence of ported files | check each file's notice against TI's official MSPDS package; use only BSD-3-Clause files |

## Corrections made while planning B1 (2026-11-13)

The B1 plan (`docs/superpowers/plans/2026-11-13-msp430-debug-b1.md`, section
"Spec corrections needed") found the following; items 3, 4, 5 and 7 are
already worked into the text above, the others take precedence over it:

1. No target description is sent: `msp430-elf-gdb` fixes the remote register
   layout itself (16 registers R0–R15, 4 bytes each).
2. GDB selects MSP430X with 20-bit addresses only from the ELF's
   `.MSP430.attributes` section; `symelf.py` writes it.
3. The RAM window is the intersection of the host's range and the probe's own
   device table (see `PROTECTED`).
4. The flash controller registers are excluded from the writable peripherals.
5. The BSL area is left out of the memory map and never read.
6. `READ_MEM` / `WRITE_MEM` while running return `RUNNING`.
7. `INFO` has a ninth byte with the breakpoint count.
8. `DBG_STATUS`: state 0 running / 1 halted; stop reason 0 while running;
   breakpoint index 0xFF for none; PC 0 while running.
9. After Ctrl-C (0x03) the server replies `T02`; breakpoints and steps reply
   `T05` (`hwbreak:;` for breakpoints).
10. `symelf.py` accepts name tables without a size column and skips type and
    member rows; with `--image` only main flash goes into `.text`.
11. The F5529's EEM size (8 triggers) and device parameters come from the
    MSPDS 3.3.1.3 device database (BSD-3-Clause); the 3.15.1.1 package has
    none.
12. `PollJStateReg` is not ported (the F5529 is polled with `WaitForEem`).
13. Clocks while halted: EEM general clock control 0x000E (MCLK, SMCLK, ACLK
    stopped); per-module clock control stays at the device's defaults; the
    acceptance measures Timer0_B while halted.
14. The watchdog is held right after the sync, then `FCTL3.BUSY` is waited
    for (up to 100 ms), then the status register is saved (changed in the
    Task 5 review so the watchdog cannot bite during the wait).
15. The reset-vector substitution on attach applies only to PC 0x0FFFE or
    0xFFFFE; MSPDS applies it to any address ending in FFFE, which on the
    F5529 includes 0x1FFFE in main flash.
16. A failure after the CPU is in full emulation (failed init-state check,
    WDTCTL without its 0x69 password) restores the PC before releasing the
    CPU, as every TI release does; failures before full emulation release
    without writing anything to memory. On the reset-and-halt path a failure
    after the power-on reset leaves the CPU at the safe PC (vacant memory), so
    instead of a release it gets a second power-on reset; and on every
    reset-and-halt failure after the clock switch, GENCLKCTRL is set back to
    0x000E (the macro's own end value) before the release or the reset, so
    the CPU is never let go with its clocks on the JTAG clock.
17. A single step that does not stop in time halts the CPU, restores trigger
    block 0 and returns `TIMEOUT`.
18. `DBG_ATTACH` uses only the normal SBW entry; its full-mode flag is
    `BAD_ARG` in B1; `DISCONNECT` in a debug state is `BAD_STATE`; `ERASE`
    and `FLASH_BEGIN` are `PROTECTED`; GDB's `k` detaches instead of killing;
    `monitor speed` takes effect at the next attach; `--allow-flash-write` is
    refused in B1; the server listens on 127.0.0.1 only.
