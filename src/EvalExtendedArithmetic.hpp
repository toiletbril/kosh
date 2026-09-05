/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file declares the compact ArithmeticValue and radix helpers used by
 * extended arithmetic. Small values remain inline, while promoted integers
 * and fixed-scale decimals use arena-owned limb storage. The separate header
 * exposes this shared contract. Eval.hpp remains independent of backend
 * details.
 */

#pragma once

#include "ArrayList.hpp"
#include "Common.hpp"
#include "String.hpp"
#include "StringView.hpp"

#include <type_traits>

namespace koshka::arithmetic_internal {

class ArithmeticValue
{
public:
  ArithmeticValue() = default;
  explicit ArithmeticValue(i64 value) wontthrow;

  static fn parse(StringView source, u32 radix, BumpArena &arena) throws
      -> ArithmeticValue;
  static fn parse_decimal(StringView text, BumpArena &arena) throws
      -> ArithmeticValue;

  pure fn is_zero() const wontthrow -> bool;
  pure fn is_negative() const wontthrow -> bool;
  pure fn wrapped_i64() const wontthrow -> i64;
  fn checked_i64() const throws -> i64;
  fn to_string(Allocator allocator) const throws -> String;
  fn compare(const ArithmeticValue &other, Allocator allocator) const throws
      -> i32;
  pure fn get_decimal_scale() const wontthrow -> u32;
  pure fn is_integer() const wontthrow -> bool;
  fn has_integer_value(BumpArena &arena) const throws -> bool;
  fn copy_magnitude(Allocator allocator) const throws -> ArrayList<u64>;

  static fn add(const ArithmeticValue &left, const ArithmeticValue &right,
                BumpArena &arena) throws -> ArithmeticValue;
  static fn subtract(const ArithmeticValue &left, const ArithmeticValue &right,
                     BumpArena &arena) throws -> ArithmeticValue;
  static fn multiply(const ArithmeticValue &left, const ArithmeticValue &right,
                     BumpArena &arena) throws -> ArithmeticValue;
  static fn divide(const ArithmeticValue &left, const ArithmeticValue &right,
                   BumpArena &arena) throws -> ArithmeticValue;
  static fn modulo(const ArithmeticValue &left, const ArithmeticValue &right,
                   BumpArena &arena) throws -> ArithmeticValue;
  static fn power(const ArithmeticValue &base, const ArithmeticValue &exponent,
                  BumpArena &arena) throws -> ArithmeticValue;
  static fn square_root(const ArithmeticValue &value, u32 decimal_scale,
                        BumpArena &arena) throws -> ArithmeticValue;
  static fn rescale(const ArithmeticValue &value, u32 decimal_scale,
                    BumpArena &arena) throws -> ArithmeticValue;
  static fn shift_left(const ArithmeticValue &value,
                       const ArithmeticValue &count, BumpArena &arena) throws
      -> ArithmeticValue;
  static fn shift_right(const ArithmeticValue &value,
                        const ArithmeticValue &count, BumpArena &arena) throws
      -> ArithmeticValue;
  static fn bit_and(const ArithmeticValue &left, const ArithmeticValue &right,
                    BumpArena &arena) throws -> ArithmeticValue;
  static fn bit_or(const ArithmeticValue &left, const ArithmeticValue &right,
                   BumpArena &arena) throws -> ArithmeticValue;
  static fn bit_xor(const ArithmeticValue &left, const ArithmeticValue &right,
                    BumpArena &arena) throws -> ArithmeticValue;
  static fn bit_not(const ArithmeticValue &value, BumpArena &arena) throws
      -> ArithmeticValue;
  static fn absolute(const ArithmeticValue &value, BumpArena &arena) throws
      -> ArithmeticValue;
  static fn integer_part(const ArithmeticValue &value, BumpArena &arena) throws
      -> ArithmeticValue;

private:
  struct Storage;

  static constexpr u64 PROMOTED_MARKER = 0x7fffc0dec0dec0deULL;
  static constexpr uintptr NEGATIVE_STORAGE_FLAG = 1;

  static fn from_signed_128(i128 value, BumpArena &arena) throws
      -> ArithmeticValue;
  static fn from_magnitude(const u64 *limbs, usize limb_count, bool is_negative,
                           u32 decimal_scale, BumpArena &arena) throws
      -> ArithmeticValue;
  static fn allocate_promoted(usize limb_count, bool is_negative,
                              u32 decimal_scale, BumpArena &arena,
                              u64 *&limbs) throws -> ArithmeticValue;
  static fn from_power_of_two(usize bit_position, bool is_negative,
                              BumpArena &arena) throws -> ArithmeticValue;

  pure fn is_promoted() const wontthrow -> bool;
  pure fn get_storage() const wontthrow -> const Storage *;
  pure fn inline_value() const wontthrow -> i128;
  pure fn limb_count() const wontthrow -> usize;
  pure fn limb_at(usize index) const wontthrow -> u64;

  u64 m_low{0};
  u64 m_high{0};
};

static_assert(sizeof(ArithmeticValue) == 16);
static_assert(std::is_trivially_copyable_v<ArithmeticValue>);

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
