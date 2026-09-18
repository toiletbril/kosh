/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements the evil utility. It reports the host, kernel,
 * processor, memory, root filesystem, init system, and container state.
 */

#include "../CLI.hpp"
#include "../CLIColors.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Path.hpp"
#include "../Platform.hpp"
#include "../StaticStringMap.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-asu]");

HELP_DESCRIPTION_DECL(
    "The evil utility reports what the machine is and how it is running.");

FLAG(EVIL_ALL, Bool, 'a', "all", "Print additional system details.");
FLAG(EVIL_SHORT, Bool, 's', "short",
     "Print system identity without resource usage.");
FLAG(EVIL_USERS, Bool, 'u', "users", "Print every local user account.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Evil);

namespace koshka::koshkit {

namespace {

fn read_first_line(StringView path, Allocator allocator) throws -> Maybe<String>
{
  let const body = Path{path}.read_entire_file();
  if (!body.has_value()) return None;

  let const view = body->view();
  usize line_end = 0;
  while (line_end < view.length && view[line_end] != '\n')
    line_end++;

  return String{allocator, view.substring_of_length(0, line_end)};
}

fn detect_container(Allocator allocator) throws -> Maybe<String>
{
  if (os::path_exists("/.dockerenv")) return String{allocator, "docker"};

  let const named = os::get_environment_variable("container");
  if (named.has_value() && !named->is_empty()) {
    return String{allocator, named->view()};
  }

  if (os::path_exists("/run/.containerenv")) return String{allocator, "podman"};

  return None;
}

fn format_uptime(u64 seconds, Allocator allocator) throws -> String
{
  let text = String{allocator};
  let const days = seconds / 86400;
  let const hours = (seconds % 86400) / 3600;
  let const minutes = (seconds % 3600) / 60;

  if (days > 0) {
    text += String::from(days, allocator).view();
    text += days == 1 ? " day, " : " days, ";
  }

  if (days > 0 || hours > 0) {
    text += String::from(hours, allocator).view();
    text += hours == 1 ? " hour, " : " hours, ";
  }

  text += String::from(minutes, allocator).view();
  text += minutes == 1 ? " minute" : " minutes";
  return text;
}

fn resolve_color(const ExecContext &ec, EvalContext &cxt,
                 bool &out_should_color) throws -> bool
{
  unused(ec);
  unused(cxt);
  out_should_color = koshkit_should_color();
  return true;
}

fn format_limit_value(u64 value, Allocator allocator) throws -> String
{
  if (value == os::RESOURCE_UNLIMITED) return String{allocator, "unlimited"};

  return String::from(value, allocator);
}

fn append_resource_limit(String &output, StringView name,
                         os::resource_kind kind, Allocator allocator,
                         bool should_color) throws -> void
{
  os::resource_limit limit{};
  if (!os::get_resource_limit(kind, limit)) return;

  let value = format_limit_value(limit.soft, allocator);
  value += " soft, ";
  value += format_limit_value(limit.hard, allocator).view();
  value += " hard";
  append_report_field(output, name, value.view(), colors::ansi::BOLD_CYAN,
                      should_color);
}

fn append_system_configuration(String &output, StringView name,
                               os::system_configuration_key key,
                               Allocator allocator, bool should_color) throws
    -> void
{
  let const value = os::system_configuration(key);
  if (!value.has_value()) return;

  append_report_field(output, name, String::from(*value, allocator).view(),
                      colors::ansi::BOLD_CYAN, should_color);
}

struct mapped_library_family
{
  String family;
  String path;
};

fn append_anomaly_report(String &output, EvalContext &cxt,
                         bool should_color) throws -> void
{
  let const allocator = cxt.scratch_allocator();
  if (!os::has_process_open_file_listing()) {
    append_report_field(output, "Mixed libraries", "unavailable",
                        colors::ansi::BOLD_CYAN, should_color);
    return;
  }

  let const files =
      os::list_process_open_files(os::get_current_process_id(), allocator);
  let families = ArrayList<mapped_library_family>{allocator};
  usize findings = 0;
  for (let const &file : files) {
    if (file.use != os::process_file_use::Mapped) continue;
    let const filename = Path{file.path.view()}.filename();
    let const marker = filename.find_substring(".so");
    if (!marker.has_value()) continue;
    let const family = filename.substring_of_length(0, *marker);
    usize family_index = 0;
    while (family_index < families.count() &&
           families[family_index].family.view() != family)
      family_index++;
    if (family_index == families.count()) {
      families.push(mapped_library_family{
          String{allocator, family          },
          String{allocator, file.path.view()}
      });
    } else if (families[family_index].path.view() != file.path.view()) {
      findings++;
    }
  }

  append_report_field(output, "Mixed libraries",
                      String::from(findings, allocator).view(),
                      colors::ansi::BOLD_CYAN, should_color);
  append_report_field(output, "Confidence", "high", colors::ansi::BOLD_CYAN,
                      should_color);
  append_report_field(output, "Cost", "process mappings",
                      colors::ansi::BOLD_CYAN, should_color);
}

fn append_procfs_report(String &output, bool should_color,
                        Allocator allocator) throws -> void
{
  os::system_activity_status activity{};
  if (os::read_system_activity_status(activity)) {
    append_report_field(output, "CPU user units",
                        String::from(activity.cpu_user_units, allocator).view(),
                        colors::ansi::BOLD_CYAN, should_color);
    append_report_field(
        output, "CPU system units",
        String::from(activity.cpu_system_units, allocator).view(),
        colors::ansi::BOLD_CYAN, should_color);
    append_report_field(
        output, "Page faults",
        String::from(activity.page_fault_count, allocator).view(),
        colors::ansi::BOLD_CYAN, should_color);
    append_report_field(
        output, "Major page faults",
        String::from(activity.major_page_fault_count, allocator).view(),
        colors::ansi::BOLD_CYAN, should_color);
    append_report_field(
        output, "Runnable processes",
        String::from(activity.runnable_process_count, allocator).view(),
        colors::ansi::BOLD_CYAN, should_color);
    append_report_field(
        output, "Blocked processes",
        String::from(activity.blocked_process_count, allocator).view(),
        colors::ansi::BOLD_CYAN, should_color);
    append_report_field(
        output, "CPU stall microseconds",
        String::from(activity.cpu_some_stall_microseconds, allocator).view(),
        colors::ansi::BOLD_CYAN, should_color);
    append_report_field(
        output, "Memory stall microseconds",
        String::from(activity.memory_some_stall_microseconds, allocator).view(),
        colors::ansi::BOLD_CYAN, should_color);
    append_report_field(
        output, "IO stall microseconds",
        String::from(activity.io_some_stall_microseconds, allocator).view(),
        colors::ansi::BOLD_CYAN, should_color);
    append_report_field(output, "OOM kills",
                        String::from(activity.oom_kill_count, allocator).view(),
                        colors::ansi::BOLD_CYAN, should_color);
  } else {
    append_report_field(output, "Activity", "unavailable",
                        colors::ansi::BOLD_CYAN, should_color);
  }

  os::memory_status memory{};
  if (os::read_memory_status(memory)) {
    let memory_line = String::from(memory.available_kib, allocator) +
                      " KiB available of " +
                      String::from(memory.total_kib, allocator).view() + " KiB";
    append_report_field(output, "Memory", memory_line.view(),
                        colors::ansi::BOLD_CYAN, should_color);
  }
  let const processors = os::get_processor_counts();
  append_report_field(output, "Processors",
                      String::from(processors.online_count, allocator).view(),
                      colors::ansi::BOLD_CYAN, should_color);
}

fn names_text(const ArrayList<String> &names, Allocator allocator) throws
    -> Maybe<String>
{
  if (names.is_empty()) return None;

  let result = String{allocator};
  for (let const &name : names) {
    if (!result.is_empty()) result += ", ";
    result += name.view();
  }

  return result;
}

} // namespace

Evil::Evil() = default;

pure fn Evil::kind() const wontthrow -> Utility::Kind { return Kind::Evil; }

fn Evil::execute(const ExecContext &ec, EvalContext &cxt,
                 const ArrayList<String> &args,
                 const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let operand_locations = ArrayList<SourceLocation>{cxt.scratch_allocator()};
  let const operands =
      PARSE_KOSHKIT_ARGS_WITH_LOCATIONS(args, arg_locations, operand_locations);

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  if (!operands.is_empty()) {
    KOSHKIT_REPORT_ERROR_AT(operand_locations[0], "unexpected operand",
                            "this utility reads no operand");
    return 1;
  }

  bool should_color = false;
  if (!resolve_color(ec, cxt, should_color)) return 1;

  let const allocator = cxt.scratch_allocator();
  let output = String{allocator};

  let const host = os::get_hostname();
  append_report_field(output, "Host",
                      host.has_value() ? host->view() : "unknown",
                      colors::ansi::BOLD_CYAN, should_color);

  let system_line = String{allocator, os::executable_system_name().view()};
  let const release = os::system_release_name();
  if (!release.is_empty()) {
    system_line += " ";
    system_line += release.view();
  }

  append_report_field(output, "Kernel", system_line.view(),
                      colors::ansi::BOLD_CYAN, should_color);
  append_report_field(output, "Architecture", os::machine_type().view(),
                      colors::ansi::BOLD_CYAN, should_color);
  if (FLAG_EVIL_ALL.is_enabled()) {
    append_report_field(output, "System version",
                        os::system_version_name().view(),
                        colors::ansi::BOLD_CYAN, should_color);
    append_report_field(output, "Target", os::machine_target_name().view(),
                        colors::ansi::BOLD_CYAN, should_color);
    append_report_field(output, "OS type", os::ostype_name(),
                        colors::ansi::BOLD_CYAN, should_color);
  }

  let const distribution = read_first_line("/etc/os-release", allocator);
  if (distribution.has_value() && distribution->view().starts_with("NAME=")) {
    let const named =
        distribution->view().substring_of_length(5, distribution->length() - 5);
    let const unquoted = named.length >= 2 && named[0] == '"'
                             ? named.substring_of_length(1, named.length - 2)
                             : named;
    append_report_field(output, "Distribution", unquoted,
                        colors::ansi::BOLD_CYAN, should_color);
  }

  append_report_field(output, "Init", get_init_system_name(allocator).view(),
                      colors::ansi::BOLD_CYAN, should_color);

  let const container = detect_container(allocator);
  if (container.has_value()) {
    append_report_field(output, "Container", container->view(),
                        colors::ansi::BOLD_CYAN, should_color);
  }

  let const shell = os::get_environment_variable("SHELL");
  if (shell.has_value()) {
    append_report_field(output, "Shell", shell->view(), colors::ansi::BOLD_CYAN,
                        should_color);
  }

  let const user = os::get_current_user();
  if (user.has_value()) {
    append_report_field(output, "User", user->view(), colors::ansi::BOLD_CYAN,
                        should_color);
  }

  if (FLAG_EVIL_ALL.is_enabled()) {
    let const login_user = os::get_login_user();
    if (login_user.has_value()) {
      append_report_field(output, "Login user", login_user->view(),
                          colors::ansi::BOLD_CYAN, should_color);
    }
  }

  append_report_field(output, "Kosh", short_version_string(allocator).view(),
                      colors::ansi::BOLD_CYAN, should_color);
  if (FLAG_EVIL_ALL.is_enabled()) {
    let const executable = os::current_executable_path();
    if (executable.has_value()) {
      append_report_field(output, "Executable", executable->view(),
                          colors::ansi::BOLD_CYAN, should_color);
    }
  }
  append_report_field(output, "Directory",
                      os::read_current_directory().text().view(),
                      colors::ansi::BOLD_CYAN, should_color);
  if (FLAG_EVIL_ALL.is_enabled()) {
    append_report_field(
        output, "Time",
        os::format_local_time(
            "%Y-%m-%d %H:%M:%S %z",
            static_cast<i64>(os::realtime_microseconds() / 1000000))
            .view(),
        colors::ansi::BOLD_CYAN, should_color);
    append_report_field(
        output, "Process",
        String::from(os::get_current_process_id(), allocator).view(),
        colors::ansi::BOLD_CYAN, should_color);
    append_report_field(
        output, "Parent process",
        String::from(os::get_parent_process_id(), allocator).view(),
        colors::ansi::BOLD_CYAN, should_color);

    let user_ids = String::from(os::get_real_user_id(), allocator);
    user_ids += " real, ";
    user_ids += String::from(os::get_effective_user_id(), allocator).view();
    user_ids += " effective";
    append_report_field(output, "User IDs", user_ids.view(),
                        colors::ansi::BOLD_CYAN, should_color);

    let group_ids = String::from(os::get_real_group_id(), allocator);
    group_ids += " real, ";
    group_ids += String::from(os::get_effective_group_id(), allocator).view();
    group_ids += " effective";
    append_report_field(output, "Group IDs", group_ids.view(),
                        colors::ansi::BOLD_CYAN, should_color);

    let const supplementary_groups = os::get_supplementary_group_ids(allocator);
    let group_list = String{allocator};
    for (usize index = 0; index < supplementary_groups.count(); index++) {
      if (index != 0) group_list += ", ";
      group_list += String::from(supplementary_groups[index], allocator).view();
    }
    append_report_field(output, "Supplementary groups", group_list.view(),
                        colors::ansi::BOLD_CYAN, should_color);

    let const groups = names_text(os::enumerate_groups(), allocator);
    if (groups.has_value())
      append_report_field(output, "Groups", groups->view(),
                          colors::ansi::BOLD_CYAN, should_color);

    let const terminal = os::terminal_name(KOSH_STDIN);
    if (terminal.has_value()) {
      append_report_field(output, "Terminal", terminal->view(),
                          colors::ansi::BOLD_CYAN, should_color);
    }
  }

  if (FLAG_EVIL_USERS.is_enabled()) {
    let const users = names_text(os::enumerate_users(), allocator);
    if (users.has_value()) {
      append_report_field(output, "Users", users->view(),
                          colors::ansi::BOLD_CYAN, should_color);
    }
  }

  if (FLAG_EVIL_SHORT.is_enabled()) {
    ec.print_to_stdout(output);
    return 0;
  }

  let const uptime = os::system_uptime_seconds();
  if (uptime.has_value()) {
    append_report_field(output, "Uptime",
                        format_uptime(*uptime, allocator).view(),
                        colors::ansi::BOLD_CYAN, should_color);
  }

  let const processors = os::get_processor_counts();
  let processor_line = String{allocator};
  let const model = os::processor_model_name(allocator);
  if (model.has_value() && !model->is_empty()) {
    processor_line += model->view();
    processor_line += ", ";
  }

  processor_line += String::from(processors.online_count, allocator).view();
  processor_line += processors.online_count == 1 ? " online thread of "
                                                 : " online threads of ";
  processor_line += String::from(processors.configured_count, allocator).view();
  append_report_field(output, "Processor", processor_line.view(),
                      colors::ansi::BOLD_CYAN, should_color);

  os::memory_status memory{};
  if (os::read_memory_status(memory) && memory.total_kib > 0) {
    let const used_kib = memory.total_kib > memory.available_kib
                             ? memory.total_kib - memory.available_kib
                             : 0;
    let memory_line = format_human_size(used_kib * 1024, allocator);
    memory_line += " used of ";
    memory_line += format_human_size(memory.total_kib * 1024, allocator).view();
    append_report_field(output, "Memory", memory_line.view(),
                        colors::ansi::BOLD_CYAN, should_color);
    if (FLAG_EVIL_ALL.is_enabled()) {
      append_report_field(
          output, "Memory available",
          format_human_size(memory.available_kib * 1024, allocator).view(),
          colors::ansi::BOLD_CYAN, should_color);
      append_report_field(
          output, "Memory free",
          format_human_size(memory.free_kib * 1024, allocator).view(),
          colors::ansi::BOLD_CYAN, should_color);
    }

    if (memory.swap_total_kib > 0) {
      let const swap_used_kib =
          memory.swap_total_kib > memory.swap_free_kib
              ? memory.swap_total_kib - memory.swap_free_kib
              : 0;
      let swap_line = format_human_size(swap_used_kib * 1024, allocator);
      swap_line += " used of ";
      swap_line +=
          format_human_size(memory.swap_total_kib * 1024, allocator).view();
      append_report_field(output, "Swap", swap_line.view(),
                          colors::ansi::BOLD_CYAN, should_color);
    }
  }

  os::filesystem_status root{};
  if (os::stat_filesystem("/", root) && root.total_blocks > 0) {
    let const unit = root.fundamental_block_size;
    let const used_blocks = root.total_blocks - root.free_blocks;
    let root_line = format_human_size(used_blocks * unit, allocator);
    root_line += " used of ";
    root_line += format_human_size(root.total_blocks * unit, allocator).view();
    root_line += " on ";
    root_line += StringView{root.type_name}.is_empty()
                     ? StringView{"the root filesystem"}
                     : StringView{root.type_name};
    append_report_field(output, "Root", root_line.view(),
                        colors::ansi::BOLD_CYAN, should_color);
    if (FLAG_EVIL_ALL.is_enabled()) {
      append_report_field(
          output, "Root available",
          format_human_size(root.available_blocks * unit, allocator).view(),
          colors::ansi::BOLD_CYAN, should_color);
    }
  }

  let const load = read_first_line("/proc/loadavg", allocator);
  if (load.has_value() && !load->is_empty()) {
    append_report_field(output, "Load", load->view(), colors::ansi::BOLD_CYAN,
                        should_color);
  }

  let const processes = os::enumerate_processes();
  append_report_field(output, "Processes",
                      String::from(processes.count(), allocator).view(),
                      colors::ansi::BOLD_CYAN, should_color);

  if (FLAG_EVIL_ALL.is_enabled()) {
    let const sessions = os::logged_in_users();
    append_report_field(output, "Login sessions",
                        String::from(sessions.count(), allocator).view(),
                        colors::ansi::BOLD_CYAN, should_color);

    let const environment = os::environment_names();
    append_report_field(output, "Environment variables",
                        String::from(environment.count(), allocator).view(),
                        colors::ansi::BOLD_CYAN, should_color);
  }

  let const mounts = os::mounted_filesystems();
  append_report_field(output, "Filesystems",
                      String::from(mounts.count(), allocator).view(),
                      colors::ansi::BOLD_CYAN, should_color);

  let const addresses = os::network_interface_addresses();
  let interface_names = ArrayList<StringView>{allocator};
  interface_names.reserve(addresses.count());
  for (let const &address : addresses)
    interface_names.push(address.interface_name.view());
  interface_names.sort();

  usize interface_count = 0;
  StringView previous_interface;
  for (let const name : interface_names) {
    if (interface_count == 0 || name != previous_interface) interface_count++;
    previous_interface = name;
  }

  let network_line = String::from(interface_count, allocator);
  network_line += interface_count == 1 ? " interface, " : " interfaces, ";
  network_line += String::from(addresses.count(), allocator).view();
  network_line += addresses.count() == 1 ? " address" : " addresses";
  append_report_field(output, "Network", network_line.view(),
                      colors::ansi::BOLD_CYAN, should_color);

  if (FLAG_EVIL_ALL.is_enabled()) {
    append_system_configuration(output, "Argument limit",
                                os::system_configuration_key::ArgMax, allocator,
                                should_color);
    append_system_configuration(output, "Open file maximum",
                                os::system_configuration_key::OpenMax,
                                allocator, should_color);
    append_system_configuration(output, "Child maximum",
                                os::system_configuration_key::ChildMax,
                                allocator, should_color);
    append_system_configuration(output, "Clock ticks per second",
                                os::system_configuration_key::ClockTicks,
                                allocator, should_color);
    append_system_configuration(output, "Page size",
                                os::system_configuration_key::PageSize,
                                allocator, should_color);
    append_resource_limit(output, "Open file limit",
                          os::resource_kind::OpenFiles, allocator,
                          should_color);
    append_resource_limit(output, "Process limit", os::resource_kind::Processes,
                          allocator, should_color);
    append_resource_limit(output, "Core size limit",
                          os::resource_kind::CoreBlocks, allocator,
                          should_color);
    append_procfs_report(output, should_color, allocator);
    append_anomaly_report(output, cxt, should_color);
  }

  ec.print_to_stdout(output);
  return 0;
}

} // namespace koshka::koshkit
