/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements the touch utility. It parses explicit or reference
 * timestamps, creates missing files when allowed, and updates selected access
 * and modification times.
 */

#include "../Cli.hpp"
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

static fn parse_touch_pair(StringView text, usize position) throws -> i32
{
  if (position + 2 > text.length || text[position] < '0' ||
      text[position] > '9' || text[position + 1] < '0' ||
      text[position + 1] > '9')
  {
    throw Error{"touch: invalid time '" + String{text} + "'"};
  }

  return static_cast<i32>((text[position] - '0') * 10 + text[position + 1] -
                          '0');
}

static fn parse_touch_time(StringView text) throws -> i64
{
  let main_length = text.length;
  i32 second = 0;
  if (main_length >= 3 && text[main_length - 3] == '.') {
    second = parse_touch_pair(text, main_length - 2);
    main_length -= 3;
  }
  if (main_length != 8 && main_length != 10 && main_length != 12)
    throw Error{"touch: invalid time '" + String{text} + "'"};

  let const now = std::time(NULL);
  let const *current = std::localtime(&now);
  if (current == NULL) throw Error{"touch: cannot read the current time"};

  struct tm value = *current;
  usize position = 0;
  if (main_length == 12) {
    value.tm_year = parse_touch_pair(text, position) * 100 +
                    parse_touch_pair(text, position + 2) - 1900;
    position += 4;
  } else if (main_length == 10) {
    let const year = parse_touch_pair(text, position);
    value.tm_year = year >= 69 ? year : year + 100;
    position += 2;
  }
  let const month = parse_touch_pair(text, position);
  let const day = parse_touch_pair(text, position + 2);
  let const hour = parse_touch_pair(text, position + 4);
  let const minute = parse_touch_pair(text, position + 6);
  if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 ||
      minute > 59 || second > 60)
  {
    throw Error{"touch: invalid time '" + String{text} + "'"};
  }

  value.tm_mon = month - 1;
  value.tm_mday = day;
  value.tm_hour = hour;
  value.tm_min = minute;
  value.tm_sec = second;
  value.tm_isdst = -1;
  let const result = std::mktime(&value);
  if (result == static_cast<time_t>(-1) || value.tm_mon != month - 1 ||
      value.tm_mday != day || value.tm_hour != hour || value.tm_min != minute ||
      value.tm_sec != second)
  {
    throw Error{"touch: invalid time '" + String{text} + "'"};
  }

  return static_cast<i64>(result);
}

Touch::Touch() = default;

pure fn Touch::kind() const wontthrow -> Utility::Kind { return Kind::Touch; }

fn Touch::execute(const ExecContext &ec, EvalContext &cxt,
                  const ArrayList<String> &args,
                  const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let const operands = parse_util_operands(FLAG_LIST, args, &arg_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  if (operands.is_empty()) return report_usage_error(ec, cxt, args[0].view());
  if (FLAG_TOUCH_REFERENCE.is_set() && FLAG_TOUCH_TIME.is_set())
    throw Error{"touch: -r and -t cannot be used together"};

  os::file_status reference_status{};
  if (FLAG_TOUCH_REFERENCE.is_set() &&
      !os::stat_path_following(FLAG_TOUCH_REFERENCE.value(), reference_status))
  {
    throw Error{"touch: cannot stat '" + FLAG_TOUCH_REFERENCE.value() +
                "': " + os::last_system_error_message()};
  }

  Maybe<i64> requested_time;
  if (FLAG_TOUCH_TIME.is_set())
    requested_time = parse_touch_time(FLAG_TOUCH_TIME.value());

  let const should_change_access =
      FLAG_TOUCH_ACCESS.is_enabled() || !FLAG_TOUCH_MODIFICATION.is_enabled();
  let const should_change_modification =
      FLAG_TOUCH_MODIFICATION.is_enabled() || !FLAG_TOUCH_ACCESS.is_enabled();

  i32 status = 0;
  for (const String &operand : operands) {
    if (!Path{operand.view()}.exists()) {
      if (FLAG_TOUCH_NO_CREATE.is_enabled()) continue;

      let const fd =
          os::open_file_descriptor(operand.view(), os::file_open_mode::Append);
      if (!fd.has_value()) {
        report_soft_koshkit_error(ec, cxt,
                                  "touch: cannot touch '" + operand +
                                      "': " + os::last_system_error_message());
        status = 1;
        continue;
      }

      os::close_fd(*fd);
    }

    os::file_status current_status{};
    if (!os::stat_path_following(operand.view(), current_status)) {
      report_soft_koshkit_error(ec, cxt,
                                "touch: cannot touch '" + operand +
                                    "': " + os::last_system_error_message());
      status = 1;
      continue;
    }

    let const now = static_cast<i64>(std::time(NULL));
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
      report_soft_koshkit_error(ec, cxt,
                                "touch: cannot touch '" + operand +
                                    "': " + os::last_system_error_message());
      status = 1;
    }
  }

  return status;
}

} /* namespace koshkit */

} /* namespace koshka */
