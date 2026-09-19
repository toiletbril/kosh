/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This header defines portable batched filesystem and descriptor operations.
 * Platform.hpp includes it after the native descriptor and file status types
 * are available. Batch owns the operation vector and returns ordered results.
 */

#pragma once

#include "Common.hpp"
#include "Platform.hpp"

namespace koshka::os {

namespace batch_internal {

struct batch_operation_access;

} /* namespace batch_internal */

struct batch_operation
{
  enum class Kind : u8
  {
    Read = 0,
    Write = 1,
    WriteCurrent = 2,
    Lstat = 3,
    Stat = 4,
    Exists = 5,
    Invalid = 127,
  };

  static fn read(descriptor fd, char *buffer, usize byte_count,
                 u64 byte_offset = 0) wontthrow -> batch_operation;
  static fn write(descriptor fd, const char *buffer, usize byte_count,
                  u64 byte_offset = 0) wontthrow -> batch_operation;
  static fn write_current(descriptor fd, const char *buffer,
                          usize byte_count) wontthrow -> batch_operation;
  static fn lstat(const Path &path, file_status &status) wontthrow
      -> batch_operation;
  static fn stat(const Path &path, file_status &status) wontthrow
      -> batch_operation;
  static fn exists(const Path &path) wontthrow -> batch_operation;
  static fn lstat(Path &&path, file_status &status) wontthrow
      -> batch_operation = delete;
  static fn lstat(const Path &&path, file_status &status) wontthrow
      -> batch_operation = delete;
  static fn stat(Path &&path, file_status &status) wontthrow
      -> batch_operation = delete;
  static fn stat(const Path &&path, file_status &status) wontthrow
      -> batch_operation = delete;
  static fn exists(Path &&path) wontthrow -> batch_operation = delete;
  static fn exists(const Path &&path) wontthrow -> batch_operation = delete;

  u64 request_id{0};
  u64 byte_offset{0};
  usize byte_count{0};

private:
  Kind syscall_id{Kind::Invalid};
  union
  {
    const Path *path;
    const char *input_buffer;
    char *output_buffer;
  } m_primary{};
  union
  {
    file_status *status;
    descriptor fd;
  } m_secondary{};

  batch_operation() = default;
  friend class Batch;
  friend struct batch_internal::batch_operation_access;
};

static_assert(sizeof(usize) != 8 || sizeof(batch_operation) == 48);

struct batch_result
{
  u64 request_id{0};
  usize transferred_byte_count{0};
  i32 error_number{0};
  bool is_existing{false};
};

static_assert(sizeof(usize) != 8 || sizeof(batch_result) == 24);

namespace batch_internal {

using batched_syscall_id = batch_operation::Kind;
using batched_syscall_result = batch_result;
using batched_syscall = batch_operation;

struct batch_operation_access
{
  static pure fn get_kind(const batch_operation &operation) wontthrow
      -> batch_operation::Kind
  {
    return operation.syscall_id;
  }

  static pure fn get_path(const batch_operation &operation) wontthrow
      -> const Path *
  {
    switch (operation.syscall_id) {
    case batch_operation::Kind::Lstat:
    case batch_operation::Kind::Stat:
    case batch_operation::Kind::Exists: return operation.m_primary.path;
    case batch_operation::Kind::Read:
    case batch_operation::Kind::Write:
    case batch_operation::Kind::WriteCurrent:
    case batch_operation::Kind::Invalid: return nullptr;
    }

    return nullptr;
  }

  static pure fn get_input_buffer(const batch_operation &operation) wontthrow
      -> const char *
  {
    switch (operation.syscall_id) {
    case batch_operation::Kind::Write:
    case batch_operation::Kind::WriteCurrent:
      return operation.m_primary.input_buffer;
    case batch_operation::Kind::Read:
    case batch_operation::Kind::Lstat:
    case batch_operation::Kind::Stat:
    case batch_operation::Kind::Exists:
    case batch_operation::Kind::Invalid: return nullptr;
    }

    return nullptr;
  }

  static pure fn get_output_buffer(const batch_operation &operation) wontthrow
      -> char *
  {
    return operation.syscall_id == batch_operation::Kind::Read
               ? operation.m_primary.output_buffer
               : nullptr;
  }

  static pure fn get_status(const batch_operation &operation) wontthrow
      -> file_status *
  {
    switch (operation.syscall_id) {
    case batch_operation::Kind::Lstat:
    case batch_operation::Kind::Stat: return operation.m_secondary.status;
    case batch_operation::Kind::Read:
    case batch_operation::Kind::Write:
    case batch_operation::Kind::WriteCurrent:
    case batch_operation::Kind::Exists:
    case batch_operation::Kind::Invalid: return nullptr;
    }

    return nullptr;
  }

  static pure fn get_descriptor(const batch_operation &operation) wontthrow
      -> descriptor
  {
    switch (operation.syscall_id) {
    case batch_operation::Kind::Read:
    case batch_operation::Kind::Write:
    case batch_operation::Kind::WriteCurrent: return operation.m_secondary.fd;
    case batch_operation::Kind::Lstat:
    case batch_operation::Kind::Stat:
    case batch_operation::Kind::Exists:
    case batch_operation::Kind::Invalid: return KOSH_INVALID_FD;
    }

    return KOSH_INVALID_FD;
  }
};

fn execute_batch_operations(const batched_syscall *operations,
                            usize operation_count,
                            batch_result *results) wontthrow -> void;

} /* namespace batch_internal */

class Batch
{
public:
  explicit Batch(Allocator allocator);

  fn reserve(usize operation_count) throws -> void;
  fn add(batch_operation operation) throws -> void;
  fn clear() wontthrow -> void;
  fn execute(ArrayList<batch_result> &results) const throws -> void;
  fn execute() const throws -> ArrayList<batch_result>;

  pure fn count() const wontthrow -> usize;

private:
  ArrayList<batch_operation> m_operations;
};

} /* namespace koshka::os */
