#include "Allocator.hpp"
#include "Debug.hpp"
#include "EvalTypes.hpp"

namespace koshka {

static function_body_storage *LIVE_FUNCTION_STORAGES = nullptr;
static constexpr usize INITIAL_FUNCTION_ARENA_SIZE = 16 * 1024;

function_body_storage::function_body_storage(BumpArena *owned_arena)
    : arena(owned_arena)
{
  next_live = LIVE_FUNCTION_STORAGES;
  if (next_live != nullptr) next_live->previous_live = this;
  LIVE_FUNCTION_STORAGES = this;
}

function_body_storage::~function_body_storage()
{
  ASSERT(arena != nullptr);
  if (previous_live != nullptr)
    previous_live->next_live = next_live;
  else
    LIVE_FUNCTION_STORAGES = next_live;
  if (next_live != nullptr) next_live->previous_live = previous_live;

  let const previous_function_arena = FUNCTION_ARENA;
  FUNCTION_ARENA = arena;
  arena->~BumpArena();
  FUNCTION_ARENA = previous_function_arena;
  heap_allocator().free_array(arena, 1);
}

FunctionBodyHandle::FunctionBodyHandle(const FunctionBodyHandle &other)
    : m_storage(other.m_storage)
{
  retain();
}

FunctionBodyHandle::FunctionBodyHandle(FunctionBodyHandle &&other) noexcept
    : m_storage(other.m_storage)
{
  other.m_storage = nullptr;
}

FunctionBodyHandle::~FunctionBodyHandle() { release(); }

fn FunctionBodyHandle::operator=(const FunctionBodyHandle &other)
    -> FunctionBodyHandle &
{
  if (this == &other) return *this;

  release();
  m_storage = other.m_storage;
  retain();
  return *this;
}

fn FunctionBodyHandle::operator=(FunctionBodyHandle &&other) noexcept
    -> FunctionBodyHandle &
{
  if (this == &other) return *this;

  release();
  m_storage = other.m_storage;
  other.m_storage = nullptr;
  return *this;
}

fn FunctionBodyHandle::create() throws -> FunctionBodyHandle
{
  let const arena_storage = heap_allocator().alloc_array<BumpArena>(1);
  try {
    new (arena_storage) BumpArena{INITIAL_FUNCTION_ARENA_SIZE};
  } catch (...) {
    heap_allocator().free_array(arena_storage, 1);
    throw;
  }

  let *storage = static_cast<function_body_storage *>(nullptr);
  try {
    storage = heap_allocator().alloc_array<function_body_storage>(1);
    new (storage) function_body_storage{arena_storage};
  } catch (...) {
    arena_storage->~BumpArena();
    heap_allocator().free_array(arena_storage, 1);
    throw;
  }

  return FunctionBodyHandle{storage};
}

pure fn FunctionBodyHandle::get_arena() const wontthrow -> BumpArena *
{
  return m_storage != nullptr ? m_storage->arena : nullptr;
}

pure fn FunctionBodyHandle::get_body() const wontthrow -> const Expression *
{
  return m_storage != nullptr ? m_storage->body : nullptr;
}

pure fn FunctionBodyHandle::get_source() const wontthrow -> const String *
{
  return m_storage != nullptr ? &m_storage->source : nullptr;
}

pure fn FunctionBodyHandle::get_definition_info() const wontthrow
    -> const function_definition_info *
{
  return m_storage != nullptr ? &m_storage->definition_info : nullptr;
}

fn FunctionBodyHandle::set_body(const Expression *body) wontthrow -> void
{
  ASSERT(m_storage != nullptr);
  m_storage->body = body;
}

fn FunctionBodyHandle::set_definition(
    StringView source, function_definition_info definition_info) const throws
    -> void
{
  ASSERT(m_storage != nullptr);

  let owned_source = String{heap_allocator(), source};
  m_storage->source = steal(owned_source);
  m_storage->definition_info = steal(definition_info);
}

pure fn FunctionBodyHandle::get_stats() const wontthrow -> function_arena_stats
{
  if (m_storage == nullptr) return {};

  let const &arena = *m_storage->arena;
  return {arena.bytes_used(), arena.bytes_capacity(), arena.block_count(),
          arena.destructor_count(), arena.destructor_capacity()};
}

pure fn live_function_storage_stats() wontthrow -> function_arena_stats
{
  function_arena_stats total{};

  for (let const *storage = LIVE_FUNCTION_STORAGES; storage != nullptr;
       storage = storage->next_live)
  {
    let const &arena = *storage->arena;
    total.bytes_used += arena.bytes_used();
    total.bytes_capacity += arena.bytes_capacity();
    total.block_count += arena.block_count();
    total.destructor_count += arena.destructor_count();
    total.destructor_capacity += arena.destructor_capacity();
  }

  return total;
}

fn FunctionBodyHandle::retain() wontthrow -> void
{
  if (m_storage != nullptr) m_storage->reference_count++;
}

fn FunctionBodyHandle::release() wontthrow -> void
{
  if (m_storage == nullptr) return;

  ASSERT(m_storage->reference_count > 0);
  m_storage->reference_count--;
  if (m_storage->reference_count == 0) {
    m_storage->~function_body_storage();
    heap_allocator().free_array(m_storage, 1);
  }
  m_storage = nullptr;
}

} /* namespace koshka */
