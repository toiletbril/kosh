#include "String.hpp"

#include "ErrorOr.hpp"
#include "IntBase.hpp"
#include "Utils.hpp"

namespace koshka {

String::String(Allocator allocator, StringView initial) throws
    : m_allocator(allocator)
{
  reset_to_inline();
  append(initial);
}

String::String(const char *cstr) throws : m_allocator(heap_allocator())
{
  reset_to_inline();
  append(StringView{cstr});
}

String::String(StringView initial) throws : m_allocator(heap_allocator())
{
  reset_to_inline();
  append(initial);
}

cold String::String(const String &other) throws : m_allocator(other.m_allocator)
{
  reset_to_inline();
  append(other.view());
}

hot fn String::adopt_storage_of(String &&other) wontthrow -> void
{
  if (other.is_inline()) {
    std::memcpy(m_inline, other.m_inline, other.m_length + 1);
    m_data = m_inline;
    m_length = other.m_length;
    m_capacity = INLINE_CAPACITY;
  } else {
    m_data = other.m_data;
    m_length = other.m_length;
    m_capacity = other.m_capacity;
    other.reset_to_inline();
  }
}

String::String(String &&other) wontthrow : m_allocator(other.m_allocator)
{
  adopt_storage_of(steal(other));
}

fn String::operator=(const String &other) throws -> String &
{
  if (this != &other) {
    clear();
    append(other.view());
  }
  return *this;
}

fn String::operator=(String &&other) wontthrow -> String &
{
  if (this != &other) {
    free_storage();
    m_allocator = other.m_allocator;
    adopt_storage_of(steal(other));
  }
  return *this;
}

fn String::clear() wontthrow -> void
{
  m_length = 0;
  if (m_data != nullptr) m_data[0] = '\0';
}

cold fn String::reserve(usize needed) throws -> void
{
  if (needed < m_capacity) [[likely]]
    return;
  if (needed == SIZE_MAX) [[unlikely]]
    throw std::bad_alloc{};

  let const required_capacity = needed + 1;
  let new_capacity = required_capacity;

  /* The first heap block is exact. Most strings that outgrow the inline buffer
     are built from one known run and are never appended to again, and the heap
     pool rounds the request up to a power of two on top of any slack left
     here. A string that keeps growing pays one extra copy and then grows
     geometrically from its second block on. */
  if (!is_inline()) {
    let const growth = m_capacity < 64 ? usize{4} : usize{2};
    new_capacity = m_capacity > SIZE_MAX / growth ? required_capacity
                                                  : m_capacity * growth;
    while (new_capacity < required_capacity) {
      if (new_capacity > SIZE_MAX / 2) {
        new_capacity = required_capacity;
        break;
      }
      new_capacity *= 2;
    }
  }

  let fresh = m_allocator.alloc_array<char>(new_capacity);
  let const preserved_length = m_length;
  if (preserved_length > 0) std::memcpy(fresh, m_data, preserved_length);
  fresh[preserved_length] = '\0';
  free_storage();
  m_data = fresh;
  m_length = preserved_length;
  m_capacity = new_capacity;
}

fn String::pop_back() wontthrow -> void
{
  ASSERT(m_length > 0, "pop_back on empty string");
  m_length--;
  m_data[m_length] = '\0';
}

fn String::strip_trailing_newlines() wontthrow -> void
{
  while (m_length != 0 && m_data[m_length - 1] == '\n')
    m_data[--m_length] = '\0';
}

fn String::operator+=(StringView other) throws -> String &
{
  append(other);
  return *this;
}

fn String::operator+=(char c) throws -> String &
{
  push(c);
  return *this;
}

hot fn String::operator<(const String &other) const wontthrow -> bool
{
  return view() < other.view();
}

fn String::find_substring(StringView needle, usize from) const wontthrow
    -> Maybe<usize>
{
  if (needle.length == 0) return from <= m_length ? Maybe<usize>{from} : None;
  if (needle.length > m_length) return None;
  let const last_start = m_length - needle.length;
  if (from > last_start) return None;

  let i = from;
  while (i <= last_start) {
    let const scan_length = last_start - i + 1;
    let const found = std::memchr(
        m_data + i, static_cast<unsigned char>(needle.data[0]), scan_length);
    if (found == nullptr) return None;
    let const candidate =
        static_cast<usize>(static_cast<const char *>(found) - m_data);
    if (std::memcmp(m_data + candidate, needle.data, needle.length) == 0)
      return candidate;
    i = candidate + 1;
  }
  return None;
}

fn String::find_last_character(char wanted) const wontthrow -> Maybe<usize>
{
  for (usize i = m_length; i > 0; i--)
    if (m_data[i - 1] == wanted) return i - 1;
  return None;
}

cold fn String::free_storage() wontthrow -> void
{
  /* The inline buffer is part of the object and is never freed. */
  if (m_data != nullptr && m_data != m_inline) {
    m_allocator.free_array(m_data, m_capacity);
  }
  reset_to_inline();
}

fn operator+(StringView left, StringView right) throws->String
{
  if (right.length > SIZE_MAX - left.length) [[unlikely]]
    throw std::bad_alloc{};
  let result = String{heap_allocator()};
  result.reserve(left.length + right.length);
  result.append(left);
  result.append(right);
  return result;
}

template <class T>
fn String::to() const throws -> ErrorOr<T>
{
  return view().to<T>();
}

template <>
fn String::to<f64>() const throws -> ErrorOr<f64>
{
  return utils::parse_decimal_f64(*this);
}

template <>
fn String::from<f64>(f64 value, Allocator allocator) throws -> String
{
  return utils::format_f64(value, allocator);
}

#define KOSH_STRING_TO(T) template ErrorOr<T> String::to<T>() const;
KOSH_STRING_TO(i16)
KOSH_STRING_TO(u16)
KOSH_STRING_TO(i32)
KOSH_STRING_TO(u32)
KOSH_STRING_TO(i64)
KOSH_STRING_TO(u64)
KOSH_STRING_TO(bi16)
KOSH_STRING_TO(bi32)
KOSH_STRING_TO(bi64)
KOSH_STRING_TO(bu16)
KOSH_STRING_TO(bu32)
KOSH_STRING_TO(bu64)
KOSH_STRING_TO(oi16)
KOSH_STRING_TO(oi32)
KOSH_STRING_TO(oi64)
KOSH_STRING_TO(ou16)
KOSH_STRING_TO(ou32)
KOSH_STRING_TO(ou64)
KOSH_STRING_TO(hi16)
KOSH_STRING_TO(hi32)
KOSH_STRING_TO(hi64)
KOSH_STRING_TO(hu16)
KOSH_STRING_TO(hu32)
KOSH_STRING_TO(hu64)
#undef KOSH_STRING_TO

} /* namespace koshka */
