/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements evilio. It reports processor, memory, paging,
 * scheduler, stall, disk, swap, and process I/O activity.
 */

#include "../CLI.hpp"
#include "../CLIColors.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Platform.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-a] [--live [seconds]] [--cumulative [seconds]] "
                   "[--ps | -NUMBER | -n count | -p pid]");

HELP_DESCRIPTION_DECL(
    "The evilio utility reports system and process I/O activity.");

static pure fn is_evilio_sample_duration(koshka::StringView value) wontthrow
    -> bool
{
  return !value.is_empty() &&
         ((value[0] >= '0' && value[0] <= '9') || value[0] == '.');
}

FLAG(EVILIO_ALL, Bool, 'a', "all", "Include sampled system activity.");
static koshka::FlagOptionalValue FLAG_EVILIO_CUMULATIVE{
    FLAG_LIST,
    '\0',
    "cumulative",
    koshka::flag_section::NoSection,
    "Collect activity over an optional window; the default is one second.",
    is_evilio_sample_duration,
    "seconds"};
FLAG(EVILIO_PS, Bool, '\0', "ps", "Show every visible process.");
static koshka::FlagOptionalValue FLAG_EVILIO_LIVE{
    FLAG_LIST,
    'l',
    "live",
    koshka::flag_section::NoSection,
    "Refresh live output at an optional interval; the default is 0.5 seconds.",
    is_evilio_sample_duration,
    "seconds"};
FLAG(EVILIO_COUNT, String, 'n', "count", "Show this many processes.");
FLAG(EVILIO_PID, String, 'p', "pid", "Show only this process.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(EvilIO);

namespace koshka::koshkit {

namespace {

struct io_row
{
  String name{heap_allocator()};
  i64 pid{0};
  os::process_io_status status{};
};

pure fn saturated_sum(u64 left, u64 right) wontthrow -> u64
{
  return left > UINT64_MAX - right ? UINT64_MAX : left + right;
}

pure fn counter_delta(u64 before, u64 after) wontthrow -> Maybe<u64>
{
  if (after < before) return None;
  return after - before;
}

pure fn counter_rate(u64 before, u64 after, u64 elapsed_nanoseconds) wontthrow
    -> Maybe<u64>
{
  let const delta = counter_delta(before, after);
  if (!delta.has_value() || elapsed_nanoseconds == 0) return None;

  return static_cast<u64>(static_cast<u128>(*delta) * 1000000000ULL /
                          elapsed_nanoseconds);
}

pure fn is_idle_process_io(const os::process_io_status &status) wontthrow
    -> bool
{
  return status.read_bytes == 0 && status.written_bytes == 0 &&
         (!status.has_operation_counts || (status.read_operation_count == 0 &&
                                           status.write_operation_count == 0));
}

fn read_process_io_rows(Allocator allocator, Maybe<i64> selected_pid,
                        bool should_include_idle) throws -> ArrayList<io_row>
{
  let rows = ArrayList<io_row>{allocator};
  let const processes = os::enumerate_processes();
  let process_ids = ArrayList<i64>{allocator};
  let process_positions = ArrayList<usize>{allocator};
  process_ids.reserve(processes.count());
  process_positions.reserve(processes.count());
  for (usize process_position = 0; process_position < processes.count();
       process_position++)
  {
    let const &process = processes[process_position];
    if (process.pid <= 0) continue;
    if (selected_pid.has_value() && process.pid != *selected_pid) continue;

    process_ids.push(process.pid);
    process_positions.push(process_position);
  }

  let statuses = ArrayList<os::process_io_status>{allocator};
  let availability = ArrayList<u8>{allocator};
  os::read_process_io_statuses(process_ids, statuses, availability);
  for (usize query_position = 0; query_position < process_ids.count();
       query_position++)
  {
    if (availability[query_position] == 0) continue;

    let const &process = processes[process_positions[query_position]];
    let const &status = statuses[query_position];
    if (!should_include_idle && is_idle_process_io(status)) {
      continue;
    }

    rows.push(io_row{
        String{allocator, process.name.view()},
        process.pid, status
    });
  }

  rows.sort([](const io_row &left, const io_row &right) {
    return left.pid < right.pid;
  });

  return rows;
}

fn sample_process_io_rows(const ArrayList<io_row> &before_rows,
                          const ArrayList<io_row> &after_rows,
                          u64 elapsed_nanoseconds, Allocator allocator) throws
    -> ArrayList<io_row>
{
  let sampled_rows = ArrayList<io_row>{allocator};
  usize before_position = 0;
  for (let const &after : after_rows) {
    while (before_position < before_rows.count() &&
           before_rows[before_position].pid < after.pid)
    {
      before_position++;
    }
    if (before_position == before_rows.count() ||
        before_rows[before_position].pid != after.pid)
    {
      continue;
    }

    let const &before = before_rows[before_position];
    let const read_rate = counter_rate(
        before.status.read_bytes, after.status.read_bytes, elapsed_nanoseconds);
    let const write_rate =
        counter_rate(before.status.written_bytes, after.status.written_bytes,
                     elapsed_nanoseconds);
    if (!read_rate.has_value() || !write_rate.has_value()) continue;

    os::process_io_status status{*read_rate, *write_rate, 0, 0, false};
    if (before.status.has_operation_counts && after.status.has_operation_counts)
    {
      let const read_operation_rate =
          counter_rate(before.status.read_operation_count,
                       after.status.read_operation_count, elapsed_nanoseconds);
      let const write_operation_rate =
          counter_rate(before.status.write_operation_count,
                       after.status.write_operation_count, elapsed_nanoseconds);
      if (read_operation_rate.has_value() && write_operation_rate.has_value()) {
        status.read_operation_count = *read_operation_rate;
        status.write_operation_count = *write_operation_rate;
        status.has_operation_counts = true;
      }
    }
    if (is_idle_process_io(status)) continue;

    sampled_rows.push(io_row{
        String{allocator, after.name.view()},
        after.pid, status
    });
  }

  sampled_rows.sort([](const io_row &left, const io_row &right) {
    let const left_total =
        saturated_sum(left.status.read_bytes, left.status.written_bytes);
    let const right_total =
        saturated_sum(right.status.read_bytes, right.status.written_bytes);
    if (left_total != right_total) return left_total > right_total;
    return left.pid < right.pid;
  });

  return sampled_rows;
}

struct live_process_row
{
  io_row row;
  u64 last_seen_nanoseconds{0};
};

fn append_process_io_rate_report(String &output, const ArrayList<io_row> &rows,
                                 usize row_limit, Allocator allocator,
                                 bool should_color) throws -> void
{
  append_report_column(output, "PID", 8, true, colors::ansi::BOLD_CYAN,
                       should_color);
  output += "  ";
  append_report_column(output, "READ/S", 10, true, colors::ansi::BOLD_CYAN,
                       should_color);
  output += "  ";
  append_report_column(output, "WRITE/S", 10, true, colors::ansi::BOLD_CYAN,
                       should_color);
  output += "  ";
  append_report_column(output, "READ IOPS", 10, true, colors::ansi::BOLD_CYAN,
                       should_color);
  output += "  ";
  append_report_column(output, "WRITE IOPS", 11, true, colors::ansi::BOLD_CYAN,
                       should_color);
  output += "  ";
  append_report_text(output, "COMMAND", colors::ansi::BOLD_CYAN, should_color);
  output += "\n";

  let const shown_count = rows.count() < row_limit ? rows.count() : row_limit;
  for (usize index = 0; index < shown_count; index++) {
    let const &row = rows[index];
    append_report_column(output, String::from(row.pid, allocator).view(), 8,
                         true, colors::ansi::BOLD_MAGENTA, should_color);
    output += "  ";
    append_report_column(
        output, format_human_size(row.status.read_bytes, allocator).view(), 10,
        true, colors::ansi::GREEN, should_color);
    output += "  ";
    append_report_column(
        output, format_human_size(row.status.written_bytes, allocator).view(),
        10, true, colors::ansi::GREEN, should_color);
    output += "  ";
    append_report_column(
        output,
        row.status.has_operation_counts
            ? String::from(row.status.read_operation_count, allocator).view()
            : StringView{"-"},
        10, true, {}, should_color);
    output += "  ";
    append_report_column(
        output,
        row.status.has_operation_counts
            ? String::from(row.status.write_operation_count, allocator).view()
            : StringView{"-"},
        11, true, {}, should_color);
    output += "  ";
    append_report_text(output, row.name.view(), colors::ansi::BOLD_CYAN,
                       should_color);
    output += "\n";
  }
}

pure fn find_disk_io_status(const os::disk_io_snapshot &snapshot,
                            StringView name) wontthrow
    -> const os::disk_io_status *
{
  for (let const &disk : snapshot.disks) {
    if (disk.name == name) return &disk;
  }

  return nullptr;
}

fn percent_text(u64 part, u64 total, Allocator allocator) throws -> String
{
  if (total == 0) return String{allocator, "0.0%"};
  let const tenths = static_cast<u64>(static_cast<u128>(part) * 1000 / total);
  let result = String::from(tenths / 10, allocator);
  result += ".";
  result += String::from(tenths % 10, allocator).view();
  result += "%";
  return result;
}

fn append_disk_io_report(String &output,
                         const os::disk_io_snapshot &before_snapshot,
                         const os::disk_io_snapshot &after_snapshot,
                         u64 elapsed_nanoseconds, bool is_sampled,
                         bool should_include_heading, Allocator allocator,
                         bool should_color) throws -> void
{
  if (after_snapshot.disks.is_empty() && !is_sampled) return;

  if (should_include_heading) {
    output += "\n  ";
  }
  append_report_column(output, "DEVICE", 16, false, colors::ansi::BOLD_CYAN,
                       should_color);
  let const do_append_header = [&](StringView text, usize width)
                                   throws -> void {
    output += "  ";
    append_report_column(output, text, width, true, colors::ansi::BOLD_CYAN,
                         should_color);
  };
  do_append_header(is_sampled ? "READ/S" : "READ", 10);
  do_append_header(is_sampled ? "WRITE/S" : "WRITTEN", 10);
  do_append_header(is_sampled ? "READ OPS/S" : "READ OPS", 11);
  do_append_header(is_sampled ? "WRITE OPS/S" : "WRITE OPS", 12);
  if (is_sampled) {
    do_append_header("BUSY", 7);
    do_append_header("READ LAT", 9);
    do_append_header("WRITE LAT", 9);
    do_append_header("AVG QUEUE", 9);
  }
  do_append_header("QUEUE", 7);
  do_append_header("ERRORS", 8);
  do_append_header("RETRIES", 8);
  output += "\n";

  for (let const &after : after_snapshot.disks) {
    let const before = find_disk_io_status(before_snapshot, after.name.view());
    if (should_include_heading) output += "  ";
    append_report_column(output, after.name.view(), 16, false,
                         colors::ansi::BOLD_GREEN, should_color);
    let read_value = Maybe<u64>{};
    let write_value = Maybe<u64>{};
    let read_operation_value = Maybe<u64>{};
    let write_operation_value = Maybe<u64>{};
    if (is_sampled) {
      if (before != nullptr) {
        if (before->has_field(os::disk_io_field::ReadBytes) &&
            after.has_field(os::disk_io_field::ReadBytes))
        {
          read_value = counter_rate(before->read_bytes, after.read_bytes,
                                    elapsed_nanoseconds);
        }
        if (before->has_field(os::disk_io_field::WrittenBytes) &&
            after.has_field(os::disk_io_field::WrittenBytes))
        {
          write_value = counter_rate(before->written_bytes, after.written_bytes,
                                     elapsed_nanoseconds);
        }
        if (before->has_field(os::disk_io_field::ReadOperations) &&
            after.has_field(os::disk_io_field::ReadOperations))
        {
          read_operation_value =
              counter_rate(before->read_operation_count,
                           after.read_operation_count, elapsed_nanoseconds);
        }
        if (before->has_field(os::disk_io_field::WriteOperations) &&
            after.has_field(os::disk_io_field::WriteOperations))
        {
          write_operation_value =
              counter_rate(before->write_operation_count,
                           after.write_operation_count, elapsed_nanoseconds);
        }
      }
    } else {
      if (after.has_field(os::disk_io_field::ReadBytes))
        read_value = after.read_bytes;
      if (after.has_field(os::disk_io_field::WrittenBytes))
        write_value = after.written_bytes;
      if (after.has_field(os::disk_io_field::ReadOperations))
        read_operation_value = after.read_operation_count;
      if (after.has_field(os::disk_io_field::WriteOperations))
        write_operation_value = after.write_operation_count;
    }
    output += "  ";
    append_report_column(output,
                         read_value.has_value()
                             ? format_human_size(*read_value, allocator).view()
                             : StringView{"-"},
                         10, true, colors::ansi::GREEN, should_color);
    output += "  ";
    append_report_column(output,
                         write_value.has_value()
                             ? format_human_size(*write_value, allocator).view()
                             : StringView{"-"},
                         10, true, colors::ansi::GREEN, should_color);
    output += "  ";
    append_report_column(
        output,
        read_operation_value.has_value()
            ? String::from(*read_operation_value, allocator).view()
            : StringView{"-"},
        11, true, {}, should_color);
    output += "  ";
    append_report_column(
        output,
        write_operation_value.has_value()
            ? String::from(*write_operation_value, allocator).view()
            : StringView{"-"},
        12, true, {}, should_color);
    if (is_sampled) {
      let busy = String{allocator};
      if (before != nullptr && elapsed_nanoseconds != 0 &&
          before->has_field(os::disk_io_field::BusyTime) &&
          after.has_field(os::disk_io_field::BusyTime))
      {
        let const delta = counter_delta(before->busy_time_nanoseconds,
                                        after.busy_time_nanoseconds);
        if (delta.has_value()) {
          busy = percent_text(
              *delta < elapsed_nanoseconds ? *delta : elapsed_nanoseconds,
              elapsed_nanoseconds, allocator);
        }
      } else if (before != nullptr && elapsed_nanoseconds != 0 &&
                 before->has_field(os::disk_io_field::IdleTime) &&
                 after.has_field(os::disk_io_field::IdleTime))
      {
        let const idle = counter_delta(before->idle_time_nanoseconds,
                                       after.idle_time_nanoseconds);
        if (idle.has_value()) {
          busy = percent_text(
              *idle < elapsed_nanoseconds ? elapsed_nanoseconds - *idle : 0,
              elapsed_nanoseconds, allocator);
        }
      }
      output += "  ";
      append_report_column(output,
                           busy.is_empty() ? StringView{"-"} : busy.view(), 7,
                           true, {}, should_color);

      let read_latency = String{allocator};
      if (before != nullptr && before->has_field(os::disk_io_field::ReadTime) &&
          after.has_field(os::disk_io_field::ReadTime) &&
          before->has_field(os::disk_io_field::ReadOperations) &&
          after.has_field(os::disk_io_field::ReadOperations))
      {
        let const time = counter_delta(before->read_time_nanoseconds,
                                       after.read_time_nanoseconds);
        let const operations = counter_delta(before->read_operation_count,
                                             after.read_operation_count);
        if (time.has_value() && operations.has_value() && *operations != 0) {
          read_latency = utils::format_duration_nanoseconds(*time / *operations,
                                                            allocator);
        }
      }
      output += "  ";
      append_report_column(output,
                           read_latency.is_empty() ? StringView{"-"}
                                                   : read_latency.view(),
                           9, true, {}, should_color);

      let write_latency = String{allocator};
      if (before != nullptr &&
          before->has_field(os::disk_io_field::WriteTime) &&
          after.has_field(os::disk_io_field::WriteTime) &&
          before->has_field(os::disk_io_field::WriteOperations) &&
          after.has_field(os::disk_io_field::WriteOperations))
      {
        let const time = counter_delta(before->write_time_nanoseconds,
                                       after.write_time_nanoseconds);
        let const operations = counter_delta(before->write_operation_count,
                                             after.write_operation_count);
        if (time.has_value() && operations.has_value() && *operations != 0) {
          write_latency = utils::format_duration_nanoseconds(
              *time / *operations, allocator);
        }
      }
      output += "  ";
      append_report_column(output,
                           write_latency.is_empty() ? StringView{"-"}
                                                    : write_latency.view(),
                           9, true, {}, should_color);

      let average_queue = String{allocator};
      if (before != nullptr && elapsed_nanoseconds != 0 &&
          before->has_field(os::disk_io_field::WeightedBusyTime) &&
          after.has_field(os::disk_io_field::WeightedBusyTime))
      {
        let const weighted =
            counter_delta(before->weighted_busy_time_nanoseconds,
                          after.weighted_busy_time_nanoseconds);
        if (weighted.has_value()) {
          let const tenths = static_cast<u64>(static_cast<u128>(*weighted) *
                                              10 / elapsed_nanoseconds);
          average_queue = String::from(tenths / 10, allocator);
          average_queue += ".";
          average_queue += String::from(tenths % 10, allocator).view();
        }
      }
      output += "  ";
      append_report_column(output,
                           average_queue.is_empty() ? StringView{"-"}
                                                    : average_queue.view(),
                           9, true, {}, should_color);
    }
    output += "  ";
    append_report_column(output,
                         after.has_field(os::disk_io_field::QueueDepth)
                             ? String::from(after.queue_depth, allocator).view()
                             : StringView{"-"},
                         7, true, {}, should_color);
    let const do_failure_total =
        [&](os::disk_io_field read_field, os::disk_io_field write_field,
            u64 os::disk_io_status::*read_member,
            u64 os::disk_io_status::*write_member) wontthrow -> Maybe<u64> {
      u64 total = 0;
      bool has_total = false;
      if (after.has_field(read_field)) {
        let value = Maybe<u64>{after.*read_member};
        if (is_sampled) {
          value = before != nullptr && before->has_field(read_field)
                      ? counter_delta(before->*read_member, after.*read_member)
                      : Maybe<u64>{};
        }
        if (value.has_value()) {
          total = saturated_sum(total, *value);
          has_total = true;
        }
      }
      if (after.has_field(write_field)) {
        let value = Maybe<u64>{after.*write_member};
        if (is_sampled) {
          value =
              before != nullptr && before->has_field(write_field)
                  ? counter_delta(before->*write_member, after.*write_member)
                  : Maybe<u64>{};
        }
        if (value.has_value()) {
          total = saturated_sum(total, *value);
          has_total = true;
        }
      }
      return has_total ? Maybe<u64>{total} : Maybe<u64>{};
    };
    let const error_count = do_failure_total(
        os::disk_io_field::ReadErrors, os::disk_io_field::WriteErrors,
        &os::disk_io_status::read_error_count,
        &os::disk_io_status::write_error_count);
    output += "  ";
    append_report_column(
        output,
        error_count.has_value() ? String::from(*error_count, allocator).view()
                                : StringView{"-"},
        8, true,
        error_count.has_value() && *error_count != 0 ? colors::ansi::BOLD_RED
                                                     : colors::ansi::GREEN,
        should_color);
    let const retry_count = do_failure_total(
        os::disk_io_field::ReadRetries, os::disk_io_field::WriteRetries,
        &os::disk_io_status::read_retry_count,
        &os::disk_io_status::write_retry_count);
    output += "  ";
    append_report_column(
        output,
        retry_count.has_value() ? String::from(*retry_count, allocator).view()
                                : StringView{"-"},
        8, true,
        retry_count.has_value() && *retry_count != 0 ? colors::ansi::BOLD_RED
                                                     : colors::ansi::GREEN,
        should_color);
    output += "\n";
  }
  if (should_include_heading) output += "\n";
}

fn run_live_process_io(const ExecContext &ec, Maybe<i64> selected_pid,
                       usize row_limit, f64 sample_duration_seconds,
                       f64 refresh_interval_seconds, f64 falloff_seconds,
                       bool is_terminal, bool should_color) throws -> i32
{
  let const allocator = heap_allocator();
  let retained = ArrayList<live_process_row>{allocator};
  let const falloff_nanoseconds =
      static_cast<u64>(falloff_seconds * 1000000000.0);
  let const sample_interval_nanoseconds =
      static_cast<u64>(sample_duration_seconds * 1000000000.0);
  let const refresh_interval_nanoseconds =
      static_cast<u64>(refresh_interval_seconds * 1000000000.0);
  u64 last_refresh_nanoseconds = os::monotonic_nanos();
  u64 last_sample_nanoseconds = last_refresh_nanoseconds;
  let before_rows = read_process_io_rows(allocator, selected_pid, true);
  if (selected_pid.has_value() && before_rows.is_empty()) return 1;

  loop
  {
    let const before_wait_nanoseconds = os::monotonic_nanos();
    let const until_sample =
        sample_interval_nanoseconds -
        (before_wait_nanoseconds - last_sample_nanoseconds <
                 sample_interval_nanoseconds
             ? before_wait_nanoseconds - last_sample_nanoseconds
             : sample_interval_nanoseconds);
    let const until_refresh =
        refresh_interval_nanoseconds -
        (before_wait_nanoseconds - last_refresh_nanoseconds <
                 refresh_interval_nanoseconds
             ? before_wait_nanoseconds - last_refresh_nanoseconds
             : refresh_interval_nanoseconds);
    let const wait_nanoseconds =
        until_sample < until_refresh ? until_sample : until_refresh;
    os::sleep_for_seconds(static_cast<f64>(wait_nanoseconds) / 1000000000.0);
    if (os::INTERRUPT_REQUESTED != 0) {
      os::INTERRUPT_REQUESTED = 0;
      return 130;
    }

    let const now = os::monotonic_nanos();
    if (now - last_sample_nanoseconds >= sample_interval_nanoseconds) {
      let after_rows = read_process_io_rows(allocator, selected_pid, true);
      if (selected_pid.has_value() && after_rows.is_empty()) return 1;

      let const sampled_rows = sample_process_io_rows(
          before_rows, after_rows, now - last_sample_nanoseconds, allocator);
      for (let const &row : sampled_rows) {
        bool is_known = false;
        for (usize index = 0; index < retained.count(); index++) {
          if (retained[index].row.pid != row.pid) continue;
          retained[index].row = row;
          retained[index].last_seen_nanoseconds = now;
          is_known = true;
          break;
        }
        if (!is_known) retained.push(live_process_row{row, now});
      }
      for (usize index = retained.count(); index > 0; index--) {
        let const position = index - 1;
        if (now - retained[position].last_seen_nanoseconds >=
            falloff_nanoseconds)
        {
          retained.remove(position);
        }
      }
      before_rows = steal(after_rows);
      last_sample_nanoseconds = now;
    }
    if (now - last_refresh_nanoseconds < refresh_interval_nanoseconds) continue;

    last_refresh_nanoseconds = now;
    retained.sort(
        [](const live_process_row &left, const live_process_row &right) {
          let const left_total = saturated_sum(left.row.status.read_bytes,
                                               left.row.status.written_bytes);
          let const right_total = saturated_sum(right.row.status.read_bytes,
                                                right.row.status.written_bytes);
          if (left_total != right_total) return left_total > right_total;
          return left.row.pid < right.row.pid;
        });
    let rows = ArrayList<io_row>{allocator};
    rows.reserve(retained.count());
    for (let const &row : retained)
      rows.push(row.row);
    let output = String{allocator};
    if (is_terminal) output += "\x1b[H\x1b[2J";
    append_process_io_rate_report(output, rows, row_limit, allocator,
                                  should_color);
    ec.print_to_stdout(output);
  }
}

struct live_disk_row
{
  os::disk_io_status status;
  u64 last_seen_nanoseconds{0};
};

fn run_live_disk_io(const ExecContext &ec, f64 sample_duration_seconds,
                    f64 refresh_interval_seconds, f64 falloff_seconds,
                    bool is_terminal, bool should_color) throws -> i32
{
  let const allocator = heap_allocator();
  let before_snapshot = os::read_disk_io_snapshot(allocator);
  let retained = ArrayList<live_disk_row>{allocator};
  let const sample_interval_nanoseconds =
      static_cast<u64>(sample_duration_seconds * 1000000000.0);
  let const falloff_nanoseconds =
      static_cast<u64>(falloff_seconds * 1000000000.0);
  let const refresh_interval_nanoseconds =
      static_cast<u64>(refresh_interval_seconds * 1000000000.0);
  u64 last_refresh_nanoseconds = os::monotonic_nanos();
  u64 last_sample_nanoseconds = last_refresh_nanoseconds;

  loop
  {
    let const now_before_wait = os::monotonic_nanos();
    let const sample_elapsed = now_before_wait - last_sample_nanoseconds;
    let const refresh_elapsed = now_before_wait - last_refresh_nanoseconds;
    let const until_sample =
        sample_interval_nanoseconds > sample_elapsed
            ? sample_interval_nanoseconds - sample_elapsed
            : 0;
    let const until_refresh =
        refresh_interval_nanoseconds > refresh_elapsed
            ? refresh_interval_nanoseconds - refresh_elapsed
            : 0;
    let const wait_nanoseconds =
        until_sample < until_refresh ? until_sample : until_refresh;
    os::sleep_for_seconds(static_cast<f64>(wait_nanoseconds) / 1000000000.0);
    if (os::INTERRUPT_REQUESTED != 0) {
      os::INTERRUPT_REQUESTED = 0;
      return 130;
    }

    let after_snapshot = os::read_disk_io_snapshot(allocator);
    let const now = os::monotonic_nanos();
    let const did_sample =
        now - last_sample_nanoseconds >= sample_interval_nanoseconds;
    if (did_sample) {
      for (let const &disk : after_snapshot.disks) {
        bool is_known = false;
        for (usize index = 0; index < retained.count(); index++) {
          if (retained[index].status.name != disk.name) continue;
          retained[index].status = disk;
          retained[index].last_seen_nanoseconds = now;
          is_known = true;
          break;
        }
        if (!is_known) retained.push(live_disk_row{disk, now});
      }
      for (usize index = retained.count(); index > 0; index--) {
        let const position = index - 1;
        if (now - retained[position].last_seen_nanoseconds >=
            falloff_nanoseconds)
        {
          retained.remove(position);
        }
      }
      last_sample_nanoseconds = now;
    }
    if (now - last_refresh_nanoseconds < refresh_interval_nanoseconds) {
      if (did_sample) before_snapshot = steal(after_snapshot);
      continue;
    }

    last_refresh_nanoseconds = now;
    let current_snapshot = os::disk_io_snapshot{};
    current_snapshot.sampled_at_nanoseconds =
        after_snapshot.sampled_at_nanoseconds;
    current_snapshot.disks.reserve(retained.count());
    for (let const &row : retained)
      current_snapshot.disks.push(row.status);
    u64 elapsed_nanoseconds = 0;
    if (after_snapshot.sampled_at_nanoseconds >=
        before_snapshot.sampled_at_nanoseconds)
    {
      elapsed_nanoseconds = after_snapshot.sampled_at_nanoseconds -
                            before_snapshot.sampled_at_nanoseconds;
    }

    let output = String{allocator};
    if (is_terminal) output += "\x1b[H\x1b[2J";
    append_disk_io_report(output, before_snapshot, current_snapshot,
                          elapsed_nanoseconds, true, false, allocator,
                          should_color);
    ec.print_to_stdout(output);
    if (did_sample) before_snapshot = steal(after_snapshot);
  }
}

fn append_rate_field(String &body, StringView name, Maybe<u64> rate,
                     StringView suffix, Allocator allocator,
                     bool should_color) throws -> void
{
  if (!rate.has_value()) return;
  let value = String::from(*rate, allocator);
  value += suffix;
  append_report_field(body, name, value.view(), colors::ansi::BOLD_CYAN,
                      should_color);
}

fn append_stall_field(String &body, StringView name, Maybe<u64> rate,
                      Allocator allocator, bool should_color) throws -> void
{
  if (!rate.has_value()) return;
  let value = String::from(*rate, allocator);
  value += " us/s (";
  value += percent_text(*rate, 1000000, allocator).view();
  value += ")";
  append_report_field(body, name, value.view(),
                      *rate == 0 ? colors::ansi::BOLD_CYAN
                                 : colors::ansi::BOLD_RED,
                      should_color);
}

fn append_process_io_report(String &output, const ArrayList<io_row> &rows,
                            usize row_limit, u64 total_read_bytes,
                            u64 total_written_bytes,
                            u64 total_read_operation_count,
                            u64 total_write_operation_count,
                            bool has_operation_counts, Allocator allocator,
                            bool should_color) throws -> void
{
  let summary = String{allocator};
  append_report_field(summary, "Visible processes",
                      String::from(rows.count(), allocator).view(),
                      colors::ansi::BOLD_CYAN, should_color);
  append_report_field(summary, "Read",
                      format_human_size(total_read_bytes, allocator).view(),
                      colors::ansi::BOLD_CYAN, should_color);
  append_report_field(summary, "Written",
                      format_human_size(total_written_bytes, allocator).view(),
                      colors::ansi::BOLD_CYAN, should_color);
  if (has_operation_counts) {
    append_report_field(
        summary, "Read operations",
        String::from(total_read_operation_count, allocator).view(),
        colors::ansi::BOLD_CYAN, should_color);
    append_report_field(
        summary, "Write operations",
        String::from(total_write_operation_count, allocator).view(),
        colors::ansi::BOLD_CYAN, should_color);
  }
  append_report_body(output, summary.view());

  output += "\n";
  output += "\n  ";
  append_report_column(output, "PID", 8, true, colors::ansi::BOLD_CYAN,
                       should_color);
  output += "  ";
  append_report_column(output, "READ", 8, true, colors::ansi::BOLD_CYAN,
                       should_color);
  output += "  ";
  append_report_column(output, "WRITTEN", 8, true, colors::ansi::BOLD_CYAN,
                       should_color);
  if (has_operation_counts) {
    output += "  ";
    append_report_column(output, "READ OPS", 10, true, colors::ansi::BOLD_CYAN,
                         should_color);
    output += "  ";
    append_report_column(output, "WRITE OPS", 10, true, colors::ansi::BOLD_CYAN,
                         should_color);
  }
  output += "  ";
  append_report_text(output, "COMMAND", colors::ansi::BOLD_CYAN, should_color);
  output += "\n";

  let const shown_count = rows.count() < row_limit ? rows.count() : row_limit;
  for (usize index = 0; index < shown_count; index++) {
    let const &row = rows[index];
    output += "  ";
    append_report_column(output, String::from(row.pid, allocator).view(), 8,
                         true, colors::ansi::BOLD_MAGENTA, should_color);
    output += "  ";
    append_report_column(
        output, format_human_size(row.status.read_bytes, allocator).view(), 8,
        true, colors::ansi::GREEN, should_color);
    output += "  ";
    append_report_column(
        output, format_human_size(row.status.written_bytes, allocator).view(),
        8, true, colors::ansi::GREEN, should_color);
    if (has_operation_counts) {
      output += "  ";
      append_report_column(
          output,
          row.status.has_operation_counts
              ? String::from(row.status.read_operation_count, allocator).view()
              : StringView{"-"},
          10, true, colors::ansi::GREEN, should_color);
      output += "  ";
      append_report_column(
          output,
          row.status.has_operation_counts
              ? String::from(row.status.write_operation_count, allocator).view()
              : StringView{"-"},
          10, true, colors::ansi::GREEN, should_color);
    }
    output += "  ";
    append_report_text(output, row.name.view(), colors::ansi::BOLD_CYAN,
                       should_color);
    output += "\n";
  }
}

} /* namespace */

EvilIO::EvilIO() = default;

pure fn EvilIO::kind() const wontthrow -> Utility::Kind { return Kind::EvilIO; }

fn EvilIO::execute(const ExecContext &ec, EvalContext &cxt,
                   const ArrayList<String> &args,
                   const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let operand_locations = ArrayList<SourceLocation>{cxt.scratch_allocator()};
  let const operands = PARSE_KOSHKIT_ARGS_WITH_LOCATIONS(
      args, arg_locations, operand_locations, true, true);

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  let const allocator = cxt.scratch_allocator();
  let process_limit_operand = Maybe<StringView>{};
  let process_limit_location = Maybe<SourceLocation>{};
  let sample_duration_operand = Maybe<StringView>{};
  let sample_duration_location = Maybe<SourceLocation>{};
  if (FLAG_EVILIO_CUMULATIVE.has_value()) {
    sample_duration_operand = FLAG_EVILIO_CUMULATIVE.value();
    sample_duration_location = FLAG_EVILIO_CUMULATIVE.value_location();
  }

  for (usize operand_index = 0; operand_index < operands.count();
       operand_index++)
  {
    let const &operand = operands[operand_index];
    let const is_process_limit = operand.length() > 1 && operand[0] == '-';
    if (is_process_limit && !process_limit_operand.has_value()) {
      process_limit_operand = operand.view();
      process_limit_location = operand_locations[operand_index];
      continue;
    }
    if (!is_process_limit && !sample_duration_operand.has_value()) {
      sample_duration_operand = operand.view();
      sample_duration_location = operand_locations[operand_index];
      continue;
    }

    KOSHKIT_REPORT_ERROR_AT(
        operand_locations[operand_index], "unexpected operand",
        "use at most one sample duration and one -NUMBER limit");
    return 1;
  }

  if (sample_duration_operand.has_value() &&
      !FLAG_EVILIO_CUMULATIVE.is_enabled())
  {
    KOSHKIT_REPORT_ERROR_AT(*sample_duration_location, "unexpected operand",
                            "a sample duration requires --cumulative");
    return 1;
  }

  f64 cumulative_duration_seconds = 1.0;
  if (sample_duration_operand.has_value()) {
    cumulative_duration_seconds = parse_koshkit_duration_seconds(
        *sample_duration_operand, *sample_duration_location, allocator);
    if (cumulative_duration_seconds <= 0.0) {
      KOSHKIT_REPORT_ERROR_AT(*sample_duration_location, "invalid duration",
                              "the duration must be greater than zero");
      return 1;
    }
  }

  usize row_limit = FLAG_EVILIO_PS.is_enabled() ? SIZE_MAX : 10;
  if (process_limit_operand.has_value()) {
    if (FLAG_EVILIO_COUNT.is_set()) {
      let conflict_location = *process_limit_location;
      if (FLAG_EVILIO_COUNT.value_location().position >
          conflict_location.position)
      {
        conflict_location = FLAG_EVILIO_COUNT.value_location();
      }
      KOSHKIT_REPORT_ERROR_AT(conflict_location, "conflicting process limits",
                              "use either -NUMBER or --count");
      return 1;
    }
    let const parsed = utils::parse_integer_in_base(
        process_limit_operand->substring(1), int_base::decimal);
    if (parsed.is_error() || parsed.value() < 1 || parsed.value() > 100000) {
      KOSHKIT_REPORT_ERROR_AT(*process_limit_location, "invalid count",
                              "the count must be from 1 through 100000");
      return 1;
    }
    row_limit = static_cast<usize>(parsed.value());
  }
  if (FLAG_EVILIO_COUNT.is_set()) {
    let const parsed = utils::parse_integer_in_base(FLAG_EVILIO_COUNT.value(),
                                                    int_base::decimal);
    if (parsed.is_error() || parsed.value() < 1 || parsed.value() > 100000) {
      KOSHKIT_REPORT_ERROR_AT(FLAG_EVILIO_COUNT.value_location(),
                              "invalid count",
                              "the count must be from 1 through 100000");
      return 1;
    }
    row_limit = static_cast<usize>(parsed.value());
  }

  Maybe<i64> selected_pid;
  if (FLAG_EVILIO_PID.is_set()) {
    let const parsed = utils::parse_integer_in_base(FLAG_EVILIO_PID.value(),
                                                    int_base::decimal);
    if (parsed.is_error() || parsed.value() <= 0) {
      KOSHKIT_REPORT_ERROR_AT(FLAG_EVILIO_PID.value_location(),
                              "invalid process id",
                              "the process id must be a positive integer");
      return 1;
    }
    selected_pid = parsed.value();
  }

  let const should_color = koshkit_should_color();
  f64 live_interval_seconds = 0.5;
  if (FLAG_EVILIO_LIVE.has_value()) {
    let const parsed = utils::parse_decimal_f64(FLAG_EVILIO_LIVE.value());
    if (parsed.is_error() || parsed.value() <= 0) {
      KOSHKIT_REPORT_ERROR_AT(FLAG_EVILIO_LIVE.value_location(),
                              "invalid live interval",
                              "use a positive number of seconds");
      return 1;
    }
    live_interval_seconds = parsed.value();
  }
  let const sample_duration_seconds = FLAG_EVILIO_CUMULATIVE.is_enabled()
                                          ? cumulative_duration_seconds
                                          : live_interval_seconds;
  let const refresh_interval_seconds = live_interval_seconds;
  let const falloff_seconds =
      live_interval_seconds > 5.0 ? live_interval_seconds * 3.0 : 5.0;
  let const should_show_processes =
      FLAG_EVILIO_PS.is_enabled() || FLAG_EVILIO_COUNT.is_set() ||
      selected_pid.has_value() || process_limit_operand.has_value();

  if (FLAG_EVILIO_ALL.is_enabled() &&
      (FLAG_EVILIO_CUMULATIVE.is_enabled() || FLAG_EVILIO_LIVE.is_enabled()))
  {
    let conflict_location = FLAG_EVILIO_ALL.value_location();
    if (FLAG_EVILIO_CUMULATIVE.position() > FLAG_EVILIO_ALL.position())
      conflict_location = FLAG_EVILIO_CUMULATIVE.value_location();
    if (FLAG_EVILIO_LIVE.position() > FLAG_EVILIO_ALL.position() &&
        FLAG_EVILIO_LIVE.position() > FLAG_EVILIO_CUMULATIVE.position())
    {
      conflict_location = FLAG_EVILIO_LIVE.value_location();
    }
    KOSHKIT_REPORT_ERROR_AT(conflict_location, "conflicting report modes",
                            "use --all without --cumulative or --live");
    return 1;
  }

  if (FLAG_EVILIO_LIVE.is_enabled()) {
    let const is_terminal = colors::stdout_is_a_terminal();
    bool is_alternate_screen_active = false;
    if (is_terminal) is_alternate_screen_active = enter_alternate_screen(ec);
    defer
    {
      if (is_alternate_screen_active) leave_alternate_screen(ec);
    };

    if (should_show_processes) {
      return run_live_process_io(
          ec, selected_pid, row_limit, sample_duration_seconds,
          refresh_interval_seconds, falloff_seconds, is_terminal, should_color);
    }

    return run_live_disk_io(ec, sample_duration_seconds,
                            refresh_interval_seconds, falloff_seconds,
                            is_terminal, should_color);
  }

  if (FLAG_EVILIO_CUMULATIVE.is_enabled() && should_show_processes) {
    let const before_rows = read_process_io_rows(allocator, selected_pid, true);
    let const started_at_nanoseconds = os::monotonic_nanos();
    os::sleep_for_seconds(cumulative_duration_seconds);
    if (os::INTERRUPT_REQUESTED != 0) {
      os::INTERRUPT_REQUESTED = 0;
      return 130;
    }
    let const after_rows = read_process_io_rows(allocator, selected_pid, true);
    let const elapsed_nanoseconds =
        os::monotonic_nanos() - started_at_nanoseconds;
    let const sampled_rows = sample_process_io_rows(
        before_rows, after_rows, elapsed_nanoseconds, allocator);
    let output = String{allocator};
    append_process_io_rate_report(output, sampled_rows, row_limit, allocator,
                                  should_color);
    ec.print_to_stdout(output);
    return selected_pid.has_value() && after_rows.is_empty() ? 1 : 0;
  }

  if (should_show_processes) {
    let rows = read_process_io_rows(allocator, selected_pid, false);
    u64 total_read_bytes = 0;
    u64 total_written_bytes = 0;
    u64 total_read_operation_count = 0;
    u64 total_write_operation_count = 0;
    bool has_operation_counts = false;
    for (let const &row : rows) {
      total_read_bytes = saturated_sum(total_read_bytes, row.status.read_bytes);
      total_written_bytes =
          saturated_sum(total_written_bytes, row.status.written_bytes);
      if (row.status.has_operation_counts) {
        total_read_operation_count = saturated_sum(
            total_read_operation_count, row.status.read_operation_count);
        total_write_operation_count = saturated_sum(
            total_write_operation_count, row.status.write_operation_count);
        has_operation_counts = true;
      }
    }
    rows.sort([](const io_row &left, const io_row &right) {
      let const left_total =
          saturated_sum(left.status.read_bytes, left.status.written_bytes);
      let const right_total =
          saturated_sum(right.status.read_bytes, right.status.written_bytes);
      if (left_total != right_total) return left_total > right_total;
      return left.pid < right.pid;
    });

    let output = String{allocator};
    append_process_io_report(output, rows, row_limit, total_read_bytes,
                             total_written_bytes, total_read_operation_count,
                             total_write_operation_count, has_operation_counts,
                             allocator, should_color);
    ec.print_to_stdout(output);
    return selected_pid.has_value() && rows.is_empty() ? 1 : 0;
  }

  os::system_activity_status activity_before{};
  os::system_activity_status activity_after{};
  os::swap_status swap_before{};
  os::swap_status swap_after{};
  os::disk_io_snapshot disk_before{};
  os::disk_io_snapshot disk_after{};
  bool has_activity_before = false;
  bool has_activity_after = false;
  bool has_swap_before = false;
  bool has_swap_after = false;
  u64 elapsed_nanoseconds = 0;
  disk_after = os::read_disk_io_snapshot(allocator);
  if (FLAG_EVILIO_ALL.is_enabled()) {
    has_activity_before = os::read_system_activity_status(activity_before);
    has_swap_before = os::read_swap_status(swap_before);
    disk_before = steal(disk_after);
    os::sleep_for_seconds(cumulative_duration_seconds);
    has_activity_after = os::read_system_activity_status(activity_after);
    disk_after = os::read_disk_io_snapshot(allocator);
    has_swap_after = os::read_swap_status(swap_after);
    if (disk_after.sampled_at_nanoseconds >= disk_before.sampled_at_nanoseconds)
    {
      elapsed_nanoseconds = disk_after.sampled_at_nanoseconds -
                            disk_before.sampled_at_nanoseconds;
    }
  } else if (FLAG_EVILIO_CUMULATIVE.is_enabled()) {
    disk_before = steal(disk_after);
    os::sleep_for_seconds(cumulative_duration_seconds);
    if (os::INTERRUPT_REQUESTED != 0) {
      os::INTERRUPT_REQUESTED = 0;
      return 130;
    }
    disk_after = os::read_disk_io_snapshot(allocator);
    if (disk_after.sampled_at_nanoseconds >= disk_before.sampled_at_nanoseconds)
    {
      elapsed_nanoseconds = disk_after.sampled_at_nanoseconds -
                            disk_before.sampled_at_nanoseconds;
    }
  } else {
    has_swap_after = os::read_swap_status(swap_after);
  }

  os::memory_status memory{};
  let const has_memory_status =
      !FLAG_EVILIO_CUMULATIVE.is_enabled() && os::read_memory_status(memory);

  let output = String{allocator};
  if (has_activity_before && has_activity_after &&
      activity_before.has_field(os::system_activity_field::Cpu) &&
      activity_after.has_field(os::system_activity_field::Cpu))
  {
    let const user = counter_delta(activity_before.cpu_user_units,
                                   activity_after.cpu_user_units);
    let const system = counter_delta(activity_before.cpu_system_units,
                                     activity_after.cpu_system_units);
    let const idle = counter_delta(activity_before.cpu_idle_units,
                                   activity_after.cpu_idle_units);
    if (user.has_value() && system.has_value() && idle.has_value()) {
      let wait = Maybe<u64>{};
      if (activity_before.has_field(os::system_activity_field::CpuWait) &&
          activity_after.has_field(os::system_activity_field::CpuWait))
      {
        wait = counter_delta(activity_before.cpu_wait_units,
                             activity_after.cpu_wait_units);
      }
      let stolen = Maybe<u64>{};
      if (activity_before.has_field(os::system_activity_field::CpuStolen) &&
          activity_after.has_field(os::system_activity_field::CpuStolen))
      {
        stolen = counter_delta(activity_before.cpu_stolen_units,
                               activity_after.cpu_stolen_units);
      }
      u64 total = saturated_sum(saturated_sum(*user, *system), *idle);
      if (wait.has_value()) total = saturated_sum(total, *wait);
      if (stolen.has_value()) total = saturated_sum(total, *stolen);
      let body = String{allocator};
      append_report_field(body, "User", percent_text(*user, total, allocator),
                          colors::ansi::BOLD_CYAN, should_color);
      append_report_field(body, "System",
                          percent_text(*system, total, allocator),
                          colors::ansi::BOLD_CYAN, should_color);
      append_report_field(body, "Idle", percent_text(*idle, total, allocator),
                          colors::ansi::BOLD_CYAN, should_color);
      if (wait.has_value()) {
        append_report_field(body, "Wait", percent_text(*wait, total, allocator),
                            colors::ansi::BOLD_CYAN, should_color);
      }
      if (stolen.has_value()) {
        append_report_field(body, "Stolen",
                            percent_text(*stolen, total, allocator),
                            colors::ansi::BOLD_CYAN, should_color);
      }
      append_report_body(output, body.view());
      output += "\n";
    }
  }

  let memory_body = String{allocator};
  if (has_memory_status) {
    let const total_bytes = memory.total_kib > UINT64_MAX / 1024
                                ? UINT64_MAX
                                : memory.total_kib * 1024;
    let const available_bytes = memory.available_kib > UINT64_MAX / 1024
                                    ? UINT64_MAX
                                    : memory.available_kib * 1024;
    let const free_bytes = memory.free_kib > UINT64_MAX / 1024
                               ? UINT64_MAX
                               : memory.free_kib * 1024;
    let const used_bytes =
        total_bytes > available_bytes ? total_bytes - available_bytes : 0;
    let status = format_human_size(used_bytes, allocator);
    status += " / ";
    status += format_human_size(total_bytes, allocator).view();
    status += " (";
    status += percent_text(used_bytes, total_bytes, allocator).view();
    status += ")";
    append_report_field(memory_body, "Used", status.view(),
                        colors::ansi::BOLD_CYAN, should_color);
    append_report_field(memory_body, "Available",
                        format_human_size(available_bytes, allocator),
                        colors::ansi::BOLD_CYAN, should_color);
    append_report_field(memory_body, "Free",
                        format_human_size(free_bytes, allocator),
                        colors::ansi::BOLD_CYAN, should_color);
  }
  if (has_activity_before && has_activity_after) {
    if (activity_before.has_field(os::system_activity_field::PageScan) &&
        activity_after.has_field(os::system_activity_field::PageScan))
    {
      append_rate_field(memory_body, "Pages scanned",
                        counter_rate(activity_before.page_scan_count,
                                     activity_after.page_scan_count,
                                     elapsed_nanoseconds),
                        "/s", allocator, should_color);
    }
    if (activity_before.has_field(os::system_activity_field::PageSteal) &&
        activity_after.has_field(os::system_activity_field::PageSteal))
    {
      append_rate_field(memory_body, "Pages reclaimed",
                        counter_rate(activity_before.page_steal_count,
                                     activity_after.page_steal_count,
                                     elapsed_nanoseconds),
                        "/s", allocator, should_color);
    }
    if (activity_before.has_field(os::system_activity_field::DirectReclaim) &&
        activity_after.has_field(os::system_activity_field::DirectReclaim))
    {
      append_rate_field(memory_body, "Direct reclaim stalls",
                        counter_rate(activity_before.direct_reclaim_count,
                                     activity_after.direct_reclaim_count,
                                     elapsed_nanoseconds),
                        "/s", allocator, should_color);
    }
    if (activity_before.has_field(os::system_activity_field::CompactionStall) &&
        activity_after.has_field(os::system_activity_field::CompactionStall))
    {
      append_rate_field(memory_body, "Compaction stalls",
                        counter_rate(activity_before.compaction_stall_count,
                                     activity_after.compaction_stall_count,
                                     elapsed_nanoseconds),
                        "/s", allocator, should_color);
    }
    if (activity_before.has_field(os::system_activity_field::OomKills) &&
        activity_after.has_field(os::system_activity_field::OomKills))
    {
      append_rate_field(memory_body, "OOM kills",
                        counter_rate(activity_before.oom_kill_count,
                                     activity_after.oom_kill_count,
                                     elapsed_nanoseconds),
                        "/s", allocator, should_color);
    }
    if (activity_after.has_field(os::system_activity_field::DirtyPages)) {
      append_report_field(
          memory_body, "Dirty pages",
          String::from(activity_after.dirty_page_count, allocator),
          colors::ansi::BOLD_CYAN, should_color);
    }
    if (activity_after.has_field(os::system_activity_field::WritebackPages)) {
      append_report_field(
          memory_body, "Writeback pages",
          String::from(activity_after.writeback_page_count, allocator),
          colors::ansi::BOLD_CYAN, should_color);
    }
  }
  if (!memory_body.is_empty()) {
    append_report_body(output, memory_body.view());
    output += "\n";
  }

  let paging_body = String{allocator};
  if (has_activity_before && has_activity_after) {
    if (activity_before.has_field(os::system_activity_field::PageInput) &&
        activity_after.has_field(os::system_activity_field::PageInput))
    {
      let const rate =
          counter_rate(activity_before.page_input_bytes,
                       activity_after.page_input_bytes, elapsed_nanoseconds);
      if (rate.has_value()) {
        append_report_field(paging_body, "Input",
                            (format_human_size(*rate, allocator) + "/s").view(),
                            colors::ansi::BOLD_CYAN, should_color);
      }
    }
    if (activity_before.has_field(os::system_activity_field::PageOutput) &&
        activity_after.has_field(os::system_activity_field::PageOutput))
    {
      let const rate =
          counter_rate(activity_before.page_output_bytes,
                       activity_after.page_output_bytes, elapsed_nanoseconds);
      if (rate.has_value()) {
        append_report_field(paging_body, "Output",
                            (format_human_size(*rate, allocator) + "/s").view(),
                            colors::ansi::BOLD_CYAN, should_color);
      }
    }
    if (activity_before.has_field(os::system_activity_field::Faults) &&
        activity_after.has_field(os::system_activity_field::Faults))
    {
      append_rate_field(paging_body, "Faults",
                        counter_rate(activity_before.page_fault_count,
                                     activity_after.page_fault_count,
                                     elapsed_nanoseconds),
                        "/s", allocator, should_color);
    }
    if (activity_before.has_field(os::system_activity_field::MajorFaults) &&
        activity_after.has_field(os::system_activity_field::MajorFaults))
    {
      append_rate_field(paging_body, "Major faults",
                        counter_rate(activity_before.major_page_fault_count,
                                     activity_after.major_page_fault_count,
                                     elapsed_nanoseconds),
                        "/s", allocator, should_color);
    }
  }
  if (!paging_body.is_empty()) {
    append_report_body(output, paging_body.view());
    output += "\n";
  }

  let scheduler_body = String{allocator};
  if (has_activity_after) {
    if (activity_after.has_field(os::system_activity_field::Runnable)) {
      let runnable =
          String::from(activity_after.runnable_process_count, allocator);
      runnable += " for ";
      runnable +=
          String::from(os::get_processor_counts().online_count, allocator)
              .view();
      runnable += " processors";
      append_report_field(scheduler_body, "Runnable", runnable.view(),
                          colors::ansi::BOLD_CYAN, should_color);
    }
    if (activity_after.has_field(os::system_activity_field::Blocked)) {
      append_report_field(
          scheduler_body, "I/O blocked",
          String::from(activity_after.blocked_process_count, allocator),
          colors::ansi::BOLD_CYAN, should_color);
    }
  }
  if (!scheduler_body.is_empty()) {
    append_report_body(output, scheduler_body.view());
    output += "\n";
  }

  let stalls = String{allocator};
  if (has_activity_before && has_activity_after) {
    if (activity_before.has_field(os::system_activity_field::CpuSomeStall) &&
        activity_after.has_field(os::system_activity_field::CpuSomeStall))
    {
      append_stall_field(
          stalls, "CPU some",
          counter_rate(activity_before.cpu_some_stall_microseconds,
                       activity_after.cpu_some_stall_microseconds,
                       elapsed_nanoseconds),
          allocator, should_color);
    }
    if (activity_before.has_field(os::system_activity_field::CpuFullStall) &&
        activity_after.has_field(os::system_activity_field::CpuFullStall))
    {
      append_stall_field(
          stalls, "CPU full",
          counter_rate(activity_before.cpu_full_stall_microseconds,
                       activity_after.cpu_full_stall_microseconds,
                       elapsed_nanoseconds),
          allocator, should_color);
    }
    if (activity_before.has_field(os::system_activity_field::MemorySomeStall) &&
        activity_after.has_field(os::system_activity_field::MemorySomeStall))
    {
      append_stall_field(
          stalls, "Memory some",
          counter_rate(activity_before.memory_some_stall_microseconds,
                       activity_after.memory_some_stall_microseconds,
                       elapsed_nanoseconds),
          allocator, should_color);
    }
    if (activity_before.has_field(os::system_activity_field::MemoryFullStall) &&
        activity_after.has_field(os::system_activity_field::MemoryFullStall))
    {
      append_stall_field(
          stalls, "Memory full",
          counter_rate(activity_before.memory_full_stall_microseconds,
                       activity_after.memory_full_stall_microseconds,
                       elapsed_nanoseconds),
          allocator, should_color);
    }
    if (activity_before.has_field(os::system_activity_field::IoSomeStall) &&
        activity_after.has_field(os::system_activity_field::IoSomeStall))
    {
      append_stall_field(
          stalls, "I/O some",
          counter_rate(activity_before.io_some_stall_microseconds,
                       activity_after.io_some_stall_microseconds,
                       elapsed_nanoseconds),
          allocator, should_color);
    }
    if (activity_before.has_field(os::system_activity_field::IoFullStall) &&
        activity_after.has_field(os::system_activity_field::IoFullStall))
    {
      append_stall_field(
          stalls, "I/O full",
          counter_rate(activity_before.io_full_stall_microseconds,
                       activity_after.io_full_stall_microseconds,
                       elapsed_nanoseconds),
          allocator, should_color);
    }
  }
  if (!stalls.is_empty()) {
    append_report_body(output, stalls.view());
    output += "\n";
  }

  if (!disk_after.disks.is_empty() || FLAG_EVILIO_CUMULATIVE.is_enabled()) {
    append_disk_io_report(
        output, disk_before, disk_after, elapsed_nanoseconds,
        FLAG_EVILIO_ALL.is_enabled() || FLAG_EVILIO_CUMULATIVE.is_enabled(),
        !FLAG_EVILIO_CUMULATIVE.is_enabled(), allocator, should_color);
  }

  if (FLAG_EVILIO_CUMULATIVE.is_enabled()) {
    ec.print_to_stdout(output);
    return 0;
  }

  let swap_body = String{allocator};
  if (!has_swap_after) {
    append_report_field(swap_body, "Status", "unavailable",
                        colors::ansi::BOLD_CYAN, should_color);
  } else {
    let utilization = String::from(
        swap_after.total_bytes == 0
            ? u64{0}
            : static_cast<u64>(static_cast<u128>(swap_after.used_bytes) * 100 /
                               swap_after.total_bytes),
        allocator);
    utilization += "%";
    let status = format_human_size(swap_after.used_bytes, allocator);
    status += " / ";
    status += format_human_size(swap_after.total_bytes, allocator).view();
    status += " (";
    status += utilization.view();
    status += "), ";
    status += format_human_size(swap_after.free_bytes, allocator).view();
    status += " free";
    append_report_inline_field(swap_body, "Status", status.view(),
                               colors::ansi::BOLD_CYAN, should_color);
    swap_body += "\n";
    if (FLAG_EVILIO_ALL.is_enabled() && swap_after.has_activity) {
      let activity = String{allocator};
      if (FLAG_EVILIO_ALL.is_enabled() && has_swap_before &&
          swap_before.has_activity)
      {
        let const input_rate =
            counter_rate(swap_before.input_bytes, swap_after.input_bytes,
                         elapsed_nanoseconds);
        let const output_rate =
            counter_rate(swap_before.output_bytes, swap_after.output_bytes,
                         elapsed_nanoseconds);
        if (input_rate.has_value() && output_rate.has_value()) {
          activity = format_human_size(*input_rate, allocator);
          activity += "/s in, ";
          activity += format_human_size(*output_rate, allocator).view();
          activity += "/s out";
        }
      }
      if (activity.is_empty()) {
        activity = format_human_size(swap_after.input_bytes, allocator);
        activity += " in, ";
        activity +=
            format_human_size(swap_after.output_bytes, allocator).view();
        activity += " out";
      }
      append_report_inline_field(swap_body, "Activity", activity.view(),
                                 colors::ansi::BOLD_CYAN, should_color);
      swap_body += "\n";
    }
    if (swap_after.has_encryption_state) {
      append_report_field(swap_body, "Encryption",
                          swap_after.is_encrypted ? "enabled" : "disabled",
                          colors::ansi::BOLD_CYAN, should_color);
    }
  }
  append_report_body(output, swap_body.view());

  ec.print_to_stdout(output);
  return 0;
}

} /* namespace koshka::koshkit */
