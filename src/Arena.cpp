#include "Arena.hpp"

#include "Allocator.hpp"
#include "Containers.hpp"
#include "Trace.hpp"

namespace koshka {

BumpArena *AST_ARENA = nullptr;
BumpArena *FUNCTION_ARENA = nullptr;
static u32 NEXT_ARENA_INCARNATION = 1;

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

BumpArena::BumpArena() : m_arena_incarnation{NEXT_ARENA_INCARNATION++} {}

BumpArena::BumpArena(usize initial_block_size)
    : m_arena_incarnation{NEXT_ARENA_INCARNATION++}
{
  ASSERT(initial_block_size > 0);
  add_block(initial_block_size, initial_block_size);
}

BumpArena::~BumpArena()
{
  run_destructors_down_to(0);
  release_destructor_chunks(0);

  for (block &block : m_blocks)
    std::free(block.base);
}

fn BumpArena::push_destructor(pending_destructor pending) throws -> void
{
  usize chunk_index = 0;
  usize position_in_chunk = m_destructor_count;
  if (m_destructor_count >= FIRST_DESTRUCTOR_CHUNK_COUNT) {
    let const later_position =
        m_destructor_count - FIRST_DESTRUCTOR_CHUNK_COUNT;
    chunk_index = 1 + later_position / DESTRUCTORS_PER_CHUNK;
    position_in_chunk = later_position % DESTRUCTORS_PER_CHUNK;
  }

  if (chunk_index == m_destructor_chunks.count()) [[unlikely]] {
    let const chunk_count =
        chunk_index == 0 ? FIRST_DESTRUCTOR_CHUNK_COUNT : DESTRUCTORS_PER_CHUNK;
    let const chunk =
        heap_allocator().alloc_array<pending_destructor>(chunk_count);
    try {
      m_destructor_chunks.push(chunk);
    } catch (...) {
      heap_allocator().free_array(chunk, chunk_count);
      throw;
    }
  }

  m_destructor_chunks[chunk_index][position_in_chunk] = pending;
  m_destructor_count++;
}

fn BumpArena::run_destructors_down_to(usize first) wontthrow -> void
{
  while (m_destructor_count > first) {
    m_destructor_count--;
    usize chunk_index = 0;
    usize position_in_chunk = m_destructor_count;
    if (m_destructor_count >= FIRST_DESTRUCTOR_CHUNK_COUNT) {
      let const later_position =
          m_destructor_count - FIRST_DESTRUCTOR_CHUNK_COUNT;
      chunk_index = 1 + later_position / DESTRUCTORS_PER_CHUNK;
      position_in_chunk = later_position % DESTRUCTORS_PER_CHUNK;
    }
    let const &pending = m_destructor_chunks[chunk_index][position_in_chunk];
    pending.run(pending.object);
  }
}

/* The kept chunks are handed to the next fill without another allocation. */
cold fn BumpArena::release_destructor_chunks(usize kept_chunk_count) wontthrow
    -> void
{
  while (m_destructor_chunks.count() > kept_chunk_count) {
    let const chunk_count = m_destructor_chunks.count() == 1
                                ? FIRST_DESTRUCTOR_CHUNK_COUNT
                                : DESTRUCTORS_PER_CHUNK;
    heap_allocator().free_array(m_destructor_chunks.back(), chunk_count);
    m_destructor_chunks.pop_back();
  }
}

cold fn BumpArena::add_block(usize minimum_size, usize preferred_size) throws
    -> void
{
  let size = preferred_size;
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

    add_block(size + alignment, DEFAULT_BLOCK_SIZE);
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

fn BumpArena::register_lifetime() throws -> LifetimeIdentity
{
  u32 slot_position = m_first_free_lifetime_slot;
  if (slot_position == UINT32_MAX) {
    if (m_lifetime_slots.count() >= UINT32_MAX) throw std::bad_alloc{};
    slot_position = static_cast<u32>(m_lifetime_slots.count());
    m_lifetime_slots.push(lifetime_slot{});
  } else {
    ASSERT(m_lifetime_slots[slot_position].next_free_position != slot_position,
           "a free lifetime slot cannot point to itself");
    m_first_free_lifetime_slot =
        m_lifetime_slots[slot_position].next_free_position;
  }

  let &slot = m_lifetime_slots[slot_position];
  slot.payload_end = mark();
  slot.next_free_position = slot_position;
  slot.incarnation++;
  m_active_lifetime_slots.push(slot_position);
  return LifetimeIdentity{m_arena_incarnation, slot_position, slot.incarnation};
}

pure fn BumpArena::is_lifetime_valid(LifetimeIdentity identity) const wontthrow
    -> bool
{
  if (identity.arena_incarnation != m_arena_incarnation ||
      identity.slot_position >= m_lifetime_slots.count())
    return false;

  let const &slot = m_lifetime_slots[identity.slot_position];
  return slot.next_free_position == identity.slot_position &&
         slot.incarnation == identity.slot_incarnation;
}

fn BumpArena::release(Mark saved) wontthrow -> void
{
  ASSERT(saved.block_index <= m_current_index,
         "mark cannot name a block above the current one");

  run_destructors_down_to(saved.destructor_count);
  m_reset_generation++;

  while (!m_active_lifetime_slots.is_empty()) {
    let const slot_position = m_active_lifetime_slots.back();
    let &slot = m_lifetime_slots[slot_position];
    if (slot.payload_end.block_index < saved.block_index ||
        (slot.payload_end.block_index == saved.block_index &&
         slot.payload_end.used_in_block <= saved.used_in_block))
      break;

    slot.next_free_position = m_first_free_lifetime_slot;
    m_first_free_lifetime_slot = slot_position;
    m_active_lifetime_slots.pop_back();
  }

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
  m_reset_generation++;

  for (let const slot_position : m_active_lifetime_slots) {
    let &slot = m_lifetime_slots[slot_position];
    slot.next_free_position = m_first_free_lifetime_slot;
    m_first_free_lifetime_slot = slot_position;
  }
  m_active_lifetime_slots.clear();

  for (usize i = 1; i < m_blocks.count(); i++)
    std::free(m_blocks[i].base);
  while (m_blocks.count() > 1)
    m_blocks.pop_back();
  if (!m_blocks.is_empty()) m_blocks.front().used = 0;

  m_current_index = 0;
}

} /* namespace koshka */
