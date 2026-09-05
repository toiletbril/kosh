/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements and is responsible for the pkill utility in koshkit.
 * The pkill utility sends a signal to each process whose name matches a
 * pattern.
 */

#include "../Cli.hpp"
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

fn resolve_koshkit_signal(StringView spelled, Allocator allocator) throws -> i32
{
  if (spelled.is_empty()) return SIGTERM;
  let const parsed = spelled.to<i64>();
  if (!parsed.is_error()) {
    if (parsed.value() < INT32_MIN || parsed.value() > INT32_MAX) {
      throw Error{"signal number is out of range"};
    }
    return static_cast<i32>(parsed.value());
  }
  let const uppercased = uppercase_signal_name(spelled, allocator);
  let const named = os::signal_number_from_name(uppercased.view());
  if (!named.has_value())
    throw Error{
        "unknown signal '" + String{allocator, spelled}
          + "'"
    };
  return *named;
}

Pkill::Pkill() = default;

pure fn Pkill::kind() const wontthrow -> Utility::Kind { return Kind::Pkill; }

fn Pkill::execute(const ExecContext &ec, EvalContext &cxt,
                  const ArrayList<String> &args,
                  const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let const operands = parse_util_operands(FLAG_LIST, args, &arg_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  if (FLAG_PKILL_LIST.is_enabled()) {
    ec.print_to_stdout(format_signal_list());
    return 0;
  }

  if (operands.is_empty()) return report_usage_error(ec, cxt, args[0].view());
  if (operands.count() != 1)
    throw ErrorWithDetails{"pkill expects one pattern",
                           "Pass one pattern, e.g. `pkill ssh`"};

  let const pattern = operands[0].view();
  if (pattern.is_empty())
    throw ErrorWithDetails{"pkill requires a non-empty pattern",
                           "Pass a pattern, e.g. `pkill ssh`"};

  let const signal_number = resolve_koshkit_signal(
      FLAG_PKILL_SIGNAL.is_set() ? FLAG_PKILL_SIGNAL.value() : StringView{},
      cxt.scratch_allocator());

  let const self_pid = os::get_shell_process_id();
  let const processes = os::enumerate_processes();
  bool did_signal_any = false;
  for (const os::process_entry &process : processes) {
    if (process.pid == self_pid) continue;
    if (process.name.find_substring(pattern, 0).has_value()) {
      if (os::signal_process(os::process_from_pid(process.pid), signal_number))
      {
        did_signal_any = true;
      } else {
        let const reason = os::last_system_error_message();
        report_soft_koshkit_error(
            ec, cxt,
            "pkill: killing pid " +
                String::from(process.pid, cxt.scratch_allocator()) +
                " failed: " + reason);
      }
    }
  }

  return did_signal_any ? 0 : 1;
}

} /* namespace koshkit */

} /* namespace koshka */
