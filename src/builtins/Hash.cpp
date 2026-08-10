#include "../Builtin.hpp"
#include "../Cli.hpp"
#include "../Eval.hpp"
#include "../Trace.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-r] [-p pathname] [name ...]");
HELP_DESCRIPTION_DECL(
    "The hash builtin manages the cache of resolved command locations.");

FLAG(RESET, Bool, 'r', "", "Forget remembered command locations.");
FLAG(PATHNAME, String, 'p', "", "Remember each name at pathname.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_BUILTIN_FLAGS(Hash);

namespace koshka {

Hash::Hash() = default;

pure fn Hash::kind() const wontthrow -> Builtin::Kind { return Kind::Hash; }

fn Hash::execute(ExecContext &ec, EvalContext &cxt) const throws -> i32
{
  let const args = PARSE_BUILTIN_ARGS(ec);

  if (FLAG_HELP.is_enabled()) SHOW_BUILTIN_HELP_AND_RETURN(ec);

  if (FLAG_RESET.is_enabled()) {
    LOG(Info, "hash forgetting every remembered command location");
    cxt.get_program_resolver().invalidate();
  }

  if (FLAG_PATHNAME.is_set()) {
    cxt.guard_restricted_path(FLAG_PATHNAME.value(),
                              FLAG_PATHNAME.value_location(),
                              restricted_path_use::Hash);
    for (usize i = 1; i < args.count(); i++)
      cxt.get_program_resolver().remember_path(args[i].view(),
                                               Path{FLAG_PATHNAME.value()});
    return 0;
  }

  i32 status = 0;
  for (usize i = 1; i < args.count(); i++) {
    let const &name = args[i];

    LOG(Debug, "hash resolving '%s' to remember its location", name.c_str());

    if (os::has_directory_separator(name.view())) continue;

    if (cxt.get_program_resolver()
            .search(name, ProgramResolver::SearchMode::First,
                    ProgramResolver::Requirement::Runnable,
                    ProgramResolver::CachePolicy::Remember)
            .count() == 0)
    {
      report_soft_builtin_error(ec, cxt,
                                "The command '" + name + "' was not found");
      status = 1;
    }
  }

  return status;
}

} // namespace koshka
