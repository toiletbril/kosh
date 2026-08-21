#pragma once

#include "Common.hpp"
#include "Debug.hpp"

#include <new>

namespace koshka {

class BumpArena;
fn bump_arena_allocate(BumpArena *arena, usize length, usize alignment) throws
    -> opaque *;
fn bump_arena_owns(const BumpArena *arena, const opaque *pointer) wontthrow
    -> bool;

namespace os {
fn allocate_aligned(usize length, usize alignment) wontthrow -> opaque *;
fn free_aligned(opaque *pointer) wontthrow -> void;
} /* namespace os */

namespace allocators {

/* A size-classed cache over the C allocator. musl returns a freed page group to
   the kernel at once, so a tight allocate then free of the same size churns
   mmap and munmap once per turn, which dominates the bench on Alpine where
   glibc would have cached the page. A freed block parks on a per-class free
   list and is handed back on the next request of that class, so the kernel sees
   a steady working set. The cache is bounded per class so a burst does not pin
   memory. The pool is single threaded, since the evaluator never shares an
   allocator across threads. */
class HeapPool
{
public:
  hot fn take(usize length) wontthrow -> opaque *
  {
    let const shift = class_shift_for(length);
    if (shift > MAX_CLASS_SHIFT) return std::malloc(length);

    let const class_index = shift - MIN_CLASS_SHIFT;
    if (m_bins[class_index] != nullptr) {
      let const reused = m_bins[class_index];
      m_bins[class_index] = reused->next;
      m_counts[class_index]--;
      return reused;
    }

    return std::malloc(usize{1} << shift);
  }

  hot fn give(opaque *pointer, usize length) wontthrow -> void
  {
    if (pointer == nullptr) return;

    let const shift = class_shift_for(length);
    if (shift > MAX_CLASS_SHIFT) {
      std::free(pointer);
      return;
    }

    let const class_index = shift - MIN_CLASS_SHIFT;
    if (m_counts[class_index] >= MAX_BLOCKS_PER_CLASS) {
      std::free(pointer);
      return;
    }

    let const recycled = static_cast<node *>(pointer);
    recycled->next = m_bins[class_index];
    m_bins[class_index] = recycled;
    m_counts[class_index]++;
  }

private:
  static constexpr usize MIN_CLASS_SHIFT =
      4; /* the smallest class is 16 bytes */
  static constexpr usize MAX_CLASS_SHIFT =
      16; /* the largest pooled block is 64 KiB */
  static constexpr usize CLASS_COUNT = MAX_CLASS_SHIFT - MIN_CLASS_SHIFT + 1;
  static constexpr usize MAX_BLOCKS_PER_CLASS = 512;

  struct node
  {
    node *next;
  };

  node *m_bins[CLASS_COUNT] = {};
  u32 m_counts[CLASS_COUNT] = {};

  hot static fn class_shift_for(usize length) wontthrow -> usize
  {
    let const size = length <= (usize{1} << MIN_CLASS_SHIFT)
                         ? (usize{1} << MIN_CLASS_SHIFT)
                         : length;
    let shift = static_cast<usize>(64 - __builtin_clzll(size - 1));
    if (shift < MIN_CLASS_SHIFT) shift = MIN_CLASS_SHIFT;
    return shift;
  }
};

/* The single process-wide cache, one instance across every translation unit
   through the inline function local static. The pool is trivially destructible,
   so it registers no exit destructor and its storage stays valid through
   process teardown. A heap free from a file-scope cache destructor at process
   exit then reaches live storage whatever the static destruction order names.
 */
hot inline fn heap_pool_instance() wontthrow -> HeapPool &
{
  static HeapPool pool;
  return pool;
}

hot inline fn heap_alloc(usize length, usize alignment) wontthrow -> opaque *
{
  /* malloc already meets every alignment up to alignof(max_align_t), so the
  common request stays on the pooled path. The over-aligned path is rare and
     stays uncached, and its length is rounded up to a multiple of the alignment
     for aligned_alloc. */
  if (alignment > alignof(max_align_t)) {
    if (length > SIZE_MAX - (alignment - 1)) return nullptr;
    let const rounded_length = (length + alignment - 1) & ~(alignment - 1);
    return os::allocate_aligned(rounded_length, alignment);
  }
#if defined KOSH_HAS_ADDRESS_SANITIZER
  return std::malloc(length);
#else
  return heap_pool_instance().take(length);
#endif
}
hot inline fn heap_free(opaque *pointer, usize length,
                        usize alignment) wontthrow -> void
{
  /* An over-aligned block skips the pool, so a pooled block always belongs to
     one size class. */
  if (alignment > alignof(max_align_t)) {
    os::free_aligned(pointer);
    return;
  }
#if defined KOSH_HAS_ADDRESS_SANITIZER
  unused(length);
  std::free(pointer);
#else
  heap_pool_instance().give(pointer, length);
#endif
}

} /* namespace allocators */

/* One tagged word. The three kinds are the pooled heap, a bump arena, and the
   fake allocator a container carries while it holds no storage. An arena is
   aligned well past four bytes, so the two low bits carry the kind and the
   remaining bits carry the arena address. The heap and the fake kind hold no
   address. */
class Allocator
{
public:
  enum class Kind : uintptr
  {
    Heap = 0,
    Bump = 1,
    Fake = 2,
  };

  static constexpr uintptr KIND_MASK = 3;

  uintptr tagged;

  pure fn get_kind() const wontthrow -> Kind
  {
    return static_cast<Kind>(tagged & KIND_MASK);
  }

  pure fn operator==(Allocator other) const wontthrow->bool
  {
    return tagged == other.tagged;
  }

  pure fn owns(const opaque *pointer) const wontthrow -> bool
  {
    switch (get_kind()) {
    case Kind::Heap: return false;
    case Kind::Bump: return bump_arena_owns(get_arena(), pointer);
    case Kind::Fake: return false;
    }

    unreachable("the allocator carries no known kind");
  }

  hot flatten fn raw_alloc(usize length, usize alignment) const throws
      -> opaque *
  {
    switch (get_kind()) {
    case Kind::Heap: return allocators::heap_alloc(length, alignment);
    case Kind::Bump: return bump_arena_allocate(get_arena(), length, alignment);
    case Kind::Fake:
      unreachable("a container with the fake allocator attempted to allocate");
    }

    unreachable("the allocator carries no known kind");
  }

  flatten fn raw_free(opaque *pointer, usize length,
                      usize alignment) const wontthrow -> void
  {
    /* An arena hands nothing back, and the fake allocator never handed anything
       out. */
    if (get_kind() != Kind::Heap) return;

    allocators::heap_free(pointer, length, alignment);
  }

  template <class T>
  hot flatten fn alloc_array(usize count) const throws -> T *
  {
    /* The product overflows usize for a large enough count, wrapping to a small
       request that the caller then writes past. The division guards the
       multiply, since count times sizeof(T) cannot exceed the max when count is
       at most the max divided by sizeof(T). */
    if (sizeof(T) != 0 && count > (static_cast<usize>(-1) / sizeof(T)))
        [[unlikely]]
    {
      throw std::bad_alloc{};
    }
    return static_cast<T *>(raw_alloc(count * sizeof(T), alignof(T)));
  }
  template <class T>
  flatten fn free_array(T *pointer, usize count) const wontthrow -> void
  {
    raw_free(pointer, count * sizeof(T), alignof(T));
  }

private:
  pure fn get_arena() const wontthrow -> BumpArena *
  {
    return reinterpret_cast<BumpArena *>(tagged & ~KIND_MASK);
  }
};

static_assert(sizeof(usize) != 8 || sizeof(Allocator) == 8);

inline fn bump_allocator(BumpArena &arena) wontthrow -> Allocator
{
  let const address = reinterpret_cast<uintptr>(&arena);
  ASSERT((address & Allocator::KIND_MASK) == 0,
         "an arena address must leave the two tag bits clear");

  return Allocator{address | static_cast<uintptr>(Allocator::Kind::Bump)};
}

inline fn heap_allocator() wontthrow -> Allocator
{
  return Allocator{static_cast<uintptr>(Allocator::Kind::Heap)};
}

inline fn fake_allocator() wontthrow -> Allocator
{
  return Allocator{static_cast<uintptr>(Allocator::Kind::Fake)};
}

} /* namespace koshka */
