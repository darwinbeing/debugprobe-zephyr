# Programming an MSP430F5529 over Spy-Bi-Wire

The probe can erase, program, verify and read an MSP430F5529 through its
two-wire Spy-Bi-Wire (SBW) interface. It uses the same two pins as SWD and the
same USB interface. SWD/CMSIS-DAP debugging works as before; the only visible
difference is the idle state of the two pins (see "How it works"). Debugging
MSP430 targets with GDB is described in `docs/MSP430_DEBUG.md`.

**`flash` and `erase` destroy what is in the target's flash. Before the first
write to a board, follow "First use on a board" below and make a backup you
have checked.**

## Wiring

| Probe board | SBWTCK → target TEST | SBWTDIO ↔ target RST | Ground |
|---|---|---|---|
| Raspberry Pi Pico / Pico 2 | GP2 | GP3 | any GND pin |
| Raspberry Pi Debug Probe | "D" connector SC | "D" connector SD | "D" connector GND |

Keep the wires short (10–20 cm). The target must be powered by its own supply.

## Check before you connect

1. **Measure the target's VCC.** The probe drives 3.3 V. If the target runs
   below 3.0 V, do not connect it directly; the MSP430's pins tolerate at most
   VCC + 0.3 V.
2. **Look at the RST pin circuit.** TI recommends about 47 kΩ to VCC and at
   most 2.2 nF to ground. A larger capacitor slows the data line; lower the
   speed (`--speed 100`), and if that is not enough, the capacitor has to go.
3. **Power the target before you run a command.** The probe drives RST high
   from the moment it takes the pins.
4. **Do not leave an unpowered probe attached to a powered target.** The
   RP2040's protection diodes then load RST and TEST and can hold the target
   in reset.
5. **Do not run SWD tools (openocd, probe-rs) against the probe while an
   MSP430 is attached.** They drive the same two pins.

## Commands

```bash
pip install -r tools/msp430/requirements.txt
alias sbw="python3 $PWD/tools/msp430/sbw.py"   # from the repository root

sbw info                              # identify the target
sbw selftest                          # check the read and write paths using the target's RAM
sbw read 0x4400 0x20000 -o main.hex   # dump memory (here: all of main flash)
sbw trial --backup main.hex           # prove erase/program/verify on one blank segment
sbw verify firmware.hex               # compare flash with an image
sbw reset                             # reset the target and let it run
sbw eem-peek                          # read-only check of the 32-bit SBW scans (no reset)
```

The two commands that destroy flash contents:

```bash
sbw flash firmware.hex                # erases ALL main flash, programs, verifies, resets
sbw erase                             # erases ALL main flash
```

Global options go before the command: `--speed KHZ` sets the SBW clock
(100–10000 kHz, default 250; 500 kHz is the rate of TI's reference
implementation), `--serial SERIAL` selects one of several probes. Above
1000 kHz the limit is usually the target board's RST/SBWTDIO circuit (pull-up,
capacitor, wire length), not the MSP430 (up to 20 MHz): try a speed with a
read-only `sbw --speed KHZ verify image.hex` before writing at it. A speed
the line cannot carry usually shows at once as "no MSP430 found (JTAG ID read
as 0x..)"; on the board used in testing, 1400 kHz worked and 1600 kHz did not.
Leave a margin below the highest speed that works: above 1 MHz the gain is
small, because USB round trips dominate.

Images are Intel HEX or TI-TXT. Convert an ELF file with
`msp430-elf-objcopy -O ihex firmware.elf firmware.hex`.

Details:

- `flash --erase segments` erases only the 512-byte segments the image
  touches; `--erase none` erases nothing. `--no-verify` skips the read-back.
- `erase --segment ADDR` erases only the segment that contains ADDR.
- The image file is read and checked before the probe touches the target. An
  image with data outside the programmable flash is rejected before anything
  is erased.
- Information segments B, C and D (`0x1800`–`0x197F`) are erased and written
  only when the image contains data for them. Information segment A and the
  bootloader area (`0x1000`–`0x17FF`) are never written.
- `verify` and the read-back after `flash` compare exactly the bytes that are
  in the image file.
- `read` writes Intel HEX, or raw bytes if the file name ends in `.bin`. It
  refuses to replace an existing file unless you pass `--force`, writes the
  file only after the whole read succeeded, and prints the SHA-256 of the data.
- Do not use `read` on the bootloader area (`0x1000`–`0x17FF`). The device
  normally protects it; on an F5529 a read there returned zeros and then
  different data, without an error.
- **Every command resets the target**, including `info` and `read`: taking
  control of the CPU involves a power-on reset, and releasing it a JTAG reset.
  The exceptions are `gdbserver`, which attaches without reset unless given
  `--reset` (`docs/MSP430_DEBUG.md`), and `eem-peek` (below), which never
  takes control of the CPU. Any other command closes a debug session that is
  open, which detaches the debugger.

`sbw eem-peek [--addr 0x00] [--speed KHZ]` is a read-only smoke test of the
32-bit JTAG scans that debugging uses for the EEM (the breakpoint logic), to
run on a board before its first `gdbserver` attach. It sends vendor operation
`DBG_EEM_PEEK` (0x13; arguments `u16 speed_khz`, `u8 eem_addr`; reply
`u8 jtag_id`, `u32 first`, `u32 second`; protocol version 2), which the probe
accepts only when no programming or debug session is open (`BAD_STATE`
otherwise). The probe enters SBW with RST high and reads the JTAG ID, the
lock state, the core ID and the device-ID pointer exactly as a debug attach
does before it synchronizes with the CPU; then it reads one EEM register twice
through the 32-bit data exchange, the way TI's MSP Debug Stack reads EEM
registers, reads the JTAG ID once more, and releases the pins. The CPU is not
stopped or reset, gets no JTAG clock (TCLK), and nothing is written, neither
memory nor an EEM register. `--addr` must be even, 0x00–0x3E: the value,
control, mask and combination registers of trigger blocks 0–7, whose reads
have no side effects. The tool prints `JTAG ID 0x91; EEM 0x00: 0x00000000
0x00000000 (equal)` and exits 1 if the two values differ. `TARGET_ERROR`
means the JTAG ID was wrong before or after the 32-bit reads, that is, the
link did not survive them; do not attach a debugger until that is
understood. The check is that the two reads agree; the value itself is
whatever the trigger block holds (an earlier debug session may have set it).

Exit status: 0 on success, 1 on failure, 2 for a usage error, 130 when
interrupted with Ctrl-C.

## First use on a board

Do these in order. Nothing before step 6 changes the firmware that is on the
board, and each step checks something the later ones rely on.

1. **`sbw info`** must report `MSP430F5529`, JTAG ID `0x91` and device ID
   bytes `55h 29h`. If the tool reports an unsupported device with other ID
   bytes, stop: either the target is a different chip or the probe is not
   reading correctly. Do not edit the device table to make it pass.
2. **`sbw selftest`** writes known data to the target's RAM and reads it
   back, and re-reads the device ID inside a larger block. It proves that the
   probe writes and reads the right data at the right addresses. Flash is not
   touched.
3. **Back up main flash twice, at two speeds, to two files,** in a directory
   outside the repository (a `git clean` would delete them otherwise):

   ```bash
   mkdir -p ~/msp430-backup && cd ~/msp430-backup
   sbw --speed 250 read 0x4400 0x20000 -o main-250.hex
   sbw --speed 100 read 0x4400 0x20000 -o main-100.hex
   cmp main-250.hex main-100.hex && echo identical
   sbw read 0x1800 0x200 -o info.hex
   ```

   The two reads are split into chunks at different addresses, so an error
   tied to chunk boundaries shows up as a difference; the two `sha256:` lines
   must be equal as well. Keep the files read-only and copy them somewhere
   else. (`info.hex` is for your records: it includes information segment A,
   which the tool will not write back.)
4. **`sbw trial --backup main-250.hex`** picks a 512-byte segment that is
   blank in the backup, checks that the target still matches the backup,
   programs a test pattern into that segment, verifies it, erases the segment
   again, and then checks that all of main flash still matches the backup. It
   exercises segment erase, program and verify on real flash while the
   original firmware is intact.
   - If it fails or is interrupted after it started writing, the segment may
     hold part of the test pattern. The message names the segment: run
     `sbw erase --segment 0x…` and then `sbw verify main-250.hex`, and find
     out why it failed before going on.
   - If it reports a difference outside the test segment, write nothing more
     until the cause is known.
   - It needs a blank segment. If the firmware fills the flash completely,
     skip this step and accept that step 6 is the first test of the write
     path.
5. **Know how you would recover** if step 6 goes wrong. This tool never touches
   the F5529's USB bootloader, so TI's BSL tools can program `main-250.hex`
   back if the board exposes USB and the BSL entry; otherwise a TI programmer
   (MSP-FET, or the eZ-FET on a LaunchPad) can.
6. **`sbw flash firmware.hex`**, at the speed that worked so far. This is
   also the first use of the mass erase, which cannot be tried out without
   erasing. If the verify fails, put the backup back with
   `sbw flash --erase segments main-250.hex`: it uses only the segment erase
   that step 4 proved, and takes a few minutes.

Stay at 250 kHz (or 100) until a flash and verify have succeeded once; try
higher speeds afterwards.

## Speed

Measured on an MSP430F5529 with a Raspberry Pi Pico as the probe and a 128 KB
image:

| `--speed` | Programming | Read-back | `flash` of 128 KB, with verify |
|---|---|---|---|
| 100 kHz | not measured | 2.1 kB/s | not measured |
| 250 kHz (default) | 1.7 kB/s | 5.4 kB/s | about 100 s |
| 500 kHz | 3.3 kB/s | about 10 kB/s | about 55 s |
| 1000 kHz | 6.0 kB/s | about 18 kB/s | about 30 s |

While a command runs, the probe's UART bridge can pause for up to about half a
second at a time (longer at 100 kHz). The target is halted during that time
anyway.

## If a run is interrupted

Ctrl-C closes the session before the tool exits. If the tool is killed, or the
host crashes, the target stays halted under the probe's control and the probe
keeps the two pins. Run any command again (for example `reset`): each command
starts by closing whatever session was left open (`gdbserver` is the
exception: it refuses a leftover programming session; unplug and replug the
probe). Starting an SWD debugger or
replugging the probe has the same effect.

## Troubleshooting

| Message | Likely cause |
|---|---|
| `no MSP430 found (JTAG ID read as 0xFF)` | The data line stayed high: nothing connected, or TEST/RST swapped. |
| `no MSP430 found (JTAG ID read as 0x..)` | Marginal signal: shorten the wires, check the RST capacitor, lower `--speed`. |
| `the device's JTAG lock key is programmed` | The device is locked. This tool does not unlock devices. |
| `unsupported device: ID bytes ..` | The target is not an MSP430F5529, or the read path is not reliable. Stop and find out which. |
| `verify failed at 0x…` | The flash was not erased first (`--erase none`), or the signal is marginal: lower `--speed`. |
| `selftest: ...` | The probe does not read back what it wrote. Do not write to flash; check wiring, the RST capacitor and the speed. |
| `TARGET_ERROR (lost synchronization with the CPU)` | The JTAG link broke during the command (loose wire, target reset or power loss, signal too marginal for the speed). The probe has closed the session; run the command again. |
| `TIMEOUT (no answer from the target)` | The flash routine on the target did not answer. Run the command again at a lower `--speed`. |
| `BUSY (the SWD port is in use)` | Another program is using the probe for SWD at the same time. |
| `the probe's replies are out of step with the requests` | An earlier run was interrupted. Run the command again; if it keeps happening, replug the probe. |
| `the probe does not answer the MSP430 SBW command` | The probe firmware is older than this feature or was built with `CONFIG_PROBE_MSP430_SBW=n`. |
| `pyusb is not installed` / `libusb was not found` | Install the tool's requirements; on macOS also `brew install libusb`. |

The probe firmware logs its own errors (prefix `sbw_pio`) on its console UART.

## How it works

`drivers/sbw_pio` clocks JTAG cycles onto the two wires with a PIO program on
PIO1. `lib/msp430` implements the JTAG sequences of TI's SLAU320 reference
(`Replicator430Xv2`) and exposes them through CMSIS-DAP vendor command `0x90`.
Flash is erased and written by TI's RAM funclets, which run on the target and
exchange data through the JTAG mailbox.

Outside a session the two pins are high-impedance, with a pull-up on the data
pin and a pull-down on the clock pin. That is also their state after power-up
and after an SWD debugger disconnects; an SWD connect takes the pins over.
Before this feature the data pin had a pull-down until the first SWD connect,
and the pins stayed driven after an SWD disconnect.

The design is described in
`docs/superpowers/specs/2026-11-11-msp430-sbw-design.md`. TI's licence notice
for the ported code is in `THIRD_PARTY_NOTICES.md`.
