/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements the goodfsw utility. It scans watched paths at fixed
 * intervals and reports creation, removal, content, and attribute changes.
 */

#include "../CLI.hpp"
#include "../CLIColors.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../base/Path.hpp"
#include "../Platform.hpp"
#include "../Utils.hpp"

#include <ctime>

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-hmrtx1] [-l latency] [-e substring] path ...");

HELP_DESCRIPTION_DECL(
    "The goodfsw utility reports changes under the paths it watches.");

FLAG(GOODFSW_RECURSIVE, Bool, 'r', "recursive", "Watch every subdirectory.");
FLAG(GOODFSW_HUMAN, Bool, 'h', "human-readable",
     "Use normal timestamps and descriptive event names.");
FLAG(GOODFSW_MACHINE, Bool, 'm', "machine-readable",
     "Use Unix timestamps and numeric event masks.");
FLAG(GOODFSW_TIMESTAMP, Bool, 't', "timestamp",
     "Prefix every record with the Unix scan timestamp.");
FLAG(GOODFSW_EVENT_FLAGS, Bool, '\0', "event-flags",
     "Append the event names to every record.");
FLAG(GOODFSW_ONE_EVENT, Bool, '1', "one-event",
     "Report the first batch of changes and stop.");
FLAG(GOODFSW_ONE_FILE_SYSTEM, Bool, 'x', "one-file-system",
     "Do not descend across filesystem devices.");
FLAG(GOODFSW_LATENCY, String, 'l', "latency",
     "Wait this many seconds between scans. The default is one.");
FLAG(GOODFSW_EXCLUDE, String, 'e', "exclude",
     "Skip every path that contains this text.");
FLAG(GOODFSW_TIMEZONE, String, '\0', "timezone",
     "Use local or UTC time for human timestamps.");
FLAG(GOODFSW_PRECISION, String, '\0', "precision",
     "Use zero to nine fractional timestamp digits.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(GoodFSW);

namespace koshka::koshkit {

namespace {

enum class goodfsw_traversal_mode : u8
{
  SinglePath,
  Recursive,
};

constexpr f64 DEFAULT_LATENCY_SECONDS = 1.0;
constexpr usize MAXIMUM_SCAN_DEPTH = 64;

enum class timestamp_timezone : u8
{
  Local,
  UTC,
};

enum class watch_event : u8
{
  Created = 1,
  Removed = 2,
  Updated = 4,
  AttributeModified = 8,
};

struct watched_entry
{
  String path;
  u64 device_id{0};
  u64 size{0};
  u64 file_id{0};
  i64 modification_time{0};
  u32 modification_nanoseconds{0};
  u32 mode{0};
  u32 owner_id{0};
  u32 group_id{0};
};

pure fn is_same_content(const watched_entry &previous,
                        const watched_entry &current) wontthrow -> bool
{
  return previous.size == current.size &&
         previous.modification_time == current.modification_time &&
         previous.modification_nanoseconds ==
             current.modification_nanoseconds &&
         previous.file_id == current.file_id;
}

pure fn is_same_attributes(const watched_entry &previous,
                           const watched_entry &current) wontthrow -> bool
{
  return previous.mode == current.mode &&
         previous.owner_id == current.owner_id &&
         previous.group_id == current.group_id;
}

pure fn event_style(watch_event event) wontthrow -> StringView
{
  switch (event) {
  case watch_event::Created: return colors::ansi::BOLD_GREEN;
  case watch_event::Removed: return colors::ansi::BOLD_RED;
  case watch_event::Updated: return colors::ansi::BOLD_CYAN;
  case watch_event::AttributeModified: return colors::ansi::BOLD_YELLOW;
  }

  unreachable("unknown filesystem watch event");
}

fn append_event_names(String &output, const os::file_status &status,
                      watch_event event, bool should_color) throws -> void
{
  let event_name = StringView{};
  switch (event) {
  case watch_event::Created: event_name = "Created"; break;
  case watch_event::Removed: event_name = "Removed"; break;
  case watch_event::Updated: event_name = "Updated"; break;
  case watch_event::AttributeModified: event_name = "AttributeModified"; break;
  }
  append_report_text(output, event_name, event_style(event), should_color);

  switch (os::file_type_letter(status.mode)) {
  case 'd': output += " IsDir"; break;
  case 'l': output += " IsSymLink"; break;
  default: output += " IsFile"; break;
  }
}

pure fn event_mask(watch_event event) wontthrow -> u8
{
  return static_cast<u8>(event);
}

fn format_watch_timestamp(i64 seconds, u32 nanoseconds, usize precision,
                          timestamp_timezone timezone, Allocator allocator)
    throws -> String
{
  let const when = static_cast<time_t>(seconds);
  let const *broken_down = timezone == timestamp_timezone::UTC
                               ? std::gmtime(&when)
                               : std::localtime(&when);
  if (broken_down == nullptr) return String{allocator};

  char date_buffer[32];
  let const date_length =
      std::strftime(date_buffer, sizeof(date_buffer), "%Y-%m-%d %H:%M:%S",
                    broken_down);
  let text = String{allocator, StringView{date_buffer, date_length}};
  if (precision != 0) {
    text += ".";
    let const digits = String::from(nanoseconds, allocator);
    let fraction = String{allocator};
    for (usize index = digits.length(); index < 9; index++) fraction += "0";
    fraction += digits.view();
    text += fraction.substring_of_length(0, precision);
  }

  char zone_buffer[16];
  let const zone_length =
      std::strftime(zone_buffer, sizeof(zone_buffer), "%z", broken_down);
  text += " ";
  text += StringView{zone_buffer, zone_length};
  return text;
}

fn report_event(String &output, StringView path, const os::file_status &status,
                watch_event event, i64 scan_time, u32 scan_nanoseconds,
                usize timestamp_precision, timestamp_timezone timezone,
                bool should_color) throws -> void
{
  let const is_human = FLAG_GOODFSW_HUMAN.is_enabled() &&
                       !FLAG_GOODFSW_MACHINE.is_enabled();
  if (is_human) {
    output += format_watch_timestamp(scan_time, scan_nanoseconds,
                                     timestamp_precision, timezone,
                                     output.allocator());
    output += " ";
  } else if (FLAG_GOODFSW_MACHINE.is_enabled() ||
             FLAG_GOODFSW_TIMESTAMP.is_enabled()) {
    output +=
        String::from(static_cast<u64>(scan_time), output.allocator()).view();
    output += " ";
  }

  if (is_human) {
    append_report_text(output, path, colors::ansi::BOLD, should_color);
    output += " ";
    append_event_names(output, status, event, should_color);
  } else {
    output += String::from(event_mask(event), output.allocator()).view();
    output += " ";
    append_report_text(output, path, colors::ansi::BOLD, should_color);
    if (FLAG_GOODFSW_EVENT_FLAGS.is_enabled()) {
      output += " ";
      append_event_names(output, status, event, should_color);
    }
  }

  output += "\n";
}

fn is_excluded(StringView path) wontthrow -> bool
{
  if (!FLAG_GOODFSW_EXCLUDE.is_set()) return false;

  return path.find_substring(FLAG_GOODFSW_EXCLUDE.value()).has_value();
}

fn scan_path(StringView path, ArrayList<watched_entry> &entries, usize depth,
             Allocator allocator,
             u64 root_device_id,
             const os::file_status *known_status,
             goodfsw_traversal_mode traversal) throws -> void
{
  if (os::INTERRUPT_REQUESTED != 0) return;

  if (depth > MAXIMUM_SCAN_DEPTH) return;

  if (is_excluded(path)) return;

  os::file_status queried_status{};
  if (known_status == nullptr) {
    if (!os::stat_path(path, queried_status)) return;

    known_status = &queried_status;
  }

  if (depth != 0 && FLAG_GOODFSW_ONE_FILE_SYSTEM.is_enabled() &&
      known_status->device_id != root_device_id)
    return;

  watched_entry entry{
      String{allocator, path}
  };
  entry.device_id = known_status->device_id;
  entry.size = known_status->size;
  entry.file_id = known_status->file_id;
  entry.modification_time = known_status->modification_time;
  entry.modification_nanoseconds = known_status->modification_nanoseconds;
  entry.mode = known_status->mode;
  entry.owner_id = known_status->owner_id;
  entry.group_id = known_status->group_id;
  entries.push(steal(entry));

  if (os::file_type_letter(known_status->mode) != 'd') return;

  if (depth > 0 && traversal == goodfsw_traversal_mode::SinglePath) return;

  let const children = os::list_directory_status(path, allocator);
  if (!children.has_value()) return;

  for (let const &child_entry : *children) {
    if (os::INTERRUPT_REQUESTED) return;
    let const &child = child_entry.child;
    if (child.name.view() == "." || child.name.view() == "..") continue;

    let child_path = Path{path, allocator};
    child_path.append(child.name.view());
    let const child_status =
        child_entry.has_status ? &child_entry.status : nullptr;
    scan_path(child_path.view(), entries, depth + 1, allocator, root_device_id,
              child_status, traversal);
  }
}

fn sort_entries(ArrayList<watched_entry> &entries) throws -> void
{
  entries.sort([](const watched_entry &left, const watched_entry &right) {
    return left.path.view() < right.path.view();
  });
}

} // namespace

GoodFSW::GoodFSW() = default;

pure fn GoodFSW::kind() const wontthrow -> Utility::Kind
{
  return Kind::GoodFSW;
}

fn GoodFSW::execute(const ExecContext &ec, EvalContext &cxt,
                    const ArrayList<String> &args,
                    const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let operand_locations = ArrayList<SourceLocation>{cxt.scratch_allocator()};
  let const operands =
      PARSE_KOSHKIT_ARGS_WITH_LOCATIONS(args, arg_locations, operand_locations);

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  if (operands.is_empty()) {
    return report_usage_error(ec, cxt, args[0].view());
  }

  let const allocator = cxt.scratch_allocator();
  let const watch_allocator = heap_allocator();
  f64 latency_seconds = DEFAULT_LATENCY_SECONDS;
  if (FLAG_GOODFSW_LATENCY.is_set()) {
    latency_seconds = parse_koshkit_duration_seconds(
        FLAG_GOODFSW_LATENCY.value(), FLAG_GOODFSW_LATENCY.value_location(),
        allocator);
    if (latency_seconds < 0.05) latency_seconds = 0.05;
  }

  usize timestamp_precision = 9;
  if (FLAG_GOODFSW_PRECISION.is_set()) {
    let const parsed = utils::parse_decimal_u64(FLAG_GOODFSW_PRECISION.value());
    if (parsed.is_error() || parsed.value() > 9) {
      KOSHKIT_REPORT_ERROR_AT(
          FLAG_GOODFSW_PRECISION.value_location(),
          "Invalid timestamp precision '" + FLAG_GOODFSW_PRECISION.value() +
              "'",
          "use a number from 0 through 9");
      return 1;
    }
    timestamp_precision = static_cast<usize>(parsed.value());
  }

  timestamp_timezone timezone = timestamp_timezone::Local;
  if (FLAG_GOODFSW_TIMEZONE.is_set()) {
    let const value = FLAG_GOODFSW_TIMEZONE.value();
    if (value == "utc") {
      timezone = timestamp_timezone::UTC;
    } else if (value != "local") {
      KOSHKIT_REPORT_ERROR_AT(FLAG_GOODFSW_TIMEZONE.value_location(),
                              "Invalid timestamp timezone '" + value + "'",
                              "use local or utc");
      return 1;
    }
  }

  let const traversal = FLAG_GOODFSW_RECURSIVE.is_enabled()
                            ? goodfsw_traversal_mode::Recursive
                            : goodfsw_traversal_mode::SinglePath;

  let operand_paths = ArrayList<Path>{allocator};
  let operand_statuses = ArrayList<os::file_status>{allocator};
  let operand_batch = os::Batch{allocator};
  operand_paths.reserve(operands.count());
  operand_statuses.reserve(operands.count());
  operand_batch.reserve(operands.count());
  for (let const &operand : operands) {
    operand_paths.push(Path{operand.view(), allocator});
    operand_statuses.push({});
  }
  for (usize index = 0; index < operands.count(); index++)
    operand_batch.add(os::batch_operation::lstat(operand_paths[index],
                                                 operand_statuses[index]));
  let const operand_results = operand_batch.execute();
  for (usize index = 0; index < operands.count(); index++) {
    if (operand_results[index].error_number != 0) {
      os::set_last_system_error(operand_results[index].error_number);
      KOSHKIT_REPORT_ERROR_AT(operand_locations[index],
                              "cannot watch '" + operands[index] +
                                  "': " + os::last_system_error_message());
      return 1;
    }
  }

  ArrayList<watched_entry> previous{watch_allocator};
  for (usize index = 0; index < operands.count(); index++)
    scan_path(operands[index].view(), previous, 0, watch_allocator,
              operand_statuses[index].device_id, &operand_statuses[index],
              traversal);
  if (os::INTERRUPT_REQUESTED != 0) {
    os::INTERRUPT_REQUESTED = 0;
    return 130;
  }
  sort_entries(previous);

  let const should_color = koshkit_should_color();

  bool was_interrupted = false;
  loop
  {
    os::sleep_for_seconds(latency_seconds);
    if (os::INTERRUPT_REQUESTED != 0) {
      was_interrupted = true;
      os::INTERRUPT_REQUESTED = 0;
      break;
    }

    ArrayList<watched_entry> current{watch_allocator};
    for (usize index = 0; index < operands.count(); index++) {
      if (os::INTERRUPT_REQUESTED) {
        was_interrupted = true;
        break;
      }
      scan_path(operands[index].view(), current, 0, watch_allocator,
                operand_statuses[index].device_id, nullptr, traversal);
    }
    if (was_interrupted) {
      os::INTERRUPT_REQUESTED = 0;
      break;
    }
    sort_entries(current);

    let const scan_microseconds = os::realtime_microseconds();
    let const scan_time = static_cast<i64>(scan_microseconds / 1000000u);
    let const scan_nanoseconds =
        static_cast<u32>((scan_microseconds % 1000000u) * 1000u);
    let output = String{watch_allocator};

    usize previous_position = 0;
    usize current_position = 0;
    while (previous_position < previous.count() ||
           current_position < current.count())
    {
      if (os::INTERRUPT_REQUESTED) {
        was_interrupted = true;
        break;
      }
      if (current_position >= current.count() ||
          (previous_position < previous.count() &&
           previous[previous_position].path.view() <
               current[current_position].path.view()))
      {
        let const &entry = previous[previous_position];
        os::file_status rendered{};
        rendered.mode = entry.mode;
        report_event(output, entry.path.view(), rendered, watch_event::Removed,
                     scan_time, scan_nanoseconds, timestamp_precision, timezone,
                     should_color);
        previous_position++;
        continue;
      }

      if (previous_position >= previous.count() ||
          current[current_position].path.view() <
              previous[previous_position].path.view())
      {
        let const &entry = current[current_position];
        os::file_status rendered{};
        rendered.mode = entry.mode;
        report_event(output, entry.path.view(), rendered, watch_event::Created,
                     scan_time, scan_nanoseconds, timestamp_precision, timezone,
                     should_color);
        current_position++;
        continue;
      }

      let const &previous_entry = previous[previous_position];
      let const &current_entry = current[current_position];
      os::file_status rendered{};
      rendered.mode = current_entry.mode;
      if (!is_same_content(previous_entry, current_entry)) {
        report_event(output, current_entry.path.view(), rendered,
                     watch_event::Updated, scan_time, scan_nanoseconds,
                     timestamp_precision, timezone, should_color);
      } else if (!is_same_attributes(previous_entry, current_entry)) {
        report_event(output, current_entry.path.view(), rendered,
                     watch_event::AttributeModified, scan_time, scan_nanoseconds,
                     timestamp_precision, timezone, should_color);
      }

      previous_position++;
      current_position++;
    }

    previous = steal(current);

    if (output.is_empty()) continue;

    ec.print_to_stdout(output);

    if (FLAG_GOODFSW_ONE_EVENT.is_enabled()) break;
  }

  return was_interrupted ? 130 : 0;
}

} // namespace koshka::koshkit
