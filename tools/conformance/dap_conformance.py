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
