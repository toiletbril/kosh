/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements the portable Batch operation queue. It stores requests
 * in insertion order, assigns their stable result identifiers, and submits the
 * complete vector to the active platform backend in one operation.
 */

#include "Platform.hpp"

namespace koshka::os {

fn batch_operation::read(descriptor fd, char *buffer, usize byte_count,
                         u64 byte_offset) wontthrow -> batch_operation
{
  batch_operation operation;
  operation.syscall_id = Kind::Read;
  operation.fd = fd;
  operation.output_buffer = buffer;
  operation.byte_count = byte_count;
  operation.byte_offset = byte_offset;
  return operation;
}

fn batch_operation::write(descriptor fd, const char *buffer, usize byte_count,
                          u64 byte_offset) wontthrow -> batch_operation
{
  batch_operation operation;
  operation.syscall_id = Kind::Write;
  operation.fd = fd;
  operation.input_buffer = buffer;
  operation.byte_count = byte_count;
  operation.byte_offset = byte_offset;
  return operation;
}

fn batch_operation::write_current(descriptor fd, const char *buffer,
                                  usize byte_count) wontthrow -> batch_operation
{
  batch_operation operation;
  operation.syscall_id = Kind::WriteCurrent;
  operation.fd = fd;
  operation.input_buffer = buffer;
  operation.byte_count = byte_count;
  return operation;
}

fn batch_operation::lstat(const Path &path, file_status &status) wontthrow
    -> batch_operation
{
  batch_operation operation;
  operation.syscall_id = Kind::Lstat;
  operation.path = &path;
  operation.status = &status;
  return operation;
}

fn batch_operation::stat(const Path &path, file_status &status) wontthrow
    -> batch_operation
{
  batch_operation operation;
  operation.syscall_id = Kind::Stat;
  operation.path = &path;
  operation.status = &status;
  return operation;
}

fn batch_operation::exists(const Path &path) wontthrow -> batch_operation
{
  batch_operation operation;
  operation.syscall_id = Kind::Exists;
  operation.path = &path;
  return operation;
}

Batch::Batch(Allocator allocator) : m_operations(allocator) {}

fn Batch::reserve(usize operation_count) throws -> void
{
  m_operations.reserve(operation_count);
}

fn Batch::add(batch_operation operation) throws -> void
{
  operation.request_id = m_operations.count();
  m_operations.push(steal(operation));
}

fn Batch::clear() wontthrow -> void { m_operations.clear(); }

static pure fn is_same_metadata_request(
    const batch_internal::batched_syscall &left,
    const batch_internal::batched_syscall &right) wontthrow -> bool
{
  if (left.syscall_id != right.syscall_id) return false;
  switch (left.syscall_id) {
  case batch_operation::Kind::Lstat:
  case batch_operation::Kind::Stat:
  case batch_operation::Kind::Exists: break;
  case batch_operation::Kind::Read:
  case batch_operation::Kind::Write:
  case batch_operation::Kind::WriteCurrent:
  case batch_operation::Kind::Invalid: return false;
  }
  if (left.path == nullptr || right.path == nullptr) return false;

  return left.path->text().view() == right.path->text().view();
}

static pure fn is_metadata_request(
    const batch_internal::batched_syscall &operation) wontthrow -> bool
{
  if (operation.path == nullptr) return false;

  switch (operation.syscall_id) {
  case batch_operation::Kind::Lstat:
  case batch_operation::Kind::Stat:
  case batch_operation::Kind::Exists: return true;
  case batch_operation::Kind::Read:
  case batch_operation::Kind::Write:
  case batch_operation::Kind::WriteCurrent:
  case batch_operation::Kind::Invalid: return false;
  }

  return false;
}

static fn find_canonical_operation_positions(
    const ArrayList<batch_internal::batched_syscall> &operations,
    ArrayList<usize> &canonical_positions) throws -> bool
{
  constexpr usize LINEAR_METADATA_LIMIT = 8;
  usize metadata_count = 0;
  for (let const &operation : operations)
    if (is_metadata_request(operation)) metadata_count++;

  if (metadata_count < 2 ||
      metadata_count > ArrayList<usize>::MAXIMUM_ELEMENT_COUNT / 4)
  {
    return false;
  }

  if (metadata_count <= LINEAR_METADATA_LIMIT) {
    for (usize left_index = 0; left_index < operations.count(); left_index++) {
      if (!is_metadata_request(operations[left_index])) continue;

      for (usize right_index = left_index + 1;
           right_index < operations.count(); right_index++) {
        if (is_same_metadata_request(operations[left_index],
                                     operations[right_index]))
          goto has_duplicate_metadata;
      }
    }

    return false;
  }

has_duplicate_metadata:

  canonical_positions.clear();
  canonical_positions.reserve(operations.count());
  for (usize index = 0; index < operations.count(); index++)
    canonical_positions.push(index);

  usize bucket_count = 4;
  while (bucket_count < metadata_count * 2)
    bucket_count *= 2;

  let buckets = ArrayList<usize>{operations.allocator()};
  buckets.reserve(bucket_count);
  for (usize index = 0; index < bucket_count; index++)
    buckets.push(SIZE_MAX);

  bool has_repeated_request = false;
  for (usize index = 0; index < operations.count(); index++) {
    let const &operation = operations[index];
    if (!is_metadata_request(operation)) continue;

    let const path = operation.path->text().view();
    let const kind_hash =
        static_cast<u64>(operation.syscall_id) * 0x9e3779b97f4a7c15ull;
    usize bucket =
        static_cast<usize>(hash_bytes(path) ^ kind_hash) & (bucket_count - 1);
    loop
    {
      let const existing_position = buckets[bucket];
      if (existing_position == SIZE_MAX) {
        buckets[bucket] = index;
        break;
      }
      if (is_same_metadata_request(operations[existing_position], operation)) {
        canonical_positions[index] = existing_position;
        has_repeated_request = true;
        break;
      }

      bucket = (bucket + 1) & (bucket_count - 1);
    }
  }

  return has_repeated_request;
}

fn Batch::execute(ArrayList<batch_result> &results) const throws -> void
{
  let canonical_positions = ArrayList<usize>{m_operations.allocator()};
  if (!find_canonical_operation_positions(m_operations, canonical_positions)) {
    results.clear();
    results.reserve(m_operations.count());
    for (usize index = 0; index < m_operations.count(); index++)
      results.push({});

    batch_internal::execute_batch_operations(
        m_operations.begin(), m_operations.count(), results.begin());
    return;
  }

  let optimized_operations =
      ArrayList<batch_internal::batched_syscall>{m_operations.allocator()};
  let optimized_positions = ArrayList<usize>{m_operations.allocator()};
  optimized_operations.reserve(m_operations.count());
  optimized_positions.reserve(m_operations.count());
  for (usize index = 0; index < m_operations.count(); index++) {
    let const canonical_position = canonical_positions[index];
    if (canonical_position != index) {
      optimized_positions.push(optimized_positions[canonical_position]);
      continue;
    }

    optimized_positions.push(optimized_operations.count());
    optimized_operations.push(m_operations[index]);
  }

  let optimized_results = ArrayList<batch_result>{m_operations.allocator()};
  optimized_results.reserve(optimized_operations.count());
  for (usize index = 0; index < optimized_operations.count(); index++)
    optimized_results.push({});

  batch_internal::execute_batch_operations(optimized_operations.begin(),
                                           optimized_operations.count(),
                                           optimized_results.begin());

  results.clear();
  results.reserve(m_operations.count());
  for (usize index = 0; index < m_operations.count(); index++) {
    let const optimized_position = optimized_positions[index];
    let result = optimized_results[optimized_position];
    result.request_id = index;
    results.push(result);

    let const &operation = m_operations[index];
    let const &optimized_operation = optimized_operations[optimized_position];
    if (result.error_number == 0 && operation.status != nullptr &&
        optimized_operation.status != nullptr &&
        operation.status != optimized_operation.status)
    {
      *operation.status = *optimized_operation.status;
    }
  }
}

fn Batch::execute() const throws -> ArrayList<batch_result>
{
  let results = ArrayList<batch_result>{m_operations.allocator()};
  execute(results);
  return results;
}

pure fn Batch::count() const wontthrow -> usize { return m_operations.count(); }

} /* namespace koshka::os */
