#pragma once

#include "ArrayList.hpp"
#include "Common.hpp"

#include <type_traits>

namespace koshka {

class BumpArena
{
public:
  BumpArena();
  ~BumpArena();

  BumpArena(const BumpArena &) = delete;
  BumpArena &operator=(const BumpArena &) = delete;

  hot fn allocate(usize size, usize alignment) throws -> opaque *;
  fn owns(const opaque *pointer) const wontthrow -> bool;
  cold fn reset() wontthrow -> void;

  struct Mark
  {
    usize block_index;
    usize used_in_block;
    usize destructor_count;
  };

  struct LifetimeIdentity
  {
    u32 arena_incarnation{0};
    u32 slot_position{UINT32_MAX};
    u32 slot_incarnation{0};
  };

  pure fn reset_generation() const wontthrow -> usize
  {
    return m_reset_generation;
  }

  fn register_lifetime() throws -> LifetimeIdentity;
  pure fn is_lifetime_valid(LifetimeIdentity identity) const wontthrow -> bool;

  fn bytes_used() const wontthrow -> usize;

  fn block_count() const wontthrow -> usize { return m_blocks.count(); }
  fn destructor_count() const wontthrow -> usize { return m_destructor_count; }
  fn destructor_capacity() const wontthrow -> usize
  {
    return m_destructor_chunks.count() * DESTRUCTORS_PER_CHUNK;
  }
  fn bytes_capacity() const wontthrow -> usize
  {
    usize total = 0;
    for (const block &block : m_blocks)
      total += block.size;
    return total;
  }

  /* A saved bump position, so a scope can reclaim everything it allocated above
     the mark while leaving earlier allocations alone. The marks nest. */
  fn mark() const wontthrow -> Mark;
  fn release(Mark saved) wontthrow -> void;

  template <class T, class... Args>
  flatten alwaysinline fn create(Args &&...args) throws -> T *
  {
    static_assert(std::is_nothrow_destructible_v<T>);
    let const saved = mark();
    opaque *storage = nullptr;
    try {
      storage = allocate(sizeof(T), alignof(T));
    } catch (...) {
      release(saved);
      throw;
    }

    T *object = nullptr;
    try {
      object = new (storage) T(std::forward<Args>(args)...);
    } catch (...) {
      release(saved);
      throw;
    }

    if constexpr (!std::is_trivially_destructible_v<T>) {
      if constexpr (requires { T::is_arena_destructor_noop; }) {
        if constexpr (T::is_arena_destructor_noop) return object;
      }
      try {
        push_destructor(
            pending_destructor{object, [](opaque *pointer) noexcept {
                                 static_cast<T *>(pointer)->~T();
                               }});
      } catch (...) {
        object->~T();
        release(saved);
        throw;
      }
    }
    return object;
  }

private:
  struct block
  {
    u8 *base;
    usize size;
    usize used;
  };

  struct lifetime_slot
  {
    Mark payload_end;
    u32 incarnation{0};
    u32 next_free_position{UINT32_MAX};
    bool is_active{false};
  };

  struct pending_destructor
  {
    opaque *object;
    void (*run)(opaque *) noexcept;
  };

  static constexpr usize DEFAULT_BLOCK_SIZE = 64 * 1024;
  /* One chunk is 64 KiB, the largest block the heap pool keeps on a free list.
     A registry of two million entries grows by appending a chunk, and no
     doubling copies the entries already registered. */
  static constexpr usize DESTRUCTORS_PER_CHUNK =
      DEFAULT_BLOCK_SIZE / sizeof(pending_destructor);

  ArrayList<block> m_blocks{heap_allocator()};
  /* The half-open address range covering every live block. A pointer outside it
     belongs to no block, so the ownership scan is skipped. The range only ever
     widens while blocks are added, and it stays conservative after a reset. */
  uintptr m_lowest_address{UINTPTR_MAX};
  uintptr m_highest_address{0};
  ArrayList<pending_destructor *> m_destructor_chunks{heap_allocator()};
  ArrayList<lifetime_slot> m_lifetime_slots{heap_allocator()};
  ArrayList<u32> m_active_lifetime_slots{heap_allocator()};
  usize m_destructor_count{0};
  usize m_reset_generation{0};
  u32 m_arena_incarnation{0};
  u32 m_first_free_lifetime_slot{UINT32_MAX};
  /* Every block above this index is empty, so a release rewinds the index and
     the blocks it reclaimed are handed out again. */
  usize m_current_index{0};

  fn add_block(usize minimum_size) throws -> void;
  fn push_destructor(pending_destructor pending) throws -> void;
  /* Run and drop every registered destructor from the index down to first, in
     reverse of registration so an object tears down before the one it followed.
   */
  fn run_destructors_down_to(usize first) wontthrow -> void;
  fn release_destructor_chunks(usize kept_chunk_count) wontthrow -> void;
};

/* The arena that the lexer and parser allocate nodes from while a command is
   being built. The operator delete on a node consults it to tell arena storage
   apart from an ordinary heap node. */
extern BumpArena *AST_ARENA;

/* The arena that holds function bodies. A function body outlives the command
   that defined it, so it is parsed here instead of the per-command arena. */
extern BumpArena *FUNCTION_ARENA;

fn is_arena_pointer(const opaque *pointer) wontthrow -> bool;

} /* namespace koshka */
