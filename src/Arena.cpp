#include "Arena.hpp"

#include "Allocator.hpp"
#include "Containers.hpp"
#include "Trace.hpp"

namespace koshka {

BumpArena *AST_ARENA = nullptr;
BumpArena *FUNCTION_ARENA = nullptr;

fn is_arena_pointer(const opaque *pointer) wontthrow -> bool
{
  return (AST_ARENA != nullptr && AST_ARENA->owns(pointer)) ||
         (FUNCTION_ARENA != nullptr && FUNCTION_ARENA->owns(pointer));
}

hot fn bump_arena_allocate(BumpArena *arena, usize length,
                           usize alignment) throws -> opaque *
{
  return arena->allocate(length, alignment);
}

fn bump_arena_owns(const BumpArena *arena, const opaque *pointer) wontthrow
    -> bool
{
  return arena != nullptr && arena->owns(pointer);
}

BumpArena::BumpArena() = default;

BumpArena::~BumpArena()
{
  run_destructors_down_to(0);
  release_destructor_chunks(0);

  for (block &block : m_blocks)
    std::free(block.base);
}

fn BumpArena::push_destructor(pending_destructor pending) throws -> void
{
  let const chunk_index = m_destructor_count / DESTRUCTORS_PER_CHUNK;

  if (chunk_index == m_destructor_chunks.count()) [[unlikely]] {
    let const chunk =
        heap_allocator().alloc_array<pending_destructor>(DESTRUCTORS_PER_CHUNK);
    try {
      m_destructor_chunks.push(chunk);
    } catch (...) {
      heap_allocator().free_array(chunk, DESTRUCTORS_PER_CHUNK);
      throw;
    }
  }

  m_destructor_chunks[chunk_index][m_destructor_count % DESTRUCTORS_PER_CHUNK] =
      pending;
  m_destructor_count++;
}

fn BumpArena::run_destructors_down_to(usize first) wontthrow -> void
{
  while (m_destructor_count > first) {
    m_destructor_count--;
    let const &pending =
        m_destructor_chunks[m_destructor_count / DESTRUCTORS_PER_CHUNK]
                           [m_destructor_count % DESTRUCTORS_PER_CHUNK];
    pending.run(pending.object);
  }
}

/* The kept chunks are handed to the next fill without another allocation. */
cold fn BumpArena::release_destructor_chunks(usize kept_chunk_count) wontthrow
    -> void
{
  while (m_destructor_chunks.count() > kept_chunk_count) {
    heap_allocator().free_array(m_destructor_chunks.back(),
                                DESTRUCTORS_PER_CHUNK);
    m_destructor_chunks.pop_back();
  }
}

cold fn BumpArena::add_block(usize minimum_size) throws -> void
{
  let size = DEFAULT_BLOCK_SIZE;
  if (minimum_size > size) size = minimum_size;

  let const base = static_cast<u8 *>(std::malloc(size));
  if (base == nullptr) throw std::bad_alloc{};

  ASSERT(size >= minimum_size, "fresh block must fit the requested allocation");

  LOG(All, "mapping a new arena block of %zu bytes", size);

  try {
    m_blocks.push(block{base, size, 0});
  } catch (...) {
    std::free(base);
    throw;
  }

  let const low = reinterpret_cast<uintptr>(base);
  if (low < m_lowest_address) m_lowest_address = low;
  if (low + size > m_highest_address) m_highest_address = low + size;
}

hot fn BumpArena::allocate(usize size, usize alignment) throws -> opaque *
{
  if (size > SIZE_MAX - alignment) throw std::bad_alloc{};

  loop
  {
    while (m_current_index < m_blocks.count()) {
      let &block = m_blocks[m_current_index];
      let const aligned = (block.used + (alignment - 1)) & ~(alignment - 1);

      if (aligned <= block.size && size <= block.size - aligned) [[likely]] {
        ASSERT(block.base != nullptr);

        let const pointer = block.base + aligned;
        block.used = aligned + size;

        return pointer;
      }

      m_current_index++;
    }

    add_block(size + alignment);
    m_current_index = m_blocks.count() - 1;
  }
}

hot fn BumpArena::owns(const opaque *pointer) const wontthrow -> bool
{
  let const candidate = reinterpret_cast<uintptr>(pointer);
  if (candidate < m_lowest_address || candidate >= m_highest_address) {
    return false;
  }

  for (usize i = m_blocks.count(); i > 0; i--) {
    const block &block = m_blocks[i - 1];
    let const base = reinterpret_cast<uintptr>(block.base);
    if (candidate >= base && candidate - base < block.size) {
      return true;
    }
  }
  return false;
}

fn BumpArena::bytes_used() const wontthrow -> usize
{
  usize total = 0;
  for (const block &block : m_blocks)
    total += block.used;
  return total;
}

fn BumpArena::mark() const wontthrow -> BumpArena::Mark
{
  if (m_current_index >= m_blocks.count())
    return Mark{m_current_index, 0, m_destructor_count};

  return Mark{m_current_index, m_blocks[m_current_index].used,
              m_destructor_count};
}

fn BumpArena::release(Mark saved) wontthrow -> void
{
  ASSERT(saved.block_index <= m_current_index,
         "mark cannot name a block above the current one");

  run_destructors_down_to(saved.destructor_count);

  /* The rewound span is handed out again, so a cache holding an address inside
     it would read a later object. */
  m_reset_generation++;

  for (usize i = saved.block_index + 1; i < m_blocks.count(); i++)
    m_blocks[i].used = 0;

  if (saved.block_index < m_blocks.count()) {
    ASSERT(saved.used_in_block <= m_blocks[saved.block_index].size);
    m_blocks[saved.block_index].used = saved.used_in_block;
  }

  m_current_index = saved.block_index;
}

cold fn BumpArena::reset() wontthrow -> void
{
  LOG(All, "resetting the arena holding %zu blocks and %zu used bytes",
      m_blocks.count(), bytes_used());

  run_destructors_down_to(0);
  release_destructor_chunks(1);

  /* Bumping the generation invalidates any cache keyed on an earlier one. */
  m_reset_generation++;

  for (usize i = 1; i < m_blocks.count(); i++)
    std::free(m_blocks[i].base);
  while (m_blocks.count() > 1)
    m_blocks.pop_back();
  if (!m_blocks.is_empty()) m_blocks.front().used = 0;

  m_current_index = 0;
}

} /* namespace koshka */
