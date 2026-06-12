# CMSIS-DAP conformance results (phase 5)

This document records the byte-level conformance comparison between the Zephyr
rewrite and the official Raspberry Pi debugprobe v2.3.1 firmware.  The host tool
`tools/conformance/dap_conformance.py` sends a fixed 31-command DAP request
sequence to the probe and records `{label: {req, resp}}` JSON.  The `compare`
subcommand byte-diffs two captures and reports WHITELIST / ENV-DEP / MISMATCH
against a documented allowlist.

## Tool & procedure

The live old-vs-new runbook requires two firmware images on the same hardware.
Run all commands from the repo root.

### Step 1 — capture our firmware (live)

Flash `build/rpi_pico/zephyr/zephyr.uf2` (the Zephyr rewrite), then:

```bash
tools/conformance/.venv/bin/python tools/conformance/dap_conformance.py \
    capture --out /tmp/new.json
```

Expected output: `captured 31 responses`.

### Step 2 — flash the official firmware

Hold BOOTSEL on the probe board while connecting USB to force mass-storage mode,
then:

```bash
cp /tmp/debugprobe_on_pico.uf2 /Volumes/RPI-RP2/
```

> **Note:** `picotool` cannot force a BOOTSEL reset on the debugprobe because
> it exposes no reset interface that picotool can address.  A physical BOOTSEL
> button press is required.

### Step 3 — capture the official firmware (live)

```bash
tools/conformance/.venv/bin/python tools/conformance/dap_conformance.py \
    capture --out /tmp/old.json
```

Expected output: `captured 31 responses`.

### Step 4 — compare

```bash
tools/conformance/.venv/bin/python tools/conformance/dap_conformance.py \
    compare /tmp/old.json /tmp/new.json
```

### Step 5 — re-flash our firmware

Hold BOOTSEL again and copy `build/rpi_pico/zephyr/zephyr.uf2` back.

---

## Result

**Board:** rpi_pico, USB 2e8a:000c, USB serial E661A4D41777B429.
**Firmware under test:** Zephyr rewrite (phase 5), endpoints OUT=0x01/IN=0x81.
**Reference:** official debugprobe v2.3.1 (endpoints OUT=0x04/IN=0x85).

**Headline: 27 byte-identical / 3 whitelisted / 1 environment-dependent / 0 unexpected mismatches.**

### Full cross-check table

The live capture of our firmware was compared against the expected original
responses derived by reading the official v2.3.1 source (`DAP.c`,
`DAP_config.h`, `DAP_vendor.c`) in `~/Developer/debugprobe`.  See
[Pending empirical confirmation](#pending-empirical-confirmation) below.

| label | ours (live) | original (from source) | verdict |
|---|---|---|---|
| info_vendor | `000d52617370626572727920506900` | `0000` | WHITELIST |
| info_product | `001f446562756770726f6265206f6e205069636f2028434d5349532d4441502900` | `0000` | WHITELIST |
| info_serial | `00114536363141344434313737374234323900` | `0000` | WHITELIST |
| info_fw_version | `0006322e302e3000` | `0006322e302e3000` | MATCH |
| info_device_vendor | `0000` | `0000` | MATCH |
| info_device_name | `0000` | `0000` | MATCH |
| info_capabilities | `000131` | `000131` | MATCH |
| info_timestamp_clk | `000440420f00` | `000440420f00` | MATCH |
| info_swo_buf | `0000` | `0000` | MATCH |
| info_packet_count | `000106` | `000106` | MATCH |
| info_packet_size | `00024000` | `00024000` | MATCH |
| hoststatus_conn_on | `0100` | `0100` | MATCH |
| hoststatus_run_off | `0100` | `0100` | MATCH |
| hoststatus_bad | `01ff` | `01ff` | MATCH |
| connect_default | `0201` | `0201` | MATCH |
| connect_swd | `0201` | `0201` | MATCH |
| connect_jtag | `0200` | `0200` | MATCH |
| swj_clock_1mhz | `1100` | `1100` | MATCH |
| swj_clock_zero | `11ff` | `11ff` | MATCH |
| swj_pins_read | `1080` | (env-dependent) | ENV-DEP |
| swj_sequence | `1200` | `1200` | MATCH |
| swd_configure | `1300` | `1300` | MATCH |
| transfer_configure | `0400` | `0400` | MATCH |
| swd_sequence_out | `1d00` | `1d00` | MATCH |
| delay | `0900` | `0900` | MATCH |
| reset_target | `0a0000` | `0a0000` | MATCH |
| jtag_seq_err | `14ff` | `14ff` | MATCH |
| unknown_cmd | `ff` | `ff` | MATCH |
| vendor_echo | `85` | `85` | MATCH |
| execute_batch | `7f020006322e302e30000900` | `7f020006322e302e30000900` | MATCH |
| disconnect | `0300` | `0300` | MATCH |

### Compare tool output

Running `compare` on `old_rpi_pico_source_derived.json` vs `new_rpi_pico.json`
produced:

```
[WHITELIST] info_vendor: old=0000 new=000d52617370626572727920506900  (original returns empty; ours = vendor string)
[WHITELIST] info_product: old=0000 new=001f446562756770726f6265206f6e205069636f2028434d5349532d4441502900  (original returns empty; ours = product string)
[WHITELIST] info_serial: old=0000 new=00114536363141344434313737374234323900  (original returns empty; ours = hwinfo serial)

OK: byte-identical on all non-whitelisted commands (3 whitelisted, 0 missing)
```

### Whitelisted DAP_Info differences

The three WHITELIST entries are a deliberate enhancement, not a regression.

The official debugprobe v2.3.1 leaves `GetVendorString`, `GetProductString`, and
`GetSerNumString` returning `0` in `DAP_config.h`, so DAP_Info responses for
IDs 0x01 / 0x02 / 0x03 are empty (`0000`).

Our firmware populates them:

| DAP_Info ID | ours | why |
|---|---|---|
| 0x01 Vendor | `"Raspberry Pi"` | constant string |
| 0x02 Product | `"Debugprobe on Pico (CMSIS-DAP)"` | board-specific |
| 0x03 Serial | `"E661A4D41777B429"` (8 bytes from `hwinfo`) | matches USB iSerialNumber |

The practical benefit: openocd's `Info : CMSIS-DAP: Serial# = E661A4D41777B429`
log line makes it unambiguous which probe is selected when multiple probes are
connected to the same host.

### Environment-dependent: swj_pins_read

The `swj_pins_read` response byte (`0x80` in our live capture, encoded as `1080`)
reflects the live state of the SWDIO / SWDCLK / nRESET / TDO / TDI / nTRST pins
at the instant the command was issued.  This value is inherently hardware- and
wiring-dependent and is excluded from the byte-identical count.  Both firmwares
implement `DAP_SWJ_Pins` identically; only the environment differs.

### Pending empirical confirmation

The reference file `tools/conformance/captures/old_rpi_pico_source_derived.json`
was **derived by reading the official v2.3.1 source** — it is not a live capture.
It represents the expected response for every command based on the compile-time
constants and logic in `DAP.c`, `DAP_config.h`, and `DAP_vendor.c`.

A live empirical capture of the official firmware requires physically pressing
the BOOTSEL button on the probe board to force mass-storage mode for re-flashing
(`picotool` cannot force a BOOTSEL reset on the debugprobe because it exposes no
reset interface that picotool can address).  This live capture is a documented
manual step for final sign-off and is expected to reproduce this exact result.

Committed artifacts:
- `tools/conformance/captures/new_rpi_pico.json` — live capture of our firmware.
- `tools/conformance/captures/old_rpi_pico_source_derived.json` — source-derived
  reference; pending empirical confirmation via live BOOTSEL re-flash.
