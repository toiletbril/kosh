/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements and is responsible for the logname utility in
 * koshkit. The logname utility writes the login name.
 */

#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("");

HELP_DESCRIPTION_DECL("The logname utility writes the login name.");

FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Logname);

namespace koshka::koshkit {

Logname::Logname() = default;

pure fn Logname::kind() const wontthrow -> Utility::Kind
{
  return Kind::Logname;
}

fn Logname::execute(const ExecContext &ec, EvalContext &cxt,
                    const ArrayList<String> &args,
                    const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
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
  let const name = os::get_login_user();
  if (!name.has_value()) {
    report_soft_koshkit_util_error(ec, cxt, args[0].view(),
                                   "login name is unavailable");
    return 1;
  }
  ec.print_to_stdout(name->view());
  ec.print_to_stdout("\n");
  return 0;
}

} // namespace koshka::koshkit
