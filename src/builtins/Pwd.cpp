#include "../Builtin.hpp"
#include "../Eval.hpp"
#include "../Path.hpp"
#include "../Trace.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("");
HELP_DESCRIPTION_DECL(
    "The pwd builtin prints the absolute path of the current working "
    "directory.");

FLAG(PWD_LOGICAL, Bool, 'L', "",
     "Print the logical directory, keeping the symlinks the path was reached "
     "through. This is the default.");
FLAG(PWD_PHYSICAL, Bool, 'P', "",
     "Print the physical directory, with every symlink resolved.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_BUILTIN_FLAGS(Pwd);

namespace koshka {

Pwd::Pwd() = default;

pure fn Pwd::kind() const wontthrow -> Builtin::Kind { return Kind::Pwd; }

fn Pwd::execute(ExecContext &ec, EvalContext &cxt) const throws -> i32
{
  let const args = PARSE_BUILTIN_ARGS(ec);

  if (FLAG_HELP.is_enabled()) SHOW_BUILTIN_HELP_AND_RETURN(ec);

  let output = String{cxt.scratch_allocator()};
  let const want_physical = FLAG_PWD_PHYSICAL.is_enabled();
  let const directory = want_physical ? Path::current_directory()
                                      : logical_working_directory(cxt);
  output.append(directory.text());
  output += '\n';
  ec.print_to_stdout(output);
  return 0;
}

} // namespace koshka
