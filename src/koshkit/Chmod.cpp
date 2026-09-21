/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements the chmod utility. It applies octal or symbolic modes to
 * files and recursively traverses directories when requested.
 */

#include "../CLI.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Path.hpp"
#include "../Platform.hpp"
#include "Mode.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-R] mode file ...");

HELP_DESCRIPTION_DECL("The chmod utility changes file permission modes.");

FLAG(CHMOD_RECURSIVE, Bool, 'R', "recursive",
     "Change directories and their contents recursively.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Chmod);

namespace koshka::koshkit {

static fn change_mode(const ExecContext &ec, EvalContext &cxt, const Path &path,
                      StringView expression, bool should_recurse,
                      const os::file_status *known_status = nullptr) throws
    -> bool
{
  os::file_status status{};
  if (known_status != nullptr) {
    status = *known_status;
  } else if (!os::stat_path_following(path.view(), status)) {
    report_soft_koshkit_util_error(ec, cxt, "chmod",
                                   "cannot access '" + path.text() +
                                       "': " + os::last_system_error_message());
    return false;
  }

  let const parsed =
      parse_file_mode(expression, status.mode, os::get_file_creation_mask(),
                      os::file_type_letter(status.mode) == 'd');
  ASSERT(parsed.has_value());

  bool did_succeed = true;
  if (!os::set_file_mode(path.view(), *parsed)) {
    report_soft_koshkit_util_error(ec, cxt, "chmod",
                                   "cannot change mode of '" + path.text() +
                                       "': " + os::last_system_error_message());
    did_succeed = false;
  }

  if (os::INTERRUPT_REQUESTED) return did_succeed;
  if (!should_recurse || os::file_type_letter(status.mode) != 'd')
    return did_succeed;

  let children =
      os::list_directory_status(path.view(), cxt.scratch_allocator());
  if (!children.has_value()) {
    report_soft_koshkit_util_error(ec, cxt, "chmod",
                                   "cannot read directory '" + path.text() +
                                       "': " + os::last_system_error_message());
    return false;
  }

  for (let const &child_entry : *children) {
    if (os::INTERRUPT_REQUESTED) return did_succeed;
    let const is_child_symlink =
        child_entry.child.kind == Path::entry_kind::Symlink ||
        (child_entry.has_status &&
         os::file_type_letter(child_entry.status.mode) == 'l');
    if (is_child_symlink) continue;

    let child = Path{path.view(), cxt.scratch_allocator()};
    child.append(child_entry.child.name.view());
    let const child_status =
        child_entry.has_status &&
                os::file_type_letter(child_entry.status.mode) != 'l'
            ? &child_entry.status
            : nullptr;
    if (!change_mode(ec, cxt, child, expression, true, child_status))
      did_succeed = false;
  }

  return did_succeed;
}

Chmod::Chmod() = default;

pure fn Chmod::kind() const wontthrow -> Utility::Kind { return Kind::Chmod; }

fn Chmod::execute(const ExecContext &ec, EvalContext &cxt,
                  const ArrayList<String> &args,
                  const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let operand_locations = ArrayList<SourceLocation>{cxt.scratch_allocator()};
  let const operands =
      PARSE_KOSHKIT_ARGS_WITH_LOCATIONS(args, arg_locations, operand_locations);

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  if (operands.count() < 2) return report_usage_error(ec, cxt, args[0].view());
  let const expression = operands[0].view();
  if (!parse_file_mode(expression, 0, 0, false).has_value()) {
    KOSHKIT_REPORT_ERROR_AT(
        operand_locations[0], "invalid mode '" + operands[0] + "'",
        "use one to four octal digits or symbolic clauses such as u+x,g-w");
    return 1;
  }

  i32 status = 0;

  for (usize index = 1; index < operands.count(); index++) {
    if (os::INTERRUPT_REQUESTED) return 130;
    if (!change_mode(ec, cxt, Path{operands[index].view()}, expression,
                     FLAG_CHMOD_RECURSIVE.is_enabled()))
      status = 1;
    if (os::INTERRUPT_REQUESTED) return 130;
  }

  return status;
}

} // namespace koshka::koshkit
