/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements the chgrp utility. It resolves group identifiers and
 * applies recursive ownership changes under the selected symbolic-link
 * traversal policy.
 */

#include "../CLI.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../UtilsOwnership.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-hR] [-H|-L|-P] group file ...");

HELP_DESCRIPTION_DECL("The chgrp utility changes file group ownership.");

FLAG(CHGRP_NO_DEREFERENCE, Bool, 'h', "no-dereference",
     "Change a symbolic link instead of its target.");
FLAG(CHGRP_RECURSIVE, Bool, 'R', "recursive",
     "Change directories and their contents recursively.");
FLAG(CHGRP_COMMAND_LINE_FOLLOW, Bool, 'H', "dereference-arguments",
     "Follow symbolic links named on the command line during recursion.");
FLAG(CHGRP_FOLLOW, Bool, 'L', "dereference",
     "Follow every symbolic link during recursion.");
FLAG(CHGRP_PHYSICAL, Bool, 'P', "physical",
     "Do not follow symbolic links during recursion.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Chgrp);

namespace koshka::koshkit {

Chgrp::Chgrp() = default;

pure fn Chgrp::kind() const wontthrow -> Utility::Kind { return Kind::Chgrp; }

fn Chgrp::execute(const ExecContext &ec, EvalContext &cxt,
                  const ArrayList<String> &args,
                  const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let operand_locations = ArrayList<SourceLocation>{cxt.scratch_allocator()};
  let const operands =
      PARSE_KOSHKIT_ARGS_WITH_LOCATIONS(args, arg_locations, operand_locations);

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  if (operands.count() < 2) return report_usage_error(ec, cxt, args[0].view());
  let const group_id =
      utilsownership::resolve_group_id(operands[0].view());
  if (!group_id.has_value()) {
    KOSHKIT_REPORT_ERROR_AT(operand_locations[0],
                            "invalid group '" + operands[0] + "'",
                            "use a group name or an unsigned decimal group id");
    return 1;
  }

  i32 status = 0;

  for (usize index = 1; index < operands.count(); index++) {
    if (os::INTERRUPT_REQUESTED) return 130;
    if (!utilsownership::change_path_ownership(
            ec, cxt, "chgrp", Path{operands[index].view()}, -1, *group_id,
            FLAG_CHGRP_RECURSIVE.is_enabled(),
            FLAG_CHGRP_NO_DEREFERENCE.is_enabled(),
            FLAG_CHGRP_COMMAND_LINE_FOLLOW.position(),
            FLAG_CHGRP_FOLLOW.position(), FLAG_CHGRP_PHYSICAL.position()))
      status = 1;
    if (os::INTERRUPT_REQUESTED) return 130;
  }

  return status;
}

} // namespace koshka::koshkit
