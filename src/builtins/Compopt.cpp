/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements and is responsible for the compopt builtin. The
 * compopt builtin accepts completion option changes with no effect.
 */

#include "../Builtin.hpp"
#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Trace.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-o option] [-DEI] [+o option] [name ...]");
HELP_DESCRIPTION_DECL(
    "The compopt builtin accepts completion option changes with no effect.");

FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_BUILTIN_FLAGS(Compopt);

namespace koshka {

Compopt::Compopt() = default;

pure fn Compopt::kind() const wontthrow -> Builtin::Kind
{
  return Kind::Compopt;
}

fn Compopt::execute(ExecContext &ec, EvalContext &cxt) const throws -> i32
{
  unused(cxt);
  let const &args = ec.args();
  ASSERT(!args.is_empty());

  if (args.count() > 1 && args[1] == "--help") {
    SHOW_BUILTIN_HELP_AND_RETURN(ec);
  }

  LOG(Debug, "compopt accepting %zu arguments without effect",
      args.count() - 1);
  return 0;
}

} /* namespace koshka */
