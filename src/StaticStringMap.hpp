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

/* A query that shares no leading byte with any key and a query outside the key
   length range are both rejected before the 64-byte pack and the binary search
   are paid. The masks are derived from the table itself, so a table edit cannot
   leave the filter stale. */
struct static_string_prefilter
{
  u64 leading_byte_mask[4]{};
  usize shortest_key_length{PackedStringKey::BYTE_CAPACITY};
  usize longest_key_length{0};

  constexpr fn record(const PackedStringKey &key) wontthrow -> void
  {
    let const key_length = key.packed_length();
    if (key_length < shortest_key_length) shortest_key_length = key_length;
    if (key_length > longest_key_length) longest_key_length = key_length;

    let const leading = key.leading_byte();
    leading_byte_mask[leading >> 6] |= 1ull << (leading & 63);
  }

  hot mustuse pure constexpr fn might_contain(StringView text) const wontthrow
      -> bool
  {
    if (text.length < shortest_key_length || text.length > longest_key_length) {
      return false;
    }

    let const leading = static_cast<u8>(text.data[0]);

    return ((leading_byte_mask[leading >> 6] >> (leading & 63)) & 1) != 0;
  }
};

template <class Value, usize Count>
class StaticStringMap
{
public:
  static_string_entry<Value> entries[Count]{};
  static_string_prefilter prefilter{};

  consteval StaticStringMap(
      const static_string_entry<Value> (&table)[Count]) wontthrow
  {
    for (usize i = 0; i < Count; i++) {
      entries[i] = table[i];
      prefilter.record(table[i].key);
    }

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
    if (!prefilter.might_contain(text)) return None;
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
  static_string_prefilter prefilter{};

  consteval StaticStringSet(const PackedStringKey (&table)[Count]) wontthrow
  {
    for (usize i = 0; i < Count; i++) {
      keys[i] = table[i];
      prefilter.record(table[i]);
    }

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

  hot mustuse fn find_index(StringView text) const wontthrow -> Maybe<usize>
  {
    if (!prefilter.might_contain(text)) return None;
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

    if (low < Count && keys[low] == wanted &&
        text.count() == keys[low].packed_length())
      return low;
    return None;
  }

  hot mustuse fn contains(StringView text) const wontthrow -> bool
  {
    return find_index(text).has_value();
  }
};

template <usize Count>
StaticStringSet(const PackedStringKey (&)[Count]) -> StaticStringSet<Count>;

} /* namespace koshka */
