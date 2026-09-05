/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements and is responsible for the nohup utility in koshkit.
 * The nohup utility invokes a command that ignores terminal hangups.
 */

#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Platform.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("utility [argument ...]");

HELP_DESCRIPTION_DECL(
    "The nohup utility invokes a command that ignores terminal hangups.");

FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Nohup);

namespace koshka::koshkit {

Nohup::Nohup() = default;

pure fn Nohup::kind() const wontthrow -> Utility::Kind { return Kind::Nohup; }

fn Nohup::execute(const ExecContext &ec, EvalContext &cxt,
                  const ArrayList<String> &args,
                  const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let operand_locations = ArrayList<SourceLocation>{cxt.scratch_allocator()};
  let const operands =
      parse_util_operands(FLAG_LIST, args, &arg_locations, &operand_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  if (operands.is_empty()) return report_usage_error(ec, cxt, args[0].view());
  let command = ArrayList<String>{cxt.scratch_allocator()};
  for (let const &operand : operands)
    command.push(operand.clone());
  let const home_value = cxt.get_variable_value("HOME");
  let const home = home_value.has_value() ? home_value->view() : StringView{};
  unused(cxt.materialize_kosh_identity());
  let const result = os::run_nohup(command, ec.in_fd.value_or(KOSH_STDIN),
                                   ec.out_fd.value_or(KOSH_STDOUT),
                                   ec.err_fd.value_or(KOSH_STDERR), home);
  if (!result.has_value()) {
    report_soft_koshkit_util_error(
        ec, cxt, operand_locations[0], args[0].view(),
        "cannot run '" + operands[0] + "': " + os::last_system_error_message());
    return 126;
  }

  return *result;
}

} // namespace koshka::koshkit
