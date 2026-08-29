#include "../Cli.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Platform.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-n] file");

HELP_DESCRIPTION_DECL(
    "The readlink utility prints the target of a symbolic link.");

FLAG(READLINK_NO_NEWLINE, Bool, 'n', "", "Do not print a trailing newline.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Readlink);

namespace koshka {

namespace koshkit {

Readlink::Readlink() = default;

pure fn Readlink::kind() const wontthrow -> Utility::Kind
{
  return Kind::Readlink;
}

fn Readlink::execute(
    const ExecContext &ec, EvalContext &cxt, const ArrayList<String> &args,
    const ArrayList<SourceLocation> &arg_locations) const throws -> i32
{
  let operand_locations = ArrayList<SourceLocation>{cxt.scratch_allocator()};
  let const operands =
      parse_util_operands(FLAG_LIST, args, &arg_locations, &operand_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  if (operands.is_empty()) return report_usage_error(ec, cxt, args[0].view());
  if (operands.count() > 1) {
    report_soft_koshkit_util_error(ec, cxt, operand_locations[1],
                                   args[0].view(),
                                   "extra operand '" + operands[1] + "'");
    return 1;
  }

  let target = os::read_symlink(operands[0].view(), cxt.scratch_allocator());
  if (!target.has_value()) {
    report_soft_koshkit_util_error(
        ec, cxt, operand_locations[0], args[0].view(),
        "'" + operands[0] + "': " + os::last_system_error_message());
    return 1;
  }

  if (!FLAG_READLINK_NO_NEWLINE.is_enabled()) target->push('\n');
  ec.print_to_stdout(target->view());
  return 0;
}

} /* namespace koshkit */

} /* namespace koshka */
