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

  /* Counts how many times the arena has been reset. A cache that holds a
     pointer into this arena stores the generation it was filled in, so a hit
     after a reset is recognised as stale and refilled. */
  pure fn reset_generation() const wontthrow -> usize
  {
    return m_reset_generation;
  }

  fn bytes_used() const wontthrow -> usize;

  fn block_count() const wontthrow -> usize { return m_blocks.count(); }
  fn bytes_capacity() const wontthrow -> usize
  {
    usize total = 0;
    for (const block &block : m_blocks)
      total += block.size;
    return total;
  }

  /* A saved bump position, so a scope can reclaim everything it allocated above
     the mark while leaving earlier allocations alone. The marks nest. */
  struct Mark
  {
    usize block_count;
    usize used_in_last;
    usize destructor_count;
  };
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
      try {
        m_destructors.push(
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

  struct pending_destructor
  {
    opaque *object;
    void (*run)(opaque *) noexcept;
  };

  static constexpr usize DEFAULT_BLOCK_SIZE = 64 * 1024;

  ArrayList<block> m_blocks{heap_allocator()};
  ArrayList<pending_destructor> m_destructors{heap_allocator()};
  usize m_reset_generation{0};

  fn add_block(usize minimum_size) throws -> void;
  /* Run and drop every registered destructor from the index down to first, in
     reverse of registration so an object tears down before the one it followed.
   */
  fn run_destructors_down_to(usize first) wontthrow -> void;
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
