#pragma once

#include "Common.hpp"
#include "StringView.hpp"

namespace koshka::arithmetic_internal {

struct radix_prefix
{
  u32 radix;
  usize prefix_length;
};

alwaysinline pure fn count_leading_digits(StringView text, u32 radix) wontthrow
    -> usize
{
  usize length = 0;

  while (length < text.length) {
    let const current_byte = text[length];
    u32 digit;
    if (current_byte >= '0' && current_byte <= '9')
      digit = static_cast<u32>(current_byte - '0');
    else if (current_byte >= 'a' && current_byte <= 'f')
      digit = static_cast<u32>(current_byte - 'a') + 10;
    else if (current_byte >= 'A' && current_byte <= 'F')
      digit = static_cast<u32>(current_byte - 'A') + 10;
    else
      break;
    if (digit >= radix) break;
    length++;
  }

  return length;
}

alwaysinline pure fn detect_radix_prefix(StringView body) wontthrow
    -> radix_prefix
{
  if (body.length >= 2 && body[0] == '0' && (body[1] == 'x' || body[1] == 'X'))
    return {16, 2};
  if (body.length >= 2 && body[0] == '0' && (body[1] == 'b' || body[1] == 'B'))
    return {2, 2};
  if (body.length >= 1 && body[0] == '0') return {8, 0};
  return {10, 0};
}

alwaysinline fn skip_spaces(StringView source, usize &position) wontthrow
    -> void
{
  while (position < source.length &&
         (source[position] == ' ' || source[position] == '\t' ||
          source[position] == '\n' || source[position] == '\r'))
    position++;
}

alwaysinline fn starts_with(StringView source, usize &position,
                            StringView operation) wontthrow -> bool
{
  skip_spaces(source, position);
  if (position + operation.length > source.length) return false;

  for (usize index = 0; index < operation.length; index++)
    if (source[position + index] != operation[index]) return false;

  return true;
}

alwaysinline fn consume(StringView source, usize &position,
                        StringView operation) wontthrow -> bool
{
  if (!starts_with(source, position, operation)) return false;
  position += operation.length;
  return true;
}

} /* namespace koshka::arithmetic_internal */
