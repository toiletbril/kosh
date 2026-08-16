#pragma once

#include "Common.hpp"
#include "Debug.hpp"

#include <new>

namespace koshka {

class BumpArena;
fn bump_arena_allocate(BumpArena *arena, usize length, usize alignment) throws
    -> opaque *;

namespace os {
fn allocate_aligned(usize length, usize alignment) wontthrow -> opaque *;
fn free_aligned(opaque *pointer) wontthrow -> void;
} /* namespace os */

class Allocator
{
public:
  struct VTable
  {
    opaque *(*alloc)(opaque *context, usize length, usize alignment);
    void (*free)(opaque *context, opaque *pointer, usize length,
                 usize alignment);
  };

  opaque *context;
  const VTable *vtable;

  hot flatten fn raw_alloc(usize length, usize alignment) const throws
      -> opaque *
  {
    return vtable->alloc(context, length, alignment);
  }
  flatten fn raw_free(opaque *pointer, usize length,
                      usize alignment) const wontthrow -> void
  {
    vtable->free(context, pointer, length, alignment);
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
};

namespace allocators {

hot inline fn bump_alloc(opaque *context, usize length, usize alignment) throws
    -> opaque *
{
  return bump_arena_allocate(static_cast<BumpArena *>(context), length,
                             alignment);
}
inline fn bump_free(opaque *context, opaque *pointer, usize length,
                    usize alignment) wontthrow -> void
{
  unused(context);
  unused(pointer);
  unused(length);
  unused(alignment);
}

inline constexpr Allocator::VTable BUMP_VTABLE{bump_alloc, bump_free};

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

hot inline fn heap_alloc(opaque *context, usize length,
                         usize alignment) wontthrow -> opaque *
{
  unused(context);
  /* malloc already meets every alignment up to alignof(max_align_t), so the
  common request stays on the pooled path. The over-aligned path is rare and
     stays uncached, and its length is rounded up to a multiple of the alignment
     for aligned_alloc. */
  if (alignment > alignof(max_align_t)) {
    if (length > SIZE_MAX - (alignment - 1)) return nullptr;
    let const rounded_length = (length + alignment - 1) & ~(alignment - 1);
    return os::allocate_aligned(rounded_length, alignment);
  }
  return heap_pool_instance().take(length);
}
hot inline fn heap_free(opaque *context, opaque *pointer, usize length,
                        usize alignment) wontthrow -> void
{
  unused(context);
  /* An over-aligned block skips the pool, so a pooled block always belongs to
     one size class. */
  if (alignment > alignof(max_align_t)) {
    os::free_aligned(pointer);
    return;
  }
  heap_pool_instance().give(pointer, length);
}

inline constexpr Allocator::VTable HEAP_VTABLE{heap_alloc, heap_free};

inline fn fake_alloc(opaque *context, usize length, usize alignment) wontthrow
    -> opaque *
{
  unused(context);
  unused(length);
  unused(alignment);
  unreachable("a container with the fake allocator attempted to allocate");
}
inline fn fake_free(opaque *context, opaque *pointer, usize length,
                    usize alignment) wontthrow -> void
{
  unused(context);
  unused(pointer);
  unused(length);
  unused(alignment);
}

inline constexpr Allocator::VTable FAKE_VTABLE{fake_alloc, fake_free};

} /* namespace allocators */

inline fn bump_allocator(BumpArena &arena) wontthrow -> Allocator
{
  return Allocator{&arena, &allocators::BUMP_VTABLE};
}

inline fn heap_allocator() wontthrow -> Allocator
{
  return Allocator{nullptr, &allocators::HEAP_VTABLE};
}

inline fn fake_allocator() wontthrow -> Allocator
{
  return Allocator{nullptr, &allocators::FAKE_VTABLE};
}

} /* namespace koshka */
