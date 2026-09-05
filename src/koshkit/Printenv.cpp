/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements the printenv utility in koshkit. It writes the whole
 * process environment or the values of named variables in operand order.
 */

#include "../Cli.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Platform.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[name ...]");

HELP_DESCRIPTION_DECL("The printenv utility writes environment variables.");

FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Printenv);

namespace koshka {

namespace koshkit {

Printenv::Printenv() = default;

pure fn Printenv::kind() const wontthrow -> Utility::Kind
{
  return Kind::Printenv;
}

fn Printenv::execute(
    const ExecContext &ec, EvalContext &cxt, const ArrayList<String> &args,
    const ArrayList<SourceLocation> &arg_locations) const throws -> i32
{
  let const operands = parse_util_operands(FLAG_LIST, args, &arg_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  if (operands.is_empty()) {
    print_environment(ec, cxt);
    return 0;
  }

  unused(cxt.materialize_kosh_identity());
  let output = String{cxt.scratch_allocator()};
  i32 status = 0;
  for (let const &name : operands) {
    let const value = os::get_environment_variable(name.view());
    if (!value.has_value()) {
      status = 1;
      continue;
    }

    output += value->view();
    output += '\n';
  }

  ec.print_to_stdout(output);
  return status;
}

} /* namespace koshkit */

} /* namespace koshka */
