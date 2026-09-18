/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements the cal utility. It computes Gregorian dates, renders
 * Sunday-first and Monday-first calendars, applies terminal styles, and
 * describes the current day.
 */

#include "../CLI.hpp"
#include "../CLIColors.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-a] [[month] year]");

HELP_DESCRIPTION_DECL("The cal utility writes a Gregorian calendar.");

FLAG(CAL_TODAY, Bool, 'a', "today",
     "Start weeks on Monday and describe the current date.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Cal);

namespace koshka::koshkit {

static constexpr StringView CAL_MONTH_NAMES[12] = {
    "January", "February", "March",     "April",   "May",      "June",
    "July",    "August",   "September", "October", "November", "December"};

static constexpr StringView CAL_WEEKDAY_NAMES[7] = {
    "Sunday",   "Monday", "Tuesday", "Wednesday",
    "Thursday", "Friday", "Saturday"};

static constexpr usize CAL_MONTH_LENGTHS[12] = {31, 28, 31, 30, 31, 30,
                                                31, 31, 30, 31, 30, 31};

static pure fn is_cal_leap_year(i64 year) wontthrow -> bool
{
  return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

static pure fn cal_weekday(i64 year, usize month, usize day) wontthrow -> usize
{
  static constexpr usize MONTH_OFFSETS[12] = {0, 3, 2, 5, 0, 3,
                                              5, 1, 4, 6, 2, 4};
  if (month < 3) year--;
  let const adjusted_year = static_cast<u64>(year);
  return static_cast<usize>((adjusted_year % 7 + adjusted_year / 4 % 7 + 7 -
                             adjusted_year / 100 % 7 + adjusted_year / 400 % 7 +
                             MONTH_OFFSETS[month - 1] + day) %
                            7);
}

static fn append_calendar_month(String &output, usize month, i64 year,
                                const std::tm &current_date,
                                bool is_monday_first, bool should_color,
                                Allocator allocator) throws -> void
{
  let title = String{allocator, CAL_MONTH_NAMES[month - 1]};
  title += ' ';
  title += String::from(year, allocator);
  let const left_padding = title.length() < 20 ? (20 - title.length()) / 2 : 0;
  for (usize position = 0; position < left_padding; position++)
    output += ' ';
  append_report_text(output, title.view(), colors::ansi::BOLD_BLUE,
                     should_color);
  output += '\n';
  append_report_text(
      output, is_monday_first ? "Mo Tu We Th Fr Sa Su" : "Su Mo Tu We Th Fr Sa",
      colors::ansi::BOLD_CYAN, should_color);
  output += '\n';

  let const first_weekday = cal_weekday(year, month, 1);
  let const first_column =
      is_monday_first ? (first_weekday + 6) % 7 : first_weekday;
  let day_count = CAL_MONTH_LENGTHS[month - 1];
  if (month == 2 && is_cal_leap_year(year)) day_count++;
  for (usize position = 0; position < first_column; position++)
    output += "   ";

  for (usize day = 1; day <= day_count; day++) {
    if (day < 10) output += ' ';
    let const weekday = (first_weekday + day - 1) % 7;
    let const is_current_day =
        year == current_date.tm_year + 1900 &&
        month == static_cast<usize>(current_date.tm_mon + 1) &&
        day == static_cast<usize>(current_date.tm_mday);
    let const is_weekend = weekday == 0 || weekday == 6;
    let const style = is_current_day && is_weekend
                          ? colors::ansi::BOLD_BRIGHT_RED
                      : is_current_day ? colors::ansi::BOLD_GREEN
                      : is_weekend     ? colors::ansi::RED
                                       : StringView{};
    let const day_text = String::from(day, allocator);
    append_report_text(output, day_text.view(), style, should_color);
    let const column = is_monday_first ? (weekday + 6) % 7 : weekday;
    if (column == 6 || day == day_count)
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
  let const[operands, operand_locations] = parse_util_operands(
      FLAG_LIST, args, cxt.scratch_allocator(), &arg_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  if (operands.count() > 2) {
    KOSHKIT_REPORT_ERROR_AT(operand_locations[2],
                            "extra operand '" + operands[2] + "'",
                            "use `cal [-a] [month] year`");
    return 1;
  }

  let const now = std::time(nullptr);
  let const *local = std::localtime(&now);
  if (local == nullptr) throw Error{"cannot read the current date"};
  let const current_date = *local;

  usize month = 0;
  i64 year = 0;
  if (operands.is_empty()) {
    month = static_cast<usize>(current_date.tm_mon + 1);
    year = current_date.tm_year + 1900;
  } else if (operands.count() == 2) {
    let const parsed_month = utils::parse_decimal_u64(operands[0].view());
    let const parsed_year = utils::parse_decimal_u64(operands[1].view());
    if (parsed_month.is_error() || parsed_month.value() < 1 ||
        parsed_month.value() > 12)
    {
      KOSHKIT_REPORT_ERROR_AT(operand_locations[0],
                              "invalid month '" + operands[0] + "'",
                              "use a month from 1 through 12");
      return 1;
    }
    if (parsed_year.is_error() || parsed_year.value() < 1 ||
        parsed_year.value() > INT64_MAX)
    {
      KOSHKIT_REPORT_ERROR_AT(
          operand_locations[1], "invalid year '" + operands[1] + "'",
          "use a positive decimal year no greater than 9223372036854775807");
      return 1;
    }
    month = static_cast<usize>(parsed_month.value());
    year = static_cast<i64>(parsed_year.value());
  } else {
    let const parsed_year = utils::parse_decimal_u64(operands[0].view());
    if (parsed_year.is_error() || parsed_year.value() < 1 ||
        parsed_year.value() > INT64_MAX)
    {
      KOSHKIT_REPORT_ERROR_AT(
          operand_locations[0], "invalid year '" + operands[0] + "'",
          "use a positive decimal year no greater than 9223372036854775807");
      return 1;
    }
    year = static_cast<i64>(parsed_year.value());
  }

  let output = String{cxt.scratch_allocator()};
  let const should_color = koshkit_should_color();
  let const is_monday_first = FLAG_CAL_TODAY.is_enabled();
  if (month != 0) {
    append_calendar_month(output, month, year, current_date, is_monday_first,
                          should_color, cxt.scratch_allocator());
  } else {
    for (usize current_month = 1; current_month <= 12; current_month++) {
      if (current_month != 1) output += '\n';
      append_calendar_month(output, current_month, year, current_date,
                            is_monday_first, should_color,
                            cxt.scratch_allocator());
    }
  }

  if (FLAG_CAL_TODAY.is_enabled()) {
    output += '\n';
    append_report_text(output, "Today", colors::ansi::BOLD_GREEN, should_color);
    output += " is ";
    output += CAL_WEEKDAY_NAMES[static_cast<usize>(current_date.tm_wday)];
    output += ", ";
    output += CAL_MONTH_NAMES[static_cast<usize>(current_date.tm_mon)];
    output += ' ';
    output += String::from(current_date.tm_mday, cxt.scratch_allocator());
    output += ", ";
    output +=
        String::from(current_date.tm_year + 1900, cxt.scratch_allocator());
    output += ". It is day ";
    output += String::from(current_date.tm_yday + 1, cxt.scratch_allocator());
    output += " of the year.\n";
  }

  ec.print_to_stdout(output);
  return 0;
}

} // namespace koshka::koshkit
