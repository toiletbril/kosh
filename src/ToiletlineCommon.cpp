/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements UTF-8 length, position, and terminal display-width
 * conversion shared by the interactive editor and noninteractive stubs. It
 * remains outside either implementation so both build configurations use the
 * same text measurement behavior without initializing terminal state.
 */

#include "Toiletline.hpp"
#include "Utils.hpp"

namespace koshka::internal {

struct codepoint_interval
{
  u32 first;
  u32 last;
};

static constexpr codepoint_interval ZERO_WIDTH_INTERVALS[] = {
    {0x0300, 0x036f},
    {0x0483, 0x0489},
    {0x0591, 0x05bd},
    {0x0610, 0x061a},
    {0x064b, 0x065f},
    {0x0670, 0x0670},
    {0x06d6, 0x06dc},
    {0x0e31, 0x0e31},
    {0x0e34, 0x0e3a},
    {0x200b, 0x200f},
    {0x2060, 0x2064},
    {0xfe00, 0xfe0f},
    {0xfe20, 0xfe2f},
};

static constexpr codepoint_interval WIDE_INTERVALS[] = {
    {0x1100,  0x115f },
    {0x2e80,  0x303e },
    {0x3041,  0x33ff },
    {0x3400,  0x4dbf },
    {0x4e00,  0x9fff },
    {0xa000,  0xa4cf },
    {0xac00,  0xd7a3 },
    {0xf900,  0xfaff },
    {0xfe10,  0xfe19 },
    {0xfe30,  0xfe6f },
    {0xff00,  0xff60 },
    {0xffe0,  0xffe6 },
    {0x1f300, 0x1faff},
    {0x20000, 0x3fffd},
};

static fn codepoint_is_in(u32 codepoint, const codepoint_interval *intervals,
                          usize interval_count) -> bool
{
  usize low = 0;
  usize high = interval_count;
  while (low < high) {
    let const middle = low + (high - low) / 2;
    if (codepoint < intervals[middle].first)
      high = middle;
    else if (codepoint > intervals[middle].last)
      low = middle + 1;
    else
      return true;
  }

  return false;
}

static fn codepoint_display_width(u32 codepoint) -> usize
{
  if (codepoint == '\t') return 1;
  if (codepoint < 0x20 || (codepoint >= 0x7f && codepoint < 0xa0)) return 0;
  if (codepoint_is_in(codepoint, ZERO_WIDTH_INTERVALS,
                      countof(ZERO_WIDTH_INTERVALS)))
  {
    return 0;
  }
  if (codepoint_is_in(codepoint, WIDE_INTERVALS, countof(WIDE_INTERVALS)))
    return 2;

  return 1;
}

static fn display_width_walk(StringView text, usize stop_after,
                             usize *out_byte_offset) -> usize
{
  usize width = 0;
  usize byte_offset = 0;
  while (byte_offset < text.length && text[byte_offset] != '\0') {
    if (static_cast<u8>(text[byte_offset]) == 0x1b) {
      byte_offset++;
      if (byte_offset >= text.length) break;
      if (text[byte_offset] == '[') {
        byte_offset++;
        while (byte_offset < text.length && text[byte_offset] != '\0' &&
               (text[byte_offset] < 0x40 || text[byte_offset] > 0x7e))
        {
          byte_offset++;
        }
        if (byte_offset < text.length && text[byte_offset] != '\0')
          byte_offset++;
      } else if (text[byte_offset] == ']') {
        byte_offset++;
        while (byte_offset < text.length && text[byte_offset] != '\0' &&
               static_cast<u8>(text[byte_offset]) != 0x07 &&
               !(byte_offset + 1 < text.length &&
                 static_cast<u8>(text[byte_offset]) == 0x1b &&
                 text[byte_offset + 1] == '\\'))
        {
          byte_offset++;
        }
        if (byte_offset + 1 < text.length &&
            static_cast<u8>(text[byte_offset]) == 0x1b &&
            text[byte_offset + 1] == '\\')
        {
          byte_offset += 2;
        } else if (byte_offset < text.length && text[byte_offset] != '\0') {
          byte_offset++;
        }
      } else if (text[byte_offset] != '\0') {
        byte_offset++;
      }
      continue;
    }

    let const decoded = koshka::utils::decode_utf8(text, byte_offset, 0xfffd);
    let const character_width = codepoint_display_width(decoded.value);
    if (character_width > 0 && width >= stop_after) break;
    width += character_width;
    byte_offset += decoded.length;
  }

  if (out_byte_offset != nullptr) *out_byte_offset = byte_offset;
  return width;
}

} /* namespace koshka::internal */

namespace toiletline {

fn utf8_strlen(const koshka::String &string, usize byte_count) -> usize
{
  let const limited_length =
      byte_count < string.length() ? byte_count : string.length();
  return utf8_strnlen(string.c_str(), limited_length);
}

fn utf8_strnlen(const char *bytes, usize byte_count) -> usize
{
  usize codepoint_count = 0;
  for (usize byte_offset = 0;
       byte_offset < byte_count && bytes[byte_offset] != '\0'; byte_offset++)
  {
    if ((static_cast<u8>(bytes[byte_offset]) & 0xc0) != 0x80) codepoint_count++;
  }

  return codepoint_count;
}

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

fn display_width(StringView text) -> usize
{
  return koshka::internal::display_width_walk(text, static_cast<usize>(-1),
                                              nullptr);
}

fn byte_offset_at_or_before_display_cell(StringView text, usize cell_position,
                                         usize &actual_cell_position) -> usize
{
  usize byte_offset = 0;
  actual_cell_position =
      koshka::internal::display_width_walk(text, cell_position, &byte_offset);
  if (actual_cell_position <= cell_position) return byte_offset;

  usize previous_byte_offset = byte_offset - 1;
  while (previous_byte_offset > 0 &&
         (static_cast<u8>(text[previous_byte_offset]) & 0xc0) == 0x80)
  {
    previous_byte_offset--;
  }

  actual_cell_position = koshka::internal::display_width_walk(
      text.substring_of_length(0, previous_byte_offset), static_cast<usize>(-1),
      nullptr);
  return previous_byte_offset;
}

} /* namespace toiletline */
