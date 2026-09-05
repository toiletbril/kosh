/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements and is responsible for the id utility in koshkit. The
 * id utility writes user and group identities.
 */

#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "Ownership.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-Ggu] [-nr] [user]");

HELP_DESCRIPTION_DECL("The id utility writes user and group identities.");

FLAG(ID_GROUPS, Bool, 'G', "groups", "Write all group identities.");
FLAG(ID_GROUP, Bool, 'g', "group", "Write the group identity.");
FLAG(ID_NAME, Bool, 'n', "name", "Write names instead of numbers.");
FLAG(ID_REAL, Bool, 'r', "real", "Use real identities.");
FLAG(ID_USER, Bool, 'u', "user", "Write the user identity.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Id);

namespace koshka::koshkit {

static fn id_user_text(u32 id, bool should_use_name, Allocator allocator) throws
    -> String
{
  if (should_use_name)
    if (let const name = os::uid_to_username(id); name.has_value())
      return *name;
  return String::from(id, allocator);
}

static fn id_group_text(u32 id, bool should_use_name,
                        Allocator allocator) throws -> String
{
  if (should_use_name)
    if (let const name = os::gid_to_groupname(id); name.has_value())
      return *name;
  return String::from(id, allocator);
}

Id::Id() = default;

pure fn Id::kind() const wontthrow -> Utility::Kind { return Kind::Id; }

fn Id::execute(const ExecContext &ec, EvalContext &cxt,
               const ArrayList<String> &args,
               const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let operand_locations = ArrayList<SourceLocation>{cxt.scratch_allocator()};
  let const operands =
      parse_util_operands(FLAG_LIST, args, &arg_locations, &operand_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  if (operands.count() > 1) return report_usage_error(ec, cxt, args[0].view());
  let const selector_count = static_cast<usize>(FLAG_ID_GROUPS.is_enabled()) +
                             static_cast<usize>(FLAG_ID_GROUP.is_enabled()) +
                             static_cast<usize>(FLAG_ID_USER.is_enabled());
  if (selector_count > 1 || (FLAG_ID_NAME.is_enabled() && selector_count == 0))
    return report_usage_error(ec, cxt, args[0].view());

  u32 user_id =
      static_cast<u32>(FLAG_ID_REAL.is_enabled() ? os::get_real_user_id()
                                                 : os::get_effective_user_id());
  u32 group_id = static_cast<u32>(FLAG_ID_REAL.is_enabled()
                                      ? os::get_real_group_id()
                                      : os::get_effective_group_id());
  if (!operands.is_empty()) {
    let const resolved = resolve_user_id(operands[0].view());
    if (!resolved.has_value()) {
      report_soft_koshkit_util_error(ec, cxt, operand_locations[0],
                                     args[0].view(),
                                     "unknown user '" + operands[0] + "'");
      return 1;
    }
    user_id = *resolved;
  }

  if (FLAG_ID_USER.is_enabled()) {
    ec.print_to_stdout(id_user_text(user_id, FLAG_ID_NAME.is_enabled(),
                                    cxt.scratch_allocator()) +
                       "\n");
    return 0;
  }
  if (FLAG_ID_GROUP.is_enabled()) {
    ec.print_to_stdout(id_group_text(group_id, FLAG_ID_NAME.is_enabled(),
                                     cxt.scratch_allocator()) +
                       "\n");
    return 0;
  }

  let groups = os::get_supplementary_group_ids(cxt.scratch_allocator());
  if (FLAG_ID_GROUPS.is_enabled()) {
    let output = String{cxt.scratch_allocator()};
    for (usize index = 0; index < groups.count(); index++) {
      if (index != 0) output += ' ';
      output += id_group_text(groups[index], FLAG_ID_NAME.is_enabled(),
                              cxt.scratch_allocator());
    }
    output += '\n';
    ec.print_to_stdout(output);
    return 0;
  }

  let output = String{cxt.scratch_allocator()};
  output += "uid=";
  output += String::from(user_id, cxt.scratch_allocator());
  if (let const name = os::uid_to_username(user_id); name.has_value()) {
    output += '(';
    output += name->view();
    output += ')';
  }
  output += " gid=";
  output += String::from(group_id, cxt.scratch_allocator());
  if (let const name = os::gid_to_groupname(group_id); name.has_value()) {
    output += '(';
    output += name->view();
    output += ')';
  }
  output += " groups=";
  for (usize index = 0; index < groups.count(); index++) {
    if (index != 0) output += ',';
    output += String::from(groups[index], cxt.scratch_allocator());
    if (let const name = os::gid_to_groupname(groups[index]); name.has_value())
    {
      output += '(';
      output += name->view();
      output += ')';
    }
  }
  output += '\n';
  ec.print_to_stdout(output);
  return 0;
}

} // namespace koshka::koshkit
