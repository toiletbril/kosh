#include "../Builtin.hpp"
#include "../Eval.hpp"
#include "../Trace.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-clpv]");
HELP_DESCRIPTION_DECL(
    "The dirs builtin prints the directory stack, the current directory first, "
    "then the saved directories from the top down.");

FLAG(DIRS_CLEAR, Bool, 'c', "", "Clear the directory stack.");
FLAG(DIRS_LONG, Bool, 'l', "",
     "Print full paths rather than abbreviating the home directory to ~.");
FLAG(DIRS_PER_LINE, Bool, 'p', "", "Print one entry per line.");
FLAG(DIRS_NUMBERED, Bool, 'v', "",
     "Print one entry per line, each numbered from the top.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_BUILTIN_FLAGS(Dirs);

namespace koshka {

Dirs::Dirs() = default;

pure fn Dirs::kind() const wontthrow -> Builtin::Kind { return Kind::Dirs; }

fn Dirs::execute(ExecContext &ec, EvalContext &cxt) const throws -> i32
{
  let const args = PARSE_BUILTIN_ARGS(ec);
  unused(args);

  if (FLAG_HELP.is_enabled()) SHOW_BUILTIN_HELP_AND_RETURN(ec);

  if (FLAG_DIRS_CLEAR.is_enabled()) {
    LOG(Debug, "dirs clearing the directory stack");
    cxt.directory_stack().clear();
    return 0;
  }

  print_directory_stack(cxt, ec, FLAG_DIRS_PER_LINE.is_enabled(),
                        FLAG_DIRS_NUMBERED.is_enabled(),
                        FLAG_DIRS_LONG.is_enabled());
  return 0;
}

} // namespace koshka
