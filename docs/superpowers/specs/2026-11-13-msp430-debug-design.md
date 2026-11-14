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
| 0x0A | DBG_DETACH | `u8 action` (0 restore context and run, 1 reset and run) | – (`TARGET_ERROR`: the session is over, but the program does not continue where it stopped; correction 26) |
| 0x0B | DBG_HALT | – | `u32 pc` |
| 0x0C | DBG_RESUME | – | – (breakpoints armed, CPU runs) |
| 0x0D | DBG_STATUS | – | `u8 state` (running / halted), `u8 stop_reason` (request, breakpoint, step, reset), `u8 breakpoint_index`, `u32 pc` (0 while running) |
| 0x0E | DBG_STEP | – | `u32 pc` |
| 0x0F | DBG_READ_REGS | – | 16 × `u32` (R0 PC, R1 SP, R2 SR, R3 CG, R4–R15) |
| 0x10 | DBG_WRITE_REG | `u8 index`, `u32 value` | – |
| 0x11 | DBG_BP_SET | `u8 index`, `u32 address` | – |
| 0x12 | DBG_BP_CLEAR | `u8 index` (0xFF: all) | – |
| 0x13 | DBG_EEM_PEEK | `u16 speed_khz` (100–10000, correction 29), `u8 eem_addr` (even, 0x00–0x3E) | `u8 jtag_id`, `u32 first`, `u32 second` (correction 25; Idle only, no session opened) |

- `READ_MEM` / `WRITE_MEM` (Phase A ops) work in DebugHalted through the
  debug layer, which preserves the halted context. In DebugRunning they
  return `RUNNING` (correction 6).
- New status `PROTECTED` (0x09): in read-only mode `WRITE_MEM` may only
  touch the device's peripheral registers (F5529: 0x0100–0x093F, `SFRIE1` to
  `USBFUNADR`; the vacant 0x0000–0x00FF and 0x0940–0x0FFF are refused, as
  GDB's map leaves them out) **without the flash controller registers
  (0x0140–0x014F)** and the three blocks below, and RAM. The RAM window is
  the **intersection** of the range given with `DBG_ATTACH` and the RAM the
  probe itself knows for the device it identified (F5529, device ID bytes
  55h 29h: 0x1C00–0x43FF, USB RAM included); for a device the probe does not
  know, the window is empty, and so is the peripheral window (it and the
  refused ranges below are per device). Any other address — main flash,
  information memory, BSL, vacant memory — is refused with `PROTECTED`. A
  wrong host table can therefore only narrow the window, never open flash. Three more peripheral ranges are refused: the DMA
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
  An attach without reset never resets the target on a healthy link; only if
  the link itself fails after the context was saved (its check fails twice in
  a row) does the probe reset the target, so that the firmware restarts
  instead of hanging (correction 26).
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

## Corrections made while executing B1 (2026-11-13)

Found during the implementation and the final review (the plan's "As-built
corrections found during execution" has the details and commits). Item 21 is
already worked into the text above; the others take precedence over it.

19. `DBG_ATTACH` in DebugHalted or DebugRunning ends the open session as
    `DBG_DETACH` with action 0 does (a running CPU is halted first) and then
    attaches as from Idle; it stays `BAD_STATE` in Connected and Flashing.
    `monitor reset` relies on it.
20. Every release that feeds the CPU its context or its PC after a failure
    (detach with action 0, which `Session::abort` uses after a memory,
    register or breakpoint access returned `TARGET_ERROR`, and the release at
    the PC after a failed sync) first checks the link again (JTAG ID 0x91
    and a control-signal bit of 0x0301). If the check fails, nothing is fed
    over the link: the EEM is turned off and the CPU released where it is,
    nothing written to its memory (for detach with action 0, correction 26
    replaces this with a reset). This refines "after it, a failure releases
    with the saved context" under Error Handling Principles, and it refines
    correction 16 too: after a failed init-state check the link check
    before the release usually fails as well, so the release now usually
    lets the CPU go where it is (context not yet saved) or resets it
    (context saved, correction 26) instead of restoring its PC or context.
21. The peripheral write window is the device's own: F5529 0x0100–0x093F
    (`SFRIE1` to `USBFUNADR`) minus the refused blocks; the vacant
    0x0000–0x00FF and 0x0940–0x0FFF are refused.
22. `sbw.py gdbserver` refuses a probe that still holds a programming
    session from an earlier command, debugs only with the protocol versions
    it knows (2), and takes no abbreviated options. SIGTERM and SIGHUP stop
    it as Ctrl-C does, after a detach. While it detaches, Ctrl-C, SIGTERM and
    SIGHUP are ignored, and a detach that was cut off anyway is sent again
    until the probe has answered it, never after; a signal that gets through
    while the handlers are put back (with the three signals blocked) is
    handled once the detach is over and reported.
23. `monitor reset` sets the hardware breakpoints GDB holds again after the
    reset's attach, which clears every trigger, and asks the user to run
    `flushregs`: GDB keeps its register cache across a monitor command.
24. The host waits for each command's reply for as long as the probe may
    need at worst: 3 s for most, and for `DBG_ATTACH`, `DBG_HALT`,
    `DBG_STEP`, `DBG_STATUS` and `DBG_DETACH`, which can sync with the CPU,
    the worst case of the sync's loops at the session's SBW speed (about
    20 s per sync at 250 kHz) with a margin. `DBG_ATTACH` counts both of its
    syncs (the one that ends an open session and its own) at the slower of
    its own speed and that session's; the first attach of a process does not
    know the speed of a session an earlier process left open and counts with
    100 kHz.

## Corrections made after B1 (2026-11-13)

25. **The 32-bit read-only smoke check.** EEM registers are read and written
    with 32-bit DR scans, which had never run on hardware: Phase A's longest
    port call was 25 cycles, while a 32-bit scan (37 cycles) is the first
    call of four PIO words and the first pause between two port calls inside
    one scan. The first attach on the user's board would write EEM registers
    through that path, so `DBG_EEM_PEEK` (0x13, in the table above; protocol
    version and feature bits unchanged) checks it first without writing
    anything. It is accepted in Idle only (`BAD_STATE` elsewhere, nothing
    sent) and leaves the session Idle; `u16 speed_khz` must be 100–10000 (1000 before correction 29) and
    `u8 eem_addr` even and 0x00–0x3E (the value, control, mask and
    combination registers of trigger blocks 0–7, whose reads have no side
    effects), else `BAD_ARG` with nothing sent. It runs `Cpuxv2::identify()`
    with the normal entry as `DBG_ATTACH` does before its sync, selects
    `IR_EMEX_DATA_EXCHANGE32`, reads the register twice with
    `EemDataExchangeXv2`'s read framing (the address with the read bit set,
    then 32 bits out; as `SyncJtag_Conditional_SaveContextXv2` reads trigger
    block 0 when it attaches), shifts `IR_CNTRL_SIG_CAPTURE` once more, and
    releases the pins as a failed identify does (`Tap::close()`: the PIO
    state machine is stopped where it waits with SBWTCK high, then both pins
    go high-impedance with the idle pulls; nothing more is clocked). No TCLK,
    no control-signal write, no EEM register or control write, no memory
    access: the CPU keeps running untouched. The JTAG ID captured by the IR
    scans in front of and after the reads must be 0x91, else `TARGET_ERROR`,
    so the reply also says whether the link survived the 32-bit scans. The
    identify phase's failures are reported as `DBG_ATTACH` reports them
    (`NO_TARGET` with the ID, `LOCKED`, `BUSY`, `TARGET_ERROR`).
    `sbw.py eem-peek [--addr 0x00] [--speed 250]` prints
    `JTAG ID 0x91; EEM 0x00: 0x… 0x… (equal)` and exits 1 when the two
    values differ. The pause between the two port calls of a 32-bit scan is
    electrically the pause between two scans that Phase A ran: in both the
    PIO program waits at `pull block side 1`, SBWTCK driven high, SBWTDIO
    driven at the last TDI level, and the next slot is a TMS slot. Only the
    TAP state (Shift-DR instead of Run-Test/Idle) and the level held on
    SBWTDIO (data bit 3 of the scan instead of the TCLK level) differ; SBW
    limits only the low phase of SBWTCK.
26. **A detach that cannot restore the context resets the target.** When
    the link check before the restore of a detach with action 0 fails twice
    in a row (correction 20; the check is made a second time so that one bad
    capture on a healthy link does not cost a reset), the CPU's context is
    saved: it has been fed
    instructions, holds the watchdog (WDTHOLD) and has GIE cleared, and its
    PC is wherever JTAG left it (0x0004 after a register read's safe-PC
    step, for example). Let go there, it would hang until a reset if only
    the link's reads had failed while its writes still arrived. So it gets
    a power-on reset instead, as with action 1 (EEM off,
    `ReleaseDevice_430Xv2(V_RESET)`: control-signal writes with the R/W bit
    on read and no TCLK, so nothing is written to memory and no instruction
    is fed), and the firmware restarts. The same applies wherever the probe
    detaches with action 0: `Session::abort` after a memory, register or
    breakpoint access returned `TARGET_ERROR`, and an attach whose final
    check fails after the sync: so an attach without reset can, as its one
    exception, end in a reset, but only when the link has failed after the
    context was saved, never on a healthy link. `DBG_DETACH` then answers
    `TARGET_ERROR`, as it does when the halt for the detach of a running CPU
    fails (the CPU is released where it was); the session is over either
    way. The host does not report "the program continues where it stopped"
    then: `sbw.py gdbserver` prints that the probe either could not restore
    the context and reset the target (its firmware restarts), or the halt
    for the detach failed and the CPU was released where it was; a failed
    `DBG_ATTACH` is worded the same way (let go where it was, or reset if
    the link failed after the context was saved). `sbw.py gdbserver` also
    ends a debug session an earlier process left open with `DBG_DETACH 0`
    before it serves, and reports which of the two happened.
27. **The bootcode area is read with TI's quick read (found on the board).**
    Task 25 Step 5 stopped because the GDB server refused the device: on the
    user's F5529 the debugger's address-register read (`ReadMemWordXv2`) of
    the bootcode area 0x1A00–0x1AFF, the TLV with the device descriptor,
    returned 0x3FFF for every word, so the attach reported device ID 0x3FFF
    (the pointer 0x1A00 was right). A read-only diagnostic on the board
    (attach without reset, `READ_MEM`, compared with the backups) found
    information memory, main flash, the reset vector, RAM and the
    peripheral registers read right. TI's DLL never reads that area with
    `ReadMemWordsXv2`: `BootcodeRomAccess::doRead` uses `ID_ReadMemQuick`
    (`SpecialMemoryTypes.cpp:63-66`; the F5xx device database maps 0x1A00,
    size 0x100, to it, `MSP430F5xxx.h:77-82`), and the device descriptor is
    read with `ID_ReadMemQuickXv2` after the conditional sync
    (`DeviceHandleMSP430.cpp:507`; `ReadMemWordsXv2` only for the reset
    vector, :450). Phase A's quick read always read it right. The debugger
    now ports `ReadMemQuickXv2` (JTAG ID 0x91: the PC set to the address,
    the quick reads, then the PC set to the safe address 0x0004; only the
    CPU's PC moves, the saved context keeps the program's) and uses it for
    the device-ID read of both attach modes and, in `READ_MEM` of a debug
    session, for every word in 0x1A00–0x1AFF; ranges that cross the area's
    edges are split, and the address-register read stays everywhere else.
28. **Memory is read with the quick read too, for speed (2026-11-14).** A
    backup of the board's external SPI flash through the debugger (the
    firmware's own read routine called per 2 KB, the result read from RAM)
    ran at 1.7 KB/s at 250 kHz, 3.1 KB/s at 500 kHz and 5.6 KB/s at
    1000 kHz (measured on the user's F5529): `ReadMemWordXv2` needs about 80
    JTAG cycles a word, `ReadMemQuickXv2` 23. `READ_MEM` of a debug session
    now uses the quick read (correction 27's port) for information memory
    0x1800–0x19FF, the bootcode area, RAM 0x1C00–0x43FF and main flash
    0x4400–0x243FF, and `ReadMemWordsXv2` for peripherals, the BSL area and
    vacant addresses; a range that crosses from one kind to the other is
    split. TI's DLL reads flash (`MainMemoryAccessBase.cpp:65`) and lockable
    RAM (`LockableRamMemoryAccess.cpp:76`) with the quick read on devices
    with `quickMemRead`, as the F5xx (`MSP430F5xxx.h:120`), but plain RAM
    (`RandomMemoryAccess`, `MemoryAreaBase.cpp:231`) word by word: reading
    RAM quickly departs from the DLL. The quick read goes through the CPU's
    fetch path, so it is never used for registers whose read has side
    effects. While the CPU is halted the EEM is off, and a resume clears the
    EEM's stop state (`kClearStop`), so the fetches cannot raise a
    breakpoint; only the CPU's PC moves (to the address, then 0x0004), the
    saved context keeps the program's. Host tests pin the split and the
    transcripts; on the board the change is checked read-only (main flash
    against its image, information memory against the backup, RAM read
    twice and a pattern written to free RAM read back).
29. **The SBW clock may be set up to 10 MHz (2026-11-14).** The limit was
    1 MHz, chosen with the transport, not from the target: the F551x/F552x
    datasheet (JTAG and Spy-Bi-Wire interface) allows an SBW input frequency
    of 0–20 MHz and a low phase of at least 25 ns. `kMaxSpeedKhz`, the
    driver's clamp (`SBW_MAX_SLOT_HZ`), the host tool (`sbw.py --speed`,
    `monitor speed`, `probe.MAX_SPEED_KHZ`) and the fake firmware now accept
    100–10000 kHz; the default stays 250 kHz. 10 MHz keeps half of the
    target's margin and is reachable by `sbw.pio` (8 ticks a slot): 80 MHz
    ticks need a fractional divider of 1.5625 at 125 MHz (1.875 at 150 MHz),
    so a tick lasts one or two system clocks and the shortest low phase
    (4 ticks) is 32 ns (26.7 ns on the RP2350). What a board carries above
    1 MHz depends on its RST/SBWTDIO circuit (pull-up, capacitor) and the
    wiring; it is measured on the board read-only (the EEM peek, debug-mode
    reads of main flash against its image, RAM read twice) before a speed is
    used for writing.
