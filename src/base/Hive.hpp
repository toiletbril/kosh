/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements the hive, a container whose elements never move. It
 * stores elements in fixed blocks and reuses erased slots through a free
 * list, so a pointer to an element stays valid until that element is erased.
 */

#pragma once

#include "Allocator.hpp"
#include "Common.hpp"
#include "Debug.hpp"

namespace koshka {

template <class T>
class Hive
{
public:
  static_assert(std::is_nothrow_destructible_v<T>);

  static constexpr usize BLOCK_SLOT_COUNT = 64;

  explicit Hive(Allocator allocator = heap_allocator()) wontthrow
      : m_allocator{allocator}
  {}

  Hive(const Hive &) = delete;
  Hive &operator=(const Hive &) = delete;

  Hive(Hive &&other) wontthrow
      : m_allocator{other.m_allocator},
        m_blocks{other.m_blocks},
        m_free{other.m_free},
        m_count{other.m_count}
  {
    other.m_blocks = nullptr;
    other.m_free = nullptr;
    other.m_count = 0;
  }

  fn operator=(Hive &&other) wontthrow -> Hive &
  {
    if (this == &other) return *this;

    destroy_all();
    m_allocator = other.m_allocator;
    m_blocks = other.m_blocks;
    m_free = other.m_free;
    m_count = other.m_count;
    other.m_blocks = nullptr;
    other.m_free = nullptr;
    other.m_count = 0;

    return *this;
  }

  ~Hive() { destroy_all(); }

  template <class... Arguments>
  fn emplace(Arguments &&...arguments) throws -> T *
  {
    if (m_free == nullptr) add_block();

    let *const free_slot = m_free;
    let *const element = new (free_slot->bytes)
        T{static_cast<Arguments &&>(arguments)...};
    m_free = free_slot->next_free;
    free_slot->next_free = nullptr;
    free_slot->is_live = true;
    m_count++;

    return element;
  }

  fn erase(T *element) wontthrow -> void
  {
    ASSERT(element != nullptr);

    let *const erased_slot = reinterpret_cast<slot *>(element);
    ASSERT(erased_slot->is_live);
    element->~T();
    erased_slot->is_live = false;
    erased_slot->next_free = m_free;
    m_free = erased_slot;
    m_count--;
  }

  mustuse pure fn count() const wontthrow -> usize { return m_count; }

private:
  struct slot
  {
    alignas(T) unsigned char bytes[sizeof(T)];
    slot *next_free;
    bool is_live;
  };
  static_assert(std::is_standard_layout_v<slot>);

  struct block
  {
    slot slots[BLOCK_SLOT_COUNT];
    block *next;
  };

  fn add_block() throws -> void
  {
    let *const added = m_allocator.alloc_array<block>(1);
    added->next = m_blocks;
    m_blocks = added;
    for (usize i = BLOCK_SLOT_COUNT; i > 0; i--) {
      added->slots[i - 1].is_live = false;
      added->slots[i - 1].next_free = m_free;
      m_free = &added->slots[i - 1];
    }
  }

  fn destroy_all() wontthrow -> void
  {
    while (m_blocks != nullptr) {
      let *const next = m_blocks->next;
      for (let &each_slot : m_blocks->slots)
        if (each_slot.is_live) reinterpret_cast<T *>(each_slot.bytes)->~T();
      m_allocator.free_array(m_blocks, 1);
      m_blocks = next;
    }
    m_free = nullptr;
    m_count = 0;
  }

  Allocator m_allocator;
  block *m_blocks{nullptr};
  slot *m_free{nullptr};
  usize m_count{0};
};

} /* namespace koshka */
