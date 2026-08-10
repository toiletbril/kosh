#pragma once

#include "Common.hpp"
#include "Platform.hpp"
#include "String.hpp"
#include "StringView.hpp"

namespace koshka {

class PackedStringKey
{
public:
  static constexpr usize WORD_COUNT = 8;
  static constexpr usize BYTE_CAPACITY = WORD_COUNT * 8;

  u64 words[WORD_COUNT]{};

  /* Pack a NUL-terminated literal at compile time, so a static table entry
     becomes a constant with no runtime initialization. consteval forbids the
     runtime path, so a caller that passed a non-literal would fail to compile
     rather than silently pack the key at run time. */
  static consteval fn from_literal(const char *text) wontthrow
      -> PackedStringKey
  {
    PackedStringKey key{};
    for (usize i = 0; text[i] != '\0' && i < BYTE_CAPACITY; i++)
      key.words[i / 8] |= static_cast<u64>(static_cast<u8>(text[i]))
                          << (8 * (i % 8));
    return key;
  }

  hot static fn from_view(StringView text) wontthrow -> PackedStringKey
  {
    PackedStringKey key{};
    let const count =
        text.count() < BYTE_CAPACITY ? text.count() : BYTE_CAPACITY;
    os::pack_little_endian_bytes(key.words, text.data, count);
    return key;
  }

  hot mustuse constexpr fn
  operator==(const PackedStringKey &other) const wontthrow->bool
  {
    if (words[0] != other.words[0]) return false;
    u64 difference = 0;
#pragma clang loop unroll_count(4)
    for (usize i = 1; i < WORD_COUNT; i++)
      difference |= words[i] ^ other.words[i];
    return difference == 0;
  }

  hot mustuse constexpr fn
  operator<(const PackedStringKey &other) const wontthrow->bool
  {
    for (usize i = 0; i < WORD_COUNT; i++)
      if (words[i] != other.words[i]) return words[i] < other.words[i];
    return false;
  }

  /* The byte length of a key with no embedded NUL, read as the position of the
     first zero byte and capped at the byte capacity. A key built from a name
     round-trips its length here, so a packed match alone does not let a
     NUL-padded query stand in for a shorter name. */
  hot mustuse pure fn packed_length() const wontthrow -> usize
  {
    for (usize k = 0; k < WORD_COUNT; k++) {
      let const w = words[k];
      let const z = (w - 0x0101010101010101ull) & ~w & 0x8080808080808080ull;
      if (z != 0) return k * 8 + static_cast<usize>(__builtin_ctzll(z) >> 3);
    }
    return BYTE_CAPACITY;
  }

  /* Unpack the bytes back into a String, stopping at the first NUL or the byte
     capacity. A key with no embedded NUL and no more than that many bytes
     round-trips exactly, which holds for every builtin name. */
  cold mustuse fn to_string() const throws -> String
  {
    char buffer[BYTE_CAPACITY];
    usize length = 0;
    for (; length < BYTE_CAPACITY; length++) {
      let const byte =
          static_cast<char>((words[length / 8] >> (8 * (length % 8))) & 0xFF);
      if (byte == '\0') break;
      buffer[length] = byte;
    }
    return String{
        StringView{buffer, length}
    };
  }
};

} // namespace koshka

#define SSK(literal) koshka::PackedStringKey::from_literal(literal)
