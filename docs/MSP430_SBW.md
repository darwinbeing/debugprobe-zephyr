# Programming an MSP430F5529 over Spy-Bi-Wire

The probe can erase, program, verify and read an MSP430F5529 through its
two-wire Spy-Bi-Wire (SBW) interface. It uses the same two pins as SWD and the
same USB interface; the SWD/CMSIS-DAP function is unchanged. Debugging
(breakpoints, single-step) is not implemented yet.

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

## Usage

```bash
pip install -r tools/msp430/requirements.txt

python3 tools/msp430/sbw.py info
python3 tools/msp430/sbw.py read 0x4400 0x20000 -o backup.hex   # back up main flash
python3 tools/msp430/sbw.py flash firmware.hex
python3 tools/msp430/sbw.py verify firmware.hex
python3 tools/msp430/sbw.py erase
python3 tools/msp430/sbw.py reset
```

Images are Intel HEX or TI-TXT. Convert an ELF file with
`msp430-elf-objcopy -O ihex firmware.elf firmware.hex`.

`flash` erases all main flash (`0x4400`–`0x243FF`), programs the image, reads
it back and resets the target. Options:

- `--erase segments` erases only the 512-byte segments the image touches;
  `--erase none` erases nothing.
- `--no-verify` skips the read-back.
- `--speed KHZ` (before the command) sets the SBW clock, 100–1000 kHz,
  default 250. 500 kHz is the rate of TI's reference implementation.
- `--serial SERIAL` selects one of several probes.

The image file is read and checked before the probe touches the target. An
image with data outside the programmable flash is rejected before anything is
erased.

Information segments B, C and D (`0x1800`–`0x197F`) are erased and written only
when the image contains data for them. Information segment A and the bootloader
area (`0x1000`–`0x17FF`) are never written.

`verify` and the read-back after `flash` compare exactly the bytes that are in
the image file.

**Every command resets the target**, including `info` and `read`: taking
control of the CPU involves a power-on reset, and releasing it a JTAG reset.

Exit status: 0 on success, 1 on failure, 2 for a usage error, 130 when
interrupted with Ctrl-C.

### Speed

| `--speed` | Programming | Read-back |
|---|---|---|
| 250 kHz (default) | about 2 kB/s | about 6 kB/s |
| 500 kHz | about 4 kB/s | about 12 kB/s |
| 1000 kHz | about 7 kB/s | about 24 kB/s |

While a command runs, the probe's UART bridge can pause for up to about half a
second at a time (longer at 100 kHz). The target is halted during that time
anyway.

### If a run is interrupted

Ctrl-C closes the session before the tool exits. If the tool is killed, or the
host crashes, the target stays halted under the probe's control and the probe
keeps the two pins. Run any command again (for example `reset`): each command
starts by closing whatever session was left open. Starting an SWD debugger or
replugging the probe has the same effect.

## Troubleshooting

| Message | Likely cause |
|---|---|
| `no MSP430 found (JTAG ID read as 0xFF)` | The data line stayed high: nothing connected, or TEST/RST swapped. |
| `no MSP430 found (JTAG ID read as 0x..)` | Marginal signal: shorten the wires, check the RST capacitor, lower `--speed`. |
| `BUSY (the SWD port is in use)` | An SWD debugger is connected through the same probe. |
| `the device's JTAG lock key is programmed` | The device is locked. This tool does not unlock devices. |
| `unsupported device: ID bytes ..` | The target is not an MSP430F5529. |
| `verify failed at 0x…` | The flash was not erased first (`--erase none`), or the signal is marginal: lower `--speed`. |
| `TARGET_ERROR (lost synchronization with the CPU)` | The JTAG link broke during the command (loose wire, target reset or power loss, signal too marginal for the speed). The probe has closed the session; run the command again. |
| `TIMEOUT (no answer from the target)` | The flash routine on the target did not answer. Run the command again at a lower `--speed`. |
| `the probe's replies are out of step with the requests` | An earlier run was interrupted. Run the command again; if it keeps happening, replug the probe. |
| `the probe does not answer the MSP430 SBW command` | The probe firmware is older than this feature or was built with `CONFIG_PROBE_MSP430_SBW=n`. |
| `pyusb is not installed` / `libusb was not found` | Install the tool's requirements; on macOS also `brew install libusb`. |

The probe firmware logs its own errors (prefix `sbw_pio`) on its console UART.

## How it works

`drivers/sbw_pio` clocks JTAG cycles onto the two wires with a PIO program on
PIO1. `lib/msp430` implements the JTAG sequences of TI's SLAU320 reference
(`Replicator430Xv2`) and exposes them through CMSIS-DAP vendor command `0x90`.
Flash is erased and written by TI's RAM funclets, which run on the target and
exchange data through the JTAG mailbox. Outside a session the two pins are
high-impedance; an SWD connect takes them over and an SWD disconnect hands them
back. The design is described in
`docs/superpowers/specs/2026-11-11-msp430-sbw-design.md`.
