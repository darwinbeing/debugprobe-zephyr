#pragma once
/* RAM funclets from TI slau320 (Replicator430Xv2/FlashWrite.c, FlashErase.c).
 * Position-independent MSP430 code that runs in the target's RAM and talks to
 * the probe through the JTAG mailbox.
 *
 * Layout of both arrays: word 0 = entry offset in bytes, words 2.. = parameters
 * patched by the loader:
 *   erase: [2] address low, [3] address high, [4] FCTL1 erase mode, [5] FCTL3 key
 *   write: [2] address low, [3] address high, [4] length low, [5] length high
 *          (in words), [6] FCTL3 key
 */
#include <cstddef>
#include <cstdint>

namespace msp430 {

inline constexpr size_t kFlashWriteWords = 120;
inline constexpr size_t kFlashEraseWords = 89;

extern const uint16_t kFlashWriteFunclet[kFlashWriteWords];
extern const uint16_t kFlashEraseFunclet[kFlashEraseWords];

} // namespace msp430
