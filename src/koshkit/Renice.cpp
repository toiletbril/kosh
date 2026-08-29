#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Platform.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-g | -p | -u] -n increment id ...");

HELP_DESCRIPTION_DECL(
    "The renice utility adjusts scheduling priorities of running processes.");

FLAG(RENICE_GROUP, Bool, 'g', "pgrp",
     "Interpret identifiers as process groups.");
FLAG(RENICE_PROCESS, Bool, 'p', "pid",
     "Interpret identifiers as process identifiers.");
FLAG(RENICE_USER, Bool, 'u', "user", "Interpret identifiers as users.");
FLAG(RENICE_INCREMENT, String, 'n', "increment",
     "Add this value to each current priority.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Renice);

namespace koshka::koshkit {

static fn renice_identifier(StringView text, os::priority_target target) throws
    -> Maybe<i64>
{
  let const parsed = utils::parse_decimal_i64(text);
  if (!parsed.is_error() && parsed.value() >= 0) return parsed.value();
  if (target == os::priority_target::User) {
    let const user = os::username_to_uid(text);
    if (user.has_value()) return *user;
  }
  return None;
}

Renice::Renice() = default;

pure fn Renice::kind() const wontthrow -> Utility::Kind { return Kind::Renice; }

fn Renice::execute(const ExecContext &ec, EvalContext &cxt,
                   const ArrayList<String> &args,
                   const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let operand_locations = ArrayList<SourceLocation>{cxt.scratch_allocator()};
  let const operands =
      parse_util_operands(FLAG_LIST, args, &arg_locations, &operand_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  let const selector_count =
      static_cast<usize>(FLAG_RENICE_GROUP.is_enabled()) +
      FLAG_RENICE_PROCESS.is_enabled() + FLAG_RENICE_USER.is_enabled();
  if (operands.is_empty() || !FLAG_RENICE_INCREMENT.is_set() ||
      selector_count > 1)
    return report_usage_error(ec, cxt, args[0].view());
  let const parsed = utils::parse_decimal_i64(FLAG_RENICE_INCREMENT.value());
  if (parsed.is_error() || parsed.value() < INT32_MIN ||
      parsed.value() > INT32_MAX)
  {
    report_soft_koshkit_util_error(
        ec, cxt, FLAG_RENICE_INCREMENT.value_location(), args[0].view(),
        "invalid increment '" + String{FLAG_RENICE_INCREMENT.value()} + "'");
    return 2;
  }
  let const increment = parsed.value();
  let const target =
      FLAG_RENICE_GROUP.is_enabled()  ? os::priority_target::ProcessGroup
      : FLAG_RENICE_USER.is_enabled() ? os::priority_target::User
                                      : os::priority_target::Process;
  i32 result = 0;
  for (usize operand_position = 0; operand_position < operands.count();
       operand_position++)
  {
    let const &operand = operands[operand_position];
    let const id = renice_identifier(operand.view(), target);
    if (!id.has_value()) {
      report_soft_koshkit_util_error(
          ec, cxt, operand_locations[operand_position], args[0].view(),
          "invalid identifier '" + operand + "'");
      result = 1;
      continue;
    }
    let const current = os::get_priority(target, *id);
    if (!current.has_value()) {
      report_soft_koshkit_util_error(
          ec, cxt, operand_locations[operand_position], args[0].view(),
          "cannot read priority for '" + operand +
              "': " + os::last_system_error_message());
      result = 1;
      continue;
    }
    let priority = static_cast<i64>(*current) + increment;
    if (priority < -20) priority = -20;
    if (priority > 19) priority = 19;
    if (!os::set_priority(target, *id, static_cast<i32>(priority))) {
      report_soft_koshkit_util_error(
          ec, cxt, operand_locations[operand_position], args[0].view(),
          "cannot adjust '" + operand +
              "': " + os::last_system_error_message());
      result = 1;
    }
  }
  return result;
}

} // namespace koshka::koshkit
