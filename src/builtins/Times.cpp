#include "../Builtin.hpp"
#include "../Cli.hpp"
#include "../Eval.hpp"
#include "../Platform.hpp"
#include "../Trace.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("");

HELP_DESCRIPTION_DECL(
    "The times builtin prints the user and system time the shell and its "
    "children have used.");

FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_BUILTIN_FLAGS(Times);

namespace koshka {

Times::Times() = default;

pure fn Times::kind() const wontthrow -> Builtin::Kind { return Kind::Times; }

cold i32 Times::execute(ExecContext &ec, EvalContext &cxt) const throws
{
  if (ec.args().count() > 1 && ec.args()[1] == "--help") {
    SHOW_BUILTIN_HELP_AND_RETURN(ec);
  }

  LOG(Debug, "times printing the shell and child process accounting");

  let const times = os::read_process_cpu_times();

  let out = String{cxt.scratch_allocator()};
  out += utils::format_minutes_seconds(times.self_user_seconds) + " " +
         utils::format_minutes_seconds(times.self_system_seconds) + "\n";
  out += utils::format_minutes_seconds(times.child_user_seconds) + " " +
         utils::format_minutes_seconds(times.child_system_seconds) + "\n";
  ec.print_to_stdout(out);

  return 0;
}

} /* namespace koshka */
