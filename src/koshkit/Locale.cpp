#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-a|-m] | [-ck] [name ...]");

HELP_DESCRIPTION_DECL("The locale utility writes locale information.");

FLAG(LOCALE_ALL_LOCALES, Bool, 'a', "all-locales", "List available locales.");
FLAG(LOCALE_CHARMAPS, Bool, 'm', "charmaps", "List available charmaps.");
FLAG(LOCALE_CATEGORY, Bool, 'c', "category-name",
     "Write the category name for each selected keyword.");
FLAG(LOCALE_KEYWORD, Bool, 'k', "keyword-name",
     "Write each selected keyword name.");
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

  if (FLAG_LOCALE_ALL_LOCALES.is_enabled() && FLAG_LOCALE_CHARMAPS.is_enabled())
    return report_usage_error(ec, cxt, args[0].view());
  if (FLAG_LOCALE_ALL_LOCALES.is_enabled()) {
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
    static constexpr static_string_entry<StringView> CATEGORY_ENTRIES[] = {
        {SSK("charmap"),         "LC_CTYPE"   },
        {SSK("currency_symbol"), "LC_MONETARY"},
        {SSK("decimal_point"),   "LC_NUMERIC" },
        {SSK("thousands_sep"),   "LC_NUMERIC" },
    };
    static constexpr StaticStringMap CATEGORIES{CATEGORY_ENTRIES};
    let const category = CATEGORIES.find(operand.view());
    if (!category.has_value()) {
      report_soft_koshkit_error(ec, cxt,
                                "locale: unknown name '" + operand + "'");
      return 1;
    }
    if (FLAG_LOCALE_CATEGORY.is_enabled()) {
      ec.print_to_stdout(*category);
      ec.print_to_stdout("\n");
    }

    StringView value;
    if (operand.view() == "charmap") {
      let lowered = String{cxt.scratch_allocator(), active_locale};
      lowered.lowercase_ascii();
      value = std::strstr(lowered.c_str(), "utf") != NULL
                  ? StringView{"UTF-8"}
                  : StringView{"ANSI_X3.4-1968"};
    } else if (operand.view() == "decimal_point") {
      value = StringView{locale_values->decimal_point};
    } else if (operand.view() == "thousands_sep") {
      value = StringView{locale_values->thousands_sep};
    } else if (operand.view() == "currency_symbol") {
      value = StringView{locale_values->currency_symbol};
    }
    if (FLAG_LOCALE_KEYWORD.is_enabled()) {
      ec.print_to_stdout(operand.view());
      ec.print_to_stdout("=\"");
      ec.print_to_stdout(value);
      ec.print_to_stdout("\"\n");
    } else {
      ec.print_to_stdout(value);
      ec.print_to_stdout("\n");
    }
  }
  return 0;
}

} // namespace koshka::koshkit
