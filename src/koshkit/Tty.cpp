/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements and is responsible for the tty utility in koshkit.
 * The tty utility writes the terminal name.
 */

#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-s]");

HELP_DESCRIPTION_DECL("The tty utility writes the terminal name.");

FLAG(TTY_SILENT, Bool, 's', "silent", "Write no output.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Tty);

namespace koshka::koshkit {

Tty::Tty() = default;

pure fn Tty::kind() const wontthrow -> Utility::Kind { return Kind::Tty; }

fn Tty::execute(const ExecContext &ec, EvalContext &cxt,
                const ArrayList<String> &args,
                const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let const operands = parse_util_operands(FLAG_LIST, args, &arg_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  if (!operands.is_empty()) return report_usage_error(ec, cxt, args[0].view());
  let const name = os::terminal_name(ec.in_fd.value_or(KOSH_STDIN));
  if (!name.has_value()) {
    if (!FLAG_TTY_SILENT.is_enabled()) ec.print_to_stdout("not a tty\n");
    return 1;
  }
  if (!FLAG_TTY_SILENT.is_enabled()) {
    ec.print_to_stdout(name->view());
    ec.print_to_stdout("\n");
  }
  return 0;
}

} // namespace koshka::koshkit
