/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements the link utility. It validates two operands and creates
 * one hard link through the platform filesystem interface.
 */

#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Platform.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("file1 file2");

HELP_DESCRIPTION_DECL("The link utility creates a hard link to a file.");

FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Link);

namespace koshka::koshkit {

Link::Link() = default;

pure fn Link::kind() const wontthrow -> Utility::Kind { return Kind::Link; }

fn Link::execute(const ExecContext &ec, EvalContext &cxt,
                 const ArrayList<String> &args,
                 const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let const operands = parse_util_operands(FLAG_LIST, args, &arg_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  if (operands.count() != 2) return report_usage_error(ec, cxt, args[0].view());
  if (os::create_hard_link(operands[0].view(), operands[1].view())) return 0;
  report_soft_koshkit_error(ec, cxt,
                            "link: cannot create '" + operands[1] +
                                "': " + os::last_system_error_message());
  return 1;
}

} // namespace koshka::koshkit
