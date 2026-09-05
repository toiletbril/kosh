/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements and is responsible for the unlink utility in koshkit.
 * The unlink utility removes the single named file.
 */

#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Path.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("file");

HELP_DESCRIPTION_DECL("The unlink utility removes the single named file.");

FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Unlink);

namespace koshka {

namespace koshkit {

Unlink::Unlink() = default;

pure fn Unlink::kind() const wontthrow -> Utility::Kind { return Kind::Unlink; }

cold fn Unlink::execute(
    const ExecContext &ec, EvalContext &cxt, const ArrayList<String> &args,
    const ArrayList<SourceLocation> &arg_locations) const throws -> i32
{
  let const operands = parse_util_operands(FLAG_LIST, args, &arg_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  if (operands.is_empty()) return report_usage_error(ec, cxt, args[0].view());
  if (operands.count() > 1) {
    report_soft_koshkit_error(ec, cxt,
                              "unlink: extra operand '" + operands[1] + "'");
    return 1;
  }

  /* unlink(2) removes the link itself, so a symlink to a directory passes and
     only a real directory is refused. */
  let const &target = operands[0];
  let const target_path = Path{target.view()};
  if (target_path.is_directory() && !target_path.is_symbolic_link()) {
    report_soft_koshkit_error(
        ec, cxt, "unlink: cannot unlink '" + target + "': it is a directory");
    return 1;
  }

  if (!remove_path(target.view(), removal_mode::SinglePath)) {
    report_soft_koshkit_error(ec, cxt,
                              "unlink: cannot unlink '" + target +
                                  "': " + os::last_system_error_message());
    return 1;
  }
  return 0;
}

} /* namespace koshkit */

} /* namespace koshka */
