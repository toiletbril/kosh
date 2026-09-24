/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements the touch utility. It parses explicit or reference
 * timestamps, creates missing files when allowed, and updates selected access
 * and modification times.
 */

#include "../CLI.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Path.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-acm] [-r file | -t time] file ...");

HELP_DESCRIPTION_DECL("The touch utility sets the access and the modification "
                      "times of each named file.");

FLAG(TOUCH_ACCESS, Bool, 'a', "", "Change the access time.");
FLAG(TOUCH_NO_CREATE, Bool, 'c', "", "Do not create a file that is missing.");
FLAG(TOUCH_MODIFICATION, Bool, 'm', "", "Change the modification time.");
FLAG(TOUCH_REFERENCE, String, 'r', "", "Use the timestamps of this file.");
FLAG(TOUCH_TIME, String, 't', "", "Use [[CC]YY]MMDDhhmm[.SS].");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Touch);

namespace koshka {

namespace koshkit {

static fn parse_touch_pair(StringView text, usize position,
                           i32 &value) wontthrow -> bool
{
  if (position + 2 > text.length || text[position] < '0' ||
      text[position] > '9' || text[position + 1] < '0' ||
      text[position + 1] > '9')
  {
    return false;
  }

  value =
      static_cast<i32>((text[position] - '0') * 10 + text[position + 1] - '0');
  return true;
}

static fn parse_touch_time(StringView text, i64 &parsed_time) throws -> bool
{
  let main_length = text.length;
  i32 second = 0;
  if (main_length >= 3 && text[main_length - 3] == '.') {
    if (!parse_touch_pair(text, main_length - 2, second)) return false;

    main_length -= 3;
  }
  if (main_length != 8 && main_length != 10 && main_length != 12) return false;

  let const now = std::time(nullptr);
  let const *current = std::localtime(&now);
  if (current == nullptr) throw Error{"cannot read the current time"};

  struct tm value = *current;
  usize position = 0;
  if (main_length == 12) {
    i32 century;
    i32 year;
    if (!parse_touch_pair(text, position, century) ||
        !parse_touch_pair(text, position + 2, year))
    {
      return false;
    }

    value.tm_year = century * 100 + year - 1900;
    position += 4;
  } else if (main_length == 10) {
    i32 year;
    if (!parse_touch_pair(text, position, year)) return false;

    value.tm_year = year >= 69 ? year : year + 100;
    position += 2;
  }
  i32 month;
  i32 day;
  i32 hour;
  i32 minute;
  if (!parse_touch_pair(text, position, month) ||
      !parse_touch_pair(text, position + 2, day) ||
      !parse_touch_pair(text, position + 4, hour) ||
      !parse_touch_pair(text, position + 6, minute))
  {
    return false;
  }

  if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 ||
      minute > 59 || second > 60)
  {
    return false;
  }

  value.tm_mon = month - 1;
  value.tm_mday = day;
  value.tm_hour = hour;
  value.tm_min = minute;
  value.tm_sec = second;
  value.tm_isdst = -1;
  let const timestamp = std::mktime(&value);
  if (timestamp == static_cast<time_t>(-1) || value.tm_mon != month - 1 ||
      value.tm_mday != day || value.tm_hour != hour || value.tm_min != minute ||
      value.tm_sec != second)
  {
    return false;
  }

  parsed_time = static_cast<i64>(timestamp);
  return true;
}

Touch::Touch() = default;

pure fn Touch::kind() const wontthrow -> Utility::Kind { return Kind::Touch; }

fn Touch::execute(const ExecContext &ec, EvalContext &cxt,
                  const ArrayList<String> &args,
                  const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let const[operands, operand_locations] = parse_util_operands(
      FLAG_LIST, args, cxt.scratch_allocator(), &arg_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  if (operands.is_empty()) return report_usage_error(ec, cxt, args[0].view());
  if (FLAG_TOUCH_REFERENCE.is_set() && FLAG_TOUCH_TIME.is_set()) {
    let const conflict_location =
        FLAG_TOUCH_REFERENCE.position() > FLAG_TOUCH_TIME.position()
            ? FLAG_TOUCH_REFERENCE.value_location()
            : FLAG_TOUCH_TIME.value_location();
    KOSHKIT_REPORT_ERROR_AT(conflict_location,
                            "-r and -t cannot be used together");
    return 1;
  }

  os::file_status reference_status{};
  if (FLAG_TOUCH_REFERENCE.is_set() &&
      !os::stat_path_following(FLAG_TOUCH_REFERENCE.value(), reference_status))
  {
    KOSHKIT_REPORT_ERROR_AT(FLAG_TOUCH_REFERENCE.value_location(),
                            "cannot stat '" + FLAG_TOUCH_REFERENCE.value() +
                                "': " + os::last_system_error_message());
    return 1;
  }

  Maybe<i64> requested_time;
  if (FLAG_TOUCH_TIME.is_set()) {
    i64 parsed_time;
    if (!parse_touch_time(FLAG_TOUCH_TIME.value(), parsed_time)) {
      KOSHKIT_REPORT_ERROR_AT(
          FLAG_TOUCH_TIME.value_location(),
          "invalid time '" + FLAG_TOUCH_TIME.value() + "'",
          "use a valid local calendar time in [[CC]YY]MMDDhhmm[.SS] format");
      return 1;
    }

    requested_time = parsed_time;
  }

  let const should_change_access =
      FLAG_TOUCH_ACCESS.is_enabled() || !FLAG_TOUCH_MODIFICATION.is_enabled();
  let const should_change_modification =
      FLAG_TOUCH_MODIFICATION.is_enabled() || !FLAG_TOUCH_ACCESS.is_enabled();

  let const allocator = cxt.scratch_allocator();
  i32 status = 0;
  for (let const &operand : operands) {
    if (!Path{operand.view(), allocator}.exists()) {
      if (FLAG_TOUCH_NO_CREATE.is_enabled()) continue;

      let const fd =
          os::open_file_descriptor(operand.view(), os::file_open_mode::Append);
      if (!fd.has_value()) {
        report_soft_koshkit_util_error(ec, cxt, args[0].view(),
                                       "cannot touch '" + operand + "': " +
                                           os::last_system_error_message());
        status = 1;
        continue;
      }

      os::close_fd(*fd);
    }

    os::file_status current_status{};
    if (!os::stat_path_following(operand.view(), current_status)) {
      report_soft_koshkit_util_error(
          ec, cxt, args[0].view(),
          "cannot touch '" + operand + "': " + os::last_system_error_message());
      status = 1;
      continue;
    }

    let const now = static_cast<i64>(std::time(nullptr));
    let const selected_access_time =
        FLAG_TOUCH_REFERENCE.is_set() ? reference_status.access_time
        : requested_time.has_value()  ? *requested_time
                                      : now;
    let const selected_access_nanoseconds =
        FLAG_TOUCH_REFERENCE.is_set() ? reference_status.access_nanoseconds : 0;
    let const selected_modification_time =
        FLAG_TOUCH_REFERENCE.is_set() ? reference_status.modification_time
        : requested_time.has_value()  ? *requested_time
                                      : now;
    let const selected_modification_nanoseconds =
        FLAG_TOUCH_REFERENCE.is_set()
            ? reference_status.modification_nanoseconds
            : 0;
    if (!os::set_file_times(
            operand.view(),
            should_change_access ? selected_access_time
                                 : current_status.access_time,
            should_change_access ? selected_access_nanoseconds
                                 : current_status.access_nanoseconds,
            should_change_modification ? selected_modification_time
                                       : current_status.modification_time,
            should_change_modification
                ? selected_modification_nanoseconds
                : current_status.modification_nanoseconds))
    {
      report_soft_koshkit_util_error(
          ec, cxt, args[0].view(),
          "cannot touch '" + operand + "': " + os::last_system_error_message());
      status = 1;
    }
  }

  return status;
}

} /* namespace koshkit */

} /* namespace koshka */
