#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../ResolvedCommand.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[option ...] duration command [argument ...]");

HELP_DESCRIPTION_DECL("The timeout utility runs a command with a time limit.");

FLAG(TIMEOUT_SIGNAL, String, 's', "signal",
     "Send this signal when the time limit expires.");
FLAG(TIMEOUT_KILL_AFTER, String, 'k', "kill-after",
     "Send KILL when the command survives this additional duration.");
FLAG(TIMEOUT_PRESERVE_STATUS, Bool, 'p', "preserve-status",
     "Return the command status after the time limit expires.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Timeout);

namespace koshka {

namespace koshkit {

Timeout::Timeout() = default;

pure fn Timeout::kind() const wontthrow -> Utility::Kind
{
  return Kind::Timeout;
}

static fn duration_to_nanos(f64 seconds) wontthrow -> u64
{
  if (__builtin_isinf(seconds)) return UINT64_MAX;
  if (seconds == 0.0) return 0;

  constexpr f64 NANOS_PER_SECOND = 1000000000.0;
  constexpr f64 MAX_SECONDS = static_cast<f64>(UINT64_MAX) / NANOS_PER_SECOND;
  if (seconds >= MAX_SECONDS) return UINT64_MAX;

  let const nanos = seconds * NANOS_PER_SECOND;
  if (nanos < 1.0) return 1;
  return static_cast<u64>(nanos);
}

enum class supervision_wait_result : u8
{
  Exited,
  TimedOut,
  Interrupted,
};

enum class wait_completion : u8
{
  ChildExited,
  ProcessGroupEmpty,
};

static fn wait_for_process_until(os::process child, os::process process_group,
                                 u64 timeout_nanos, i32 &status,
                                 bool &has_child_exited,
                                 wait_completion completion) throws
    -> supervision_wait_result
{
  let const wait_for_process_group =
      completion == wait_completion::ProcessGroupEmpty;
  let const has_deadline = timeout_nanos != 0 && timeout_nanos != UINT64_MAX;
  let const started_at_nanos = os::monotonic_nanos();
  loop
  {
    if (os::INTERRUPT_REQUESTED) return supervision_wait_result::Interrupted;

    if (!has_child_exited) {
      let const state = os::poll_process(child, status);
      has_child_exited = state == os::process_state::Exited;
    }
    if (has_child_exited && (!wait_for_process_group ||
                             !os::process_group_has_members(process_group)))
    {
      return supervision_wait_result::Exited;
    }

    u64 sleep_nanos = 10000000;
    if (has_deadline) {
      let const elapsed_nanos = os::monotonic_nanos() - started_at_nanos;
      if (elapsed_nanos >= timeout_nanos)
        return supervision_wait_result::TimedOut;

      let const remaining_nanos = timeout_nanos - elapsed_nanos;
      sleep_nanos = remaining_nanos < 1000000 ? remaining_nanos : 1000000;
    }
    os::sleep_for_seconds(static_cast<f64>(sleep_nanos) / 1000000000.0);
  }
}

static fn signal_supervised_process(os::process child,
                                    os::process process_group,
                                    bool has_child_exited,
                                    i32 signal_number) wontthrow -> bool
{
  if (os::signal_process(process_group, signal_number)) return true;
  if (has_child_exited) return false;
  return os::signal_process(child, signal_number);
}

static fn timeout_expiration_status(i32 command_status,
                                    i32 timeout_signal) wontthrow -> i32
{
  if (timeout_signal == 9) return 137;
  if (FLAG_TIMEOUT_PRESERVE_STATUS.is_enabled()) return command_status;
  return 124;
}

static fn finish_interrupted_supervision(os::process child,
                                         os::process process_group,
                                         bool has_child_exited) throws -> i32
{
  os::INTERRUPT_REQUESTED = 0;
  if (let const interrupt_signal = os::signal_number_from_name("INT");
      interrupt_signal.has_value() &&
      os::is_process_signal_supported(*interrupt_signal))
  {
    signal_supervised_process(child, process_group, has_child_exited,
                              *interrupt_signal);
  }

  os::sleep_for_seconds(0.01);
  signal_supervised_process(child, process_group, has_child_exited, 9);
  if (!has_child_exited) os::reap_process_quietly(child);
  os::INTERRUPT_REQUESTED = 0;
  return 130;
}

static fn resolve_timeout_program(StringView program_name,
                                  EvalContext &cxt) throws -> Maybe<Path>
{
  if (os::has_directory_separator(program_name)) {
    let const typed_program_path = Path{program_name};
    if (typed_program_path.has_trailing_separator()) {
      let const normalized_program_path = typed_program_path.normalized();
      if (!normalized_program_path.exists()) return None;
      return normalized_program_path;
    }
    return Path::canonicalize(program_name);
  }

  let const matches = cxt.get_program_resolver().search(
      program_name, ProgramResolver::SearchMode::First,
      ProgramResolver::Requirement::Runnable,
      ProgramResolver::CachePolicy::Bypass);
  if (matches.is_empty()) return None;
  return matches[0];
}

static fn checked_timeout_program(StringView program_name,
                                  SourceLocation program_location,
                                  EvalContext &cxt) throws -> Maybe<Path>
{
  let const typed_program_path = Path{program_name};
  let const program_path = resolve_timeout_program(program_name, cxt);
  if (!program_path.has_value()) return None;

  if (typed_program_path.has_trailing_separator() &&
      !program_path->is_directory())
  {
    let error =
        ErrorWithLocation{program_location, "This file is not a directory"};
    error.set_command_status(126);
    throw error;
  }
  if (program_path->is_directory()) {
    let error =
        ErrorWithLocation{program_location, "The command is a directory"};
    error.set_command_status(126);
    throw error;
  }

  return program_path;
}

fn preflight_timeout_stage(const ExecContext &ec, EvalContext &cxt,
                           usize name_index, SourceLocation &error_location,
                           String &error_message) throws -> Maybe<i32>
{
  let args = ArrayList<String>{cxt.scratch_allocator()};
  let arg_locations = ArrayList<SourceLocation>{cxt.scratch_allocator()};
  args.reserve(ec.args().count() - name_index);
  arg_locations.reserve(ec.args().count() - name_index);
  for (usize argument_index = name_index; argument_index < ec.args().count();
       argument_index++)
  {
    args.push_managed(ec.args()[argument_index]);
    arg_locations.push(ec.arg_location_at(argument_index));
  }

  let operand_locations = ArrayList<SourceLocation>{cxt.scratch_allocator()};
  defer { reset_flags(FLAG_LIST); };
  let operands = ArrayList<String>{cxt.scratch_allocator()};
  try {
    operands = parse_util_operands(FLAG_LIST, args, &arg_locations,
                                   &operand_locations, true);
  } catch (const ErrorBase &) {
    return None;
  }
  if (FLAG_HELP.is_enabled() || operands.count() < 2) {
    return None;
  }
  if (!Path{operands[1].view()}.has_trailing_separator()) return None;

  try {
    unused(parse_koshkit_duration_seconds(
        operands[0].view(), StringView{"timeout"}, cxt.scratch_allocator()));
    if (FLAG_TIMEOUT_KILL_AFTER.is_set())
      unused(parse_koshkit_duration_seconds(FLAG_TIMEOUT_KILL_AFTER.value(),
                                            StringView{"timeout"},
                                            cxt.scratch_allocator()));
    let const timeout_signal = resolve_koshkit_signal(
        FLAG_TIMEOUT_SIGNAL.is_set() ? FLAG_TIMEOUT_SIGNAL.value()
                                     : StringView{"TERM"},
        cxt.scratch_allocator());
    if (!os::is_process_signal_supported(timeout_signal)) return None;
  } catch (const ErrorBase &) {
    return None;
  }

  try {
    unused(
        checked_timeout_program(operands[1].view(), operand_locations[1], cxt));
  } catch (const ErrorWithLocation &error) {
    error_location = error.location();
    error_message = "koshkit timeout: " + error.message();
    return static_cast<i32>(error.command_status());
  }

  return None;
}

fn Timeout::execute(const ExecContext &ec, EvalContext &cxt,
                    const ArrayList<String> &args,
                    const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let operand_locations = ArrayList<SourceLocation>{cxt.scratch_allocator()};
  defer { reset_flags(FLAG_LIST); };
  let const operands = parse_util_operands(FLAG_LIST, args, &arg_locations,
                                           &operand_locations, true);

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  if (operands.count() < 2) return report_usage_error(ec, cxt, args[0].view());

  let const timeout_seconds = parse_koshkit_duration_seconds(
      operands[0].view(), StringView{"timeout"}, cxt.scratch_allocator());
  let const timeout_nanos = duration_to_nanos(timeout_seconds);

  u64 kill_after_nanos = 0;
  if (FLAG_TIMEOUT_KILL_AFTER.is_set()) {
    let const kill_after_seconds = parse_koshkit_duration_seconds(
        FLAG_TIMEOUT_KILL_AFTER.value(), StringView{"timeout"},
        cxt.scratch_allocator());
    kill_after_nanos = duration_to_nanos(kill_after_seconds);
  }

  let const timeout_signal = resolve_koshkit_signal(
      FLAG_TIMEOUT_SIGNAL.is_set() ? FLAG_TIMEOUT_SIGNAL.value()
                                   : StringView{"TERM"},
      cxt.scratch_allocator());
  if (!os::is_process_signal_supported(timeout_signal))
    throw Error{"timeout cannot deliver signal " +
                String::from(timeout_signal, cxt.scratch_allocator()) +
                " on this platform"};

  let const program_path =
      checked_timeout_program(operands[1].view(), operand_locations[1], cxt);
  if (!program_path.has_value()) {
    report_soft_koshkit_error(
        ec, cxt, "timeout: command '" + operands[1] + "' was not found");
    return 127;
  }

  let command_args = ArrayList<String>{cxt.scratch_allocator()};
  let command_locations = ArrayList<SourceLocation>{cxt.scratch_allocator()};
  command_args.reserve(operands.count() - 1);
  command_locations.reserve(operands.count() - 1);
  for (usize operand_index = 1; operand_index < operands.count();
       operand_index++)
  {
    command_args.push_managed(operands[operand_index]);
    command_locations.push(operand_locations[operand_index]);
  }

  let command = ExecContext::from_resolved(
      ec.source_location(), ResolvedCommand::from_program(*program_path),
      steal(command_args), steal(command_locations));
  let const source = cxt.current_source();
  let const has_controlling_terminal =
      cxt.shell_is_interactive() && os::shell_has_controlling_terminal();
  let const process_group_mode =
      os::get_environment_variable("KOSH_TEST_TIMEOUT_JOB_LIFETIME") ==
              StringView{"leader"}
          ? os::process_group_mode::NewLeaderOwned
          : os::process_group_mode::New;
  unused(cxt.materialize_kosh_identity());
  defer
  {
    if (has_controlling_terminal) os::reclaim_controlling_terminal();
  };

  os::process child = os::execute_program(
      steal(command), os::script_fallback_policy::Allow, process_group_mode,
      source != nullptr ? source->view() : StringView{},
      has_controlling_terminal ? os::terminal_handoff::BeforeStart
                               : os::terminal_handoff::Keep);
  if (child == KOSH_INVALID_PROCESS) {
    let const shell_path = os::current_executable_path();
    if (!shell_path.has_value())
      throw Error{"Could not locate the shell for script fallback"};

    let fallback_args = ArrayList<String>{cxt.scratch_allocator()};
    let fallback_locations = ArrayList<SourceLocation>{cxt.scratch_allocator()};
    fallback_args.reserve(operands.count());
    fallback_locations.reserve(operands.count());
    fallback_args.push(String{cxt.scratch_allocator(), shell_path->view()});
    fallback_locations.push(ec.source_location());
    for (usize operand_index = 1; operand_index < operands.count();
         operand_index++)
    {
      fallback_args.push_managed(operands[operand_index]);
      fallback_locations.push(operand_locations[operand_index]);
    }

    let fallback = ExecContext::from_resolved(
        ec.source_location(),
        ResolvedCommand::from_program(Path{shell_path->view()}),
        steal(fallback_args), steal(fallback_locations));
    child = os::execute_program(
        steal(fallback), os::script_fallback_policy::Reject, process_group_mode,
        source != nullptr ? source->view() : StringView{},
        has_controlling_terminal ? os::terminal_handoff::BeforeStart
                                 : os::terminal_handoff::Keep);
  }

  os::process process_group = KOSH_INVALID_PROCESS;
  try {
    process_group = os::process_group_of(child);
  } catch (...) {
    os::signal_process(child, 9);
    try {
      os::reap_process_quietly(child);
    } catch (...) {}
    throw;
  }
  defer { os::close_process_group(process_group); };

  i32 status = 0;
  let has_child_exited = false;
  let wait_result =
      wait_for_process_until(child, process_group, timeout_nanos, status,
                             has_child_exited, wait_completion::ChildExited);
  if (wait_result == supervision_wait_result::Exited) return status;
  if (wait_result == supervision_wait_result::Interrupted)
    return finish_interrupted_supervision(child, process_group,
                                          has_child_exited);

  if (!signal_supervised_process(child, process_group, has_child_exited,
                                 timeout_signal))
  {
    if (!has_child_exited &&
        os::poll_process(child, status) == os::process_state::Exited)
    {
      has_child_exited = true;
      return status;
    }
    signal_supervised_process(child, process_group, has_child_exited, 9);
    if (!has_child_exited) os::reap_process_quietly(child);
    return 125;
  }

  if (let const continue_signal = os::signal_number_from_name("CONT");
      continue_signal.has_value() &&
      os::is_process_signal_supported(*continue_signal))
  {
    signal_supervised_process(child, process_group, has_child_exited,
                              *continue_signal);
  }

  wait_result = wait_for_process_until(
      child, process_group,
      kill_after_nanos == 0 ? UINT64_MAX : kill_after_nanos, status,
      has_child_exited,
      kill_after_nanos != 0 ? wait_completion::ProcessGroupEmpty
                            : wait_completion::ChildExited);
  if (wait_result == supervision_wait_result::Interrupted)
    return finish_interrupted_supervision(child, process_group,
                                          has_child_exited);

  if (wait_result == supervision_wait_result::TimedOut) {
    if (!signal_supervised_process(child, process_group, has_child_exited, 9)) {
      if (!has_child_exited &&
          os::poll_process(child, status) == os::process_state::Exited)
      {
        has_child_exited = true;
        return timeout_expiration_status(status, timeout_signal);
      }
      return 125;
    }
    if (!has_child_exited) os::reap_process_quietly(child);
    return 137;
  }

  return timeout_expiration_status(status, timeout_signal);
}

} /* namespace koshkit */

} /* namespace koshka */
