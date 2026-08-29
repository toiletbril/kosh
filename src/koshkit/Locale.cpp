#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-a|-m] [name ...]");

HELP_DESCRIPTION_DECL("The locale utility writes locale information.");

FLAG(LOCALE_ALL, Bool, 'a', "all-locales", "List available locales.");
FLAG(LOCALE_CHARMAPS, Bool, 'm', "charmaps", "List available charmaps.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Locale);

namespace koshka::koshkit {

static fn locale_environment_value(const char *name) wontthrow -> StringView
{
  let const *value = std::getenv(name);
  return value == nullptr ? StringView{} : StringView{value};
}

Locale::Locale() = default;

pure fn Locale::kind() const wontthrow -> Utility::Kind { return Kind::Locale; }

fn Locale::execute(const ExecContext &ec, EvalContext &cxt,
                   const ArrayList<String> &args,
                   const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let const operands = parse_util_operands(FLAG_LIST, args, &arg_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  if (FLAG_LOCALE_ALL.is_enabled() && FLAG_LOCALE_CHARMAPS.is_enabled())
    return report_usage_error(ec, cxt, args[0].view());
  if (FLAG_LOCALE_ALL.is_enabled()) {
    ec.print_to_stdout("C\nPOSIX\n");
    return 0;
  }
  if (FLAG_LOCALE_CHARMAPS.is_enabled()) {
    ec.print_to_stdout("ANSI_X3.4-1968\nUTF-8\n");
    return 0;
  }

  let const active_locale = StringView{setlocale(LC_ALL, nullptr)};
  if (operands.is_empty()) {
    let output = String{cxt.scratch_allocator()};
    output += "LANG=";
    output += locale_environment_value("LANG");
    output += "\nLC_CTYPE=";
    output += locale_environment_value("LC_CTYPE");
    output += "\nLC_COLLATE=";
    output += locale_environment_value("LC_COLLATE");
    output += "\nLC_MONETARY=";
    output += locale_environment_value("LC_MONETARY");
    output += "\nLC_NUMERIC=";
    output += locale_environment_value("LC_NUMERIC");
    output += "\nLC_TIME=";
    output += locale_environment_value("LC_TIME");
    output += "\nLC_MESSAGES=";
    output += locale_environment_value("LC_MESSAGES");
    output += "\nLC_ALL=";
    output += locale_environment_value("LC_ALL");
    output += '\n';
    ec.print_to_stdout(output);
    return 0;
  }

  let const *locale_values = localeconv();
  for (let const &operand : operands) {
    if (operand.view() == "charmap") {
      let lowered = String{cxt.scratch_allocator(), active_locale};
      lowered.lowercase_ascii();
      ec.print_to_stdout(std::strstr(lowered.c_str(), "utf") != nullptr
                             ? "UTF-8\n"
                             : "ANSI_X3.4-1968\n");
    } else if (operand.view() == "decimal_point") {
      ec.print_to_stdout(StringView{locale_values->decimal_point});
      ec.print_to_stdout("\n");
    } else if (operand.view() == "thousands_sep") {
      ec.print_to_stdout(StringView{locale_values->thousands_sep});
      ec.print_to_stdout("\n");
    } else if (operand.view() == "currency_symbol") {
      ec.print_to_stdout(StringView{locale_values->currency_symbol});
      ec.print_to_stdout("\n");
    } else {
      report_soft_koshkit_error(ec, cxt,
                                "locale: unknown name '" + operand + "'");
      return 1;
    }
  }
  return 0;
}

} // namespace koshka::koshkit
