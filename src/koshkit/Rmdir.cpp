/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements the rmdir utility. It removes empty operand directories
 * and can continue upward through empty parent directories.
 */

#include "../CLI.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-p] directory ...");

HELP_DESCRIPTION_DECL("The rmdir utility removes each empty directory.");

FLAG(RMDIR_PARENTS, Bool, 'p', "", "Remove empty parent directories.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Rmdir);

namespace koshka {

namespace koshkit {

Rmdir::Rmdir() = default;

pure fn Rmdir::kind() const wontthrow -> Utility::Kind { return Kind::Rmdir; }

fn Rmdir::execute(const ExecContext &ec, EvalContext &cxt,
                  const ArrayList<String> &args,
                  const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let operand_locations = ArrayList<SourceLocation>{cxt.scratch_allocator()};
  let const operands =
      PARSE_KOSHKIT_ARGS_WITH_LOCATIONS(args, arg_locations, operand_locations);

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  if (operands.is_empty()) return report_usage_error(ec, cxt, args[0].view());

  i32 status = 0;
  for (usize operand_index = 0; operand_index < operands.count();
       operand_index++)
  {
    let const &operand = operands[operand_index];
    if (!os::remove_directory(operand.view())) {
      KOSHKIT_REPORT_ERROR_AT(operand_locations[operand_index],
                              "failed to remove '" + operand +
                                  "': " + os::last_system_error_message());
      status = 1;
      continue;
    }

    if (FLAG_RMDIR_PARENTS.is_enabled()) {
      let current = Path{operand.view()};
      loop
      {
        let parent = current.parent();
        if (parent.is_empty() || parent == current) break;
        if (!os::remove_directory(parent.view())) {
          KOSHKIT_REPORT_ERROR_AT(operand_locations[operand_index],
                                  "failed to remove '" + parent.text() +
                                      "': " + os::last_system_error_message());
          status = 1;
          break;
        }
        current = steal(parent);
      }
    }
  }
  return status;
}

} /* namespace koshkit */

} /* namespace koshka */
