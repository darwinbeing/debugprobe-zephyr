#!/usr/bin/env bash
# Build all three boards; artifacts land in build/<board>/zephyr/zephyr.uf2
set -euo pipefail

APP_DIR="$(cd "$(dirname "$0")/.." && pwd)"
WS="${ZEPHYR_WORKSPACE:-$HOME/Developer/zephyrproject}"
# Zephyr 4.4 needs Python >= 3.12; override WEST if your `west` already
# runs under a new enough interpreter.
WEST="${WEST:-/usr/local/opt/python@3.14/bin/python3.14 -m west}"
export ZEPHYR_TOOLCHAIN_VARIANT="${ZEPHYR_TOOLCHAIN_VARIANT:-gnuarmemb}"
export GNUARMEMB_TOOLCHAIN_PATH="${GNUARMEMB_TOOLCHAIN_PATH:-$WS/arm-gnu-toolchain-14.2.rel1-darwin-x86_64-arm-none-eabi}"

for board in rpi_debug_probe rpi_pico rpi_pico2/rp2350a/m33; do
    build_dir="$APP_DIR/build/$(echo "$board" | cut -d/ -f1)"
    (cd "$WS" && $WEST build -p always -b "$board" -d "$build_dir" "$APP_DIR")
done

echo
ls -l "$APP_DIR"/build/*/zephyr/zephyr.uf2
