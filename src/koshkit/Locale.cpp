/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements and is responsible for the locale utility in koshkit.
 * The locale utility writes locale information.
 */

#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../StaticStringMap.hpp"

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

enum class locale_value_kind : uchar
{
  CharacterMap,
  CurrencySymbol,
  DecimalPoint,
  ThousandsSeparator,
};

struct locale_keyword
{
  StringView category;
  locale_value_kind value_kind;
};

static constexpr static_string_entry<locale_keyword> LOCALE_KEYWORD_ENTRIES[] =
    {
        {SSK("charmap"),         {"LC_CTYPE", locale_value_kind::CharacterMap}  },
        {SSK("currency_symbol"),
         {"LC_MONETARY", locale_value_kind::CurrencySymbol}                     },
        {SSK("decimal_point"),   {"LC_NUMERIC", locale_value_kind::DecimalPoint}},
        {SSK("thousands_sep"),
         {"LC_NUMERIC", locale_value_kind::ThousandsSeparator}                  },
};
static constexpr StaticStringMap LOCALE_KEYWORDS{LOCALE_KEYWORD_ENTRIES};

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
  let operand_locations = ArrayList<SourceLocation>{cxt.scratch_allocator()};
  let const operands =
      parse_util_operands(FLAG_LIST, args, &arg_locations, &operand_locations);
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
  for (usize operand_position = 0; operand_position < operands.count();
       operand_position++)
  {
    let const &operand = operands[operand_position];
    let const keyword = LOCALE_KEYWORDS.find(operand.view());
    if (!keyword.has_value()) {
      report_soft_koshkit_util_error(
          ec, cxt, operand_locations[operand_position], args[0].view(),
          "unknown name '" + operand + "'");
      return 1;
    }
    if (FLAG_LOCALE_CATEGORY.is_enabled()) {
      ec.print_to_stdout(keyword->category);
      ec.print_to_stdout("\n");
    }

    StringView value;
    switch (keyword->value_kind) {
    case locale_value_kind::CharacterMap: {
      let lowered = String{cxt.scratch_allocator(), active_locale};
      lowered.lowercase_ascii();
      value = std::strstr(lowered.c_str(), "utf") != NULL
                  ? StringView{"UTF-8"}
                  : StringView{"ANSI_X3.4-1968"};
      break;
    }
    case locale_value_kind::CurrencySymbol:
      value = StringView{locale_values->currency_symbol};
      break;
    case locale_value_kind::DecimalPoint:
      value = StringView{locale_values->decimal_point};
      break;
    case locale_value_kind::ThousandsSeparator:
      value = StringView{locale_values->thousands_sep};
      break;
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
