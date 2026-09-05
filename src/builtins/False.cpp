/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements and is responsible for the false builtin. The false
 * builtin exits with a failure status.
 */

#include "../Builtin.hpp"
#include "../Eval.hpp"
#include "../Trace.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("");
HELP_DESCRIPTION_DECL("The false builtin exits with a failure status.");

FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_BUILTIN_FLAGS(False);

namespace koshka {

False::False() = default;

pure fn False::kind() const wontthrow -> Builtin::Kind { return Kind::False; }

fn False::execute(ExecContext &ec, EvalContext &cxt) const throws -> i32
{
  unused(ec);
  unused(cxt);

  LOG(All, "false returning a failure status");
  return 1;
}

} /* namespace koshka */
