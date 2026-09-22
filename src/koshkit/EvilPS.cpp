/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements the evilps utility. It links every visible process to
 * its parent, samples processor counters over bounded sliding windows, selects
 * a root, and renders the descendants as an indented tree with optional
 * identifiers, owners, resource values, and command lines.
 */

#include "../CLI.hpp"
#include "../CLIColors.hpp"
#include "../Arena.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Platform.hpp"
#include "../StaticStringMap.hpp"
#include "../Toiletline.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-NUMBER] [-pAncUhMw] [--sort key] [--live [seconds]] "
                   "[--cumulative [seconds]] [pid]");

HELP_DESCRIPTION_DECL("The evilps utility shows running processes as a tree.");

FLAG(EVILPS_PIDS, Bool, 'p', "show-pids",
     "Show the identifier of each process.");
FLAG(EVILPS_NUMERIC_SORT, Bool, 'n', "numeric-sort",
     "Sort the children by identifier.");
FLAG(EVILPS_ALL, Bool, 'a', "all",
     "Show process identifiers, owners, processor time, memory, and command "
     "lines.");
FLAG(EVILPS_ARGUMENTS, Bool, 'A', "arguments", "Show the command line.");
FLAG(EVILPS_OWNER, Bool, 'U', "show-owner", "Show the owner of each process.");
FLAG(EVILPS_CPU, Bool, 'c', "cpu",
     "Show accumulated processor time, or rolling utilization when sampled.");
FLAG(EVILPS_MEMORY, Bool, 'M', "memory", "Show resident memory usage.");
FLAG(EVILPS_WIDE, Bool, 'w', "wide",
     "Do not truncate command lines to the terminal width.");
FLAG(EVILPS_SORT, String, '\0', "sort",
     "Sort children by a unique prefix of name, pid, cpu, or memory.");
static pure fn is_evilps_sample_duration(koshka::StringView value) wontthrow
    -> bool
{
  return !value.is_empty() &&
         ((value[0] >= '0' && value[0] <= '9') || value[0] == '.');
}
FLAG_OPTIONAL(EVILPS_LIVE, 'l', "live",
              Live,
              "Refresh the process tree every N seconds until interrupted; the "
              "default is 0.5 seconds. N changes refresh only; sampling remains "
              "every 0.5 seconds.",
              is_evilps_sample_duration, "seconds");
FLAG_OPTIONAL(EVILPS_CUMULATIVE, 'C', "cumulative",
              Live,
              "Average counters over an M-second sliding window; without "
              "--live, compare snapshots across M seconds.",
              is_evilps_sample_duration, "seconds");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(EvilPS);

namespace koshka::koshkit {

namespace {

constexpr usize MAXIMUM_TREE_DEPTH = 128;

enum class evilps_sort_key : u8
{
  Name,
  Pid,
  Cpu,
  Memory,
};

struct evilps_sort_spec
{
  evilps_sort_key key;
  const char *name;
};

static constexpr static_string_entry<evilps_sort_spec> SORT_KEY_ENTRIES[] = {
    {SSK("cpu"),    {evilps_sort_key::Cpu, "cpu"}      },
    {SSK("memory"), {evilps_sort_key::Memory, "memory"}},
    {SSK("name"),   {evilps_sort_key::Name, "name"}    },
    {SSK("pid"),    {evilps_sort_key::Pid, "pid"}      },
};

static constexpr StaticStringMap SORT_KEYS{SORT_KEY_ENTRIES};

fn resolve_sort_key(StringView value) throws -> Maybe<evilps_sort_key>
{
  if (let const exact = SORT_KEYS.find(value); exact.has_value())
    return exact->key;

  Maybe<evilps_sort_key> match{};
  for (let const &entry : SORT_KEY_ENTRIES) {
    if (!StringView{entry.value.name}.starts_with(value)) continue;
    if (match.has_value()) return None;
    match = entry.value.key;
  }

  return match;
}

struct tree_node
{
  i64 pid{0};
  i64 parent_pid{0};
  u64 cpu_milliseconds{0};
  u64 cpu_percentage_hundredths{0};
  u64 resident_kib{0};
  u64 start_token{0};
  u32 owner_id{0};
  String name{heap_allocator()};
  String command_line{heap_allocator()};
  bool has_cpu_percentage{false};
  bool was_rendered{false};
  bool search_visible{true};
};

struct live_process_cpu_row
{
  i64 pid{0};
  u64 start_token{0};
  ArrayList<u64> history_milliseconds{heap_allocator()};
  ArrayList<u64> history_nanoseconds{heap_allocator()};
  u64 last_seen_nanoseconds{0};
};

pure fn interpolate_cpu_milliseconds(u64 before, u64 after,
                                     u64 elapsed_nanoseconds,
                                     u64 passed_nanoseconds) wontthrow -> u64
{
  if (after < before || elapsed_nanoseconds == 0) return after;
  return before + static_cast<u64>(
                      static_cast<u128>(after - before) * passed_nanoseconds /
                      elapsed_nanoseconds);
}

fn set_cpu_percentage(tree_node &node, const live_process_cpu_row &history,
                      u64 window_start_nanoseconds,
                      u64 now_nanoseconds) wontthrow -> void
{
  if (history.history_nanoseconds.count() < 2) return;

  usize oldest = 0;
  while (oldest + 1 < history.history_nanoseconds.count() &&
         history.history_nanoseconds[oldest + 1] <= window_start_nanoseconds)
  {
    oldest++;
  }

  u64 baseline_milliseconds = history.history_milliseconds[oldest];
  u64 baseline_nanoseconds = history.history_nanoseconds[oldest];
  if (baseline_nanoseconds < window_start_nanoseconds &&
      oldest + 1 < history.history_nanoseconds.count())
  {
    let const next_nanoseconds = history.history_nanoseconds[oldest + 1];
    baseline_milliseconds = interpolate_cpu_milliseconds(
        baseline_milliseconds, history.history_milliseconds[oldest + 1],
        next_nanoseconds - baseline_nanoseconds,
        window_start_nanoseconds - baseline_nanoseconds);
    baseline_nanoseconds = window_start_nanoseconds;
  }

  let const current_milliseconds = history.history_milliseconds.back();
  if (current_milliseconds < baseline_milliseconds ||
      now_nanoseconds <= baseline_nanoseconds)
  {
    return;
  }

  node.cpu_percentage_hundredths = static_cast<u64>(
      static_cast<u128>(current_milliseconds - baseline_milliseconds) *
      10000000000ULL / (now_nanoseconds - baseline_nanoseconds));
  node.has_cpu_percentage = true;
}

fn update_cpu_history(ArrayList<tree_node> &nodes,
                      ArrayList<live_process_cpu_row> &history,
                      u64 now_nanoseconds,
                      u64 window_nanoseconds) throws -> void
{
  let const window_start_nanoseconds =
      now_nanoseconds > window_nanoseconds ? now_nanoseconds - window_nanoseconds
                                           : 0;
  for (let &node : nodes) {
    live_process_cpu_row *row = nullptr;
    for (let &candidate : history) {
      if (candidate.pid != node.pid ||
          candidate.start_token != node.start_token)
        continue;
      row = &candidate;
      break;
    }

    if (row == nullptr) {
      live_process_cpu_row fresh{};
      fresh.pid = node.pid;
      fresh.start_token = node.start_token;
      fresh.history_milliseconds.push(node.cpu_milliseconds);
      fresh.history_nanoseconds.push(now_nanoseconds);
      fresh.last_seen_nanoseconds = now_nanoseconds;
      history.push(steal(fresh));
      continue;
    }

    if (node.cpu_milliseconds < row->history_milliseconds.back()) {
      row->history_milliseconds.clear();
      row->history_nanoseconds.clear();
    }
    row->history_milliseconds.push(node.cpu_milliseconds);
    row->history_nanoseconds.push(now_nanoseconds);
    row->last_seen_nanoseconds = now_nanoseconds;
    while (row->history_nanoseconds.count() > 2 &&
           row->history_nanoseconds[1] <= window_start_nanoseconds)
    {
      row->history_milliseconds.remove(0);
      row->history_nanoseconds.remove(0);
    }
    set_cpu_percentage(node, *row, window_start_nanoseconds, now_nanoseconds);
  }

  for (usize index = history.count(); index > 0; index--) {
    if (history[index - 1].last_seen_nanoseconds != now_nanoseconds)
      history.remove(index - 1);
  }
}

fn compare_nodes(const tree_node &left, const tree_node &right,
                 Maybe<evilps_sort_key> sort_key,
                 bool is_sampled) wontthrow -> bool
{
  if (sort_key.has_value()) {
    switch (*sort_key) {
    case evilps_sort_key::Cpu: {
      if (is_sampled &&
          left.has_cpu_percentage != right.has_cpu_percentage)
      {
        return left.has_cpu_percentage;
      }
      let const left_cpu = is_sampled ? left.cpu_percentage_hundredths
                                      : left.cpu_milliseconds;
      let const right_cpu = is_sampled ? right.cpu_percentage_hundredths
                                       : right.cpu_milliseconds;
      if (left_cpu != right_cpu) return left_cpu > right_cpu;
      break;
    }
    case evilps_sort_key::Memory:
      if (left.resident_kib != right.resident_kib)
        return left.resident_kib > right.resident_kib;
      break;
    case evilps_sort_key::Pid: return left.pid < right.pid;
    case evilps_sort_key::Name: break;
    }
  } else if (FLAG_EVILPS_NUMERIC_SORT.is_enabled()) {
    return left.pid < right.pid;
  }

  if (left.name.view() < right.name.view()) return true;

  if (right.name.view() < left.name.view()) return false;

  return left.pid < right.pid;
}

fn sort_nodes(ArrayList<tree_node> &nodes,
              Maybe<evilps_sort_key> sort_key,
              bool is_sampled) throws -> void
{
  let const do_compare = [sort_key, is_sampled](const tree_node &left,
                                                const tree_node &right) {
    return compare_nodes(left, right, sort_key, is_sampled);
  };
  nodes.sort(do_compare);
}

fn append_cpu_value(String &output, const tree_node &node, Allocator allocator,
                    bool is_sampled) throws -> void
{
  if (is_sampled) {
    if (!node.has_cpu_percentage) {
      output += "-";
      return;
    }
    output += String::from(node.cpu_percentage_hundredths / 100, allocator);
    output += ".";
    let const fraction =
        String::from(node.cpu_percentage_hundredths % 100, allocator);
    output.append_repeated('0', 2 - fraction.length());
    output += fraction.view();
    output += "%";
    return;
  }

  output += String::from(node.cpu_milliseconds / 1000, allocator);
  output += ".";
  let const milliseconds =
      String::from(node.cpu_milliseconds % 1000, allocator);
  output.append_repeated('0', 3 - milliseconds.length());
  output += milliseconds.view();
  output += "s";
}

fn append_bounded_command(String &output, StringView command,
                          usize line_width_limit, bool should_color) throws
    -> void
{
  if (command.is_empty()) return;
  if (line_width_limit == 0 || line_width_limit == SIZE_MAX) {
    output += " ";
    append_report_text(output, command, colors::ansi::DIM, should_color);
    return;
  }

  let const line_start = output.view().find_last_character('\n');
  let const current_line = line_start.has_value()
                               ? output.view().substring(*line_start + 1)
                               : output.view();
  let const current_width = toiletline::display_width(current_line);
  if (current_width >= line_width_limit) return;
  let const available = line_width_limit - current_width - 1;
  if (available == 0) return;

  output += " ";
  if (toiletline::display_width(command) <= available) {
    append_report_text(output, command, colors::ansi::DIM, should_color);
    return;
  }

  if (available <= 3) {
    let usize actual_cells = 0;
    let const kept_bytes = toiletline::byte_offset_at_or_before_display_cell(
        command, available, actual_cells);
    append_report_text(output, command.substring_of_length(0, kept_bytes),
                       colors::ansi::DIM, should_color);
    return;
  }

  let usize actual_cells = 0;
  let const kept_bytes = toiletline::byte_offset_at_or_before_display_cell(
      command, available - 3, actual_cells);
  append_report_text(output, command.substring_of_length(0, kept_bytes),
                     colors::ansi::DIM, should_color);
  append_report_text(output, "...", colors::ansi::DIM, should_color);
}

fn append_label(String &output, const tree_node &node, Allocator allocator,
                bool should_color, bool should_human,
                Maybe<evilps_sort_key> sort_key,
                bool is_sampled, usize line_width_limit) throws -> void
{
  if (should_human) {
    append_report_text(output,
                       String::from(static_cast<u64>(node.pid), allocator).view(),
                       colors::ansi::CYAN, should_color);
    output += "  ";
    append_report_text(
        output, String::from(static_cast<u64>(node.parent_pid), allocator).view(),
        colors::ansi::CYAN, should_color);
    output += "  ";
    append_cpu_value(output, node, allocator, is_sampled);
    output += "  ";
    output += format_human_size(node.resident_kib * 1024, allocator).view();
    output += "  ";
    append_report_text(output, node.name.view(), colors::ansi::BOLD_GREEN,
                       should_color);
    if ((FLAG_EVILPS_ALL.is_enabled() ||
         FLAG_EVILPS_ARGUMENTS.is_enabled()) &&
        !node.command_line.is_empty()) {
      append_bounded_command(output, node.command_line.view(),
                             line_width_limit, should_color);
    }
    output += "\n";
    return;
  }

  append_report_text(output, node.name.view(), colors::ansi::BOLD_GREEN,
                     should_color);

  if (FLAG_EVILPS_ALL.is_enabled() || FLAG_EVILPS_PIDS.is_enabled()) {
    output += "(";
    append_report_text(
        output, String::from(static_cast<u64>(node.pid), allocator).view(),
        colors::ansi::CYAN, should_color);
    output += ")";
  }

  if (FLAG_EVILPS_ALL.is_enabled() || FLAG_EVILPS_OWNER.is_enabled()) {
    let const owner = os::uid_to_username(node.owner_id);
    output += ",";
    append_report_text(output,
                       owner.has_value()
                           ? owner->view()
                           : String::from(node.owner_id, allocator).view(),
                       colors::ansi::YELLOW, should_color);
  }

  let const should_show_cpu =
      FLAG_EVILPS_ALL.is_enabled() || FLAG_EVILPS_CPU.is_enabled() ||
      (sort_key.has_value() && *sort_key == evilps_sort_key::Cpu);
  let const should_show_memory =
      FLAG_EVILPS_ALL.is_enabled() || FLAG_EVILPS_MEMORY.is_enabled() ||
      (sort_key.has_value() && *sort_key == evilps_sort_key::Memory);
  if (should_show_cpu || should_show_memory) {
    output += " [";
    if (should_show_cpu) {
      append_report_text(output, "CPU", colors::ansi::BOLD_CYAN, should_color);
      output += " ";
      append_cpu_value(output, node, allocator, is_sampled);
    }
    if (should_show_cpu && should_show_memory) output += "  ";
    if (should_show_memory) {
      append_report_text(output, "MEM", colors::ansi::BOLD_CYAN, should_color);
      output += " ";
      output += format_human_size(node.resident_kib * 1024, allocator).view();
    }
    output += "]";
  }

  if ((FLAG_EVILPS_ALL.is_enabled() || FLAG_EVILPS_ARGUMENTS.is_enabled()) &&
      !node.command_line.is_empty()) {
    append_bounded_command(output, node.command_line.view(), line_width_limit,
                           should_color);
  }

  output += "\n";
}

fn render_children(String &output, ArrayList<tree_node> &nodes, i64 parent_pid,
                   const String &prefix, usize depth, Allocator allocator,
                   bool should_color, usize output_limit,
                   usize &rendered_count, bool should_human,
                   Maybe<evilps_sort_key> sort_key,
                   bool is_sampled, usize line_width_limit) throws -> void
{
  if (depth > MAXIMUM_TREE_DEPTH || rendered_count >= output_limit) return;

  ArrayList<usize> child_positions{allocator};
  for (usize position = 0; position < nodes.count(); position++) {
    if (nodes[position].parent_pid != parent_pid) continue;

    if (!nodes[position].search_visible) continue;

    if (nodes[position].pid == parent_pid) continue;

    if (nodes[position].was_rendered) continue;

    child_positions.push(position);
  }

  for (usize index = 0; index < child_positions.count(); index++) {
    if (rendered_count >= output_limit) break;

    let const position = child_positions[index];
    let const is_last = index + 1 == child_positions.count();
    nodes[position].was_rendered = true;

    append_report_text(output, prefix.view(), colors::ansi::CYAN, should_color);
    append_report_text(output, is_last ? "└── " : "├── ", colors::ansi::CYAN,
                       should_color);
    append_label(output, nodes[position], allocator, should_color, should_human,
                 sort_key, is_sampled, line_width_limit);
    rendered_count++;

    let child_prefix = String{allocator, prefix.view()};
    child_prefix += is_last ? "    " : "│   ";
    render_children(output, nodes, nodes[position].pid, child_prefix, depth + 1,
                    allocator, should_color, output_limit, rendered_count,
                    should_human, sort_key, is_sampled, line_width_limit);
  }
}

fn read_process_nodes(Allocator allocator, bool should_read_resources,
                      usize line_width_limit) throws
    -> ArrayList<tree_node>
{
  let const processes = os::enumerate_processes(
      should_read_resources ? os::process_detail::ResourceStats
                            : os::process_detail::Basic);
  ArrayList<tree_node> nodes{allocator};
  for (let const &process : processes) {
    tree_node node{};
    node.pid = process.pid;
    node.parent_pid = process.parent_pid;
    node.cpu_milliseconds = process.cpu_milliseconds;
    node.resident_kib = process.resident_kib;
    node.start_token = process.start_token;
    node.owner_id = process.owner_id;
    node.name = String{allocator, process.name.view()};
    node.command_line = String{allocator, process.command_line.view()};
    if (line_width_limit != 0 && line_width_limit != SIZE_MAX &&
        toiletline::display_width(node.command_line.view()) > line_width_limit)
    {
      const StringView text = node.command_line.view();
      usize actual_cells = 0;
      let const kept_bytes = toiletline::byte_offset_at_or_before_display_cell(
          text, line_width_limit - 3, actual_cells);
      node.command_line.truncate(kept_bytes);
      node.command_line += "...";
    }
    nodes.push(steal(node));
  }

  return nodes;
}

fn mark_search_visibility(ArrayList<tree_node> &nodes, StringView search) throws
    -> void
{
  if (search.is_empty()) return;

  let const parsed_pid = utils::parse_integer_in_base(search, int_base::decimal);
  let const is_exact_pid =
      !parsed_pid.is_error() && parsed_pid.value() > 0;
  for (let &node : nodes) {
    node.search_visible =
        is_exact_pid
            ? static_cast<u64>(node.pid) == parsed_pid.value()
            : node.name.view().find_substring(search).has_value() ||
                  node.command_line.view().find_substring(search).has_value();
  }

  for (usize index = 0; index < nodes.count(); index++) {
    if (!nodes[index].search_visible) continue;
    let parent_pid = nodes[index].parent_pid;
    while (parent_pid > 0) {
      bool found_parent = false;
      for (let &candidate : nodes) {
        if (candidate.pid != parent_pid) continue;
        candidate.search_visible = true;
        parent_pid = candidate.parent_pid;
        found_parent = true;
        break;
      }
      if (!found_parent) break;
    }
  }
}

fn render_process_snapshot(const ExecContext &ec, EvalContext &cxt,
                           Allocator allocator, String &output,
                           ArrayList<tree_node> &nodes,
                           const ArrayList<String> &operands,
                           const ArrayList<SourceLocation> &operand_locations,
                           usize output_limit, bool should_color,
                           u32 viewport_rows,
                           usize scroll_offset, StringView search,
                           bool should_human,
                           Maybe<evilps_sort_key> sort_key, bool is_sampled,
                           usize line_width_limit,
                           usize &visible_line_count) throws -> i32
{
  if (nodes.is_empty()) {
    report_soft_koshkit_error(ec, cxt,
                              "the process listing is unavailable",
                              "this platform exposes no process table");
    return 1;
  }

  sort_nodes(nodes, sort_key, is_sampled);
  mark_search_visibility(nodes, search);

  i64 root_pid = 1;
  if (!operands.is_empty()) {
    let const parsed =
        utils::parse_integer_in_base(operands[0].view(), int_base::decimal);
    if (parsed.is_error()) {
      report_soft_koshkit_util_error(
          ec, cxt, operand_locations[0], "evilps",
          "invalid process id '" + operands[0] + "'",
          "provide a decimal process id");
      return 1;
    }

    root_pid = static_cast<i64>(parsed.value());
  }

  usize root_position = nodes.count();
  for (usize position = 0; position < nodes.count(); position++) {
    if (nodes[position].pid != root_pid) continue;

    root_position = position;
    break;
  }

  usize rendered_count = 0;

  if (should_human) output += "PID  PPID  CPU  MEM  COMMAND\n";

  if (root_position < nodes.count() && !sort_key.has_value() &&
      (search.is_empty() || nodes[root_position].search_visible)) {
    if (nodes[root_position].search_visible) {
      nodes[root_position].was_rendered = true;
      append_label(output, nodes[root_position], allocator, should_color,
                   should_human, sort_key, is_sampled, line_width_limit);
      rendered_count++;
      render_children(output, nodes, root_pid, String{allocator}, 0, allocator,
                      should_color, output_limit, rendered_count, should_human,
                      sort_key, is_sampled, line_width_limit);
    }
    visible_line_count = 1;
    if (viewport_rows != 0) {
      let const full_output = String{allocator, output.view()};
      output.clear();
      usize line_number = 0;
      usize position = 0;
      while (position < full_output.length()) {
        let const relative_end =
            full_output.view().substring(position).find_character('\n');
        let const line_end = relative_end.has_value()
                                 ? position + *relative_end
                                 : full_output.length();
        let const line = full_output.view().substring_of_length(
            position, line_end - position);
        {
          if (line_number >= scroll_offset &&
              line_number - scroll_offset < viewport_rows - 1) {
            output += line;
            output += "\n";
          }
          line_number++;
        }
        position = relative_end.has_value() ? line_end + 1 : full_output.length();
      }
      visible_line_count = line_number;
    }
    return 0;
  }

  if (!operands.is_empty()) {
    report_soft_koshkit_util_error(
        ec, cxt, operand_locations[0], "evilps",
        "no process has the id " + operands[0],
        "read the current identifiers with ps");
    return 1;
  }

  for (usize position = 0; position < nodes.count(); position++) {
    if (rendered_count >= output_limit) break;

    if (nodes[position].was_rendered) continue;
    if (!nodes[position].search_visible) continue;

    if (sort_key.has_value()) {
      nodes[position].was_rendered = true;
      append_label(output, nodes[position], allocator, should_color,
                   should_human, sort_key, is_sampled, line_width_limit);
      rendered_count++;
      render_children(output, nodes, nodes[position].pid, String{allocator}, 0,
                      allocator, should_color, output_limit, rendered_count,
                      should_human, sort_key, is_sampled, line_width_limit);
      continue;
    }

    bool has_visible_parent = false;
    for (let const &candidate : nodes) {
      if (candidate.pid != nodes[position].parent_pid) continue;

      if (candidate.pid == nodes[position].pid || !candidate.search_visible)
        continue;

      has_visible_parent = true;
      break;
    }

    if (has_visible_parent) continue;

    nodes[position].was_rendered = true;
    append_label(output, nodes[position], allocator, should_color,
                 should_human, sort_key, is_sampled, line_width_limit);
    rendered_count++;
    render_children(output, nodes, nodes[position].pid, String{allocator}, 0,
                    allocator, should_color, output_limit, rendered_count,
                    should_human, sort_key, is_sampled, line_width_limit);
  }

  visible_line_count = rendered_count;
  if (viewport_rows != 0) {
    let const full_output = String{allocator, output.view()};
    output.clear();
    usize line_number = 0;
    usize position = 0;
    while (position < full_output.length()) {
      let const relative_end =
          full_output.view().substring(position).find_character('\n');
      let const line_end = relative_end.has_value()
                               ? position + *relative_end
                               : full_output.length();
      let const line = full_output.view().substring_of_length(
          position, line_end - position);
      {
        if (line_number >= scroll_offset &&
            line_number - scroll_offset < viewport_rows - 1) {
          output += line;
          output += "\n";
        }
        line_number++;
      }
      position = relative_end.has_value() ? line_end + 1 : full_output.length();
    }
    visible_line_count = line_number;
  }
  return 0;
}

fn poll_live_input(os::descriptor input_fd, String &input, String &search,
                   usize &scroll_offset, Maybe<evilps_sort_key> &sort_key,
                   bool &should_sample_cpu, bool &should_read_resources) wontthrow
    -> bool
{
  if (os::wait_for_fd_readable(input_fd, 0) <= 0) return true;

  char buffer[64];
  let const read_count = os::read_fd(input_fd, buffer, sizeof(buffer));
  if (!read_count.has_value()) return true;

  for (usize index = 0; index < *read_count; index++) {
    let const byte = buffer[index];
    if (byte == 'q' || byte == 'Q' || byte == 3 || byte == 27) return false;
    if (byte == 'j' || byte == 'J' || byte == ' ') {
      scroll_offset++;
      continue;
    }
    if (byte == 'k' || byte == 'K') {
      if (scroll_offset > 0) scroll_offset--;
      continue;
    }
    if (byte == 'g') {
      scroll_offset = 0;
      continue;
    }
    if (byte == 'G') {
      scroll_offset = SIZE_MAX;
      continue;
    }
    if (byte == 's' || byte == 'S') {
      if (!sort_key.has_value()) sort_key = evilps_sort_key::Name;
      else {
        switch (*sort_key) {
          case evilps_sort_key::Name: sort_key = evilps_sort_key::Pid; break;
          case evilps_sort_key::Pid: sort_key = evilps_sort_key::Cpu; break;
          case evilps_sort_key::Cpu: sort_key = evilps_sort_key::Memory; break;
          case evilps_sort_key::Memory: sort_key = None; break;
        }
      }
      scroll_offset = 0;
      if (sort_key.has_value() && *sort_key == evilps_sort_key::Cpu)
        should_sample_cpu = true;
      if (sort_key.has_value() && *sort_key == evilps_sort_key::Memory)
        should_read_resources = true;
      continue;
    }
    if (byte == '/') {
      input.clear();
      input += '/';
      search.clear();
      scroll_offset = 0;
      continue;
    }
    if (byte == 127 || byte == 8) {
      if (!input.is_empty()) {
        input.truncate(input.length() - 1);
        if (input.is_empty()) {
          search.clear();
          scroll_offset = 0;
        }
      }
      continue;
    }
    if (byte == '\n' || byte == '\r') {
      if (!input.is_empty() && input[0] == '/') {
        search = String{search.allocator(), input.view().substring(1)};
        scroll_offset = 0;
        input.clear();
      }
      continue;
    }
    if (!input.is_empty() && input[0] == '/') input += byte;
  }

  return true;
}

} // namespace

EvilPS::EvilPS() = default;

pure fn EvilPS::kind() const wontthrow -> Utility::Kind { return Kind::EvilPS; }

fn EvilPS::execute(const ExecContext &ec, EvalContext &cxt,
                   const ArrayList<String> &args,
                   const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let operand_locations = ArrayList<SourceLocation>{cxt.scratch_allocator()};
  let operands = PARSE_KOSHKIT_ARGS_WITH_LOCATIONS(
      args, arg_locations, operand_locations, true, true);

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  let const allocator = cxt.scratch_allocator();
  usize output_limit = SIZE_MAX;
  if (!operands.is_empty() && operands[0].length() > 1 && operands[0][0] == '-')
  {
    let const parsed = utils::parse_integer_in_base(
        operands[0].view().substring(1), int_base::decimal);
    if (parsed.is_error() || parsed.value() <= 0 ||
        static_cast<u64>(parsed.value()) > SIZE_MAX)
    {
      KOSHKIT_REPORT_ERROR_AT(operand_locations[0], "invalid process limit",
                              "the limit must be a positive integer");
      return 1;
    }
    output_limit = static_cast<usize>(parsed.value());
    operands.remove(0);
    operand_locations.remove(0);
  }

  if (operands.count() > 1) {
    KOSHKIT_REPORT_ERROR_AT(operand_locations[1], "too many operands",
                            "name at most one process id");
    return 1;
  }

  Maybe<evilps_sort_key> sort_key{};
  if (FLAG_EVILPS_SORT.is_set()) {
    sort_key = resolve_sort_key(FLAG_EVILPS_SORT.value());
    if (!sort_key.has_value()) {
      KOSHKIT_REPORT_ERROR_AT(FLAG_EVILPS_SORT.value_location(),
                              "invalid sort key",
                              "use name, pid, cpu, or memory");
      return 1;
    }
  }

  bool should_sample_cpu =
      FLAG_EVILPS_ALL.is_enabled() || FLAG_EVILPS_CPU.is_enabled() ||
      (sort_key.has_value() && *sort_key == evilps_sort_key::Cpu);
  bool should_read_resources =
      should_sample_cpu || FLAG_EVILPS_MEMORY.is_enabled() ||
      (sort_key.has_value() && *sort_key == evilps_sort_key::Memory);
  let const should_color = koshkit_should_color();

  f64 live_interval_seconds = 0.5;
  if (FLAG_EVILPS_LIVE.has_value()) {
    let const parsed = parse_koshkit_duration_seconds(
        FLAG_EVILPS_LIVE.value(), FLAG_EVILPS_LIVE.value_location(), allocator);
    if (parsed <= 0.0) {
      KOSHKIT_REPORT_ERROR_AT(FLAG_EVILPS_LIVE.value_location(),
                              "invalid live interval",
                              "use a positive number of seconds");
      return 1;
    }
    live_interval_seconds = parsed;
  }

  f64 cumulative_interval_seconds = 1.0;
  if (FLAG_EVILPS_CUMULATIVE.has_value()) {
    cumulative_interval_seconds = parse_koshkit_duration_seconds(
        FLAG_EVILPS_CUMULATIVE.value(),
        FLAG_EVILPS_CUMULATIVE.value_location(), allocator);
    if (cumulative_interval_seconds <= 0.0) {
      KOSHKIT_REPORT_ERROR_AT(FLAG_EVILPS_CUMULATIVE.value_location(),
                              "invalid cumulative interval",
                              "use a positive number of seconds");
      return 1;
    }
  }

  let line_width_limit = SIZE_MAX;
  if (!FLAG_EVILPS_WIDE.is_enabled()) {
    u32 terminal_columns = 0;
    u32 terminal_rows = 0;
    if (os::terminal_size(terminal_columns, terminal_rows,
                          ec.out_fd.value_or(KOSH_STDOUT)) &&
        terminal_columns > 8)
      line_width_limit = terminal_columns;
  }

  if (FLAG_EVILPS_LIVE.is_enabled()) {
    let const live_allocator = heap_allocator();
    let frame_arena = BumpArena{};
    let const is_terminal =
        os::is_fd_a_tty(ec.out_fd.value_or(KOSH_STDOUT));
    let const sample_interval_nanoseconds = 500000000ULL;
    let const refresh_interval_nanoseconds =
        static_cast<u64>(live_interval_seconds * 1000000000.0);
    let const window_nanoseconds =
        static_cast<u64>(cumulative_interval_seconds * 1000000000.0);
    let history = ArrayList<live_process_cpu_row>{live_allocator};
    let nodes = read_process_nodes(live_allocator, should_read_resources,
                                   line_width_limit);
    u64 last_sample_nanoseconds = os::monotonic_nanos();
    u64 last_refresh_nanoseconds =
        last_sample_nanoseconds > refresh_interval_nanoseconds
            ? last_sample_nanoseconds - refresh_interval_nanoseconds
            : 0;
    if (should_sample_cpu)
      update_cpu_history(nodes, history, last_sample_nanoseconds,
                         window_nanoseconds);
    bool is_alternate_screen_active = false;
    let live_input = String{live_allocator};
    let live_search = String{live_allocator};
    usize scroll_offset = 0;
    usize live_line_width_limit = line_width_limit;
    if (is_terminal) is_alternate_screen_active = enter_alternate_screen(ec);
    let const is_cursor_hidden = is_terminal && hide_cursor(ec);
    defer
    {
      if (is_cursor_hidden) show_cursor(ec);
      if (is_alternate_screen_active) leave_alternate_screen(ec);
    };

    loop
    {
      let const frame_mark = frame_arena.mark();
      defer { frame_arena.release(frame_mark); };
      let const frame_allocator = bump_allocator(frame_arena);

      let const before_wait_nanoseconds = os::monotonic_nanos();
      let const sample_elapsed =
          before_wait_nanoseconds - last_sample_nanoseconds;
      let const refresh_elapsed =
          before_wait_nanoseconds - last_refresh_nanoseconds;
      let const until_sample = sample_interval_nanoseconds > sample_elapsed
                                   ? sample_interval_nanoseconds - sample_elapsed
                                   : 0;
      let const until_refresh =
          refresh_interval_nanoseconds > refresh_elapsed
              ? refresh_interval_nanoseconds - refresh_elapsed
              : 0;
      let const wait_nanoseconds =
          until_sample < until_refresh ? until_sample : until_refresh;
      if (wait_nanoseconds != 0)
        os::sleep_for_seconds(static_cast<f64>(wait_nanoseconds) /
                              1000000000.0);
      if (os::INTERRUPT_REQUESTED != 0) {
        os::INTERRUPT_REQUESTED = 0;
        return 130;
      }

      let const now = os::monotonic_nanos();
      if (now - last_sample_nanoseconds >= sample_interval_nanoseconds) {
        live_line_width_limit = line_width_limit;
        if (!FLAG_EVILPS_WIDE.is_enabled() && is_terminal) {
          u32 terminal_columns = 0;
          u32 terminal_rows = 0;
          if (os::terminal_size(terminal_columns, terminal_rows,
                                ec.out_fd.value_or(KOSH_STDOUT)) &&
              terminal_columns > 8)
            live_line_width_limit = terminal_columns;
          unused(terminal_rows);
        }
        nodes = read_process_nodes(live_allocator, should_read_resources,
                                   live_line_width_limit);
        if (should_sample_cpu)
          update_cpu_history(nodes, history, now, window_nanoseconds);
        last_sample_nanoseconds = now;
      }
      if (now - last_refresh_nanoseconds < refresh_interval_nanoseconds) {
        if (is_terminal && !poll_live_input(
                                ec.in_fd.value_or(KOSH_STDIN), live_input,
                                live_search, scroll_offset, sort_key,
                                should_sample_cpu, should_read_resources))
          return 0;
        continue;
      }
      last_refresh_nanoseconds = now;
      u32 terminal_rows = 0;
      if (is_terminal) {
        u32 terminal_columns = 0;
        if (!os::terminal_size(terminal_columns, terminal_rows,
                               ec.out_fd.value_or(KOSH_STDOUT)))
          terminal_rows = 24;
      }
      usize visible_line_count = 0;
      let frame = String{frame_allocator};
      if (is_terminal) frame += "\x1b[H\x1b[2J";
      append_live_controls_bar(
          frame,
          format_live_duration(cumulative_interval_seconds, frame_allocator)
              .view(),
          format_live_duration(live_interval_seconds, frame_allocator).view(),
          should_color);
      frame += "SORT ";
      if (!sort_key.has_value()) frame += "tree";
      else if (*sort_key == evilps_sort_key::Name) frame += "name";
      else if (*sort_key == evilps_sort_key::Pid) frame += "pid";
      else if (*sort_key == evilps_sort_key::Cpu) frame += "cpu";
      else frame += "memory";
      frame += " | s sort | / search | q quit";
      if (!live_search.is_empty() || !live_input.is_empty()) {
        frame += " | SEARCH ";
        if (!live_input.is_empty())
          frame += live_input.view();
        else {
          frame += "/";
          frame += live_search.view();
        }
      }
      frame += "\n";
      let const status = render_process_snapshot(
          ec, cxt, frame_allocator, frame, nodes, operands, operand_locations,
          output_limit, should_color,
          is_terminal && terminal_rows > 2 ? terminal_rows - 1 : 0,
          scroll_offset, live_search.view(), false, sort_key, true,
          live_line_width_limit, visible_line_count);
      if (status != 0) return status;
      ec.print_to_stdout(frame);
      if (visible_line_count > terminal_rows && terminal_rows > 1) {
        let const maximum_offset = visible_line_count - (terminal_rows - 1);
        if (scroll_offset == SIZE_MAX || scroll_offset > maximum_offset)
          scroll_offset = maximum_offset;
      } else {
        scroll_offset = 0;
      }

      if (is_terminal && !poll_live_input(
                              ec.in_fd.value_or(KOSH_STDIN), live_input,
                              live_search, scroll_offset, sort_key,
                              should_sample_cpu, should_read_resources))
        return 0;
    }
  }

  let nodes = read_process_nodes(allocator, should_read_resources,
                                 line_width_limit);
  bool is_sampled = false;
  if (FLAG_EVILPS_CUMULATIVE.is_enabled()) {
    let history = ArrayList<live_process_cpu_row>{allocator};
    let const before_nanoseconds = os::monotonic_nanos();
    if (should_sample_cpu)
      update_cpu_history(nodes, history, before_nanoseconds,
                         static_cast<u64>(cumulative_interval_seconds *
                                          1000000000.0));
    os::sleep_for_seconds(cumulative_interval_seconds);
    if (os::INTERRUPT_REQUESTED != 0) {
      os::INTERRUPT_REQUESTED = 0;
      return 130;
    }
    nodes = read_process_nodes(allocator, should_read_resources,
                               line_width_limit);
    let const after_nanoseconds = os::monotonic_nanos();
    if (should_sample_cpu)
      update_cpu_history(nodes, history, after_nanoseconds,
                         static_cast<u64>(cumulative_interval_seconds *
                                          1000000000.0));
    is_sampled = true;
  }

  let output = String{allocator};
  let const status = render_process_snapshot(
      ec, cxt, allocator, output, nodes, operands, operand_locations,
      output_limit, should_color, 0, 0, StringView{}, false, sort_key,
      is_sampled, line_width_limit, output_limit);
  if (status == 0) ec.print_to_stdout(output);
  return status;
}

} // namespace koshka::koshkit
