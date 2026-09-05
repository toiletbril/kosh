/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file defines source locations and the owned error, warning, note, trace,
 * and located-diagnostic hierarchy shared by parsing and evaluation. Errors.cpp
 * owns source interning and source-aware rendering.
 */

#pragma once

#include "Common.hpp"
#include "Maybe.hpp"
#include "String.hpp"
#include "StringView.hpp"

namespace koshka {

class EvalContext;

enum class error_severity : u8
{
  Error,
  Warning,
  Note,
  Details,
  Trace,
};

pure inline fn get_error_severity_word(error_severity severity) wontthrow
    -> StringView
{
  switch (severity) {
  case error_severity::Error: return "error";
  case error_severity::Warning: return "warning";
  case error_severity::Note: return "note";
  case error_severity::Details: return "details";
  case error_severity::Trace: return "trace";
  }
  unreachable("invalid error severity %d", ENUM(severity));
}

/* One row per distinct source name, so a location holds a four-byte index where
   it held a twenty-four byte view. The table owns its copies, and a row is
   never released, because the row count is the number of distinct script paths
   one run touches. Index zero is the source with no name. */
fn intern_source_name(StringView name) throws -> u32;
pure fn source_name_at(u32 source_name_index) wontthrow -> Maybe<StringView>;

/* The offsets are 32-bit because one shell source is far below four gigabytes,
   and every token and every syntax node carries one of these. The constructor
   accepts usize so the many call sites that compute an offset need no cast. */
struct SourceLocation
{
  u32 position{0};
  u32 length{0};
  u32 source_name_index{0};

  SourceLocation() = default;

  SourceLocation(usize position, usize length,
                 u32 source_name_index = 0) wontthrow
      : position{static_cast<u32>(position)},
        length{static_cast<u32>(length)},
        source_name_index{source_name_index}
  {}

  pure fn get_filename() const wontthrow -> Maybe<StringView>
  {
    return source_name_at(source_name_index);
  }

  pure fn has_same_source_as(const SourceLocation &other) const wontthrow
      -> bool
  {
    return source_name_index == other.source_name_index;
  }

  pure fn get_source_text(StringView source) const wontthrow
      -> Maybe<StringView>
  {
    if (position > source.length || length > source.length - position)
      return None;
    return source.substring_of_length(position, length);
  }

  pure fn subspan(usize relative_position,
                  usize relative_length) const wontthrow -> SourceLocation
  {
    ASSERT(relative_position <= length);
    ASSERT(relative_length <= length - relative_position);
    return SourceLocation{position + relative_position, relative_length,
                          source_name_index};
  }

  pure fn subspan_for_view(StringView source, StringView part,
                           SourceLocation &storage,
                           usize source_offset = 0) const wontthrow
      -> const SourceLocation *
  {
    ASSERT(part.data >= source.data &&
           part.data + part.length <= source.data + source.length);
    let const part_offset = static_cast<usize>(part.data - source.data);
    if (part_offset < source_offset) return nullptr;
    let const mapped_offset = part_offset - source_offset;
    ASSERT(mapped_offset <= length);
    ASSERT(part.length <= length - mapped_offset);
    storage = subspan(mapped_offset, part.length);
    return &storage;
  }
};

class ErrorBase
{
public:
  virtual ~ErrorBase();

  virtual pure fn message() const wontthrow -> const String & = 0;
  virtual pure fn detail_message() const wontthrow -> StringView { return {}; }

  virtual fn get_severity() const wontthrow -> error_severity;

  virtual fn to_string(StringView source,
                       EvalContext *context = nullptr) const throws -> String;

  /* The command status is 1 for most errors and 2 for a [[ ]] operand error. A
     relocation that rewraps an error must carry both the fatal mark and the
     status over. */
  fn set_script_fatal() wontthrow -> void { m_is_script_fatal = true; }
  pure fn is_script_fatal() const wontthrow -> bool
  {
    return m_is_script_fatal;
  }
  fn set_command_status(i64 status) wontthrow -> void
  {
    m_command_status = status;
  }
  pure fn command_status() const wontthrow -> i64 { return m_command_status; }

protected:
  fn trailing_details_to_string() const throws -> String;

  bool m_is_script_fatal{false};
  i64 m_command_status{1};
};

class Error : public ErrorBase
{
public:
  Error(StringView message);

  pure fn message() const wontthrow -> const String & override
  {
    return m_message;
  }

  fn to_string() const throws -> String;
  using ErrorBase::to_string;

protected:
  String m_message{heap_allocator()};
};

class ErrorWithDetails : public Error
{
public:
  ErrorWithDetails(StringView message, StringView note);

  pure fn detail_message() const wontthrow -> StringView override
  {
    return m_note.view();
  }

private:
  String m_note{heap_allocator()};
};

class Warning : public Error
{
public:
  Warning(StringView message);

  fn get_severity() const wontthrow -> error_severity override;
};

class WarningWithDetails : public Warning
{
public:
  WarningWithDetails(StringView message, StringView note);

  pure fn detail_message() const wontthrow -> StringView override
  {
    return m_note.view();
  }

private:
  String m_note{heap_allocator()};
};

class Note : public Error
{
public:
  Note(StringView message);

  fn get_severity() const wontthrow -> error_severity override;
};

/* Thrown by print_to_stdout and print_to_stderr when write returns EPIPE,
   since the shell ignores SIGPIPE and so a builtin only sees the EPIPE return.
   Caught at the builtin and forked-stage boundaries and turned into a silent
   exit 141, mirroring the SIGPIPE reap in wait_and_monitor_process. */
class BrokenPipeExit : public Error
{
public:
  BrokenPipeExit();
};

class ErrorWithLocation : public Error
{
public:
  ErrorWithLocation(SourceLocation location, StringView message);

  fn to_string(StringView source, EvalContext *context = nullptr) const throws
      -> String override;

  /* The line numbering starts this many lines past one, for a source that is a
     window into a larger file. */
  fn set_line_offset(usize offset) wontthrow -> void { m_line_offset = offset; }

  pure fn location() const wontthrow -> SourceLocation { return m_location; }
  fn set_location(SourceLocation location) wontthrow -> void
  {
    m_location = steal(location);
  }

  fn set_rendered() wontthrow -> void { m_was_rendered = true; }
  pure fn was_rendered() const wontthrow -> bool { return m_was_rendered; }

protected:
  SourceLocation m_location;
  usize m_line_offset{0};
  bool m_was_rendered{false};
};

/* The mimic boundary tests this type, never the message text, so a
   program-thrown Error reading "Interrupted" is not mistaken for it. */
class InterruptErrorWithLocation : public ErrorWithLocation
{
public:
  explicit InterruptErrorWithLocation(SourceLocation location);
};

class CommandResolutionErrorWithLocation : public ErrorWithLocation
{
public:
  CommandResolutionErrorWithLocation(SourceLocation location,
                                     StringView message,
                                     i64 command_status = 127);
};

class CommandResolutionErrorWithLocationAndDetails
    : public CommandResolutionErrorWithLocation
{
public:
  CommandResolutionErrorWithLocationAndDetails(SourceLocation location,
                                               StringView message,
                                               StringView note,
                                               i64 command_status = 127);

  pure fn detail_message() const wontthrow -> StringView override
  {
    return m_note.view();
  }

private:
  String m_note{heap_allocator()};
};

class WarningWithLocation : public ErrorWithLocation
{
public:
  WarningWithLocation(SourceLocation location, StringView message);

  fn get_severity() const wontthrow -> error_severity override;
};

class WarningWithLocationAndDetails : public WarningWithLocation
{
public:
  WarningWithLocationAndDetails(SourceLocation location, StringView message,
                                StringView note);

  pure fn detail_message() const wontthrow -> StringView override
  {
    return m_note.view();
  }

private:
  String m_note{heap_allocator()};
};

class TraceWithLocation : public ErrorWithLocation
{
public:
  TraceWithLocation(SourceLocation location);

  fn get_severity() const wontthrow -> error_severity override;
};

class DetailsWithLocation : public ErrorWithLocation
{
public:
  DetailsWithLocation(SourceLocation location, StringView message);

  fn get_severity() const wontthrow -> error_severity override;
  fn to_string(StringView source, EvalContext *context = nullptr) const throws
      -> String override;
};

class ErrorWithLocationAndDetails : public ErrorWithLocation
{
public:
  ErrorWithLocationAndDetails(SourceLocation location, StringView message,
                              SourceLocation details_location,
                              StringView details_message, StringView note = {});
  ErrorWithLocationAndDetails(SourceLocation location, StringView message,
                              StringView note);

  pure fn detail_message() const wontthrow -> StringView override
  {
    return m_note.view();
  }

  pure fn details_location() const wontthrow -> SourceLocation
  {
    return m_details_location;
  }

  pure fn details_message() const wontthrow -> StringView
  {
    return m_details_message.view();
  }

  fn details_to_string(StringView source,
                       EvalContext *context = nullptr) const throws -> String;

protected:
  SourceLocation m_details_location;
  String m_details_message;
  String m_note{heap_allocator()};
};

static_assert(std::is_abstract_v<ErrorBase>);
static_assert(std::is_base_of_v<Error, ErrorWithLocation>);
static_assert(!std::is_same_v<ErrorWithDetails, Error>);
static_assert(!std::is_same_v<WarningWithDetails, Warning>);
static_assert(
    !std::is_same_v<WarningWithLocationAndDetails, WarningWithLocation>);

[[noreturn]] inline fn relocate_error(const ErrorBase &error,
                                      const SourceLocation &location) throws
    -> void
{
  if (!error.detail_message().is_empty()) {
    let relocated = ErrorWithLocationAndDetails{
        location, error.message().view(), error.detail_message()};
    if (error.is_script_fatal()) relocated.set_script_fatal();
    relocated.set_command_status(error.command_status());
    throw relocated;
  }

  let relocated = ErrorWithLocation{location, error.message().view()};
  if (error.is_script_fatal()) relocated.set_script_fatal();
  relocated.set_command_status(error.command_status());
  throw relocated;
}

} /* namespace koshka */
