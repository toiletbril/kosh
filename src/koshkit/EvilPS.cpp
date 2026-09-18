/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements the evilps utility. It links every visible process to
 * its parent, selects a root, and renders the descendants as an indented tree
 * with optional identifiers, owners, and command lines.
 */

#include "../CLI.hpp"
#include "../CLIColors.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Platform.hpp"
#include "../StaticStringMap.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-NUMBER] [-pnaUCM] [--sort key] [--live [seconds]] "
                   "[--cumulative [seconds]] [pid]");

HELP_DESCRIPTION_DECL("The evilps utility shows running processes as a tree.");

FLAG(EVILPS_PIDS, Bool, 'p', "show-pids",
     "Show the identifier of each process.");
FLAG(EVILPS_NUMERIC_SORT, Bool, 'n', "numeric-sort",
     "Sort the children by identifier.");
FLAG(EVILPS_ARGUMENTS, Bool, 'a', "arguments", "Show the command line.");
FLAG(EVILPS_OWNER, Bool, 'U', "show-owner", "Show the owner of each process.");
FLAG(EVILPS_CPU, Bool, 'C', "cpu", "Show accumulated processor time.");
FLAG(EVILPS_MEMORY, Bool, 'M', "memory", "Show resident memory usage.");
FLAG(EVILPS_SORT, String, '\0', "sort",
     "Sort children by name, pid, cpu, or memory.");
static pure fn is_evilps_sample_duration(koshka::StringView value) wontthrow
    -> bool
{
  return !value.is_empty() &&
         ((value[0] >= '0' && value[0] <= '9') || value[0] == '.');
}
FLAG_OPTIONAL(EVILPS_LIVE, 'l', "live",
              "Refresh the process tree at an optional interval until "
              "interrupted.",
              is_evilps_sample_duration, "seconds");
FLAG_OPTIONAL(EVILPS_CUMULATIVE, '\0', "cumulative",
              "Wait an optional interval before collecting the process tree.",
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
  u64 resident_kib{0};
  u32 owner_id{0};
  String name{heap_allocator()};
  String command_line{heap_allocator()};
  bool was_rendered{false};
};

fn compare_nodes(const tree_node &left, const tree_node &right) wontthrow
    -> bool
{
  if (FLAG_EVILPS_SORT.is_set()) {
    let const key = FLAG_EVILPS_SORT.value();
    if (key == "cpu" && left.cpu_milliseconds != right.cpu_milliseconds)
      return left.cpu_milliseconds > right.cpu_milliseconds;
    if (key == "memory" && left.resident_kib != right.resident_kib)
      return left.resident_kib > right.resident_kib;
    if (key == "pid") return left.pid < right.pid;
  }
  if (FLAG_EVILPS_NUMERIC_SORT.is_enabled()) return left.pid < right.pid;

  if (left.name.view() < right.name.view()) return true;

  if (right.name.view() < left.name.view()) return false;

  return left.pid < right.pid;
}

fn sort_nodes(ArrayList<tree_node> &nodes) throws -> void
{
  nodes.sort(compare_nodes);
}

fn append_label(String &output, const tree_node &node, Allocator allocator,
                bool should_color) throws -> void
{
  append_report_text(output, node.name.view(), colors::ansi::BOLD_GREEN,
                     should_color);

  if (FLAG_EVILPS_PIDS.is_enabled()) {
    output += "(";
    append_report_text(
        output, String::from(static_cast<u64>(node.pid), allocator).view(),
        colors::ansi::CYAN, should_color);
    output += ")";
  }

  if (FLAG_EVILPS_OWNER.is_enabled()) {
    let const owner = os::uid_to_username(node.owner_id);
    output += ",";
    append_report_text(output,
                       owner.has_value()
                           ? owner->view()
                           : String::from(node.owner_id, allocator).view(),
                       colors::ansi::YELLOW, should_color);
  }

  let const should_show_cpu =
      FLAG_EVILPS_CPU.is_enabled() ||
      (FLAG_EVILPS_SORT.is_set() && FLAG_EVILPS_SORT.value() == "cpu");
  let const should_show_memory =
      FLAG_EVILPS_MEMORY.is_enabled() ||
      (FLAG_EVILPS_SORT.is_set() && FLAG_EVILPS_SORT.value() == "memory");
  if (should_show_cpu || should_show_memory) {
    output += " [";
    if (should_show_cpu) {
      append_report_text(output, "CPU", colors::ansi::BOLD_CYAN, should_color);
      output += " ";
      let cpu_time = String::from(node.cpu_milliseconds / 1000, allocator);
      cpu_time += ".";
      let const milliseconds =
          String::from(node.cpu_milliseconds % 1000, allocator);
      cpu_time.append_repeated('0', 3 - milliseconds.length());
      cpu_time += milliseconds.view();
      cpu_time += "s";
      output += cpu_time.view();
    }
    if (should_show_cpu && should_show_memory) output += "  ";
    if (should_show_memory) {
      append_report_text(output, "MEM", colors::ansi::BOLD_CYAN, should_color);
      output += " ";
      output += format_human_size(node.resident_kib * 1024, allocator).view();
    }
    output += "]";
  }

  if (FLAG_EVILPS_ARGUMENTS.is_enabled() && !node.command_line.is_empty()) {
    output += " ";
    append_report_text(output, node.command_line.view(), colors::ansi::DIM,
                       should_color);
  }

  output += "\n";
}

fn render_children(String &output, ArrayList<tree_node> &nodes, i64 parent_pid,
                   const String &prefix, usize depth, Allocator allocator,
                   bool should_color, usize output_limit,
                   usize &rendered_count) throws -> void
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
    append_label(output, nodes[position], allocator, should_color);
    rendered_count++;

    let child_prefix = String{allocator, prefix.view()};
    child_prefix += is_last ? "    " : "│   ";
    render_children(output, nodes, nodes[position].pid, child_prefix, depth + 1,
                    allocator, should_color, output_limit, rendered_count);
  }
}

fn render_process_snapshot(const ExecContext &ec, EvalContext &cxt,
                           Allocator allocator,
                           const ArrayList<String> &operands,
                           const ArrayList<SourceLocation> &operand_locations,
                           usize output_limit, bool should_read_resources,
                           bool should_color, u32 viewport_rows,
                           usize scroll_offset, StringView search,
                           usize &visible_line_count) throws -> i32
{
  let const processes = os::enumerate_processes(
      should_read_resources ? os::process_detail::ResourceStats
                            : os::process_detail::Basic);
  if (processes.is_empty()) {
    report_soft_koshkit_error(ec, cxt,
                              "evilps: the process listing is unavailable",
                              "this platform exposes no process table");
    return 1;
  }

  ArrayList<tree_node> nodes{allocator};
  for (let const &process : processes) {
    tree_node node{};
    node.pid = process.pid;
    node.parent_pid = process.parent_pid;
    node.cpu_milliseconds = process.cpu_milliseconds;
    node.resident_kib = process.resident_kib;
    node.owner_id = process.owner_id;
    node.name = String{allocator, process.name.view()};
    node.command_line = String{allocator, process.command_line.view()};
    nodes.push(steal(node));
  }

  sort_nodes(nodes);

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

  let output = String{allocator};
  usize rendered_count = 0;

  if (root_position < nodes.count()) {
    nodes[root_position].was_rendered = true;
    append_label(output, nodes[root_position], allocator, should_color);
    rendered_count++;
    render_children(output, nodes, root_pid, String{allocator}, 0, allocator,
                    should_color, output_limit, rendered_count);
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
    ec.print_to_stdout(output);
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

    bool has_visible_parent = false;
    for (let const &candidate : nodes) {
      if (candidate.pid != nodes[position].parent_pid) continue;

      if (candidate.pid == nodes[position].pid) continue;

      has_visible_parent = true;
      break;
    }

    if (has_visible_parent) continue;

    nodes[position].was_rendered = true;
    append_label(output, nodes[position], allocator, should_color);
    rendered_count++;
    render_children(output, nodes, nodes[position].pid, String{allocator}, 0,
                    allocator, should_color, output_limit, rendered_count);
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
  ec.print_to_stdout(output);
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

  let const should_read_resources =
      FLAG_EVILPS_CPU.is_enabled() || FLAG_EVILPS_MEMORY.is_enabled() ||
      (FLAG_EVILPS_SORT.is_set() && (FLAG_EVILPS_SORT.value() == "cpu" ||
                                     FLAG_EVILPS_SORT.value() == "memory"));
  let const should_color = koshkit_should_color();

  f64 live_interval_seconds = 1.0;
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

  if (FLAG_EVILPS_LIVE.is_enabled()) {
    let const is_terminal = colors::stdout_is_a_terminal();
    let const refresh_interval_seconds =
        FLAG_EVILPS_CUMULATIVE.is_enabled() ? cumulative_interval_seconds
                                            : live_interval_seconds;
    let const refresh_interval_nanoseconds = static_cast<u64>(
        refresh_interval_seconds * 1000000000.0);
    u64 last_refresh_nanoseconds = 0;
    bool is_alternate_screen_active = false;
    let live_input = String{allocator};
    let live_search = String{allocator};
    usize scroll_offset = 0;
    if (is_terminal) is_alternate_screen_active = enter_alternate_screen(ec);
    defer
    {
      if (is_alternate_screen_active) leave_alternate_screen(ec);
    };

    loop
    {
      let const now = os::monotonic_nanos();
      u32 terminal_rows = 0;
      if (is_terminal) {
        u32 terminal_columns = 0;
        if (!os::terminal_size(terminal_columns, terminal_rows))
          terminal_rows = 24;
      }
      if (last_refresh_nanoseconds == 0 ||
          now - last_refresh_nanoseconds >= refresh_interval_nanoseconds) {
        last_refresh_nanoseconds = now;
        if (is_terminal) ec.print_to_stdout("\x1b[H\x1b[2J");
        usize visible_line_count = 0;
        let const status = render_process_snapshot(
            ec, cxt, allocator, operands, operand_locations, output_limit,
            should_read_resources, should_color,
            is_terminal && terminal_rows > 1 ? terminal_rows : 0,
            scroll_offset, live_search.view(), visible_line_count);
        if (status != 0) return status;
        if (visible_line_count > terminal_rows && terminal_rows > 1) {
          let const maximum_offset = visible_line_count - (terminal_rows - 1);
          if (scroll_offset == SIZE_MAX || scroll_offset > maximum_offset)
            scroll_offset = maximum_offset;
        } else {
          scroll_offset = 0;
        }
      }

      if (is_terminal && !poll_live_input(
                              ec.in_fd.value_or(KOSH_STDIN), live_input,
                              live_search, scroll_offset))
        return 0;
      os::sleep_for_seconds(live_interval_seconds);
      if (os::INTERRUPT_REQUESTED != 0) {
        os::INTERRUPT_REQUESTED = 0;
        return 130;
      }
    }
  }

  if (FLAG_EVILPS_CUMULATIVE.is_enabled()) {
    os::sleep_for_seconds(cumulative_interval_seconds);
    if (os::INTERRUPT_REQUESTED != 0) {
      os::INTERRUPT_REQUESTED = 0;
      return 130;
    }
  }

  return render_process_snapshot(ec, cxt, allocator, operands,
                                 operand_locations, output_limit,
                                 should_read_resources, should_color, 0, 0,
                                 StringView{}, output_limit);
}

} // namespace koshka::koshkit
