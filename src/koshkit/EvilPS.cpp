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
     "Sort children by name, pid, cpu, or memory.");
static pure fn is_evilps_sample_duration(koshka::StringView value) wontthrow
    -> bool
{
  return !value.is_empty() &&
         ((value[0] >= '0' && value[0] <= '9') || value[0] == '.');
}
FLAG_OPTIONAL(EVILPS_LIVE, 'l', "live",
              "Refresh the process tree until interrupted; the optional "
              "interval defaults to 0.5 seconds.",
              is_evilps_sample_duration, "seconds");
FLAG_OPTIONAL(EVILPS_CUMULATIVE, 'C', "cumulative",
              "Average counters over an optional sliding window; without "
              "--live, compare snapshots across that interval.",
              is_evilps_sample_duration, "seconds");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(EvilPS);

namespace koshka::koshkit {

namespace {

constexpr usize MAXIMUM_TREE_DEPTH = 128;

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
                 bool is_sampled) wontthrow -> bool
{
  if (FLAG_EVILPS_SORT.is_set()) {
    let const key = FLAG_EVILPS_SORT.value();
    if (key == "cpu") {
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
    }
    if (key == "memory" && left.resident_kib != right.resident_kib)
      return left.resident_kib > right.resident_kib;
    if (key == "pid") return left.pid < right.pid;
  }
  if (FLAG_EVILPS_NUMERIC_SORT.is_enabled()) return left.pid < right.pid;

  if (left.name.view() < right.name.view()) return true;

  if (right.name.view() < left.name.view()) return false;

  return left.pid < right.pid;
}

fn sort_nodes(ArrayList<tree_node> &nodes, bool is_sampled) throws -> void
{
  let const do_compare = [is_sampled](const tree_node &left,
                                      const tree_node &right) {
    return compare_nodes(left, right, is_sampled);
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

fn append_label(String &output, const tree_node &node, Allocator allocator,
                bool should_color, bool should_human,
                bool is_sampled) throws -> void
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
      output += " ";
      append_report_text(output, node.command_line.view(), colors::ansi::DIM,
                         should_color);
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
      (FLAG_EVILPS_SORT.is_set() && FLAG_EVILPS_SORT.value() == "cpu");
  let const should_show_memory =
      FLAG_EVILPS_ALL.is_enabled() || FLAG_EVILPS_MEMORY.is_enabled() ||
      (FLAG_EVILPS_SORT.is_set() && FLAG_EVILPS_SORT.value() == "memory");
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
    output += " ";
    append_report_text(output, node.command_line.view(), colors::ansi::DIM,
                       should_color);
  }

  output += "\n";
}

fn render_children(String &output, ArrayList<tree_node> &nodes, i64 parent_pid,
                   const String &prefix, usize depth, Allocator allocator,
                   bool should_color, usize output_limit,
                   usize &rendered_count, bool should_human,
                   bool is_sampled) throws -> void
{
  if (depth > MAXIMUM_TREE_DEPTH || rendered_count >= output_limit) return;

  ArrayList<usize> child_positions{allocator};
  for (usize position = 0; position < nodes.count(); position++) {
    if (nodes[position].parent_pid != parent_pid) continue;

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
                 is_sampled);
    rendered_count++;

    let child_prefix = String{allocator, prefix.view()};
    child_prefix += is_last ? "    " : "│   ";
    render_children(output, nodes, nodes[position].pid, child_prefix, depth + 1,
                    allocator, should_color, output_limit, rendered_count,
                    should_human, is_sampled);
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

fn render_process_snapshot(const ExecContext &ec, EvalContext &cxt,
                           Allocator allocator, String &output,
                           ArrayList<tree_node> &nodes,
                           const ArrayList<String> &operands,
                           const ArrayList<SourceLocation> &operand_locations,
                           usize output_limit, bool should_color,
                           u32 viewport_rows,
                           usize scroll_offset, StringView search,
                           bool should_human, bool is_sampled,
                           usize &visible_line_count) throws -> i32
{
  if (nodes.is_empty()) {
    report_soft_koshkit_error(ec, cxt,
                              "the process listing is unavailable",
                              "this platform exposes no process table");
    return 1;
  }

  sort_nodes(nodes, is_sampled);

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

  if (root_position < nodes.count() && !FLAG_EVILPS_SORT.is_set()) {
    nodes[root_position].was_rendered = true;
    append_label(output, nodes[root_position], allocator, should_color,
                 should_human, is_sampled);
    rendered_count++;
    render_children(output, nodes, root_pid, String{allocator}, 0, allocator,
                    should_color, output_limit, rendered_count, should_human,
                    is_sampled);
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
        if (search.is_empty() || line.find_substring(search).has_value()) {
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

    if (FLAG_EVILPS_SORT.is_set()) {
      nodes[position].was_rendered = true;
      append_label(output, nodes[position], allocator, should_color,
                   should_human, is_sampled);
      rendered_count++;
      render_children(output, nodes, nodes[position].pid, String{allocator}, 0,
                      allocator, should_color, output_limit, rendered_count,
                      should_human, is_sampled);
      continue;
    }

    bool has_visible_parent = false;
    for (let const &candidate : nodes) {
      if (candidate.pid != nodes[position].parent_pid) continue;

      if (candidate.pid == nodes[position].pid) continue;

      has_visible_parent = true;
      break;
    }

    if (has_visible_parent) continue;

    nodes[position].was_rendered = true;
    append_label(output, nodes[position], allocator, should_color,
                 should_human, is_sampled);
    rendered_count++;
    render_children(output, nodes, nodes[position].pid, String{allocator}, 0,
                    allocator, should_color, output_limit, rendered_count,
                    should_human, is_sampled);
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
      if (search.is_empty() || line.find_substring(search).has_value()) {
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
                   usize &scroll_offset) wontthrow -> bool
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
    if (byte == '/') {
      input.clear();
      input += '/';
      continue;
    }
    if (byte == 127 || byte == 8) {
      if (!input.is_empty()) input.truncate(input.length() - 1);
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

  if (FLAG_EVILPS_SORT.is_set()) {
    let const key = FLAG_EVILPS_SORT.value();
    static constexpr PackedStringKey SORT_KEYS[] = {SSK("name"), SSK("pid"),
                                                    SSK("cpu"), SSK("memory")};
    static constexpr StaticStringSet VALID_SORT_KEYS{SORT_KEYS};
    if (!VALID_SORT_KEYS.contains(key)) {
      KOSHKIT_REPORT_ERROR_AT(FLAG_EVILPS_SORT.value_location(),
                              "invalid sort key",
                              "use name, pid, cpu, or memory");
      return 1;
    }
  }

  let const should_sample_cpu =
      FLAG_EVILPS_ALL.is_enabled() || FLAG_EVILPS_CPU.is_enabled() ||
      (FLAG_EVILPS_SORT.is_set() && FLAG_EVILPS_SORT.value() == "cpu");
  let const should_read_resources =
      should_sample_cpu || FLAG_EVILPS_MEMORY.is_enabled() ||
      (FLAG_EVILPS_SORT.is_set() && FLAG_EVILPS_SORT.value() == "memory");
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

  f64 cumulative_interval_seconds = FLAG_EVILPS_LIVE.is_enabled()
                                        ? live_interval_seconds
                                        : 1.0;
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
    let const is_terminal =
        os::is_fd_a_tty(ec.out_fd.value_or(KOSH_STDOUT));
    let const sample_interval_nanoseconds =
        static_cast<u64>(live_interval_seconds * 1000000000.0);
    let const window_nanoseconds =
        static_cast<u64>(cumulative_interval_seconds * 1000000000.0);
    let history = ArrayList<live_process_cpu_row>{live_allocator};
    u64 last_sample_nanoseconds = 0;
    bool is_alternate_screen_active = false;
    let live_input = String{live_allocator};
    let live_search = String{live_allocator};
    usize scroll_offset = 0;
    if (is_terminal) is_alternate_screen_active = enter_alternate_screen(ec);
    let const is_cursor_hidden = is_terminal && hide_cursor(ec);
    defer
    {
      if (is_cursor_hidden) show_cursor(ec);
      if (is_alternate_screen_active) leave_alternate_screen(ec);
    };

    loop
    {
      if (last_sample_nanoseconds != 0) {
        let const before_wait_nanoseconds = os::monotonic_nanos();
        let const elapsed_nanoseconds =
            before_wait_nanoseconds - last_sample_nanoseconds;
        let const wait_nanoseconds =
            sample_interval_nanoseconds > elapsed_nanoseconds
                ? sample_interval_nanoseconds - elapsed_nanoseconds
                : 0;
        os::sleep_for_seconds(static_cast<f64>(wait_nanoseconds) /
                              1000000000.0);
        if (os::INTERRUPT_REQUESTED != 0) {
          os::INTERRUPT_REQUESTED = 0;
          return 130;
        }
      }

      let const now = os::monotonic_nanos();
      let nodes = read_process_nodes(live_allocator, should_read_resources,
                                     line_width_limit);
      if (should_sample_cpu)
        update_cpu_history(nodes, history, now, window_nanoseconds);
      last_sample_nanoseconds = now;
      u32 terminal_rows = 0;
      if (is_terminal) {
        u32 terminal_columns = 0;
        if (!os::terminal_size(terminal_columns, terminal_rows))
          terminal_rows = 24;
      }
      usize visible_line_count = 0;
      let frame = String{live_allocator};
      if (is_terminal) frame += "\x1b[H\x1b[2J";
      append_live_controls_bar(
          frame,
          format_live_duration(cumulative_interval_seconds, live_allocator)
              .view(),
          format_live_duration(live_interval_seconds, live_allocator).view(),
          should_color);
      let const status = render_process_snapshot(
          ec, cxt, live_allocator, frame, nodes, operands, operand_locations,
          output_limit, should_color,
          is_terminal && terminal_rows > 2 ? terminal_rows - 1 : 0,
          scroll_offset, live_search.view(), false, true, visible_line_count);
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
                              live_search, scroll_offset))
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
      output_limit, should_color, 0, 0, StringView{}, false, is_sampled,
      output_limit);
  if (status == 0) ec.print_to_stdout(output);
  return status;
}

} // namespace koshka::koshkit
