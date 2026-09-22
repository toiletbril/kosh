/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements evilio. It reports processor, memory, paging,
 * scheduler, stall, disk, swap, and process I/O activity.
 */

#include "../CLI.hpp"
#include "../CLIColors.hpp"
#include "../Arena.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Platform.hpp"
#include "../StaticStringMap.hpp"
#include "../Utils.hpp"

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
FLAG_OPTIONAL(EVILIO_CUMULATIVE, 'C', "cumulative",
              Live,
              "Use an M-second rolling window for sampled activity; the "
              "default is one second.",
              is_evilio_sample_duration, "seconds");
FLAG(EVILIO_PS, Bool, '\0', "ps", "Show every visible process.");
FLAG_OPTIONAL(EVILIO_LIVE, 'l', "live",
              Live,
              "Refresh live output every N seconds; the default is 0.5 seconds. "
              "N changes refresh only; sampling remains every 0.5 seconds.",
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
        process.pid, process.start_token, status
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
      if (read_operation_delta.has_value() && write_operation_delta.has_value()) {
        status.read_operation_count = *read_operation_delta;
        status.write_operation_count = *write_operation_delta;
        status.has_operation_counts = true;
      }
    }
    if (is_idle_process_io(status)) continue;

    sampled_rows.push(io_row{
        String{allocator, after.name.view()},
        after.pid, after.start_token, status
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

pure fn interpolate_counter(u64 before, u64 after, u64 elapsed_nanoseconds,
                            u64 passed_nanoseconds) wontthrow -> u64
{
  if (after < before || elapsed_nanoseconds == 0) return after;
  return before + static_cast<u64>(static_cast<u128>(after - before) *
                                   passed_nanoseconds / elapsed_nanoseconds);
}

fn get_process_window_status(const live_process_row &row,
                             u64 window_start_nanoseconds) wontthrow
    -> os::process_io_status
{
  usize oldest = 0;
  while (oldest + 1 < row.history_nanoseconds.count() &&
         row.history_nanoseconds[oldest + 1] <= window_start_nanoseconds)
    oldest++;

  let before = row.history[oldest];
  u64 before_nanoseconds = row.history_nanoseconds[oldest];
  if (before_nanoseconds < window_start_nanoseconds &&
      oldest + 1 < row.history.count())
  {
    let const &next = row.history[oldest + 1];
    let const next_nanoseconds = row.history_nanoseconds[oldest + 1];
    let const elapsed_nanoseconds = next_nanoseconds - before_nanoseconds;
    let const passed_nanoseconds =
        window_start_nanoseconds - before_nanoseconds;
    before.read_bytes =
        interpolate_counter(before.read_bytes, next.read_bytes,
                            elapsed_nanoseconds, passed_nanoseconds);
    before.written_bytes =
        interpolate_counter(before.written_bytes, next.written_bytes,
                            elapsed_nanoseconds, passed_nanoseconds);
    if (before.has_operation_counts && next.has_operation_counts) {
      before.read_operation_count = interpolate_counter(
          before.read_operation_count, next.read_operation_count,
          elapsed_nanoseconds, passed_nanoseconds);
      before.write_operation_count = interpolate_counter(
          before.write_operation_count, next.write_operation_count,
          elapsed_nanoseconds, passed_nanoseconds);
    }
    before_nanoseconds = window_start_nanoseconds;
  }

  let const &newest = row.history.back();
  os::process_io_status status{0, 0, 0, 0, false};
  if (let const delta = counter_delta(before.read_bytes, newest.read_bytes);
      delta.has_value())
    status.read_bytes = *delta;
  if (let const delta = counter_delta(before.written_bytes, newest.written_bytes);
      delta.has_value())
    status.written_bytes = *delta;
  if (before.has_operation_counts && newest.has_operation_counts) {
    let const read_delta =
        counter_delta(before.read_operation_count, newest.read_operation_count);
    let const write_delta = counter_delta(before.write_operation_count,
                                          newest.write_operation_count);
    if (read_delta.has_value() && write_delta.has_value()) {
      status.read_operation_count = *read_delta;
      status.write_operation_count = *write_delta;
      status.has_operation_counts = true;
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
  append_report_column(output, "PID", 8, true, colors::ansi::BOLD_CYAN,
                       should_color);
  output += "  ";
  append_report_column(output, String{"READ"} + duration_suffix, 10, true,
                       colors::ansi::BOLD_CYAN, should_color);
  output += "  ";
  append_report_column(output, String{"WRITE"} + duration_suffix, 10, true,
                       colors::ansi::BOLD_CYAN, should_color);
  output += "  ";
  append_report_column(output, String{"READ OPS"} + duration_suffix, 10, true,
                       colors::ansi::BOLD_CYAN, should_color);
  output += "  ";
  append_report_column(output, String{"WRITE OPS"} + duration_suffix, 11, true,
                       colors::ansi::BOLD_CYAN, should_color);
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

pure fn disk_failure_total(const os::disk_io_status *before,
                           const os::disk_io_status &after, bool is_sampled,
                           os::disk_io_field read_field,
                           os::disk_io_field write_field,
                           u64 os::disk_io_status::*read_member,
                           u64 os::disk_io_status::*write_member) wontthrow
    -> Maybe<u64>
{
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

fn make_disk_io_rows(const os::disk_io_snapshot &before_snapshot,
                     const os::disk_io_snapshot &after_snapshot,
                     u64 elapsed_nanoseconds, bool is_sampled,
                     Allocator allocator) throws -> ArrayList<disk_io_row>
{
  let rows = ArrayList<disk_io_row>{allocator};
  rows.reserve(after_snapshot.disks.count());
  for (let const &after : after_snapshot.disks) {
    let const before = find_disk_io_status(before_snapshot, after.name.view());
    disk_io_row row{};
    row.name = String{allocator, after.name.view()};
    if (is_sampled) {
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
          row.read_operations =
              counter_delta(before->read_operation_count,
                            after.read_operation_count);
        }
        if (before->has_field(os::disk_io_field::WriteOperations) &&
            after.has_field(os::disk_io_field::WriteOperations))
        {
          row.write_operations =
              counter_delta(before->write_operation_count,
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

    if (is_sampled && before != nullptr && elapsed_nanoseconds != 0) {
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
        before, after, is_sampled, os::disk_io_field::ReadErrors,
        os::disk_io_field::WriteErrors, &os::disk_io_status::read_error_count,
        &os::disk_io_status::write_error_count);
    row.retries = disk_failure_total(
        before, after, is_sampled, os::disk_io_field::ReadRetries,
        os::disk_io_field::WriteRetries, &os::disk_io_status::read_retry_count,
        &os::disk_io_status::write_retry_count);
    rows.push(steal(row));
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
                         bool is_sampled, bool should_include_heading,
                         Allocator allocator, bool should_color,
                         StringView duration_suffix = "/S") throws -> void
{
  if (rows.is_empty() && !is_sampled) return;

  if (should_include_heading) output += "\n";
  append_report_column(output, "DEVICE", 16, false, colors::ansi::BOLD_CYAN,
                       should_color);
  let const do_append_header = [&](StringView text, usize width)
                                   throws -> void {
    output += "  ";
    append_report_column(output, text, width, true, colors::ansi::BOLD_CYAN,
                         should_color);
  };
  do_append_header(is_sampled ? String{"READ"} + duration_suffix : "READ", 10);
  do_append_header(is_sampled ? String{"WRITE"} + duration_suffix : "WRITTEN",
                   10);
  do_append_header(
      is_sampled ? String{"READ OPS"} + duration_suffix : "READ OPS", 11);
  do_append_header(
      is_sampled ? String{"WRITE OPS"} + duration_suffix : "WRITE OPS", 12);
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

  for (let const &row : rows) {
    append_report_column(output, row.name.view(), 16, false,
                         colors::ansi::BOLD_GREEN, should_color);
    output += "  ";
    append_report_column(output,
                         row.read.has_value()
                             ? format_human_size(*row.read, allocator).view()
                             : StringView{"-"},
                         10, true, colors::ansi::GREEN, should_color);
    output += "  ";
    append_report_column(output,
                         row.write.has_value()
                             ? format_human_size(*row.write, allocator).view()
                             : StringView{"-"},
                         10, true, colors::ansi::GREEN, should_color);
    output += "  ";
    append_report_column(
        output,
        row.read_operations.has_value()
            ? String::from(*row.read_operations, allocator).view()
            : StringView{"-"},
        11, true, {}, should_color);
    output += "  ";
    append_report_column(
        output,
        row.write_operations.has_value()
            ? String::from(*row.write_operations, allocator).view()
            : StringView{"-"},
        12, true, {}, should_color);
    if (is_sampled) {
      output += "  ";
      append_report_column(
          output,
          row.busy_tenths.has_value()
              ? tenths_text(*row.busy_tenths, allocator, true).view()
              : StringView{"-"},
          7, true, {}, should_color);
      output += "  ";
      append_report_column(output,
                           row.read_latency_nanoseconds.has_value()
                               ? utils::format_duration_nanoseconds(
                                     *row.read_latency_nanoseconds, allocator)
                                     .view()
                               : StringView{"-"},
                           9, true, {}, should_color);
      output += "  ";
      append_report_column(output,
                           row.write_latency_nanoseconds.has_value()
                               ? utils::format_duration_nanoseconds(
                                     *row.write_latency_nanoseconds, allocator)
                                     .view()
                               : StringView{"-"},
                           9, true, {}, should_color);
      output += "  ";
      append_report_column(
          output,
          row.average_queue_tenths.has_value()
              ? tenths_text(*row.average_queue_tenths, allocator, false).view()
              : StringView{"-"},
          9, true, {}, should_color);
    }
    output += "  ";
    append_report_column(output,
                         row.queue.has_value()
                             ? String::from(*row.queue, allocator).view()
                             : StringView{"-"},
                         7, true, {}, should_color);
    output += "  ";
    append_report_column(
        output,
        row.errors.has_value() ? String::from(*row.errors, allocator).view()
                               : StringView{"-"},
        8, true,
        row.errors.has_value() && *row.errors != 0 ? colors::ansi::BOLD_RED
                                                   : colors::ansi::GREEN,
        should_color);
    output += "  ";
    append_report_column(
        output,
        row.retries.has_value() ? String::from(*row.retries, allocator).view()
                                : StringView{"-"},
        8, true,
        row.retries.has_value() && *row.retries != 0 ? colors::ansi::BOLD_RED
                                                     : colors::ansi::GREEN,
        should_color);
    output += "\n";
  }
  if (should_include_heading) output += "\n";
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
      if (selected_pid.has_value() && after_rows.is_empty()) return 1;

      for (let const &row : after_rows) {
        bool is_known = false;
        for (usize index = 0; index < retained.count(); index++) {
          if (retained[index].pid != row.pid ||
              retained[index].start_token != row.start_token)
            continue;
          if (process_io_counter_reset(retained[index].history.back(),
                                       row.status)) {
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
        let const window_start =
            now > falloff_nanoseconds ? now - falloff_nanoseconds : 0;
        while (retained[position].history_nanoseconds.count() > 2 &&
               retained[position].history_nanoseconds[1] <= window_start)
        {
          retained[position].history.remove(0);
          retained[position].history_nanoseconds.remove(0);
        }
      }
      last_sample_nanoseconds = now;
    }
    if (now - last_refresh_nanoseconds < refresh_interval_nanoseconds) continue;

    last_refresh_nanoseconds = now;
    let rows = ArrayList<io_row>{frame_allocator};
    rows.reserve(retained.count());
    let const window_start = last_sample_nanoseconds > falloff_nanoseconds
                                 ? last_sample_nanoseconds - falloff_nanoseconds
                                 : 0;
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

fn get_disk_window_status(const live_disk_row &row,
                          u64 window_start_nanoseconds) throws
    -> os::disk_io_status
{
  usize oldest = 0;
  while (oldest + 1 < row.history_nanoseconds.count() &&
         row.history_nanoseconds[oldest + 1] <= window_start_nanoseconds)
    oldest++;

  let before = row.history[oldest];
  u64 before_nanoseconds = row.history_nanoseconds[oldest];
  if (before_nanoseconds < window_start_nanoseconds &&
      oldest + 1 < row.history.count())
  {
    let const &next = row.history[oldest + 1];
    let const elapsed_nanoseconds =
        row.history_nanoseconds[oldest + 1] - before_nanoseconds;
    let const passed_nanoseconds =
        window_start_nanoseconds - before_nanoseconds;
    let const do_interpolate = [&](u64 os::disk_io_status::*member) {
      before.*member =
          interpolate_counter(before.*member, next.*member, elapsed_nanoseconds,
                              passed_nanoseconds);
    };
    do_interpolate(&os::disk_io_status::read_bytes);
    do_interpolate(&os::disk_io_status::written_bytes);
    do_interpolate(&os::disk_io_status::read_operation_count);
    do_interpolate(&os::disk_io_status::write_operation_count);
    do_interpolate(&os::disk_io_status::read_time_nanoseconds);
    do_interpolate(&os::disk_io_status::write_time_nanoseconds);
    do_interpolate(&os::disk_io_status::busy_time_nanoseconds);
    do_interpolate(&os::disk_io_status::idle_time_nanoseconds);
    do_interpolate(&os::disk_io_status::weighted_busy_time_nanoseconds);
    do_interpolate(&os::disk_io_status::read_error_count);
    do_interpolate(&os::disk_io_status::write_error_count);
    do_interpolate(&os::disk_io_status::read_retry_count);
    do_interpolate(&os::disk_io_status::write_retry_count);
    before_nanoseconds = window_start_nanoseconds;
  }

  let const &newest = row.history.back();
  let sampled = newest;
  sampled.name = String{heap_allocator(), row.name.view()};
  let const do_sample = [&](u64 os::disk_io_status::*member) {
    let const delta = counter_delta(before.*member, newest.*member);
    sampled.*member = delta.has_value() ? *delta : 0;
  };
  do_sample(&os::disk_io_status::read_bytes);
  do_sample(&os::disk_io_status::written_bytes);
  do_sample(&os::disk_io_status::read_operation_count);
  do_sample(&os::disk_io_status::write_operation_count);
  do_sample(&os::disk_io_status::read_time_nanoseconds);
  do_sample(&os::disk_io_status::write_time_nanoseconds);
  do_sample(&os::disk_io_status::busy_time_nanoseconds);
  do_sample(&os::disk_io_status::idle_time_nanoseconds);
  do_sample(&os::disk_io_status::weighted_busy_time_nanoseconds);
  do_sample(&os::disk_io_status::read_error_count);
  do_sample(&os::disk_io_status::write_error_count);
  do_sample(&os::disk_io_status::read_retry_count);
  do_sample(&os::disk_io_status::write_retry_count);
  return sampled;
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
        let const window_start =
            now > falloff_nanoseconds ? now - falloff_nanoseconds : 0;
        while (retained[position].history_nanoseconds.count() > 2 &&
               retained[position].history_nanoseconds[1] <= window_start)
        {
          retained[position].history.remove(0);
          retained[position].history_nanoseconds.remove(0);
        }
      }
      last_sample_nanoseconds = now;
    }
    if (now - last_refresh_nanoseconds < refresh_interval_nanoseconds) {
      continue;
    }

    last_refresh_nanoseconds = now;
    let after_snapshot = os::disk_io_snapshot{};
    after_snapshot.sampled_at_nanoseconds = 1000000000ULL;
    after_snapshot.disks.reserve(retained.count());
    let before_snapshot = os::disk_io_snapshot{};
    before_snapshot.sampled_at_nanoseconds = 0;
    before_snapshot.disks.reserve(retained.count());
    let const window_start = last_sample_nanoseconds > falloff_nanoseconds
                                 ? last_sample_nanoseconds - falloff_nanoseconds
                                 : 0;
    for (let const &row : retained) {
      let sampled = get_disk_window_status(row, window_start);
      let baseline = os::disk_io_status{};
      baseline.name = String{frame_allocator, row.name.view()};
      baseline.available_fields = sampled.available_fields;
      before_snapshot.disks.push(steal(baseline));
      after_snapshot.disks.push(steal(sampled));
    }
    let const elapsed_nanoseconds = 1000000000ULL;
    let rows = make_disk_io_rows(before_snapshot, after_snapshot,
                                 elapsed_nanoseconds, true, frame_allocator);
    sort_disk_rows(rows, sort_key);

    let output = String{frame_allocator};
    if (is_terminal) output += "\x1b[H\x1b[2J";
    append_live_controls_bar(output, sample_label.view(), refresh_label.view(),
                             should_color);
    append_disk_io_report(output, rows, true, false, frame_allocator,
                          should_color,
                          sample_duration_label);
    ec.print_to_stdout(output);
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
  append_report_body(output, summary.view(), "");

  output += "\n";
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
                                          : (FLAG_EVILIO_LIVE.is_enabled()
                                                 ? 1.0
                                                 : live_interval_seconds);
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
    let const is_terminal =
        os::is_fd_a_tty(ec.out_fd.value_or(KOSH_STDOUT));
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
                                 sample_duration_seconds, 0.5,
                                 refresh_interval_seconds, is_terminal,
                                 should_color, sample_duration_label.view());
    }

    return run_live_disk_io(ec, sample_duration_seconds, 0.5,
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
      append_report_body(output, body.view(), "");
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
    append_report_body(output, memory_body.view(), "");
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
    append_report_body(output, paging_body.view(), "");
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
    append_report_body(output, scheduler_body.view(), "");
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
    append_report_body(output, stalls.view(), "");
    output += "\n";
  }

  if (!disk_after.disks.is_empty() || FLAG_EVILIO_CUMULATIVE.is_enabled()) {
    let disk_rows = make_disk_io_rows(
        disk_before, disk_after, elapsed_nanoseconds,
        FLAG_EVILIO_ALL.is_enabled() || FLAG_EVILIO_CUMULATIVE.is_enabled(),
        allocator);
    sort_disk_rows(disk_rows, sort_key);
    append_disk_io_report(output, disk_rows,
                          FLAG_EVILIO_ALL.is_enabled() ||
                              FLAG_EVILIO_CUMULATIVE.is_enabled(),
                          !FLAG_EVILIO_CUMULATIVE.is_enabled(), allocator,
                          should_color, sample_duration_label.view());
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
  append_report_body(output, swap_body.view(), "");

  ec.print_to_stdout(output);
  return 0;
}

} /* namespace koshka::koshkit */
