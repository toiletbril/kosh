#include "../Builtin.hpp"
#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Platform.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-a] [-r] [-h] [%job ...]");

HELP_DESCRIPTION_DECL("The disown builtin removes a job from the job table.");

FLAG(HELP, Bool, '\0', "help", "Display help.");
FLAG(ALL, Bool, 'a', "all", "Remove every job.");
FLAG(RUNNING, Bool, 'r', "running", "Remove only the running jobs.");
FLAG(NO_HUP, Bool, 'h', "nohup",
     "Mark the job to skip a hangup on shell exit and keep it in the job "
     "table.");

REGISTER_BUILTIN_FLAGS(Disown);

namespace koshka {

Disown::Disown() = default;

pure fn Disown::kind() const wontthrow -> Builtin::Kind { return Kind::Disown; }

fn Disown::execute(ExecContext &ec, EvalContext &cxt) const throws -> i32
{
  let operand_locations = ArrayList<SourceLocation>{cxt.scratch_allocator()};
  let const names = PARSE_BUILTIN_ARGS_WITH_LOCATIONS(ec, operand_locations);

  ASSERT(!names.is_empty());

  if (FLAG_HELP.is_enabled()) SHOW_BUILTIN_HELP_AND_RETURN(ec);

  if (FLAG_NO_HUP.is_enabled()) return 0;

  if (FLAG_ALL.is_enabled() || FLAG_RUNNING.is_enabled()) {
    let const should_keep_stopped = FLAG_RUNNING.is_enabled();

    if (should_keep_stopped) cxt.update_jobs();

    let ids = ArrayList<i32>{cxt.scratch_allocator()};
    for (const job &job : cxt.jobs()) {
      if (!should_keep_stopped || job.state == job::State::Running) {
        ids.push(job.id);
      }
    }

    for (const i32 id : ids)
      cxt.remove_job(id);

    return 0;
  }

  if (names.count() <= 1) {
    let const job = cxt.most_recent_job();
    if (job == nullptr) throw Error{"There is no such job"};
    cxt.remove_job(job->id);
    return 0;
  }

  for (usize i = 1; i < names.count(); i++) {
    let const job = cxt.find_job_by_spec(names[i]);
    if (job == nullptr || !cxt.remove_job(job->id)) {
      let const loc = i < operand_locations.count() ? operand_locations[i]
                                                    : ec.source_location();
      throw ErrorWithLocation{loc, "'" + names[i] + "' is not a valid job"};
    }
  }

  return 0;
}

} /* namespace koshka */
