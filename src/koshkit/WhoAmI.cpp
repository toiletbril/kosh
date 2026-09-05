/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements the whoami utility. It resolves the effective user
 * identifier through the platform account interface and prints the
 * corresponding user name.
 */

#include "../Cli.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Platform.hpp"
#include "../Trace.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("");

HELP_DESCRIPTION_DECL(
    "The whoami utility prints the name of the effective user.");

FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(WhoAmI);

namespace koshka {

namespace koshkit {

WhoAmI::WhoAmI() = default;

pure fn WhoAmI::kind() const wontthrow -> Utility::Kind { return Kind::WhoAmI; }

cold fn WhoAmI::execute(
    const ExecContext &ec, EvalContext &cxt, const ArrayList<String> &args,
    const ArrayList<SourceLocation> &arg_locations) const throws -> i32
{
  let operand_locations = ArrayList<SourceLocation>{cxt.scratch_allocator()};
  let const operands =
      parse_util_operands(FLAG_LIST, args, &arg_locations, &operand_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  if (!operands.is_empty()) {
    report_soft_koshkit_util_error(ec, cxt, operand_locations[0],
                                   args[0].view(),
                                   "unexpected operand '" + operands[0] + "'");
    return 2;
  }

  LOG(Debug, "whoami printing the effective user name");

  let output = String{cxt.scratch_allocator()};

  if (let const user =
          os::uid_to_username(static_cast<u32>(os::get_effective_user_id()));
      user.has_value())
  {
    output.append(user->view());
    output += '\n';
    ec.print_to_stdout(output);
    return 0;
  }

  report_soft_koshkit_util_error(ec, cxt, args[0].view(),
                                 "cannot determine the effective user: " +
                                     os::last_system_error_message());
  return 1;
}

} /* namespace koshkit */

} /* namespace koshka */
