#pragma once

#include "Allocator.hpp"
#include "Common.hpp"
#include "Debug.hpp"
#include "IntBase.hpp"
#include "Maybe.hpp"
#include "StringView.hpp"

namespace koshka {

class String
{
public:
  /* The inline buffer length. A string shorter than this, counting the trailing
     null, lives inline. The value keeps sizeof(String) at sixty-four bytes next
     to the sixteen-byte allocator and the three size words. */
  static constexpr usize INLINE_CAPACITY = 24;

  explicit String(Allocator allocator) : m_allocator(allocator)
  {
    reset_to_inline();
  }
  String(Allocator allocator, StringView initial) throws;
  String(const char *cstr) throws;
  String(StringView initial) throws;

  String(const String &other) throws;
  String(String &&other) wontthrow;

  fn operator=(const String &other) throws->String &;
  fn operator=(String &&other) wontthrow->String &;

  mustuse fn clone() const throws -> String { return String{*this}; }

  static fn from_in_base(u64 magnitude, bool is_negative, int_base base,
                         Allocator allocator) throws -> String
  {
    char buffer[72];
    usize position = sizeof(buffer);
    let const radix = static_cast<u64>(base);

    do {
      let const digit = static_cast<u32>(magnitude % radix);
      buffer[--position] = digit < 10 ? static_cast<char>('0' + digit)
                                      : static_cast<char>('a' + digit - 10);
      magnitude /= radix;
    } while (magnitude != 0);

    if (is_negative) buffer[--position] = '-';

    return String{
        allocator, StringView{buffer + position, sizeof(buffer) - position}
    };
  }

  template <class T>
  mustuse static fn from(T value, Allocator allocator) throws -> String
  {
    if constexpr (is_tagged_int_v<T>) {
      return from_signed_or_unsigned(value.value, T::base, allocator);
    } else {
      static_assert(std::is_integral_v<T>, "String::from takes an integer");
      return from_signed_or_unsigned(value, int_base::decimal, allocator);
    }
  }

  template <class U>
  mustuse static fn from_signed_or_unsigned(U value, int_base base,
                                            Allocator allocator) throws
      -> String
  {
    if constexpr (std::is_signed_v<U>) {
      let const is_negative = value < 0;
      let const magnitude =
          is_negative ? ~static_cast<u64>(value) + 1u : static_cast<u64>(value);
      return from_in_base(magnitude, is_negative, base, allocator);
    } else {
      return from_in_base(static_cast<u64>(value), false, base, allocator);
    }
  }

  template <class T>
  mustuse fn to() const throws -> ErrorOr<T>;

  ~String()
  {
    if (m_data != m_inline) free_storage();
  }

  mustuse pure fn allocator() const wontthrow -> Allocator
  {
    return m_allocator;
  }

  hot mustuse pure fn count() const wontthrow -> usize { return m_length; }
  hot mustuse pure fn is_empty() const wontthrow -> bool
  {
    return m_length == 0;
  }
  hot mustuse pure fn operator[](usize i) const wontthrow->char
  {
    ASSERT(i < m_length, "string index is past the end");
    return m_data[i];
  }
  hot fn lowercase_ascii() wontthrow -> void
  {
    for (usize position = 0; position < m_length; position++) {
      let &byte = m_data[position];
      if (byte >= 'A' && byte <= 'Z')
        byte = static_cast<char>(byte - 'A' + 'a');
    }
  }
  hot fn assign_lowercase_ascii(StringView text) throws -> void
  {
    clear();
    append(text);
    lowercase_ascii();
  }
  hot fn normalize_crlf_line_endings() wontthrow -> void
  {
    usize output_position = 0;

    for (usize input_position = 0; input_position < m_length; input_position++)
    {
      if (m_data[input_position] == '\r' && input_position + 1 < m_length &&
          m_data[input_position + 1] == '\n')
      {
        continue;
      }

      m_data[output_position++] = m_data[input_position];
    }

    m_length = output_position;
    m_data[m_length] = '\0';
  }
  hot mustuse pure fn view() const wontthrow -> StringView
  {
    return StringView{m_data, m_length};
  }
  operator StringView() const wontthrow { return StringView{m_data, m_length}; }
  hot mustuse pure fn c_str() const wontthrow -> const char *
  {
    return m_data != nullptr ? m_data : "";
  }

  fn clear() wontthrow -> void;

  /* The has-capacity fast path is inlined, so a hot append that fits pays no
     call. reserve stays cold for the growth path, and m_capacity counts the
     null slot, so the fit test is length + count < capacity. */
  hot fn push(char c) throws -> void
  {
    if (m_length == SIZE_MAX) [[unlikely]]
      throw std::bad_alloc{};
    let const new_length = m_length + 1;
    if (new_length < m_capacity) [[likely]] {
      m_data[m_length++] = c;
      m_data[m_length] = '\0';
      return;
    }
    reserve(new_length);
    m_data[m_length++] = c;
    m_data[m_length] = '\0';
  }
  hot fn append(StringView other) throws -> void
  {
    if (other.length == 0) return;
    if (other.length > SIZE_MAX - m_length) [[unlikely]]
      throw std::bad_alloc{};
    let const new_length = m_length + other.length;
    if (new_length < m_capacity) [[likely]] {
      std::memcpy(m_data + m_length, other.data, other.length);
      m_length = new_length;
      m_data[m_length] = '\0';
      return;
    }
    let const source_address = reinterpret_cast<uintptr>(other.data);
    let const storage_address = reinterpret_cast<uintptr>(m_data);
    let const is_aliased = source_address >= storage_address &&
                           source_address - storage_address < m_length;
    usize source_offset = 0;
    if (is_aliased) source_offset = source_address - storage_address;
    reserve(new_length);
    if (is_aliased) other.data = m_data + source_offset;
    std::memcpy(m_data + m_length, other.data, other.length);
    m_length = new_length;
    m_data[m_length] = '\0';
  }

  hot fn append_repeated(char byte, usize repeat_count) throws -> void
  {
    if (repeat_count == 0) return;
    if (repeat_count > SIZE_MAX - m_length) [[unlikely]]
      throw std::bad_alloc{};
    let const new_length = m_length + repeat_count;
    if (new_length >= m_capacity) reserve(new_length);
    std::memset(m_data + m_length, static_cast<unsigned char>(byte),
                repeat_count);
    m_length = new_length;
    m_data[m_length] = '\0';
  }

  cold fn reserve(usize needed) throws -> void;

  mustuse pure fn data() const wontthrow -> const char * { return c_str(); }
  mustuse pure fn length() const wontthrow -> usize { return m_length; }
  hot mustuse pure fn back() const wontthrow -> char
  {
    ASSERT(m_length > 0, "back() on an empty string");
    return m_data[m_length - 1];
  }

  fn pop_back() wontthrow -> void;
  fn strip_trailing_newlines() wontthrow -> void;

  hot flatten fn append(char c) throws -> void { push(c); }
  fn operator+=(StringView other) throws->String &;
  fn operator+=(char c) throws->String &;

  hot flatten mustuse pure fn find_character(char wanted) const wontthrow
      -> Maybe<usize>
  {
    return view().find_character(wanted);
  }
  flatten mustuse pure fn substring(usize start) const wontthrow -> StringView
  {
    return view().substring(start);
  }
  flatten mustuse pure fn substring_of_length(usize start,
                                              usize count) const wontthrow
      -> StringView
  {
    return view().substring_of_length(start, count);
  }
  flatten mustuse pure fn starts_with(StringView prefix) const wontthrow -> bool
  {
    return view().starts_with(prefix);
  }

  hot flatten mustuse pure fn operator==(StringView other) const wontthrow->bool
  {
    return view() == other;
  }
  hot flatten mustuse pure fn operator!=(StringView other) const wontthrow->bool
  {
    return !(view() == other);
  }

  /* Byte order, so a sort matches the C locale collating order. */
  hot mustuse pure fn operator<(const String &other) const wontthrow->bool;

  /* The first byte. The caller guarantees the string is not empty. */
  mustuse pure fn first_character() const wontthrow -> char
  {
    ASSERT(m_length > 0, "first_character() on an empty string");
    return m_data[0];
  }

  mustuse pure fn find_substring(StringView needle,
                                 usize from = 0) const wontthrow
      -> Maybe<usize>;

  mustuse pure fn find_last_character(char wanted) const wontthrow
      -> Maybe<usize>;

private:
  /* A default String is inline and empty, so it can serve as a container slot
     before its real allocator and value are assigned. The friend keeps it
     reachable to the table while every call site must name its lifetime. */
  template <class Value>
  friend class StringMap;
  String() : m_allocator(heap_allocator()) { reset_to_inline(); }

  cold fn free_storage() wontthrow -> void;

  hot fn adopt_storage_of(String &&other) wontthrow -> void;

  mustuse pure fn is_inline() const wontthrow -> bool
  {
    return m_data == m_inline;
  }

  /* Point the string at its empty inline buffer. The caller must have already
     released any heap storage, since this overwrites the data pointer. */
  fn reset_to_inline() wontthrow -> void
  {
    m_data = m_inline;
    m_inline[0] = '\0';
    m_length = 0;
    m_capacity = INLINE_CAPACITY;
  }

  Allocator m_allocator;
  char *m_data{nullptr};
  usize m_length{0};
  usize m_capacity{0};
  char m_inline[INLINE_CAPACITY];
};

static_assert(sizeof(usize) != 8 || sizeof(String) == 64);

template <>
fn String::from<f64>(f64 value, Allocator allocator) throws -> String;

template <>
fn String::to<f64>() const throws -> ErrorOr<f64>;

fn operator+(StringView left, StringView right) throws->String;

} /* namespace koshka */
