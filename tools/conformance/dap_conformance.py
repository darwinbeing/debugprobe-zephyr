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
    ("hoststatus_bad",     [0x01, 0x02, 0x01]),   # intentionally invalid LED id 0x02
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
    ("jtag_seq_err",       [0x14, 0x01, 0x01, 0xAA]),  # degenerate JTAG_Sequence: TDI=1, 0 TCK cycles
    ("unknown_cmd",        [0x17]),
    ("vendor_echo",        [0x85, 0x01]),  # DAP_Vendor5: DAP_vendor.c echoes only the command-ID byte (-> 85), no data
    ("execute_batch",      [0x7F, 0x02, 0x00, 0x04, 0x09, 0x64, 0x00]),  # DAP_ExecuteCommands: count=2 -> DAP_Info(0x04) + DAP_Delay(100us)
    ("disconnect",         [0x03]),
]

# Requests whose responses depend on the attached SWD target — compared
# structurally (same command echo + same length class), not byte-for-byte.
TARGET_DEPENDENT = set()  # none in the sequence above (all probe-level)


def _bulk_eps(dev):
    cfg = dev.get_active_configuration()
    if0 = cfg[(0, 0)]
    out_ep = next((e for e in if0 if not (e.bEndpointAddress & 0x80)), None)
    in_ep  = next((e for e in if0 if (e.bEndpointAddress & 0x80)),  None)
    if out_ep is None or in_ep is None:
        return None, None
    return out_ep.bEndpointAddress, in_ep.bEndpointAddress


def capture(out_path):
    import usb.core
    import usb.util
    dev = usb.core.find(idVendor=VID, idProduct=PID)
    if dev is None:
        print(f"device {VID:04x}:{PID:04x} not found", file=sys.stderr)
        return 2

    out_ep, in_ep = _bulk_eps(dev)
    if out_ep is None or in_ep is None:
        print("bulk endpoints not found on interface 0", file=sys.stderr)
        return 2

    # Detach any kernel driver that may hold interface 0 (Linux; no-op on macOS)
    try:
        if dev.is_kernel_driver_active(0):
            dev.detach_kernel_driver(0)
    except NotImplementedError:
        pass  # macOS raises NotImplementedError — safe to ignore

    try:
        usb.util.claim_interface(dev, 0)
    except usb.core.USBError as e:
        print(
            f"failed to claim interface 0: {e}; "
            "it may be held by the OS or a running openocd — stop openocd and retry",
            file=sys.stderr,
        )
        return 2

    try:
        result = {}
        for label, req in SEQUENCE:
            req_bytes = bytes(req)
            dev.write(out_ep, req_bytes)
            resp = bytes(dev.read(in_ep, 512, timeout=1000))
            result[label] = {"req": req_bytes.hex(), "resp": resp.hex()}
    finally:
        usb.util.release_interface(dev, 0)
        usb.util.dispose_resources(dev)

    with open(out_path, "w") as f:
        json.dump(result, f, indent=2)
    print(f"captured {len(result)} responses -> {out_path}")
    return 0


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


if __name__ == "__main__":
    sys.exit(main())
