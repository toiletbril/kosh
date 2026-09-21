/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements the pkill utility. It compiles a process-name pattern,
 * resolves the requested signal, enumerates matching processes, and signals
 * each match.
 */

#include "../CLI.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-l] [-s signal] pattern");

HELP_DESCRIPTION_DECL(
    "The pkill utility sends a signal to each process whose name matches a "
    "pattern.");

FLAG(PKILL_SIGNAL, String, 's', "signal",
     "The signal to send, a name such as TERM or a number such as 15.");
FLAG(PKILL_LIST, Bool, 'l', "list", "List the signal names and exit.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Pkill);

namespace koshka {

namespace koshkit {

static fn uppercase_signal_name(StringView spelled, Allocator allocator) throws
    -> String
{
  let uppercased = String{allocator};
  uppercased.reserve(spelled.length);
  for (usize i = 0; i < spelled.length; i++) {
    let const byte = spelled[i];
    uppercased.push(byte >= 'a' && byte <= 'z'
                        ? static_cast<char>(byte - 'a' + 'A')
                        : byte);
  }
  return uppercased;
}

fn resolve_koshkit_signal(StringView spelled, SourceLocation location,
                          Allocator allocator) throws -> i32
{
  if (spelled.is_empty()) return SIGTERM;
  let const parsed = spelled.to<i64>();
  if (!parsed.is_error()) {
    if (parsed.value() < INT32_MIN || parsed.value() > INT32_MAX) {
      throw ErrorWithLocationAndDetails{
          location, "signal number is out of range",
          "Use a signal name such as TERM or a number such as 15"};
    }
    return static_cast<i32>(parsed.value());
  }
  let const uppercased = uppercase_signal_name(spelled, allocator);
  let const named = os::signal_number_from_name(uppercased.view());
  if (!named.has_value()) {
    let const unknown_signal = String{allocator, spelled};
    throw ErrorWithLocationAndDetails{
        location, "unknown signal '" + unknown_signal + "'",
        "Use a signal name such as TERM or a number such as 15"};
  }
  return *named;
}

Pkill::Pkill() = default;

pure fn Pkill::kind() const wontthrow -> Utility::Kind { return Kind::Pkill; }

fn Pkill::execute(const ExecContext &ec, EvalContext &cxt,
                  const ArrayList<String> &args,
                  const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let const[operands, operand_locations] = parse_util_operands(
      FLAG_LIST, args, cxt.scratch_allocator(), &arg_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  if (FLAG_PKILL_LIST.is_enabled()) {
    ec.print_to_stdout(format_signal_list());
    return 0;
  }

  if (operands.is_empty()) return report_usage_error(ec, cxt, args[0].view());
  if (operands.count() != 1) {
    KOSHKIT_REPORT_ERROR_AT(operand_locations[1], "expects one pattern",
                            "pass one pattern, e.g. `pkill ssh`");
    return 1;
  }

  let const pattern = operands[0].view();
  if (pattern.is_empty()) {
    KOSHKIT_REPORT_ERROR_AT(operand_locations[0],
                            "requires a non-empty pattern",
                            "pass a pattern, e.g. `pkill ssh`");
    return 1;
  }

  let const signal_number = resolve_koshkit_signal(
      FLAG_PKILL_SIGNAL.is_set() ? FLAG_PKILL_SIGNAL.value() : StringView{},
      FLAG_PKILL_SIGNAL.value_location(), cxt.scratch_allocator());

  let const self_pid = os::get_shell_process_id();
  let const processes = os::enumerate_processes();
  bool did_signal_any = false;
  for (let const &process : processes) {
    if (process.pid == self_pid) continue;
    if (process.name.find_substring(pattern, 0).has_value()) {
      if (os::signal_process(os::process_from_pid(process.pid), signal_number))
      {
        did_signal_any = true;
      } else {
        let const reason = os::last_system_error_message();
        report_soft_koshkit_util_error(
            ec, cxt, args[0].view(),
            "killing pid " +
                String::from(process.pid, cxt.scratch_allocator()) +
                " failed: " + reason);
      }
    }
  }

  return did_signal_any ? 0 : 1;
}

} /* namespace koshkit */

} /* namespace koshka */
