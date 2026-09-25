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
#include "../base/Arena.hpp"
#include "../base/StaticStringMap.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-ah] [-C [seconds]] [--live [seconds]] [--cumulative "
                   "[seconds]] [--sort key] "
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
FLAG(EVILIO_HUMAN, Bool, 'h', "human-readable",
     "Print byte values with compact binary units such as 4.0K or 1.5M.");
FLAG_OPTIONAL(EVILIO_CUMULATIVE, 'C', "cumulative", Live,
              "Use an M-second rolling window for sampled activity; the "
              "default is one second.",
              is_evilio_sample_duration, "seconds");
FLAG(EVILIO_PS, Bool, '\0', "ps", "Show every visible process.");
FLAG_OPTIONAL(EVILIO_LIVE, 'l', "live", Live,
              "Refresh live output every N seconds; default 0.5 seconds while "
              "sampling.",
              is_evilio_sample_duration, "seconds");
FLAG(EVILIO_COUNT, String, 'n', "count", "Show this many processes.");
FLAG(EVILIO_PID, String, 'p', "pid", "Show only this process.");
FLAG(EVILIO_SORT, String, '\0', "sort",
     "Sort by a full metric name or its shortest unambiguous prefix.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(EvilIO);

namespace koshka::koshkit {

namespace {

enum class evilio_sort_key : u8
{
  Pid,
  Read,
  Write,
  ReadOperations,
  WriteOperations,
  Busy,
  ReadLatency,
  WriteLatency,
  AverageQueue,
  Queue,
  Errors,
  Retries,
};

struct evilio_sort_spec
{
  evilio_sort_key key;
  const char *name;
  bool is_process_metric;
  bool is_disk_metric;
};

static constexpr static_string_entry<evilio_sort_spec> SORT_KEY_ENTRIES[] = {
    {SSK("average-queue"),
     {evilio_sort_key::AverageQueue, "average-queue", false, true}           },
    {SSK("busy"),          {evilio_sort_key::Busy, "busy", false, true}      },
    {SSK("errors"),        {evilio_sort_key::Errors, "errors", false, true}  },
    {SSK("pid"),           {evilio_sort_key::Pid, "pid", true, false}        },
    {SSK("queue"),         {evilio_sort_key::Queue, "queue", false, true}    },
    {SSK("read"),          {evilio_sort_key::Read, "read", true, true}       },
    {SSK("read-latency"),
     {evilio_sort_key::ReadLatency, "read-latency", false, true}             },
    {SSK("read-ops"),
     {evilio_sort_key::ReadOperations, "read-ops", true, true}               },
    {SSK("retries"),       {evilio_sort_key::Retries, "retries", false, true}},
    {SSK("write"),         {evilio_sort_key::Write, "write", true, true}     },
    {SSK("write-latency"),
     {evilio_sort_key::WriteLatency, "write-latency", false, true}           },
    {SSK("write-ops"),
     {evilio_sort_key::WriteOperations, "write-ops", true, true}             },
};

static constexpr StaticStringMap SORT_KEYS{SORT_KEY_ENTRIES};

struct evilio_sort_resolution
{
  Maybe<evilio_sort_key> key{};
  usize match_count{0};
  String matches{heap_allocator()};
};

fn resolve_sort_key(StringView value, Allocator allocator) throws
    -> evilio_sort_resolution
{
  evilio_sort_resolution result{};
  result.matches = String{allocator};
  if (let const exact = SORT_KEYS.find(value); exact.has_value()) {
    result.key = exact->key;
    result.match_count = 1;
    result.matches += exact->name;
    return result;
  }

  for (let const &entry : SORT_KEY_ENTRIES) {
    if (!StringView{entry.value.name}.starts_with(value)) continue;
    if (!result.matches.is_empty()) result.matches += ", ";
    result.matches += entry.value.name;
    result.key = entry.value.key;
    result.match_count++;
  }

  return result;
}

pure fn find_sort_spec(evilio_sort_key key) wontthrow
    -> const evilio_sort_spec *
{
  for (let const &entry : SORT_KEY_ENTRIES) {
    if (entry.value.key == key) return &entry.value;
  }

  return nullptr;
}

struct io_row
{
  String name{heap_allocator()};
  i64 pid{0};
  u64 start_token{0};
  os::process_io_status status{};
};

pure fn saturated_sum(u64 left, u64 right) wontthrow -> u64
{
  return left > UINT64_MAX - right ? UINT64_MAX : left + right;
}

pure fn process_sort_value(const io_row &row, evilio_sort_key key) wontthrow
    -> Maybe<u64>
{
  switch (key) {
  case evilio_sort_key::Pid: return static_cast<u64>(row.pid);
  case evilio_sort_key::Read: return row.status.read_bytes;
  case evilio_sort_key::Write: return row.status.written_bytes;
  case evilio_sort_key::ReadOperations:
    return row.status.has_operation_counts
               ? Maybe<u64>{row.status.read_operation_count}
               : Maybe<u64>{};
  case evilio_sort_key::WriteOperations:
    return row.status.has_operation_counts
               ? Maybe<u64>{row.status.write_operation_count}
               : Maybe<u64>{};
  default: return None;
  }
}

fn sort_process_rows(ArrayList<io_row> &rows,
                     Maybe<evilio_sort_key> sort_key) throws -> void
{
  let const do_compare = [sort_key](const io_row &left, const io_row &right) {
    if (!sort_key.has_value()) {
      let const left_total =
          saturated_sum(left.status.read_bytes, left.status.written_bytes);
      let const right_total =
          saturated_sum(right.status.read_bytes, right.status.written_bytes);
      if (left_total != right_total) return left_total > right_total;
      return left.pid < right.pid;
    }

    if (*sort_key == evilio_sort_key::Pid) return left.pid < right.pid;
    let const left_value = process_sort_value(left, *sort_key);
    let const right_value = process_sort_value(right, *sort_key);
    if (left_value.has_value() != right_value.has_value())
      return left_value.has_value();
    if (left_value.has_value() && *left_value != *right_value) {
      return *left_value > *right_value;
    }
    return left.pid < right.pid;
  };
  rows.sort(do_compare);
}

pure fn counter_delta(u64 before, u64 after) wontthrow -> Maybe<u64>
{
  if (after < before) return None;
  return after - before;
}

pure fn process_io_counter_reset(const os::process_io_status &before,
                                 const os::process_io_status &after) wontthrow
    -> bool
{
  if (after.read_bytes < before.read_bytes ||
      after.written_bytes < before.written_bytes)
    return true;
  return before.has_operation_counts && after.has_operation_counts &&
         (after.read_operation_count < before.read_operation_count ||
          after.write_operation_count < before.write_operation_count);
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
        process.pid,
        process.start_token, status
    });
  }

  rows.sort([](const io_row &left, const io_row &right) {
    return left.pid < right.pid;
  });

  return rows;
}

fn sample_process_io_rows(const ArrayList<io_row> &before_rows,
                          const ArrayList<io_row> &after_rows,
                          u64 elapsed_nanoseconds, Allocator allocator,
                          Maybe<evilio_sort_key> sort_key) throws
    -> ArrayList<io_row>
{
  unused(elapsed_nanoseconds);
  let sampled_rows = ArrayList<io_row>{allocator};
  usize before_position = 0;
  for (let const &after : after_rows) {
    while (before_position < before_rows.count() &&
           before_rows[before_position].pid < after.pid)
    {
      before_position++;
    }
    if (before_position == before_rows.count() ||
        before_rows[before_position].pid != after.pid ||
        before_rows[before_position].start_token != after.start_token)
    {
      continue;
    }

    let const &before = before_rows[before_position];
    let const read_delta =
        counter_delta(before.status.read_bytes, after.status.read_bytes);
    let const write_delta =
        counter_delta(before.status.written_bytes, after.status.written_bytes);
    if (!read_delta.has_value() || !write_delta.has_value()) continue;

    os::process_io_status status{*read_delta, *write_delta, 0, 0, false};
    if (before.status.has_operation_counts && after.status.has_operation_counts)
    {
      let const read_operation_delta =
          counter_delta(before.status.read_operation_count,
                        after.status.read_operation_count);
      let const write_operation_delta =
          counter_delta(before.status.write_operation_count,
                        after.status.write_operation_count);
      if (read_operation_delta.has_value() && write_operation_delta.has_value())
      {
        status.read_operation_count = *read_operation_delta;
        status.write_operation_count = *write_operation_delta;
        status.has_operation_counts = true;
      }
    }
    if (is_idle_process_io(status)) continue;

    sampled_rows.push(io_row{
        String{allocator, after.name.view()},
        after.pid, after.start_token,
        status
    });
  }

  sort_process_rows(sampled_rows, sort_key);

  return sampled_rows;
}

struct live_process_row
{
  i64 pid{0};
  u64 start_token{0};
  String name{heap_allocator()};
  ArrayList<os::process_io_status> history{heap_allocator()};
  ArrayList<u64> history_nanoseconds{heap_allocator()};
  u64 last_seen_nanoseconds{0};
};

fn get_process_window_status(const live_process_row &row,
                             u64 window_start_nanoseconds) wontthrow
    -> os::process_io_status
{
  let const boundary = find_rolling_window_boundary(row.history_nanoseconds,
                                                    window_start_nanoseconds);
  let const &before = row.history[boundary.before_index];
  let const &after_boundary = row.history[boundary.after_index];
  let const &newest = row.history.back();
  os::process_io_status status{0, 0, 0, 0, false};

  let const get_baseline = [&](u64 before_value, u64 after_value) {
    return interpolate_rolling_counter(
        before_value, after_value,
        row.history_nanoseconds[boundary.before_index],
        row.history_nanoseconds[boundary.after_index], boundary.timestamp);
  };
  if (let const baseline =
          get_baseline(before.read_bytes, after_boundary.read_bytes);
      baseline.has_value())
  {
    if (let const delta = counter_delta(*baseline, newest.read_bytes);
        delta.has_value())
      status.read_bytes = *delta;
  }
  if (let const baseline =
          get_baseline(before.written_bytes, after_boundary.written_bytes);
      baseline.has_value())
  {
    if (let const delta = counter_delta(*baseline, newest.written_bytes);
        delta.has_value())
      status.written_bytes = *delta;
  }
  if (before.has_operation_counts && after_boundary.has_operation_counts &&
      newest.has_operation_counts)
  {
    let const read_baseline = get_baseline(before.read_operation_count,
                                           after_boundary.read_operation_count);
    let const write_baseline = get_baseline(
        before.write_operation_count, after_boundary.write_operation_count);
    if (read_baseline.has_value() && write_baseline.has_value()) {
      let const read_delta =
          counter_delta(*read_baseline, newest.read_operation_count);
      let const write_delta =
          counter_delta(*write_baseline, newest.write_operation_count);
      if (read_delta.has_value() && write_delta.has_value()) {
        status.read_operation_count = *read_delta;
        status.write_operation_count = *write_delta;
        status.has_operation_counts = true;
      }
    }
  }
  return status;
}

fn append_process_io_rate_report(
    String &output, const ArrayList<io_row> &rows, usize row_limit,
    Allocator allocator, bool should_color, StringView duration_suffix,
    const ArrayList<u64> *idle_nanoseconds_list) throws -> void
{
  unused(idle_nanoseconds_list);
  let table = ReportTable{allocator};
  table.add_column("PID", report_table_alignment::Right,
                   colors::ansi::BOLD_CYAN);
  table.add_column(String{"READ"} + duration_suffix,
                   report_table_alignment::Right, colors::ansi::BOLD_CYAN);
  table.add_column(String{"WRITE"} + duration_suffix,
                   report_table_alignment::Right, colors::ansi::BOLD_CYAN);
  table.add_column(String{"READ OPS"} + duration_suffix,
                   report_table_alignment::Right, colors::ansi::BOLD_CYAN);
  table.add_column(String{"WRITE OPS"} + duration_suffix,
                   report_table_alignment::Right, colors::ansi::BOLD_CYAN);
  table.add_column("COMMAND", report_table_alignment::Left,
                   colors::ansi::BOLD_CYAN);

  let const shown_count = rows.count() < row_limit ? rows.count() : row_limit;
  for (usize index = 0; index < shown_count; index++) {
    let const &row = rows[index];
    let const pid = String::from(row.pid, allocator);
    let const read = format_human_size(row.status.read_bytes, allocator);
    let const write = format_human_size(row.status.written_bytes, allocator);
    let const read_operations =
        row.status.has_operation_counts
            ? String::from(row.status.read_operation_count, allocator)
            : String{allocator, "-"};
    let const write_operations =
        row.status.has_operation_counts
            ? String::from(row.status.write_operation_count, allocator)
            : String{allocator, "-"};
    let cells = ArrayList<report_table_cell_view>{allocator};
    cells.push({pid.view(), colors::ansi::BOLD_MAGENTA});
    cells.push({read.view(), colors::ansi::GREEN});
    cells.push({write.view(), colors::ansi::GREEN});
    cells.push({read_operations.view(), {}});
    cells.push({write_operations.view(), {}});
    cells.push({row.name.view(), colors::ansi::BOLD_CYAN});
    table.add_row(cells);
  }
  append_titled_report_table(output, "Process I/O", table, should_color);
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

struct disk_io_row
{
  String name{heap_allocator()};
  Maybe<u64> read{};
  Maybe<u64> write{};
  Maybe<u64> read_operations{};
  Maybe<u64> write_operations{};
  Maybe<u64> busy_tenths{};
  Maybe<u64> read_latency_nanoseconds{};
  Maybe<u64> write_latency_nanoseconds{};
  Maybe<u64> average_queue_tenths{};
  Maybe<u64> queue{};
  Maybe<u64> errors{};
  Maybe<u64> retries{};
};

pure fn disk_failure_total(
    const os::disk_io_status *before, const os::disk_io_status &after,
    os::disk_io_field read_field, os::disk_io_field write_field,
    u64 os::disk_io_status::*read_member, u64 os::disk_io_status::*write_member,
    report_sampling_mode sampling) wontthrow -> Maybe<u64>
{
  u64 total = 0;
  bool has_total = false;
  if (after.has_field(read_field)) {
    let value = Maybe<u64>{after.*read_member};
    if (sampling == report_sampling_mode::Rolling) {
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
    if (sampling == report_sampling_mode::Rolling) {
      value = before != nullptr && before->has_field(write_field)
                  ? counter_delta(before->*write_member, after.*write_member)
                  : Maybe<u64>{};
    }
    if (value.has_value()) {
      total = saturated_sum(total, *value);
      has_total = true;
    }
  }

  return has_total ? Maybe<u64>{total} : Maybe<u64>{};
}

fn make_disk_io_row(const os::disk_io_status *before,
                    const os::disk_io_status &after, u64 elapsed_nanoseconds,
                    Allocator allocator, report_sampling_mode sampling) throws
    -> disk_io_row
{
  disk_io_row row{};
  row.name = String{allocator, after.name.view()};
  if (sampling == report_sampling_mode::Rolling) {
    if (before != nullptr) {
      if (before->has_field(os::disk_io_field::ReadBytes) &&
          after.has_field(os::disk_io_field::ReadBytes))
      {
        row.read = counter_delta(before->read_bytes, after.read_bytes);
      }
      if (before->has_field(os::disk_io_field::WrittenBytes) &&
          after.has_field(os::disk_io_field::WrittenBytes))
      {
        row.write = counter_delta(before->written_bytes, after.written_bytes);
      }
      if (before->has_field(os::disk_io_field::ReadOperations) &&
          after.has_field(os::disk_io_field::ReadOperations))
      {
        row.read_operations = counter_delta(before->read_operation_count,
                                            after.read_operation_count);
      }
      if (before->has_field(os::disk_io_field::WriteOperations) &&
          after.has_field(os::disk_io_field::WriteOperations))
      {
        row.write_operations = counter_delta(before->write_operation_count,
                                             after.write_operation_count);
      }
    }
  } else {
    if (after.has_field(os::disk_io_field::ReadBytes))
      row.read = after.read_bytes;
    if (after.has_field(os::disk_io_field::WrittenBytes))
      row.write = after.written_bytes;
    if (after.has_field(os::disk_io_field::ReadOperations))
      row.read_operations = after.read_operation_count;
    if (after.has_field(os::disk_io_field::WriteOperations))
      row.write_operations = after.write_operation_count;
  }

  if (sampling == report_sampling_mode::Rolling && before != nullptr &&
      elapsed_nanoseconds != 0)
  {
    if (before->has_field(os::disk_io_field::BusyTime) &&
        after.has_field(os::disk_io_field::BusyTime))
    {
      if (let const delta = counter_delta(before->busy_time_nanoseconds,
                                          after.busy_time_nanoseconds);
          delta.has_value())
      {
        let const busy =
            *delta < elapsed_nanoseconds ? *delta : elapsed_nanoseconds;
        row.busy_tenths = static_cast<u64>(static_cast<u128>(busy) * 1000 /
                                           elapsed_nanoseconds);
      }
    } else if (before->has_field(os::disk_io_field::IdleTime) &&
               after.has_field(os::disk_io_field::IdleTime))
    {
      if (let const idle = counter_delta(before->idle_time_nanoseconds,
                                         after.idle_time_nanoseconds);
          idle.has_value())
      {
        let const busy =
            *idle < elapsed_nanoseconds ? elapsed_nanoseconds - *idle : 0;
        row.busy_tenths = static_cast<u64>(static_cast<u128>(busy) * 1000 /
                                           elapsed_nanoseconds);
      }
    }

    if (before->has_field(os::disk_io_field::ReadTime) &&
        after.has_field(os::disk_io_field::ReadTime) &&
        before->has_field(os::disk_io_field::ReadOperations) &&
        after.has_field(os::disk_io_field::ReadOperations))
    {
      let const time = counter_delta(before->read_time_nanoseconds,
                                     after.read_time_nanoseconds);
      let const operations = counter_delta(before->read_operation_count,
                                           after.read_operation_count);
      if (time.has_value() && operations.has_value() && *operations != 0) {
        row.read_latency_nanoseconds = *time / *operations;
      }
    }
    if (before->has_field(os::disk_io_field::WriteTime) &&
        after.has_field(os::disk_io_field::WriteTime) &&
        before->has_field(os::disk_io_field::WriteOperations) &&
        after.has_field(os::disk_io_field::WriteOperations))
    {
      let const time = counter_delta(before->write_time_nanoseconds,
                                     after.write_time_nanoseconds);
      let const operations = counter_delta(before->write_operation_count,
                                           after.write_operation_count);
      if (time.has_value() && operations.has_value() && *operations != 0) {
        row.write_latency_nanoseconds = *time / *operations;
      }
    }
    if (before->has_field(os::disk_io_field::WeightedBusyTime) &&
        after.has_field(os::disk_io_field::WeightedBusyTime))
    {
      if (let const weighted =
              counter_delta(before->weighted_busy_time_nanoseconds,
                            after.weighted_busy_time_nanoseconds);
          weighted.has_value())
      {
        row.average_queue_tenths = static_cast<u64>(
            static_cast<u128>(*weighted) * 10 / elapsed_nanoseconds);
      }
    }
  }

  if (after.has_field(os::disk_io_field::QueueDepth))
    row.queue = after.queue_depth;
  row.errors = disk_failure_total(
      before, after, os::disk_io_field::ReadErrors,
      os::disk_io_field::WriteErrors, &os::disk_io_status::read_error_count,
      &os::disk_io_status::write_error_count, sampling);
  row.retries = disk_failure_total(
      before, after, os::disk_io_field::ReadRetries,
      os::disk_io_field::WriteRetries, &os::disk_io_status::read_retry_count,
      &os::disk_io_status::write_retry_count, sampling);
  return row;
}

fn make_disk_io_rows(const os::disk_io_snapshot &before_snapshot,
                     const os::disk_io_snapshot &after_snapshot,
                     u64 elapsed_nanoseconds, Allocator allocator,
                     report_sampling_mode sampling) throws
    -> ArrayList<disk_io_row>
{
  let rows = ArrayList<disk_io_row>{allocator};
  rows.reserve(after_snapshot.disks.count());
  for (let const &after : after_snapshot.disks) {
    let const before = find_disk_io_status(before_snapshot, after.name.view());
    rows.push(make_disk_io_row(before, after, elapsed_nanoseconds, allocator,
                               sampling));
  }

  return rows;
}

pure fn disk_sort_value(const disk_io_row &row, evilio_sort_key key) wontthrow
    -> Maybe<u64>
{
  switch (key) {
  case evilio_sort_key::Read: return row.read;
  case evilio_sort_key::Write: return row.write;
  case evilio_sort_key::ReadOperations: return row.read_operations;
  case evilio_sort_key::WriteOperations: return row.write_operations;
  case evilio_sort_key::Busy: return row.busy_tenths;
  case evilio_sort_key::ReadLatency: return row.read_latency_nanoseconds;
  case evilio_sort_key::WriteLatency: return row.write_latency_nanoseconds;
  case evilio_sort_key::AverageQueue: return row.average_queue_tenths;
  case evilio_sort_key::Queue: return row.queue;
  case evilio_sort_key::Errors: return row.errors;
  case evilio_sort_key::Retries: return row.retries;
  default: return None;
  }
}

pure fn disk_io_counter_reset(const os::disk_io_status &before,
                              const os::disk_io_status &after) wontthrow -> bool
{
  return after.read_bytes < before.read_bytes ||
         after.written_bytes < before.written_bytes ||
         after.read_operation_count < before.read_operation_count ||
         after.write_operation_count < before.write_operation_count ||
         after.read_time_nanoseconds < before.read_time_nanoseconds ||
         after.write_time_nanoseconds < before.write_time_nanoseconds ||
         after.busy_time_nanoseconds < before.busy_time_nanoseconds ||
         after.idle_time_nanoseconds < before.idle_time_nanoseconds ||
         after.weighted_busy_time_nanoseconds <
             before.weighted_busy_time_nanoseconds ||
         after.read_error_count < before.read_error_count ||
         after.write_error_count < before.write_error_count ||
         after.read_retry_count < before.read_retry_count ||
         after.write_retry_count < before.write_retry_count;
}

fn sort_disk_rows(ArrayList<disk_io_row> &rows,
                  Maybe<evilio_sort_key> sort_key) throws -> void
{
  if (!sort_key.has_value()) return;
  let const do_compare = [sort_key](const disk_io_row &left,
                                    const disk_io_row &right) {
    let const left_value = disk_sort_value(left, *sort_key);
    let const right_value = disk_sort_value(right, *sort_key);
    if (left_value.has_value() != right_value.has_value())
      return left_value.has_value();
    if (left_value.has_value() && *left_value != *right_value) {
      return *left_value > *right_value;
    }
    return left.name.view() < right.name.view();
  };
  rows.sort(do_compare);
}

pure fn sort_key_needs_sample(evilio_sort_key key) wontthrow -> bool
{
  switch (key) {
  case evilio_sort_key::Busy:
  case evilio_sort_key::ReadLatency:
  case evilio_sort_key::WriteLatency:
  case evilio_sort_key::AverageQueue: return true;
  default: return false;
  }
}

fn tenths_text(u64 tenths, Allocator allocator,
               bool should_append_percent) throws -> String
{
  let result = String::from(tenths / 10, allocator);
  result += ".";
  result += String::from(tenths % 10, allocator).view();
  if (should_append_percent) result += "%";
  return result;
}

fn append_disk_io_report(String &output, const ArrayList<disk_io_row> &rows,
                         Allocator allocator, bool should_color,
                         StringView duration_suffix,
                         report_sampling_mode sampling) throws -> void
{
  if (rows.is_empty() && sampling == report_sampling_mode::Instant) return;

  let table = ReportTable{allocator};
  table.add_column("DEVICE", report_table_alignment::Left,
                   colors::ansi::BOLD_CYAN);
  table.add_column(sampling == report_sampling_mode::Rolling
                       ? String{"READ"} + duration_suffix
                       : "READ",
                   report_table_alignment::Right, colors::ansi::BOLD_CYAN);
  table.add_column(sampling == report_sampling_mode::Rolling
                       ? String{"WRITE"} + duration_suffix
                       : "WRITTEN",
                   report_table_alignment::Right, colors::ansi::BOLD_CYAN);
  table.add_column(sampling == report_sampling_mode::Rolling
                       ? String{"READ OPS"} + duration_suffix
                       : "READ OPS",
                   report_table_alignment::Right, colors::ansi::BOLD_CYAN);
  table.add_column(sampling == report_sampling_mode::Rolling
                       ? String{"WRITE OPS"} + duration_suffix
                       : "WRITE OPS",
                   report_table_alignment::Right, colors::ansi::BOLD_CYAN);
  if (sampling == report_sampling_mode::Rolling) {
    table.add_column("BUSY", report_table_alignment::Right,
                     colors::ansi::BOLD_CYAN);
    table.add_column("READ LATENCY", report_table_alignment::Right,
                     colors::ansi::BOLD_CYAN);
    table.add_column("WRITE LATENCY", report_table_alignment::Right,
                     colors::ansi::BOLD_CYAN);
    table.add_column("AVG QUEUE", report_table_alignment::Right,
                     colors::ansi::BOLD_CYAN);
  }
  table.add_column("QUEUE", report_table_alignment::Right,
                   colors::ansi::BOLD_CYAN);
  table.add_column("ERRORS", report_table_alignment::Right,
                   colors::ansi::BOLD_CYAN);
  table.add_column("RETRIES", report_table_alignment::Right,
                   colors::ansi::BOLD_CYAN);

  for (let const &row : rows) {
    let const read = row.read.has_value()
                         ? format_human_size(*row.read, allocator)
                         : String{allocator, "-"};
    let const write = row.write.has_value()
                          ? format_human_size(*row.write, allocator)
                          : String{allocator, "-"};
    let const read_operations =
        row.read_operations.has_value()
            ? String::from(*row.read_operations, allocator)
            : String{allocator, "-"};
    let const write_operations =
        row.write_operations.has_value()
            ? String::from(*row.write_operations, allocator)
            : String{allocator, "-"};
    let cells = ArrayList<report_table_cell_view>{allocator};
    cells.push({row.name.view(), colors::ansi::BOLD_GREEN});
    cells.push({read.view(), colors::ansi::GREEN});
    cells.push({write.view(), colors::ansi::GREEN});
    cells.push({read_operations.view(), {}});
    cells.push({write_operations.view(), {}});
    let busy = String{allocator};
    let read_latency = String{allocator};
    let write_latency = String{allocator};
    let average_queue = String{allocator};
    if (sampling == report_sampling_mode::Rolling) {
      busy = row.busy_tenths.has_value()
                 ? tenths_text(*row.busy_tenths, allocator, true)
                 : String{allocator, "-"};
      read_latency = row.read_latency_nanoseconds.has_value()
                         ? utils::format_duration_nanoseconds(
                               *row.read_latency_nanoseconds, allocator)
                         : String{allocator, "-"};
      write_latency = row.write_latency_nanoseconds.has_value()
                          ? utils::format_duration_nanoseconds(
                                *row.write_latency_nanoseconds, allocator)
                          : String{allocator, "-"};
      average_queue =
          row.average_queue_tenths.has_value()
              ? tenths_text(*row.average_queue_tenths, allocator, false)
              : String{allocator, "-"};
      cells.push({busy.view(), {}});
      cells.push({read_latency.view(), {}});
      cells.push({write_latency.view(), {}});
      cells.push({average_queue.view(), {}});
    }
    let const queue = row.queue.has_value()
                          ? String::from(*row.queue, allocator)
                          : String{allocator, "-"};
    let const errors = row.errors.has_value()
                           ? String::from(*row.errors, allocator)
                           : String{allocator, "-"};
    let const retries = row.retries.has_value()
                            ? String::from(*row.retries, allocator)
                            : String{allocator, "-"};
    cells.push({queue.view(), {}});
    cells.push({errors.view(), row.errors.has_value() && *row.errors != 0
                                   ? colors::ansi::BOLD_RED
                                   : colors::ansi::GREEN});
    cells.push({retries.view(), row.retries.has_value() && *row.retries != 0
                                    ? colors::ansi::BOLD_RED
                                    : colors::ansi::GREEN});
    table.add_row(cells);
  }
  append_titled_report_table(output, "Disk I/O", table, should_color);
}
fn run_live_process_io(const ExecContext &ec, Maybe<i64> selected_pid,
                       usize row_limit, Maybe<evilio_sort_key> sort_key,
                       f64 window_seconds, f64 sample_interval_seconds,
                       f64 refresh_interval_seconds, bool is_terminal,
                       bool should_color,
                       StringView sample_duration_label) throws -> i32
{
  let const allocator = heap_allocator();
  let frame_arena = BumpArena{};
  let retained = ArrayList<live_process_row>{allocator};
  let const falloff_nanoseconds =
      static_cast<u64>(window_seconds * 1000000000.0);
  let const sample_interval_nanoseconds =
      static_cast<u64>(sample_interval_seconds * 1000000000.0);
  let const refresh_interval_nanoseconds =
      static_cast<u64>(refresh_interval_seconds * 1000000000.0);
  u64 last_refresh_nanoseconds = os::monotonic_nanos();
  u64 last_sample_nanoseconds = last_refresh_nanoseconds;
  let const sample_label = format_live_duration(window_seconds, allocator);
  let const refresh_label =
      format_live_duration(refresh_interval_seconds, allocator);
  let baseline_rows = read_process_io_rows(allocator, selected_pid, true);
  if (os::INTERRUPT_REQUESTED != 0) {
    os::INTERRUPT_REQUESTED = 0;
    return 130;
  }
  if (selected_pid.has_value() && baseline_rows.is_empty()) return 1;
  for (let const &row : baseline_rows) {
    live_process_row entry{};
    entry.pid = row.pid;
    entry.start_token = row.start_token;
    entry.name = String{allocator, row.name.view()};
    entry.history.push(row.status);
    entry.history_nanoseconds.push(last_sample_nanoseconds);
    entry.last_seen_nanoseconds = last_sample_nanoseconds;
    retained.push(steal(entry));
  }

  loop
  {
    let const frame_mark = frame_arena.mark();
    defer { frame_arena.release(frame_mark); };
    let const frame_allocator = bump_allocator(frame_arena);

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
      let after_rows =
          read_process_io_rows(frame_allocator, selected_pid, true);
      if (os::INTERRUPT_REQUESTED != 0) {
        os::INTERRUPT_REQUESTED = 0;
        return 130;
      }
      if (selected_pid.has_value() && after_rows.is_empty()) return 1;

      for (let const &row : after_rows) {
        bool is_known = false;
        for (usize index = 0; index < retained.count(); index++) {
          if (retained[index].pid != row.pid ||
              retained[index].start_token != row.start_token)
            continue;
          if (process_io_counter_reset(retained[index].history.back(),
                                       row.status))
          {
            retained[index].history.clear();
            retained[index].history_nanoseconds.clear();
          }
          retained[index].history.push(row.status);
          retained[index].history_nanoseconds.push(now);
          retained[index].last_seen_nanoseconds = now;
          is_known = true;
          break;
        }
        if (!is_known) {
          live_process_row entry{};
          entry.pid = row.pid;
          entry.start_token = row.start_token;
          entry.name = String{allocator, row.name.view()};
          entry.history.push(row.status);
          entry.history_nanoseconds.push(now);
          entry.last_seen_nanoseconds = now;
          retained.push(steal(entry));
        }
      }
      for (usize index = retained.count(); index > 0; index--) {
        let const position = index - 1;
        if (retained[position].history_nanoseconds.back() != now) {
          retained[position].history.push(retained[position].history.back());
          retained[position].history_nanoseconds.push(now);
        }
        if (now - retained[position].last_seen_nanoseconds >=
            falloff_nanoseconds)
        {
          retained.remove(position);
          continue;
        }
        trim_rolling_history(retained[position].history,
                             retained[position].history_nanoseconds,
                             rolling_window_start(now, falloff_nanoseconds));
      }
      last_sample_nanoseconds = now;
    }
    if (now - last_refresh_nanoseconds < refresh_interval_nanoseconds) continue;

    last_refresh_nanoseconds = now;
    let rows = ArrayList<io_row>{frame_allocator};
    rows.reserve(retained.count());
    let const window_start =
        rolling_window_start(last_sample_nanoseconds, falloff_nanoseconds);
    for (let const &row : retained) {
      rows.push(io_row{
          String{frame_allocator, row.name.view()},
          row.pid, row.start_token,
          get_process_window_status(row, window_start)
      });
    }
    sort_process_rows(rows, sort_key);
    let output = String{frame_allocator};
    if (is_terminal) output += "\x1b[H\x1b[2J";
    append_live_controls_bar(output, sample_label.view(), refresh_label.view(),
                             should_color);
    append_process_io_rate_report(output, rows, row_limit, frame_allocator,
                                  should_color, sample_duration_label, nullptr);
    ec.print_to_stdout(output);
  }
}

struct live_disk_row
{
  String name{heap_allocator()};
  ArrayList<os::disk_io_status> history{heap_allocator()};
  ArrayList<u64> history_nanoseconds{heap_allocator()};
  u64 last_seen_nanoseconds{0};
};

fn make_disk_window_row(const live_disk_row &row, u64 window_start_nanoseconds,
                        Allocator allocator) throws -> disk_io_row
{
  let const boundary = find_rolling_window_boundary(row.history_nanoseconds,
                                                    window_start_nanoseconds);
  let const &lower = row.history[boundary.before_index];
  let const &upper = row.history[boundary.after_index];
  let const &newest = row.history.back();
  os::disk_io_status before{};
  before.name = String{allocator, row.name.view()};
  before.available_fields = lower.available_fields & upper.available_fields;
  let const do_interpolate = [&](os::disk_io_field field,
                                 u64 os::disk_io_status::*member) {
    if (!lower.has_field(field) || !upper.has_field(field)) return;
    if (let const value = interpolate_rolling_counter(
            lower.*member, upper.*member,
            row.history_nanoseconds[boundary.before_index],
            row.history_nanoseconds[boundary.after_index], boundary.timestamp);
        value.has_value())
      before.*member = *value;
  };
  do_interpolate(os::disk_io_field::ReadBytes, &os::disk_io_status::read_bytes);
  do_interpolate(os::disk_io_field::WrittenBytes,
                 &os::disk_io_status::written_bytes);
  do_interpolate(os::disk_io_field::ReadOperations,
                 &os::disk_io_status::read_operation_count);
  do_interpolate(os::disk_io_field::WriteOperations,
                 &os::disk_io_status::write_operation_count);
  do_interpolate(os::disk_io_field::ReadTime,
                 &os::disk_io_status::read_time_nanoseconds);
  do_interpolate(os::disk_io_field::WriteTime,
                 &os::disk_io_status::write_time_nanoseconds);
  do_interpolate(os::disk_io_field::BusyTime,
                 &os::disk_io_status::busy_time_nanoseconds);
  do_interpolate(os::disk_io_field::IdleTime,
                 &os::disk_io_status::idle_time_nanoseconds);
  do_interpolate(os::disk_io_field::WeightedBusyTime,
                 &os::disk_io_status::weighted_busy_time_nanoseconds);
  do_interpolate(os::disk_io_field::ReadErrors,
                 &os::disk_io_status::read_error_count);
  do_interpolate(os::disk_io_field::WriteErrors,
                 &os::disk_io_status::write_error_count);
  do_interpolate(os::disk_io_field::ReadRetries,
                 &os::disk_io_status::read_retry_count);
  do_interpolate(os::disk_io_field::WriteRetries,
                 &os::disk_io_status::write_retry_count);
  let const elapsed_nanoseconds =
      row.history_nanoseconds.back() > boundary.timestamp
          ? row.history_nanoseconds.back() - boundary.timestamp
          : 0;
  return make_disk_io_row(&before, newest, elapsed_nanoseconds, allocator,
                          report_sampling_mode::Rolling);
}

fn run_live_disk_io(const ExecContext &ec, f64 window_seconds,
                    f64 sample_interval_seconds, f64 refresh_interval_seconds,
                    bool is_terminal, bool should_color,
                    StringView sample_duration_label,
                    Maybe<evilio_sort_key> sort_key) throws -> i32
{
  let const allocator = heap_allocator();
  let frame_arena = BumpArena{};
  let retained = ArrayList<live_disk_row>{allocator};
  let const falloff_nanoseconds =
      static_cast<u64>(window_seconds * 1000000000.0);
  let const sample_interval_nanoseconds =
      static_cast<u64>(sample_interval_seconds * 1000000000.0);
  let const refresh_interval_nanoseconds =
      static_cast<u64>(refresh_interval_seconds * 1000000000.0);
  u64 last_refresh_nanoseconds = os::monotonic_nanos();
  u64 last_sample_nanoseconds = last_refresh_nanoseconds;
  let const sample_label = format_live_duration(window_seconds, allocator);
  let const refresh_label =
      format_live_duration(refresh_interval_seconds, allocator);
  let baseline_snapshot = os::read_disk_io_snapshot(allocator);
  for (let const &disk : baseline_snapshot.disks) {
    live_disk_row entry{};
    entry.name = String{allocator, disk.name.view()};
    entry.history.push(disk);
    entry.history_nanoseconds.push(last_sample_nanoseconds);
    entry.last_seen_nanoseconds = last_sample_nanoseconds;
    retained.push(steal(entry));
  }

  loop
  {
    let const frame_mark = frame_arena.mark();
    defer { frame_arena.release(frame_mark); };
    let const frame_allocator = bump_allocator(frame_arena);

    let const now_before_wait = os::monotonic_nanos();
    let const sample_elapsed = now_before_wait - last_sample_nanoseconds;
    let const refresh_elapsed = now_before_wait - last_refresh_nanoseconds;
    let const until_sample = sample_interval_nanoseconds > sample_elapsed
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

    let const now = os::monotonic_nanos();
    let const did_sample =
        now - last_sample_nanoseconds >= sample_interval_nanoseconds;
    if (did_sample) {
      let after_snapshot = os::read_disk_io_snapshot(allocator);
      for (let const &disk : after_snapshot.disks) {
        bool is_known = false;
        for (usize index = 0; index < retained.count(); index++) {
          if (retained[index].name != disk.name) continue;
          if (disk_io_counter_reset(retained[index].history.back(), disk)) {
            retained[index].history.clear();
            retained[index].history_nanoseconds.clear();
          }
          retained[index].history.push(disk);
          retained[index].history_nanoseconds.push(now);
          retained[index].last_seen_nanoseconds = now;
          is_known = true;
          break;
        }
        if (!is_known) {
          live_disk_row entry{};
          entry.name = String{allocator, disk.name.view()};
          entry.history.push(disk);
          entry.history_nanoseconds.push(now);
          entry.last_seen_nanoseconds = now;
          retained.push(steal(entry));
        }
      }
      for (usize index = retained.count(); index > 0; index--) {
        let const position = index - 1;
        if (retained[position].history_nanoseconds.back() != now) {
          retained[position].history.push(retained[position].history.back());
          retained[position].history_nanoseconds.push(now);
        }
        if (now - retained[position].last_seen_nanoseconds >=
            falloff_nanoseconds)
        {
          retained.remove(position);
          continue;
        }
        trim_rolling_history(retained[position].history,
                             retained[position].history_nanoseconds,
                             rolling_window_start(now, falloff_nanoseconds));
      }
      last_sample_nanoseconds = now;
    }
    if (now - last_refresh_nanoseconds < refresh_interval_nanoseconds) {
      continue;
    }

    last_refresh_nanoseconds = now;
    let const window_start =
        rolling_window_start(last_sample_nanoseconds, falloff_nanoseconds);
    let rows = ArrayList<disk_io_row>{frame_allocator};
    rows.reserve(retained.count());
    for (let const &row : retained)
      rows.push(make_disk_window_row(row, window_start, frame_allocator));
    sort_disk_rows(rows, sort_key);

    let output = String{frame_allocator};
    if (is_terminal) output += "\x1b[H\x1b[2J";
    append_live_controls_bar(output, sample_label.view(), refresh_label.view(),
                             should_color);
    append_disk_io_report(output, rows, frame_allocator, should_color,
                          sample_duration_label, report_sampling_mode::Rolling);
    ec.print_to_stdout(output);
  }
}

fn make_metric_table(Allocator allocator) throws -> ReportTable
{
  let table = ReportTable{allocator};
  table.add_column("METRIC", report_table_alignment::Left,
                   colors::ansi::BOLD_CYAN);
  table.add_column("VALUE", report_table_alignment::Right,
                   colors::ansi::BOLD_CYAN);
  return table;
}

fn add_metric_row(ReportTable &table, StringView name, StringView value,
                  Allocator allocator, StringView style = {}) throws -> void
{
  let cells = ArrayList<report_table_cell_view>{allocator};
  cells.push({name, colors::ansi::BOLD_CYAN});
  cells.push({value, style});
  table.add_row(cells);
}

fn add_rate_row(ReportTable &table, StringView name, Maybe<u64> rate,
                StringView suffix, Allocator allocator) throws -> void
{
  if (!rate.has_value()) return;
  let value = String::from(*rate, allocator);
  value += suffix;
  add_metric_row(table, name, value.view(), allocator);
}

fn add_stall_row(ReportTable &table, StringView name, Maybe<u64> rate,
                 Allocator allocator) throws -> void
{
  if (!rate.has_value()) return;
  let value = String::from(*rate, allocator);
  value += " us/s (";
  value += percent_text(*rate, 1000000, allocator).view();
  value += ")";
  add_metric_row(table, name, value.view(), allocator,
                 *rate == 0 ? colors::ansi::BOLD_CYAN : colors::ansi::BOLD_RED);
}

fn append_process_io_report(String &output, const ArrayList<io_row> &rows,
                            usize row_limit, u64 total_read_bytes,
                            u64 total_written_bytes,
                            u64 total_read_operation_count,
                            u64 total_write_operation_count,
                            bool has_operation_counts, Allocator allocator,
                            bool should_color) throws -> void
{
  let summary_table = make_metric_table(allocator);
  add_metric_row(summary_table, "Visible processes",
                 String::from(rows.count(), allocator), allocator);
  add_metric_row(summary_table, "Total bytes read",
                 format_human_size(total_read_bytes, allocator), allocator);
  add_metric_row(summary_table, "Total bytes written",
                 format_human_size(total_written_bytes, allocator), allocator);
  if (has_operation_counts) {
    add_metric_row(summary_table, "Total read operations",
                   String::from(total_read_operation_count, allocator),
                   allocator);
    add_metric_row(summary_table, "Total write operations",
                   String::from(total_write_operation_count, allocator),
                   allocator);
  }
  append_titled_report_table(output, "Process I/O summary", summary_table,
                             should_color);

  let process_table = ReportTable{allocator};
  process_table.add_column("PID", report_table_alignment::Right,
                           colors::ansi::BOLD_CYAN);
  process_table.add_column("READ", report_table_alignment::Right,
                           colors::ansi::BOLD_CYAN);
  process_table.add_column("WRITTEN", report_table_alignment::Right,
                           colors::ansi::BOLD_CYAN);
  if (has_operation_counts) {
    process_table.add_column("READ OPS", report_table_alignment::Right,
                             colors::ansi::BOLD_CYAN);
    process_table.add_column("WRITE OPS", report_table_alignment::Right,
                             colors::ansi::BOLD_CYAN);
  }
  process_table.add_column("COMMAND", report_table_alignment::Left,
                           colors::ansi::BOLD_CYAN);

  let const shown_count = rows.count() < row_limit ? rows.count() : row_limit;
  for (usize index = 0; index < shown_count; index++) {
    let const &row = rows[index];
    let const pid = String::from(row.pid, allocator);
    let const read = format_human_size(row.status.read_bytes, allocator);
    let const written = format_human_size(row.status.written_bytes, allocator);
    let read_operations = String{allocator};
    let write_operations = String{allocator};
    let cells = ArrayList<report_table_cell_view>{allocator};
    cells.push({pid.view(), colors::ansi::BOLD_MAGENTA});
    cells.push({read.view(), colors::ansi::GREEN});
    cells.push({written.view(), colors::ansi::GREEN});
    if (has_operation_counts) {
      read_operations =
          row.status.has_operation_counts
              ? String::from(row.status.read_operation_count, allocator)
              : String{allocator, "-"};
      write_operations =
          row.status.has_operation_counts
              ? String::from(row.status.write_operation_count, allocator)
              : String{allocator, "-"};
      cells.push({read_operations.view(), colors::ansi::GREEN});
      cells.push({write_operations.view(), colors::ansi::GREEN});
    }
    cells.push({row.name.view(), colors::ansi::BOLD_CYAN});
    process_table.add_row(cells);
  }
  append_titled_report_table(output, "Processes", process_table, should_color);
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
        process_limit_operand->substring(1), nullptr, int_base::decimal);
    if (parsed.is_error() || parsed.value() < 1 || parsed.value() > 100000) {
      KOSHKIT_REPORT_ERROR_AT(*process_limit_location, "invalid count",
                              "the count must be from 1 through 100000");
      return 1;
    }
    row_limit = static_cast<usize>(parsed.value());
  }
  if (FLAG_EVILIO_COUNT.is_set()) {
    let const parsed = utils::parse_integer_in_base(FLAG_EVILIO_COUNT.value(),
                                                    nullptr, int_base::decimal);
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
                                                    nullptr, int_base::decimal);
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
  let const sample_duration_seconds =
      FLAG_EVILIO_CUMULATIVE.is_enabled() ? cumulative_duration_seconds : 1.0;
  String sample_duration_label{allocator, "/S"};
  if (FLAG_EVILIO_CUMULATIVE.is_enabled() || FLAG_EVILIO_LIVE.is_enabled())
    sample_duration_label =
        String{allocator, "/"} +
        format_live_duration(sample_duration_seconds, allocator).view();
  let const refresh_interval_seconds = live_interval_seconds;
  let const should_show_processes =
      FLAG_EVILIO_PS.is_enabled() || FLAG_EVILIO_COUNT.is_set() ||
      selected_pid.has_value() || process_limit_operand.has_value();

  Maybe<evilio_sort_key> sort_key{};
  if (FLAG_EVILIO_SORT.is_set()) {
    let const resolved = resolve_sort_key(FLAG_EVILIO_SORT.value(), allocator);
    if (resolved.match_count == 0) {
      KOSHKIT_REPORT_ERROR_AT(
          FLAG_EVILIO_SORT.value_location(), "Invalid sort key",
          "Use pid, read, write, read-ops, write-ops, busy, read-latency, "
          "write-latency, average-queue, queue, errors, or retries");
      return 1;
    }
    if (resolved.match_count > 1) {
      let note = String{allocator, "Matches "};
      note += resolved.matches.view();
      note += "; use a longer prefix";
      KOSHKIT_REPORT_ERROR_AT(FLAG_EVILIO_SORT.value_location(),
                              "Ambiguous sort key", note.view());
      return 1;
    }

    sort_key = resolved.key;
    let const spec = find_sort_spec(*sort_key);
    if (should_show_processes && (spec == nullptr || !spec->is_process_metric))
    {
      KOSHKIT_REPORT_ERROR_AT(FLAG_EVILIO_SORT.value_location(),
                              "Sort key is unavailable for process reports",
                              "Use pid, read, write, read-ops, or write-ops");
      return 1;
    }
    if (!should_show_processes && (spec == nullptr || !spec->is_disk_metric)) {
      KOSHKIT_REPORT_ERROR_AT(
          FLAG_EVILIO_SORT.value_location(),
          "Sort key is unavailable for disk reports",
          "Use read, write, read-ops, write-ops, busy, read-latency, "
          "write-latency, average-queue, queue, errors, or retries");
      return 1;
    }
    if (!should_show_processes && sort_key_needs_sample(*sort_key) &&
        !FLAG_EVILIO_ALL.is_enabled() && !FLAG_EVILIO_CUMULATIVE.is_enabled() &&
        !FLAG_EVILIO_LIVE.is_enabled())
    {
      KOSHKIT_REPORT_ERROR_AT(
          FLAG_EVILIO_SORT.value_location(),
          "Sort key is unavailable without sampling",
          "Use --all, --cumulative, or --live with busy, read-latency, "
          "write-latency, or average-queue");
      return 1;
    }
  }

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
    let const is_terminal = os::is_fd_a_tty(ec.out_fd.value_or(KOSH_STDOUT));
    bool is_alternate_screen_active = false;
    if (is_terminal) is_alternate_screen_active = enter_alternate_screen(ec);
    let const is_cursor_hidden = is_terminal && hide_cursor(ec);
    defer
    {
      if (is_cursor_hidden) show_cursor(ec);
      if (is_alternate_screen_active) leave_alternate_screen(ec);
    };

    if (should_show_processes) {
      return run_live_process_io(ec, selected_pid, row_limit, sort_key,
                                 sample_duration_seconds, live_interval_seconds,
                                 refresh_interval_seconds, is_terminal,
                                 should_color, sample_duration_label.view());
    }

    return run_live_disk_io(ec, sample_duration_seconds, live_interval_seconds,
                            refresh_interval_seconds, is_terminal, should_color,
                            sample_duration_label.view(), sort_key);
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
        before_rows, after_rows, elapsed_nanoseconds, allocator, sort_key);
    let output = String{allocator};
    append_process_io_rate_report(output, sampled_rows, row_limit, allocator,
                                  should_color, sample_duration_label.view(),
                                  nullptr);
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
    sort_process_rows(rows, sort_key);

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
      let table = make_metric_table(allocator);
      add_metric_row(table, "User CPU time",
                     percent_text(*user, total, allocator), allocator);
      add_metric_row(table, "System CPU time",
                     percent_text(*system, total, allocator), allocator);
      add_metric_row(table, "Idle CPU time",
                     percent_text(*idle, total, allocator), allocator);
      if (wait.has_value()) {
        add_metric_row(table, "I/O wait CPU time",
                       percent_text(*wait, total, allocator), allocator);
      }
      if (stolen.has_value()) {
        add_metric_row(table, "Stolen CPU time",
                       percent_text(*stolen, total, allocator), allocator);
      }
      append_titled_report_table(output, "Processor activity", table,
                                 should_color);
    }
  }

  let memory_table = make_metric_table(allocator);
  if (has_memory_status) {
    if (memory.has_field(os::memory_status_field::Total) &&
        (memory.has_field(os::memory_status_field::Available) ||
         memory.has_field(os::memory_status_field::Free)))
    {
      let const total_bytes = memory.total_kib > UINT64_MAX / 1024
                                  ? UINT64_MAX
                                  : memory.total_kib * 1024;
      let const available_bytes = memory.available_kib > UINT64_MAX / 1024
                                      ? UINT64_MAX
                                      : memory.available_kib * 1024;
      let const used_bytes =
          total_bytes > available_bytes ? total_bytes - available_bytes : 0;
      let status = format_human_size(used_bytes, allocator);
      status += " / ";
      status += format_human_size(total_bytes, allocator).view();
      status += " (";
      status += percent_text(used_bytes, total_bytes, allocator).view();
      status += ")";
      add_metric_row(memory_table, "Used memory", status.view(), allocator);
    }
    if (memory.has_field(os::memory_status_field::Available)) {
      let const available_bytes = memory.available_kib > UINT64_MAX / 1024
                                      ? UINT64_MAX
                                      : memory.available_kib * 1024;
      add_metric_row(memory_table, "Available memory",
                     format_human_size(available_bytes, allocator), allocator);
    }
    if (memory.has_field(os::memory_status_field::Free)) {
      let const free_bytes = memory.free_kib > UINT64_MAX / 1024
                                 ? UINT64_MAX
                                 : memory.free_kib * 1024;
      add_metric_row(memory_table, "Free memory",
                     format_human_size(free_bytes, allocator), allocator);
    }
    if (FLAG_EVILIO_ALL.is_enabled()) {
      let const do_add_memory_size =
          [&](StringView name, os::memory_status_field field, u64 value_kib)
              throws -> void {
        if (!memory.has_field(field)) return;

        let const value_bytes =
            value_kib > UINT64_MAX / 1024 ? UINT64_MAX : value_kib * 1024;
        add_metric_row(memory_table, name,
                       format_human_size(value_bytes, allocator), allocator);
      };

      do_add_memory_size("Buffer memory", os::memory_status_field::Buffers,
                         memory.buffer_kib);
      do_add_memory_size("Page cache memory", os::memory_status_field::Cached,
                         memory.cached_kib);
      do_add_memory_size("Reclaimable slab memory",
                         os::memory_status_field::ReclaimableSlab,
                         memory.reclaimable_slab_kib);
      do_add_memory_size("Shared memory", os::memory_status_field::Shared,
                         memory.shared_kib);
      do_add_memory_size("Slab memory", os::memory_status_field::Slab,
                         memory.slab_kib);
      do_add_memory_size("Active memory", os::memory_status_field::Active,
                         memory.active_kib);
      do_add_memory_size("Inactive memory", os::memory_status_field::Inactive,
                         memory.inactive_kib);
      do_add_memory_size("Commit limit", os::memory_status_field::CommitLimit,
                         memory.commit_limit_kib);
      do_add_memory_size("Committed virtual memory",
                         os::memory_status_field::Committed,
                         memory.committed_kib);
      if (memory.has_field(os::memory_status_field::HugePagesTotal)) {
        add_metric_row(memory_table, "Huge pages total",
                       String::from(memory.huge_page_total_count, allocator),
                       allocator);
      }
      if (memory.has_field(os::memory_status_field::HugePagesFree)) {
        add_metric_row(memory_table, "Huge pages free",
                       String::from(memory.huge_page_free_count, allocator),
                       allocator);
      }
    }
  }
  if (has_activity_before && has_activity_after) {
    if (activity_before.has_field(os::system_activity_field::PageScan) &&
        activity_after.has_field(os::system_activity_field::PageScan))
    {
      add_rate_row(memory_table, "Pages scanned",
                   counter_rate(activity_before.page_scan_count,
                                activity_after.page_scan_count,
                                elapsed_nanoseconds),
                   "/s", allocator);
    }
    if (activity_before.has_field(os::system_activity_field::PageSteal) &&
        activity_after.has_field(os::system_activity_field::PageSteal))
    {
      add_rate_row(memory_table, "Pages reclaimed",
                   counter_rate(activity_before.page_steal_count,
                                activity_after.page_steal_count,
                                elapsed_nanoseconds),
                   "/s", allocator);
    }
    if (activity_before.has_field(os::system_activity_field::DirectReclaim) &&
        activity_after.has_field(os::system_activity_field::DirectReclaim))
    {
      add_rate_row(memory_table, "Direct reclaim stalls",
                   counter_rate(activity_before.direct_reclaim_count,
                                activity_after.direct_reclaim_count,
                                elapsed_nanoseconds),
                   "/s", allocator);
    }
    if (activity_before.has_field(os::system_activity_field::CompactionStall) &&
        activity_after.has_field(os::system_activity_field::CompactionStall))
    {
      add_rate_row(memory_table, "Compaction stalls",
                   counter_rate(activity_before.compaction_stall_count,
                                activity_after.compaction_stall_count,
                                elapsed_nanoseconds),
                   "/s", allocator);
    }
    if (activity_before.has_field(os::system_activity_field::OomKills) &&
        activity_after.has_field(os::system_activity_field::OomKills))
    {
      add_rate_row(memory_table, "Out-of-memory kills",
                   counter_rate(activity_before.oom_kill_count,
                                activity_after.oom_kill_count,
                                elapsed_nanoseconds),
                   "/s", allocator);
    }
    if (activity_after.has_field(os::system_activity_field::DirtyPages)) {
      add_metric_row(memory_table, "Dirty pages",
                     String::from(activity_after.dirty_page_count, allocator),
                     allocator);
    }
    if (activity_after.has_field(os::system_activity_field::WritebackPages)) {
      add_metric_row(
          memory_table, "Writeback pages",
          String::from(activity_after.writeback_page_count, allocator),
          allocator);
    }
  }
  if (has_memory_status || (has_activity_before && has_activity_after))
    append_titled_report_table(output, "Memory", memory_table, should_color);

  let paging_table = make_metric_table(allocator);
  if (has_activity_before && has_activity_after) {
    if (activity_before.has_field(os::system_activity_field::PageInput) &&
        activity_after.has_field(os::system_activity_field::PageInput))
    {
      let const rate =
          counter_rate(activity_before.page_input_bytes,
                       activity_after.page_input_bytes, elapsed_nanoseconds);
      if (rate.has_value()) {
        add_metric_row(paging_table, "Page input rate",
                       (format_human_size(*rate, allocator) + "/s").view(),
                       allocator);
      }
    }
    if (activity_before.has_field(os::system_activity_field::PageOutput) &&
        activity_after.has_field(os::system_activity_field::PageOutput))
    {
      let const rate =
          counter_rate(activity_before.page_output_bytes,
                       activity_after.page_output_bytes, elapsed_nanoseconds);
      if (rate.has_value()) {
        add_metric_row(paging_table, "Page output rate",
                       (format_human_size(*rate, allocator) + "/s").view(),
                       allocator);
      }
    }
    if (activity_before.has_field(os::system_activity_field::Faults) &&
        activity_after.has_field(os::system_activity_field::Faults))
    {
      add_rate_row(paging_table, "Page faults",
                   counter_rate(activity_before.page_fault_count,
                                activity_after.page_fault_count,
                                elapsed_nanoseconds),
                   "/s", allocator);
    }
    if (activity_before.has_field(os::system_activity_field::MajorFaults) &&
        activity_after.has_field(os::system_activity_field::MajorFaults))
    {
      add_rate_row(paging_table, "Major page faults",
                   counter_rate(activity_before.major_page_fault_count,
                                activity_after.major_page_fault_count,
                                elapsed_nanoseconds),
                   "/s", allocator);
    }
  }
  if (has_activity_before && has_activity_after)
    append_titled_report_table(output, "Paging", paging_table, should_color);

  let scheduler_table = make_metric_table(allocator);
  if (has_activity_after) {
    if (activity_after.has_field(os::system_activity_field::Runnable)) {
      let runnable =
          String::from(activity_after.runnable_process_count, allocator);
      runnable += " for ";
      runnable +=
          String::from(os::get_processor_counts().online_count, allocator)
              .view();
      runnable += " processors";
      add_metric_row(scheduler_table, "Runnable processes", runnable.view(),
                     allocator);
    }
    if (activity_after.has_field(os::system_activity_field::Blocked)) {
      add_metric_row(
          scheduler_table, "I/O-blocked processes",
          String::from(activity_after.blocked_process_count, allocator),
          allocator);
    }
  }
  if (has_activity_after)
    append_titled_report_table(output, "Scheduler", scheduler_table,
                               should_color);

  let stalls_table = make_metric_table(allocator);
  if (has_activity_before && has_activity_after) {
    if (activity_before.has_field(os::system_activity_field::CpuSomeStall) &&
        activity_after.has_field(os::system_activity_field::CpuSomeStall))
    {
      add_stall_row(stalls_table, "CPU partial pressure stall",
                    counter_rate(activity_before.cpu_some_stall_microseconds,
                                 activity_after.cpu_some_stall_microseconds,
                                 elapsed_nanoseconds),
                    allocator);
    }
    if (activity_before.has_field(os::system_activity_field::CpuFullStall) &&
        activity_after.has_field(os::system_activity_field::CpuFullStall))
    {
      add_stall_row(stalls_table, "CPU full pressure stall",
                    counter_rate(activity_before.cpu_full_stall_microseconds,
                                 activity_after.cpu_full_stall_microseconds,
                                 elapsed_nanoseconds),
                    allocator);
    }
    if (activity_before.has_field(os::system_activity_field::MemorySomeStall) &&
        activity_after.has_field(os::system_activity_field::MemorySomeStall))
    {
      add_stall_row(stalls_table, "Memory partial pressure stall",
                    counter_rate(activity_before.memory_some_stall_microseconds,
                                 activity_after.memory_some_stall_microseconds,
                                 elapsed_nanoseconds),
                    allocator);
    }
    if (activity_before.has_field(os::system_activity_field::MemoryFullStall) &&
        activity_after.has_field(os::system_activity_field::MemoryFullStall))
    {
      add_stall_row(stalls_table, "Memory full pressure stall",
                    counter_rate(activity_before.memory_full_stall_microseconds,
                                 activity_after.memory_full_stall_microseconds,
                                 elapsed_nanoseconds),
                    allocator);
    }
    if (activity_before.has_field(os::system_activity_field::IoSomeStall) &&
        activity_after.has_field(os::system_activity_field::IoSomeStall))
    {
      add_stall_row(stalls_table, "I/O partial pressure stall",
                    counter_rate(activity_before.io_some_stall_microseconds,
                                 activity_after.io_some_stall_microseconds,
                                 elapsed_nanoseconds),
                    allocator);
    }
    if (activity_before.has_field(os::system_activity_field::IoFullStall) &&
        activity_after.has_field(os::system_activity_field::IoFullStall))
    {
      add_stall_row(stalls_table, "I/O full pressure stall",
                    counter_rate(activity_before.io_full_stall_microseconds,
                                 activity_after.io_full_stall_microseconds,
                                 elapsed_nanoseconds),
                    allocator);
    }
  }
  if (has_activity_before && has_activity_after)
    append_titled_report_table(output, "Pressure stalls", stalls_table,
                               should_color);

  if (!disk_after.disks.is_empty() || FLAG_EVILIO_CUMULATIVE.is_enabled()) {
    let disk_rows = make_disk_io_rows(
        disk_before, disk_after, elapsed_nanoseconds, allocator,
        (FLAG_EVILIO_ALL.is_enabled() || FLAG_EVILIO_CUMULATIVE.is_enabled())
            ? report_sampling_mode::Rolling
            : report_sampling_mode::Instant);
    sort_disk_rows(disk_rows, sort_key);
    append_disk_io_report(
        output, disk_rows, allocator, should_color,
        sample_duration_label.view(),
        (FLAG_EVILIO_ALL.is_enabled() || FLAG_EVILIO_CUMULATIVE.is_enabled())
            ? report_sampling_mode::Rolling
            : report_sampling_mode::Instant);
  }

  if (FLAG_EVILIO_CUMULATIVE.is_enabled()) {
    ec.print_to_stdout(output);
    return 0;
  }

  let swap_table = make_metric_table(allocator);
  let add_swap_row = [&](StringView section, StringView status) throws -> void {
    add_metric_row(swap_table, section, status, allocator);
  };
  if (!has_swap_after) {
    add_swap_row("Status", "Unavailable");
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
    add_swap_row("Usage", status.view());
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
      add_swap_row("Input/output activity", activity.view());
    }
    if (swap_after.has_encryption_state) {
      add_swap_row("Encryption status",
                   swap_after.is_encrypted ? "enabled" : "disabled");
    }
  }
  append_titled_report_table(output, "Swap", swap_table, should_color);

  ec.print_to_stdout(output);
  return 0;
}

} /* namespace koshka::koshkit */
