#pragma once

#include "Allocator.hpp"
#include "Common.hpp"
#include "Maybe.hpp"

namespace koshka {

pure constexpr fn is_ascii_whitespace(char byte) wontthrow -> bool
{
  return byte == ' ' || byte == '\t' || byte == '\n' || byte == '\v' ||
         byte == '\f' || byte == '\r';
}

template <class T>
class ErrorOr;
class String;

class StringView
{
public:
  const char *data{nullptr};
  usize length{0};

  StringView() = default;
  constexpr StringView(const char *bytes, usize count)
      : data(bytes), length(count)
  {}
  /* The length folds at compile time for a literal, so a static table of views
     costs no startup work and no strlen on read. */
  constexpr StringView(const char *cstr) wontthrow
      : data(cstr),
        length(cstr != nullptr ? __builtin_strlen(cstr) : 0)
  {}

  hot mustuse pure fn count() const wontthrow -> usize { return length; }
  hot mustuse pure fn is_empty() const wontthrow -> bool { return length == 0; }
  hot mustuse pure fn operator[](usize i) const wontthrow->char
  {
    ASSERT(i < length, "string-view index is past the end");
    return data[i];
  }

  mustuse fn to_lower_ascii(Allocator allocator) const throws -> String;

  /* A copy of these bytes in the allocator, borrowed for as long as that
     allocator keeps them. An arena copy lives as long as the arena. */
  mustuse fn copy_to(Allocator allocator) const throws -> StringView;

  hot flatten mustuse pure fn operator==(StringView other) const wontthrow->bool
  {
    if (length != other.length) return false;
    if (length == 0) return true;

    if (length <= 8) {
      u64 left = 0;
      u64 right = 0;
      __builtin_memcpy(&left, data, length);
      __builtin_memcpy(&right, other.data, length);
      return left == right;
    }
    if (length <= 16) {
      u64 left_head = 0;
      u64 right_head = 0;
      u64 left_tail = 0;
      u64 right_tail = 0;
      __builtin_memcpy(&left_head, data, 8);
      __builtin_memcpy(&right_head, other.data, 8);
      __builtin_memcpy(&left_tail, data + length - 8, 8);
      __builtin_memcpy(&right_tail, other.data + length - 8, 8);
      return left_head == right_head && left_tail == right_tail;
    }

    return __builtin_memcmp(data, other.data, length) == 0;
  }
  hot flatten mustuse pure fn operator!=(StringView other) const wontthrow->bool
  {
    return !(*this == other);
  }
  hot mustuse pure fn operator<(StringView other) const wontthrow->bool
  {
    let const shared_length = length < other.length ? length : other.length;
    let const order = shared_length == 0
                          ? 0
                          : __builtin_memcmp(data, other.data, shared_length);
    return order < 0 || (order == 0 && length < other.length);
  }

  template <class T>
  mustuse fn to() const throws -> ErrorOr<T>;

  hot mustuse pure fn find_character(char wanted) const wontthrow
      -> Maybe<usize>;

  mustuse pure fn substring(usize start) const wontthrow -> StringView;

  mustuse pure fn substring_of_length(usize start, usize count) const wontthrow
      -> StringView;
  mustuse fn next_line(usize &position) const wontthrow -> StringView;
  mustuse fn next_ascii_whitespace_word(usize &position) const wontthrow
      -> StringView;

  mustuse pure fn starts_with(StringView prefix) const wontthrow -> bool;

  template <class Callback>
  fn for_each_ascii_whitespace_word(Callback do_word) const throws -> void
  {
    usize position = 0;
    while (position < length) {
      let const word = next_ascii_whitespace_word(position);
      if (!word.is_empty()) do_word(word);
    }
  }

  /* Whether the view is one or more decimal digits and nothing else, the strict
     digit scan a numeric name, positional, or descriptor shares before it
     parses. An empty view is not a number, and no sign or surrounding
     whitespace is allowed, so a caller can tell a bare number apart. */
  mustuse pure fn is_all_decimal_digits() const wontthrow -> bool
  {
    if (length == 0) return false;

    for (usize i = 0; i < length; i++) {
      if (data[i] < '0' || data[i] > '9') return false;
    }
    return true;
  }

  mustuse pure fn trim_blanks() const wontthrow -> StringView
  {
    usize start = 0;
    usize end = length;
    while (start < end && (data[start] == ' ' || data[start] == '\t'))
      start++;
    while (end > start && (data[end - 1] == ' ' || data[end - 1] == '\t'))
      end--;

    return substring_of_length(start, end - start);
  }

  mustuse pure fn without_trailing_newline() const wontthrow -> StringView
  {
    if (length > 0 && data[length - 1] == '\n') {
      return substring_of_length(0, length - 1);
    }
    return *this;
  }
};

pure alwaysinline fn hash_bytes(StringView view) wontthrow -> u64
{
  u64 hash = view.length * 0x9e3779b97f4a7c15ull;
  usize i = 0;
#pragma clang loop unroll_count(4)
  for (; i + 8 <= view.length; i += 8) {
    u64 chunk;
    __builtin_memcpy(&chunk, view.data + i, 8);
    hash = (hash ^ chunk) * 0x100000001b3ull;
  }
  u64 tail = 0;
  if (view.length > i) __builtin_memcpy(&tail, view.data + i, view.length - i);
  hash = (hash ^ tail) * 0x100000001b3ull;
  hash ^= hash >> 31;
  return hash;
}

} /* namespace koshka */
