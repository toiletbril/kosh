/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements the cal utility. It computes Gregorian leap years and
 * weekdays, then renders one month or a complete year.
 */

#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[[month] year]");

HELP_DESCRIPTION_DECL("The cal utility writes a Gregorian calendar.");

FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Cal);

namespace koshka::koshkit {

static pure fn is_cal_leap_year(i64 year) wontthrow -> bool
{
  return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

static pure fn cal_weekday(i64 year, usize month, usize day) wontthrow -> usize
{
  static constexpr usize MONTH_OFFSETS[12] = {0, 3, 2, 5, 0, 3,
                                              5, 1, 4, 6, 2, 4};
  if (month < 3) year--;
  return static_cast<usize>((year + year / 4 - year / 100 + year / 400 +
                             MONTH_OFFSETS[month - 1] + day) %
                            7);
}

static fn append_calendar_month(String &output, usize month, i64 year,
                                Allocator allocator) throws -> void
{
  static constexpr StringView MONTH_NAMES[12] = {
      "January", "February", "March",     "April",   "May",      "June",
      "July",    "August",   "September", "October", "November", "December"};
  static constexpr usize MONTH_LENGTHS[12] = {31, 28, 31, 30, 31, 30,
                                              31, 31, 30, 31, 30, 31};
  let title = String{allocator, MONTH_NAMES[month - 1]};
  title += ' ';
  title += String::from(year, allocator);
  let const left_padding = title.length() < 20 ? (20 - title.length()) / 2 : 0;
  for (usize position = 0; position < left_padding; position++)
    output += ' ';
  output += title.view();
  output += "\nSu Mo Tu We Th Fr Sa\n";

  let const first_weekday = cal_weekday(year, month, 1);
  let day_count = MONTH_LENGTHS[month - 1];
  if (month == 2 && is_cal_leap_year(year)) day_count++;
  for (usize position = 0; position < first_weekday; position++)
    output += "   ";

  for (usize day = 1; day <= day_count; day++) {
    if (day < 10) output += ' ';
    output += String::from(day, allocator);
    let const weekday = (first_weekday + day - 1) % 7;
    if (weekday == 6 || day == day_count)
      output += '\n';
    else
      output += ' ';
  }
}

Cal::Cal() = default;

pure fn Cal::kind() const wontthrow -> Utility::Kind { return Kind::Cal; }

fn Cal::execute(const ExecContext &ec, EvalContext &cxt,
                const ArrayList<String> &args,
                const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let const operands = parse_util_operands(FLAG_LIST, args, &arg_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  if (operands.count() > 2) return report_usage_error(ec, cxt, args[0].view());

  usize month = 0;
  i64 year = 0;
  if (operands.is_empty()) {
    let const now = std::time(nullptr);
    let const *local = std::localtime(&now);
    if (local == nullptr) throw Error{"cal: cannot read the current date"};
    month = static_cast<usize>(local->tm_mon + 1);
    year = local->tm_year + 1900;
  } else if (operands.count() == 2) {
    let const parsed_month = utils::parse_decimal_u64(operands[0].view());
    let const parsed_year = utils::parse_decimal_u64(operands[1].view());
    if (parsed_month.is_error() || parsed_year.is_error() ||
        parsed_month.value() < 1 || parsed_month.value() > 12 ||
        parsed_year.value() < 1 || parsed_year.value() > INT64_MAX)
      throw Error{"cal: invalid month or year"};
    month = static_cast<usize>(parsed_month.value());
    year = static_cast<i64>(parsed_year.value());
  } else {
    let const parsed_year = utils::parse_decimal_u64(operands[0].view());
    if (parsed_year.is_error() || parsed_year.value() < 1 ||
        parsed_year.value() > INT64_MAX)
      throw Error{"cal: invalid year"};
    year = static_cast<i64>(parsed_year.value());
  }

  let output = String{cxt.scratch_allocator()};
  if (month != 0) {
    append_calendar_month(output, month, year, cxt.scratch_allocator());
  } else {
    for (usize current_month = 1; current_month <= 12; current_month++) {
      if (current_month != 1) output += '\n';
      append_calendar_month(output, current_month, year,
                            cxt.scratch_allocator());
    }
  }
  ec.print_to_stdout(output);
  return 0;
}

} // namespace koshka::koshkit
