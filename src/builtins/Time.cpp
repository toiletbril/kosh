#include "../Builtin.hpp"
#include "../Cli.hpp"
#include "../Eval.hpp"
#include "../Platform.hpp"
#include "../Trace.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("command [argument ...]");

HELP_DESCRIPTION_DECL(
    "The time builtin runs a command and reports how long it took.");

FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_BUILTIN_FLAGS(Time);

namespace koshka {

Time::Time() = default;

pure fn Time::kind() const wontthrow -> Builtin::Kind { return Kind::Time; }

cold fn Time::execute(ExecContext &ec, EvalContext &cxt) const throws -> i32
{
  ASSERT(!ec.args().is_empty());

  if (ec.args().count() > 1 && ec.args()[1] == "--help") {
    SHOW_BUILTIN_HELP_AND_RETURN(ec);
  }

  if (ec.args().count() < 2) return 0;

  let command = String{cxt.scratch_allocator()};
  for (usize i = 1; i < ec.args().count(); i++) {
    if (i > 1) command.push(' ');
    command.append(ec.args()[i].view());
  }

  LOG(Debug, "time running command '%s' under the clock", command.c_str());

  double user_before = 0, system_before = 0;
  os::children_cpu_seconds(user_before, system_before);
  let const rss_before = os::children_peak_rss_bytes();

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
  let const peak_rss_bytes = rss_after > rss_before ? rss_after : 0;

  /* An empty TIMEFORMAT prints nothing, an unset value keeps the pretty
     default. */
  String report{cxt.scratch_allocator()};
  if (let const time_format = cxt.get_variable_value("TIMEFORMAT");
      time_format.has_value())
  {
    if (!time_format->is_empty())
      report = utils::format_time_report_custom(
          time_format->view(), real_seconds, user_cpu, system_cpu);
  } else {
    report = utils::format_time_report_pretty(real_seconds, user_cpu,
                                              system_cpu, peak_rss_bytes);
  }

  if (!report.is_empty()) {
    koshka::print_error(report);
    koshka::flush();
  }

  return status;
}

} // namespace koshka
