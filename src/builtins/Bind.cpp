#include "../Builtin.hpp"
#include "../Cli.hpp"
#include "../Eval.hpp"
#include "../Trace.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-lmrvpPsSq] [-f filename] [-q function] [-u function] "
                   "[keyseq: function ...]");

HELP_DESCRIPTION_DECL(
    "The bind builtin accepts readline key bindings without effect. The "
    "line editor is the vendored toiletline, so key binding is not wired "
    "through this builtin.");

FLAG(LIST_NAMES, Bool, 'l', "\0", "Accepted without effect.");
FLAG(READLINE_META, Bool, 'm', "\0", "Accepted without effect.");
FLAG(REMOVE, Bool, 'r', "\0", "Accepted without effect.");
FLAG(PRINT, Bool, 'p', "\0", "Accepted without effect.");
FLAG(PRINT_ALL, Bool, 'P', "\0", "Accepted without effect.");
FLAG(QUERY, Bool, 'q', "\0", "Accepted without effect.");
FLAG(SHOW_HELP, Bool, 's', "\0", "Accepted without effect.");
FLAG(VARIABLE, Bool, 'v', "\0", "Accepted without effect.");
FLAG(FILENAME, String, 'f', "\0", "Accepted without effect.");
FLAG(UNBIND, String, 'u', "\0", "Accepted without effect.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_BUILTIN_FLAGS(Bind);

namespace koshka {

Bind::Bind() = default;

pure fn Bind::kind() const wontthrow -> Builtin::Kind { return Kind::Bind; }

fn Bind::execute(ExecContext &ec, EvalContext &cxt) const throws -> i32
{
  let operand_locations = ArrayList<SourceLocation>{cxt.scratch_allocator()};
  PARSE_BUILTIN_ARGS_WITH_LOCATIONS(ec, operand_locations);

  if (FLAG_HELP.is_enabled()) SHOW_BUILTIN_HELP_AND_RETURN(ec);

  LOG(All, "bind accepted as a no-op with %zu operands", ec.args().count() - 1);
  return 0;
}

} /* namespace koshka */
