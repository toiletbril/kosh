/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements the chown utility. It resolves owner and group
 * identifiers and applies recursive ownership changes under the selected
 * symbolic-link traversal policy.
 */

#include "../CLI.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../UtilsOwnership.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-hR] [-H|-L|-P] owner[:group] file ...");

HELP_DESCRIPTION_DECL("The chown utility changes file owner and group.");

FLAG(CHOWN_NO_DEREFERENCE, Bool, 'h', "no-dereference",
     "Change a symbolic link instead of its target.");
FLAG(CHOWN_RECURSIVE, Bool, 'R', "recursive",
     "Change directories and their contents recursively.");
FLAG(CHOWN_COMMAND_LINE_FOLLOW, Bool, 'H', "dereference-arguments",
     "Follow symbolic links named on the command line during recursion.");
FLAG(CHOWN_FOLLOW, Bool, 'L', "dereference",
     "Follow every symbolic link during recursion.");
FLAG(CHOWN_PHYSICAL, Bool, 'P', "physical",
     "Do not follow symbolic links during recursion.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Chown);

namespace koshka::koshkit {

Chown::Chown() = default;

pure fn Chown::kind() const wontthrow -> Utility::Kind { return Kind::Chown; }

fn Chown::execute(const ExecContext &ec, EvalContext &cxt,
                  const ArrayList<String> &args,
                  const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let operand_locations = ArrayList<SourceLocation>{cxt.scratch_allocator()};
  let const operands =
      PARSE_KOSHKIT_ARGS_WITH_LOCATIONS(args, arg_locations, operand_locations);

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  if (operands.count() < 2) return report_usage_error(ec, cxt, args[0].view());
  let const specification = operands[0].view();
  let const colon = specification.find_character(':');
  let const owner_text = colon.has_value()
                             ? specification.substring_of_length(0, *colon)
                             : specification;
  let const group_text =
      colon.has_value() ? specification.substring(*colon + 1) : StringView{};
  if (owner_text.is_empty() && !colon.has_value()) {
    KOSHKIT_REPORT_ERROR_AT(operand_locations[0],
                            "invalid owner '" + operands[0] + "'",
                            "use an owner name or an unsigned decimal user id");
    return 1;
  }
  if (colon.has_value() && group_text.is_empty()) {
    KOSHKIT_REPORT_ERROR_AT(operand_locations[0],
                            "invalid specification '" + operands[0] + "'",
                            "use owner, owner:group, or :group");
    return 1;
  }

  i64 owner_id = -1;
  i64 group_id = -1;
  if (!owner_text.is_empty()) {
    let const resolved = utils::resolve_user_id(owner_text);
    if (!resolved.has_value()) {
      KOSHKIT_REPORT_ERROR_AT(
          operand_locations[0],
          "invalid owner '" + String{cxt.scratch_allocator(), owner_text} + "'",
          "use an owner name or an unsigned decimal user id");
      return 1;
    }
    owner_id = *resolved;
  }
  if (!group_text.is_empty()) {
    let const resolved = utils::resolve_group_id(group_text);
    if (!resolved.has_value()) {
      KOSHKIT_REPORT_ERROR_AT(
          operand_locations[0],
          "invalid group '" + String{cxt.scratch_allocator(), group_text} + "'",
          "use a group name or an unsigned decimal group id");
      return 1;
    }
    group_id = *resolved;
  }

  let const should_recurse = FLAG_CHOWN_RECURSIVE.is_enabled();
  let traversal_position = FLAG_CHOWN_COMMAND_LINE_FOLLOW.position();
  if (FLAG_CHOWN_FOLLOW.position() > traversal_position)
    traversal_position = FLAG_CHOWN_FOLLOW.position();
  if (FLAG_CHOWN_PHYSICAL.position() > traversal_position)
    traversal_position = FLAG_CHOWN_PHYSICAL.position();
  let const should_follow_nested =
      FLAG_CHOWN_FOLLOW.position() == traversal_position &&
      traversal_position != 0;
  let const should_follow_command_line =
      should_follow_nested ||
      (FLAG_CHOWN_COMMAND_LINE_FOLLOW.position() == traversal_position &&
       traversal_position != 0);
  let const should_follow_argument =
      !FLAG_CHOWN_NO_DEREFERENCE.is_enabled() &&
      (!should_recurse || should_follow_command_line);
  i32 status = 0;

  for (usize index = 1; index < operands.count(); index++)
    if (!utils::change_path_ownership(
            ec, cxt, "chown", Path{operands[index].view()}, owner_id, group_id,
            should_recurse, should_follow_argument, should_follow_nested))
      status = 1;

  return status;
}

} // namespace koshka::koshkit
