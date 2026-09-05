/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements and is responsible for the cd builtin. The cd builtin
 * changes the working directory.
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

HELP_SYNOPSIS_DECL("[dir]");

HELP_DESCRIPTION_DECL("The cd builtin changes the working directory.");

FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_BUILTIN_FLAGS(Cd);

namespace koshka {

Cd::Cd() = default;

pure fn Cd::kind() const wontthrow -> Builtin::Kind { return Kind::Cd; }

/* An absolute operand, or one led by dot or dot-dot, skips the CDPATH search.
 */
static fn cdpath_search_applies(const String &operand) throws -> bool
{
  if (operand.is_empty() || os::path_is_absolute(operand.view()) ||
      os::path_is_drive_relative(operand.view()))
  {
    return false;
  }
  if (operand == "." || operand == "..") {
    return false;
  }
  if (operand.length() >= 2 && operand[0] == '.' &&
      os::is_directory_separator(operand[1]))
  {
    return false;
  }
  if (operand.length() >= 3 && operand[0] == '.' && operand[1] == '.' &&
      os::is_directory_separator(operand[2]))
  {
    return false;
  }
  return true;
}

fn Cd::execute(ExecContext &ec, EvalContext &cxt) const throws -> i32
{
  ASSERT(!ec.args().is_empty());

  if (ec.args().count() > 1 && ec.args()[1] == "--help") {
    SHOW_BUILTIN_HELP_AND_RETURN(ec);
  }
  if (cxt.restricted_enforcement_active())
    throw ErrorWithLocation{ec.source_location(),
                            "cd is forbidden in a restricted shell"};

  let is_physical =
      !cxt.is_posix_mode() && cxt.shell_option_state(shell_option_id::Physical);
  usize operand_index = 1;
  while (operand_index < ec.args().count()) {
    let const option = ec.args()[operand_index].view();
    if (option == "--") {
      operand_index++;
      break;
    }

    if (option.length < 2 || option[0] != '-') {
      break;
    }
    let is_options = true;
    for (usize k = 1; k < option.length; k++)
      if (option[k] != 'L' && option[k] != 'P') {
        is_options = false;
        break;
      }
    if (!is_options) break;

    is_physical = option[option.length - 1] == 'P';
    operand_index++;
  }

  let const operand_count = ec.args().count() - operand_index;

  if (operand_count > 1) {
    let too_many = ErrorWithLocationAndDetails{
        ec.source_location(), "cd accepts only a single directory operand",
        "Quote a path that contains spaces"};
    too_many.set_command_status(2);
    throw too_many;
  }

  let arg_path = String{cxt.scratch_allocator()};

  let const is_to_previous =
      operand_count == 1 && ec.args()[operand_index] == "-";

  if (is_to_previous) {
    let const old_directory = cxt.get_variable_value("OLDPWD");
    if (!old_directory.has_value() || old_directory->is_empty()) {
      throw ErrorWithLocationAndDetails{
          ec.source_location(),
          "Unable to return to the previous directory because OLDPWD is not "
          "set",
          "Change directory at least once before `cd -`"};
    }
    arg_path.append(old_directory->view());
  } else if (operand_count > 0) {
    arg_path.append(ec.args()[operand_index]);
  } else {
    let const home_directory = os::get_home_directory();
    if (!home_directory.has_value())
      throw ErrorWithLocationAndDetails{
          ec.source_location(), "Unable to determine the home directory",
          "Set `HOME` to a valid path"};
    arg_path.append(home_directory->text());
  }

  LOG(Info, "cd changing directory to '%s'", arg_path.c_str());

  let target = Path{arg_path};
  let old_directory = Path{};
  let is_logical_target_available = true;
  let const do_resolve_logical_target = [](const Path &raw_target,
                                           bool &did_fail_before_dotdot) {
    let logical_candidate = Path{raw_target.text().view().substring_of_length(
        0, os::path_root_length(raw_target.text().view()))};
    usize component_position = os::path_root_length(raw_target.text().view());
    while (component_position < raw_target.count()) {
      while (component_position < raw_target.count() &&
             os::is_directory_separator(raw_target.text()[component_position]))
        component_position++;
      if (component_position >= raw_target.count()) break;
      let const component_start = component_position;
      while (component_position < raw_target.count() &&
             !os::is_directory_separator(raw_target.text()[component_position]))
        component_position++;
      let const component = raw_target.text().view().substring_of_length(
          component_start, component_position - component_start);
      if (component == StringView{"."}) continue;
      if (component == StringView{".."}) {
        if (!logical_candidate.is_directory()) {
          did_fail_before_dotdot = true;
          return raw_target.clone();
        }
        logical_candidate.push_component(component);
        logical_candidate = logical_candidate.normalized();
        continue;
      }
      logical_candidate.push_component(component);
    }
    return logical_candidate;
  };

  /* An empty CDPATH entry, including one a leading, trailing, or doubled colon
     makes, names the current directory. */
  let should_print_target = is_to_previous;
  if (!is_to_previous && operand_count > 0 && cdpath_search_applies(arg_path)) {
    if (let const cdpath = cxt.get_variable_value("CDPATH")) {
      let const entries = cdpath->view();
      usize start = 0;
      while (start <= entries.length) {
        usize end = start;
        while (end < entries.length && entries.data[end] != os::PATH_DELIMITER)
          end++;
        let const entry = entries.substring_of_length(start, end - start);
        let candidate = entry.is_empty()
                            ? Path{arg_path}
                            : Path{entry}.push_component(arg_path.view());
        let resolved = candidate;
        let is_candidate_available = true;
        if (is_physical) {
          if (resolved.is_relative()) {
            let current_directory = Path::current_directory();
            if (current_directory.is_empty()) break;
            resolved = current_directory.push_component(resolved.text().view());
          }
          if (let canonical = os::canonical_path(resolved)) {
            resolved = canonical.take();
          } else {
            is_candidate_available = false;
          }
        } else {
          resolved = resolved.to_absolute_without_normalizing();
          let did_fail_before_dotdot = false;
          resolved =
              do_resolve_logical_target(resolved, did_fail_before_dotdot);
          is_candidate_available = !did_fail_before_dotdot;
        }
        if (is_candidate_available && resolved.is_directory()) {
          LOG(Info, "cd resolved '%s' through CDPATH entry '%.*s'",
              arg_path.c_str(), static_cast<int>(entry.length), entry.data);
          target = steal(resolved);
          should_print_target = !entry.is_empty();
          break;
        }
        if (end >= entries.length) break;
        start = end + 1;
      }
    }
  }

  if (!is_to_previous && operand_count > 0 && !target.is_directory() &&
      cxt.is_shopt_enabled("cdable_vars"))
  {
    if (let const directory_variable = cxt.get_variable_value(arg_path)) {
      arg_path.clear();
      arg_path.append(directory_variable->view());
      target = Path{arg_path};
      should_print_target = true;
    }
  }

  /* A relative operand joins onto the logical PWD when that names a directory,
     the bash -L default, so cd .. out of a symlinked directory returns to the
     symlink's parent. */
  let is_physical_target_available = true;
  if (is_physical) {
    if (target.is_relative()) {
      target = target.to_absolute_without_normalizing();
      if (!target.is_absolute())
        throw ErrorWithLocation{
            ec.source_location(),
            StringView{"Unable to resolve '"} + arg_path +
                "' because the current directory is unavailable"};
    }

    if (let resolved = os::canonical_path(target)) {
      target = resolved.take();
    } else {
      is_physical_target_available = false;
    }
  } else {
    let raw_logical_target = Path{};
    let logical_operand = target.text().view();
    if (target.is_absolute() ||
        os::path_is_drive_relative(target.text().view()))
    {
      raw_logical_target = target.to_absolute_without_normalizing();
    } else {
      old_directory = logical_working_directory(cxt);
      raw_logical_target = Path{old_directory.text().view()};
      raw_logical_target.push_component(logical_operand);
    }

    if (!raw_logical_target.is_absolute())
      throw ErrorWithLocation{
          ec.source_location(),
          StringView{"Unable to resolve '"} + arg_path +
              "' because the current directory is unavailable"};

    let did_fail_before_dotdot = false;
    let logical_candidate =
        do_resolve_logical_target(raw_logical_target, did_fail_before_dotdot);
    is_logical_target_available =
        !did_fail_before_dotdot && logical_candidate.is_directory();
    target = is_logical_target_available ? logical_candidate.normalized()
             : did_fail_before_dotdot    ? steal(raw_logical_target)
                                         : steal(logical_candidate);
  }

  if (is_logical_target_available && is_physical_target_available &&
      target.exists())
  {
    if (!target.is_directory())
      throw ErrorWithLocation{ec.arg_location_at(operand_index),
                              StringView{"The path '"} + arg_path +
                                  "' is not a directory"};

    if (!is_physical) target = target.normalized();

    if (old_directory.is_empty())
      old_directory = logical_working_directory(cxt);
    /* A path that exists can still refuse the move, so the chdir failure throws
       before PWD and OLDPWD are rewritten, leaving them untouched like dash. */
    if (Path::set_current_directory(target).is_error()) {
      throw ErrorWithLocation{
          ec.source_location(),
          StringView{"Unable to change to the directory '"} + arg_path +
              "': " + os::last_system_error_message()};
    }
    /* A relative or empty PATH entry now names a different directory, so a
       cached resolution is marked stale for the next command to re-resolve. */
    cxt.get_program_resolver().working_directory_changed();
    if (!old_directory.is_empty())
      cxt.set_shell_variable("OLDPWD", old_directory.text());
    cxt.set_shell_variable("PWD", target.text());
    record_directory_access(target.text().view(), cxt.scratch_allocator());
    if (should_print_target) {
      ec.print_to_stdout(target.text() + "\n");
    }
    return 0;
  }

  let operand_location = ec.arg_location_at(operand_index);
  let raw_operand = arg_path.view();
  if (operand_index < ec.arg_locations().count()) {
    if (let const source = cxt.current_source(); source != nullptr)
      if (let source_text = operand_location.get_source_text(source->view()))
        raw_operand = *source_text;
  }

  let const unavailable = utils::locate_first_unavailable_path_component(
      target, arg_path.view(), raw_operand, operand_location,
      cxt.scratch_allocator());
  if (!unavailable.has_value() || is_to_previous || operand_count == 0) {
    throw ErrorWithLocationAndDetails{
        ec.source_location(),
        StringView{"The directory '"} + arg_path + "' does not exist",
        "Check the spelling or create it with `mkdir -p`"};
  }

  if (unavailable->is_not_directory) {
    throw ErrorWithLocation{unavailable->location,
                            StringView{"The path '"} +
                                unavailable->reported_prefix +
                                "' is not a directory"};
  }

  let details = String{cxt.scratch_allocator(),
                       "Check the spelling or create it with `mkdir -p`"};
  if (unavailable->has_single_raw_component) {
    if (let const suggested_name = utils::suggest_directory_entry(
            unavailable->prefix.parent(), unavailable->prefix.filename()))
    {
      let suggested_path = String{cxt.scratch_allocator()};
      suggested_path.append(
          unavailable->typed_prefix.view().substring_of_length(
              0, unavailable->typed_component_start));
      suggested_path.append(suggested_name->view());
      let quoted_path = String{cxt.scratch_allocator()};
      append_shell_quoted_arg(quoted_path, suggested_path.view());
      details = "Did you mean `" + quoted_path + "`?";
    }
  }

  throw ErrorWithLocationAndDetails{unavailable->location,
                                    StringView{"The directory '"} +
                                        unavailable->reported_prefix +
                                        "' does not exist",
                                    details};
}

} /* namespace koshka */
