#include "ArbitraryArithmetic.hpp"

#include "Common.hpp"
#include "Debug.hpp"
#include "Errors.hpp"
#include "Utils.hpp"

namespace koshka::arithmetic_internal {

struct alignas(u64) ArithmeticValue::Storage
{
  u32 limb_count{0};
  u32 decimal_scale{0};
};

namespace {

fn make_zero_limbs(usize count, Allocator allocator) throws -> ArrayList<u64>
{
  let limbs = ArrayList<u64>{allocator};
  limbs.reserve(count);

  for (usize index = 0; index < count; index++)
    limbs.push(0);

  return limbs;
}

fn trim_limbs(ArrayList<u64> &limbs) wontthrow -> void
{
  while (!limbs.is_empty() && limbs.back() == 0)
    limbs.pop_back();
}

pure fn compare_limbs(const ArrayList<u64> &left,
                      const ArrayList<u64> &right) wontthrow -> i32
{
  if (left.count() < right.count()) return -1;
  if (left.count() > right.count()) return 1;

  for (usize index = left.count(); index > 0; index--) {
    if (left[index - 1] < right[index - 1]) return -1;
    if (left[index - 1] > right[index - 1]) return 1;
  }

  return 0;
}

fn add_limbs(const ArrayList<u64> &left, const ArrayList<u64> &right,
             Allocator allocator) throws -> ArrayList<u64>
{
  let const result_count =
      left.count() > right.count() ? left.count() : right.count();
  let result = make_zero_limbs(result_count + 1, allocator);
  u64 carry = 0;

  for (usize index = 0; index < result_count; index++) {
    let const left_limb = index < left.count() ? left[index] : 0;
    let const right_limb = index < right.count() ? right[index] : 0;
    let const sum = static_cast<u128>(left_limb) + right_limb + carry;
    result[index] = static_cast<u64>(sum);
    carry = static_cast<u64>(sum >> 64u);
  }

  result[result_count] = carry;
  trim_limbs(result);

  return result;
}

fn subtract_limbs(const ArrayList<u64> &left, const ArrayList<u64> &right,
                  Allocator allocator) throws -> ArrayList<u64>
{
  ASSERT(compare_limbs(left, right) >= 0);

  let result = make_zero_limbs(left.count(), allocator);
  u64 borrow = 0;

  for (usize index = 0; index < left.count(); index++) {
    let const right_limb = index < right.count() ? right[index] : 0;
    let const subtrahend = right_limb + borrow;
    let const did_subtrahend_wrap = subtrahend < right_limb;
    result[index] = left[index] - subtrahend;
    borrow = did_subtrahend_wrap || left[index] < subtrahend;
  }

  trim_limbs(result);

  return result;
}

fn subtract_limbs_in_place(ArrayList<u64> &left,
                           const ArrayList<u64> &right) wontthrow -> void
{
  ASSERT(compare_limbs(left, right) >= 0);
  u64 borrow = 0;

  for (usize index = 0; index < left.count(); index++) {
    let const right_limb = index < right.count() ? right[index] : 0;
    let const subtrahend = right_limb + borrow;
    let const did_subtrahend_wrap = subtrahend < right_limb;
    let const original = left[index];
    left[index] -= subtrahend;
    borrow = did_subtrahend_wrap || original < subtrahend;
  }

  trim_limbs(left);
}

fn multiply_limbs(const ArrayList<u64> &left, const ArrayList<u64> &right,
                  Allocator allocator) throws -> ArrayList<u64>
{
  if (left.is_empty() || right.is_empty()) return ArrayList<u64>{allocator};
  if (left.count() > ArrayList<u64>::MAXIMUM_ELEMENT_COUNT - right.count())
    throw std::bad_alloc{};

  let result = make_zero_limbs(left.count() + right.count(), allocator);

  for (usize left_index = 0; left_index < left.count(); left_index++) {
    u64 carry = 0;

    for (usize right_index = 0; right_index < right.count(); right_index++) {
      let const result_index = left_index + right_index;
      let const product =
          static_cast<u128>(left[left_index]) * right[right_index] +
          result[result_index] + carry;
      result[result_index] = static_cast<u64>(product);
      carry = static_cast<u64>(product >> 64u);
    }

    result[left_index + right.count()] = carry;
  }

  trim_limbs(result);

  return result;
}

fn shift_left_one(ArrayList<u64> &limbs) throws -> void
{
  u64 carry = 0;

  for (usize index = 0; index < limbs.count(); index++) {
    let const next_carry = limbs[index] >> 63u;
    limbs[index] = (limbs[index] << 1u) | carry;
    carry = next_carry;
  }

  if (carry != 0) limbs.push(carry);
}

fn add_small(ArrayList<u64> &limbs, u64 value) throws -> void
{
  if (value == 0) return;
  if (limbs.is_empty()) {
    limbs.push(value);
    return;
  }

  u64 carry = value;

  for (usize index = 0; index < limbs.count() && carry != 0; index++) {
    let const previous = limbs[index];
    limbs[index] += carry;
    carry = limbs[index] < previous;
  }

  if (carry != 0) limbs.push(carry);
}

fn multiply_add_small(ArrayList<u64> &limbs, u64 multiplier, u64 addend) throws
    -> void
{
  u64 carry = addend;

  for (usize index = 0; index < limbs.count(); index++) {
    let const product = static_cast<u128>(limbs[index]) * multiplier + carry;
    limbs[index] = static_cast<u64>(product);
    carry = static_cast<u64>(product >> 64u);
  }

  if (carry != 0) limbs.push(carry);
  if (limbs.is_empty()) limbs.push(0);
}

fn multiply_power_of_ten(ArrayList<u64> &limbs, u32 decimal_count) throws
    -> void
{
  constexpr u64 DECIMAL_CHUNK_BASE = 10000000000000000000ULL;

  while (decimal_count >= 19) {
    multiply_add_small(limbs, DECIMAL_CHUNK_BASE, 0);
    decimal_count -= 19;
  }

  u64 multiplier = 1;
  for (u32 index = 0; index < decimal_count; index++)
    multiplier *= 10;
  multiply_add_small(limbs, multiplier, 0);
  trim_limbs(limbs);
}

fn get_scaled_magnitude(const ArithmeticValue &value, u32 decimal_scale,
                        Allocator allocator) throws -> ArrayList<u64>
{
  let magnitude = value.copy_magnitude(allocator);
  multiply_power_of_ten(magnitude, decimal_scale - value.get_decimal_scale());
  return magnitude;
}

fn divide_small(ArrayList<u64> &limbs, u64 divisor) wontthrow -> u64
{
  u128 remainder = 0;

  for (usize index = limbs.count(); index > 0; index--) {
    let const current = (remainder << 64u) | limbs[index - 1];
    limbs[index - 1] = static_cast<u64>(current / divisor);
    remainder = current % divisor;
  }

  trim_limbs(limbs);

  return static_cast<u64>(remainder);
}

pure fn magnitude_bit_length(const ArrayList<u64> &limbs) wontthrow -> usize
{
  if (limbs.is_empty()) return 0;
  let const top = limbs.back();
  return (limbs.count() - 1) * 64 +
         static_cast<usize>(64 - __builtin_clzll(top));
}

fn magnitude_divide(const ArrayList<u64> &dividend,
                    const ArrayList<u64> &divisor, ArrayList<u64> &quotient,
                    ArrayList<u64> &remainder, Allocator allocator) throws
    -> void
{
  ASSERT(!divisor.is_empty());

  if (compare_limbs(dividend, divisor) < 0) {
    remainder = dividend.clone();
    quotient.clear();
    return;
  }

  if (divisor.count() == 1) {
    quotient = dividend.clone();
    let const remainder_value = divide_small(quotient, divisor[0]);
    remainder.clear();
    if (remainder_value != 0) remainder.push(remainder_value);
    return;
  }

  let const bit_count = magnitude_bit_length(dividend);
  quotient = make_zero_limbs((bit_count + 63) / 64, allocator);
  remainder.clear();

  for (usize bit_position = bit_count; bit_position > 0; bit_position--) {
    shift_left_one(remainder);
    let const source_position = bit_position - 1;
    let const source_limb = source_position / 64;
    let const source_bit = source_position % 64;
    if (((dividend[source_limb] >> source_bit) & 1u) != 0)
      add_small(remainder, 1);

    if (compare_limbs(remainder, divisor) >= 0) {
      subtract_limbs_in_place(remainder, divisor);
      quotient[source_limb] |= u64{1} << source_bit;
    }
  }

  trim_limbs(quotient);
  trim_limbs(remainder);
}

fn shift_magnitude_left(const ArrayList<u64> &source, usize bit_count,
                        Allocator allocator) throws -> ArrayList<u64>
{
  if (source.is_empty()) return ArrayList<u64>{allocator};
  let const whole_limbs = bit_count / 64;
  let const partial_bits = bit_count % 64;
  let const extra_limb = partial_bits == 0 ? 0 : 1;
  if (whole_limbs > ArrayList<u64>::MAXIMUM_ELEMENT_COUNT ||
      source.count() > ArrayList<u64>::MAXIMUM_ELEMENT_COUNT - whole_limbs ||
      source.count() + whole_limbs >
          ArrayList<u64>::MAXIMUM_ELEMENT_COUNT - extra_limb)
  {
    throw std::bad_alloc{};
  }

  let result =
      make_zero_limbs(source.count() + whole_limbs + extra_limb, allocator);
  u64 carry = 0;

  for (usize index = 0; index < source.count(); index++) {
    if (partial_bits == 0) {
      result[index + whole_limbs] = source[index];
      continue;
    }

    let const shifted = static_cast<u128>(source[index]) << partial_bits;
    result[index + whole_limbs] = static_cast<u64>(shifted) | carry;
    carry = static_cast<u64>(shifted >> 64u);
  }

  if (partial_bits != 0) result[source.count() + whole_limbs] = carry;
  trim_limbs(result);

  return result;
}

fn shift_magnitude_right(const ArrayList<u64> &source, usize bit_count,
                         Allocator allocator) throws -> ArrayList<u64>
{
  let const whole_limbs = bit_count / 64;
  let const partial_bits = bit_count % 64;
  if (whole_limbs >= source.count()) return ArrayList<u64>{allocator};

  let result = make_zero_limbs(source.count() - whole_limbs, allocator);
  u64 carry = 0;

  for (usize source_index = source.count(); source_index > whole_limbs;
       source_index--)
  {
    let const limb = source[source_index - 1];
    let const result_index = source_index - 1 - whole_limbs;
    if (partial_bits == 0) {
      result[result_index] = limb;
      continue;
    }

    result[result_index] = (limb >> partial_bits) | carry;
    carry = limb << (64 - partial_bits);
  }

  trim_limbs(result);

  return result;
}

pure fn has_discarded_bits(const ArrayList<u64> &source,
                           usize bit_count) wontthrow -> bool
{
  let const whole_limbs = bit_count / 64;
  let const partial_bits = bit_count % 64;
  let const complete_count =
      whole_limbs < source.count() ? whole_limbs : source.count();

  for (usize index = 0; index < complete_count; index++)
    if (source[index] != 0) return true;

  return partial_bits != 0 && whole_limbs < source.count() &&
         (source[whole_limbs] & ((u64{1} << partial_bits) - 1)) != 0;
}

fn twos_complement_limbs(const ArithmeticValue &value, usize width,
                         Allocator allocator) throws -> ArrayList<u64>
{
  let result = make_zero_limbs(width, allocator);
  let magnitude = value.copy_magnitude(allocator);

  for (usize index = 0; index < magnitude.count() && index < width; index++)
    result[index] = magnitude[index];

  if (!value.is_negative()) return result;

  for (usize index = 0; index < result.count(); index++)
    result[index] = ~result[index];
  add_small(result, 1);
  while (result.count() > width)
    result.pop_back();

  return result;
}

} // namespace

ArithmeticValue::ArithmeticValue(i64 value) wontthrow
    : m_low{static_cast<u64>(value)},
      m_high{value < 0 ? ~u64{0} : u64{0}}
{}

pure fn ArithmeticValue::is_promoted() const wontthrow -> bool
{
  return m_high == PROMOTED_MARKER;
}

pure fn ArithmeticValue::get_storage() const wontthrow -> const Storage *
{
  ASSERT(is_promoted());

  return reinterpret_cast<const Storage *>(static_cast<uintptr>(m_low) &
                                           ~NEGATIVE_STORAGE_FLAG);
}

pure fn ArithmeticValue::inline_value() const wontthrow -> i128
{
  ASSERT(!is_promoted());
  let const bits = (static_cast<u128>(m_high) << 64u) | m_low;
  return static_cast<i128>(bits);
}

pure fn ArithmeticValue::limb_count() const wontthrow -> usize
{
  if (is_promoted()) return get_storage()->limb_count;

  let const value = inline_value();
  let const magnitude =
      value < 0 ? u128{0} - static_cast<u128>(value) : static_cast<u128>(value);
  if ((magnitude >> 64u) != 0) return 2;
  return magnitude == 0 ? 0 : 1;
}

pure fn ArithmeticValue::limb_at(usize index) const wontthrow -> u64
{
  ASSERT(index < limb_count());
  if (is_promoted())
    return reinterpret_cast<const u64 *>(get_storage() + 1)[index];

  let const value = inline_value();
  let const magnitude =
      value < 0 ? u128{0} - static_cast<u128>(value) : static_cast<u128>(value);
  return static_cast<u64>(magnitude >> (index * 64));
}

fn ArithmeticValue::copy_magnitude(Allocator allocator) const throws
    -> ArrayList<u64>
{
  let result = ArrayList<u64>{allocator};
  result.reserve(limb_count());

  for (usize index = 0; index < limb_count(); index++)
    result.push(limb_at(index));

  return result;
}

fn ArithmeticValue::from_signed_128(i128 value, BumpArena &arena) throws
    -> ArithmeticValue
{
  let const bits = static_cast<u128>(value);
  let result = ArithmeticValue{};
  result.m_low = static_cast<u64>(bits);
  result.m_high = static_cast<u64>(bits >> 64u);
  if (result.m_high != PROMOTED_MARKER) return result;

  let const is_negative = value < 0;
  let const magnitude = is_negative ? u128{0} - bits : bits;
  const u64 limbs[] = {static_cast<u64>(magnitude),
                       static_cast<u64>(magnitude >> 64u)};
  return from_magnitude(limbs, limbs[1] == 0 ? 1 : 2, is_negative, 0, arena);
}

fn ArithmeticValue::from_magnitude(const u64 *limbs, usize count,
                                   bool is_negative, u32 decimal_scale,
                                   BumpArena &arena) throws -> ArithmeticValue
{
  while (count > 0 && limbs[count - 1] == 0)
    count--;
  if (count == 0) is_negative = false;
  if (count == 0 && decimal_scale == 0) return ArithmeticValue{};

  if (decimal_scale == 0 && count <= 2) {
    let const low = count > 0 ? limbs[0] : 0;
    let const high = count == 2 ? limbs[1] : 0;
    let const magnitude = (static_cast<u128>(high) << 64u) | low;
    let const positive_limit = (u128{1} << 127u) - 1;
    let const negative_limit = u128{1} << 127u;
    if ((!is_negative && magnitude <= positive_limit) ||
        (is_negative && magnitude <= negative_limit))
    {
      let const signed_value =
          static_cast<i128>(is_negative ? u128{0} - magnitude : magnitude);
      let result = ArithmeticValue{};
      result.m_low = static_cast<u64>(static_cast<u128>(signed_value));
      result.m_high = static_cast<u64>(static_cast<u128>(signed_value) >> 64u);
      if (result.m_high != PROMOTED_MARKER) return result;
    }
  }

  u64 *destination;
  let result =
      allocate_promoted(count, is_negative, decimal_scale, arena, destination);
  if (count > 0) std::memcpy(destination, limbs, count * sizeof(u64));

  return result;
}

fn ArithmeticValue::allocate_promoted(usize limb_count, bool is_negative,
                                      u32 decimal_scale, BumpArena &arena,
                                      u64 *&limbs) throws -> ArithmeticValue
{
  static_assert(sizeof(Storage) == 8);
  static_assert(alignof(Storage) >= alignof(u64));

  if (limb_count > static_cast<usize>(~u32{0}) ||
      limb_count > (SIZE_MAX - sizeof(Storage)) / sizeof(u64))
    throw std::bad_alloc{};
  let const allocation_length = sizeof(Storage) + limb_count * sizeof(u64);
  let const storage = static_cast<Storage *>(
      bump_allocator(arena).raw_alloc(allocation_length, alignof(Storage)));
  if (storage == nullptr) throw std::bad_alloc{};
  new (storage) Storage{};
  storage->limb_count = static_cast<u32>(limb_count);
  storage->decimal_scale = decimal_scale;
  limbs = reinterpret_cast<u64 *>(storage + 1);

  let result = ArithmeticValue{};
  result.m_low = static_cast<u64>(reinterpret_cast<uintptr>(storage) |
                                  (is_negative ? NEGATIVE_STORAGE_FLAG : 0));
  result.m_high = PROMOTED_MARKER;

  return result;
}

fn ArithmeticValue::from_power_of_two(usize bit_position, bool is_negative,
                                      BumpArena &arena) throws
    -> ArithmeticValue
{
  if ((!is_negative && bit_position < 127) ||
      (is_negative && bit_position <= 127))
  {
    let const magnitude = u128{1} << bit_position;
    return from_signed_128(
        static_cast<i128>(is_negative ? u128{0} - magnitude : magnitude),
        arena);
  }

  let const limb_position = bit_position / 64;
  let const limb_count = limb_position + 1;
  u64 *limbs;
  let result = allocate_promoted(limb_count, is_negative, 0, arena, limbs);
  std::memset(limbs, 0, limb_count * sizeof(u64));
  limbs[limb_position] = u64{1} << (bit_position % 64);

  return result;
}

fn ArithmeticValue::parse(StringView text, u32 radix, BumpArena &arena) throws
    -> ArithmeticValue
{
  let const allocator = bump_allocator(arena);
  let body = text;
  let is_negative = false;
  if (!body.is_empty() && (body[0] == '+' || body[0] == '-')) {
    is_negative = body[0] == '-';
    body = body.substring(1);
  }

  let magnitude = ArrayList<u64>{allocator};

  if (radix == 10) {
    constexpr u64 DECIMAL_CHUNK_BASE = 10000000000000000000ULL;
    let const first_chunk_length =
        body.length % 19 == 0 ? usize{19} : body.length % 19;
    usize position = 0;

    while (position < body.length) {
      let const chunk_length = position == 0 ? first_chunk_length : usize{19};
      u64 chunk = 0;

      for (usize index = 0; index < chunk_length; index++)
        chunk = chunk * 10 + static_cast<u64>(body[position + index] - '0');

      let const multiplier = position == 0 ? u64{1} : DECIMAL_CHUNK_BASE;
      multiply_add_small(magnitude, multiplier, chunk);
      position += chunk_length;
    }

    trim_limbs(magnitude);
    return from_magnitude(magnitude.begin(), magnitude.count(), is_negative, 0,
                          arena);
  }

  for (usize index = 0; index < body.length; index++) {
    let const byte = body[index];
    i32 digit = -1;
    if (byte >= '0' && byte <= '9')
      digit = byte - '0';
    else if (byte >= 'a' && byte <= 'z')
      digit = byte - 'a' + 10;
    else if (byte >= 'A' && byte <= 'Z')
      digit = radix <= 36 ? byte - 'A' + 10 : byte - 'A' + 36;
    else if (byte == '@')
      digit = 62;
    else if (byte == '_')
      digit = 63;
    if (digit < 0 || static_cast<u32>(digit) >= radix) break;
    multiply_add_small(magnitude, radix, static_cast<u64>(digit));
  }

  trim_limbs(magnitude);

  return from_magnitude(magnitude.begin(), magnitude.count(), is_negative, 0,
                        arena);
}

fn ArithmeticValue::parse_decimal(StringView text, BumpArena &arena) throws
    -> ArithmeticValue
{
  let const allocator = bump_allocator(arena);
  let body = text;
  let is_negative = false;
  if (!body.is_empty() && (body[0] == '+' || body[0] == '-')) {
    is_negative = body[0] == '-';
    body = body.substring(1);
  }

  let magnitude = ArrayList<u64>{allocator};
  u32 decimal_scale = 0;
  let has_decimal_point = false;

  for (usize index = 0; index < body.length; index++) {
    let const byte = body[index];
    if (byte == '.') {
      has_decimal_point = true;
      continue;
    }
    multiply_add_small(magnitude, 10, static_cast<u64>(byte - '0'));
    if (has_decimal_point) {
      if (decimal_scale == ~u32{0}) throw std::bad_alloc{};
      decimal_scale++;
    }
  }

  trim_limbs(magnitude);
  return from_magnitude(magnitude.begin(), magnitude.count(), is_negative,
                        decimal_scale, arena);
}

pure fn ArithmeticValue::is_zero() const wontthrow -> bool
{
  return is_promoted() ? limb_count() == 0 : m_low == 0 && m_high == 0;
}

pure fn ArithmeticValue::get_decimal_scale() const wontthrow -> u32
{
  if (!is_promoted()) return 0;

  return get_storage()->decimal_scale;
}

pure fn ArithmeticValue::is_integer() const wontthrow -> bool
{
  return get_decimal_scale() == 0;
}

pure fn ArithmeticValue::is_negative() const wontthrow -> bool
{
  if (is_promoted()) return (m_low & NEGATIVE_STORAGE_FLAG) != 0;

  return inline_value() < 0;
}

pure fn ArithmeticValue::wrapped_i64() const wontthrow -> i64
{
  if (!is_promoted()) return static_cast<i64>(m_low);
  let const low = limb_at(0);
  return static_cast<i64>(is_negative() ? u64{0} - low : low);
}

fn ArithmeticValue::checked_i64() const throws -> i64
{
  if (!is_promoted()) {
    let const value = inline_value();
    if (value >= INT64_MIN && value <= INT64_MAX)
      return static_cast<i64>(value);
  }

  throw ErrorWithDetails{"Arithmetic value is out of range",
                         "This operation requires a signed 64-bit value"};
}

fn ArithmeticValue::compare(const ArithmeticValue &other,
                            Allocator allocator) const throws -> i32
{
  let const left_negative = is_negative();
  let const right_negative = other.is_negative();
  if (left_negative != right_negative) return left_negative ? -1 : 1;
  if (get_decimal_scale() == other.get_decimal_scale()) {
    i32 ordering = 0;
    if (limb_count() != other.limb_count()) {
      ordering = limb_count() < other.limb_count() ? -1 : 1;
    } else {
      for (usize position = limb_count(); position > 0; position--) {
        let const left_limb = limb_at(position - 1);
        let const right_limb = other.limb_at(position - 1);
        if (left_limb == right_limb) continue;
        ordering = left_limb < right_limb ? -1 : 1;
        break;
      }
    }

    return left_negative ? -ordering : ordering;
  }
  let const decimal_scale = get_decimal_scale() > other.get_decimal_scale()
                                ? get_decimal_scale()
                                : other.get_decimal_scale();
  let const left_magnitude =
      get_scaled_magnitude(*this, decimal_scale, allocator);
  let const right_magnitude =
      get_scaled_magnitude(other, decimal_scale, allocator);
  let const ordering = compare_limbs(left_magnitude, right_magnitude);

  return left_negative ? -ordering : ordering;
}

fn ArithmeticValue::to_string(Allocator allocator) const throws -> String
{
  let chunks = ArrayList<u64>{allocator};
  constexpr u64 DECIMAL_CHUNK_BASE = 10000000000000000000ULL;

  for (usize limb_position = limb_count(); limb_position > 0; limb_position--) {
    u64 carry = limb_at(limb_position - 1);

    for (usize chunk_position = 0; chunk_position < chunks.count();
         chunk_position++)
    {
      let const current =
          (static_cast<u128>(chunks[chunk_position]) << 64u) | carry;
      chunks[chunk_position] = static_cast<u64>(current % DECIMAL_CHUNK_BASE);
      carry = static_cast<u64>(current / DECIMAL_CHUNK_BASE);
    }

    while (carry != 0) {
      chunks.push(carry % DECIMAL_CHUNK_BASE);
      carry /= DECIMAL_CHUNK_BASE;
    }
  }

  let digits = String{allocator};
  char buffer[32];
  if (chunks.is_empty()) {
    digits.push('0');
  } else {
    let length = static_cast<usize>(
        std::snprintf(buffer, sizeof(buffer), "%llu",
                      static_cast<unsigned long long>(chunks.back())));
    digits.append(StringView{buffer, length});

    for (usize index = chunks.count() - 1; index > 0; index--) {
      length = static_cast<usize>(
          std::snprintf(buffer, sizeof(buffer), "%019llu",
                        static_cast<unsigned long long>(chunks[index - 1])));
      digits.append(StringView{buffer, length});
    }
  }

  let const decimal_scale = static_cast<usize>(get_decimal_scale());
  if (decimal_scale == 0 && !is_negative()) return digits;

  let result = String{allocator};
  if (is_negative()) result.push('-');
  if (decimal_scale == 0) {
    result.append(digits.view());
    return result;
  }
  if (digits.count() <= decimal_scale) {
    result.append("0.");
    for (usize index = digits.count(); index < decimal_scale; index++)
      result.push('0');
    result.append(digits.view());
    return result;
  }

  let const integer_length = digits.count() - decimal_scale;
  result.append(digits.view().substring_of_length(0, integer_length));
  result.push('.');
  result.append(digits.view().substring(integer_length));
  return result;
}

fn ArithmeticValue::add(const ArithmeticValue &left,
                        const ArithmeticValue &right, BumpArena &arena) throws
    -> ArithmeticValue
{
  let const allocator = bump_allocator(arena);
  if (!left.is_promoted() && !right.is_promoted()) {
    i128 result;
    if (!__builtin_add_overflow(left.inline_value(), right.inline_value(),
                                &result))
      return from_signed_128(result, arena);
  }

  let const decimal_scale = left.get_decimal_scale() > right.get_decimal_scale()
                                ? left.get_decimal_scale()
                                : right.get_decimal_scale();
  let const left_magnitude =
      get_scaled_magnitude(left, decimal_scale, allocator);
  let const right_magnitude =
      get_scaled_magnitude(right, decimal_scale, allocator);
  let result = ArrayList<u64>{allocator};
  let is_negative = false;

  if (left.is_negative() == right.is_negative()) {
    result = add_limbs(left_magnitude, right_magnitude, allocator);
    is_negative = left.is_negative();
  } else {
    let const ordering = compare_limbs(left_magnitude, right_magnitude);
    if (ordering == 0)
      return from_magnitude(NULL, 0, false, decimal_scale, arena);
    if (ordering > 0) {
      result = subtract_limbs(left_magnitude, right_magnitude, allocator);
      is_negative = left.is_negative();
    } else {
      result = subtract_limbs(right_magnitude, left_magnitude, allocator);
      is_negative = right.is_negative();
    }
  }

  return from_magnitude(result.begin(), result.count(), is_negative,
                        decimal_scale, arena);
}

fn ArithmeticValue::subtract(const ArithmeticValue &left,
                             const ArithmeticValue &right,
                             BumpArena &arena) throws -> ArithmeticValue
{
  let const allocator = bump_allocator(arena);
  if (!left.is_promoted() && !right.is_promoted()) {
    i128 result;
    if (!__builtin_sub_overflow(left.inline_value(), right.inline_value(),
                                &result))
      return from_signed_128(result, arena);
  }

  let right_magnitude = right.copy_magnitude(allocator);
  let const negated =
      from_magnitude(right_magnitude.begin(), right_magnitude.count(),
                     !right.is_negative(), right.get_decimal_scale(), arena);
  return add(left, negated, arena);
}

fn ArithmeticValue::multiply(const ArithmeticValue &left,
                             const ArithmeticValue &right,
                             BumpArena &arena) throws -> ArithmeticValue
{
  let const allocator = bump_allocator(arena);
  if (left.get_decimal_scale() > ~u32{0} - right.get_decimal_scale())
    throw std::bad_alloc{};
  let const decimal_scale =
      left.get_decimal_scale() + right.get_decimal_scale();
  if (left.is_zero() || right.is_zero())
    return from_magnitude(NULL, 0, false, decimal_scale, arena);
  if (!left.is_promoted() && !right.is_promoted()) {
    i128 result;
    if (!__builtin_mul_overflow(left.inline_value(), right.inline_value(),
                                &result))
      return from_signed_128(result, arena);
  }

  let const left_magnitude = left.copy_magnitude(allocator);
  let const right_magnitude = right.copy_magnitude(allocator);
  let result = multiply_limbs(left_magnitude, right_magnitude, allocator);
  return from_magnitude(result.begin(), result.count(),
                        left.is_negative() != right.is_negative(),
                        decimal_scale, arena);
}

fn ArithmeticValue::divide(const ArithmeticValue &left,
                           const ArithmeticValue &right,
                           BumpArena &arena) throws -> ArithmeticValue
{
  let const allocator = bump_allocator(arena);
  ASSERT(!right.is_zero());
  if (!left.is_promoted() && !right.is_promoted()) {
    let const left_value = left.inline_value();
    let const right_value = right.inline_value();
    let const minimum = static_cast<i128>(u128{1} << 127u);
    if (left_value == minimum && right_value == -1) {
      const u64 magnitude[] = {0, u64{1} << 63u};
      return from_magnitude(magnitude, 2, false, 0, arena);
    }
    return from_signed_128(left_value / right_value, arena);
  }

  let const decimal_scale = left.get_decimal_scale() > right.get_decimal_scale()
                                ? left.get_decimal_scale()
                                : right.get_decimal_scale();
  let left_magnitude = left.copy_magnitude(allocator);
  let const numerator_scale = static_cast<u64>(decimal_scale) +
                              right.get_decimal_scale() -
                              left.get_decimal_scale();
  if (numerator_scale > ~u32{0}) throw std::bad_alloc{};
  multiply_power_of_ten(left_magnitude, static_cast<u32>(numerator_scale));
  let const right_magnitude = right.copy_magnitude(allocator);
  let quotient = ArrayList<u64>{allocator};
  let remainder = ArrayList<u64>{allocator};
  magnitude_divide(left_magnitude, right_magnitude, quotient, remainder,
                   allocator);
  return from_magnitude(quotient.begin(), quotient.count(),
                        left.is_negative() != right.is_negative(),
                        decimal_scale, arena);
}

fn ArithmeticValue::modulo(const ArithmeticValue &left,
                           const ArithmeticValue &right,
                           BumpArena &arena) throws -> ArithmeticValue
{
  let const allocator = bump_allocator(arena);
  ASSERT(!right.is_zero());
  if (!left.is_promoted() && !right.is_promoted()) {
    let const left_value = left.inline_value();
    let const right_value = right.inline_value();
    let const minimum = static_cast<i128>(u128{1} << 127u);
    if (left_value == minimum && right_value == -1) return ArithmeticValue{};
    return from_signed_128(left_value % right_value, arena);
  }

  let const decimal_scale = left.get_decimal_scale() > right.get_decimal_scale()
                                ? left.get_decimal_scale()
                                : right.get_decimal_scale();
  let const left_magnitude =
      get_scaled_magnitude(left, decimal_scale, allocator);
  let const right_magnitude =
      get_scaled_magnitude(right, decimal_scale, allocator);
  let quotient = ArrayList<u64>{allocator};
  let remainder = ArrayList<u64>{allocator};
  magnitude_divide(left_magnitude, right_magnitude, quotient, remainder,
                   allocator);
  return from_magnitude(remainder.begin(), remainder.count(),
                        left.is_negative(), decimal_scale, arena);
}

fn ArithmeticValue::power(const ArithmeticValue &base,
                          const ArithmeticValue &exponent,
                          BumpArena &arena) throws -> ArithmeticValue
{
  if (!exponent.has_integer_value(arena))
    throw ErrorWithDetails{"Exponent is not an integer",
                           "'**' requires an integer exponent"};
  let const integral_exponent = integer_part(exponent, arena);
  if (integral_exponent.is_negative())
    throw ErrorWithDetails{"Exponent less than 0",
                           "'**' requires a non-negative exponent"};
  if (integral_exponent.is_zero()) return ArithmeticValue{1};
  if (base.is_zero() && base.is_integer()) {
    return ArithmeticValue{};
  }
  if (base.is_integer() && base.limb_count() == 1 && base.limb_at(0) == 1) {
    if (!base.is_negative()) return ArithmeticValue{1};
    return ArithmeticValue{(integral_exponent.limb_at(0) & 1u) != 0 ? -1 : 1};
  }
  if (integral_exponent.limb_count() > 1)
    throw ErrorWithDetails{"Exponent is too large",
                           "The result cannot fit in addressable memory"};

  let const exponent_value = integral_exponent.limb_at(0);
  if (base.is_zero()) {
    if (exponent_value > ~u32{0} / base.get_decimal_scale())
      throw ErrorWithDetails{"Exponent is too large",
                             "The result cannot fit in addressable memory"};
    return from_magnitude(
        NULL, 0, false,
        static_cast<u32>(exponent_value) * base.get_decimal_scale(), arena);
  }
  if (base.is_integer() && base.limb_count() == 1 &&
      __builtin_popcountll(base.limb_at(0)) == 1)
  {
    let const base_bit_position =
        static_cast<usize>(__builtin_ctzll(base.limb_at(0)));
    if (exponent_value > SIZE_MAX / base_bit_position)
      throw ErrorWithDetails{"Exponent is too large",
                             "The result cannot fit in addressable memory"};
    let const result_bit_position =
        static_cast<usize>(exponent_value) * base_bit_position;
    let const result_limb_position = result_bit_position / 64;
    if (result_limb_position >= ArrayList<u64>::MAXIMUM_ELEMENT_COUNT)
      throw ErrorWithDetails{"Exponent is too large",
                             "The result cannot fit in addressable memory"};
    return from_power_of_two(result_bit_position,
                             base.is_negative() && (exponent_value & 1u) != 0,
                             arena);
  }

  u64 remaining = exponent_value;
  let result = ArithmeticValue{1};
  let factor = base;

  while (remaining > 0) {
    if ((remaining & 1u) != 0) result = multiply(result, factor, arena);
    remaining >>= 1u;
    if (remaining != 0) factor = multiply(factor, factor, arena);
  }

  return result;
}

fn ArithmeticValue::square_root(const ArithmeticValue &value, u32 decimal_scale,
                                BumpArena &arena) throws -> ArithmeticValue
{
  if (value.is_negative())
    throw ErrorWithDetails{"Square root operand is negative",
                           "sqrt requires a non-negative value"};
  if (decimal_scale < value.get_decimal_scale())
    decimal_scale = value.get_decimal_scale();
  if (value.is_zero())
    return from_magnitude(NULL, 0, false, decimal_scale, arena);
  let const allocator = bump_allocator(arena);
  let magnitude = value.copy_magnitude(allocator);
  let const scale_growth_wide =
      static_cast<u64>(decimal_scale) * 2 - value.get_decimal_scale();
  if (scale_growth_wide > ~u32{0}) throw std::bad_alloc{};
  let const scale_growth = static_cast<u32>(scale_growth_wide);
  multiply_power_of_ten(magnitude, scale_growth);
  let const scaled =
      from_magnitude(magnitude.begin(), magnitude.count(), false, 0, arena);
  let const highest_limb = scaled.limb_at(scaled.limb_count() - 1);
  let const bit_count = (scaled.limb_count() - 1) * 64 + 64 -
                        static_cast<usize>(__builtin_clzll(highest_limb));
  let estimate = from_power_of_two((bit_count + 1) / 2, false, arena);

  loop
  {
    let const quotient = divide(scaled, estimate, arena);
    let const next =
        divide(add(estimate, quotient, arena), ArithmeticValue{2}, arena);
    if (next.compare(estimate, allocator) >= 0) break;
    estimate = next;
  }

  let const result_magnitude = estimate.copy_magnitude(allocator);
  return from_magnitude(result_magnitude.begin(), result_magnitude.count(),
                        false, decimal_scale, arena);
}

fn ArithmeticValue::shift_left(const ArithmeticValue &value,
                               const ArithmeticValue &count,
                               BumpArena &arena) throws -> ArithmeticValue
{
  let const allocator = bump_allocator(arena);
  if (!value.is_integer() || !count.is_integer())
    throw ErrorWithDetails{"Shift operand is not an integer",
                           "Bit shifts require integer operands"};
  if (count.is_negative())
    throw ErrorWithDetails{"Shift count is negative",
                           "An exact shift requires a non-negative count"};
  if (value.is_zero()) return ArithmeticValue{};
  if (count.limb_count() > 1 ||
      (count.limb_count() == 1 && count.limb_at(0) > SIZE_MAX))
    throw ErrorWithDetails{"Shift count is too large",
                           "The result cannot fit in addressable memory"};

  let const bit_count =
      count.limb_count() == 0 ? 0 : static_cast<usize>(count.limb_at(0));
  let const magnitude = value.copy_magnitude(allocator);
  let result = shift_magnitude_left(magnitude, bit_count, allocator);
  return from_magnitude(result.begin(), result.count(), value.is_negative(), 0,
                        arena);
}

fn ArithmeticValue::shift_right(const ArithmeticValue &value,
                                const ArithmeticValue &count,
                                BumpArena &arena) throws -> ArithmeticValue
{
  let const allocator = bump_allocator(arena);
  if (!value.is_integer() || !count.is_integer())
    throw ErrorWithDetails{"Shift operand is not an integer",
                           "Bit shifts require integer operands"};
  if (count.is_negative())
    throw ErrorWithDetails{"Shift count is negative",
                           "An exact shift requires a non-negative count"};
  if (count.limb_count() > 1 ||
      (count.limb_count() == 1 && count.limb_at(0) > SIZE_MAX))
    return value.is_negative() ? ArithmeticValue{-1} : ArithmeticValue{};

  let const bit_count =
      count.limb_count() == 0 ? 0 : static_cast<usize>(count.limb_at(0));
  let const magnitude = value.copy_magnitude(allocator);
  let result = shift_magnitude_right(magnitude, bit_count, allocator);
  if (value.is_negative() && has_discarded_bits(magnitude, bit_count))
    add_small(result, 1);
  return from_magnitude(result.begin(), result.count(), value.is_negative(), 0,
                        arena);
}

fn ArithmeticValue::bit_and(const ArithmeticValue &left,
                            const ArithmeticValue &right,
                            BumpArena &arena) throws -> ArithmeticValue
{
  let const allocator = bump_allocator(arena);
  if (!left.is_integer() || !right.is_integer())
    throw ErrorWithDetails{"Bitwise operand is not an integer",
                           "Bitwise operations require integer operands"};
  let const width =
      (left.limb_count() > right.limb_count() ? left.limb_count()
                                              : right.limb_count()) +
      1;
  let left_bits = twos_complement_limbs(left, width, allocator);
  let const right_bits = twos_complement_limbs(right, width, allocator);
  for (usize index = 0; index < width; index++)
    left_bits[index] &= right_bits[index];
  let const is_negative = (left_bits.back() >> 63u) != 0;
  if (is_negative) {
    for (usize index = 0; index < width; index++)
      left_bits[index] = ~left_bits[index];
    add_small(left_bits, 1);
  }
  trim_limbs(left_bits);
  return from_magnitude(left_bits.begin(), left_bits.count(), is_negative, 0,
                        arena);
}

fn ArithmeticValue::bit_or(const ArithmeticValue &left,
                           const ArithmeticValue &right,
                           BumpArena &arena) throws -> ArithmeticValue
{
  let const allocator = bump_allocator(arena);
  if (!left.is_integer() || !right.is_integer())
    throw ErrorWithDetails{"Bitwise operand is not an integer",
                           "Bitwise operations require integer operands"};
  let const width =
      (left.limb_count() > right.limb_count() ? left.limb_count()
                                              : right.limb_count()) +
      1;
  let left_bits = twos_complement_limbs(left, width, allocator);
  let const right_bits = twos_complement_limbs(right, width, allocator);
  for (usize index = 0; index < width; index++)
    left_bits[index] |= right_bits[index];
  let const is_negative = (left_bits.back() >> 63u) != 0;
  if (is_negative) {
    for (usize index = 0; index < width; index++)
      left_bits[index] = ~left_bits[index];
    add_small(left_bits, 1);
  }
  trim_limbs(left_bits);
  return from_magnitude(left_bits.begin(), left_bits.count(), is_negative, 0,
                        arena);
}

fn ArithmeticValue::bit_xor(const ArithmeticValue &left,
                            const ArithmeticValue &right,
                            BumpArena &arena) throws -> ArithmeticValue
{
  let const allocator = bump_allocator(arena);
  if (!left.is_integer() || !right.is_integer())
    throw ErrorWithDetails{"Bitwise operand is not an integer",
                           "Bitwise operations require integer operands"};
  let const width =
      (left.limb_count() > right.limb_count() ? left.limb_count()
                                              : right.limb_count()) +
      1;
  let left_bits = twos_complement_limbs(left, width, allocator);
  let const right_bits = twos_complement_limbs(right, width, allocator);
  for (usize index = 0; index < width; index++)
    left_bits[index] ^= right_bits[index];
  let const is_negative = (left_bits.back() >> 63u) != 0;
  if (is_negative) {
    for (usize index = 0; index < width; index++)
      left_bits[index] = ~left_bits[index];
    add_small(left_bits, 1);
  }
  trim_limbs(left_bits);
  return from_magnitude(left_bits.begin(), left_bits.count(), is_negative, 0,
                        arena);
}

fn ArithmeticValue::bit_not(const ArithmeticValue &value,
                            BumpArena &arena) throws -> ArithmeticValue
{
  if (!value.is_integer())
    throw ErrorWithDetails{"Bitwise operand is not an integer",
                           "Bitwise operations require integer operands"};
  return subtract(ArithmeticValue{-1}, value, arena);
}

fn ArithmeticValue::absolute(const ArithmeticValue &value,
                             BumpArena &arena) throws -> ArithmeticValue
{
  if (!value.is_negative()) return value;
  let const magnitude = value.copy_magnitude(bump_allocator(arena));
  return from_magnitude(magnitude.begin(), magnitude.count(), false,
                        value.get_decimal_scale(), arena);
}

fn ArithmeticValue::integer_part(const ArithmeticValue &value,
                                 BumpArena &arena) throws -> ArithmeticValue
{
  let decimal_count = value.get_decimal_scale();
  if (decimal_count == 0) return value;

  let magnitude = value.copy_magnitude(bump_allocator(arena));
  constexpr u64 DECIMAL_CHUNK_BASE = 10000000000000000000ULL;
  while (decimal_count >= 19) {
    divide_small(magnitude, DECIMAL_CHUNK_BASE);
    decimal_count -= 19;
  }

  u64 divisor = 1;
  for (u32 index = 0; index < decimal_count; index++)
    divisor *= 10;
  divide_small(magnitude, divisor);
  return from_magnitude(magnitude.begin(), magnitude.count(),
                        value.is_negative(), 0, arena);
}

fn ArithmeticValue::rescale(const ArithmeticValue &value, u32 decimal_scale,
                            BumpArena &arena) throws -> ArithmeticValue
{
  let const current_scale = value.get_decimal_scale();
  if (decimal_scale == current_scale) return value;
  let magnitude = value.copy_magnitude(bump_allocator(arena));
  if (decimal_scale > current_scale) {
    multiply_power_of_ten(magnitude, decimal_scale - current_scale);
  } else {
    let decimal_count = current_scale - decimal_scale;
    constexpr u64 DECIMAL_CHUNK_BASE = 10000000000000000000ULL;
    while (decimal_count >= 19) {
      divide_small(magnitude, DECIMAL_CHUNK_BASE);
      decimal_count -= 19;
    }
    u64 divisor = 1;
    for (u32 index = 0; index < decimal_count; index++)
      divisor *= 10;
    divide_small(magnitude, divisor);
  }
  return from_magnitude(magnitude.begin(), magnitude.count(),
                        value.is_negative(), decimal_scale, arena);
}

fn ArithmeticValue::has_integer_value(BumpArena &arena) const throws -> bool
{
  if (is_integer()) return true;
  let const integer = integer_part(*this, arena);
  return compare(integer, bump_allocator(arena)) == 0;
}

} /* namespace koshka::arithmetic_internal */
