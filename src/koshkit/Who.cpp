/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements the who utility. It queries platform login records and
 * renders selected session, boot, process, run-level, terminal-state, and
 * idle-time fields.
 */

#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-abdHlmpqrstTu] [am i]");

HELP_DESCRIPTION_DECL("The who utility writes logged-in users.");

FLAG(WHO_ALL, Bool, 'a', "all", "Write all available records.");
FLAG(WHO_BOOT, Bool, 'b', "boot", "Write the last system boot record.");
FLAG(WHO_DEAD, Bool, 'd', "dead", "Write dead process records.");
FLAG(WHO_HEADING, Bool, 'H', "heading", "Write column headings.");
FLAG(WHO_LOGIN, Bool, 'l', "login", "Write login process records.");
FLAG(WHO_CURRENT, Bool, 'm', "current", "Write the current terminal record.");
FLAG(WHO_PROCESS, Bool, 'p', "process", "Write active process records.");
FLAG(WHO_QUICK, Bool, 'q', "quick", "Write login names and their count.");
FLAG(WHO_RUNLEVEL, Bool, 'r', "runlevel", "Write the current run level.");
FLAG(WHO_SHORT, Bool, 's', "short", "Write names, lines, and login times.");
FLAG(WHO_TIME, Bool, 't', "time", "Write the last system clock change.");
FLAG(WHO_STATE, Bool, 'T', "terminal-state", "Write terminal write states.");
FLAG(WHO_IDLE, Bool, 'u', "idle", "Write idle times.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Who);

namespace koshka::koshkit {

Who::Who() = default;

pure fn Who::kind() const wontthrow -> Utility::Kind { return Kind::Who; }

fn Who::execute(const ExecContext &ec, EvalContext &cxt,
                const ArrayList<String> &args,
                const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let const operands = parse_util_operands(FLAG_LIST, args, &arg_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  if (!operands.is_empty() &&
      (operands.count() != 2 || operands[0].view() != "am" ||
       operands[1].view() != "i"))
    return report_usage_error(ec, cxt, args[0].view());
  let const is_current = FLAG_WHO_CURRENT.is_enabled() || !operands.is_empty();
  if (!is_current) {
    let const has_nonuser_selection =
        !FLAG_WHO_ALL.is_enabled() &&
        (FLAG_WHO_BOOT.is_enabled() || FLAG_WHO_DEAD.is_enabled() ||
         FLAG_WHO_LOGIN.is_enabled() || FLAG_WHO_PROCESS.is_enabled() ||
         FLAG_WHO_RUNLEVEL.is_enabled() || FLAG_WHO_TIME.is_enabled());
    if (has_nonuser_selection) return 0;

    let const sessions = os::logged_in_users();
    let output = String{cxt.scratch_allocator()};
    if (FLAG_WHO_QUICK.is_enabled()) {
      for (let const &session : sessions) {
        output += session.user.view();
        output += ' ';
      }
      if (!sessions.is_empty()) output += '\n';
      output += "# users = ";
      output += String::from(sessions.count(), cxt.scratch_allocator());
      output += '\n';
      ec.print_to_stdout(output);
      return 0;
    }
    if (FLAG_WHO_HEADING.is_enabled()) output += "NAME LINE TIME\n";

    for (let const &session : sessions) {
      output += session.user.view();
      output += ' ';
      if (FLAG_WHO_STATE.is_enabled()) output += "? ";
      output += session.terminal.view();
      if (session.login_time != 0) {
        output += ' ';
        output +=
            utils::format_unix_timestamp(session.login_time, "%b %e %H:%M");
      }
      if (FLAG_WHO_IDLE.is_enabled()) output += " .";
      output += '\n';
    }

    ec.print_to_stdout(output);
    return 0;
  }

  let const user = os::get_current_user();
  let const terminal = os::terminal_name(ec.in_fd.value_or(KOSH_STDIN));
  if (!user.has_value() || !terminal.has_value()) return 1;
  ec.print_to_stdout(user->view());
  ec.print_to_stdout(" ");
  ec.print_to_stdout(terminal->view());
  ec.print_to_stdout("\n");
  return 0;
}

} // namespace koshka::koshkit
