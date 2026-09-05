/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements the nice utility. It parses a priority adjustment,
 * resolves the requested program, starts it with the adjusted scheduling
 * priority, and returns its status.
 */

#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Platform.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-n increment] utility [argument ...]");

HELP_DESCRIPTION_DECL(
    "The nice utility invokes a command with an adjusted scheduling priority.");

FLAG(NICE_INCREMENT, String, 'n', "increment",
     "Add this value to the inherited priority.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Nice);

namespace koshka::koshkit {

Nice::Nice() = default;

pure fn Nice::kind() const wontthrow -> Utility::Kind { return Kind::Nice; }

fn Nice::execute(const ExecContext &ec, EvalContext &cxt,
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
  i64 increment = 10;
  if (FLAG_NICE_INCREMENT.is_set()) {
    let const parsed = utils::parse_decimal_i64(FLAG_NICE_INCREMENT.value());
    if (parsed.is_error()) {
      report_soft_koshkit_util_error(
          ec, cxt, FLAG_NICE_INCREMENT.value_location(), args[0].view(),
          "invalid increment '" + String{FLAG_NICE_INCREMENT.value()} + "'");
      return 2;
    }
    increment = parsed.value();
  }
  if (increment < INT32_MIN || increment > INT32_MAX) {
    report_soft_koshkit_util_error(ec, cxt,
                                   FLAG_NICE_INCREMENT.value_location(),
                                   args[0].view(), "increment is out of range");
    return 2;
  }

  let command = ArrayList<String>{cxt.scratch_allocator()};
  for (let const &operand : operands)
    command.push(operand.clone());
  unused(cxt.materialize_kosh_identity());
  let const result = os::run_nice(command, static_cast<i32>(increment));
  if (!result.has_value()) {
    report_soft_koshkit_util_error(
        ec, cxt, operand_locations[0], args[0].view(),
        "cannot run '" + operands[0] + "': " + os::last_system_error_message());
    return 126;
  }
  return *result;
}

} // namespace koshka::koshkit
