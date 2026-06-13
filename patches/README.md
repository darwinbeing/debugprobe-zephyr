# Out-of-tree patches

These patch the Zephyr west workspace, which `west update` re-fetches fresh on
every build — so the patches must be re-applied after each update. CI
(`.github/workflows/release.yml`) applies them automatically right after the
Zephyr setup step. For local builds run `scripts/apply_patches.sh`.

debugprobe does **not** currently exercise either bug (it never writes its own
flash and never calls `malloc`/`new`). They are carried as **insurance**: both
are latent in the pinned Zephyr v4.4.0 / hal_rpi_pico trees and would cost a
long debug session if a future feature trips them. See the "When you'll need
them" notes below.

| Patch | Target tree | Fixes |
|-------|-------------|-------|
| `rp2040_flash_ramfunc.patch` | `modules/hal/rpi_pico` | Zephyr #68728 (root cause): `hardware_flash/flash.c` helpers `flash_wait_ready` / `flash_enable_write` / `flash_put_cmd_addr` are `static inline` without the RAM attribute → when not inlined they sit in XIP flash and are called by `flash_write_partial_internal` while XIP is off → instruction-fetch HardFault → double fault → **MCU lockup on the first NVS/flash write**. Forces them into RAM. |
| `zephyr_sysconf_newlib_abi.patch` | `zephyr` | Zephyr #111011: Zephyr's POSIX `sysconf()` numbers `_SC_*` with its own enum, but full newlib's prebuilt dlmalloc calls `sysconf(8)` = newlib's `_SC_PAGESIZE`, which Zephyr reads as `_SC_MAPPED_FILES` → bogus "page size" → first `malloc()` returns NULL / `k_panic`. Aligns Zephyr's `_SC_*` values with the newlib ABI. |

## When you'll need them (the conditions that make them mandatory)

- **`rp2040_flash_ramfunc`** — the moment debugprobe persists anything to its own
  flash at runtime (NVS, the `settings` subsystem, a flash-backed serial number).
  Without it the **first** write locks the MCU.
- **`zephyr_sysconf_newlib_abi`** — only bites when *all three* hold: full newlib
  (`CONFIG_NEWLIB_LIBC=y`, already true here) **and** Zephyr's POSIX `sysconf`
  is compiled in (`CONFIG_POSIX_API=y`, currently off) **and** something calls
  `malloc`/`new` (`std::string`, `std::vector`, …). Adding C++ heap use plus the
  POSIX layer would trip it.

## Patches deliberately NOT carried

The upstream `~/Developer/zpsu_mon/patches` set also has `st7789v_clear_gram`
(ST7789 LCD) and `cyw43439_ap_set_channel` (CYW43439 WiFi SoftAP). They are
**excluded** here:

- A debug probe has no display and is not a WiFi AP, so neither feature can
  appear.
- `cyw43439_ap_set_channel` targets `modules/hal/infineon`, which this project's
  `west.yml` does not import (`name-allowlist` = `cmsis_6`, `hal_rpi_pico`), so
  it has no tree to apply to in CI.

If the hardware target ever changes, copy the patch back from `zpsu_mon` and add
its `git -C <tree> apply` line to both `scripts/apply_patches.sh` and the CI
workflow.

## Apply locally

```sh
scripts/apply_patches.sh            # idempotent; safe to re-run
```

Or by hand, from a west workspace root (note the different target trees):

```sh
git -C zephyr               apply <repo>/patches/zephyr_sysconf_newlib_abi.patch
git -C modules/hal/rpi_pico apply <repo>/patches/rp2040_flash_ramfunc.patch
```
