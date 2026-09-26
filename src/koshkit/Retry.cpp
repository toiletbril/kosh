/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements the retry utility. It runs a command until the command
 * succeeds or the attempts run out, and it waits between attempts for a delay
 * that grows by a backoff factor up to a ceiling.
 */

#include "../CLI.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Platform.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL(
    "[-q] [-n attempts] [-d delay] [-b backoff] [-m max-delay] command ...");

HELP_DESCRIPTION_DECL(
    "The retry utility runs a command until it succeeds or the attempts run "
    "out.");

FLAG(RETRY_ATTEMPTS, String, 'n', "attempts",
     "Make at most this many attempts. The default is five.");
FLAG(RETRY_DELAY, String, 'd', "delay",
     "Wait this long after a failed attempt. The default is one second.");
FLAG(RETRY_BACKOFF, String, 'b', "backoff",
     "Multiply the delay by this factor after every failed attempt.");
FLAG(RETRY_MAX_DELAY, String, 'm', "max-delay",
     "Never wait longer than this between attempts.");
FLAG(RETRY_QUIET, Bool, 'q', "quiet", "Report no attempt lines.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Retry);

namespace koshka::koshkit {

namespace {

constexpr i64 DEFAULT_ATTEMPT_COUNT = 5;
constexpr i64 MAXIMUM_ATTEMPT_COUNT = 1000000;
constexpr f64 DEFAULT_DELAY_SECONDS = 1.0;
constexpr f64 DEFAULT_BACKOFF_FACTOR = 1.0;
constexpr f64 MINIMUM_BACKOFF_FACTOR = 1.0;
constexpr f64 MAXIMUM_BACKOFF_FACTOR = 60.0;
constexpr f64 SLEEP_SLICE_SECONDS = 0.05;

fn parse_backoff_factor(StringView text, f64 &out_factor) throws -> bool
{
  let const digits = String{heap_allocator(), text};
  let const parsed = digits.to<f64>();
  if (parsed.is_error()) return false;

  if (parsed.value() < MINIMUM_BACKOFF_FACTOR ||
      parsed.value() > MAXIMUM_BACKOFF_FACTOR)
  {
    return false;
  }

  out_factor = parsed.value();
  return true;
}

fn sleep_interruptibly(f64 seconds) wontthrow -> bool
{
  f64 remaining_seconds = seconds;
  while (remaining_seconds > 0.0) {
    if (os::INTERRUPT_REQUESTED) return false;

    let const slice = remaining_seconds < SLEEP_SLICE_SECONDS
                          ? remaining_seconds
                          : SLEEP_SLICE_SECONDS;
    os::sleep_for_seconds(slice);
    remaining_seconds -= slice;
  }

  return !os::INTERRUPT_REQUESTED;
}

fn build_command_source(const ArrayList<String> &operands,
                        Allocator allocator) throws -> String
{
  let source = String{allocator};
  for (usize index = 0; index < operands.count(); index++) {
    if (index > 0) source.push(' ');

    utils::append_shell_quoted(source, operands[index].view());
  }

  return source;
}

} // namespace

Retry::Retry() = default;

pure fn Retry::kind() const wontthrow -> Utility::Kind { return Kind::Retry; }

fn Retry::execute(const ExecContext &ec, EvalContext &cxt,
                  const ArrayList<String> &args,
                  const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let const operands = PARSE_KOSHKIT_ARGS(args, arg_locations);

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  let const allocator = cxt.scratch_allocator();

  if (operands.is_empty()) {
    return report_usage_error(ec, cxt, args[0].view());
  }

  i64 attempt_limit = DEFAULT_ATTEMPT_COUNT;
  if (FLAG_RETRY_ATTEMPTS.is_set()) {
    let const parsed = utils::parse_integer_in_base(FLAG_RETRY_ATTEMPTS.value(),
                                                    nullptr, int_base::decimal);
    if (parsed.is_error() || parsed.value() < 1 ||
        parsed.value() > MAXIMUM_ATTEMPT_COUNT)
    {
      report_soft_koshkit_util_error(
          ec, cxt, FLAG_RETRY_ATTEMPTS.value_location(), "retry",
          "invalid attempt count '" +
              String{allocator, FLAG_RETRY_ATTEMPTS.value()} + "'",
          "the value is a count of attempts of one or more");
      return 1;
    }

    attempt_limit = parsed.value();
  }

  f64 delay_seconds = DEFAULT_DELAY_SECONDS;
  if (FLAG_RETRY_DELAY.is_set()) {
    delay_seconds = parse_koshkit_duration_seconds(
        FLAG_RETRY_DELAY.value(), FLAG_RETRY_DELAY.value_location(), allocator);
  }

  f64 maximum_delay_seconds = 0.0;
  if (FLAG_RETRY_MAX_DELAY.is_set()) {
    maximum_delay_seconds = parse_koshkit_duration_seconds(
        FLAG_RETRY_MAX_DELAY.value(), FLAG_RETRY_MAX_DELAY.value_location(),
        allocator);
  }

  f64 backoff_factor = DEFAULT_BACKOFF_FACTOR;
  if (FLAG_RETRY_BACKOFF.is_set()) {
    if (!parse_backoff_factor(FLAG_RETRY_BACKOFF.value(), backoff_factor)) {
      report_soft_koshkit_util_error(
          ec, cxt, FLAG_RETRY_BACKOFF.value_location(), "retry",
          "invalid backoff factor '" +
              String{allocator, FLAG_RETRY_BACKOFF.value()} + "'",
          "the value is a multiplier between one and sixty");
      return 1;
    }
  }

  let const source = build_command_source(operands, allocator);
  i32 status = 0;

  let const saved_terminal_exec = cxt.terminal_exec_allowed();
  cxt.set_terminal_exec_allowed(false);
  defer { cxt.set_terminal_exec_allowed(saved_terminal_exec); };

  for (i64 attempt = 1; attempt <= attempt_limit; attempt++) {
    status = cxt.run_source(source.view(), "retry", ec.source_location(),
                            StringView{"retry"}, nullptr, nullptr,
                            return_handling::Consume);
    if (status == 0) return 0;

    if (os::INTERRUPT_REQUESTED) {
      os::INTERRUPT_REQUESTED = 0;
      return 130;
    }

    if (attempt == attempt_limit) break;

    if (!FLAG_RETRY_QUIET.is_enabled()) {
      let line = String{allocator};
      line += "retry attempt ";
      line += String::from(attempt, allocator).view();
      line += " of ";
      line += String::from(attempt_limit, allocator).view();
      line += " failed with status ";
      line += String::from(status, allocator).view();
      show_warning(Warning{line.view()});
    }

    if (delay_seconds > 0.0 && !sleep_interruptibly(delay_seconds)) {
      os::INTERRUPT_REQUESTED = 0;
      return 130;
    }

    delay_seconds *= backoff_factor;
    if (maximum_delay_seconds > 0.0 && delay_seconds > maximum_delay_seconds) {
      delay_seconds = maximum_delay_seconds;
    }
  }

  return status;
}

} // namespace koshka::koshkit
