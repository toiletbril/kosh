/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements allocator-aware string-keyed maps. It provides
 * open-addressed lookup, insertion, removal, iteration, and guarded value
 * construction.
 */

#pragma once

#include "Allocator.hpp"
#include "Common.hpp"
#include "Debug.hpp"
#include "String.hpp"
#include "StringView.hpp"

namespace koshka {

template <class Value = String>
class StringMap
{
public:
  explicit StringMap(Allocator allocator) : m_allocator(allocator) {}

  cold StringMap(const StringMap &other) : StringMap(other.m_allocator)
  {
    if (other.m_count == 0) return;
    if (other.m_tombstones == 0) {
      let const fresh_slots = m_allocator.alloc_array<slot>(other.m_capacity);
      usize constructed_count = 0;
      try {
        for (; constructed_count < other.m_capacity; constructed_count++)
          new (&fresh_slots[constructed_count])
              slot{other.m_slots[constructed_count]};
      } catch (...) {
        for (usize i = 0; i < constructed_count; i++)
          fresh_slots[i].~slot();
        m_allocator.free_array(fresh_slots, other.m_capacity);
        throw;
      }

      m_slots = fresh_slots;
      m_capacity = other.m_capacity;
      m_count = other.m_count;
      return;
    }

    rehash(other.m_capacity);
    for (usize i = 0; i < other.m_capacity; i++) {
      if (other.m_slots[i].state == slot::Occupied)
        set_value_with_hash(other.m_slots[i].key.view(),
                            Value{other.m_slots[i].value},
                            other.m_slots[i].hash);
    }
  }

  mustuse cold fn clone() const throws -> StringMap { return StringMap{*this}; }

  StringMap(StringMap &&other) noexcept
      : m_allocator(other.m_allocator), m_slots(other.m_slots),
        m_capacity(other.m_capacity), m_count(other.m_count),
        m_tombstones(other.m_tombstones)
  {
    other.m_slots = nullptr;
    other.m_capacity = 0;
    other.m_count = 0;
    other.m_tombstones = 0;
  }
  fn operator=(StringMap &&other) wontthrow->StringMap &
  {
    if (this != &other) {
      destroy_all();
      m_allocator = other.m_allocator;
      m_slots = other.m_slots;
      m_capacity = other.m_capacity;
      m_count = other.m_count;
      m_tombstones = other.m_tombstones;
      other.m_slots = nullptr;
      other.m_capacity = 0;
      other.m_count = 0;
      other.m_tombstones = 0;
    }
    return *this;
  }
  cold fn operator=(const StringMap &other) throws->StringMap &
  {
    if (this != &other) {
      StringMap copy{other};
      *this = steal(copy);
    }
    return *this;
  }

  ~StringMap() { destroy_all(); }

  mustuse pure fn count() const wontthrow -> usize { return m_count; }

  cold fn reserve(usize expected_count) throws -> void
  {
    let const third = expected_count / 3;
    if (expected_count > SIZE_MAX - third - 1) [[unlikely]]
      throw std::bad_alloc{};
    let const needed = expected_count + third + 1;
    usize new_capacity = m_capacity == 0 ? 16 : m_capacity;
    while (new_capacity < needed) {
      if (new_capacity > SIZE_MAX / 2) [[unlikely]]
        throw std::bad_alloc{};
      new_capacity *= 2;
    }

    if (new_capacity > m_capacity) rehash(new_capacity);
  }

  hot mustuse pure fn find(StringView key) const wontthrow -> const Value *
  {
    if (m_capacity == 0) return nullptr;
    let const found = probe(key, hash_bytes(key)).found;
    return found == NO_INDEX ? nullptr : &m_slots[found].value;
  }

  hot flatten mustuse fn find(StringView key) wontthrow -> Value *
  {
    return const_cast<Value *>(static_cast<const StringMap *>(this)->find(key));
  }

  pure fn allocator() const wontthrow -> Allocator { return m_allocator; }

  hot fn set(StringView key, Value value) throws -> Value *
  {
    return set_value(key, steal(value));
  }

  hot fn get_or_create(StringView key, Value default_value) throws -> Value &
  {
    let const hash = hash_bytes(key);
    let const result = prepare_insertion(key, hash);
    if (result.found != NO_INDEX) return m_slots[result.found].value;
    return *place(result.insertion, key, hash, steal(default_value));
  }

  /* Store a String value built from a view. An existing slot reuses its String
     buffer, so a tight reassignment loop pays no per-turn allocation. The value
     must not view the existing slot's own buffer, since clear then append would
     read bytes the clear already truncated. */
  hot fn set(StringView key, StringView value) throws -> Value *
  {
    let const hash = hash_bytes(key);
    let const result = prepare_insertion(key, hash);
    if (result.found != NO_INDEX) {
      Value *existing = &m_slots[result.found].value;
      /* A buffer that once held a large value and now takes a far smaller one
         is rebuilt at the right size rather than reused, so a name that held a
         big value does not pin that memory for the session. */
      let const buffer_is_wasteful =
          existing->count() > 256 && value.length < existing->count() / 2;
      if (!buffer_is_wasteful) {
        existing->clear();
        existing->append(value);
        return existing;
      }
      *existing = String{m_allocator, value};
      return existing;
    }
    return place(result.insertion, key, hash, String{m_allocator, value});
  }

  hot fn erase(StringView key) throws -> void
  {
    if (m_capacity == 0) return;
    let const found = probe(key, hash_bytes(key)).found;
    if (found == NO_INDEX) return;
    ASSERT(m_slots != nullptr);
    let &slot = m_slots[found];
    slot.key = String{m_allocator};
    slot.value = Value{};
    slot.state = slot::Tombstone;
    m_count--;
    m_tombstones++;
  }

  template <class Fn>
  fn for_each(Fn callback) const throws -> void
  {
    for (usize i = 0; i < m_capacity; i++) {
      if (m_slots[i].state == slot::Occupied)
        callback(m_slots[i].key.view(), m_slots[i].value);
    }
  }

  template <class Fn>
  fn for_each(Fn callback) throws -> void
  {
    for (usize i = 0; i < m_capacity; i++) {
      if (m_slots[i].state == slot::Occupied)
        callback(m_slots[i].key.view(), m_slots[i].value);
    }
  }

  fn clear() wontthrow -> void { destroy_all(); }

private:
  struct slot
  {
    enum State : u8
    {
      Empty,
      Occupied,
      Tombstone,
    };
    State state{Empty};
    u64 hash{0};
    String key{};
    notunique Value value{};
  };

  static constexpr usize NO_INDEX = static_cast<usize>(-1);

  struct probe_result
  {
    usize found{NO_INDEX};
    usize insertion{NO_INDEX};
  };

  hot mustuse fn probe(StringView key, u64 hash) const wontthrow -> probe_result
  {
    ASSERT(m_capacity != 0);
    let const mask = m_capacity - 1;
    let index = static_cast<usize>(hash) & mask;
    let first_tombstone = NO_INDEX;

    for (usize probe_count = 0; probe_count < m_capacity; probe_count++) {
      let const &candidate = m_slots[index];
      if (candidate.state == slot::Empty) {
        return {NO_INDEX,
                first_tombstone != NO_INDEX ? first_tombstone : index};
      }
      if (candidate.state == slot::Occupied && candidate.hash == hash &&
          candidate.key.view() == key) [[likely]]
      {
        return {index, index};
      }
      if (candidate.state == slot::Tombstone && first_tombstone == NO_INDEX) {
        first_tombstone = index;
      }
      index = (index + 1) & mask;
    }

    return {NO_INDEX, first_tombstone};
  }

  hot fn prepare_insertion(StringView key, u64 hash) throws -> probe_result
  {
    if (m_capacity == 0) {
      rehash(16);
    }

    ASSERT(m_slots != nullptr);
    let result = probe(key, hash);
    ASSERT(result.insertion != NO_INDEX);
    if (result.found != NO_INDEX ||
        m_slots[result.insertion].state == slot::Tombstone)
    {
      return result;
    }

    let const maximum_occupied = (m_capacity >> 1) + (m_capacity >> 2);
    if (m_count + m_tombstones + 1 <= maximum_occupied) return result;

    if (m_count + 1 > maximum_occupied) {
      if (m_capacity > UINT32_MAX / 2) [[unlikely]]
        throw std::bad_alloc{};
      rehash(m_capacity * 2);
    } else {
      rehash(m_capacity);
    }
    result = probe(key, hash);
    return result;
  }

  hot fn set_value(StringView key, Value value) throws -> Value *
  {
    return set_value_with_hash(key, steal(value), hash_bytes(key));
  }

  hot fn set_value_with_hash(StringView key, Value value, u64 hash) throws
      -> Value *
  {
    let const result = prepare_insertion(key, hash);
    if (result.found != NO_INDEX) {
      m_slots[result.found].value = steal(value);
      return &m_slots[result.found].value;
    }
    ASSERT(result.insertion != NO_INDEX);
    return place(result.insertion, key, hash, steal(value));
  }

  fn place(usize index, StringView key, u64 hash, Value value) throws -> Value *
  {
    let &slot = m_slots[index];
    let const was_tombstone = slot.state == slot::Tombstone;
    slot.key = String{m_allocator, key};
    slot.hash = hash;
    slot.value = steal(value);
    if (was_tombstone) m_tombstones--;
    slot.state = slot::Occupied;
    m_count++;
    return &slot.value;
  }

  cold fn rehash(usize new_capacity) throws -> void
  {
    if (new_capacity > UINT32_MAX) [[unlikely]]
      throw std::bad_alloc{};
    let const fresh_slots = m_allocator.alloc_array<slot>(new_capacity);
    usize constructed_count = 0;
    try {
      for (; constructed_count < new_capacity; constructed_count++)
        new (&fresh_slots[constructed_count]) slot{};
    } catch (...) {
      for (usize i = 0; i < constructed_count; i++)
        fresh_slots[i].~slot();
      m_allocator.free_array(fresh_slots, new_capacity);
      throw;
    }

    let const old_slots = m_slots;
    let const old_capacity = m_capacity;

    usize fresh_count = 0;
    let const mask = new_capacity - 1;
    try {
      for (usize i = 0; i < old_capacity; i++) {
        if (old_slots[i].state != slot::Occupied) continue;

        let index = static_cast<usize>(old_slots[i].hash) & mask;
        while (fresh_slots[index].state == slot::Occupied)
          index = (index + 1) & mask;
        let &destination = fresh_slots[index];
        destination.hash = old_slots[i].hash;
        destination.key = old_slots[i].key;
        destination.state = slot::Occupied;
        fresh_count++;
      }

      for (usize i = 0; i < old_capacity; i++) {
        if (old_slots[i].state != slot::Occupied) continue;

        let index = static_cast<usize>(old_slots[i].hash) & mask;
        while (fresh_slots[index].state != slot::Occupied ||
               fresh_slots[index].hash != old_slots[i].hash ||
               fresh_slots[index].key.view() != old_slots[i].key.view())
          index = (index + 1) & mask;

        if constexpr (std::is_copy_assignable_v<Value>) {
          fresh_slots[index].value = old_slots[i].value;
        } else {
          static_assert(std::is_nothrow_move_assignable_v<Value>);
          fresh_slots[index].value = steal(old_slots[i].value);
        }
      }
    } catch (...) {
      for (usize i = 0; i < new_capacity; i++)
        fresh_slots[i].~slot();
      m_allocator.free_array(fresh_slots, new_capacity);
      throw;
    }

    m_slots = fresh_slots;
    m_capacity = static_cast<u32>(new_capacity);
    m_count = static_cast<u32>(fresh_count);
    m_tombstones = 0;

    for (usize i = 0; i < old_capacity; i++)
      old_slots[i].~slot();
    if (old_slots != nullptr) m_allocator.free_array(old_slots, old_capacity);
  }

  cold fn destroy_all() wontthrow -> void
  {
    for (usize i = 0; i < m_capacity; i++)
      m_slots[i].~slot();
    if (m_slots != nullptr) m_allocator.free_array(m_slots, m_capacity);
    m_slots = nullptr;
    m_capacity = 0;
    m_count = 0;
    m_tombstones = 0;
  }

  Allocator m_allocator;
  slot *m_slots{nullptr};
  u32 m_capacity{0};
  u32 m_count{0};
  u32 m_tombstones{0};
};

} /* namespace koshka */
