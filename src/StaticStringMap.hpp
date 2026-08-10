#pragma once

#include "Common.hpp"
#include "Maybe.hpp"
#include "PackedStringKey.hpp"
#include "StringView.hpp"

namespace koshka {

template <class Value>
struct static_string_entry
{
  PackedStringKey key;
  Value value;
};

template <class Value, usize Count>
class StaticStringMap
{
public:
  static_string_entry<Value> entries[Count]{};

  consteval StaticStringMap(
      const static_string_entry<Value> (&table)[Count]) wontthrow
  {
    for (usize i = 0; i < Count; i++)
      entries[i] = table[i];

    for (usize i = 1; i < Count; i++) {
      let const moved = entries[i];
      usize slot = i;
      while (slot > 0 && moved.key < entries[slot - 1].key) {
        entries[slot] = entries[slot - 1];
        slot--;
      }
      entries[slot] = moved;
    }
  }

  hot mustuse fn find(StringView text) const throws -> Maybe<Value>
  {
    if (text.count() > PackedStringKey::BYTE_CAPACITY) return None;
    let const wanted = PackedStringKey::from_view(text);

    usize low = 0;
    usize high = Count;
    while (low < high) {
      let const middle = low + ((high - low) / 2);
      if (entries[middle].key < wanted)
        low = middle + 1;
      else
        high = middle;
    }

    if (low < Count && entries[low].key == wanted &&
        text.count() == entries[low].key.packed_length())
      return entries[low].value;
    return None;
  }
};

template <class Value, usize Count>
StaticStringMap(const static_string_entry<Value> (&)[Count])
    -> StaticStringMap<Value, Count>;

template <usize Count>
class StaticStringSet
{
public:
  PackedStringKey keys[Count]{};

  consteval StaticStringSet(const PackedStringKey (&table)[Count]) wontthrow
  {
    for (usize i = 0; i < Count; i++)
      keys[i] = table[i];

    for (usize i = 1; i < Count; i++) {
      let const moved = keys[i];
      usize slot = i;
      while (slot > 0 && moved < keys[slot - 1]) {
        keys[slot] = keys[slot - 1];
        slot--;
      }
      keys[slot] = moved;
    }
  }

  hot mustuse fn contains(StringView text) const wontthrow -> bool
  {
    if (text.count() > PackedStringKey::BYTE_CAPACITY) return false;
    let const wanted = PackedStringKey::from_view(text);

    usize low = 0;
    usize high = Count;
    while (low < high) {
      let const middle = low + ((high - low) / 2);
      if (keys[middle] < wanted)
        low = middle + 1;
      else
        high = middle;
    }

    return low < Count && keys[low] == wanted &&
           text.count() == keys[low].packed_length();
  }
};

template <usize Count>
StaticStringSet(const PackedStringKey (&)[Count]) -> StaticStringSet<Count>;

} // namespace koshka
