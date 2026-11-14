# Debugging an MSP430F5529 with GDB (read-only mode)

The probe can debug an MSP430F5529 over Spy-Bi-Wire: halt, resume,
single-step, CPU registers, memory, and up to eight hardware breakpoints.
`sbw.py gdbserver` speaks GDB's remote protocol and drives the probe; the
debugger is `msp430-elf-gdb` from TI's MSP430-GCC package. Wiring and
electrical checks are those of `docs/MSP430_SBW.md`.

**Status.** This is the first debugging version (phase B1). It is implemented
and tested on the host, against a fake probe and a fake target, also with
`msp430-elf-gdb` 9.1 (TI's MSP430-GCC 9.3.1.11), which is where the
statements below about GDB's own behaviour were checked
(`tools/msp430/tests/test_gdb_e2e.py`). It has not run against a real target
yet; the acceptance record in `docs/PROJECT_STATUS.md` (Phase 7) says what has
been checked.

This version has one mode, **read-only**. It is meant for boards whose
firmware must not change.

## What read-only means

Flash (main flash, information memory, bootloader) is never written for you.
Three layers refuse it independently:

1. GDB is told that flash is read-only memory (the memory map marks it
   `rom`), so it does not write there: it refuses such writes and `load`
   itself, without asking the server. It also refuses reads and writes
   outside the map.
2. The server refuses every write outside RAM and the permitted peripheral
   registers, every PC change outside main flash, software breakpoints and
   `load`, before anything is sent to the probe.
3. The probe firmware refuses the same writes again (status `PROTECTED`), and
   inside a debug session also its erase and flash-write operations.

What can be written:

- RAM, 0x1C00-0x43FF (USB buffer RAM included);
- peripheral registers, 0x0100-0x093F (`SFRIE1` to `USBFUNADR`; the vacant
  0x0000-0x00FF and 0x0940-0x0FFF are not written), except four blocks: the
  power management module PMM (0x0120-0x013F), the flash controller
  (0x0140-0x014F), the system module SYS (0x0180-0x019F) and the DMA
  controller (0x0500-0x053F). A byte write to `PMMCTL0` resets the chip,
  `SYSCTL` can move the interrupt vectors into RAM, and a DMA transfer could
  write the flash controller.

The PC may only be set to an address in main flash (0x4400-0x243FF), or left
where it is. The bootloader area (0x1000-0x17FF) is neither read nor written.

What it does not guarantee: the firmware under debug runs normally when
resumed, and may write its own information memory. Code you place by other
means, such as a changed return address on the stack, runs too. A device the
probe does not know (anything but the F5529) is refused every write and every
PC change.

## Starting

```bash
python3 tools/msp430/sbw.py gdbserver            # attach to the running program
python3 tools/msp430/sbw.py gdbserver --reset    # reset, halt at the reset vector
```

Options (`python3 tools/msp430/sbw.py gdbserver --help`):

| Option | Meaning |
|---|---|
| `--port PORT` | TCP port on 127.0.0.1, default 3333; 0 picks a free one. The server never listens on another address. |
| `--attach` | Attach to the running program without reset (the default). |
| `--reset` | Reset the target and halt at the reset vector each time GDB connects. Excludes `--attach`. |
| `--speed KHZ` | SBW clock, 100-1000, default 250. The global `--speed` before the command works too. |

`--allow-flash-write` belongs to a later version and is refused. Abbreviated
options are not accepted either.

The server waits for GDB, attaches when GDB connects and detaches when GDB
leaves. It prints the lines to type into GDB. In a second terminal:

```
msp430-elf-gdb -nx symbols.elf
(gdb) set remotetimeout 10
(gdb) set remote hardware-breakpoint-limit 8
(gdb) target remote :3333
```

`symbols.elf` is the file `symelf.py` writes (next section), or your own
build's ELF file. The longer `remotetimeout` is for the attach, which can take
longer than GDB's default of two seconds.

Attaching without reset stops the program where it is. On a working link
nothing is reset, and the target's other state (watchdog setting, low-power
mode, peripherals) is kept as TI's debug stack keeps it. When GDB detaches
(`detach`, `kill`, `quit`, or the connection drops) the server detaches too
and the program continues where it stopped, without a reset.

The one exception, for an attach without reset as for a detach: if the link
fails after the probe has saved the CPU's context (its link check fails twice
in a row), the probe cannot give the program its context back and resets the
target instead, so that the firmware restarts rather than hangs; the server
says so (Troubleshooting). If an earlier run left a debug session open on the
probe (the tool was killed, for example), the server ends it before it waits
for GDB and says whether that program continued or the target was reset.

Stop the server with Ctrl-C;
SIGTERM and SIGHUP do the same. It detaches first, and exits with status 130
(Ctrl-C) or 128 plus the signal number. While it detaches, these three
signals are ignored, so that nothing cuts the detach off; one that arrives
just as the detach ends takes effect once it is over.

**Do not unplug the probe, power it off, or kill the host tool without a
detach while the target runs with breakpoints set.** If the link dies while
the target stays powered, the breakpoints stay armed in the chip's emulation
module (EEM) and the firmware stops at the next one with no debugger to
release it. Only a reset or a power cycle of the target frees it; flash is
not affected. The same holds for TI's own tools. The normal ways out
(`detach` or `quit` in GDB, Ctrl-C in the host tool) switch the EEM off and
release the CPU.

Do not run SWD tools against the probe while an MSP430 is attached: they
drive the same two pins. Do not run another `sbw.py` command while GDB
is attached either: every other command begins by closing the open session,
which detaches the debugger, and resets the target. A second `gdbserver` on
the same port fails ("choose another with --port"). On another port it
leaves the first session alone until GDB connects to it; then it takes the
target over (the attach detaches the old session, and the program runs on
until the new attach stops it) and resets it only with `--reset`. If the probe still holds a programming session from
an earlier `sbw.py` command that was killed, `gdbserver` says so and does not
attach: unplug and replug the probe.

## Symbols for firmware without an ELF file

GDB needs an ELF file to know the MSP430X architecture (20-bit addresses) and
the symbol names. For firmware that exists only as a flash image,
`symelf.py` writes a symbol-only ELF file from name tables:

```bash
python3 tools/msp430/symelf.py names.tsv -o symbols.elf
python3 tools/msp430/symelf.py names.tsv more.tsv -o symbols.elf --image firmware.hex --force
```

Each table is tab-separated UTF-8 text with a header row; blank lines and
lines starting with `#` are skipped, quotes are ordinary characters.
Columns: `address` (1-5 hex digits, with or without `0x`; `-` skips the row),
`new_name` (or `name`), and optionally `kind` (`func`, the default; `var`,
`const`, `data`; `struct`, `union`, `member` and `type` rows are skipped) and
`size` (decimal or `0x` hex). Functions must be at even addresses, and a name
may appear only once (an exact repeat of a row counts once). A table with an
error is refused with a message that names the row; for a duplicate name it
names the symbol and both definitions, for a missing column only the file. For example:

```
address	new_name	kind	size
0x4400	demo_start	func	0x20
0x4420	demo_blink	func	0x18
0x2400	demo_counter	var	2
0x4500	demo_table	const	16
```

Main flash becomes the `.text` section and RAM the `.bss` section; with
`--image` (Intel HEX or TI-TXT) `.text` also holds the image's main-flash
bytes, so that `disassemble` works before a target is attached. `--force`
replaces an existing output file. Keep tables and images of third-party
firmware outside the repository.

The ELF file only names addresses; it has no types and no line numbers:

- GDB classes a symbol by the section it lies in, so a constant in flash is a
  text symbol. `print` asks for a cast: `print *(unsigned short *)&demo_table`
  or `x/hx &demo_table`. No symbol has a type.
- `break NAME` stops after the function's prologue, not at its first
  instruction: without line numbers GDB reads the function's first
  instructions and skips those that set up its frame, such as a `PUSHM`. For
  a function at 0x4680 that begins with `PUSHM.A #2,R10`, GDB answers
  `Breakpoint 1 at 0x4682`. `break *NAME` stops at the first instruction. In
  flash both are hardware breakpoints, which GDB chooses by itself.
- A function in RAM and an absolute symbol (one in neither flash nor RAM)
  cannot be used with `break NAME`; use `hbreak *ADDRESS`.
- `info symbol ADDRESS` answers only for addresses in flash or RAM.
- Never `load` the symbol ELF. With `--image` it holds 128 KiB of flash
  contents, and a later full mode would write them. In read-only mode GDB
  refuses to write into memory its map marks as ROM and stops with
  `Load failed`. Without `--image` there is nothing to load (`load size 0`),
  and the server refuses GDB's move of the PC to the entry address 0
  (`Could not write register ""`). Nothing is written either way.

## Using GDB

| Task | GDB |
|---|---|
| Registers | `info registers`, `p $r12`, `set var $r12 = 5` |
| Memory | `x/16xh 0x2400`, `x/hx &demo_counter` |
| Stop a running program | Ctrl-C |
| Continue | `continue` |
| One instruction | `stepi`, `nexti` |
| Breakpoint | `break *demo_blink` (its first instruction), `break demo_blink` (after its prologue), `hbreak *0x4420` |
| Leave, program keeps running | `detach` |

- **Registers** are GDB's own layout for the MSP430: R0 (PC), R1 (SP), R2
  (SR), R3 (CG, always 0 and never written) and R4-R15, 20 bits wide (SR 16).
  The server sends no target description. In GDB their names are `$pc`,
  `$sp`, `$sr`, `$cg` and `$r4`-`$r15`; `$r0`-`$r3` are not registers (GDB
  prints `void` for them).
- **Breakpoints** are the chip's eight EEM triggers, set only while the CPU
  is halted and armed when it resumes. GDB's `next`, `finish` and `until`
  need one trigger each for their temporary breakpoints; with all eight in
  use they fail.
- **Software breakpoints** are refused. GDB sets a hardware breakpoint by
  itself for code in memory its map marks as ROM (flash), and says
  "automatically using hardware breakpoints for read-only addresses". A
  `break` on an address in RAM asks for a software one, so use `hbreak`
  there. GDB reports the refusal when it resumes (`Cannot insert breakpoint
  N.`) and does not resume.
- **Source-level stepping** needs debug information that matches the flash;
  for firmware without it, step by instruction.
- **Byte writes** to RAM or peripherals are word read-modify-writes. A byte
  write whose neighbouring byte is a register with a read side effect (below)
  is refused; write the whole word.
- **Single steps** end at the first instruction of an interrupt handler when
  interrupts are enabled and one is pending, as with TI's tools. The
  watchdog runs with the firmware's own setting during each step, so many
  steps in a row with the watchdog enabled can end in a watchdog reset, again
  as with TI's tools; `continue` to a breakpoint instead. A step over an
  instruction that enters a low-power mode ends with a note that the CPU is
  halted there; a step that does not complete in time stops the CPU and says
  so. Ctrl-C during `stepi N`, or during `next` through code without line
  information (GDB steps it one instruction at a time), stops after the
  step under way.

### Reads with side effects

GDB reads memory on its own (stack unwinding, caches), so a stray read of an
interrupt vector register would clear a flag the firmware is waiting for. The
server therefore refuses reads of these registers (the list is
`read_side_effects` in `tools/msp430/devices.py`), and its message names the
`monitor read` command that reads one on purpose. What to read instead:

| Register | A read clears | Read instead |
|---|---|---|
| `SYSJMBI0`, `SYSJMBI1` | the mailbox flags JMBINxFG | `SYSJMBC` |
| `SYSBERRIV`, `SYSUNIV`, `SYSSNIV`, `SYSRSTIV` | the highest pending flag | `SFRIFG1`, `PMMIFG`, `FCTL3` (ACCVIFG, KEYV; readable); some sources have no other flag |
| `P1IV`, `P2IV` | the highest pending flag | `P1IFG`, `P2IFG` |
| `TA0IV`, `TA1IV`, `TA2IV`, `TB0IV` | the highest pending flag | `TAxCTL` (TAIFG), `TAxCCTLn` (CCIFG), likewise `TB0CTL`, `TB0CCTLn` |
| `RTCIV` | the highest pending flag | `RTCCTL0`, `RTCPS0CTL`, `RTCPS1CTL` |
| `DMAIV` | the highest pending flag | `DMAxCTL` (DMAIFG) |
| `UCA0RXBUF` ... `UCB1RXBUF` | UCRXIFG and the receive error flags | `UCAxIFG`, `UCBxIFG`, `UCAxSTAT`, `UCBxSTAT` for the error flags (the byte itself cannot be read without consuming it) |
| `UCA0IV` ... `UCB1IV` | the highest pending flag | `UCAxIFG`, `UCBxIFG` |
| `ADC12IV` | ADC12OV or ADC12TOV | none: ADC12OV and ADC12TOV have no other flag |
| `ADC12MEM0` ... `ADC12MEM15` | ADC12IFGx | `ADC12IFG` |
| `CBIV` | the highest pending flag | `CBINT` |
| `USBVECINT` | the highest pending flag | `USBIFG`, `USBIEPIFG`, `USBOEPIFG`, `USBPWRCTL`, `USBPLLIR` (and a timestamp source) |

`monitor read ADDR [WORDS]` reads one on purpose, and says which side effect
it had.

### Memory the server does not show

GDB's memory map leaves out vacant memory (0x0000-0x00FF, 0x0940-0x0FFF,
0x1B00-0x1BFF), the bootloader area (0x1000-0x17FF) and everything above
main flash. GDB refuses reads and writes there itself, with `Cannot access
memory at address ...`, and does not ask the server. `monitor read` gives
the server's reason ("outside the target's memory map", or "the BSL area ...
is never read"); so does the server's console after `set mem
inaccessible-by-default off`, which makes GDB ask. Reading the USB buffer RAM
(0x1C00-0x23FF) while the USB module is suspended (PLL off) may cause a bus
error, according to the comments in TI's USB API.

## Monitor commands

| Command | Effect |
|---|---|
| `monitor help` | list the commands |
| `monitor reset` | reset the target and halt at the reset vector; then run `flushregs` so GDB reads the registers again (GDB keeps them across a monitor command). Newer GDB versions also take `maintenance flush register-cache`; GDB 9 does not have it and runs `maintenance flush-symbol-cache` for it, which leaves the registers alone. The program continues for a moment first: the open session is detached before the new attach. Breakpoints stay set: GDB inserts them again when it resumes, and the server sets again those GDB keeps inserted (`set breakpoint always-inserted on`); one it cannot set again is named in the reply (delete it and set it again). |
| `monitor read ADDR [WORDS]` | read 1-256 words (default 1) from an even address, including registers with side effects |
| `monitor status` | device, mode, speed, state, hardware breakpoints in use |
| `monitor speed KHZ` | SBW clock, 100-1000, for the next attach (`monitor reset` or the next connection) |
| `monitor mode readonly` | the only mode of this version; `monitor mode full` says it is not available |

## While the CPU is halted

- The watchdog is held, and gets the firmware's own setting back on resume
  and detach. A read of `WDTCTL` (0x015C) returns the saved low byte with the
  read password 0x69; a write is saved for the release and the watchdog stays
  held while halted.
- MCLK, SMCLK and ACLK stop (EEM clock control 0x000E, the value of TI's Bios
  macros). The attach with `--reset` writes it, and so do the attach and the
  halt when they stop an awake, running CPU; an attach to a CPU in a
  low-power mode or one that is already stopped leaves the clocks as they
  were. Timers clocked from them stop too. TI's
  stack also sets per-module clock control from its device database; this
  port leaves that out, so the modules keep their reset defaults. On the
  acceptance board (2026-11-13) Timer0_B's counter stayed constant over one
  second while halted, but every one of those stops found the CPU in LPM4,
  where its clock is off anyway; whether it runs while a CPU halted outside a
  low-power mode is not measured (Phase 7 in `docs/PROJECT_STATUS.md`).
- A CPU in a low-power mode is taken over without waking it; its status
  register is restored on resume.
- The probe answers memory and register requests only while the CPU is
  halted. While the target runs, stop it first (Ctrl-C in GDB).

## Troubleshooting

The server prints what it refused on its console; GDB shows only a generic
error for the same packet: `Cannot access memory at address ...` for memory,
`Could not write register ""; remote failure reply 'E01'` for a register
(this GDB's raw registers have empty names), `Cannot insert breakpoint N.`
for a breakpoint. What GDB's memory map rules out, GDB refuses itself with
the same `Cannot access memory` and nothing on the server's console: writes
into flash, information memory and the device descriptor (`rom`), reads and
writes of the bootloader area, vacant memory and everything above main
flash, and `load` (`Load failed`).

| Message | Meaning |
|---|---|
| `refused: write of ... read-only mode writes only RAM and peripheral registers, not ...` | One of the four peripheral blocks. (Flash, the bootloader, vacant memory and the gap 0x1B00-0x1BFF are refused by GDB itself.) With `set mem inaccessible-by-default off`, GDB sends writes outside its map too: those to vacant memory (0x0000-0x00FF, 0x0940-0x0FFF), the bootloader area, the gap and everything above main flash get this message. |
| `refused: read-only mode: the PC may only be set inside main flash ...` | `set var $pc = ...` or `jump ADDR` to an address outside main flash, or `load` of a symbol ELF without `--image` (entry address 0). |
| `refused: software breakpoints are not allowed in read-only mode ...` | Use `hbreak`. |
| `refused: all 8 hardware breakpoints are in use` | Delete a breakpoint. |
| `refused: flash programming (load) is not allowed in read-only mode` | `load` is not available in this version. With this server's memory map GDB refuses `load` itself (`Load failed`) and sends nothing; this message is for clients that program flash anyway. |
| `refused: read of ... outside the target's memory map` | Vacant memory and everything outside the map are not read. Seen after `monitor read`, or with `set mem inaccessible-by-default off`. |
| `refused: read of ... the BSL area ... is never read` | The bootloader area is not read. Seen as the previous row. |
| `refused: read of NAME (...) clears ...` | See "Reads with side effects"; use `monitor read`. |
| `refused: byte write at ... would read ...; write the whole word` | See "Using GDB", byte writes. |
| `target lost (...); the debug session has ended` | The target was reset, lost power or the wires came off, or the link failed. The probe has ended the session; if the link failed while the CPU was halted, it may have reset the target (its firmware restarts) because it could not give the program its context back. While it ran, GDB says `Program terminated with signal SIGLOST, Resource lost.` The session is over (`detach` says `The program is not being run.`); start `target remote` again. |
| `detached, but the program does not continue where it stopped: ...` | The detach could not give the program its context back: the link failed its check twice, so the probe reset the target and its firmware restarts from the reset vector (it is not left halted or hanging); or the halt for the detach failed, and the CPU was released where it was and keeps running (if the target still has power). Check the wiring and the target supply before attaching again. |
| `DBG_ATTACH failed: TARGET_ERROR (the attach lost synchronization with the CPU: ...)` | The attach failed. The probe let the CPU go where it was; only if the link failed after it had saved the CPU's context (or during an attach with reset) did it reset the target. |
| `unexpected device: ID bytes ...` | The target is not an MSP430F5529. The target was left running. |
| `no MSP430 found (JTAG ID read as 0x..)` | See `docs/MSP430_SBW.md`, Troubleshooting; try a lower `--speed`. |
| `the device's JTAG lock key is programmed; it cannot be debugged` | The device is locked. |
| `the probe firmware does not support debugging` | Flash the probe with a firmware built from this version. |
| `the probe's SBW protocol version N is not supported by this tool for debugging` | The probe and the tool are from different versions; update the older one. |
| `the probe still holds a programming session from an earlier command` | Unplug and replug the probe. |
| `... choose another with --port` | The port is in use. |

Exit status of `gdbserver`: 1 on failure, 2 for a usage error, 130 after
Ctrl-C, 128 plus the signal number after SIGTERM or SIGHUP.

## How it works

`lib/msp430/src/debug.cpp` ports the CPUXv2 debug sequences of TI's MSP
Debug Stack (MSPDS-OPEN-SOURCE 3.15.1.1): taking a running CPU under JTAG
control and saving its context, reset and halt, register and memory access
that keep the context, single step, release with EEM breakpoints armed, and
stop detection. The deliberate differences from TI's code are listed in the
header of that file. The sequences are exposed as operations 0x09-0x12 of the
CMSIS-DAP vendor command 0x90 (protocol version 2); operation 0x13
(`DBG_EEM_PEEK`, `sbw.py eem-peek`) is a read-only check of the 32-bit scans
to run before the first attach on a board (`docs/MSP430_SBW.md`). `lib/msp430/src/session.cpp`
enforces the write rules above. `tools/msp430/gdbserver.py` translates GDB's
remote protocol into those operations. The design is
`docs/superpowers/specs/2026-11-13-msp430-debug-design.md`; TI's licence
notice is in `THIRD_PARTY_NOTICES.md`.
