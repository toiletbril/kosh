#include "../Builtin.hpp"
#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Platform.hpp"
#include "../Trace.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-f]");

HELP_DESCRIPTION_DECL(
    "The suspend builtin stops the shell until a SIGCONT is received.");

FLAG(FORCE, Bool, 'f', "\0",
     "Suspend even a login shell, which is refused by default.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_BUILTIN_FLAGS(Suspend);

namespace koshka {

Suspend::Suspend() = default;

pure fn Suspend::kind() const wontthrow -> Builtin::Kind
{
  return Kind::Suspend;
}

fn Suspend::execute(ExecContext &ec, EvalContext &cxt) const throws -> i32
{
  let operand_locations = ArrayList<SourceLocation>{cxt.scratch_allocator()};
  let const operands = PARSE_BUILTIN_ARGS_WITH_LOCATIONS(ec, operand_locations);

  if (FLAG_HELP.is_enabled()) SHOW_BUILTIN_HELP_AND_RETURN(ec);

  if (operands.count() > 1) {
    report_soft_builtin_error(ec, cxt,
                              operand_locations.count() > 1
                                  ? operand_locations[1]
                                  : ec.arg_location_at(1),
                              "Suspend takes no arguments",
                              "Run `suspend --help` for the accepted options");
    return 2;
  }

  if (cxt.is_login_shell() && !FLAG_FORCE.is_enabled()) {
    report_soft_builtin_error(ec, cxt, ec.source_location(),
                              "Cannot suspend a login shell",
                              "Pass '-f' to force the suspension");
    return 1;
  }

  let const signal_number = os::signal_number_from_name(StringView{"STOP"});
  ASSERT(signal_number.has_value());

  let const self_pid = os::get_shell_process_id();
  let const self_process = os::process_from_pid(self_pid);
  if (!os::signal_process(self_process, *signal_number)) {
    report_soft_builtin_error(ec, cxt, ec.source_location(),
                              "Unable to suspend the shell",
                              os::last_system_error_message());
    return 1;
  }

  LOG(All, "suspend stopped the shell pid %lld",
      static_cast<long long>(self_pid));
  return 0;
}

} // namespace koshka
