/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements and is responsible for the nproc utility in koshkit.
 * The nproc utility prints the number of available logical processors.
 */

#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Platform.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[--all] [--ignore=count]");

HELP_DESCRIPTION_DECL(
    "The nproc utility prints the number of available logical processors.");

FLAG(NPROC_ALL, Bool, '\0', "all", "Print the configured processor count.");
FLAG(NPROC_IGNORE, String, '\0', "ignore",
     "Exclude up to this many processors from the result.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Nproc);

namespace koshka {

namespace koshkit {

Nproc::Nproc() = default;

pure fn Nproc::kind() const wontthrow -> Utility::Kind { return Kind::Nproc; }

cold fn Nproc::execute(
    const ExecContext &ec, EvalContext &cxt, const ArrayList<String> &args,
    const ArrayList<SourceLocation> &arg_locations) const throws -> i32
{
  let operand_locations = ArrayList<SourceLocation>{cxt.scratch_allocator()};
  let const operands =
      parse_util_operands(FLAG_LIST, args, &arg_locations, &operand_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  if (!operands.is_empty())
    throw ErrorWithLocation{operand_locations[0],
                            "nproc: unexpected operand '" + operands[0] + "'"};

  u64 ignored_count = 0;
  if (FLAG_NPROC_IGNORE.is_set()) {
    let const parsed = FLAG_NPROC_IGNORE.value().to<u64>();
    if (parsed.is_error())
      throw ErrorWithLocation{
          FLAG_NPROC_IGNORE.value_location(),
          "invalid number '" +
              String{cxt.scratch_allocator(), FLAG_NPROC_IGNORE.value()}
              + "'"
      };
    ignored_count = parsed.value();
  }

  let const counts = os::get_processor_counts();
  let const processor_count = FLAG_NPROC_ALL.is_enabled()
                                  ? counts.configured_count
                                  : counts.online_count;
  let const result_count =
      ignored_count >= processor_count
          ? usize{1}
          : processor_count - static_cast<usize>(ignored_count);
  let output = String::from(result_count, cxt.scratch_allocator());
  output += '\n';
  ec.print_to_stdout(output);
  return 0;
}

} /* namespace koshkit */

} /* namespace koshka */
