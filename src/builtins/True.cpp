#include "../Builtin.hpp"
#include "../Eval.hpp"
#include "../Trace.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("");

HELP_DESCRIPTION_DECL(
    "The true builtin does nothing and always returns a success status.");

FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_BUILTIN_FLAGS(True);

namespace koshka {

True::True() = default;

pure fn True::kind() const wontthrow -> Builtin::Kind { return Kind::True; }

fn True::execute(ExecContext &ec, EvalContext &cxt) const throws -> i32
{
  unused(cxt);

  if (ec.args().count() > 1 && ec.args()[1] == "--help") {
    SHOW_BUILTIN_HELP_AND_RETURN(ec);
  }

  LOG(All, "true returning a success status");
  return 0;
}

} /* namespace koshka */
