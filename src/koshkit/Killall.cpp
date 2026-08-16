#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-l] [-s signal] name");

HELP_DESCRIPTION_DECL(
    "The killall utility sends a signal to each process by exact name.");

FLAG(KILLALL_SIGNAL, String, 's', "signal",
     "The signal to send, a name such as TERM or a number such as 15.");
FLAG(KILLALL_LIST, Bool, 'l', "list", "List the signal names and exit.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Killall);

namespace koshka {

namespace koshkit {

Killall::Killall() = default;

pure fn Killall::kind() const wontthrow -> Utility::Kind
{
  return Kind::Killall;
}

fn Killall::execute(const ExecContext &ec, EvalContext &cxt,
                    const ArrayList<String> &args,
                    const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let const operands = parse_util_operands(FLAG_LIST, args, &arg_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  if (FLAG_KILLALL_LIST.is_enabled()) {
    ec.print_to_stdout(format_signal_list());
    return 0;
  }

  if (operands.is_empty()) return report_usage_error(ec, cxt, args[0].view());
  if (operands.count() != 1)
    throw ErrorWithDetails{"killall expects one process name",
                           "Pass one name, e.g. `killall firefox`"};

  let const wanted = operands[0].view();
  let const signal_number = resolve_koshkit_signal(
      FLAG_KILLALL_SIGNAL.is_set() ? FLAG_KILLALL_SIGNAL.value() : StringView{},
      cxt.scratch_allocator());

  let const self_pid = os::get_shell_process_id();
  let const processes = os::enumerate_processes();
  bool has_signaled_any = false;
  for (const os::process_entry &process : processes) {
    if (process.pid == self_pid) continue;
    if (process.name == wanted) {
      if (os::signal_process(os::process_from_pid(process.pid), signal_number))
        has_signaled_any = true;
    }
  }

  if (!has_signaled_any)
    report_soft_koshkit_error(
        ec, cxt,
        "killall: " + String{cxt.scratch_allocator(), wanted} +
            ": no process found");
  return has_signaled_any ? 0 : 1;
}

} /* namespace koshkit */

} /* namespace koshka */
