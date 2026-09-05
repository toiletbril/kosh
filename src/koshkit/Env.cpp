/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements the env utility. It installs temporary environment
 * assignments, prints the resulting environment, or resolves and executes a
 * command before restoring prior values.
 */

#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[NAME=value ...] [command [argument ...]]");

HELP_DESCRIPTION_DECL(
    "The env utility runs a command in a modified environment.");

FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Env);

namespace koshka {

namespace koshkit {

static fn is_assignment(StringView text) wontthrow -> bool
{
  let const equals = text.find_character('=');
  if (!equals.has_value() || *equals == 0) {
    return false;
  }

  for (usize i = 0; i < *equals; i++) {
    let const character = text[i];
    let const is_name = (character >= 'a' && character <= 'z') ||
                        (character >= 'A' && character <= 'Z') ||
                        (character >= '0' && character <= '9') ||
                        character == '_';
    if (!is_name) return false;
    if (i == 0 && character >= '0' && character <= '9') {
      return false;
    }
  }

  return true;
}

Env::Env() = default;

pure fn Env::kind() const wontthrow -> Utility::Kind { return Kind::Env; }

fn Env::execute(const ExecContext &ec, EvalContext &cxt,
                const ArrayList<String> &args,
                const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let operand_locations = ArrayList<SourceLocation>{cxt.scratch_allocator()};
  let const operands =
      parse_util_operands(FLAG_LIST, args, &arg_locations, &operand_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  ArrayList<String> saved_names{cxt.scratch_allocator()};
  ArrayList<String> saved_values{cxt.scratch_allocator()};
  ArrayList<bool> was_present{cxt.scratch_allocator()};
  usize first_command = 0;
  while (first_command < operands.count() &&
         is_assignment(operands[first_command].view()))
  {
    let const text = operands[first_command].view();
    let const equals = *text.find_character('=');
    let const name = text.substring_of_length(0, equals);
    let const value = text.substring(equals + 1);

    let const previous = os::get_environment_variable(name);
    saved_names.push(String{cxt.scratch_allocator(), name});
    saved_values.push(previous.has_value() ? previous->clone()
                                           : String{cxt.scratch_allocator()});
    was_present.push(previous.has_value());

    os::set_environment_variable(name, value);
    first_command++;
  }

  defer
  {
    for (usize i = saved_names.count(); i-- > 0;) {
      if (was_present[i])
        os::set_environment_variable(saved_names[i].view(),
                                     saved_values[i].view());
      else
        os::unset_environment_variable(saved_names[i].view());
    }
  };

  if (first_command >= operands.count()) {
    print_environment(ec, cxt);
    return 0;
  }

  let env_args = ArrayList<String>{cxt.scratch_allocator()};
  let env_arg_locations = ArrayList<SourceLocation>{cxt.scratch_allocator()};
  for (usize i = first_command; i < operands.count(); i++) {
    env_args.push_managed(operands[i]);
    env_arg_locations.push(operand_locations[i]);
  }

  let environment_resolver =
      ProgramResolver{os::get_environment_variable("PATH")};
  Maybe<ExecContext> sub;
  try {
    let const *source = cxt.current_source();
    sub = ExecContext::make_from(
        ec.source_location(), source != nullptr ? source->view() : StringView{},
        steal(env_args), cxt.mood(), cxt.koshkit(),
        cxt.is_shopt_enabled("checkhash"), environment_resolver,
        steal(env_arg_locations));
  } catch (const CommandResolutionErrorWithLocation &resolution_error) {
    const String *source = cxt.current_source();
    show_message(resolution_error.to_string(
        source != nullptr ? source->view() : StringView{}, &cxt));
    return static_cast<i32>(resolution_error.command_status());
  }

  let snapshot = cxt.snapshot_state();
  cxt.enter_subshell();
  i32 status = 0;
  try {
    status =
        utils::execute_context(steal(*sub), cxt, execution_mode::Foreground);
  } catch (...) {
    cxt.leave_subshell();
    cxt.restore_state(steal(snapshot));
    throw;
  }
  if (cxt.has_pending_control_flow()) {
    status = static_cast<i32>(cxt.pending_control_flow().value);
    cxt.clear_control_flow();
  }
  cxt.leave_subshell();
  cxt.restore_state(steal(snapshot));

  return status;
}

} /* namespace koshkit */

} /* namespace koshka */
