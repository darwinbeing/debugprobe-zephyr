#include "msp430/sbw_tap.hpp"

namespace msp430 {

bool SbwTap::open() { return false; }
void SbwTap::enter(Entry) {}
void SbwTap::reset() {}
uint32_t SbwTap::scan(bool, unsigned, uint32_t) { return 0; }
uint8_t SbwTap::ir_shift(uint8_t) { return 0; }
uint16_t SbwTap::dr_shift16(uint16_t) { return 0; }
uint32_t SbwTap::dr_shift20(uint32_t) { return 0; }
void SbwTap::set_tclk() {}
void SbwTap::clr_tclk() {}

} // namespace msp430
