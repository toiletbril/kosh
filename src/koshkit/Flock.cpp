/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements the flock utility. It acquires a process lock, runs a
 * command while the lock is held, and can transfer lock ownership to a
 * transaction keeper.
 */

#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Platform.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL(
    "[--transaction-held-lock] directory command [argument ...]");

HELP_DESCRIPTION_DECL(
    "The flock utility runs a command while holding a directory lock.");

FLAG(TRANSACTION_HELD_LOCK, Bool, '\0', "transaction-held-lock",
     "Keep the lock after the process that launched the transaction exits.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Flock);

namespace koshka {

namespace koshkit {

Flock::Flock() = default;

pure fn Flock::kind() const wontthrow -> Utility::Kind { return Kind::Flock; }

fn Flock::execute(const ExecContext &ec, EvalContext &cxt,
                  const ArrayList<String> &args,
                  const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  defer { reset_flags(FLAG_LIST); };

  let operand_locations = ArrayList<SourceLocation>{cxt.scratch_allocator()};
  let const operands =
      parse_util_operands(FLAG_LIST, args, &arg_locations, &operand_locations);
  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  if (operands.count() < 2) return report_usage_error(ec, cxt, args[0].view());

  if (FLAG_TRANSACTION_HELD_LOCK.is_enabled()) {
    let const executable = os::current_executable_path();
    if (!executable.has_value()) return 126;

    let keeper = ArrayList<String>{cxt.scratch_allocator()};
    keeper.reserve(operands.count() + 7);
    keeper.push(String{executable->view()});
    keeper.push(String{"-p"});
    keeper.push(String{"--mood"});
    keeper.push(String{"sh"});
    keeper.push(String{"-c"});
    keeper.push(String{"koshkit flock \"$@\""});
    keeper.push(String{"kosh"});
    for (let const &operand : operands)
      keeper.push(operand.clone());

    unused(cxt.materialize_kosh_identity());
    let const result = os::run_measured(keeper, os::measured_output::Inherit);
    return result.has_value() ? static_cast<i32>(result->exit_status) : 126;
  }

  let const lock = os::acquire_process_lock(operands[0].view());
  if (!lock.has_value()) {
    report_soft_builtin_error(ec, cxt, operand_locations[0],
                              "Cannot acquire directory lock");
    return 1;
  }
  defer { os::release_process_lock(*lock); };

  let command = ArrayList<String>{cxt.scratch_allocator()};
  command.reserve(operands.count() - 1);
  for (usize position = 1; position < operands.count(); position++)
    command.push(operands[position].clone());

  unused(cxt.materialize_kosh_identity());
  let const result =
      os::run_measured(command, os::measured_output::Inherit, *lock);
  return result.has_value() ? static_cast<i32>(result->exit_status) : 126;
}

} /* namespace koshkit */

} /* namespace koshka */
