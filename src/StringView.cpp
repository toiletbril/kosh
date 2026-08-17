#include "StringView.hpp"

#include "ErrorOr.hpp"
#include "IntBase.hpp"
#include "String.hpp"

namespace koshka {

fn StringView::to_lower_ascii(Allocator allocator) const throws -> String
{
  let result = String{allocator, *this};
  result.lowercase_ascii();

  return result;
}

namespace utils {
fn parse_decimal_i64(StringView text, bool *out_of_range = nullptr) throws
    -> ErrorOr<i64>;
fn parse_decimal_u64(StringView text) throws -> ErrorOr<u64>;
fn parse_integer_in_base(StringView text, int_base base,
                         bool *out_of_range = nullptr) throws -> ErrorOr<i64>;
fn parse_integer_in_base_u64(StringView text, int_base base) throws
    -> ErrorOr<u64>;
} /* namespace utils */

template <class T>
static fn narrow_integer(i64 value) throws -> ErrorOr<T>
{
  static_assert(std::is_integral_v<T>, "narrow_integer targets an integer");
  if constexpr (std::is_same_v<T, i64>) {
    return value;
  } else if constexpr (std::is_signed_v<T>) {
    if (value < static_cast<i64>(std::numeric_limits<T>::min()) ||
        value > static_cast<i64>(std::numeric_limits<T>::max()))
      return Error{"integer value out of range"};
    return static_cast<T>(value);
  } else {
    if (value < 0 || static_cast<u64>(value) >
                         static_cast<u64>(std::numeric_limits<T>::max()))
      return Error{"integer value out of range"};
    return static_cast<T>(value);
  }
}

template <class T>
fn StringView::to() const throws -> ErrorOr<T>
{
  if constexpr (is_tagged_int_v<T>) {
    using U = typename T::underlying;
    if constexpr (std::is_same_v<U, u64>)
      return T{TRY(utils::parse_integer_in_base_u64(*this, T::base))};
    else
      return T{TRY(narrow_integer<U>(
          TRY(utils::parse_integer_in_base(*this, T::base))))};
  } else {
    static_assert(std::is_integral_v<T>, "StringView::to parses an integer");
    if constexpr (std::is_same_v<T, u64>)
      return utils::parse_decimal_u64(*this);
    else
      return narrow_integer<T>(TRY(utils::parse_decimal_i64(*this)));
  }
}

#define KOSH_STRINGVIEW_TO(T) template ErrorOr<T> StringView::to<T>() const;
KOSH_STRINGVIEW_TO(i16)
KOSH_STRINGVIEW_TO(u16)
KOSH_STRINGVIEW_TO(i32)
KOSH_STRINGVIEW_TO(u32)
KOSH_STRINGVIEW_TO(i64)
KOSH_STRINGVIEW_TO(u64)
KOSH_STRINGVIEW_TO(bi16)
KOSH_STRINGVIEW_TO(bi32)
KOSH_STRINGVIEW_TO(bi64)
KOSH_STRINGVIEW_TO(bu16)
KOSH_STRINGVIEW_TO(bu32)
KOSH_STRINGVIEW_TO(bu64)
KOSH_STRINGVIEW_TO(oi16)
KOSH_STRINGVIEW_TO(oi32)
KOSH_STRINGVIEW_TO(oi64)
KOSH_STRINGVIEW_TO(ou16)
KOSH_STRINGVIEW_TO(ou32)
KOSH_STRINGVIEW_TO(ou64)
KOSH_STRINGVIEW_TO(hi16)
KOSH_STRINGVIEW_TO(hi32)
KOSH_STRINGVIEW_TO(hi64)
KOSH_STRINGVIEW_TO(hu16)
KOSH_STRINGVIEW_TO(hu32)
KOSH_STRINGVIEW_TO(hu64)
#undef KOSH_STRINGVIEW_TO

StringView::StringView(const char *cstr) wontthrow
    : data(cstr),
      length(cstr != nullptr ? std::strlen(cstr) : 0)
{}

fn StringView::find_character(char wanted) const wontthrow -> Maybe<usize>
{
  if (length == 0) return None;
  let const found =
      std::memchr(data, static_cast<unsigned char>(wanted), length);
  if (found == nullptr) return None;

  return static_cast<usize>(static_cast<const char *>(found) - data);
}

fn StringView::substring(usize start) const wontthrow -> StringView
{
  if (start >= length)
    return StringView{data == nullptr ? nullptr : data + length, 0};
  return StringView{data + start, length - start};
}

fn StringView::substring_of_length(usize start, usize count) const wontthrow
    -> StringView
{
  if (start >= length)
    return StringView{data == nullptr ? nullptr : data + length, 0};
  usize remaining = length - start;

  return StringView{data + start, count < remaining ? count : remaining};
}

fn StringView::next_line(usize &position) const wontthrow -> StringView
{
  let const remaining = substring(position);
  let const newline = remaining.find_character('\n');
  let const line_length = newline.has_value() ? *newline : remaining.length;
  let const line = remaining.substring_of_length(0, line_length);
  position += line_length;
  if (position < length) position++;

  return line;
}

fn StringView::next_ascii_whitespace_word(usize &position) const wontthrow
    -> StringView
{
  while (position < length && is_ascii_whitespace(data[position]))
    position++;
  let const start_position = position;
  while (position < length && !is_ascii_whitespace(data[position]))
    position++;

  return substring_of_length(start_position, position - start_position);
}

fn StringView::starts_with(StringView prefix) const wontthrow -> bool
{
  if (prefix.length > length) return false;
  /* The length guard keeps a null data pointer out of memcmp. */
  return prefix.length == 0 ||
         std::memcmp(data, prefix.data, prefix.length) == 0;
}

} /* namespace koshka */
