/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file tests the portable Batch queue against a deterministic backend.
 * It covers request ordering, result reuse, metadata deduplication, existence
 * results, and failed metadata requests without depending on a platform
 * filesystem. Minimal allocation definitions keep the test independent from
 * the shell entrypoint. A bounded release profile mode repeats the same matrix
 * so CPU, cache, and allocation tools measure Batch work instead of startup.
 */

#include "Platform.hpp"

namespace {

using namespace koshka;
using namespace koshka::os;

struct observed_operation
{
  batch_operation::Kind syscall_id{batch_operation::Kind::Read};
  descriptor fd{KOSH_INVALID_FD};
  char *output_buffer{nullptr};
  const char *input_buffer{nullptr};
  const Path *path{nullptr};
  u64 request_id{0};
  u64 byte_offset{0};
  usize byte_count{0};
};

observed_operation observed_operations[8]{};
usize execution_count = 0;
usize observed_operation_count = 0;
usize failure_count = 0;
bool should_fail_metadata = false;
bool should_report_existing = true;
bool should_validate_operations = false;
bool should_interrupt = false;

fn expect(bool is_true, const char *message) wontthrow -> void
{
  if (is_true) return;

  std::fprintf(stderr, "Batch contract failed: %s\n", message);
  failure_count++;
}

fn reset_observations() wontthrow -> void
{
  observed_operation_count = 0;
  for (let &operation : observed_operations)
    operation = {};
}

fn test_io_order_and_reuse() throws -> void
{
  let batch = Batch{heap_allocator()};
  let results = ArrayList<batch_result>{heap_allocator()};
  char read_buffer[4]{};
  const char write_buffer[]{'x', 'y'};
  const char current_write_buffer[]{'a', 'b', 'c'};

  expect(batch.count() == 0, "a new queue is empty");
  batch.add(
      batch_operation::read(KOSH_STDIN, read_buffer, sizeof(read_buffer), 12));
  batch.add(batch_operation::write(KOSH_STDOUT, write_buffer,
                                   sizeof(write_buffer), 20));
  batch.add(batch_operation::write_current(KOSH_STDOUT, current_write_buffer,
                                           sizeof(current_write_buffer)));
  expect(batch.count() == 3, "add updates the queue count");

  reset_observations();
  batch.execute(results);
  expect(execution_count == 1, "execute invokes the backend once");
  expect(observed_operation_count == 3,
         "the backend receives every queued operation");
  if (results.count() != 3) {
    expect(false, "execute returns one result per request");
    return;
  }

  expect(observed_operations[0].request_id == 0 &&
             observed_operations[1].request_id == 1 &&
             observed_operations[2].request_id == 2,
         "request identifiers follow insertion order");
  expect(observed_operations[0].syscall_id == batch_operation::Kind::Read,
         "the read operation keeps its kind");
  expect(observed_operations[0].fd == KOSH_STDIN,
         "the read operation keeps its descriptor");
  expect(observed_operations[0].output_buffer == read_buffer,
         "the read operation keeps its buffer");
  expect(observed_operations[0].byte_count == sizeof(read_buffer) &&
             observed_operations[0].byte_offset == 12,
         "the read operation keeps its byte range");
  expect(observed_operations[1].syscall_id == batch_operation::Kind::Write,
         "the write operation keeps its kind");
  expect(observed_operations[1].fd == KOSH_STDOUT,
         "the write operation keeps its descriptor");
  expect(observed_operations[1].input_buffer == write_buffer,
         "the write operation keeps its buffer");
  expect(observed_operations[1].byte_count == sizeof(write_buffer) &&
             observed_operations[1].byte_offset == 20,
         "the write operation keeps its byte range");
  expect(observed_operations[2].syscall_id ==
             batch_operation::Kind::WriteCurrent,
         "the current write operation keeps its kind");
  expect(observed_operations[2].fd == KOSH_STDOUT,
         "the current write operation keeps its descriptor");
  expect(observed_operations[2].input_buffer == current_write_buffer,
         "the current write operation keeps its buffer");
  expect(observed_operations[2].byte_count == sizeof(current_write_buffer) &&
             observed_operations[2].byte_offset == 0,
         "the current write operation has no positioned offset");
  expect(results[0].request_id == 0 && results[1].request_id == 1 &&
             results[2].request_id == 2,
         "results preserve request order");
  expect(results[0].transferred_byte_count == 16 &&
             results[1].transferred_byte_count == 22 &&
             results[2].transferred_byte_count == 3,
         "backend transfer counts are preserved");
  expect(results[0].error_number == 30 && results[1].error_number == 31 &&
             results[2].error_number == 32,
         "backend errors are preserved");
  expect(read_buffer[0] == 'A', "the backend receives the read buffer");

  batch.clear();
  expect(batch.count() == 0, "clear empties the queue");
  batch.add(batch_operation::read(KOSH_STDIN, read_buffer, 1, 3));
  reset_observations();
  batch.execute(results);
  expect(execution_count == 2, "a reused queue invokes the backend again");
  if (results.count() != 1) {
    expect(false, "execute clears a reused result vector");
    return;
  }

  expect(results[0].request_id == 0,
         "clear restarts request identifiers from zero");
}

fn test_metadata_deduplication() throws -> void
{
  let batch = Batch{heap_allocator()};
  let first_path = Path{};
  let second_path = Path{};
  file_status first_lstat{};
  file_status second_lstat{};
  file_status first_stat{};
  file_status second_stat{};
  char read_buffer[1]{};

  batch.add(batch_operation::lstat(first_path, first_lstat));
  batch.add(batch_operation::read(KOSH_STDIN, read_buffer, 1, 7));
  batch.add(batch_operation::lstat(second_path, second_lstat));
  batch.add(batch_operation::stat(first_path, first_stat));
  batch.add(batch_operation::stat(second_path, second_stat));

  reset_observations();
  let results = batch.execute();
  expect(observed_operation_count == 3,
         "duplicate metadata requests collapse before execution");
  expect(observed_operations[0].syscall_id == batch_operation::Kind::Lstat &&
             observed_operations[1].syscall_id == batch_operation::Kind::Read &&
             observed_operations[2].syscall_id == batch_operation::Kind::Stat,
         "different metadata kinds remain separate");
  if (results.count() != 5) {
    expect(false, "deduplicated results expand to the original request count");
    return;
  }

  for (usize index = 0; index < results.count(); index++)
    expect(results[index].request_id == index,
           "expanded results preserve request order");
  expect(results[0].transferred_byte_count ==
                 results[2].transferred_byte_count &&
             results[3].transferred_byte_count ==
                 results[4].transferred_byte_count,
         "duplicate requests share their backend result");
  expect(first_lstat.size == 1000 && second_lstat.size == 1000,
         "a deduplicated lstat result reaches both destinations");
  expect(first_stat.size == 1003 && second_stat.size == 1003,
         "a deduplicated stat result reaches both destinations");
}

fn test_failed_metadata_deduplication() throws -> void
{
  let batch = Batch{heap_allocator()};
  let first_path = Path{};
  let second_path = Path{};
  file_status first_status{};
  file_status second_status{};
  first_status.size = 41;
  second_status.size = 42;

  batch.add(batch_operation::lstat(first_path, first_status));
  batch.add(batch_operation::lstat(second_path, second_status));
  should_fail_metadata = true;
  reset_observations();
  let const results = batch.execute();
  should_fail_metadata = false;

  expect(observed_operation_count == 1,
         "failed duplicate metadata requests still collapse");
  if (results.count() != 2) {
    expect(false, "failed duplicates expand to the original request count");
    return;
  }

  expect(results[0].error_number == 90 && results[1].error_number == 90,
         "a canonical metadata error reaches every duplicate");
  expect(first_status.size == 41 && second_status.size == 42,
         "failed metadata requests do not copy status data");
}

fn test_exists_results_and_deduplication() throws -> void
{
  let batch = Batch{heap_allocator()};
  let first_path = Path{};
  let second_path = Path{};
  char read_buffer[1]{};

  batch.add(batch_operation::exists(first_path));
  batch.add(batch_operation::read(KOSH_STDIN, read_buffer, 1, 4));
  batch.add(batch_operation::exists(second_path));

  reset_observations();
  let results = batch.execute();
  expect(observed_operation_count == 2,
         "duplicate existence requests collapse before execution");
  expect(observed_operations[0].syscall_id == batch_operation::Kind::Exists &&
             observed_operations[0].path == &first_path,
         "the existence operation keeps its path");
  if (results.count() != 3) {
    expect(false, "existence results preserve the original request count");
    return;
  }

  for (usize index = 0; index < results.count(); index++)
    expect(results[index].request_id == index,
           "existence results preserve request order");
  expect(results[0].is_existing && results[2].is_existing,
         "a true existence result reaches every duplicate");
  expect(results[0].error_number == 0 && results[0].transferred_byte_count == 0,
         "an existence result is not a transfer or path error");

  should_report_existing = false;
  reset_observations();
  batch.execute(results);
  should_report_existing = true;
  expect(!results[0].is_existing && !results[2].is_existing,
         "a false existence result remains a successful observation");
  expect(results[0].error_number == 0 && results[2].error_number == 0,
         "a missing path is not a batch error");
}

fn test_many_duplicate_metadata_operations() throws -> void
{
  constexpr usize OPERATION_COUNT = 64;
  let batch = Batch{uncached_heap_allocator()};
  let path = Path{};
  file_status statuses[OPERATION_COUNT]{};
  batch.reserve(OPERATION_COUNT);
  for (usize index = 0; index < OPERATION_COUNT; index++)
    batch.add(batch_operation::lstat(path, statuses[index]));

  reset_observations();
  let const results = batch.execute();
  expect(observed_operation_count == 1,
         "many duplicate metadata requests collapse to one operation");
  if (results.count() != OPERATION_COUNT) {
    expect(false, "many duplicate results expand to the request count");
    return;
  }

  for (usize index = 0; index < OPERATION_COUNT; index++) {
    expect(results[index].request_id == index,
           "many duplicate results preserve request order");
    expect(statuses[index].size == 1000,
           "many duplicate requests publish every status destination");
  }
}

fn test_zero_partial_and_aliased_operations() throws -> void
{
  let batch = Batch{heap_allocator()};
  let path = Path{};
  file_status status{};
  char buffer[1]{};

  batch.add(batch_operation::read(KOSH_STDIN, buffer, 0));
  batch.add(batch_operation::lstat(path, status));
  batch.add(batch_operation::lstat(path, status));
  reset_observations();
  let const results = batch.execute();

  expect(observed_operation_count == 2,
         "zero-length and aliased metadata requests keep valid ordering");
  expect(results.count() == 3 && results[0].transferred_byte_count == 0,
         "zero-length transfers report no bytes");
  expect(status.size == 1001,
         "aliased metadata destinations receive the canonical result");
}

fn test_invalid_operation_result() throws -> void
{
  let batch = Batch{heap_allocator()};
  char buffer[1]{};
  batch.add(batch_operation::read(KOSH_INVALID_FD, buffer, sizeof(buffer)));

  should_validate_operations = true;
  let const results = batch.execute();
  should_validate_operations = false;
  expect(results.count() == 1 && results[0].error_number == 22,
         "invalid operations return their validation error");
}

fn test_interrupted_batch() throws -> void
{
  let batch = Batch{heap_allocator()};
  let path = Path{};
  file_status first_status{};
  file_status second_status{};
  batch.add(batch_operation::lstat(path, first_status));
  batch.add(batch_operation::lstat(path, second_status));

  should_interrupt = true;
  let const results = batch.execute();
  should_interrupt = false;
  expect(results.count() == 2 && results[0].error_number == 4 &&
             results[1].error_number == 4,
         "interrupted batches propagate cancellation to every request");
  expect(first_status.size == 0 && second_status.size == 0,
         "interrupted metadata requests do not publish status data");
}

fn test_oversized_mixed_batch() throws -> void
{
  let batch = Batch{heap_allocator()};
  char buffer[1]{};
  constexpr usize OPERATION_COUNT = 300;
  batch.reserve(OPERATION_COUNT);
  for (usize index = 0; index < OPERATION_COUNT; index++)
    batch.add(batch_operation::read(KOSH_STDIN, buffer, index % 2, index));

  reset_observations();
  let const results = batch.execute();
  expect(observed_operation_count == OPERATION_COUNT,
         "oversized batches reach the backend without truncation");
  expect(results.count() == OPERATION_COUNT &&
             results[OPERATION_COUNT - 1].request_id == OPERATION_COUNT - 1,
         "oversized results preserve request order");
}

fn run_batch_contract() -> int
{
  execution_count = 0;
  observed_operation_count = 0;
  failure_count = 0;
  should_fail_metadata = false;
  should_report_existing = true;
  should_validate_operations = false;
  should_interrupt = false;

  try {
    test_io_order_and_reuse();
    test_metadata_deduplication();
    test_failed_metadata_deduplication();
    test_exists_results_and_deduplication();
    test_many_duplicate_metadata_operations();
    test_zero_partial_and_aliased_operations();
    test_invalid_operation_result();
    test_interrupted_batch();
    test_oversized_mixed_batch();
  } catch (...) {
    std::fprintf(stderr, "Batch contract failed with an exception.\n");
    return 1;
  }

  return failure_count == 0 ? 0 : 1;
}

fn run_batch_profile() -> int
{
  constexpr usize PROFILE_ITERATION_COUNT = 10000;
  for (usize index = 0; index < PROFILE_ITERATION_COUNT; index++)
    if (run_batch_contract() != 0) return 1;

  return 0;
}

}

namespace koshka {

fn bump_arena_allocate(BumpArena *arena, usize length, usize alignment) throws
    -> opaque *
{
  unused(arena);
  unused(length);
  unused(alignment);
  throw std::bad_alloc{};
}

cold fn String::free_storage() wontthrow -> void
{
  if (m_data != nullptr && m_data != m_inline) {
    m_allocator.free_array(m_data, m_capacity);
  }
  reset_to_inline();
}

hot fn Path::text() const wontthrow -> const String & { return m_text; }

namespace os {

fn allocate_aligned(usize length, usize alignment) wontthrow -> opaque *
{
#if KOSH_PLATFORM_IS KOSH_PLATFORM_WIN32
  return _aligned_malloc(length, alignment);
#else
  return ::aligned_alloc(alignment, length);
#endif
}

fn free_aligned(opaque *pointer) wontthrow -> void
{
#if KOSH_PLATFORM_IS KOSH_PLATFORM_WIN32
  _aligned_free(pointer);
#else
  std::free(pointer);
#endif
}

}

namespace os::batch_internal {

fn execute_batch_operations(const batched_syscall *operations,
                            usize operation_count,
                            batch_result *results) wontthrow -> void
{
  execution_count++;
  observed_operation_count = operation_count;
  for (usize index = 0; index < operation_count; index++) {
    let const &operation = operations[index];
    if (index < sizeof(observed_operations) / sizeof(observed_operations[0]))
      observed_operations[index] = {
          batch_operation_access::get_kind(operation),
          batch_operation_access::get_descriptor(operation),
          batch_operation_access::get_output_buffer(operation),
          batch_operation_access::get_input_buffer(operation),
          batch_operation_access::get_path(operation),
          operation.request_id,
          operation.byte_offset,
          operation.byte_count};

    let &result = results[index];
    result.request_id = operation.request_id;
    if (should_interrupt) {
      result.error_number = 4;
      continue;
    }
    result.transferred_byte_count =
        operation.byte_count + operation.byte_offset;
    result.error_number =
        should_validate_operations && batch_operation_access::get_descriptor(
                                          operation) == KOSH_INVALID_FD
            ? 22
            : 30 + static_cast<i32>(operation.request_id);

    if (batch_operation_access::get_output_buffer(operation) != nullptr &&
        operation.byte_count != 0)
    {
      batch_operation_access::get_output_buffer(operation)[0] =
          static_cast<char>('A' + index);
    }

    if (batch_operation_access::get_kind(operation) ==
        batched_syscall_id::Exists)
    {
      result.transferred_byte_count = 0;
      result.error_number = 0;
      result.is_existing = should_report_existing;
      continue;
    }

    if (batch_operation_access::get_status(operation) == nullptr) continue;
    if (should_fail_metadata) {
      result.error_number = 90;
      continue;
    }

    result.error_number = 0;
    batch_operation_access::get_status(operation)->size =
        1000 + operation.request_id;
    batch_operation_access::get_status(operation)->file_id =
        2000 + operation.request_id;
  }
}
}
}

#if KOSH_PLATFORM_IS KOSH_PLATFORM_WIN32
fn wmain(int argument_count, wchar_t **arguments) -> int
{
  if (argument_count == 2 && lstrcmpW(arguments[1], L"--profile") == 0) {
    return run_batch_profile();
  }

  return run_batch_contract();
}
#else
fn main(int argument_count, char **arguments) -> int
{
  if (argument_count == 2 && std::strcmp(arguments[1], "--profile") == 0) {
    return run_batch_profile();
  }

  return run_batch_contract();
}
#endif
