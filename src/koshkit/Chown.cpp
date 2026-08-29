#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "Ownership.hpp"

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
  let const operands = parse_util_operands(FLAG_LIST, args, &arg_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  if (operands.count() < 2) return report_usage_error(ec, cxt, args[0].view());
  let const specification = operands[0].view();
  let const colon = specification.find_character(':');
  let const owner_text = colon.has_value()
                             ? specification.substring_of_length(0, *colon)
                             : specification;
  let const group_text =
      colon.has_value() ? specification.substring(*colon + 1) : StringView{};
  if (owner_text.is_empty() && !colon.has_value())
    throw Error{"chown: invalid owner '" + operands[0] + "'"};
  if (colon.has_value() && group_text.is_empty())
    throw Error{"chown: invalid specification '" + operands[0] + "'"};

  i64 owner_id = -1;
  i64 group_id = -1;
  if (!owner_text.is_empty()) {
    let const resolved = resolve_user_id(owner_text);
    if (!resolved.has_value())
      throw Error{
          "chown: invalid owner '" +
          String{cxt.scratch_allocator(), owner_text}
          + "'"
      };
    owner_id = *resolved;
  }
  if (!group_text.is_empty()) {
    let const resolved = resolve_group_id(group_text);
    if (!resolved.has_value())
      throw Error{
          "chown: invalid group '" +
          String{cxt.scratch_allocator(), group_text}
          + "'"
      };
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
    if (!change_path_ownership(ec, cxt, "chown", Path{operands[index].view()},
                               owner_id, group_id, should_recurse,
                               should_follow_argument, should_follow_nested))
      status = 1;

  return status;
}

} // namespace koshka::koshkit
