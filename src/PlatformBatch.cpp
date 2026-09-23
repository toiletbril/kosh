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
  operation.m_secondary.fd = fd;
  operation.m_primary.output_buffer = buffer;
  operation.byte_count = byte_count;
  operation.byte_offset = byte_offset;
  return operation;
}

fn batch_operation::write(descriptor fd, const char *buffer, usize byte_count,
                          u64 byte_offset) wontthrow -> batch_operation
{
  batch_operation operation;
  operation.syscall_id = Kind::Write;
  operation.m_secondary.fd = fd;
  operation.m_primary.input_buffer = buffer;
  operation.byte_count = byte_count;
  operation.byte_offset = byte_offset;
  return operation;
}

fn batch_operation::write_current(descriptor fd, const char *buffer,
                                  usize byte_count) wontthrow -> batch_operation
{
  batch_operation operation;
  operation.syscall_id = Kind::WriteCurrent;
  operation.m_secondary.fd = fd;
  operation.m_primary.input_buffer = buffer;
  operation.byte_count = byte_count;
  return operation;
}

fn batch_operation::lstat(const Path &path, file_status &status) wontthrow
    -> batch_operation
{
  batch_operation operation;
  operation.syscall_id = Kind::Lstat;
  operation.m_primary.path = &path;
  operation.m_secondary.status = &status;
  return operation;
}

fn batch_operation::stat(const Path &path, file_status &status) wontthrow
    -> batch_operation
{
  batch_operation operation;
  operation.syscall_id = Kind::Stat;
  operation.m_primary.path = &path;
  operation.m_secondary.status = &status;
  return operation;
}

fn batch_operation::exists(const Path &path) wontthrow -> batch_operation
{
  batch_operation operation;
  operation.syscall_id = Kind::Exists;
  operation.m_primary.path = &path;
  return operation;
}

Batch::Batch(Allocator allocator)
    : m_operations(allocator), m_canonical_positions(allocator),
      m_buckets(allocator), m_optimized_operations(allocator)
{
}

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
  let const left_kind = batch_internal::batch_operation_access::get_kind(left);
  if (left_kind != batch_internal::batch_operation_access::get_kind(right))
    return false;
  switch (left_kind) {
  case batch_operation::Kind::Lstat:
  case batch_operation::Kind::Stat:
  case batch_operation::Kind::Exists: break;
  case batch_operation::Kind::Read:
  case batch_operation::Kind::Write:
  case batch_operation::Kind::WriteCurrent:
  case batch_operation::Kind::Invalid: return false;
  }
  let const *left_path = batch_internal::batch_operation_access::get_path(left);
  let const *right_path =
      batch_internal::batch_operation_access::get_path(right);
  if (left_path == nullptr || right_path == nullptr) return false;

  return left_path->text().view() == right_path->text().view();
}

static pure fn is_metadata_request(
    const batch_internal::batched_syscall &operation) wontthrow -> bool
{
  if (batch_internal::batch_operation_access::get_path(operation) == nullptr)
    return false;

  switch (batch_internal::batch_operation_access::get_kind(operation)) {
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
    ArrayList<usize> &canonical_positions, ArrayList<usize> &buckets) throws
    -> usize
{
  constexpr usize LINEAR_METADATA_LIMIT = 8;
  usize metadata_count = 0;
  for (let const &operation : operations)
    if (is_metadata_request(operation)) metadata_count++;

  if (metadata_count < 2 ||
      metadata_count > ArrayList<usize>::MAXIMUM_ELEMENT_COUNT / 4)
  {
    return 0;
  }

  if (metadata_count <= LINEAR_METADATA_LIMIT) {
    bool has_duplicate_metadata = false;
    for (usize left_index = 0; left_index < operations.count(); left_index++) {
      if (!is_metadata_request(operations[left_index])) continue;

      for (usize right_index = left_index + 1; right_index < operations.count();
           right_index++)
      {
        if (is_same_metadata_request(operations[left_index],
                                     operations[right_index]))
        {
          has_duplicate_metadata = true;
          break;
        }
      }
      if (has_duplicate_metadata) break;
    }

    if (!has_duplicate_metadata) return 0;
  }

  canonical_positions.clear();
  canonical_positions.reserve(operations.count());
  for (usize index = 0; index < operations.count(); index++)
    canonical_positions.push(index);

  usize bucket_count = 4;
  while (bucket_count < metadata_count * 2)
    bucket_count *= 2;

  buckets.clear();
  buckets.reserve(bucket_count);
  for (usize index = 0; index < bucket_count; index++)
    buckets.push(SIZE_MAX);

  bool has_repeated_request = false;
  usize unique_operation_count = operations.count() - metadata_count;
  for (usize index = 0; index < operations.count(); index++) {
    let const &operation = operations[index];
    if (!is_metadata_request(operation)) continue;

    let const path = batch_internal::batch_operation_access::get_path(operation)
                         ->text()
                         .view();
    let const kind_hash =
        static_cast<u64>(
            batch_internal::batch_operation_access::get_kind(operation)) *
        0x9e3779b97f4a7c15ull;
    usize bucket =
        static_cast<usize>(hash_bytes(path) ^ kind_hash) & (bucket_count - 1);
    loop
    {
      let const existing_position = buckets[bucket];
      if (existing_position == SIZE_MAX) {
        buckets[bucket] = index;
        unique_operation_count++;
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

  return has_repeated_request ? unique_operation_count : 0;
}

fn Batch::execute(ArrayList<batch_result> &results) throws -> void
{
  m_canonical_positions.clear();
  m_buckets.clear();
  m_optimized_operations.clear();
  defer { m_optimized_operations.clear(); };
  let const unique_operation_count = find_canonical_operation_positions(
      m_operations, m_canonical_positions, m_buckets);
  if (unique_operation_count == 0) {
    results.clear();
    results.reserve(m_operations.count());
    for (usize index = 0; index < m_operations.count(); index++)
      results.push({});

    batch_internal::execute_batch_operations(
        m_operations.begin(), m_operations.count(), results.begin());
    return;
  }

  m_optimized_operations.reserve(unique_operation_count);
  for (usize index = 0; index < m_operations.count(); index++) {
    let const canonical_position = m_canonical_positions[index];
    if (canonical_position != index) {
      m_canonical_positions[index] =
          m_canonical_positions[canonical_position];
      continue;
    }

    m_canonical_positions[index] = m_optimized_operations.count();
    m_optimized_operations.push(m_operations[index]);
  }

  results.clear();
  results.reserve(m_operations.count());
  for (usize index = 0; index < m_optimized_operations.count(); index++)
    results.push({});

  batch_internal::execute_batch_operations(m_optimized_operations.begin(),
                                           m_optimized_operations.count(),
                                           results.begin());

  while (results.count() < m_operations.count())
    results.push({});
  for (usize remaining_count = m_operations.count(); remaining_count != 0;
       remaining_count--)
  {
    let const index = remaining_count - 1;
    let const optimized_position = m_canonical_positions[index];
    let result = results[optimized_position];
    result.request_id = index;
    results[index] = result;

    let const &operation = m_operations[index];
    let const &optimized_operation =
        m_optimized_operations[optimized_position];
    let *status = batch_internal::batch_operation_access::get_status(operation);
    let *optimized_status =
        batch_internal::batch_operation_access::get_status(optimized_operation);
    if (result.error_number == 0 && status != nullptr &&
        optimized_status != nullptr && status != optimized_status)
    {
      *status = *optimized_status;
    }
  }
}

fn Batch::execute() throws -> ArrayList<batch_result>
{
  let results = ArrayList<batch_result>{m_operations.allocator()};
  execute(results);
  return results;
}

pure fn Batch::count() const wontthrow -> usize { return m_operations.count(); }

} /* namespace koshka::os */
