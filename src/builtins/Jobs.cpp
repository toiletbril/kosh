/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements and is responsible for the jobs builtin. The jobs
 * builtin lists the background jobs and their state.
 */

#include "../Builtin.hpp"
#include "../Cli.hpp"
#include "../CliColors.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Trace.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-lnprs] [jobspec ...]");
HELP_DESCRIPTION_DECL(
    "The jobs builtin lists the background jobs and their state.");

FLAG(JOBS_LONG, Bool, 'l', "",
     "List the process id in addition to the normal information.");
FLAG(JOBS_PIDS, Bool, 'p', "", "List process ids only.");
FLAG(JOBS_RUNNING, Bool, 'r', "", "Restrict the output to running jobs.");
FLAG(JOBS_STOPPED, Bool, 's', "", "Restrict the output to stopped jobs.");
FLAG(JOBS_CHANGED, Bool, 'n', "",
     "List only jobs that changed state since the last notification.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_BUILTIN_FLAGS(Jobs);

namespace koshka {

namespace {

pure fn state_word(job::State state) wontthrow -> const char *
{
  switch (state) {
  case job::State::Running: return "Running";
  case job::State::Stopped: return "Stopped";
  case job::State::Done: return "Done";
  }
  return "Unknown";
}

pure fn job_marker(const ArrayList<job> &jobs, usize index) wontthrow -> char
{
  if (jobs.is_empty()) return ' ';
  if (index == jobs.count() - 1) return '+';
  if (index == jobs.count() - 2) return '-';
  return ' ';
}

fn state_color(job::State state, bool should_color) throws -> StringView
{
  if (!should_color) return StringView{};
  switch (state) {
  case job::State::Running: return colors::ansi::BOLD_GREEN;
  case job::State::Stopped: return colors::ansi::BOLD_YELLOW;
  case job::State::Done: return colors::ansi::DIM;
  }
  return StringView{};
}

fn should_color_jobs(EvalContext &cxt) throws -> bool
{
  return cxt.shell_is_interactive() && colors::stdout_wants_color();
}

} /* namespace */

Jobs::Jobs() = default;

pure fn Jobs::kind() const wontthrow -> Builtin::Kind { return Kind::Jobs; }

fn Jobs::execute(ExecContext &ec, EvalContext &cxt) const throws -> i32
{
  let const names = PARSE_BUILTIN_ARGS(ec);

  if (FLAG_HELP.is_enabled()) SHOW_BUILTIN_HELP_AND_RETURN(ec);

  cxt.update_jobs();

  let const should_color = should_color_jobs(cxt);
  let &jobs = cxt.jobs();

  LOG(Debug, "jobs listing %zu registered jobs", jobs.count());

  let selected = ArrayList<usize>{cxt.scratch_allocator()};
  i32 status = 0;
  if (names.count() > 1) {
    for (usize a = 1; a < names.count(); a++) {
      if (let const index = cxt.find_job_index_by_spec(names[a].view());
          index.has_value())
      {
        selected.push(*index);
      } else {
        report_soft_builtin_error(ec, cxt,
                                  "Unable to list the job '" + names[a] +
                                      "' because no such job exists");
        status = 1;
      }
    }
  } else {
    for (usize i = 0; i < jobs.count(); i++)
      selected.push(i);
  }

  let out = String{cxt.scratch_allocator()};
  for (let index : selected) {
    job &job = jobs[index];

    if (FLAG_JOBS_RUNNING.is_enabled() && job.state != job::State::Running) {
      continue;
    }
    if (FLAG_JOBS_STOPPED.is_enabled() && job.state != job::State::Stopped) {
      continue;
    }
    if (FLAG_JOBS_CHANGED.is_enabled() && !job.has_unreported_state_change) {
      continue;
    }

    job.has_unreported_state_change = false;

    if (FLAG_JOBS_PIDS.is_enabled()) {
      out += String::from(job.process_id, cxt.scratch_allocator());
      out.push('\n');
      continue;
    }

    out += "[" + String::from(job.id, cxt.scratch_allocator()) + "]";
    out.push(job_marker(jobs, index));
    out += " ";

    if (FLAG_JOBS_LONG.is_enabled()) {
      out += String::from(job.process_id, cxt.scratch_allocator());
      out += " ";
    }

    out.append(state_color(job.state, should_color));
    let state = StringView{state_word(job.state)};
    out.append(state);
    out.append_repeated(' ', state.length < 7 ? 7 - state.length : 0);
    if (should_color) out += colors::ansi::RESET;

    out += "  ";
    out += job.command.c_str();
    out.push('\n');
  }
  ec.print_to_stdout(out);

  cxt.forget_done_jobs();
  return status;
}

} /* namespace koshka */
