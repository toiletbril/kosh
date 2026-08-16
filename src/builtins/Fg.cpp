#include "../Builtin.hpp"
#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Platform.hpp"
#include "../Toiletline.hpp"
#include "../Trace.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[%job]");
HELP_DESCRIPTION_DECL(
    "The fg builtin brings a job to the foreground and waits for it.");

FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_BUILTIN_FLAGS(Fg);

namespace koshka {

Fg::Fg() = default;

pure fn Fg::kind() const wontthrow -> Builtin::Kind { return Kind::Fg; }

fn Fg::execute(ExecContext &ec, EvalContext &cxt) const throws -> i32
{
  let const &args = ec.args();
  ASSERT(!args.is_empty());

  if (args.count() > 1 && args[1] == "--help") {
    SHOW_BUILTIN_HELP_AND_RETURN(ec);
  }

  job *job = nullptr;
  if (args.count() > 1 && !args[1].is_empty()) {
    job = cxt.find_job_by_spec(args[1]);
    if (job == nullptr)
      throw ErrorWithDetails{"'" + args[1] + "' is not a valid job",
                             "Use a job spec like `%1`, `%+`, or `%name`"};
  } else {
    job = cxt.most_recent_job();
  }

  if (job == nullptr)
    throw ErrorWithDetails{"There is no such job", "List jobs with `jobs`"};

  LOG(Info, "fg bringing job %d to the foreground", job->id);

  /* A job reaped by a prior poll has its status recorded, so it is reported
     without waiting on a pid that no longer exists. */
  if (job->state == job::State::Done) {
    let const done_status = job->last_status;
    cxt.forget_done_jobs();
    return done_status;
  }

  let command = job->command.view();
  if (command.count() >= 2 &&
      command.substring_of_length(command.count() - 2, 2) == " &")
  {
    command = command.substring_of_length(0, command.count() - 2);
  }
  ec.print_to_stdout(command + "\n");
  if (cxt.shell_is_interactive()) toiletline::set_title(command);

  let const should_reclaim =
      cxt.shell_is_interactive() && os::shell_has_controlling_terminal();
  let should_reclaim_after_wait = should_reclaim && job->process_group_id > 0;
  if (should_reclaim_after_wait) {
    LOG(Debug,
        "fg will give the terminal to process group %lld before it resumes job "
        "%d",
        static_cast<long long>(job->process_group_id), job->id);
    os::give_controlling_terminal_to_process_group(job->process_group_id);
  }
  defer
  {
    if (should_reclaim_after_wait) os::reclaim_controlling_terminal();
  };

  if (job->state == job::State::Stopped) {
    if (const Maybe<i32> cont = os::signal_number_from_name("CONT")) {
      if (job->is_primary_process_active) os::signal_process(job->pid, *cont);
      for (let const process : job->earlier_pipeline_processes)
        os::signal_process(process, *cont);
    }
    job->state = job::State::Running;
  }

  let was_stopped = false;
  let const status = cxt.wait_for_job_processes(*job, &was_stopped);
  if (should_reclaim_after_wait) {
    os::reclaim_controlling_terminal();
    should_reclaim_after_wait = false;
  }

  if (was_stopped) {
    job->state = job::State::Stopped;
    job->stopped_status = status;
    cxt.notify_stopped_job(job->id, job->command.view());
    return status;
  }

  cxt.forget_done_jobs();

  return status;
}

} /* namespace koshka */
