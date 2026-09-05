/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements the comm utility. It merges two sorted line streams into
 * three columns and suppresses the selected columns.
 */

#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-123] file1 file2");

HELP_DESCRIPTION_DECL("The comm utility compares two sorted files.");

FLAG(COMM_HIDE_FIRST, Bool, '1', "hide-first", "Suppress the first column.");
FLAG(COMM_HIDE_SECOND, Bool, '2', "hide-second", "Suppress the second column.");
FLAG(COMM_HIDE_COMMON, Bool, '3', "hide-common", "Suppress the common column.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Comm);

namespace koshka::koshkit {

static fn append_comm_line(String &output, usize column, StringView line,
                           const bool should_show[3]) throws -> void
{
  if (!should_show[column]) return;

  for (usize prior = 0; prior < column; prior++)
    if (should_show[prior]) output += '\t';

  output += line;
  output += '\n';
}

Comm::Comm() = default;

pure fn Comm::kind() const wontthrow -> Utility::Kind { return Kind::Comm; }

fn Comm::execute(const ExecContext &ec, EvalContext &cxt,
                 const ArrayList<String> &args,
                 const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let const operands = parse_util_operands(FLAG_LIST, args, &arg_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  if (operands.count() != 2) return report_usage_error(ec, cxt, args[0].view());

  let const left_input = open_named_or_stdin(ec, operands[0].view());
  if (!left_input.has_value()) {
    report_soft_koshkit_error(ec, cxt,
                              "comm: cannot read '" + operands[0] +
                                  "': " + os::last_system_error_message());
    return 2;
  }
  defer
  {
    if (left_input->should_close) os::close_fd(left_input->descriptor);
  };

  let const right_input = open_named_or_stdin(ec, operands[1].view());
  if (!right_input.has_value()) {
    report_soft_koshkit_error(ec, cxt,
                              "comm: cannot read '" + operands[1] +
                                  "': " + os::last_system_error_message());
    return 2;
  }
  defer
  {
    if (right_input->should_close) os::close_fd(right_input->descriptor);
  };

  let left_reader = utils::BufferedLineReader{left_input->descriptor};
  let right_reader = utils::BufferedLineReader{right_input->descriptor};
  let left_result = left_reader.next();
  let right_result = right_reader.next();
  bool const should_show[3] = {!FLAG_COMM_HIDE_FIRST.is_enabled(),
                               !FLAG_COMM_HIDE_SECOND.is_enabled(),
                               !FLAG_COMM_HIDE_COMMON.is_enabled()};
  let output = String{cxt.scratch_allocator()};

  loop
  {
    if (left_result == utils::BufferedLineReader::Result::Error ||
        right_result == utils::BufferedLineReader::Result::Error)
    {
      if (os::INTERRUPT_REQUESTED) return 130;
      report_soft_koshkit_error(ec, cxt,
                                "comm: " + os::last_system_error_message());
      return 2;
    }
    if (left_result == utils::BufferedLineReader::Result::End &&
        right_result == utils::BufferedLineReader::Result::End)
      break;

    if (left_result == utils::BufferedLineReader::Result::End) {
      append_comm_line(output, 1, right_reader.get_line(), should_show);
      right_result = right_reader.next();
    } else if (right_result == utils::BufferedLineReader::Result::End) {
      append_comm_line(output, 0, left_reader.get_line(), should_show);
      left_result = left_reader.next();
    } else {
      let const left_line = left_reader.get_line();
      let const right_line = right_reader.get_line();
      if (left_line == right_line) {
        append_comm_line(output, 2, left_line, should_show);
        left_result = left_reader.next();
        right_result = right_reader.next();
      } else if (left_line < right_line) {
        append_comm_line(output, 0, left_line, should_show);
        left_result = left_reader.next();
      } else {
        append_comm_line(output, 1, right_line, should_show);
        right_result = right_reader.next();
      }
    }

    if (output.length() >= 65536) {
      ec.print_to_stdout(output);
      output.clear();
    }
  }

  ec.print_to_stdout(output);
  return 0;
}

} // namespace koshka::koshkit
