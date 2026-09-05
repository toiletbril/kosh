/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements and is responsible for the exec builtin. The exec
 * builtin replaces the shell with the named command.
 */

#include "../Builtin.hpp"
#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Path.hpp"
#include "../Platform.hpp"
#include "../Trace.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[command [argument ...]]");

HELP_DESCRIPTION_DECL(
    "The exec builtin replaces the shell with the named command.");

FLAG(HELP, Bool, '\0', "help", "Display help.");
FLAG(EXEC_LOGIN, Bool, 'l', "",
     "Prefix the command's zeroth argument with a dash.");
FLAG(EXEC_ARGV0, String, 'a', "",
     "Pass the given name as the zeroth argument.");
FLAG(EXEC_CLEAR_ENV, Bool, 'c', "",
     "Run the command with an empty environment.");

REGISTER_BUILTIN_FLAGS(Exec);

namespace koshka {

Exec::Exec() = default;

pure fn Exec::kind() const wontthrow -> Builtin::Kind { return Kind::Exec; }

static fn report_exec_resolution_error(ExecContext &ec, EvalContext &cxt,
                                       SourceLocation location,
                                       StringView message,
                                       i32 command_status) throws -> i32
{
  let error = ErrorWithLocation{steal(location), message};
  error.set_command_status(command_status);
  const String *source = cxt.current_source();
  show_message(
      error.to_string(source != nullptr ? source->view() : StringView{}, &cxt));

  if (cxt.in_subshell() || cxt.is_in_pipeline_stage()) {
    if (cxt.in_subshell())
      cxt.request_exit(command_status, ec.source_location());
    return command_status;
  }

  if (cxt.shell_is_interactive()) return command_status;

  utils::quit(command_status, utils::farewell_policy::Goodbye);
}

fn Exec::execute(ExecContext &ec, EvalContext &cxt) const throws -> i32
{
  let const &args = ec.args();
  ASSERT(!args.is_empty());

  if (args.count() > 1 && args[1] == "--help") {
    SHOW_BUILTIN_HELP_AND_RETURN(ec);
  }

  let should_be_login_shell = false;
  let should_use_empty_environment = false;
  let has_custom_argv0 = false;
  let custom_argv0 = String{heap_allocator()};
  usize command_index = 1;

  if (!cxt.is_posix_mode()) {
    while (command_index < args.count()) {
      let const arg = args[command_index].view();
      if (arg == "--") {
        command_index++;
        break;
      }

      if (arg.length < 2 || arg[0] != '-') {
        break;
      }

      let did_consume_value_word = false;
      for (usize k = 1; k < arg.length; k++) {
        let const option = arg[k];
        if (option == 'l') {
          should_be_login_shell = true;
        } else if (option == 'c') {
          should_use_empty_environment = true;
        } else if (option == 'a') {
          if (k + 1 < arg.length) {
            has_custom_argv0 = true;
            custom_argv0 = arg.substring(k + 1);
          } else if (command_index + 1 < args.count()) {
            has_custom_argv0 = true;
            custom_argv0 = args[command_index + 1];
            did_consume_value_word = true;
          } else {
            report_soft_builtin_error(ec, cxt,
                                      ec.arg_location_at(command_index),
                                      "Option requires an argument -- a");
            return 2;
          }

          break;
        } else {
          let option_text = String{cxt.scratch_allocator()};
          option_text.push(option);
          report_soft_builtin_error(
              ec, cxt, ec.arg_location_at(command_index),
              StringView{"Invalid option -- "} + option_text,
              "Run 'exec --help' for the accepted options");
          return 2;
        }
      }

      command_index++;
      if (did_consume_value_word) command_index++;
    }
  }

  /* Inside an in-process subshell each touched descriptor is backed up first,
     so the change stays contained at the subshell's end. */
  if (command_index >= args.count()) {
    LOG(Debug, "exec applying redirections to the shell's own descriptors");
    if (ec.in_fd.has_value()) cxt.snapshot_subshell_descriptor(0);
    if (ec.out_fd.has_value()) cxt.snapshot_subshell_descriptor(1);
    if (ec.err_fd.has_value()) cxt.snapshot_subshell_descriptor(2);
    os::redirect_self(ec);
    return 0;
  }

  let const &command_name = args[command_index];
  if (cxt.restricted_enforcement_active())
    throw ErrorWithLocation{ec.arg_location_at(command_index),
                            "exec is forbidden in a restricted shell"};

  LOG(Info, "exec replacing the shell with '%s'", command_name.c_str());

  let program_path = Path{};
  if (os::has_directory_separator(command_name.view())) {
    let const typed_program_path = Path{command_name.view()};
    let resolved =
        typed_program_path.has_trailing_separator()
            ? os::canonical_path(
                  typed_program_path.to_absolute_without_normalizing())
            : Path::canonicalize(command_name);
    if (!resolved) {
      let error_location = ec.arg_location_at(command_index);
      let reported_program = command_name.view();
      let raw_program = command_name.view();
      if (let const *source = cxt.current_source(); source != nullptr)
        if (let source_text = error_location.get_source_text(source->view()))
          raw_program = *source_text;
      let const target = typed_program_path.to_absolute_without_normalizing();
      let const unavailable = utils::locate_first_unavailable_path_component(
          target, command_name.view(), raw_program, error_location,
          cxt.scratch_allocator());
      let is_missing_directory = false;
      if (unavailable.has_value()) {
        error_location = unavailable->location;
        reported_program = unavailable->reported_prefix.view();
        is_missing_directory = !unavailable->is_final_component;
      }

      if (is_missing_directory) {
        return report_exec_resolution_error(ec, cxt, error_location,
                                            StringView{"The directory '"} +
                                                reported_program +
                                                "' does not exist",
                                            127);
      }

      return report_exec_resolution_error(
          ec, cxt, error_location,
          StringView{"The command '"} + reported_program + "' was not found",
          127);
    }
    if (typed_program_path.has_trailing_separator() &&
        !resolved->is_directory())
    {
      return report_exec_resolution_error(ec, cxt,
                                          ec.arg_location_at(command_index),
                                          "This file is not a directory", 126);
    }
    program_path = resolved.take();
  } else {
    let const found = cxt.get_program_resolver().search(
        command_name, ProgramResolver::SearchMode::First,
        ProgramResolver::Requirement::Execution,
        ProgramResolver::CachePolicy::ReadOnly);
    if (found.count() == 0)
      return report_exec_resolution_error(
          ec, cxt, ec.arg_location_at(command_index),
          StringView{"The command '"} + command_name + "' was not found", 127);

    program_path = found[0];
  }

  let command_args = ArrayList<String>{heap_allocator()};
  let command_arg_locations = ArrayList<SourceLocation>{heap_allocator()};
  for (usize i = command_index; i < args.count(); i++) {
    command_args.push_managed(args[i]);
    command_arg_locations.push(ec.arg_location_at(i));
  }

  if (has_custom_argv0) command_args[0] = custom_argv0;

  if (should_be_login_shell) {
    let dashed_argv0 = StringView{"-"} + command_args[0];
    command_args[0] = steal(dashed_argv0);
  }

  let command = ExecContext::from_resolved(
      ec.source_location(), ResolvedCommand::from_program(program_path),
      steal(command_args), steal(command_arg_locations));
  if (ec.in_fd) command.in_fd = ec.in_fd.take();
  if (ec.out_fd) command.out_fd = ec.out_fd.take();
  if (ec.err_fd) command.err_fd = ec.err_fd.take();
  command.should_duplicate_error_to_output =
      ec.should_duplicate_error_to_output;
  command.should_duplicate_output_to_error =
      ec.should_duplicate_output_to_error;
  command.was_output_to_error_last = ec.was_output_to_error_last;
  command.should_use_empty_environment = should_use_empty_environment;
  command.should_use_fallback_argv0 = has_custom_argv0;

  /* Inside an in-process subshell, a command substitution, or a pipeline
     stage, the process the exec would replace is that inner scope, not the
     shell, so $(exec cat) and true | exec cat must not kill the session. The
     program runs as a spawned child and its status ends the scope, the way
     bash's forked subshell or stage dies into its exec. */
  if (cxt.in_subshell() || cxt.is_in_pipeline_stage()) {
    LOG(Info, "exec runs '%s' as a child rather than replacing the shell",
        command_name.c_str());
    let const status =
        utils::execute_context(steal(command), cxt, execution_mode::Foreground);
    if (cxt.in_subshell()) cxt.request_exit(status, ec.source_location());
    return status;
  }

  let saved_descriptors =
      ArrayList<os::saved_descriptor>{cxt.scratch_allocator()};
  defer
  {
    for (usize i = saved_descriptors.count(); i > 0; i--)
      os::restore_descriptor(saved_descriptors[i - 1]);
  };
  for (i32 shell_fd = 0; shell_fd < 3; shell_fd++) {
    let const saved_descriptor = os::save_descriptor(shell_fd);
    if (!saved_descriptor.is_dup2_ok) {
      return report_exec_resolution_error(
          ec, cxt, ec.arg_location_at(command_index),
          "Unable to preserve the shell's standard descriptors", 126);
    }
    saved_descriptors.push(saved_descriptor);
  }

  try {
    unused(cxt.materialize_kosh_identity());
    os::replace_process(steal(command));
  } catch (const ErrorBase &error) {
    return report_exec_resolution_error(
        ec, cxt, ec.arg_location_at(command_index), error.message(), 126);
  }

  command.in_fd.reset();
  command.out_fd.reset();
  command.err_fd.reset();
  let const status =
      cxt.run_program_fallback(command, cxt.mood(), script_isolation::Shared);
  utils::quit(status, utils::farewell_policy::Silent);
}

} /* namespace koshka */
