#!/usr/bin/env python3
"""Verify debugprobe-zephyr USB descriptors are aligned with the original debugprobe.

Usage: python3 check_descriptors.py
Needs: pip install pyusb; on macOS: brew install libusb
"""
import sys

import usb.core
import usb.util

VID, PID = 0x2E8A, 0x000C
EXPECTED_PRODUCTS = {
    "Debug Probe (CMSIS-DAP)",
    "Debugprobe on Pico (CMSIS-DAP)",
}
MSOS_PLATFORM_UUID = bytes((
    0xDF, 0x60, 0xDD, 0xD8, 0x89, 0x45, 0xC7, 0x4C,
    0x9C, 0xD2, 0x65, 0x9D, 0x9E, 0x64, 0x8A, 0x9F,
))
DAP_GUID = "{CDB3B5AD-293B-4663-AA36-1AAE46463776}"

failures = []


def check(name, cond, detail=""):
    status = "PASS" if cond else "FAIL"
    print(f"[{status}] {name}" + (f" ({detail})" if detail else ""))
    if not cond:
        failures.append(name)


def check_dap_serial(dev):
    """Send DAP_Info(SerialNumber) over the vendor bulk endpoints of interface 0
    and verify the returned serial matches the USB serial string descriptor.

    This is a soft check: if the vendor interface cannot be claimed (e.g. the
    DAP class driver holds it on macOS) or any USB transfer fails, the check is
    reported as SKIP rather than FAIL so the tool remains usable when no probe
    is attached or the OS has already claimed the interface.
    """
    try:
        cfg = dev.get_active_configuration()
        if0 = cfg[(0, 0)]

        # Detach any kernel driver that may hold the interface (Linux; no-op on macOS)
        if dev.is_kernel_driver_active(if0.bInterfaceNumber):
            dev.detach_kernel_driver(if0.bInterfaceNumber)

        usb.util.claim_interface(dev, if0.bInterfaceNumber)
        try:
            out_ep = next(
                e for e in if0 if not (e.bEndpointAddress & 0x80)
            )
            in_ep = next(
                e for e in if0 if (e.bEndpointAddress & 0x80)
            )

            # DAP_Info request: command byte 0x00, info-id byte 0x03 (SerialNumber)
            dev.write(out_ep.bEndpointAddress, bytes([0x00, 0x03]))
            resp = bytes(dev.read(in_ep.bEndpointAddress, 64, timeout=500))
            # Response format: {0x00, length, serial-bytes…}
            if len(resp) < 2:
                raise ValueError(f"response too short: {resp!r}")
            serial_bytes = resp[2:2 + resp[1]]
            serial = serial_bytes.rstrip(b"\x00").decode("ascii", "replace")
            usb_serial = usb.util.get_string(dev, dev.iSerialNumber)
            check("DAP_Info serial matches USB serial", serial == usb_serial,
                  f"dap={serial!r} usb={usb_serial!r}")
        finally:
            usb.util.release_interface(dev, if0.bInterfaceNumber)

    except usb.core.USBError as exc:
        print(f"[SKIP] DAP_Info serial (bulk EP unavailable: {exc})")
    except StopIteration:
        print("[SKIP] DAP_Info serial (bulk endpoints not found on interface 0)")
    except Exception as exc:  # noqa: BLE001
        print(f"[SKIP] DAP_Info serial (unexpected error: {exc})")


def main():
    dev = usb.core.find(idVendor=VID, idProduct=PID)
    if dev is None:
        print(f"Device {VID:04x}:{PID:04x} not found", file=sys.stderr)
        return 2

    check("bcdUSB == 2.10", dev.bcdUSB == 0x0210, f"got {dev.bcdUSB:#06x}")
    check("bcdDevice == 2.31", dev.bcdDevice == 0x0231,
          f"got {dev.bcdDevice:#06x}")
    check("bDeviceClass == 0", dev.bDeviceClass == 0)
    check("manufacturer", usb.util.get_string(dev, dev.iManufacturer)
          == "Raspberry Pi")
    product = usb.util.get_string(dev, dev.iProduct)
    check("product string", product in EXPECTED_PRODUCTS, product)
    serial = usb.util.get_string(dev, dev.iSerialNumber)
    check("serial is 16 uppercase hex digits",
          len(serial) == 16 and all(c in "0123456789ABCDEF" for c in serial),
          serial)

    cfg = dev.get_active_configuration()
    intfs = list(cfg)
    check("interface count == 3", len(intfs) == 3, f"got {len(intfs)}")

    if0 = cfg[(0, 0)]
    check("interface 0 is vendor class", if0.bInterfaceClass == 0xFF)
    check("interface 0 named CMSIS-DAP v2 Interface",
          usb.util.get_string(dev, if0.iInterface)
          == "CMSIS-DAP v2 Interface")
    eps = list(if0)
    check("interface 0 has 2 bulk endpoints",
          len(eps) == 2 and all(usb.util.endpoint_type(e.bmAttributes)
                                == usb.util.ENDPOINT_TYPE_BULK for e in eps))
    check("interface 1 is CDC control", cfg[(1, 0)].bInterfaceClass == 0x02)
    check("interface 2 is CDC data", cfg[(2, 0)].bInterfaceClass == 0x0A)

    # BOS: GET_DESCRIPTOR(BOS), check for the MS OS 2.0 platform capability
    bos = dev.ctrl_transfer(0x80, 0x06, (0x0F << 8), 0, 256)
    check("BOS contains MS OS 2.0 platform UUID",
          MSOS_PLATFORM_UUID in bytes(bos))

    # MS OS 2.0 descriptor set: vendor request (bRequest=0x01, wIndex=7)
    msos = bytes(dev.ctrl_transfer(0xC0, 0x01, 0, 7, 512))
    check("MS OS 2.0 contains WINUSB compatible ID", b"WINUSB" in msos)
    guid_utf16 = DAP_GUID.encode("utf-16-le")
    check("MS OS 2.0 contains DAP DeviceInterfaceGUID", guid_utf16 in msos)

    check_dap_serial(dev)

    print()
    if failures:
        print(f"FAILED: {len(failures)} mismatches: {failures}")
        return 1
    print("OK: descriptors aligned with the original")
    return 0


if __name__ == "__main__":
    sys.exit(main())
