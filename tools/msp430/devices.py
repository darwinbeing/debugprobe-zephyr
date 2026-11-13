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
    # Debugging (phase B). (start, end) pairs, end exclusive. The write rules
    # and the PC rule mirror the probe's kKnownDevices
    # (lib/msp430/src/session.cpp), which enforces them.
    peripherals: tuple  # peripheral registers
    peripheral_holes: tuple  # ((start, end, name), ...): peripherals a debug session never writes
    bsl: tuple  # bootloader area: a debug session neither reads nor writes it
    info: tuple  # information memory A-D
    tlv: tuple  # device descriptor (TLV), read-only
    ram: tuple  # all RAM, USB RAM included

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

    def memory_map(self):
        """The regions a read-only debug session may access, as GDB's memory map
        lists them: [(start, end, "ram" | "rom")], sorted. The BSL area and
        vacant memory are absent, so GDB does not access them."""
        return sorted([(*self.peripherals, "ram"), (*self.info, "rom"), (*self.tlv, "rom"),
                       (*self.ram, "ram"), (self.main_start, self.main_end, "rom")])

    def debug_readable(self, addr: int, length: int) -> bool:
        """Every byte of [addr, addr + length) lies in a memory-map region."""
        end = addr + length
        while addr < end:
            region = next((r for r in self.memory_map() if r[0] <= addr < r[1]), None)
            if region is None:
                return False
            addr = region[1]
        return True

    def debug_writable(self, addr: int, length: int) -> bool:
        """Read-only debug sessions write RAM, and peripheral registers outside
        the peripheral holes (flash controller, SYS, DMA). Nothing else."""
        end = addr + length
        in_ram = self.ram[0] <= addr and end <= self.ram[1]
        in_peripherals = self.peripherals[0] <= addr and end <= self.peripherals[1]
        hits_hole = any(addr < hi and end > lo for lo, hi, _ in self.peripheral_holes)
        return length > 0 and (in_ram or (in_peripherals and not hits_hole))

    def debug_pc_writable(self, pc: int, current_pc: int) -> bool:
        """Read-only debug sessions move the PC only into main flash, or leave
        it where it is (GDB's G packet writes every register back). An odd PC
        is the probe's BAD_ARG, not part of this rule."""
        return pc == current_pc or self.in_main(pc)


# Info segment A (0x1980) and the BSL area (0x1000-0x17FF) are deliberately
# absent from info_segments: the tool never writes them.
MSP430F5529 = Device(
    name="MSP430F5529",
    device_id=0x2955,  # bytes 55h 29h at 0x1A04
    main_start=0x4400,
    main_end=0x24400,
    segment_size=512,
    info_segments=((0x1800, 128), (0x1880, 128), (0x1900, 128)),
    ram_start=0x2400,
    peripherals=(0x0000, 0x1000),
    peripheral_holes=(
        # PMMCTL0..PM5CTL0: PMMCTL0's password (written 0xA5, read 0x96) makes a
        # byte write, done as a word read-modify-write, a reset; core voltage, SVS
        (0x0120, 0x0140, "PMM"),
        (0x0140, 0x0150, "flash controller"),  # FCTL1..FCTL4
        (0x0180, 0x01A0, "SYS"),  # SYSCTL (SYSRIVECT), SYSBSLC, ...: vectors, BSL protection
        (0x0500, 0x0540, "DMA"),  # DMACTL0..DMA2SZ: a transfer could write FCTL
    ),
    bsl=(0x1000, 0x1800),
    info=(0x1800, 0x1A00),
    tlv=(0x1A00, 0x1B00),
    ram=(0x1C00, 0x4400),
)

DEVICES = {d.device_id: d for d in (MSP430F5529,)}


def lookup(device_id: int):
    return DEVICES.get(device_id)
