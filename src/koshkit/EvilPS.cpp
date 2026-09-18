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

HELP_SYNOPSIS_DECL("[-NUMBER] [-pnaUCM] [--sort key] [pid]");

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
      KOSHKIT_REPORT_ERROR_AT(operand_locations[0],
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
  let const should_color = koshkit_should_color();
  usize rendered_count = 0;

  if (root_position < nodes.count()) {
    nodes[root_position].was_rendered = true;
    append_label(output, nodes[root_position], allocator, should_color);
    rendered_count++;
    render_children(output, nodes, root_pid, String{allocator}, 0, allocator,
                    should_color, output_limit, rendered_count);
    ec.print_to_stdout(output);
    return 0;
  }

  if (!operands.is_empty()) {
    KOSHKIT_REPORT_ERROR_AT(operand_locations[0],
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

  ec.print_to_stdout(output);
  return 0;
}

} // namespace koshka::koshkit
