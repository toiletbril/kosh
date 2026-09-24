/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements runtime registration and dispatch for bundled koshkit
 * utilities. It also provides shared argument parsing, input opening, help,
 * error reporting, signal formatting, size formatting, and duration parsing.
 * The central unit owns the utility registry and common dispatch. Each source
 * under koshkit implements one command.
 */

#include "Koshkit.hpp"

#include "Builtin.hpp"
#include "CLI.hpp"
#include "CLIColors.hpp"
#include "Errors.hpp"
#include "Eval.hpp"
#include "ProgramResolver.hpp"
#include "base/Trace.hpp"
#include "Utils.hpp"

namespace koshka {

namespace koshkit {

Utility::Utility() = default;

flatten fn find_util(StringView name) throws -> Maybe<Utility::Kind>
{
  return KOSHKIT_UTILS.find(name);
}

static thread_local cli_color_mode KOSHKIT_COLOR_MODE = cli_color_mode::Auto;

fn set_koshkit_color_mode(cli_color_mode mode) wontthrow -> void
{
  KOSHKIT_COLOR_MODE = mode;
}

fn get_koshkit_color_mode() wontthrow -> cli_color_mode
{
  return KOSHKIT_COLOR_MODE;
}

fn koshkit_should_color() throws -> bool
{
  return stdout_wants_color(KOSHKIT_COLOR_MODE);
}

/* Zero-initialized so it is immune to static-init order, filled by each
   utility's registrar. */
static const FlagList *KOSHKIT_UTIL_FLAG_LISTS[KOSHKIT_UTIL_COUNT] = {};

fn register_koshkit_util_flags(Utility::Kind chosen,
                               const FlagList *flags) wontthrow -> void
{
  KOSHKIT_UTIL_FLAG_LISTS[static_cast<usize>(chosen)] = flags;
}

fn koshkit_util_flag_list(Utility::Kind chosen) wontthrow -> const FlagList *
{
  return KOSHKIT_UTIL_FLAG_LISTS[static_cast<usize>(chosen)];
}

fn util_names() throws -> const ArrayList<String> &
{
  static ArrayList<String> names = [] throws {
    let collected = ArrayList<String>{heap_allocator()};
    for (let const &entry : KOSHKIT_ENTRIES)
      collected.push(entry.key.to_string());
    return collected;
  }();
  return names;
}

fn resolve_util_program(EvalContext &cxt, StringView name) throws -> Maybe<Path>
{
  let const matches = cxt.get_program_resolver().search(
      name, ProgramResolver::SearchMode::First,
      ProgramResolver::Requirement::Runnable,
      ProgramResolver::CachePolicy::Bypass);
  if (matches.is_empty()) return None;

  return matches[0];
}

fn capture_util_program_output(const Path &program, ArrayList<String> arguments,
                               u64 timeout_nanoseconds) throws -> Maybe<String>
{
  let command = ArrayList<String>{heap_allocator()};
  command.reserve(arguments.count() + 1);
  command.push(program.text().clone());
  for (let &argument : arguments)
    command.push(steal(argument));

  return os::capture_program_output(command, timeout_nanoseconds);
}

fn capture_util_program_output(EvalContext &cxt, StringView name,
                               ArrayList<String> arguments,
                               u64 timeout_nanoseconds) throws -> Maybe<String>
{
  let const program = resolve_util_program(cxt, name);
  if (!program.has_value()) return None;

  return capture_util_program_output(*program, steal(arguments),
                                     timeout_nanoseconds);
}

fn print_environment(const ExecContext &ec, EvalContext &cxt) throws -> void
{
  unused(cxt.materialize_kosh_identity());
  let output = String{cxt.scratch_allocator()};
  for (let const &name : os::environment_names()) {
    let const value = os::get_environment_variable(name.view());
    output += name.view();
    output += '=';
    if (value.has_value()) output += value->view();
    output += '\n';
  }
  ec.print_to_stdout(output);
}

fn run_util(Utility::Kind chosen, const ExecContext &ec, EvalContext &cxt,
            const ArrayList<String> &args,
            const ArrayList<SourceLocation> &arg_locations) throws -> i32
{
  LOG(Debug, "dispatching koshkit utility %d with %zu arguments", ENUM(chosen),
      args.count());
  switch (chosen) {
    UTILITY_SWITCH_CASES();
  }
  unreachable("unhandled koshkit utility of kind %d", ENUM(chosen));
}

[[noreturn]] fn rethrow_with_prefix(const ErrorWithLocation &error,
                                    StringView prefix) throws -> void
{
  let const message = prefix + ": " + error.message();
  if (!error.detail_message().is_empty()) {
    let rewrapped = ErrorWithLocationAndDetails{
        error.location(), message.view(), error.detail_message()};
    if (error.is_script_fatal()) rewrapped.set_script_fatal();
    rewrapped.set_command_status(error.command_status());
    throw rewrapped;
  }

  let rewrapped = ErrorWithLocation{error.location(), message.view()};
  if (error.is_script_fatal()) rewrapped.set_script_fatal();
  rewrapped.set_command_status(error.command_status());
  throw rewrapped;
}

fn render_with_prefix(const ErrorWithLocation &error, StringView prefix,
                      StringView source, EvalContext &context) throws -> String
{
  let const message = prefix + ": " + error.message();
  if (!error.detail_message().is_empty())
    return ErrorWithLocationAndDetails{error.location(), message.view(),
                                       error.detail_message()}
        .to_string(source, &context);

  return ErrorWithLocation{error.location(), message.view()}.to_string(
      source, &context);
}

fn dispatch(const ExecContext &ec, EvalContext &cxt, usize name_index,
            Maybe<Utility::Kind> chosen) throws -> i32
{
  ASSERT(name_index < ec.args().count());
  let const name = ec.args()[name_index].view();
  if (!chosen.has_value()) chosen = find_util(name);
  if (!chosen.has_value())
    throw ErrorWithLocation{ec.arg_location_at(name_index),
                            "koshkit has no utility named '" + String{name} +
                                "'"};

  ArrayList<String> shifted{cxt.scratch_allocator()};
  shifted.reserve(ec.args().count() - name_index);
  let shifted_locations = ArrayList<SourceLocation>{cxt.scratch_allocator()};
  shifted_locations.reserve(ec.args().count() - name_index);
  for (usize i = name_index; i < ec.args().count(); i++) {
    shifted.push(String{cxt.scratch_allocator(), ec.args()[i].view()});
    shifted_locations.push(ec.arg_location_at(i));
  }

  try {
    return run_util(*chosen, ec, cxt, shifted, shifted_locations);
  } catch (const BrokenPipeExit &) {
    throw;
  } catch (const ErrorWithLocation &e) {
    let const invocation_name = ec.is_multicall ? String{heap_allocator(), name}
                                                : String{"koshkit "} + name;
    rethrow_with_prefix(e, invocation_name);
  } catch (const Error &error) {
    relocate_error(error, ec.source_location());
  }
}

fn run_as_multicall(StringView util_name, ArrayList<String> operands,
                    EvalContext &cxt) throws -> i32
{
  let const chosen = find_util(util_name);
  ASSERT(chosen.has_value());

  /* The scan stops at --, where a later --version is an operand. */
  for (let const &operand : operands) {
    if (operand == "--") break;
    if (operand == "--version") {
      show_version();
      return 0;
    }
  }

  ArrayList<String> args{heap_allocator()};
  args.reserve(operands.count() + 1);
  let arg_locations = ArrayList<SourceLocation>{heap_allocator()};
  arg_locations.reserve(operands.count() + 1);
  usize source_length = util_name.length;
  args.push(String{util_name});
  arg_locations.push(SourceLocation{0, util_name.length});
  for (String &operand : operands) {
    source_length++;
    arg_locations.push(SourceLocation{source_length, operand.length()});
    source_length += operand.length();
    args.push(steal(operand));
  }

  let ec = ExecContext::from_resolved(
      SourceLocation{0, source_length},
      ResolvedCommand::from_builtin(Builtin::Kind::Koshkit), steal(args),
      steal(arg_locations));
  ec.is_multicall = true;

  try {
    return run_util(*chosen, ec, cxt, ec.args(), ec.arg_locations());
  } catch (const BrokenPipeExit &) {
    return 141;
  } catch (const ErrorWithLocation &e) {
    show_message(render_with_prefix(
        e, util_name, utils::merge_args_to_string(ec.args()), cxt));
    return 1;
  } catch (const Error &e) {
    show_message(e.to_string());
    return 1;
  } catch (const std::exception &e) {
    show_message(String{util_name} + ": " + e.what());
    return 1;
  } catch (...) {
    show_message(String{util_name} + ": unexpected error");
    return 1;
  }
}

fn print_util_help(const ExecContext &ec, StringView name, StringView synopsis,
                   StringView description, const FlagList &flags) throws -> void
{
  let help_text = String{heap_allocator()};

  if (ec.is_multicall) {
    help_text += "\n";
    help_text += wrap_text("This utility is bundled with the Koshka shell and "
                           "runs from the kosh binary reached through a "
                           "symlink, not a system program of the same name.",
                           HELP_INDENT, HELP_WRAP_WIDTH);
    help_text += "\n\n";
  }

  if (!description.is_empty()) {
    help_text += "DESCRIPTION\n";
    help_text += wrap_text(description, HELP_INDENT, HELP_WRAP_WIDTH);
    help_text += "\n\n";
  }
  SynopsisList synopsis_lines{synopsis};
  help_text += make_synopsis(name, synopsis_lines);
  help_text += '\n';
  let const should_color = colors::stdout_wants_color();
  help_text += make_flag_help(flags, should_color);
  help_text += '\n';

  ec.print_to_stdout(format_cli_help(help_text.view(), should_color));
}

fn read_fd_to_string(os::descriptor fd) throws -> Maybe<String>
{
  return os::read_fd_to_string(fd, heap_allocator());
}

fn copy_file_contents(StringView source, StringView destination,
                      bool should_force) throws -> copy_file_result
{
  let const source_descriptor =
      os::open_file_descriptor(source, os::file_open_mode::Read);
  if (!source_descriptor.has_value()) return copy_file_result::SourceOpenFailed;
  defer { unused(os::close_fd(*source_descriptor)); };

  let destination_descriptor =
      os::open_file_descriptor(destination, os::file_open_mode::Truncate);
  if (!destination_descriptor.has_value() && should_force &&
      os::remove_file(destination))
  {
    destination_descriptor =
        os::open_file_descriptor(destination, os::file_open_mode::Truncate);
  }
  if (!destination_descriptor.has_value())
    return copy_file_result::DestinationOpenFailed;
  defer { unused(os::close_fd(*destination_descriptor)); };

  char buffer[64 * 1024];
  loop
  {
    let const read_count =
        os::read_fd(*source_descriptor, buffer, sizeof(buffer));
    if (!read_count.has_value()) return copy_file_result::ReadFailed;
    if (*read_count == 0) return copy_file_result::Success;

    usize written_count = 0;
    while (written_count < *read_count) {
      let const chunk =
          os::write_fd(*destination_descriptor, buffer + written_count,
                       *read_count - written_count);
      if (!chunk.has_value() || *chunk == 0)
        return copy_file_result::WriteFailed;
      written_count += *chunk;
    }
  }
}

fn make_directories(const Path &directory, u32 mode) wontthrow -> bool
{
  let const text = directory.view();
  let const root_length = os::path_root_length(text);
  for (usize position = root_length; position <= text.length; position++) {
    if (position < text.length && !os::is_directory_separator(text[position]))
      continue;

    let const prefix = text.substring_of_length(0, position);
    if (prefix.is_empty() || Path{prefix}.is_directory()) continue;
    if (!os::make_directory(prefix, mode)) return false;
  }

  return true;
}

fn confirm_koshkit_action(const ExecContext &ec, StringView prompt) throws
    -> bool
{
  ec.print_to_stderr(prompt);

  char first_byte = '\0';
  bool is_first_byte = true;
  loop
  {
    char byte = '\0';
    let const read_count = os::read_fd(ec.in_fd.value_or(KOSH_STDIN), &byte, 1);
    if (!read_count.has_value() || *read_count == 0) break;
    if (is_first_byte) {
      first_byte = byte;
      is_first_byte = false;
    }
    if (byte == '\n') break;
  }

  return first_byte == 'y' || first_byte == 'Y';
}

fn read_named_or_stdin(const ExecContext &ec, StringView path) throws
    -> Maybe<String>
{
  if (path == "-") return read_fd_to_string(ec.in_fd.value_or(KOSH_STDIN));

  let const fd = os::open_file_descriptor(path, os::file_open_mode::Read);
  if (!fd.has_value()) return None;
  defer { os::close_fd(*fd); };
  return read_fd_to_string(*fd);
}

SourceBatchReader::SourceBatchReader(const ExecContext &ec,
                                     const ArrayList<StringView> &sources,
                                     Allocator allocator, usize read_byte_count,
                                     bool should_treat_dash_as_stdin,
                                     bool sources_are_known_regular) throws
    : m_ec(ec),
      m_sources(sources),
      m_readers(allocator),
      m_batch(allocator),
      m_results(allocator),
      m_reader_positions(allocator),
      m_metadata_paths(allocator),
      m_metadata_statuses(allocator),
      m_read_byte_count(read_byte_count),
      m_should_treat_dash_as_stdin(should_treat_dash_as_stdin),
      m_sources_are_known_regular(sources_are_known_regular)
{
  constexpr usize READER_COUNT = 16;
  m_readers.reserve(READER_COUNT);
  m_batch.reserve(READER_COUNT);
  m_results.reserve(READER_COUNT);
  m_reader_positions.reserve(READER_COUNT);
  m_metadata_paths.reserve(READER_COUNT);
  m_metadata_statuses.reserve(READER_COUNT);
}

SourceBatchReader::~SourceBatchReader()
{
  for (let &reader : m_readers)
    close_reader(reader);
  if (m_sequential_reader.has_value()) close_reader(*m_sequential_reader);
}

fn SourceBatchReader::close_reader(Reader &reader) wontthrow -> void
{
  if (reader.should_close) os::close_fd(reader.descriptor);
  reader.should_close = false;
  reader.is_complete = true;
}

fn SourceBatchReader::finish_source(usize source_index) wontthrow -> void
{
  for (let &reader : m_readers) {
    if (reader.source_index != source_index) continue;

    close_reader(reader);
    reader.pending_byte_count = 0;
    reader.pending_error_number = 0;
    reader.has_pending_chunk = false;
    return;
  }

  if (m_sequential_reader.has_value() &&
      m_sequential_reader->source_index == source_index)
  {
    close_reader(*m_sequential_reader);
    m_sequential_reader->pending_byte_count = 0;
    m_sequential_reader->pending_error_number = 0;
    m_sequential_reader->has_pending_chunk = false;
  }
}

fn SourceBatchReader::set_source_read_byte_count(usize source_index,
                                                 usize byte_count) wontthrow
    -> void
{
  let const bounded_count =
      byte_count < m_read_byte_count ? byte_count : m_read_byte_count;
  for (let &reader : m_readers) {
    if (reader.source_index != source_index) continue;

    reader.read_byte_count = bounded_count;
    return;
  }

  if (m_sequential_reader.has_value() &&
      m_sequential_reader->source_index == source_index)
  {
    m_sequential_reader->read_byte_count = bounded_count;
  }
}

fn SourceBatchReader::retire_completed_readers() throws -> void
{
  for (usize remaining_count = m_readers.count(); remaining_count > 0;
       remaining_count--)
  {
    let const reader_index = remaining_count - 1;
    let const &reader = m_readers[reader_index];
    if (reader.is_complete && !reader.has_pending_chunk)
      m_readers.remove(reader_index);
  }

  if (m_sequential_reader.has_value() && m_sequential_reader->is_complete &&
      !m_sequential_reader->has_pending_chunk)
  {
    m_sequential_reader.reset();
  }
}

fn SourceBatchReader::fill_readers() throws -> void
{
  constexpr usize READER_COUNT = 16;

  if (m_readers.is_empty()) should_defer_source = false;

  bool has_metadata_window = false;
  usize metadata_index = 0;
  while (!m_sequential_reader.has_value() && m_readers.count() < READER_COUNT &&
         m_source_index < m_sources.count())
  {
    let const source_index = m_source_index;
    let const source = m_sources[source_index];
    if (m_should_treat_dash_as_stdin && source == "-") {
      Reader reader;
      reader.buffer.reserve(m_read_byte_count);
      reader.source_index = source_index;
      reader.read_byte_count = m_read_byte_count;
      reader.descriptor = m_ec.in_fd.value_or(KOSH_STDIN);
      m_sequential_reader = steal(reader);
      m_source_index++;
      break;
    }

    let const should_probe_nonblocking =
        !m_readers.is_empty() && !m_sources_are_known_regular;
    if (should_probe_nonblocking) {
      if (should_defer_source) break;

      if (!has_metadata_window) {
        m_batch.clear();
        m_metadata_paths.clear();
        m_metadata_statuses.clear();
        let const candidate_count = READER_COUNT - m_readers.count();
        for (usize candidate_index = m_source_index;
             candidate_index < m_sources.count() &&
             m_metadata_paths.count() < candidate_count;
             candidate_index++)
        {
          let const candidate = m_sources[candidate_index];
          if (m_should_treat_dash_as_stdin && candidate == "-") break;

          m_metadata_paths.push(Path{candidate});
          m_metadata_statuses.push({});
        }
        for (usize index = 0; index < m_metadata_paths.count(); index++) {
          m_batch.add(os::batch_operation::stat(m_metadata_paths[index],
                                                m_metadata_statuses[index]));
        }
        m_batch.execute(m_results);
        has_metadata_window = true;
      }

      if (metadata_index >= m_results.count()) break;

      if (m_results[metadata_index].error_number != 0 ||
          os::file_type_letter(m_metadata_statuses[metadata_index].mode) != '-')
      {
        should_defer_source = true;
        break;
      }
      metadata_index++;
    }

    let descriptor = os::open_file_descriptor(
        source, should_probe_nonblocking ? os::file_open_mode::ReadNonblocking
                                         : os::file_open_mode::Read);
    if (!descriptor.has_value()) {
      if (os::last_system_error_is_descriptor_quota() && !m_readers.is_empty())
      {
        should_defer_source = true;
        break;
      }

      Reader reader;
      reader.source_index = source_index;
      reader.pending_error_number = os::get_last_system_error_number();
      reader.is_complete = true;
      reader.has_pending_chunk = true;
      reader.was_open_error = true;
      m_readers.push(steal(reader));
      m_source_index++;
      break;
    }

    let const is_seekable =
        m_sources_are_known_regular || os::descriptor_is_seekable(*descriptor);
    if (should_probe_nonblocking && !is_seekable) {
      os::close_fd(*descriptor);
      should_defer_source = true;
      break;
    }

    Reader reader;
    reader.buffer.reserve(m_read_byte_count);
    reader.source_index = source_index;
    reader.read_byte_count = m_read_byte_count;
    reader.descriptor = *descriptor;
    reader.should_close = true;
    m_source_index++;

    if (!is_seekable) {
      m_sequential_reader = steal(reader);
      break;
    }

    m_readers.push(steal(reader));
  }
}

fn SourceBatchReader::read_seekable() throws -> ReadResult
{
  m_batch.clear();
  m_reader_positions.clear();
  for (usize reader_index = 0; reader_index < m_readers.count(); reader_index++)
  {
    let &reader = m_readers[reader_index];
    if (reader.has_pending_chunk || reader.is_complete) continue;
    if (reader.read_byte_count == 0) {
      close_reader(reader);
      reader.has_pending_chunk = true;
      continue;
    }

    m_batch.add(
        os::batch_operation::read(reader.descriptor, reader.buffer.begin(),
                                  reader.read_byte_count, reader.byte_offset));
    m_reader_positions.push(reader_index);
  }
  if (m_batch.count() == 0) return ReadResult::Chunks;

  m_batch.execute(m_results);
  if (os::INTERRUPT_REQUESTED) return ReadResult::Interrupted;

  for (usize result_index = 0; result_index < m_results.count(); result_index++)
  {
    let const reader_index = m_reader_positions[result_index];
    let &reader = m_readers[reader_index];
    let const &result = m_results[result_index];
    if (result.error_number != 0) {
      close_reader(reader);
      reader.pending_error_number = result.error_number;
      reader.has_pending_chunk = true;
      continue;
    }
    if (result.transferred_byte_count == 0) {
      close_reader(reader);
      reader.has_pending_chunk = true;
      continue;
    }

    reader.pending_byte_count = result.transferred_byte_count;
    reader.has_pending_chunk = true;
    reader.byte_offset += result.transferred_byte_count;
    /* A short positioned read from a regular file is the EOF boundary. Avoid
       submitting a second zero-byte read for the common small-file case. */
    if (result.transferred_byte_count < reader.read_byte_count)
      close_reader(reader);
  }

  return ReadResult::Chunks;
}

fn SourceBatchReader::read_sequential() throws -> ReadResult
{
  let &reader = *m_sequential_reader;
  if (reader.has_pending_chunk || reader.is_complete) return ReadResult::Chunks;
  if (reader.read_byte_count == 0) {
    close_reader(reader);
    reader.has_pending_chunk = true;
    return ReadResult::Chunks;
  }

  let const read_count = os::read_fd(reader.descriptor, reader.buffer.begin(),
                                     reader.read_byte_count);
  if (!read_count.has_value()) {
    if (os::INTERRUPT_REQUESTED) return ReadResult::Interrupted;

    let const error_number = os::get_last_system_error_number();
    close_reader(reader);
    reader.pending_error_number = error_number;
    reader.has_pending_chunk = true;
    return ReadResult::Chunks;
  }
  if (*read_count == 0) {
    close_reader(reader);
    reader.has_pending_chunk = true;
    return ReadResult::Chunks;
  }

  reader.pending_byte_count = *read_count;
  reader.has_pending_chunk = true;
  return ReadResult::Chunks;
}

fn SourceBatchReader::append_pending_chunks(ArrayList<Chunk> &chunks,
                                            bool should_emit_one) throws -> void
{
  let const emit_capacity =
      should_emit_one ? usize{1}
                      : m_readers.count() +
                            static_cast<usize>(m_readers.is_empty() &&
                                               m_sequential_reader.has_value());
  chunks.reserve(emit_capacity);

  let const do_append = [&](Reader &reader) throws -> bool {
    if (!reader.has_pending_chunk) return false;

    chunks.push({
        StringView{reader.buffer.begin(), reader.pending_byte_count},
        reader.source_index, reader.pending_error_number, reader.is_complete,
        reader.was_open_error
    });
    reader.pending_byte_count = 0;
    reader.pending_error_number = 0;
    reader.has_pending_chunk = false;
    return true;
  };

  for (let &reader : m_readers) {
    if (!do_append(reader)) continue;
    if (should_emit_one) return;
  }

  if (m_readers.is_empty() && m_sequential_reader.has_value())
    unused(do_append(*m_sequential_reader));
}

fn SourceBatchReader::read_next_internal(ArrayList<Chunk> &chunks,
                                         bool should_emit_one) throws
    -> ReadResult
{
  chunks.clear();
  retire_completed_readers();
  if (os::INTERRUPT_REQUESTED) return ReadResult::Interrupted;

  fill_readers();
  if (os::INTERRUPT_REQUESTED) return ReadResult::Interrupted;

  if (!m_readers.is_empty()) {
    let const result = read_seekable();
    if (result == ReadResult::Interrupted) return result;
  } else if (m_sequential_reader.has_value()) {
    let const result = read_sequential();
    if (result == ReadResult::Interrupted) return result;
  }

  append_pending_chunks(chunks, should_emit_one);
  if (!chunks.is_empty()) return ReadResult::Chunks;
  if (m_source_index == m_sources.count() && m_readers.is_empty() &&
      !m_sequential_reader.has_value())
  {
    return ReadResult::Complete;
  }

  return ReadResult::Chunks;
}

fn SourceBatchReader::read_next(ArrayList<Chunk> &chunks) throws -> ReadResult
{
  return read_next_internal(chunks, false);
}

fn SourceBatchReader::read_next_ordered(ArrayList<Chunk> &chunks) throws
    -> ReadResult
{
  return read_next_internal(chunks, true);
}

fn read_named_or_stdin_batch(const ExecContext &ec,
                             const ArrayList<StringView> &sources,
                             Allocator allocator) throws
    -> ArrayList<source_read_result>
{
  let results = ArrayList<source_read_result>{allocator};
  results.reserve(sources.count());
  for (usize source_index = 0; source_index < sources.count(); source_index++)
    results.push({None, 0, false});

  let reader = SourceBatchReader{ec, sources, allocator};
  let chunks = ArrayList<SourceBatchReader::Chunk>{allocator};
  loop
  {
    let const read_result = reader.read_next(chunks);
    switch (read_result) {
    case SourceBatchReader::ReadResult::Chunks: break;
    case SourceBatchReader::ReadResult::Complete:
    case SourceBatchReader::ReadResult::Interrupted: return results;
    }

    for (let const &chunk : chunks) {
      let &result = results[chunk.source_index];
      result.is_complete = chunk.is_complete;
      if (chunk.error_number != 0) {
        result.content.reset();
        result.error_number = chunk.error_number;
        continue;
      }
      if (!result.content.has_value()) result.content = String{allocator};
      result.content->append(chunk.content);
    }
  }

  return results;
}

fn open_named_or_stdin(const ExecContext &ec, StringView path) wontthrow
    -> Maybe<input_descriptor>
{
  if (path == "-")
    return input_descriptor{ec.in_fd.value_or(KOSH_STDIN), false};

  let const descriptor =
      os::open_file_descriptor(path, os::file_open_mode::Read);
  if (!descriptor.has_value()) return None;
  return input_descriptor{*descriptor, true};
}

fn source_list_from_operands(const ArrayList<String> &operands,
                             Allocator allocator,
                             usize first_operand_index) throws
    -> ArrayList<StringView>
{
  let sources = ArrayList<StringView>{allocator};
  if (first_operand_index >= operands.count()) {
    sources.push(StringView{"-"});
  } else {
    sources.reserve(operands.count() - first_operand_index);
    for (usize i = first_operand_index; i < operands.count(); i++)
      sources.push(operands[i].view());
  }
  return sources;
}

fn format_signal_list() throws -> String
{
  static const usize COLUMN_COUNT = 5;
  static const usize NUMBER_WIDTH = 2;

  let numbered = ArrayList<utils::signal_pair>{heap_allocator()};
  for (let const name : os::signal_names()) {
    if (let const number = os::signal_number_from_name(name);
        number.has_value())
    {
      numbered.push(utils::signal_pair{*number, name});
    }
  }

  numbered.sort(
      [](const utils::signal_pair &left, const utils::signal_pair &right) {
        return left.number < right.number;
      });

  let out = String{heap_allocator()};
  for (usize index = 0; index < numbered.count(); index++) {
    let const number = String::from(numbered[index].number, heap_allocator());

    for (usize pad = number.length(); pad < NUMBER_WIDTH; pad++)
      out += ' ';

    out += number;
    out += ") SIG";
    out += numbered[index].name;
    out += (index + 1) % COLUMN_COUNT == 0 ? '\n' : '\t';
  }

  if (numbered.count() % COLUMN_COUNT != 0) out += '\n';

  return out;
}

fn format_human_size(u64 bytes, Allocator allocator) throws -> String
{
  if (bytes < 1024) return String::from(bytes, allocator);

  static const char units[] = {'K', 'M', 'G', 'T', 'P'};
  double value = static_cast<double>(bytes);
  usize unit = 0;
  /* The condition reads unit, so the last unit P stays reachable. */
  while (value >= 1024.0 && unit < sizeof(units)) {
    value /= 1024.0;
    unit++;
  }

  /* A value that rounds up to 1024 crosses over to the next unit. */
  if (value >= 1023.5 && unit < sizeof(units)) {
    value /= 1024.0;
    unit++;
  }

  String out{allocator};
  /* A scaled value below ten keeps one decimal, otherwise it rounds whole. */
  let const tenths = static_cast<u64>(value * 10.0 + 0.5);
  if (value < 10.0 && tenths < 100) {
    out += String::from(tenths / 10, allocator);
    out += '.';
    out += String::from(tenths % 10, allocator);
  } else {
    out += String::from(static_cast<u64>(value + 0.5), allocator);
  }
  out.push(units[unit - 1]);
  return out;
}

fn scaled_filesystem_blocks(u64 block_count, u64 block_size,
                            u64 output_unit) wontthrow -> u64
{
  let const byte_count = static_cast<u128>(block_count) * block_size;
  let const rounded_byte_count = byte_count + output_unit - 1;
  let const high = static_cast<u64>(rounded_byte_count >> 64u);
  if (high >= output_unit) return UINT64_MAX;

  u64 remainder = 0;
  return os::divide_u128_by_u64(high, static_cast<u64>(rounded_byte_count),
                                output_unit, remainder);
}

fn filesystem_usage_percent(u64 used, u64 available) wontthrow -> u64
{
  let const capacity_base = static_cast<u128>(used) + available;
  if (capacity_base == 0) return 0;

  let const numerator = static_cast<u128>(used) * 100 + capacity_base - 1;
  u64 remainder = 0;
  return os::divide_u128_by_u64(static_cast<u64>(numerator >> 64u),
                                static_cast<u64>(numerator),
                                static_cast<u64>(capacity_base), remainder);
}

pure fn file_type_name(const os::file_status &status) wontthrow -> StringView
{
  switch (os::file_type_letter(status.mode)) {
  case 'd': return "directory";
  case 'l': return "symbolic link";
  case 'c': return "character special file";
  case 'b': return "block special file";
  case 'p': return "fifo";
  case 's': return "socket";
  default: break;
  }

  return status.size == 0 ? "regular empty file" : "regular file";
}

fn format_file_timestamp(i64 seconds, u32 nanoseconds,
                         Allocator allocator) throws -> String
{
  let text =
      String{allocator,
             utils::format_unix_timestamp(seconds, "%Y-%m-%d %H:%M:%S").view()};
  text += ".";

  let const digits = String::from(nanoseconds, allocator);
  for (usize index = digits.length(); index < 9; index++)
    text += "0";

  text += digits.view();
  text += " ";
  text += utils::format_unix_timestamp(seconds, "%z").view();
  return text;
}

fn get_init_system_name(Allocator allocator) throws -> String
{
  constexpr static_string_entry<StringView> INIT_COMMAND_ENTRIES[] = {
      {SSK("systemd"),     "systemd"    },
      {SSK("init"),        "sysvinit"   },
      {SSK("openrc-init"), "openrc"     },
      {SSK("runit"),       "runit"      },
      {SSK("s6-svscan"),   "s6"         },
      {SSK("dinit"),       "dinit"      },
      {SSK("busybox"),     "busybox"    },
      {SSK("launchd"),     "launchd"    },
      {SSK("tini"),        "tini"       },
      {SSK("docker-init"), "docker-init"},
      {SSK("dumb-init"),   "dumb-init"  },
      {SSK("bash"),        "shell"      },
      {SSK("sh"),          "shell"      },
  };
  constexpr StaticStringMap INIT_COMMAND_NAMES{INIT_COMMAND_ENTRIES};

  if (os::path_exists("/run/systemd/system")) {
    return String{allocator, "systemd"};
  }

  if (os::path_exists("/run/openrc/softlevel")) {
    return String{allocator, "openrc"};
  }

  if (os::path_exists("/run/s6/container_environment")) {
    return String{allocator, "s6"};
  }

  if (os::path_exists("/run/runit")) return String{allocator, "runit"};

  let const body = Path{"/proc/1/comm"}.read_entire_file();
  if (body.has_value()) {
    let command = body->view();
    while (!command.is_empty() && (command[command.length - 1] == '\n' ||
                                   command[command.length - 1] == '\r'))
    {
      command = command.substring_of_length(0, command.length - 1);
    }

    if (!command.is_empty()) {
      let const named = INIT_COMMAND_NAMES.find(command);
      if (named.has_value()) return String{allocator, *named};

      return String{allocator, command};
    }
  }

  let const processes = os::enumerate_processes();
  for (let const &process : processes) {
    if (process.pid != 1) continue;

    let const named = INIT_COMMAND_NAMES.find(process.name.view());
    if (named.has_value()) return String{allocator, *named};

    return String{allocator, process.name.view()};
  }

  return String{allocator, "unknown"};
}

fn parse_koshkit_duration_seconds(StringView text, SourceLocation location,
                                  Allocator allocator) throws -> f64
{
  let const do_throw_invalid = [&]() throws -> void {
    let const invalid_value = String{allocator, text};
    throw ErrorWithLocationAndDetails{
        location, "invalid duration '" + invalid_value + "'",
        "Use a non-negative number with an optional `s`, `m`, `h`, or `d` "
        "suffix, such as `5` or `0.25s`"};
  };

  f64 multiplier = 1.0;
  usize number_length = text.length;
  if (number_length != 0) {
    switch (text[number_length - 1]) {
    case 's': multiplier = 1.0; break;
    case 'm': multiplier = 60.0; break;
    case 'h': multiplier = 60.0 * 60.0; break;
    case 'd': multiplier = 60.0 * 60.0 * 24.0; break;
    default: break;
    }
    if (multiplier != 1.0 || text[number_length - 1] == 's') {
      number_length--;
    }
  }

  let const number =
      String{allocator, text.substring_of_length(0, number_length)};
  let const parsed_value = number.to<f64>();
  if (parsed_value.is_error()) do_throw_invalid();

  let const value = parsed_value.value();
  if (__builtin_isnan(value) || value < 0.0) do_throw_invalid();

  return value * multiplier;
}

cold static fn show_soft_koshkit_error(const ExecContext &ec, EvalContext &cxt,
                                       SourceLocation location,
                                       StringView message) throws -> void
{
  const ErrorWithLocation located{steal(location), message};
  if (const String *source = cxt.current_source(); source != nullptr) {
    show_message(located.to_string(source->view(), &cxt));
    return;
  }
  if (ec.is_multicall) {
    let const source = utils::merge_args_to_string(ec.args());
    show_message(located.to_string(source.view(), &cxt));
    return;
  }

  print_error(String{message} + "\n");
}

cold noinline fn report_soft_koshkit_error(const ExecContext &ec,
                                           EvalContext &cxt,
                                           StringView message) throws -> void
{
  show_soft_koshkit_error(ec, cxt, ec.source_location(), message);
}

cold noinline fn report_soft_koshkit_error(const ExecContext &ec,
                                           EvalContext &cxt, StringView message,
                                           StringView note) throws -> void
{
  report_soft_koshkit_error(ec, cxt, message);
  show_message(Note{String{note}}.to_string());
}

cold noinline fn report_soft_koshkit_util_error(const ExecContext &ec,
                                                EvalContext &cxt,
                                                StringView utility_name,
                                                StringView message) throws
    -> void
{
  report_soft_koshkit_error(ec, cxt, String{utility_name} + ": " + message);
}

cold noinline fn report_soft_koshkit_util_error(const ExecContext &ec,
                                                EvalContext &cxt,
                                                StringView utility_name,
                                                StringView message,
                                                StringView note) throws -> void
{
  report_soft_koshkit_util_error(ec, cxt, utility_name, message);
  show_message(Note{String{note}}.to_string());
}

cold noinline fn report_soft_koshkit_util_error(
    const ExecContext &ec, EvalContext &cxt, SourceLocation location,
    StringView utility_name, StringView message) throws -> void
{
  let const prefixed = String{utility_name} + ": " + message;
  show_soft_koshkit_error(ec, cxt, steal(location), prefixed.view());
}

cold noinline fn report_soft_koshkit_util_error(
    const ExecContext &ec, EvalContext &cxt, SourceLocation location,
    StringView utility_name, StringView message, StringView note) throws -> void
{
  report_soft_koshkit_util_error(ec, cxt, steal(location), utility_name,
                                 message);
  show_message(Note{String{note}}.to_string());
}

} /* namespace koshkit */

} /* namespace koshka */
