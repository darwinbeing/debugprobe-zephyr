# debugprobe-zephyr

A Zephyr RTOS + C++20 rewrite of the Raspberry Pi Debug Probe firmware.
Design documents live in `docs/superpowers/specs/`.

## Building (reusing an existing Zephyr workspace)

Zephyr 4.4 requires Python >= 3.12 — invoke west through a new-enough
interpreter (on this machine: `python3.14 -m west`).

```bash
export ZEPHYR_TOOLCHAIN_VARIANT=gnuarmemb
export GNUARMEMB_TOOLCHAIN_PATH=$HOME/Developer/zephyrproject/arm-gnu-toolchain-14.2.rel1-darwin-x86_64-arm-none-eabi
cd ~/Developer/zephyrproject
python3.14 -m west build -p always -b rpi_debug_probe -d ~/Developer/debugprobe-zephyr/build/rpi_debug_probe ~/Developer/debugprobe-zephyr
```

Board targets: `rpi_debug_probe`, `rpi_pico`, `rpi_pico2/rp2350a/m33`.
Build all three at once: `scripts/build_all.sh`.
Artifacts: `build/<board>/zephyr/zephyr.uf2` — drag onto the BOOTSEL drive to flash.

## MSP430 Spy-Bi-Wire programming

The firmware can also program an MSP430F5529 over Spy-Bi-Wire, using the SWD
pins and a small Python tool:

    python3 tools/msp430/sbw.py flash firmware.hex

`flash` erases the target's main flash first. Before the first write to a
board, read `docs/MSP430_SBW.md`: it covers wiring, electrical checks, and how
to make and check a backup (`selftest`, `read`, `trial`).

## Fresh machine setup (T2 topology)

```bash
mkdir debugprobe-ws && cd debugprobe-ws
west init -m <this repository URL>
west update
```

## Out-of-tree patches

`patches/` holds latent-bug fixes for the pinned Zephyr / hal_rpi_pico trees.
`west update` re-fetches those trees clean, so re-apply after every update
(idempotent):

```bash
scripts/apply_patches.sh
```

CI applies them automatically. They are insurance — debugprobe doesn't trip
either bug today; see `patches/README.md` for what each fixes and when it
becomes mandatory.
