/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements and is responsible for the which utility in koshkit.
 * The which utility prints how each named program resolves.
 */

#include "../Cli.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Path.hpp"
#include "../Platform.hpp"
#include "../Trace.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-aq] program [program ...]");

HELP_DESCRIPTION_DECL(
    "The which utility prints how each named program resolves.");

FLAG(ALL, Bool, 'a', "all", "Show all matches.");
FLAG(QUIET, Bool, 'q', "quiet", "Print nothing, only set the status.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Which);

namespace koshka {

namespace koshkit {

Which::Which() = default;

pure fn Which::kind() const wontthrow -> Utility::Kind { return Kind::Which; }

fn Which::execute(const ExecContext &ec, EvalContext &cxt,
                  const ArrayList<String> &args,
                  const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let const operands = parse_util_operands(FLAG_LIST, args, &arg_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  let const is_quiet = FLAG_QUIET.is_enabled();
  let output = String{cxt.scratch_allocator()};
  bool has_missing_any = false;

  for (let const &program_name : operands) {
    LOG(Debug, "which resolving '%s' against builtins and PATH",
        program_name.c_str());
    if (search_builtin(program_name.view()).has_value()) {
      if (!is_quiet) {
        output += program_name;
        if (os::is_stdout_a_tty()) output += ": Shell builtin";
        output += '\n';
      }
    } else if (let const paths = cxt.get_program_resolver().search(
                   program_name,
                   FLAG_ALL.is_enabled() ? ProgramResolver::SearchMode::All
                                         : ProgramResolver::SearchMode::First,
                   ProgramResolver::Requirement::Runnable,
                   ProgramResolver::CachePolicy::Bypass);
               paths.count() != 0)
    {
      if (!is_quiet) {
        for (let const &path : paths) {
          output += path.text();
          output += '\n';
        }
      }
    } else if ((cxt.koshkit() || cxt.mood() == mimic_mood::Default) &&
               find_util(program_name.view()).has_value())
    {
      if (!is_quiet) {
        output += program_name;
        output += '\n';
      }
    } else {
      has_missing_any = true;
    }
  }

  if (!is_quiet) ec.print_to_stdout(output);

  return has_missing_any ? 1 : 0;
}

} /* namespace koshkit */

} /* namespace koshka */
