# debugprobe-zephyr Phase 5 (Conformance Comparison + On-Target Acceptance) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Prove the rewrite is a faithful CMSIS-DAP drop-in: a tool that sends an identical DAP request sequence to BOTH our firmware and the official `debugprobe_on_pico.uf2` v2.3.1 and byte-compares the responses (with a documented whitelist for deliberate differences), plus a documented + executed three-board on-target acceptance.

**Architecture:** A Python tool `tools/conformance/dap_conformance.py` with two modes: `capture` (send a fixed, mostly-target-independent DAP request sequence to the connected CMSIS-DAP device over the vendor bulk endpoints, record each `{request_hex: response_hex}` to a JSON file) and `compare` (byte-diff two capture files, apply a whitelist of known/deliberate differences, report PASS/divergences). The capture sequence covers the probe-level command set (Info, HostStatus, Connect/Disconnect, SWJ, SWD_Configure, TransferConfigure, unknown, vendor, ExecuteCommands) whose responses are deterministic without a target; transfers (which depend on the attached target) are captured separately and compared only structurally. The compare/whitelist logic is pure Python and unit-tested with synthetic captures (no device).

**Tech Stack:** Python 3 + pyusb (reuses `tools/conformance/check_descriptors.py`'s device-find + bulk-EP plumbing). Reference: the official `debugprobe_on_pico.uf2` v2.3.1 (already downloaded to `/tmp/debugprobe_on_pico.uf2`; re-fetch from github.com/raspberrypi/debugprobe/releases if absent). A board is attached for the actual capture runs.

---

## The known divergence this tool will surface (the design point)

The original's `DAP_Info(Vendor/Product/SerialNumber)` returns EMPTY (`DAP_config.h` GetVendorString/GetProductString/GetSerNumString all `return 0`). OUR firmware populates them (vendor "Raspberry Pi", product per board, serial from hwinfo — phase 4). So `DAP_Info(0x01/0x02/0x03)` responses DIFFER. This is a deliberate enhancement (host tools like openocd then show `Serial# = ...`), not a protocol bug — the whitelist documents it as accepted. Everything else (capabilities 0x31, packet size 64, packet count 6, timestamp clock 1 MHz, FW version "2.0.0", HostStatus, Connect, SWJ, unknown→0xFF, vendor→echo, ExecuteCommands framing) is expected byte-identical, which is what the tool proves.

## Hardware workflow (Task 3 executes this)

1. With OUR firmware flashed: `dap_conformance.py capture --out new.json`.
2. BOOTSEL + flash `/tmp/debugprobe_on_pico.uf2` (the official v2.3.1).
3. `dap_conformance.py capture --out old.json`.
4. `dap_conformance.py compare old.json new.json` → report. Re-flash our firmware after.

## Scope note

Phase 5 is verification + tooling, not new firmware. No `src/` or `lib/` changes. Full openocd memory R/W acceptance needs a SINGLE-DROP SWD target (a multidrop RP2040 fails even the official firmware — proven in the phase-3 bring-up); Task 4 documents the procedure and records whatever the available hardware allows.

**Build/test env:** host Python (`tools/conformance/.venv` already exists with pyusb). Firmware build/flash same as prior phases (gnuarmemb, west via python3.14, BOOTSEL). Host unit tests for the compare logic run via plain `python3 -m pytest` or a `__main__` self-test — no Zephyr.

**Branch:** `phase5-conformance` from master; merge at the end.

---

### Task 1: DAP transport + capture mode

**Files:**
- Create: `tools/conformance/dap_conformance.py`
- Modify: `tools/conformance/requirements.txt` (no change expected — pyusb already there; confirm)

- [ ] **Step 1: Write the DAP transport + request sequence + capture mode**

`tools/conformance/dap_conformance.py`:

```python
#!/usr/bin/env python3
"""CMSIS-DAP conformance capture/compare for debugprobe-zephyr vs the original.

capture:  python3 dap_conformance.py capture --out new.json
compare:  python3 dap_conformance.py compare old.json new.json
selftest: python3 dap_conformance.py selftest   # unit-tests the compare logic, no device
"""
import argparse
import json
import sys

VID, PID = 0x2E8A, 0x000C

# (label, request_bytes) — a fixed sequence. The probe-level commands are
# deterministic without a target; transfer commands depend on the attached
# target and are compared structurally only (see compare()).
SEQUENCE = [
    ("info_vendor",        [0x00, 0x01]),
    ("info_product",       [0x00, 0x02]),
    ("info_serial",        [0x00, 0x03]),
    ("info_fw_version",    [0x00, 0x04]),
    ("info_device_vendor", [0x00, 0x05]),
    ("info_device_name",   [0x00, 0x06]),
    ("info_capabilities",  [0x00, 0xF0]),
    ("info_timestamp_clk", [0x00, 0xF1]),
    ("info_swo_buf",       [0x00, 0xFD]),
    ("info_packet_count",  [0x00, 0xFE]),
    ("info_packet_size",   [0x00, 0xFF]),
    ("hoststatus_conn_on", [0x01, 0x00, 0x01]),
    ("hoststatus_run_off", [0x01, 0x01, 0x00]),
    ("hoststatus_bad",     [0x01, 0x02, 0x01]),
    ("connect_default",    [0x02, 0x00]),
    ("connect_swd",        [0x02, 0x01]),
    ("connect_jtag",       [0x02, 0x02]),
    ("swj_clock_1mhz",     [0x11, 0x40, 0x42, 0x0F, 0x00]),
    ("swj_clock_zero",     [0x11, 0x00, 0x00, 0x00, 0x00]),
    ("swj_pins_read",      [0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00]),
    ("swj_sequence",       [0x12, 16, 0xFF, 0x07]),
    ("swd_configure",      [0x13, 0x00]),
    ("transfer_configure", [0x04, 0x00, 0x64, 0x00, 0x00, 0x00]),
    ("swd_sequence_out",   [0x1D, 0x01, 0x08, 0x9E]),
    ("delay",              [0x09, 0xE8, 0x03]),
    ("reset_target",       [0x0A]),
    ("jtag_seq_err",       [0x14, 0x01, 0x01, 0xAA]),
    ("unknown_cmd",        [0x17]),
    ("vendor_echo",        [0x85, 0x01]),
    ("execute_batch",      [0x7F, 0x02, 0x00, 0x04, 0x09, 0x64, 0x00]),
    ("disconnect",         [0x03]),
]

# Requests whose responses depend on the attached SWD target — compared
# structurally (same command echo + same length class), not byte-for-byte.
TARGET_DEPENDENT = set()  # none in the sequence above (all probe-level)


def _bulk_eps(dev):
    cfg = dev.get_active_configuration()
    if0 = cfg[(0, 0)]
    out_ep = next(e for e in if0 if not (e.bEndpointAddress & 0x80))
    in_ep = next(e for e in if0 if (e.bEndpointAddress & 0x80))
    return out_ep.bEndpointAddress, in_ep.bEndpointAddress


def capture(out_path):
    import usb.core
    dev = usb.core.find(idVendor=VID, idProduct=PID)
    if dev is None:
        print(f"device {VID:04x}:{PID:04x} not found", file=sys.stderr)
        return 2
    out_ep, in_ep = _bulk_eps(dev)
    result = {}
    for label, req in SEQUENCE:
        dev.write(out_ep, bytes(req))
        resp = bytes(dev.read(in_ep, 512, timeout=1000))
        result[label] = {"req": bytes(req).hex(), "resp": resp.hex()}
    with open(out_path, "w") as f:
        json.dump(result, f, indent=2)
    print(f"captured {len(result)} responses -> {out_path}")
    return 0
```

(Continue with `compare()` and `main()` in Task 2 — but include the imports/argparse skeleton so the file runs. For this task, `main()` only needs to wire `capture`; Task 2 adds `compare`/`selftest`.)

```python
def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    cap = sub.add_parser("capture"); cap.add_argument("--out", required=True)
    args = ap.parse_args()
    if args.cmd == "capture":
        return capture(args.out)
    return 1


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 2: Syntax-check + smoke the sequence builder**

```bash
python3 -m py_compile ~/Developer/debugprobe-zephyr/tools/conformance/dap_conformance.py && echo OK
python3 -c "import sys; sys.path.insert(0,'$HOME/Developer/debugprobe-zephyr/tools/conformance'); import dap_conformance as d; print(len(d.SEQUENCE), 'requests'); assert all(len(r)>=1 for _,r in d.SEQUENCE)"
```

Expected: `OK` and the request count printed. (No device needed for this step.)

- [ ] **Step 3: Commit**

```bash
cd ~/Developer/debugprobe-zephyr
git add tools/conformance/dap_conformance.py
git commit -m "feat: DAP conformance capture tool (request sequence + JSON output)"
```

---

### Task 2: Compare mode + whitelist + self-test

**Files:**
- Modify: `tools/conformance/dap_conformance.py`

- [ ] **Step 1: Add the whitelist + compare + selftest, and wire them into main()**

Insert before `main()`:

```python
# Labels whose response is EXPECTED to differ between the original and our
# firmware (deliberate, documented). DAP_Info vendor/product/serial: the
# original returns empty strings (DAP_config.h GetVendorString/Product/SerNum
# all return 0); ours populates them (vendor "Raspberry Pi", product per board,
# serial from hwinfo). This is an enhancement, not a protocol bug.
WHITELIST = {
    "info_vendor": "original returns empty; ours = vendor string",
    "info_product": "original returns empty; ours = product string",
    "info_serial": "original returns empty; ours = hwinfo serial",
}


def compare(old_path, new_path):
    with open(old_path) as f:
        old = json.load(f)
    with open(new_path) as f:
        new = json.load(f)
    # Iterate only labels present in at least one capture, in SEQUENCE order
    # (so the selftest's small synthetic captures don't report the unused
    # SEQUENCE labels as MISSING).
    present = set(old) | set(new)
    seq_labels = [l for l, _ in SEQUENCE]
    labels = [l for l in seq_labels if l in present] + \
        sorted(present - set(seq_labels))
    diffs, whitelisted, missing = [], [], []
    for label in labels:
        if label not in old or label not in new:
            missing.append(label)
            continue
        o, n = old[label]["resp"], new[label]["resp"]
        if o == n:
            continue
        if label in WHITELIST or label in TARGET_DEPENDENT:
            whitelisted.append((label, o, n))
        else:
            diffs.append((label, o, n))

    for label, o, n in whitelisted:
        reason = WHITELIST.get(label, "target-dependent")
        print(f"[WHITELIST] {label}: old={o} new={n}  ({reason})")
    for label in missing:
        print(f"[MISSING]   {label}: not in both captures")
    for label, o, n in diffs:
        print(f"[DIFF]      {label}:\n  old={o}\n  new={n}")
    print()
    if diffs:
        print(f"FAIL: {len(diffs)} unexpected divergence(s)")
        return 1
    print(f"OK: byte-identical on all non-whitelisted commands "
          f"({len(whitelisted)} whitelisted, {len(missing)} missing)")
    return 0


def selftest():
    """Unit-test compare()/whitelist with synthetic captures (no device)."""
    import tempfile, os
    a = {"info_capabilities": {"req": "00f0", "resp": "000131"},
         "info_vendor": {"req": "0001", "resp": "0000"},
         "connect_swd": {"req": "0201", "resp": "0201"}}
    b_ok = {"info_capabilities": {"req": "00f0", "resp": "000131"},
            "info_vendor": {"req": "0001", "resp": "020d52617370..."},
            "connect_swd": {"req": "0201", "resp": "0201"}}
    b_bad = dict(b_ok); b_bad["info_capabilities"] = {"req": "00f0", "resp": "000130"}
    failures = 0
    with tempfile.TemporaryDirectory() as d:
        pa = os.path.join(d, "a.json"); pb = os.path.join(d, "b.json")
        for path, data in ((pa, a),):
            json.dump(data, open(path, "w"))
        # OK case: only the whitelisted info_vendor differs
        json.dump(b_ok, open(pb, "w"))
        if compare(pa, pb) != 0:
            print("SELFTEST FAIL: ok-case should pass"); failures += 1
        # FAIL case: capabilities differ (not whitelisted)
        json.dump(b_bad, open(pb, "w"))
        if compare(pa, pb) != 1:
            print("SELFTEST FAIL: bad-case should fail"); failures += 1
    print("SELFTEST", "PASS" if failures == 0 else f"FAIL ({failures})")
    return 1 if failures else 0
```

Update `main()` to add the `compare` and `selftest` subcommands:

```python
def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    cap = sub.add_parser("capture"); cap.add_argument("--out", required=True)
    cmp = sub.add_parser("compare")
    cmp.add_argument("old"); cmp.add_argument("new")
    sub.add_parser("selftest")
    args = ap.parse_args()
    if args.cmd == "capture":
        return capture(args.out)
    if args.cmd == "compare":
        return compare(args.old, args.new)
    if args.cmd == "selftest":
        return selftest()
    return 1
```

- [ ] **Step 2: Run the self-test (no device)**

```bash
python3 ~/Developer/debugprobe-zephyr/tools/conformance/dap_conformance.py selftest
```

Expected: prints a `[WHITELIST] info_vendor ...` line + `OK:` for the ok-case, then a `[DIFF] info_capabilities` + `FAIL:` for the bad-case, then `SELFTEST PASS`. Exit 0.

- [ ] **Step 3: Commit**

```bash
cd ~/Developer/debugprobe-zephyr
git add tools/conformance/dap_conformance.py
git commit -m "feat: DAP conformance compare mode + whitelist + selftest"
```

---

### Task 3: Run the old-vs-new comparison on hardware + record findings

**Files:**
- Create: `tools/conformance/captures/.gitkeep` (keep the dir; the JSON captures themselves are recorded in the findings doc, not necessarily committed)
- Create: `docs/CONFORMANCE_RESULTS.md`

This task is controller-executed (it needs the attached board + flashing both firmwares). The implementer prepares the doc skeleton + the exact commands; the controller runs them and fills the results.

- [ ] **Step 1: Write the runbook + results skeleton `docs/CONFORMANCE_RESULTS.md`**

```markdown
# CMSIS-DAP conformance: debugprobe-zephyr vs official debugprobe v2.3.1

Tool: `tools/conformance/dap_conformance.py`. Procedure:

1. Our firmware flashed → `python3 tools/conformance/dap_conformance.py capture --out /tmp/new.json`
2. BOOTSEL, flash `/tmp/debugprobe_on_pico.uf2` (official v2.3.1; fetch from
   github.com/raspberrypi/debugprobe/releases if absent)
3. `python3 tools/conformance/dap_conformance.py capture --out /tmp/old.json`
4. `python3 tools/conformance/dap_conformance.py compare /tmp/old.json /tmp/new.json`
5. Re-flash our firmware.

## Result

(Filled by the conformance run. Expected: byte-identical on all probe-level
commands except the whitelisted DAP_Info vendor/product/serial strings, which
the original leaves empty and ours populates.)
```

- [ ] **Step 2: (controller) Capture our firmware** — with our `build/rpi_pico/zephyr/zephyr.uf2` flashed: `tools/conformance/.venv/bin/python tools/conformance/dap_conformance.py capture --out /tmp/new.json`. Record the result.

- [ ] **Step 3: (controller) Flash the official firmware + capture** — BOOTSEL, `cp /tmp/debugprobe_on_pico.uf2 /Volumes/RPI-RP2/`, wait for re-enumeration, `... capture --out /tmp/old.json`.

- [ ] **Step 4: (controller) Compare + fill the results doc** — `... compare /tmp/old.json /tmp/new.json`. Paste the output into `docs/CONFORMANCE_RESULTS.md` under "## Result". Confirm the ONLY diffs are the whitelisted DAP_Info strings (or document any genuine divergence found, with an assessment: real bug vs acceptable). Re-flash our firmware.

- [ ] **Step 5: Commit**

```bash
cd ~/Developer/debugprobe-zephyr
git add tools/conformance/captures docs/CONFORMANCE_RESULTS.md
git commit -m "test: old-vs-new DAP conformance comparison results"
```

---

### Task 4: On-target acceptance procedure + project completion summary

**Files:**
- Modify: `docs/HARDWARE_BRINGUP.md` (add the phase-5 full-acceptance procedure)
- Create: `docs/PROJECT_STATUS.md` (what's verified, what's pending, per phase)

- [ ] **Step 1: Append the full openocd acceptance procedure to `docs/HARDWARE_BRINGUP.md`**

Add a section "Full on-target acceptance (phase 5)" with the exact openocd flow against a SINGLE-DROP target (connect → `reset halt` → read/write memory → flash program → reset → breakpoint), the bitbang-vs-PIO backend matrix, the UART-bridge + autobaud checks, and the note that a multidrop RP2040 target fails even the official firmware (use a single-core/single-DP target like an STM32 or a bare Cortex-M).

```markdown
## Full on-target acceptance (phase 5)

Per board (rpi_pico, rpi_pico2, rpi_debug_probe), per SWD backend (PIO default;
bitbang via CONFIG_PROBE_SWD_BITBANG + overlay status flip), against a
SINGLE-DROP SWD target (NOT a multidrop RP2040):

1. openocd connect: `openocd -f interface/cmsis-dap.cfg -c "adapter speed 1000" -f target/<chip>.cfg -c init`
2. `reset halt`; read core registers (`reg`); read memory (`mdw <addr> 4`).
3. Write + read-back RAM (`mww <ram> 0xC0FFEE42; mdw <ram> 1`).
4. Flash program a small image; `verify_image`; `reset run`.
5. Breakpoint + step.
6. UART bridge: CDC <-> uart1 round-trip at 115200 (and a second baud) with no loss.
7. Autobaud: drive uart1 RX (GP5) from a known UART source, open the CDC at the
   MAGIC_BAUD sentinel (9728), confirm uart1 is reconfigured to the detected baud.
8. LEDs (debug-probe): DAP connect/run LEDs follow openocd; UART TX/RX LEDs blink on bridge traffic.

Record pass/fail per (board, backend) in PROJECT_STATUS.md.
```

- [ ] **Step 2: Write `docs/PROJECT_STATUS.md`** — a per-phase table of what's implemented and what's verified (host tests / 3-board build / on-target), citing the actual hardware results from this whole effort (phases 1-2 USB+DAP verified; phase 3 PIO+bitbang SWD verified reading DPIDR; phase 4 bridge+LEDs+serial verified; phase 4b custom bridge loopback + autobaud no-wedge verified; phase 5 conformance result). Mark the genuinely-pending items (autobaud detection happy-path needs an external UART source; full memory-R/W acceptance needs a single-drop target).

- [ ] **Step 3: (controller) Execute whatever on-target acceptance the available hardware allows** and record results in PROJECT_STATUS.md (at minimum: conformance comparison from Task 3; the bridge loopback + autobaud-no-wedge already verified in phase 4b; DPIDR read on both backends from phase 3).

- [ ] **Step 4: Commit**

```bash
cd ~/Developer/debugprobe-zephyr
git add docs/HARDWARE_BRINGUP.md docs/PROJECT_STATUS.md
git commit -m "docs: phase-5 on-target acceptance procedure + project status"
```

---

## Known Risks and Handling

- **DAP transfer determinism**: the SEQUENCE is intentionally all probe-level (no `DAP_Transfer`/`TransferBlock`) so responses are target-independent and byte-deterministic. If a transfer comparison is desired later, add it to `TARGET_DEPENDENT` (structural compare only) — transfers against the multidrop RP2040 give non-deterministic contention values.
- **Whitelist completeness**: if the compare surfaces a diff NOT in the whitelist, it is EITHER a real protocol bug (fix the firmware) OR another deliberate difference (add to the whitelist WITH a documented reason). Task 3 Step 4 requires assessing each.
- **pyusb claiming the vendor interface on macOS**: `dap_conformance.py capture` does bulk transfers on interface 0; if macOS has it bound, the capture errors — the tool should surface a clear message (reuse check_descriptors.py's defensive pattern). openocd must NOT be running concurrently (it claims the interface).
- **Single-drop target availability**: full memory-R/W acceptance (Task 4) needs a non-multidrop target; if unavailable, PROJECT_STATUS.md records it as pending with the reason (env, not a firmware defect — proven the official firmware fails the same multidrop setup).
- **DAP_Info string divergence is the expected headline result**: it confirms the protocol engine is otherwise byte-faithful. If the user prefers strict parity over the populated-serial enhancement, the firmware change is to null `Config.vendor/product/serial` — but that loses the useful `openocd Serial# = ...`; left as a documented choice, defaulting to keep the enhancement.
