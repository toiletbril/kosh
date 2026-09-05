/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements and is responsible for the time builtin. The time
 * builtin runs a command and reports how long it took.
 */

#include "../Builtin.hpp"
#include "../Cli.hpp"
#include "../Eval.hpp"
#include "../Platform.hpp"
#include "../Trace.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-p|--posix] [-R] command [argument ...]");

HELP_DESCRIPTION_DECL(
    "The time builtin runs a command and reports how long it took.");

FLAG(HELP, Bool, '\0', "help", "Display help.");
FLAG(TIME_POSIX, Bool, 'p', "posix", "Use the POSIX time report.");
FLAG(TIME_RSS, Bool, 'R', "", "Always add the peak resident set size.");

REGISTER_BUILTIN_FLAGS(Time);

namespace koshka {

Time::Time() = default;

pure fn Time::kind() const wontthrow -> Builtin::Kind { return Kind::Time; }

cold fn Time::execute(ExecContext &ec, EvalContext &cxt) const throws -> i32
{
  let const args = PARSE_BUILTIN_ARGS(ec);

  if (FLAG_HELP.is_enabled()) SHOW_BUILTIN_HELP_AND_RETURN(ec);

  ASSERT(!args.is_empty());

  if (args.count() < 2) return 0;

  let command = String{cxt.scratch_allocator()};
  for (usize i = 1; i < args.count(); i++) {
    if (i > 1) command.push(' ');
    command.append(args[i].view());
  }

  LOG(Debug, "time running command '%s' under the clock", command.c_str());

  double user_before = 0, system_before = 0;
  os::children_cpu_seconds(user_before, system_before);

  /* The tail-exec optimization would replace the shell process on the final
     command, so the report would never print. The flag is cleared around the
     run and restored after. */
  let const saved_terminal_exec = cxt.terminal_exec_allowed();
  cxt.set_terminal_exec_allowed(false);
  defer { cxt.set_terminal_exec_allowed(saved_terminal_exec); };

  let const start_nanos = os::monotonic_nanos();

  let const status = cxt.run_source(command, "time", return_handling::Propagate,
                                    ec.source_location(), StringView{"time"});

  let const elapsed_nanos = os::monotonic_nanos() - start_nanos;

  double user_after = 0, system_after = 0;
  os::children_cpu_seconds(user_after, system_after);
  let const rss_after = os::children_peak_rss_bytes();

  let const real_seconds = static_cast<double>(elapsed_nanos) / 1000000000.0;
  let const user_cpu = user_after - user_before;
  let const system_cpu = system_after - system_before;

  let const time_format = cxt.get_variable_value("TIMEFORMAT");
  let const report = utils::format_time_report(
      FLAG_TIME_POSIX.is_enabled(), FLAG_TIME_RSS.is_enabled(), time_format,
      real_seconds, user_cpu, system_cpu, rss_after);

  if (!report.is_empty()) {
    koshka::print_error(report);
    koshka::flush();
  }

  return status;
}

} /* namespace koshka */
