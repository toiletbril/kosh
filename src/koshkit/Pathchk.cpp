/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements and is responsible for the pathchk utility in
 * koshkit. The pathchk utility checks pathname portability.
 */

#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Path.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-pP] pathname ...");

HELP_DESCRIPTION_DECL("The pathchk utility checks pathname portability.");

FLAG(PATHCHK_PORTABLE, Bool, 'p', "portable",
     "Use POSIX portable pathname limits and characters.");
FLAG(PATHCHK_LEADING_HYPHEN, Bool, 'P', "leading-hyphen",
     "Reject an empty pathname or a component beginning with a hyphen.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Pathchk);

namespace koshka::koshkit {

static pure fn is_portable_filename_byte(char byte) wontthrow -> bool
{
  return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
         (byte >= '0' && byte <= '9') || byte == '.' || byte == '_' ||
         byte == '-';
}

Pathchk::Pathchk() = default;

pure fn Pathchk::kind() const wontthrow -> Utility::Kind
{
  return Kind::Pathchk;
}

fn Pathchk::execute(const ExecContext &ec, EvalContext &cxt,
                    const ArrayList<String> &args,
                    const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let const operands = parse_util_operands(FLAG_LIST, args, &arg_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  if (operands.is_empty()) return report_usage_error(ec, cxt, args[0].view());
  let const should_check_portable = FLAG_PATHCHK_PORTABLE.is_enabled();
  let const should_check_hyphen = FLAG_PATHCHK_LEADING_HYPHEN.is_enabled();
  i32 status = 0;

  for (const String &operand : operands) {
    let const path = operand.view();
    let const configuration_path =
        !path.is_empty() && path[0] == '/' ? StringView{"/"} : StringView{"."};
    let const configured_path_limit = os::path_configuration(
        configuration_path, os::path_configuration_key::PathMax);
    let const configured_name_limit = os::path_configuration(
        configuration_path, os::path_configuration_key::NameMax);
    let const path_limit =
        should_check_portable ? i64{256} : configured_path_limit.value_or(4096);
    let const name_limit =
        should_check_portable ? i64{14} : configured_name_limit.value_or(255);
    String reason{cxt.scratch_allocator()};
    if (path.is_empty() && should_check_hyphen) {
      reason = "empty pathname";
    } else if (path_limit > 0 && path.length >= static_cast<usize>(path_limit))
    {
      reason = "pathname too long";
    } else {
      usize position = 0;
      while (position < path.length) {
        let const component = Path::next_component(path, position);
        if (name_limit > 0 &&
            component.text.length > static_cast<usize>(name_limit))
        {
          reason = "component too long";
          break;
        }
        if (should_check_hyphen && !component.text.is_empty() &&
            component.text[0] == '-')
        {
          reason = "a component begins with '-'";
          break;
        }
        if (should_check_portable)
          for (usize byte_position = 0; byte_position < component.text.length;
               byte_position++)
            if (!is_portable_filename_byte(component.text[byte_position])) {
              reason = "component contains non-portable characters";
              break;
            }
        if (!reason.is_empty()) break;
      }
    }

    if (!reason.is_empty()) {
      report_soft_koshkit_error(ec, cxt, "pathchk: " + operand + ": " + reason);
      status = 1;
    }
  }

  return status;
}

} // namespace koshka::koshkit
