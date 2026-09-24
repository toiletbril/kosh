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
#include "../base/Path.hpp"
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
  let const body = Path{path, allocator}.read_entire_file();
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

fn format_limit_value(u64 value, Allocator allocator) throws -> String
{
  if (value == os::RESOURCE_UNLIMITED) return String{allocator, "unlimited"};

  return String::from(value, allocator);
}

fn append_resource_limit(String &output, StringView name,
                         Allocator allocator, bool should_color,
                         os::resource_kind kind) throws -> void
{
  os::resource_limit limit{};
  if (!os::get_resource_limit(limit, kind)) return;

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
  u64 abi_version{0};
  bool has_version_mix{false};
};

fn append_anomaly_report(String &output, EvalContext &cxt,
                         bool should_color) throws -> void
{
  let const allocator = cxt.scratch_allocator();
  let const do_append_unavailable = [&](StringView cost) throws -> void {
    append_report_field(output, "Mixed library ABIs", "unavailable",
                        colors::ansi::BOLD_CYAN, should_color);
    append_report_field(output, "Deleted code mappings", "unavailable",
                        colors::ansi::BOLD_CYAN, should_color);
    append_report_field(output, "Finding confidence", "unavailable",
                        colors::ansi::BOLD_CYAN, should_color);
    append_report_field(output, "Cost", cost, colors::ansi::BOLD_CYAN,
                        should_color);
  };
  if (!os::has_process_open_file_listing()) {
    do_append_unavailable("not probed");
    return;
  }

  let const files =
      os::list_process_open_files(os::get_current_process_id(), allocator,
                                  true);
  let executable_path = StringView{};
  bool is_mapping_evidence_available = true;
  for (let const &file : files) {
    if (file.use == os::process_file_use::Executable && !file.is_inaccessible)
      executable_path = file.path.view();
    if (file.use == os::process_file_use::Mapped && file.is_inaccessible)
      is_mapping_evidence_available = false;
  }
  if (!is_mapping_evidence_available) {
    do_append_unavailable("process mappings");
    return;
  }

  let families = ArrayList<mapped_library_family>{allocator};
  usize mixed_family_count = 0;
  usize deleted_mapping_count = 0;
  for (let const &file : files) {
    if (file.use != os::process_file_use::Mapped) continue;

    let const filename = Path{file.path.view(), allocator}.filename();
    let const shared_object_marker = filename.find_substring(".so");
    let const marker = filename.find_substring(".so.");
    if (file.is_deleted &&
        (shared_object_marker.has_value() ||
         file.path.view() == executable_path))
      deleted_mapping_count++;
    if (!marker.has_value()) continue;

    let version_end = *marker + 4;
    while (version_end < filename.length && filename[version_end] >= '0' &&
           filename[version_end] <= '9')
      version_end++;
    if (version_end == *marker + 4) continue;
    if (version_end < filename.length && filename[version_end] != '.')
      continue;
    let const abi_version =
        filename.substring_of_length(*marker + 4,
                                     version_end - (*marker + 4))
            .to<u64>();
    if (abi_version.is_error()) continue;

    let const family = filename.substring_of_length(0, *marker);
    usize family_index = 0;
    while (family_index < families.count() &&
           families[family_index].family.view() != family)
      family_index++;
    if (family_index == families.count()) {
      families.push(mapped_library_family{
          String{allocator, family}, abi_version.value(), false
      });
    } else if (families[family_index].abi_version != abi_version.value() &&
               !families[family_index].has_version_mix)
    {
      families[family_index].has_version_mix = true;
      mixed_family_count++;
    }
  }

  append_report_field(output, "Mixed library ABIs",
                      String::from(mixed_family_count, allocator).view(),
                      colors::ansi::BOLD_CYAN, should_color);
  append_report_field(output, "Deleted code mappings",
                      String::from(deleted_mapping_count, allocator).view(),
                      colors::ansi::BOLD_CYAN, should_color);
  append_report_field(output, "Finding confidence", "high",
                      colors::ansi::BOLD_CYAN, should_color);
  append_report_field(output, "Cost", "process mappings",
                      colors::ansi::BOLD_CYAN, should_color);
}

fn append_procfs_report(String &output, bool should_color,
                        Allocator allocator) throws -> void
{
  os::system_activity_status activity{};
  if (os::read_system_activity_status(activity)) {
    struct activity_metric
    {
      os::system_activity_field field;
      StringView label;
      u64 os::system_activity_status::*value;
    };
    static constexpr activity_metric METRICS[] = {
        {os::system_activity_field::Cpu, "CPU user units",
         &os::system_activity_status::cpu_user_units},
        {os::system_activity_field::Cpu, "CPU system units",
         &os::system_activity_status::cpu_system_units},
        {os::system_activity_field::Cpu, "CPU idle units",
         &os::system_activity_status::cpu_idle_units},
        {os::system_activity_field::CpuWait, "CPU wait units",
         &os::system_activity_status::cpu_wait_units},
        {os::system_activity_field::CpuStolen, "CPU stolen units",
         &os::system_activity_status::cpu_stolen_units},
        {os::system_activity_field::Faults, "Page faults",
         &os::system_activity_status::page_fault_count},
        {os::system_activity_field::MajorFaults, "Major page faults",
         &os::system_activity_status::major_page_fault_count},
        {os::system_activity_field::PageInput, "Page input bytes",
         &os::system_activity_status::page_input_bytes},
        {os::system_activity_field::PageOutput, "Page output bytes",
         &os::system_activity_status::page_output_bytes},
        {os::system_activity_field::Runnable, "Runnable processes",
         &os::system_activity_status::runnable_process_count},
        {os::system_activity_field::Blocked, "Blocked processes",
         &os::system_activity_status::blocked_process_count},
        {os::system_activity_field::Interrupts, "Interrupts",
         &os::system_activity_status::interrupt_count},
        {os::system_activity_field::ContextSwitches, "Context switches",
         &os::system_activity_status::context_switch_count},
        {os::system_activity_field::ProcessCreations, "Processes created",
         &os::system_activity_status::process_creation_count},
        {os::system_activity_field::SoftInterrupts, "Soft interrupts",
         &os::system_activity_status::soft_interrupt_count},
        {os::system_activity_field::CpuSomeStall,
         "CPU partial stall microseconds",
         &os::system_activity_status::cpu_some_stall_microseconds},
        {os::system_activity_field::CpuFullStall,
         "CPU full stall microseconds",
         &os::system_activity_status::cpu_full_stall_microseconds},
        {os::system_activity_field::MemorySomeStall,
         "Memory partial stall microseconds",
         &os::system_activity_status::memory_some_stall_microseconds},
        {os::system_activity_field::MemoryFullStall,
         "Memory full stall microseconds",
         &os::system_activity_status::memory_full_stall_microseconds},
        {os::system_activity_field::IoSomeStall,
         "IO partial stall microseconds",
         &os::system_activity_status::io_some_stall_microseconds},
        {os::system_activity_field::IoFullStall,
         "IO full stall microseconds",
         &os::system_activity_status::io_full_stall_microseconds},
        {os::system_activity_field::PageScan, "Pages scanned",
         &os::system_activity_status::page_scan_count},
        {os::system_activity_field::PageSteal, "Pages reclaimed",
         &os::system_activity_status::page_steal_count},
        {os::system_activity_field::DirectReclaim, "Direct reclaim stalls",
         &os::system_activity_status::direct_reclaim_count},
        {os::system_activity_field::CompactionStall, "Compaction stalls",
         &os::system_activity_status::compaction_stall_count},
        {os::system_activity_field::DirtyPages, "Dirty pages",
         &os::system_activity_status::dirty_page_count},
        {os::system_activity_field::WritebackPages, "Writeback pages",
         &os::system_activity_status::writeback_page_count},
        {os::system_activity_field::OomKills, "OOM kills",
         &os::system_activity_status::oom_kill_count},
        {os::system_activity_field::NamespaceCount,
         "Current-process namespaces",
         &os::system_activity_status::namespace_count},
        {os::system_activity_field::CgroupMembershipCount,
         "Current-process cgroup memberships",
         &os::system_activity_status::cgroup_membership_count},
    };
    for (let const &metric : METRICS) {
      if (!activity.has_field(metric.field)) continue;
      append_report_field(
          output, metric.label,
          String::from(activity.*metric.value, allocator).view(),
          colors::ansi::BOLD_CYAN, should_color);
    }
  } else {
    append_report_field(output, "Activity", "unavailable",
                        colors::ansi::BOLD_CYAN, should_color);
  }
  if (activity.has_isolation_probes) {
    if (!activity.has_field(os::system_activity_field::NamespaceCount)) {
      append_report_field(output, "Current-process namespaces", "unavailable",
                          colors::ansi::BOLD_CYAN, should_color);
    }
    if (!activity.has_field(os::system_activity_field::CgroupMembershipCount)) {
      append_report_field(output, "Current-process cgroup memberships",
                          "unavailable", colors::ansi::BOLD_CYAN,
                          should_color);
    }
  }

  os::memory_status memory{};
  if (os::read_memory_status(memory) &&
      memory.has_field(os::memory_status_field::Total))
  {
    let memory_line = String{allocator};
    if (memory.has_field(os::memory_status_field::Available) ||
        memory.has_field(os::memory_status_field::Free))
    {
      memory_line = String::from(memory.available_kib, allocator);
      memory_line += " KiB available of ";
    }
    memory_line += String::from(memory.total_kib, allocator).view();
    memory_line += " KiB";
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

  let const should_color = koshkit_should_color();

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
                      os::read_current_directory().view(),
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
  if (os::read_memory_status(memory) &&
      memory.has_field(os::memory_status_field::Total) &&
      memory.total_kib > 0)
  {
    let memory_line = String{allocator};
    if (memory.has_field(os::memory_status_field::Available) ||
        memory.has_field(os::memory_status_field::Free))
    {
      let const used_kib = memory.total_kib > memory.available_kib
                               ? memory.total_kib - memory.available_kib
                               : 0;
      memory_line = format_human_size(used_kib * 1024, allocator);
      memory_line += " used of ";
    }
    memory_line += format_human_size(memory.total_kib * 1024, allocator).view();
    append_report_field(output, "Memory", memory_line.view(),
                        colors::ansi::BOLD_CYAN, should_color);
    if (FLAG_EVIL_ALL.is_enabled()) {
      let const do_append_memory_size =
          [&](StringView name, os::memory_status_field field,
              u64 value_kib) throws -> void {
        if (!memory.has_field(field)) return;

        let const value_bytes = value_kib > UINT64_MAX / 1024
                                    ? UINT64_MAX
                                    : value_kib * 1024;
        append_report_field(
            output, name, format_human_size(value_bytes, allocator).view(),
            colors::ansi::BOLD_CYAN, should_color);
      };

      if (memory.has_field(os::memory_status_field::Available)) {
        append_report_field(
            output, "Memory available",
            format_human_size(memory.available_kib * 1024, allocator).view(),
            colors::ansi::BOLD_CYAN, should_color);
      }
      if (memory.has_field(os::memory_status_field::Free)) {
        append_report_field(
            output, "Memory free",
            format_human_size(memory.free_kib * 1024, allocator).view(),
            colors::ansi::BOLD_CYAN, should_color);
      }

      do_append_memory_size("Memory buffers",
                            os::memory_status_field::Buffers,
                            memory.buffer_kib);
      do_append_memory_size("Memory page cache",
                            os::memory_status_field::Cached,
                            memory.cached_kib);
      do_append_memory_size("Memory reclaimable slab",
                            os::memory_status_field::ReclaimableSlab,
                            memory.reclaimable_slab_kib);
      do_append_memory_size("Memory shared", os::memory_status_field::Shared,
                            memory.shared_kib);
      do_append_memory_size("Memory slab", os::memory_status_field::Slab,
                            memory.slab_kib);
      do_append_memory_size("Memory active", os::memory_status_field::Active,
                            memory.active_kib);
      do_append_memory_size("Memory inactive",
                            os::memory_status_field::Inactive,
                            memory.inactive_kib);
      do_append_memory_size("Memory commit limit",
                            os::memory_status_field::CommitLimit,
                            memory.commit_limit_kib);
      do_append_memory_size("Memory committed",
                            os::memory_status_field::Committed,
                            memory.committed_kib);
      if (memory.has_field(os::memory_status_field::HugePagesTotal)) {
        append_report_field(
            output, "Huge pages total",
            String::from(memory.huge_page_total_count, allocator).view(),
            colors::ansi::BOLD_CYAN, should_color);
      }
      if (memory.has_field(os::memory_status_field::HugePagesFree)) {
        append_report_field(
            output, "Huge pages free",
            String::from(memory.huge_page_free_count, allocator).view(),
            colors::ansi::BOLD_CYAN, should_color);
      }
    }

    if (memory.has_field(os::memory_status_field::SwapTotal) &&
        memory.has_field(os::memory_status_field::SwapFree) &&
        memory.swap_total_kib > 0)
    {
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
                          allocator, should_color,
                          os::resource_kind::OpenFiles);
    append_resource_limit(output, "Process limit", allocator, should_color,
                          os::resource_kind::Processes);
    append_resource_limit(output, "Core size limit",
                          allocator, should_color,
                          os::resource_kind::CoreBlocks);
    append_procfs_report(output, should_color, allocator);
    append_anomaly_report(output, cxt, should_color);
  }

  ec.print_to_stdout(output);
  return 0;
}

} // namespace koshka::koshkit
