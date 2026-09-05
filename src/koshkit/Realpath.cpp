/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements and is responsible for the realpath utility in
 * koshkit. The realpath utility prints the absolute, normalized form of each
 * path.
 */

#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Path.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("path ...");

HELP_DESCRIPTION_DECL(
    "The realpath utility prints the absolute, normalized form of each path.");

FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Realpath);

namespace koshka {

namespace koshkit {

Realpath::Realpath() = default;

pure fn Realpath::kind() const wontthrow -> Utility::Kind
{
  return Kind::Realpath;
}

cold fn Realpath::execute(
    const ExecContext &ec, EvalContext &cxt, const ArrayList<String> &args,
    const ArrayList<SourceLocation> &arg_locations) const throws -> i32
{
  let const operands = parse_util_operands(FLAG_LIST, args, &arg_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  if (operands.is_empty()) return report_usage_error(ec, cxt, args[0].view());

  let output = String{cxt.scratch_allocator()};
  i32 status = 0;
  for (const String &operand : operands) {
    let const resolved = os::canonical_path(Path{operand.view()});
    if (!resolved) {
      report_soft_koshkit_error(ec, cxt,
                                "realpath: '" + operand +
                                    "': " + os::last_system_error_message());
      status = 1;
      continue;
    }

    output += resolved->text().view();
    output += '\n';
  }
  ec.print_to_stdout(output);
  return status;
}

} /* namespace koshkit */

} /* namespace koshka */
