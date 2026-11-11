"""Memory layout of the supported MSP430 devices."""
from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True)
class Device:
    name: str
    device_id: int  # word at DeviceIdPointer + 4, as read (little-endian)
    main_start: int
    main_end: int  # exclusive
    segment_size: int  # main flash erase segment
    info_segments: tuple  # ((start, size), ...): the writable information segments
    ram_start: int  # where the flash funclets are loaded

    def region(self, addr: int):
        """(start, end) of the programmable region that holds addr, or None."""
        if self.main_start <= addr < self.main_end:
            return (self.main_start, self.main_end)
        for start, size in self.info_segments:
            if start <= addr < start + size:
                return (start, start + size)
        return None

    def in_main(self, addr: int) -> bool:
        return self.main_start <= addr < self.main_end

    def erase_base(self, addr: int) -> int:
        """Start of the erase segment that holds addr (addr must be programmable)."""
        if self.in_main(addr):
            return addr - (addr - self.main_start) % self.segment_size
        return self.region(addr)[0]


# Info segment A (0x1980) and the BSL area (0x1000-0x17FF) are deliberately
# absent: the tool never writes them.
MSP430F5529 = Device(
    name="MSP430F5529",
    device_id=0x2955,  # bytes 55h 29h at 0x1A04
    main_start=0x4400,
    main_end=0x24400,
    segment_size=512,
    info_segments=((0x1800, 128), (0x1880, 128), (0x1900, 128)),
    ram_start=0x2400,
)

DEVICES = {d.device_id: d for d in (MSP430F5529,)}


def lookup(device_id: int):
    return DEVICES.get(device_id)
