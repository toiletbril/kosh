#pragma once

#include "Allocator.hpp"
#include "ArrayList.hpp"
#include "Common.hpp"

namespace koshka {

class Bitset
{
public:
  static constexpr usize BITS_PER_WORD = 64;

  explicit Bitset(Allocator allocator) : m_words(allocator) {}

  Bitset(const Bitset &other) : m_words(other.m_words), m_length(other.m_length)
  {}
  Bitset(Bitset &&other) noexcept
      : m_words(steal(other.m_words)), m_length(other.m_length)
  {
    other.m_length = 0;
  }
  fn operator=(const Bitset &other) throws->Bitset &
  {
    if (this != &other) {
      m_words = other.m_words;
      m_length = other.m_length;
    }
    return *this;
  }
  fn operator=(Bitset &&other) wontthrow->Bitset &
  {
    if (this != &other) {
      m_words = steal(other.m_words);
      m_length = other.m_length;
      other.m_length = 0;
    }
    return *this;
  }

  hot fn push(bool value) throws -> void
  {
    let const bit_position = m_length;
    m_length++;
    let const word_index = bit_position / BITS_PER_WORD;
    if (word_index >= m_words.count()) m_words.push(0);
    if (value) m_words[word_index] |= u64{1} << (bit_position % BITS_PER_WORD);
  }

  hot mustuse pure fn operator[](usize index) const wontthrow->bool
  {
    if (index >= m_length) return false;
    return ((m_words[index / BITS_PER_WORD] >> (index % BITS_PER_WORD)) & 1u) !=
           0;
  }

  mustuse pure fn count() const wontthrow -> usize { return m_length; }
  mustuse pure fn is_empty() const wontthrow -> bool { return m_length == 0; }

  fn reserve(usize bit_count) throws -> void
  {
    m_words.reserve((bit_count + BITS_PER_WORD - 1) / BITS_PER_WORD);
  }

  fn clear() wontthrow -> void
  {
    m_words.clear();
    m_length = 0;
  }

  pure fn allocator() const wontthrow -> Allocator
  {
    return m_words.allocator();
  }

  hot flatten fn and_with(const Bitset &other) wontthrow -> void
  {
    let const shared = m_words.count() < other.m_words.count()
                           ? m_words.count()
                           : other.m_words.count();
    for (usize i = 0; i < shared; i++)
      m_words[i] &= other.m_words[i];
    for (usize i = shared; i < m_words.count(); i++)
      m_words[i] = 0;
  }

  hot flatten fn or_with(const Bitset &other) wontthrow -> void
  {
    let const shared = m_words.count() < other.m_words.count()
                           ? m_words.count()
                           : other.m_words.count();
    for (usize i = 0; i < shared; i++)
      m_words[i] |= other.m_words[i];
  }

  hot mustuse pure fn any() const wontthrow -> bool
  {
    for (usize i = 0; i < m_words.count(); i++)
      if (m_words[i] != 0) return true;
    return false;
  }

private:
  ArrayList<u64> m_words;
  usize m_length{0};
};

} // namespace koshka
