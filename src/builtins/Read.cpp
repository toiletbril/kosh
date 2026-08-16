#include "../Builtin.hpp"
#include "../Cli.hpp"
#include "../Eval.hpp"
#include "../Platform.hpp"
#include "../Trace.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-ers] [-a name] [-d delim] [-n count] [-p prompt] "
                   "[-t timeout] [-u fd] [-q] [name ...]");
HELP_DESCRIPTION_DECL(
    "The read builtin reads one line from standard input into the named "
    "variables.");

FLAG(READ_RAW, Bool, 'r', "\0", "Do not treat a backslash as an escape.");
FLAG(READ_ARRAY, String, 'a', "\0",
     "Split the line into the named indexed array.");
FLAG(READ_PROMPT, String, 'p', "\0",
     "Print the prompt before reading, when reading from a terminal.");
FLAG(READ_TIMEOUT, String, 't', "\0", "Time out after the given seconds.");
FLAG(READ_NCHARS, String, 'n', "\0", "Read at most the given number of bytes.");
FLAG(READ_SILENT, Bool, 's', "\0", "Do not echo the input from a terminal.");
FLAG(READ_DELIM, String, 'd', "\0",
     "Read until the first byte of the given delimiter, or until a NUL byte "
     "when the delimiter is empty.");
FLAG(READ_FD, String, 'u', "\0", "Read from the given file descriptor.");
FLAG(READ_EDIT, Bool, 'e', "\0",
     "Accepted for compatibility. The line editor is always used at a "
     "terminal.");
FLAG(READ_QUERY, Bool, 'q', "\0",
     "Read one byte and return zero for y or Y, one otherwise.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_BUILTIN_FLAGS(Read);

namespace koshka {

Read::Read() = default;

pure fn Read::kind() const wontthrow -> Builtin::Kind { return Kind::Read; }

fn Read::execute(ExecContext &ec, EvalContext &cxt) const throws -> i32
{
  let const names = PARSE_BUILTIN_ARGS(ec);

  ASSERT(!names.is_empty());

  if (FLAG_HELP.is_enabled()) SHOW_BUILTIN_HELP_AND_RETURN(ec);

  /* The array, count, query, timeout, silent, delimiter, descriptor, and
     editor options are bash extensions the sh mood rejects. */
  if (cxt.is_posix_mode() &&
      (FLAG_READ_ARRAY.is_set() || FLAG_READ_TIMEOUT.is_set() ||
       FLAG_READ_NCHARS.is_set() || FLAG_READ_QUERY.is_enabled() ||
       FLAG_READ_SILENT.is_enabled() || FLAG_READ_DELIM.is_set() ||
       FLAG_READ_FD.is_set() || FLAG_READ_EDIT.is_enabled()))
  {
    report_soft_builtin_error(ec, cxt, "Illegal option",
                              "Run `read --help` for the accepted options");
    return 2;
  }

  let read_fd = ec.in_fd.value_or(KOSH_STDIN);
  if (FLAG_READ_FD.is_set()) {
    let const parsed = FLAG_READ_FD.value().to<i64>();
    if (parsed.is_error() || parsed.value() < 0) {
      report_soft_builtin_error(ec, cxt, FLAG_READ_FD.value_location(),
                                FLAG_READ_FD.value() +
                                    ": invalid file descriptor");
      return 1;
    }
    read_fd = os::descriptor_from_fd_number(parsed.value());
  }

  i64 timeout_nanos = -1;
  if (FLAG_READ_TIMEOUT.is_set()) {
    let const parsed =
        utils::parse_timeout_seconds_to_nanos(FLAG_READ_TIMEOUT.value());
    if (parsed.is_error()) {
      let timeout_arg_index = usize{1};
      for (usize k = 1; k < ec.args().count(); k++) {
        let const &a = ec.args()[k];
        if (a == "-t" || (a.length() > 2 && a[0] == '-' && a[1] == 't')) {
          timeout_arg_index = a == "-t" ? k + 1 : k;
          break;
        }
      }
      report_soft_builtin_error(
          ec, cxt, ec.arg_location_at(timeout_arg_index),
          "Invalid timeout specification",
          "The -t timeout needs a fractional second count such as 0.5");
      return 1;
    }
    timeout_nanos = parsed.value();
  }

  if (timeout_nanos == 0)
    return os::wait_for_fd_readable(read_fd, 0) == 1 ? 0 : 1;

  let was_timed_out = false;
  let const start_nanos = os::monotonic_nanos();
  let const duration_nanos = static_cast<u64>(timeout_nanos);
  let const deadline_nanos = timeout_nanos < 0
                                 ? 0
                                 : (UINT64_MAX - start_nanos < duration_nanos
                                        ? UINT64_MAX
                                        : start_nanos + duration_nanos);
  let const do_read_byte = [&](char &byte) -> Maybe<usize> {
    if (deadline_nanos != 0) {
      let const now_nanos = os::monotonic_nanos();
      if (now_nanos >= deadline_nanos) {
        was_timed_out = true;
        return koshka::None;
      }
      let const remaining_nanos_unsigned = deadline_nanos - now_nanos;
      let const remaining_nanos = static_cast<i64>(
          remaining_nanos_unsigned > static_cast<u64>(INT64_MAX)
              ? INT64_MAX
              : remaining_nanos_unsigned);
      let const readable = os::wait_for_fd_readable(read_fd, remaining_nanos);
      if (readable != 1) {
        was_timed_out = readable == 0;
        return koshka::None;
      }
    }

    return os::read_fd(read_fd, &byte, 1);
  };

  let terminal_echo =
      os::terminal_echo_guard{read_fd, FLAG_READ_SILENT.is_enabled()};
  if (!terminal_echo.did_succeed()) {
    report_soft_builtin_error(ec, cxt,
                              "Unable to disable terminal echo: " +
                                  os::last_system_error_message());
    return 1;
  }

  /* A -p prompt prints only when the read's own input is a terminal, matching
     bash for a redirected descriptor. */
  if (FLAG_READ_PROMPT.is_set() &&
      os::is_fd_a_tty(ec.in_fd.value_or(KOSH_STDIN)))
  {
    koshka::print_error(FLAG_READ_PROMPT.value());
  }

  /* Query mode is a yes or no probe, so the answer never reaches a variable. */
  if (FLAG_READ_QUERY.is_enabled()) {
    char answer = 0;
    let const got = do_read_byte(answer);
    if (!got.has_value() || *got == 0) {
      return was_timed_out ? 142 : 1;
    }
    return (answer == 'y' || answer == 'Y') ? 0 : 1;
  }

  let const has_operands = names.count() > 1;
  LOG(Debug, "read reading into '%s'",
      has_operands ? names[1].c_str() : "REPLY");
  const usize first_operand = 1;
  let const operand_count = has_operands ? names.count() - first_operand : 1;
  const String reply_name = "REPLY";
  let const do_operand_name = [&](usize index) -> const String & {
    if (!has_operands) return reply_name;
    ASSERT(first_operand + index < names.count());
    return names[first_operand + index];
  };

  /* An empty -d argument reads until a NUL byte, so the whole input is slurped.
   */
  let const delimiter =
      FLAG_READ_DELIM.is_set()
          ? (FLAG_READ_DELIM.value().is_empty() ? '\0'
                                                : FLAG_READ_DELIM.value()[0])
          : '\n';

  /* Reaching the -n count is a success the way the delimiter is, while end of
     input before the count yields the short-read status below. */
  i64 max_bytes = 0;
  if (FLAG_READ_NCHARS.is_set()) {
    let const parsed = FLAG_READ_NCHARS.value().to<i64>();
    if (parsed.is_error() || parsed.value() < 0) {
      report_soft_builtin_error(ec, cxt, FLAG_READ_NCHARS.value_location(),
                                FLAG_READ_NCHARS.value() +
                                    ": invalid byte count");
      return 1;
    }
    max_bytes = parsed.value();
  }

  let const should_process_escapes = !FLAG_READ_RAW.is_enabled();

  let was_newline_terminated = false;
  let was_read_successful = false;

  let line = String{cxt.scratch_allocator()};
  let is_literal_byte = ArrayList<bool>{cxt.scratch_allocator()};

  if (FLAG_READ_NCHARS.is_set()) {
    i64 output_count = 0;
    while (output_count < max_bytes) {
      char byte = 0;
      let const got = do_read_byte(byte);
      if (!got.has_value() || *got == 0) {
        break;
      }

      if (should_process_escapes && byte == '\\') {
        char escaped_byte = 0;
        let const got_escaped = do_read_byte(escaped_byte);
        if (!got_escaped.has_value() || *got_escaped == 0) {
          break;
        }

        if (escaped_byte == '\n') continue;

        line.push(escaped_byte);
        is_literal_byte.push(true);
        output_count++;
        continue;
      }

      if (byte == delimiter) {
        was_newline_terminated = true;
        break;
      }

      line.push(byte);
      is_literal_byte.push(false);
      output_count++;
    }

    if (output_count >= max_bytes) was_newline_terminated = true;
    was_read_successful = output_count > 0 || was_newline_terminated;
  } else {
    let read_line = utils::read_line_from_fd(
        read_fd, was_newline_terminated, delimiter, deadline_nanos,
        &was_timed_out, cxt.scratch_allocator());
    if (read_line.has_value()) {
      was_read_successful = true;

      let accumulated = steal(*read_line);
      if (should_process_escapes) {
        let const do_trailing_backslash_count = [&accumulated]() -> usize {
          usize backslash_count = 0;
          while (backslash_count < accumulated.length() &&
                 accumulated[accumulated.length() - 1 - backslash_count] ==
                     '\\')
            backslash_count++;
          return backslash_count;
        };
        while (was_newline_terminated && do_trailing_backslash_count() % 2 == 1)
        {
          accumulated.pop_back();
          let const continued = utils::read_line_from_fd(
              read_fd, was_newline_terminated, delimiter, deadline_nanos,
              &was_timed_out, cxt.scratch_allocator());
          if (!continued.has_value()) break;
          accumulated.append(continued->view());
        }
      }

      line.reserve(accumulated.length());
      is_literal_byte.reserve(accumulated.length());
      for (usize i = 0; i < accumulated.length(); i++) {
        if (should_process_escapes && accumulated[i] == '\\') {
          if (i + 1 >= accumulated.length()) break;
          line.push(accumulated[i + 1]);
          is_literal_byte.push(true);
          i++;
        } else {
          line.push(accumulated[i]);
          is_literal_byte.push(false);
        }
      }
    }
  }

  if (!was_read_successful) {
    for (usize i = 0; i < operand_count; i++)
      cxt.set_shell_variable(do_operand_name(i), "");
    return was_timed_out ? 142 : 1;
  }

  let const field_separators =
      String{cxt.scratch_allocator(), cxt.field_separators()};
  let const do_is_separator = [&](usize i) {
    return !is_literal_byte[i] &&
           field_separators.find_character(line[i]).has_value();
  };
  /* POSIX folds an IFS whitespace run into a single delimiter, while each IFS
     non-whitespace character delimits one field on its own, so an empty field
     can sit between two non-whitespace delimiters. */
  let const do_is_ifs_whitespace = [&](usize i) {
    return (line[i] == ' ' || line[i] == '\t' || line[i] == '\n') &&
           do_is_separator(i);
  };
  let const do_is_ifs_nonwhitespace = [&](usize i) {
    return do_is_separator(i) && !do_is_ifs_whitespace(i);
  };

  if (FLAG_READ_ARRAY.is_set()) {
    let words = ArrayList<String>{heap_allocator()};
    usize cursor = 0;
    while (cursor < line.length() && do_is_ifs_whitespace(cursor))
      cursor++;
    while (cursor < line.length()) {
      let const start = cursor;
      while (cursor < line.length() && !do_is_separator(cursor))
        cursor++;
      words.push(String{line.substring_of_length(start, cursor - start)});
      while (cursor < line.length() && do_is_ifs_whitespace(cursor))
        cursor++;
      if (cursor < line.length() && do_is_ifs_nonwhitespace(cursor)) {
        cursor++;
        while (cursor < line.length() && do_is_ifs_whitespace(cursor))
          cursor++;
      }
    }
    cxt.set_indexed_array(FLAG_READ_ARRAY.value(), steal(words));
    return was_timed_out ? 142 : (was_newline_terminated ? 0 : 1);
  }

  if (!has_operands) {
    cxt.set_shell_variable(reply_name, line.view());
    return was_timed_out ? 142 : (was_newline_terminated ? 0 : 1);
  }

  usize cursor = 0;
  while (cursor < line.length() && do_is_ifs_whitespace(cursor))
    cursor++;

  for (usize i = 0; i < operand_count; i++) {
    if (i + 1 == operand_count) {
      let const field_start = cursor;
      while (cursor < line.length() && !do_is_separator(cursor))
        cursor++;
      let const field_end = cursor;

      let has_trailing_content = false;
      if (cursor < line.length()) {
        if (do_is_ifs_whitespace(cursor)) {
          while (cursor < line.length() && do_is_ifs_whitespace(cursor))
            cursor++;
        } else {
          cursor++;
          while (cursor < line.length() && do_is_ifs_whitespace(cursor))
            cursor++;
        }
        has_trailing_content = cursor < line.length();
      }

      usize end_position = field_end;
      if (has_trailing_content) {
        end_position = line.length();
        while (end_position > field_start &&
               do_is_ifs_whitespace(end_position - 1))
          end_position--;
      }

      cxt.set_shell_variable(
          do_operand_name(i),
          line.substring_of_length(field_start, end_position - field_start));
      break;
    }

    let const start = cursor;
    while (cursor < line.length() && !do_is_separator(cursor))
      cursor++;
    cxt.set_shell_variable(do_operand_name(i),
                           line.substring_of_length(start, cursor - start));

    while (cursor < line.length() && do_is_ifs_whitespace(cursor))
      cursor++;
    if (cursor < line.length() && do_is_ifs_nonwhitespace(cursor)) {
      cursor++;
      while (cursor < line.length() && do_is_ifs_whitespace(cursor))
        cursor++;
    }
  }

  return was_timed_out ? 142 : (was_newline_terminated ? 0 : 1);
}

} /* namespace koshka */
