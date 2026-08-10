#include "Arena.hpp"

#include "Allocator.hpp"
#include "Containers.hpp"
#include "Trace.hpp"

namespace shit {

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

BumpArena::BumpArena() = default;

BumpArena::~BumpArena()
{
  run_destructors_down_to(0);

  for (block &block : m_blocks)
    std::free(block.base);
}

cold fn BumpArena::run_destructors_down_to(usize first) wontthrow -> void
{
  while (m_destructors.count() > first) {
    let const pending = m_destructors.back();
    m_destructors.pop_back();
    pending.run(pending.object);
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

  m_blocks.push(block{base, size, 0});
}

hot fn BumpArena::allocate(usize size, usize alignment) throws -> opaque *
{
  loop
  {
    if (!m_blocks.is_empty()) {
      let &block = m_blocks.back();
      if (block.used > SIZE_MAX - (alignment - 1)) throw std::bad_alloc{};
      let const aligned = (block.used + (alignment - 1)) & ~(alignment - 1);

      if (aligned <= block.size && size <= block.size - aligned) [[likely]] {
        ASSERT(block.base != nullptr);

        let const pointer = block.base + aligned;
        block.used = aligned + size;

        return pointer;
      }
    }

    if (size > SIZE_MAX - alignment) throw std::bad_alloc{};
    add_block(size + alignment);
  }
}

hot fn BumpArena::owns(const opaque *pointer) const wontthrow -> bool
{
  let const candidate = reinterpret_cast<uintptr>(pointer);
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
  if (m_blocks.is_empty()) return Mark{0, 0, m_destructors.count()};
  return Mark{m_blocks.count(), m_blocks.back().used, m_destructors.count()};
}

fn BumpArena::release(Mark saved) wontthrow -> void
{
  ASSERT(saved.block_count <= m_blocks.count(),
         "mark cannot name more blocks than the arena holds");

  run_destructors_down_to(saved.destructor_count);

  for (usize i = saved.block_count; i < m_blocks.count(); i++)
    m_blocks[i].used = 0;

  if (saved.block_count > 0) {
    ASSERT(saved.used_in_last <= m_blocks[saved.block_count - 1].size);
    m_blocks[saved.block_count - 1].used = saved.used_in_last;
  }
}

cold fn BumpArena::reset() wontthrow -> void
{
  LOG(All, "resetting the arena holding %zu blocks and %zu used bytes",
      m_blocks.count(), bytes_used());

  run_destructors_down_to(0);

  /* Bumping the generation invalidates any cache keyed on an earlier one. */
  m_reset_generation++;

  for (usize i = 1; i < m_blocks.count(); i++)
    std::free(m_blocks[i].base);
  while (m_blocks.count() > 1)
    m_blocks.pop_back();
  if (!m_blocks.is_empty()) m_blocks.front().used = 0;
}

} // namespace shit
