#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("duration ...");

HELP_DESCRIPTION_DECL(
    "The sleep utility pauses for the sum of the given durations.");

FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Sleep);

namespace koshka {

namespace koshkit {

Sleep::Sleep() = default;

pure fn Sleep::kind() const wontthrow -> Utility::Kind { return Kind::Sleep; }

fn Sleep::execute(const ExecContext &ec, EvalContext &cxt,
                  const ArrayList<String> &args,
                  const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let const operands = parse_util_operands(FLAG_LIST, args, &arg_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  if (operands.is_empty()) return report_usage_error(ec, cxt, args[0].view());

  f64 total_seconds = 0.0;
  bool should_sleep_forever = false;
  for (const String &operand : operands) {
    let const seconds_value = parse_koshkit_duration_seconds(
        operand.view(), StringView{"sleep"}, cxt.scratch_allocator());

    if (__builtin_isinf(seconds_value)) {
      should_sleep_forever = true;
      continue;
    }

    total_seconds += seconds_value;
    if (__builtin_isinf(total_seconds)) {
      should_sleep_forever = true;
      break;
    }
  }

  if (should_sleep_forever) {
    while (!os::INTERRUPT_REQUESTED)
      os::sleep_for_seconds(60.0 * 60.0 * 24.0);
    return 130;
  }

  os::sleep_for_seconds(total_seconds);

  return os::INTERRUPT_REQUESTED ? 130 : 0;
}

} /* namespace koshkit */

} /* namespace koshka */
