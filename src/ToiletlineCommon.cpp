/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements UTF-8 position conversion shared by the interactive
 * editor and noninteractive stubs. It remains outside either implementation
 * so both build configurations use the same codepoint-to-byte mapping.
 */

#include "Toiletline.hpp"

namespace toiletline {

fn byte_offset_of_codepoint(const char *bytes, usize byte_length,
                            usize codepoint_index) -> usize
{
  usize byte_offset = 0;
  usize seen_codepoints = 0;
  while (byte_offset < byte_length && seen_codepoints < codepoint_index) {
    if ((static_cast<unsigned char>(bytes[byte_offset]) & 0xC0) != 0x80)
      seen_codepoints += 1;
    byte_offset += 1;
  }
  /* Step over the trailing continuation bytes of the last counted codepoint. */
  while (byte_offset < byte_length &&
         (static_cast<unsigned char>(bytes[byte_offset]) & 0xC0) == 0x80)
    byte_offset += 1;
  return byte_offset;
}

} /* namespace toiletline */
