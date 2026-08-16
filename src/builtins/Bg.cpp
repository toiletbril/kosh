#include "../Builtin.hpp"
#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Platform.hpp"
#include "../Trace.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[%job]");

HELP_DESCRIPTION_DECL(
    "The bg builtin resumes a stopped job in the background.");

FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_BUILTIN_FLAGS(Bg);

namespace koshka {

Bg::Bg() = default;

pure fn Bg::kind() const wontthrow -> Builtin::Kind { return Kind::Bg; }

static fn resume_job_in_background(ExecContext &ec, EvalContext &cxt,
                                   job *job) throws -> void
{
  LOG(Info, "bg resuming job %d in the background", job->id);

  if (const Maybe<i32> cont = os::signal_number_from_name("CONT")) {
    if (job->is_primary_process_active) os::signal_process(job->pid, *cont);
    for (let const process : job->earlier_pipeline_processes)
      os::signal_process(process, *cont);
  }
  job->state = job::State::Running;

  ec.print_to_stdout("[" + String::from(job->id, cxt.scratch_allocator()) +
                     "] " + job->command + " &\n");
}

fn Bg::execute(ExecContext &ec, EvalContext &cxt) const throws -> i32
{
  let const &args = ec.args();
  ASSERT(!args.is_empty());

  if (args.count() > 1 && args[1] == "--help") {
    SHOW_BUILTIN_HELP_AND_RETURN(ec);
  }

  if (args.count() <= 1) {
    job *job = cxt.most_recent_job();
    if (job == nullptr)
      throw ErrorWithDetails{"There is no such job", "List jobs with `jobs`"};

    resume_job_in_background(ec, cxt, job);
    return 0;
  }

  for (usize arg_position = 1; arg_position < args.count(); arg_position++) {
    job *job = cxt.find_job_by_spec(args[arg_position]);
    if (job == nullptr)
      throw ErrorWithDetails{"There is no such job", "List jobs with `jobs`"};

    resume_job_in_background(ec, cxt, job);
  }

  return 0;
}

} /* namespace koshka */
