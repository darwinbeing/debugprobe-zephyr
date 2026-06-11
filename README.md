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

## Fresh machine setup (T2 topology)

```bash
mkdir debugprobe-ws && cd debugprobe-ws
west init -m <this repository URL>
west update
```
