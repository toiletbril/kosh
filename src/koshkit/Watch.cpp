/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements the watch utility. It clears the screen, renders the
 * interval and host title, runs the operand command on a fixed period, and
 * stops on an interrupt, on a failing status, or on a change of the output.
 */

#include "../CLI.hpp"
#include "../CLIColors.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Platform.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-tgex] [-n seconds] command [argument ...]");

HELP_DESCRIPTION_DECL("The watch utility runs a command at fixed intervals and "
                      "shows its output.");

FLAG(WATCH_INTERVAL, String, 'n', "interval",
     "Wait this many seconds between runs. The default is two.");
FLAG(WATCH_NO_TITLE, Bool, 't', "no-title",
     "Omit the header and its blank "
     "line.");
FLAG(WATCH_CHANGE_EXIT, Bool, 'g', "chgexit",
     "Stop as soon as the output of the command changes.");
FLAG(WATCH_ERROR_EXIT, Bool, 'e', "errexit",
     "Stop as soon as the command reports a nonzero status.");
FLAG(WATCH_EXEC, Bool, 'x', "exec",
     "Accepted for compatibility. The operands always run as one command.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Watch);

namespace koshka::koshkit {

namespace {

constexpr f64 DEFAULT_INTERVAL_SECONDS = 2.0;
constexpr u32 FALLBACK_COLUMN_COUNT = 80;

fn append_one_decimal(String &output, f64 seconds) throws -> void
{
  let const tenths = static_cast<u64>((seconds * 10.0) + 0.5);
  output += String::from(tenths / 10, output.allocator()).view();
  output += ".";
  output += String::from(tenths % 10, output.allocator()).view();
}

fn build_title(StringView command, f64 interval_seconds, u32 column_count,
               Allocator allocator) throws -> String
{
  let left = String{allocator, "Every "};
  append_one_decimal(left, interval_seconds);
  left += "s: ";
  left += command;

  let right = String{allocator};
  let const host = os::get_hostname();
  if (host.has_value()) {
    right += host->view();
    right += ": ";
  }

  right += os::format_local_time("%a %b %e %H:%M:%S %Y", -1).view();

  let title = String{allocator};
  let const total_length = left.length() + right.length();
  if (total_length + 1 > column_count) {
    title += left.view();
    title += "\n";
    return title;
  }

  title += left.view();
  for (usize index = total_length; index < column_count; index++)
    title += " ";

  title += right.view();
  title += "\n";
  return title;
}

fn append_truncated_lines(String &output, StringView body,
                          u32 column_count) throws -> void
{
  usize position = 0;
  while (position < body.length) {
    usize line_end = position;
    while (line_end < body.length && body[line_end] != '\n')
      line_end++;

    let const line_length = line_end - position;
    let const kept_length =
        line_length > column_count ? column_count : line_length;
    output += body.substring_of_length(position, kept_length);
    output += "\n";
    position = line_end < body.length ? line_end + 1 : body.length;
  }
}

} // namespace

Watch::Watch() = default;

pure fn Watch::kind() const wontthrow -> Utility::Kind { return Kind::Watch; }

fn Watch::execute(const ExecContext &ec, EvalContext &cxt,
                  const ArrayList<String> &args,
                  const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let const[operands, operand_locations] = parse_util_operands(
      FLAG_LIST, args, cxt.scratch_allocator(), &arg_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  if (operands.is_empty()) {
    return report_usage_error(ec, cxt, args[0].view());
  }

  let const allocator = cxt.scratch_allocator();
  f64 interval_seconds = DEFAULT_INTERVAL_SECONDS;
  if (FLAG_WATCH_INTERVAL.is_set()) {
    interval_seconds = parse_koshkit_duration_seconds(
        FLAG_WATCH_INTERVAL.value(), FLAG_WATCH_INTERVAL.value_location(),
        allocator);
    if (interval_seconds < 0.1) interval_seconds = 0.1;
  }

  let command = String{allocator};
  for (let const &operand : operands) {
    if (!command.is_empty()) command += " ";

    command += operand;
  }

  let previous_body = String{allocator};
  bool has_previous_body = false;
  i32 status = 0;
  let const is_terminal = colors::stdout_is_a_terminal();
  let const should_color = koshkit_should_color();

  let const saved_terminal_exec = cxt.terminal_exec_allowed();
  cxt.set_terminal_exec_allowed(false);
  defer { cxt.set_terminal_exec_allowed(saved_terminal_exec); };

  bool is_alternate_screen_active = false;
  if (is_terminal) is_alternate_screen_active = enter_alternate_screen(ec);
  defer
  {
    if (is_alternate_screen_active) leave_alternate_screen(ec);
  };

  loop
  {
    u32 column_count = FALLBACK_COLUMN_COUNT;
    if (let const dimensions = os::get_terminal_dimensions())
      column_count = dimensions->columns;

    let const body =
        cxt.capture_command_substitution(command, StringView{"watch"});
    let const command_status = cxt.last_exit_status();

    let screen = String{allocator};
    if (is_terminal) screen += "\x1b[H\x1b[2J";
    if (!FLAG_WATCH_NO_TITLE.is_enabled()) {
      let const title = build_title(command.view(), interval_seconds,
                                    column_count, allocator);
      append_report_text(screen, title.view(), colors::ansi::BOLD_CYAN,
                         should_color);
      screen += "\n";
    }

    append_truncated_lines(screen, body.view(), column_count);
    ec.print_to_stdout(screen);

    if (FLAG_WATCH_ERROR_EXIT.is_enabled() && command_status != 0) {
      status = command_status;
      break;
    }

    if (FLAG_WATCH_CHANGE_EXIT.is_enabled() && has_previous_body &&
        previous_body.view() != body.view())
    {
      break;
    }

    previous_body = String{allocator, body.view()};
    has_previous_body = true;

    os::sleep_for_seconds(interval_seconds);
    if (os::INTERRUPT_REQUESTED != 0) {
      os::INTERRUPT_REQUESTED = 0;
      break;
    }
  }

  return status;
}

} // namespace koshka::koshkit
