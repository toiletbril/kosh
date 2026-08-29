#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-u] [+format]");

HELP_DESCRIPTION_DECL("The date utility writes the date and time.");

FLAG(DATE_UTC, Bool, 'u', "utc", "Use Coordinated Universal Time.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Date);

namespace koshka::koshkit {

Date::Date() = default;

pure fn Date::kind() const wontthrow -> Utility::Kind { return Kind::Date; }

fn Date::execute(const ExecContext &ec, EvalContext &cxt,
                 const ArrayList<String> &args,
                 const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let operand_locations = ArrayList<SourceLocation>{cxt.scratch_allocator()};
  let const operands =
      parse_util_operands(FLAG_LIST, args, &arg_locations, &operand_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  if (operands.count() > 1) return report_usage_error(ec, cxt, args[0].view());
  if (!operands.is_empty() && (operands[0].is_empty() || operands[0][0] != '+'))
  {
    report_soft_koshkit_util_error(ec, cxt, operand_locations[0],
                                   args[0].view(),
                                   "setting the system clock is unsupported");
    return 1;
  }

  let const now = std::time(nullptr);
  let const *broken_down =
      FLAG_DATE_UTC.is_enabled() ? std::gmtime(&now) : std::localtime(&now);
  if (broken_down == nullptr) {
    report_soft_koshkit_util_error(ec, cxt, args[0].view(),
                                   "cannot convert the current time");
    return 1;
  }

  let const format = operands.is_empty() ? StringView{"%a %b %e %H:%M:%S %Z %Y"}
                                         : operands[0].view().substring(1);
  let const format_text = String{cxt.scratch_allocator(), format};
  char buffer[8192];
  let const length =
      std::strftime(buffer, sizeof(buffer), format_text.c_str(), broken_down);
  if (length == 0 && !format.is_empty()) {
    let const location =
        operands.is_empty() ? ec.source_location() : operand_locations[0];
    report_soft_koshkit_util_error(
        ec, cxt, location, args[0].view(),
        "formatted output exceeds the supported line length");
    return 1;
  }

  ec.print_to_stdout(StringView{buffer, length});
  ec.print_to_stdout("\n");
  return 0;
}

} // namespace koshka::koshkit
