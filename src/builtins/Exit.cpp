/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements and is responsible for the exit builtin. The exit
 * builtin ends the shell with a status.
 */

#include "../Builtin.hpp"
#include "../Eval.hpp"
#include "../Trace.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[n]");

HELP_DESCRIPTION_DECL("The exit builtin ends the shell with a status.");

FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_BUILTIN_FLAGS(Exit);

namespace koshka {

Exit::Exit() = default;

pure fn Exit::kind() const wontthrow -> Builtin::Kind { return Kind::Exit; }

fn Exit::execute(ExecContext &ec, EvalContext &cxt) const throws -> i32
{
  ASSERT(!ec.args().is_empty());

  if (ec.args().count() > 1 && ec.args()[1] == "--help") {
    SHOW_BUILTIN_HELP_AND_RETURN(ec);
  }

  let status = static_cast<i64>(cxt.last_exit_status());

  if (ec.args().count() > 1) {
    let const parsed_status = ec.args()[1].to<i64>();

    if (parsed_status.is_error()) {
      report_soft_builtin_error(ec, cxt, ec.arg_location_at(1),
                                StringView{"'"} + ec.args()[1] +
                                    "' is not a numeric exit status");
      return 2;
    }

    if (ec.args().count() > 2) {
      report_soft_builtin_error(ec, cxt, ec.arg_location_at(2),
                                "too many arguments",
                                "exit takes at most one status, e.g. `exit 1`");

      if (cxt.shell_is_interactive()) return 2;

      status = 1;
    } else {
      status = parsed_status.value();
    }
  }

  LOG(Debug, "exit ending the shell with status %lld",
      static_cast<long long>(status));

  if (cxt.in_subshell()) {
    let const masked_status = status & 0xFF;
    cxt.request_exit(masked_status, ec.source_location());
    return static_cast<i32>(masked_status);
  }

  cxt.run_exit_trap();
  utils::quit(static_cast<i32>(status), utils::farewell_policy::Goodbye);
}

} /* namespace koshka */
