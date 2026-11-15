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
    peripherals: tuple  # first to last peripheral register: a memory-map region and the
    #                     write window for registers (minus the holes), as on the probe
    peripheral_holes: tuple  # ((start, end, name), ...): peripherals a debug session never writes
    vacant: tuple  # ((start, end), ...): no registers there; never in the memory map
    bsl: tuple  # bootloader area: a debug session neither reads nor writes it
    info: tuple  # information memory A-D
    tlv: tuple  # device descriptor (TLV), read-only
    ram: tuple  # all RAM, USB RAM included
    # ((start, end, name, effect), ...), whole words, sorted: registers whose
    # read changes the device. The GDB server refuses reads there (`monitor read` reads on purpose).
    read_side_effects: tuple

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
        regions = []
        for start, end, kind in ((*self.peripherals, "ram"), (*self.info, "rom"),
                                 (*self.tlv, "rom"), (*self.ram, "ram"),
                                 (self.main_start, self.main_end, "rom")):
            pieces = [(start, end)]
            for v_lo, v_hi in self.vacant:
                pieces = [p for lo, hi in pieces
                          for p in ((lo, min(hi, v_lo)), (max(lo, v_hi), hi)) if p[0] < p[1]]
            regions += [(lo, hi, kind) for lo, hi in pieces]
        return sorted(regions)

    def debug_readable(self, addr: int, length: int) -> bool:
        """Every byte of [addr, addr + length) lies in a memory-map region, and
        none in the BSL area, whatever the map says. An empty range is not."""
        end = addr + length
        if length <= 0 or (addr < self.bsl[1] and end > self.bsl[0]):
            return False
        while addr < end:
            region = next((r for r in self.memory_map() if r[0] <= addr < r[1]), None)
            if region is None:
                return False
            addr = region[1]
        return True

    def debug_read_side_effect(self, addr: int, length: int):
        """The first read_side_effects entry that a read of [addr, addr +
        length) touches, or None. The probe reads whole words, so the range is
        widened to words first."""
        if length <= 0:
            return None
        start, end = addr & ~1, (addr + length + 1) & ~1
        return next((e for e in self.read_side_effects if start < e[1] and end > e[0]), None)

    def debug_writable(self, addr: int, length: int) -> bool:
        """Read-only debug sessions write RAM, and peripheral registers outside
        the peripheral holes (PMM, flash controller, SYS, DMA). Nothing else."""
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
    peripherals=(0x0100, 0x0940),  # SFRIE1..USBFUNADR, TI's msp430f5529.h
    peripheral_holes=(
        # PMMCTL0..PM5CTL0: PMMCTL0's password (written 0xA5, read 0x96) makes a
        # byte write, done as a word read-modify-write, a reset; core voltage, SVS
        (0x0120, 0x0140, "PMM"),
        (0x0140, 0x0150, "flash controller"),  # FCTL1..FCTL4
        (0x0180, 0x01A0, "SYS"),  # SYSCTL (SYSRIVECT), SYSBSLC, ...: vectors, BSL protection
        (0x0500, 0x0540, "DMA"),  # DMACTL0..DMA2SZ: a transfer could write FCTL
    ),
    # the peripheral space 0x0000-0x0FFF outside `peripherals`
    vacant=((0x0000, 0x0100), (0x0940, 0x1000)),
    bsl=(0x1000, 0x1800),
    info=(0x1800, 0x1A00),
    tlv=(0x1A00, 0x1B00),
    ram=(0x1C00, 0x4400),
    # Addresses from TI's msp430f5529.h; reading an interrupt vector register
    # clears the flag it reports (SLAU208).
    read_side_effects=(
        (0x0188, 0x018A, "SYSJMBI0", "clears JMBINxFG"),
        (0x018A, 0x018C, "SYSJMBI1", "clears JMBINxFG"),
        (0x0198, 0x019A, "SYSBERRIV", "clears the highest pending flag"),
        (0x019A, 0x019C, "SYSUNIV", "clears the highest pending flag"),
        (0x019C, 0x019E, "SYSSNIV", "clears the highest pending flag"),
        (0x019E, 0x01A0, "SYSRSTIV", "clears the highest pending flag"),
        (0x020E, 0x0210, "P1IV", "clears the highest pending flag"),
        (0x021E, 0x0220, "P2IV", "clears the highest pending flag"),
        (0x036E, 0x0370, "TA0IV", "clears the highest pending flag"),
        (0x03AE, 0x03B0, "TA1IV", "clears the highest pending flag"),
        (0x03EE, 0x03F0, "TB0IV", "clears the highest pending flag"),
        (0x042E, 0x0430, "TA2IV", "clears the highest pending flag"),
        (0x04AE, 0x04B0, "RTCIV", "clears the highest pending flag"),
        (0x050E, 0x0510, "DMAIV", "clears the highest pending flag"),
        (0x05CC, 0x05CE, "UCA0RXBUF", "clears UCRXIFG and the receive error flags"),
        (0x05DE, 0x05E0, "UCA0IV", "clears the highest pending flag"),
        (0x05EC, 0x05EE, "UCB0RXBUF", "clears UCRXIFG and the receive error flags"),
        (0x05FE, 0x0600, "UCB0IV", "clears the highest pending flag"),
        (0x060C, 0x060E, "UCA1RXBUF", "clears UCRXIFG and the receive error flags"),
        (0x061E, 0x0620, "UCA1IV", "clears the highest pending flag"),
        (0x062C, 0x062E, "UCB1RXBUF", "clears UCRXIFG and the receive error flags"),
        (0x063E, 0x0640, "UCB1IV", "clears the highest pending flag"),
        (0x070E, 0x0710, "ADC12IV", "clears ADC12OV or ADC12TOV when highest pending"),
        (0x0720, 0x0740, "ADC12MEM0-ADC12MEM15", "clears ADC12IFGx"),
        (0x08CE, 0x08D0, "CBIV", "clears the highest pending flag"),
        (0x0932, 0x0934, "USBVECINT", "clears the highest pending flag"),
    ),
)

DEVICES = {d.device_id: d for d in (MSP430F5529,)}


def lookup(device_id: int):
    return DEVICES.get(device_id)
