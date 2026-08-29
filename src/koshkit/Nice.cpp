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
  let const operands = parse_util_operands(FLAG_LIST, args, &arg_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  if (operands.is_empty()) return report_usage_error(ec, cxt, args[0].view());
  i64 increment = 10;
  if (FLAG_NICE_INCREMENT.is_set()) {
    let const parsed = utils::parse_decimal_i64(FLAG_NICE_INCREMENT.value());
    if (parsed.is_error()) throw Error{"nice: invalid increment"};
    increment = parsed.value();
  }
  if (increment < INT32_MIN || increment > INT32_MAX)
    throw Error{"nice: increment is out of range"};

  let command = ArrayList<String>{cxt.scratch_allocator()};
  for (let const &operand : operands)
    command.push(operand.clone());
  unused(cxt.materialize_kosh_identity());
  let const result = os::run_nice(command, static_cast<i32>(increment));
  return result.value_or(126);
}

} // namespace koshka::koshkit
