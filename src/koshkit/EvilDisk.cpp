/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements evildisk. It reports filesystem capacity, disk failure
 * counters, and available SMART data for mounted filesystems or named paths.
 */

#include "../CLI.hpp"
#include "../CLIColors.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Platform.hpp"
#include "../StaticStringMap.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-a] [file ...]");

HELP_DESCRIPTION_DECL(
    "The evildisk utility reports filesystem capacity and disk health data.");

FLAG(EVILDISK_ALL, Bool, 'a', "all", "Show available SMART data.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(EvilDisk);

namespace koshka::koshkit {

namespace {

struct disk_row
{
  String source{heap_allocator()};
  String type{heap_allocator()};
  String size{heap_allocator()};
  String used{heap_allocator()};
  String available{heap_allocator()};
  String use{heap_allocator()};
  String target{heap_allocator()};
  u64 use_percent{0};
};

struct smart_row
{
  String device{heap_allocator()};
  String status{heap_allocator()};
  String model{heap_allocator()};
  String protocol{heap_allocator()};
  String statistics{heap_allocator()};
  String warning_statistics{heap_allocator()};
};

struct smart_statistic_field
{
  StringView report_name;
  StringView label;
  bool should_warn;
};

fn usage_style(u64 percent) wontthrow -> StringView
{
  if (percent >= 90) return colors::ansi::BOLD_RED;
  if (percent >= 75) return colors::ansi::BOLD_YELLOW;
  return colors::ansi::BOLD_GREEN;
}

fn report_value(StringView report, const StringView *names, usize name_count,
                Allocator allocator) throws -> String
{
  usize position = 0;
  while (position < report.length) {
    let const line = report.next_line(position).trim_blanks();
    for (usize index = 0; index < name_count; index++) {
      if (!line.starts_with(names[index])) continue;

      let value = line.substring(names[index].length).trim_blanks();
      if (!value.is_empty() && value[0] == ':')
        value = value.substring(1).trim_blanks();
      if (!value.is_empty()) return String{allocator, value};
    }
  }

  return String{allocator};
}

pure fn is_nonzero_smart_counter(StringView value) wontthrow -> bool
{
  bool is_nonzero = false;
  for (usize index = 0; index < value.length; index++) {
    let const byte = value[index];
    if (byte >= '0' && byte <= '9') {
      if (byte != '0') is_nonzero = true;
      continue;
    }
    if (byte != ',') return false;
  }

  return is_nonzero;
}

fn append_smart_statistic(String &statistics, String &warning_statistics,
                          const smart_statistic_field &field,
                          StringView value) throws -> void
{
  if (value.is_empty()) return;

  if (!statistics.is_empty()) statistics += ", ";
  statistics += field.label;
  statistics += " ";
  statistics += value;

  if (field.should_warn && is_nonzero_smart_counter(value)) {
    if (!warning_statistics.is_empty()) warning_statistics += ", ";
    warning_statistics += field.label;
    warning_statistics += " ";
    warning_statistics += value;
  }
}

fn append_ata_smart_statistics(StringView report, String &statistics,
                               String &warning_statistics) throws -> void
{
  static constexpr static_string_entry<smart_statistic_field> FIELD_ENTRIES[] =
      {
          {SSK("5"),   {"", "reallocated", true}    },
          {SSK("9"),   {"", "power-on hours", false}},
          {SSK("187"), {"", "reported errors", true}},
          {SSK("188"), {"", "timeouts", true}       },
          {SSK("194"), {"", "temperature", false}   },
          {SSK("197"), {"", "pending", true}        },
          {SSK("198"), {"", "uncorrectable", true}  },
          {SSK("199"), {"", "CRC errors", true}     },
  };
  static constexpr StaticStringMap FIELDS{FIELD_ENTRIES};
  constexpr usize METADATA_WORD_COUNT = 7;

  usize position = 0;
  while (position < report.length) {
    let const line = report.next_line(position).trim_blanks();
    usize word_position = 0;
    let const attribute_id = line.next_ascii_whitespace_word(word_position);
    let const field = FIELDS.find(attribute_id);
    if (!field.has_value()) continue;
    if (line.next_ascii_whitespace_word(word_position).is_empty()) continue;

    bool has_metadata = true;
    for (usize index = 0; index < METADATA_WORD_COUNT; index++) {
      if (!line.next_ascii_whitespace_word(word_position).is_empty()) continue;
      has_metadata = false;
      break;
    }
    if (!has_metadata) continue;

    let const value = line.next_ascii_whitespace_word(word_position);
    append_smart_statistic(statistics, warning_statistics, *field, value);
  }
}

fn format_smart_statistics(StringView report, String &warning_statistics,
                           Allocator allocator) throws -> String
{
  static constexpr smart_statistic_field REPORT_FIELDS[] = {
      {"Temperature",                     "temperature",      false},
      {"Percentage Used",                 "used",             false},
      {"Power On Hours",                  "power-on hours",   false},
      {"Unsafe Shutdowns",                "unsafe shutdowns", false},
      {"Media and Data Integrity Errors", "media errors",     true },
      {"Data Units Read",                 "read",             false},
      {"Data Units Written",              "written",          false},
  };
  let statistics = String{allocator};
  for (let const &field : REPORT_FIELDS) {
    let const value = report_value(report, &field.report_name, 1, allocator);
    append_smart_statistic(statistics, warning_statistics, field, value.view());
  }
  append_ata_smart_statistics(report, statistics, warning_statistics);

  if (statistics.is_empty()) statistics += "-";
  return statistics;
}

pure fn smart_status_is_healthy(StringView status) wontthrow -> bool
{
  static constexpr PackedStringKey HEALTHY_STATUS_KEYS[] = {
      SSK("Verified"), SSK("PASSED"), SSK("OK"), SSK("0x00")};
  static constexpr StaticStringSet HEALTHY_STATUSES{HEALTHY_STATUS_KEYS};
  return HEALTHY_STATUSES.contains(status);
}

fn parse_smart_report(StringView report, StringView fallback_device,
                      smart_row &row, Allocator allocator) throws -> bool
{
  static constexpr StringView DEVICE_NAMES[] = {"Device Identifier"};
  static constexpr StringView STATUS_NAMES[] = {
      "SMART Status", "SMART overall-health self-assessment test result",
      "SMART Health Status", "Critical Warning"};
  static constexpr StringView MODEL_NAMES[] = {"Device / Media Name",
                                               "Device Model", "Model Number"};
  static constexpr StringView PROTOCOL_NAMES[] = {"Protocol",
                                                  "Transport protocol"};

  row.status =
      report_value(report, STATUS_NAMES, countof(STATUS_NAMES), allocator);
  if (row.status.is_empty()) return false;

  row.device =
      report_value(report, DEVICE_NAMES, countof(DEVICE_NAMES), allocator);
  if (row.device.is_empty()) row.device = String{allocator, fallback_device};
  row.model =
      report_value(report, MODEL_NAMES, countof(MODEL_NAMES), allocator);
  if (row.model.is_empty()) row.model = String{allocator, "-"};
  row.protocol =
      report_value(report, PROTOCOL_NAMES, countof(PROTOCOL_NAMES), allocator);
  if (row.protocol.is_empty()) row.protocol = String{allocator, "-"};
  row.statistics =
      format_smart_statistics(report, row.warning_statistics, allocator);
  return true;
}

fn read_smart_rows(EvalContext &cxt,
                   const ArrayList<os::mounted_filesystem> &filesystems,
                   Allocator allocator) throws -> ArrayList<smart_row>
{
  let rows = ArrayList<smart_row>{allocator};
  let const smartctl = resolve_util_program(cxt, "smartctl");
#if defined __APPLE__
  let const diskutil = resolve_util_program(cxt, "diskutil");
#endif

  for (let const &filesystem : filesystems) {
    let row = smart_row{};
    bool has_row = false;
    if (smartctl.has_value() && filesystem.source.starts_with("/dev/")) {
      let arguments = ArrayList<String>{heap_allocator()};
      arguments.push(String{"-a"});
      arguments.push(filesystem.source.clone());
      let const report = capture_util_program_output(
          *smartctl, steal(arguments), 10'000'000'000);
      if (report.has_value())
        has_row = parse_smart_report(report->view(), filesystem.source.view(),
                                     row, allocator);
    }
#if defined __APPLE__
    if (!has_row && diskutil.has_value()) {
      let arguments = ArrayList<String>{heap_allocator()};
      arguments.push(String{"info"});
      arguments.push(filesystem.target.clone());
      let const report = capture_util_program_output(
          *diskutil, steal(arguments), 10'000'000'000);
      if (report.has_value())
        has_row = parse_smart_report(report->view(), filesystem.source.view(),
                                     row, allocator);
    }
#endif
    if (!has_row) continue;

    bool is_duplicate = false;
    for (let const &existing : rows) {
      if (existing.device != row.device) continue;
      is_duplicate = true;
      break;
    }
    if (!is_duplicate) rows.push(steal(row));
  }

  return rows;
}

} // namespace

EvilDisk::EvilDisk() = default;

pure fn EvilDisk::kind() const wontthrow -> Utility::Kind
{
  return Kind::EvilDisk;
}

fn EvilDisk::execute(
    const ExecContext &ec, EvalContext &cxt, const ArrayList<String> &args,
    const ArrayList<SourceLocation> &arg_locations) const throws -> i32
{
  let operand_locations = ArrayList<SourceLocation>{cxt.scratch_allocator()};
  let const operands =
      PARSE_KOSHKIT_ARGS_WITH_LOCATIONS(args, arg_locations, operand_locations);

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  let const should_color = koshkit_should_color();
  let const allocator = cxt.scratch_allocator();

  let filesystems = ArrayList<os::mounted_filesystem>{allocator};
  if (operands.is_empty()) {
    filesystems = os::mounted_filesystems();
    filesystems.sort([](const os::mounted_filesystem &left,
                        const os::mounted_filesystem &right) {
      if (left.target != right.target) return left.target < right.target;
      return left.source < right.source;
    });
  } else {
    filesystems.reserve(operands.count());
    for (let const &operand : operands) {
      filesystems.push(os::mounted_filesystem{operand.clone(), operand.clone(),
                                              String{allocator},
                                              String{allocator}});
    }
  }

  let rows = ArrayList<disk_row>{allocator};
  rows.reserve(filesystems.count());
  i32 status = 0;
  usize skipped_permission_count = 0;
  for (usize filesystem_index = 0; filesystem_index < filesystems.count();
       filesystem_index++)
  {
    let const &mounted = filesystems[filesystem_index];
    os::filesystem_status filesystem{};
    if (!os::stat_filesystem(mounted.target.view(), filesystem)) {
      if (operands.is_empty() && os::last_system_error_is_permission_denied()) {
        skipped_permission_count++;
        continue;
      }
      let const location = operands.is_empty()
                               ? ec.source_location()
                               : operand_locations[filesystem_index];
      KOSHKIT_REPORT_ERROR_AT(location,
                              "cannot read '" + mounted.target +
                                  "': " + os::last_system_error_message());
      status = 1;
      continue;
    }

    let const total = scaled_filesystem_blocks(
        filesystem.total_blocks, filesystem.fundamental_block_size, 1);
    let const free = scaled_filesystem_blocks(
        filesystem.free_blocks, filesystem.fundamental_block_size, 1);
    let const available = scaled_filesystem_blocks(
        filesystem.available_blocks, filesystem.fundamental_block_size, 1);
    let const used = total > free ? total - free : 0;
    let const use_percent = filesystem_usage_percent(used, available);
    let row = disk_row{};
    row.source = String{allocator, mounted.source.view()};
    row.type = mounted.type.is_empty() ? String{allocator, "-"}
                                       : String{allocator, mounted.type.view()};
    row.size = format_human_size(total, allocator);
    row.used = format_human_size(used, allocator);
    row.available = format_human_size(available, allocator);
    row.use = String::from(use_percent, allocator) + "%";
    row.target = String{allocator, mounted.target.view()};
    row.use_percent = use_percent;
    rows.push(steal(row));
  }

  usize source_width = 10;
  usize type_width = 4;
  usize size_width = 4;
  usize used_width = 4;
  usize available_width = 9;
  usize use_width = 3;
  for (let const &row : rows) {
    if (row.source.length() > source_width) source_width = row.source.length();
    if (row.type.length() > type_width) type_width = row.type.length();
    if (row.size.length() > size_width) size_width = row.size.length();
    if (row.used.length() > used_width) used_width = row.used.length();
    if (row.available.length() > available_width) {
      available_width = row.available.length();
    }
    if (row.use.length() > use_width) use_width = row.use.length();
  }

  let output = String{allocator};
  let warnings = ArrayList<String>{allocator};
  output += "\n  ";
  append_report_column(output, "FILESYSTEM", source_width, false,
                       colors::ansi::BOLD_CYAN, should_color);
  output += "  ";
  append_report_column(output, "TYPE", type_width, false,
                       colors::ansi::BOLD_CYAN, should_color);
  output += "  ";
  append_report_column(output, "SIZE", size_width, true,
                       colors::ansi::BOLD_CYAN, should_color);
  output += "  ";
  append_report_column(output, "USED", used_width, true,
                       colors::ansi::BOLD_CYAN, should_color);
  output += "  ";
  append_report_column(output, "AVAILABLE", available_width, true,
                       colors::ansi::BOLD_CYAN, should_color);
  output += "  ";
  append_report_column(output, "USE", use_width, true, colors::ansi::BOLD_CYAN,
                       should_color);
  output += "  ";
  append_report_text(output, "MOUNT", colors::ansi::BOLD_CYAN, should_color);
  output += "\n";

  for (let const &row : rows) {
    output += "  ";
    append_report_column(output, row.source.view(), source_width, false,
                         colors::ansi::GREEN, should_color);
    output += "  ";
    append_report_column(output, row.type.view(), type_width, false,
                         colors::ansi::BOLD_MAGENTA, should_color);
    output += "  ";
    append_report_column(output, row.size.view(), size_width, true,
                         colors::ansi::CYAN, should_color);
    output += "  ";
    append_report_column(output, row.used.view(), used_width, true,
                         colors::ansi::CYAN, should_color);
    output += "  ";
    append_report_column(output, row.available.view(), available_width, true,
                         colors::ansi::CYAN, should_color);
    output += "  ";
    append_report_column(output, row.use.view(), use_width, true,
                         usage_style(row.use_percent), should_color);
    output += "  ";
    append_report_text(output, row.target.view(), colors::ansi::BOLD_GREEN,
                       should_color);
    output += "\n";
  }

  let disk_snapshot = os::read_disk_io_snapshot(allocator);
  disk_snapshot.disks.sort(
      [](const os::disk_io_status &left, const os::disk_io_status &right) {
        return left.name < right.name;
      });
  bool has_failure_counters = false;
  for (let const &disk : disk_snapshot.disks) {
    if (disk.has_field(os::disk_io_field::ReadErrors) ||
        disk.has_field(os::disk_io_field::WriteErrors) ||
        disk.has_field(os::disk_io_field::ReadRetries) ||
        disk.has_field(os::disk_io_field::WriteRetries))
    {
      has_failure_counters = true;
      break;
    }
  }

  output += "\n";
  if (!has_failure_counters) {
    let table = ReportTable{allocator};
    table.add("Status", "unavailable", colors::ansi::BOLD_CYAN);
    output += table.to_string(should_color, "");
  } else {
    output += "  ";
    append_report_column(output, "DEVICE", 16, false, colors::ansi::BOLD_CYAN,
                         should_color);
    constexpr StringView HEADERS[] = {
        "READ ERRORS",
        "WRITE ERRORS",
        "READ RETRIES",
        "WRITE RETRIES",
    };
    constexpr usize WIDTHS[] = {11, 12, 12, 13};
    for (usize index = 0; index < countof(HEADERS); index++) {
      output += "  ";
      append_report_column(output, HEADERS[index], WIDTHS[index], true,
                           colors::ansi::BOLD_CYAN, should_color);
    }
    output += "\n";
    for (let const &disk : disk_snapshot.disks) {
      const u64 counters[] = {
          disk.read_error_count,
          disk.write_error_count,
          disk.read_retry_count,
          disk.write_retry_count,
      };
      constexpr os::disk_io_field FIELDS[] = {
          os::disk_io_field::ReadErrors,
          os::disk_io_field::WriteErrors,
          os::disk_io_field::ReadRetries,
          os::disk_io_field::WriteRetries,
      };
      output += "  ";
      append_report_column(output, disk.name.view(), 16, false,
                           colors::ansi::BOLD_GREEN, should_color);
      for (usize index = 0; index < countof(counters); index++) {
        output += "  ";
        append_report_column(
            output,
            disk.has_field(FIELDS[index])
                ? String::from(counters[index], allocator).view()
                : StringView{"-"},
            WIDTHS[index], true,
            counters[index] == 0 ? colors::ansi::GREEN : colors::ansi::BOLD_RED,
            should_color);
      }
      output += "\n";
    }
  }

  if (FLAG_EVILDISK_ALL.is_enabled()) {
    output += "\n";
    bool has_identity = false;
    for (let const &filesystem : filesystems) {
      if (filesystem.volume_name.is_empty() &&
          filesystem.volume_uuid.is_empty())
        continue;
      has_identity = true;
      let table = ReportTable{allocator};
      table.add("Mount", filesystem.target.view(), colors::ansi::BOLD_CYAN);
      table.add("Label", filesystem.volume_name.is_empty()
                             ? StringView{"-"}
                             : filesystem.volume_name.view(),
                colors::ansi::BOLD_CYAN);
      table.add("UUID", filesystem.volume_uuid.is_empty()
                            ? StringView{"-"}
                            : filesystem.volume_uuid.view(),
                colors::ansi::BOLD_CYAN);
      output += table.to_string(should_color, "");
    }
    if (!has_identity) {
      let table = ReportTable{allocator};
      table.add("Status", "unavailable", colors::ansi::BOLD_CYAN);
      output += table.to_string(should_color, "");
    }

    output += "\n";
    bool has_filesystem_failures = false;
    for (let const &filesystem : filesystems) {
      os::filesystem_error_counters counters{};
      if (!os::read_filesystem_error_counters(filesystem.target.view(),
                                              counters))
        continue;
      has_filesystem_failures = true;
      let values = String{allocator, "read "};
      values += String::from(counters.read_count, allocator).view();
      values += ", write ";
      values += String::from(counters.write_count, allocator).view();
      values += ", flush ";
      values += String::from(counters.flush_count, allocator).view();
      values += ", corruption ";
      values += String::from(counters.corruption_count, allocator).view();
      values += ", generation ";
      values += String::from(counters.generation_count, allocator).view();
      let table = ReportTable{allocator};
      table.add("Mount", filesystem.target.view(), colors::ansi::BOLD_CYAN);
      table.add("Counters", values.view(), colors::ansi::BOLD_CYAN);
      output += table.to_string(should_color, "");
    }
    if (!has_filesystem_failures) {
      let table = ReportTable{allocator};
      table.add("Status", "unavailable", colors::ansi::BOLD_CYAN);
      output += table.to_string(should_color, "");
    }
  }

  if (FLAG_EVILDISK_ALL.is_enabled()) {
    let const smart_rows = read_smart_rows(cxt, filesystems, allocator);
    output += "\n";
    if (smart_rows.is_empty()) {
      let table = ReportTable{allocator};
      table.add("Status", "unavailable", colors::ansi::BOLD_CYAN);
      output += table.to_string(should_color, "");
    } else {
      usize device_width = 6;
      usize status_width = 6;
      usize model_width = 5;
      usize protocol_width = 8;
      for (let const &row : smart_rows) {
        if (row.device.length() > device_width)
          device_width = row.device.length();
        if (row.status.length() > status_width)
          status_width = row.status.length();
        if (row.model.length() > model_width) model_width = row.model.length();
        if (row.protocol.length() > protocol_width)
          protocol_width = row.protocol.length();
      }

      output += "  ";
      append_report_column(output, "DEVICE", device_width, false,
                           colors::ansi::BOLD_CYAN, should_color);
      output += "  ";
      append_report_column(output, "STATUS", status_width, false,
                           colors::ansi::BOLD_CYAN, should_color);
      output += "  ";
      append_report_column(output, "MODEL", model_width, false,
                           colors::ansi::BOLD_CYAN, should_color);
      output += "  ";
      append_report_column(output, "PROTOCOL", protocol_width, false,
                           colors::ansi::BOLD_CYAN, should_color);
      output += "  ";
      for (let const &row : smart_rows) {
        output += "  ";
        append_report_column(output, row.device.view(), device_width, false,
                             colors::ansi::BOLD_GREEN, should_color);
        output += "  ";
        let const status_style = smart_status_is_healthy(row.status.view())
                                     ? colors::ansi::BOLD_GREEN
                                     : colors::ansi::BOLD_YELLOW;
        append_report_column(output, row.status.view(), status_width, false,
                             status_style, should_color);
        output += "  ";
        append_report_column(output, row.model.view(), model_width, false, {},
                             should_color);
        output += "  ";
        append_report_column(output, row.protocol.view(), protocol_width, false,
                             colors::ansi::BOLD_MAGENTA, should_color);
        output += "  ";
        append_report_text(output, row.statistics.view(), colors::ansi::CYAN,
                           should_color);
        output += "\n";

        if (!smart_status_is_healthy(row.status.view())) {
          warnings.push(row.device + " reports SMART status " + row.status);
        }
        if (!row.warning_statistics.is_empty()) {
          warnings.push(row.device + " reports nonzero SMART counters " +
                        row.warning_statistics);
        }
      }
    }
  }

  ec.print_to_stdout(output);
  if (skipped_permission_count != 0) {
    let warning = String{allocator, "skipped "};
    warning += String::from(skipped_permission_count, allocator).view();
    warning += " filesystem";
    if (skipped_permission_count != 1) warning += "s";
    warning += " due to permission denied";
    show_message(Warning{warning.view()}.to_string());
  }
  for (let const &warning : warnings)
    show_message(Warning{warning.view()}.to_string());

  return status;
}

} // namespace koshka::koshkit
