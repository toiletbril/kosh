/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements eviliso. It reports namespace, cgroup, login-session,
 * and remote-socket evidence for the current isolation environment.
 */

#include "../CLI.hpp"
#include "../CLIColors.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Platform.hpp"
#include "../Toiletline.hpp"
#include "../Utils.hpp"
#include "../base/Path.hpp"

#include <cstdlib>

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-a] [-n] [-c] [-s] [-r] [-k] [--kubernetes] [--container] "
                   "[--containers]");

HELP_DESCRIPTION_DECL(
    "The eviliso utility reports namespaces, cgroups, sessions, remote "
    "connections, container runtimes, containers, and Kubernetes.");

FLAG(EVILISO_ALL, Bool, 'a', "all",
     "Include detailed rows for every selected section.");
FLAG(EVILISO_NAMESPACES, Bool, 'n', "namespaces", "Report process namespaces.");
FLAG(EVILISO_CGROUPS, Bool, 'c', "cgroups", "Report cgroup membership.");
FLAG(EVILISO_SESSIONS, Bool, 's', "sessions", "Report login sessions.");
FLAG(EVILISO_REMOTE, Bool, 'r', "remote",
     "Select remote peers and socket summary.");
FLAG(EVILISO_RUNTIME, Bool, 'k', "runtime",
     "Report container runtime evidence.");
FLAG(EVILISO_KUBERNETES, Bool, '\0', "kubernetes",
     "Report Kubernetes evidence.");
FLAG(EVILISO_CONTAINER, Bool, '\0', "container", "Report detected containers.");
FLAG(EVILISO_CONTAINERS, Bool, '\0', "containers",
     "Report detected containers.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(EvilIso);

namespace koshka::koshkit {

namespace {

enum class eviliso_detail_mode : u8
{
  Summary,
  All,
};

enum class eviliso_collection_mode : u8
{
  Skip,
  Collect,
};

enum class eviliso_remote_rows_mode : u8
{
  Hide,
  Show,
};

struct namespace_process
{
  namespace_process(i64 process_id, StringView name, bool is_self,
                    Allocator allocator)
      : process_id(process_id), name(allocator, name), is_self(is_self)
  {}

  i64 process_id{0};
  String name;
  bool is_self{false};
};

struct namespace_relation
{
  explicit namespace_relation(Allocator allocator)
      : identifier(allocator), name(allocator)
  {}

  StringView type;
  usize type_index{0};
  String identifier;
  u64 identifier_value{0};
  i64 process_id{0};
  String name;
  StringView role;
  bool is_identifier_numeric{false};
  bool is_available{false};
};

fn eviliso_namespace_process_override(Allocator allocator) throws
    -> Maybe<ArrayList<namespace_process>>
{
#ifndef NDEBUG
  if (let const *path = std::getenv("KOSH_TEST_EVILISO_NAMESPACE_PROCESSES");
      path != nullptr && path[0] != '\0')
  {
    let processes = ArrayList<namespace_process>{allocator};
    let const contents = Path{path, allocator}.read_entire_file();
    if (!contents.has_value()) return processes;
    for (let const line : utils::split_lines(contents->view())) {
      let const process_end = line.find_character('|');
      if (!process_end.has_value()) continue;
      let const remainder = line.substring(*process_end + 1);
      let const name_end = remainder.find_character('|');
      if (!name_end.has_value()) continue;
      let const process_id =
          line.substring_of_length(0, *process_end).to<i64>();
      if (process_id.is_error()) continue;
      let const name = remainder.substring_of_length(0, *name_end);
      let const role = remainder.substring(*name_end + 1);
      if (role != "self" && role != "other") continue;
      processes.push(namespace_process{process_id.value(), name, role == "self",
                                       allocator});
    }
    return processes;
  }
#endif
  unused(allocator);
  return None;
}

fn eviliso_namespace_proc_path(StringView suffix, Allocator allocator) throws
    -> String
{
#ifndef NDEBUG
  if (let const *root = std::getenv("KOSH_TEST_EVILISO_NAMESPACE_PROC");
      root != nullptr && root[0] != '\0')
  {
    let path = String{allocator, root};
    path += '/';
    path += suffix;
    return path;
  }
#endif
  let path = String{allocator, "/proc/"};
  path += suffix;
  return path;
}

fn namespace_identifier(StringView target, Allocator allocator) throws -> String
{
  let const open = target.find_character('[');
  if (!open.has_value()) return String{allocator, target};
  let const after_open = target.substring(*open + 1);
  let const close = after_open.find_character(']');
  if (!close.has_value()) return String{allocator, target};
  return String{allocator, after_open.substring_of_length(0, *close)};
}

fn append_namespace_report(String &output, bool should_color,
                           ArrayList<namespace_process> processes,
                           eviliso_detail_mode detail) throws -> void
{
  constexpr StringView names[] = {"cgroup", "ipc",  "mnt",  "net",
                                  "pid",    "time", "user", "uts"};
  constexpr usize NAME_COUNT = sizeof(names) / sizeof(*names);
  let const allocator = processes.allocator();
  let const namespace_path = eviliso_namespace_proc_path("self/ns", allocator);
  if (!Path{namespace_path.view(), allocator}.is_directory()) {
    processes.clear();
    processes.push(
        namespace_process{os::get_current_process_id(), "-", true, allocator});
  }
  let relations = ArrayList<namespace_relation>{allocator};
  relations.reserve(processes.count() * NAME_COUNT);
  for (usize type_index = 0; type_index < NAME_COUNT; type_index++) {
    let const type = names[type_index];
    let self_suffix = String{allocator, "self/ns/"};
    self_suffix += type;
    let const self_target = os::read_symlink(
        eviliso_namespace_proc_path(self_suffix.view(), allocator), allocator);
    for (let const &process : processes) {
      let target = Maybe<String>{};
      if (process.is_self) {
        if (self_target.has_value()) {
          target = String{allocator, self_target->view()};
        }
      } else {
        let process_suffix = String::from(process.process_id, allocator);
        process_suffix += "/ns/";
        process_suffix += type;
        target = os::read_symlink(
            eviliso_namespace_proc_path(process_suffix.view(), allocator),
            allocator);
      }

      let identifier = target.has_value()
                           ? namespace_identifier(target->view(), allocator)
                           : String{allocator, "unavailable"};
      let const identifier_value = identifier.view().to<u64>();
      let relation = namespace_relation{allocator};
      relation.type = type;
      relation.type_index = type_index;
      relation.identifier = steal(identifier);
      relation.identifier_value =
          identifier_value.is_error() ? 0 : identifier_value.value();
      relation.process_id = process.process_id;
      relation.name = String{allocator, process.name.view()};
      relation.role =
          process.is_self ? StringView{"self"} : StringView{"other"};
      relation.is_identifier_numeric = !identifier_value.is_error();
      relation.is_available = target.has_value();
      relations.push(steal(relation));
    }
  }

  relations.sort(
      [](const namespace_relation &left, const namespace_relation &right) {
        if (left.type_index != right.type_index)
          return left.type_index < right.type_index;
        if (left.is_available != right.is_available) return left.is_available;
        if (left.is_identifier_numeric != right.is_identifier_numeric)
          return left.is_identifier_numeric;
        if (left.is_identifier_numeric &&
            left.identifier_value != right.identifier_value)
        {
          return left.identifier_value < right.identifier_value;
        }
        if (left.identifier != right.identifier)
          return left.identifier < right.identifier;
        return left.process_id < right.process_id;
      });

  let table = ReportTable{allocator};
  table.add_column("TYPE", report_table_alignment::Left,
                   colors::ansi::BOLD_CYAN);
  table.add_column("ID", report_table_alignment::Right,
                   colors::ansi::BOLD_CYAN);
  if (detail == eviliso_detail_mode::All) {
    table.add_column("PID", report_table_alignment::Right,
                     colors::ansi::BOLD_CYAN);
    table.add_column("NAME", report_table_alignment::Left,
                     colors::ansi::BOLD_CYAN);
    table.add_column("ROLE", report_table_alignment::Left,
                     colors::ansi::BOLD_CYAN);
  } else {
    table.add_column("PROCESSES", report_table_alignment::Right,
                     colors::ansi::BOLD_CYAN);
  }

  let cells = ArrayList<report_table_cell_view>{allocator};
  cells.reserve(5);
  if (detail == eviliso_detail_mode::All) {
    for (let const &relation : relations) {
      let process_id = String::from(relation.process_id, allocator);
      cells.clear();
      cells.push({relation.type, colors::ansi::BOLD_MAGENTA});
      cells.push({relation.identifier.view(), colors::ansi::RESET});
      cells.push({process_id.view(), colors::ansi::BOLD_GREEN});
      cells.push({relation.name.view(), colors::ansi::RESET});
      cells.push({relation.role, colors::ansi::BOLD_MAGENTA});
      table.add_row(cells);
    }
    append_titled_report_table(output, "Namespaces", table, should_color);
    return;
  }

  usize relation_index = 0;
  while (relation_index < relations.count()) {
    let const &first = relations[relation_index];
    usize group_end = relation_index + 1;
    while (group_end < relations.count()) {
      let const &candidate = relations[group_end];
      if (candidate.type != first.type ||
          candidate.identifier != first.identifier)
      {
        break;
      }
      group_end++;
    }
    let count = String::from(group_end - relation_index, allocator);
    cells.clear();
    cells.push({first.type, colors::ansi::BOLD_MAGENTA});
    cells.push({first.identifier.view(), colors::ansi::RESET});
    cells.push({count.view(), colors::ansi::BOLD_GREEN});
    table.add_row(cells);
    relation_index = group_end;
  }
  append_titled_report_table(output, "Namespaces", table, should_color);
}

struct cgroup_membership
{
  cgroup_membership(StringView hierarchy, u64 hierarchy_value,
                    StringView controller, StringView path, Allocator allocator)
      : hierarchy(allocator, hierarchy), hierarchy_value(hierarchy_value),
        controller(allocator, controller), path(allocator, path)
  {}

  String hierarchy;
  u64 hierarchy_value{0};
  String controller;
  String path;
};

fn cgroup_proc_path(StringView suffix, Allocator allocator) throws -> String
{
#ifndef NDEBUG
  if (let const *root = std::getenv("KOSH_TEST_CGROUP_PROC");
      root != nullptr && root[0] != '\0')
  {
    let path = String{allocator, root};
    path += '/';
    path += suffix;
    return path;
  }
#endif
  let path = String{allocator, "/proc/"};
  path += suffix;
  return path;
}

fn container_marker_path(StringView suffix, Allocator allocator) throws
    -> String
{
#ifndef NDEBUG
  if (let const *root = std::getenv("KOSH_TEST_EVILISO_MARKER_ROOT");
      root != nullptr && root[0] != '\0')
  {
    let path = String{allocator, root};
    path += '/';
    path += suffix;
    return path;
  }
#endif
  let path = String{allocator, "/"};
  path += suffix;
  return path;
}

fn kubernetes_service_account_path(StringView name, Allocator allocator) throws
    -> String
{
#ifndef NDEBUG
  if (let const *root = std::getenv("KOSH_TEST_EVILISO_SERVICE_ACCOUNT_ROOT");
      root != nullptr && root[0] != '\0')
  {
    let path = String{allocator, root};
    path += '/';
    path += name;
    return path;
  }
#endif
  let path =
      String{allocator, "/var/run/secrets/kubernetes.io/serviceaccount/"};
  path += name;
  return path;
}

fn parse_cgroup_memberships(StringView text, Allocator allocator) throws
    -> ArrayList<cgroup_membership>
{
  let memberships = ArrayList<cgroup_membership>{allocator};
  for (let const line : utils::split_lines(text)) {
    let const first_separator = line.find_character(':');
    if (!first_separator.has_value()) continue;
    let const remainder = line.substring(*first_separator + 1);
    let const second_separator = remainder.find_character(':');
    if (!second_separator.has_value()) continue;
    let const hierarchy = line.substring_of_length(0, *first_separator);
    let const controller = remainder.substring_of_length(0, *second_separator);
    let const path = remainder.substring(*second_separator + 1);
    if (hierarchy.is_empty() || path.is_empty()) continue;
    let const hierarchy_value = hierarchy.to<u64>();
    if (hierarchy_value.is_error()) continue;
    let const controller_name =
        controller.is_empty() ? StringView{"unified"} : controller;
    bool is_known = false;
    for (let const &membership : memberships) {
      if (membership.hierarchy.view() == hierarchy &&
          membership.controller.view() == controller_name &&
          membership.path.view() == path)
      {
        is_known = true;
        break;
      }
    }
    if (is_known) continue;
    memberships.push(cgroup_membership{hierarchy, hierarchy_value.value(),
                                       controller_name, path, allocator});
  }
  return memberships;
}

enum class process_snapshot_status : u8
{
  Available,
  PermissionDenied,
  Unavailable,
  Empty,
  Exited,
  Reused,
  Unverifiable,
};

pure fn process_snapshot_identity_is_valid(
    process_snapshot_status status) wontthrow -> bool
{
  return status != process_snapshot_status::Exited &&
         status != process_snapshot_status::Reused &&
         status != process_snapshot_status::Unverifiable;
}

struct process_cgroup_snapshot
{
  explicit process_cgroup_snapshot(Allocator allocator)
      : name(allocator), command(allocator), memberships(allocator),
        evidence(allocator)
  {}

  i64 process_id{0};
  u64 start_token{0};
  u32 owner_id{0};
  String name;
  String command;
  ArrayList<cgroup_membership> memberships;
  struct identity_evidence
  {
    identity_evidence(StringView path, Allocator allocator)
        : runtime(allocator, "-"), container_id(allocator, "-"),
          pod_uid(allocator, "-"), qos(allocator, "-"), path(allocator, path)
    {}

    String runtime;
    String container_id;
    String pod_uid;
    String qos;
    bool is_kubernetes{false};
    String path;
  };
  ArrayList<identity_evidence> evidence;
  process_snapshot_status status{process_snapshot_status::Unavailable};
};

pure fn is_hexadecimal_id(StringView text) wontthrow -> bool
{
  if (text.length != 64) return false;
  for (usize index = 0; index < text.length; index++) {
    let const byte = text[index];
    if (!((byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f') ||
          (byte >= 'A' && byte <= 'F')))
      return false;
  }
  return true;
}

pure fn has_suffix(StringView text, StringView suffix) wontthrow -> bool
{
  return text.length >= suffix.length &&
         text.substring(text.length - suffix.length) == suffix;
}

pure fn scoped_container_id(StringView component, StringView prefix) wontthrow
    -> Maybe<StringView>
{
  constexpr StringView SUFFIX = ".scope";
  if (!component.starts_with(prefix) || !has_suffix(component, SUFFIX) ||
      component.length != prefix.length + 64 + SUFFIX.length)
    return None;
  let const identifier = component.substring_of_length(prefix.length, 64);
  return is_hexadecimal_id(identifier) ? Maybe<StringView>{identifier} : None;
}

pure fn normalized_pod_uid(StringView component, Allocator allocator) throws
    -> String
{
  struct pod_component_form
  {
    StringView prefix;
    StringView suffix;
  };
  static constexpr pod_component_form FORMS[] = {
      {"pod",                     ""      },
      {"kubepods-pod",            ".slice"},
      {"kubepods-burstable-pod",  ".slice"},
      {"kubepods-besteffort-pod", ".slice"},
  };
  let identifier = StringView{};
  for (let const &form : FORMS) {
    if (component.length != form.prefix.length + 36 + form.suffix.length ||
        !component.starts_with(form.prefix) ||
        !has_suffix(component, form.suffix))
      continue;
    identifier = component.substring_of_length(form.prefix.length, 36);
    break;
  }
  if (identifier.is_empty()) return String{allocator, "-"};

  let result = String{allocator};
  result.reserve(36);
  for (usize index = 0; index < identifier.length; index++) {
    let byte = identifier[index];
    if (byte == '_') byte = '-';
    let const should_be_separator =
        index == 8 || index == 13 || index == 18 || index == 23;
    let const is_valid = should_be_separator
                             ? byte == '-'
                             : (byte >= '0' && byte <= '9') ||
                                   (byte >= 'a' && byte <= 'f') ||
                                   (byte >= 'A' && byte <= 'F');
    if (!is_valid) return String{allocator, "-"};
    result.push(byte >= 'A' && byte <= 'F' ? byte - 'A' + 'a' : byte);
  }
  return result;
}

fn parse_cgroup_identity(StringView path, Allocator allocator) throws
    -> process_cgroup_snapshot::identity_evidence
{
  let result = process_cgroup_snapshot::identity_evidence{path, allocator};
  let previous = StringView{};
  let remaining = path;
  bool has_kubernetes_component = false;
  while (!remaining.is_empty()) {
    if (remaining[0] == '/') {
      remaining = remaining.substring(1);
      continue;
    }
    let const separator = remaining.find_character('/');
    let const component = separator.has_value()
                              ? remaining.substring_of_length(0, *separator)
                              : remaining;
    remaining = separator.has_value() ? remaining.substring(*separator + 1)
                                      : StringView{};

    if (component == "docker" || component == "containerd" ||
        component == "crio" || component == "libpod")
    {
      result.runtime = component == "crio"     ? String{allocator, "cri-o"}
                       : component == "libpod" ? String{allocator, "podman"}
                                               : String{allocator, component};
    }

    struct scoped_runtime
    {
      StringView prefix;
      StringView runtime;
    };
    static constexpr scoped_runtime SCOPED_RUNTIMES[] = {
        {"docker-",         "docker"    },
        {"cri-containerd-", "containerd"},
        {"crio-",           "cri-o"     },
        {"libpod-",         "podman"    },
    };
    for (let const &candidate : SCOPED_RUNTIMES) {
      let const identifier = scoped_container_id(component, candidate.prefix);
      if (!identifier.has_value()) continue;
      result.runtime = String{allocator, candidate.runtime};
      result.container_id = String{allocator, *identifier};
      break;
    }
    if (result.container_id == "-" && is_hexadecimal_id(component) &&
        (previous == "docker" || previous == "containerd" ||
         previous == "crio" || previous == "libpod" ||
         has_kubernetes_component))
      result.container_id = String{allocator, component};

    let pod_uid = normalized_pod_uid(component, allocator);
    let const has_pod_uid = pod_uid != "-";
    if (has_pod_uid) result.pod_uid = steal(pod_uid);
    if (component == "burstable" || component == "kubepods-burstable.slice" ||
        (has_pod_uid && component.starts_with("kubepods-burstable-pod")))
      result.qos = "burstable";
    if (component == "besteffort" || component == "kubepods-besteffort.slice" ||
        (has_pod_uid && component.starts_with("kubepods-besteffort-pod")))
      result.qos = "besteffort";
    if (component == "kubepods" || component == "kubepods.slice") {
      result.is_kubernetes = true;
      has_kubernetes_component = true;
    }
    previous = component;
  }
  if (result.pod_uid != "-" && result.qos == "-") result.qos = "guaranteed";
  return result;
}

fn collect_process_cgroup_snapshot(Allocator allocator,
                                   eviliso_collection_mode collection) throws
    -> ArrayList<process_cgroup_snapshot>
{
  let candidates = os::enumerate_processes(os::process_detail::ResourceStats);
  let snapshot = ArrayList<process_cgroup_snapshot>{allocator};
  snapshot.reserve(candidates.count());
  for (let const &process : candidates) {
    let record = process_cgroup_snapshot{allocator};
    record.process_id = process.pid;
    record.start_token = process.start_token;
    record.owner_id = process.owner_id;
    record.name = process.name.is_empty()
                      ? String{allocator, "-"}
                      : String{allocator, process.name.view()};
    record.command = process.command_line.is_empty()
                         ? String{allocator, "-"}
                         : String{allocator, process.command_line.view()};
    if (collection == eviliso_collection_mode::Collect) {
      let suffix = String::from(process.pid, allocator);
      suffix += "/cgroup";
      let const contents =
          Path{cgroup_proc_path(suffix.view(), allocator), allocator}
              .read_entire_file();
      if (!contents.has_value()) {
        record.status = os::last_system_error_is_permission_denied()
                            ? process_snapshot_status::PermissionDenied
                            : process_snapshot_status::Unavailable;
      } else {
        record.memberships =
            parse_cgroup_memberships(contents->view(), allocator);
        record.status = record.memberships.is_empty()
                            ? process_snapshot_status::Empty
                            : process_snapshot_status::Available;
        record.evidence.reserve(record.memberships.count());
        for (let const &membership : record.memberships) {
          record.evidence.push(
              parse_cgroup_identity(membership.path.view(), allocator));
        }
      }
    }
    snapshot.push(steal(record));
  }

  let current = os::enumerate_processes(os::process_detail::ResourceStats);
  current.sort(
      [](const os::process_entry &left, const os::process_entry &right) {
        return left.pid < right.pid;
      });
  for (let &candidate : snapshot) {
    usize lower = 0;
    usize upper = current.count();
    while (lower < upper) {
      let const middle = lower + (upper - lower) / 2;
      if (current[middle].pid < candidate.process_id)
        lower = middle + 1;
      else
        upper = middle;
    }
    if (lower == current.count() || current[lower].pid != candidate.process_id)
    {
      candidate.status = process_snapshot_status::Exited;
      candidate.memberships.clear();
      candidate.evidence.clear();
      continue;
    }
    let const &process = current[lower];
    if (process.start_token == 0 || candidate.start_token == 0) {
      candidate.status = process_snapshot_status::Unverifiable;
      candidate.memberships.clear();
      candidate.evidence.clear();
      continue;
    }
    if (process.start_token != candidate.start_token) {
      candidate.status = process_snapshot_status::Reused;
      candidate.memberships.clear();
      candidate.evidence.clear();
      continue;
    }
  }
  return snapshot;
}

struct cgroup_report_row
{
  cgroup_report_row(StringView hierarchy, u64 hierarchy_value,
                    StringView controller, StringView path,
                    const process_cgroup_snapshot &process, StringView role,
                    StringView status, Allocator allocator)
      : hierarchy(allocator, hierarchy), hierarchy_value(hierarchy_value),
        controller(allocator, controller), path(allocator, path),
        process_id(String::from(process.process_id, allocator)),
        process_id_value(process.process_id),
        process_start_token(process.start_token),
        name(allocator, process.name.view()), role(role), status(status)
  {}

  String hierarchy;
  u64 hierarchy_value{0};
  String controller;
  String path;
  String process_id;
  i64 process_id_value{0};
  u64 process_start_token{0};
  String name;
  StringView role;
  StringView status;
};

pure fn process_snapshot_status_name(process_snapshot_status status) wontthrow
    -> StringView
{
  switch (status) {
  case process_snapshot_status::Available: return "available";
  case process_snapshot_status::PermissionDenied: return "permission denied";
  case process_snapshot_status::Unavailable: return "unavailable";
  case process_snapshot_status::Empty: return "empty";
  case process_snapshot_status::Exited: return "exited";
  case process_snapshot_status::Reused: return "PID reused";
  case process_snapshot_status::Unverifiable: return "identity unavailable";
  }
  unreachable("unknown process snapshot status");
}

fn append_cgroup_failure_report(
    String &output, bool should_color,
    const ArrayList<process_cgroup_snapshot> &snapshot) throws -> void
{
  let const allocator = snapshot.allocator();
  let table = ReportTable{allocator};
  table.add_column("PID", report_table_alignment::Right,
                   colors::ansi::BOLD_CYAN);
  table.add_column("PROCESS", report_table_alignment::Left,
                   colors::ansi::BOLD_CYAN);
  table.add_column("STATUS", report_table_alignment::Left,
                   colors::ansi::BOLD_CYAN);

  bool has_failure = false;
  for (let const &process : snapshot) {
    if (process.status == process_snapshot_status::Available) continue;

    has_failure = true;
    let const process_id = String::from(process.process_id, allocator);
    let cells = ArrayList<report_table_cell_view>{allocator};
    cells.push({process_id.view(), colors::ansi::BOLD_GREEN});
    cells.push({process.name.is_empty() ? StringView{"-"}
                                          : process.name.view(),
                colors::ansi::RESET});
    cells.push({process_snapshot_status_name(process.status),
                colors::ansi::BOLD_YELLOW});
    table.add_row(cells);
  }

  if (!has_failure) {
    let cells = ArrayList<report_table_cell_view>{allocator};
    cells.reserve(3);
    cells.push({"-", colors::ansi::BOLD_GREEN});
    cells.push({"self", colors::ansi::RESET});
    cells.push({"Unavailable", colors::ansi::BOLD_YELLOW});
    table.add_row(cells);
  }
  append_titled_report_table(output, "Cgroup status failures", table,
                             should_color);
}

fn append_cgroup_report(String &output, bool should_color,
                        const ArrayList<process_cgroup_snapshot> &snapshot,
                        eviliso_detail_mode detail) throws -> void
{
  let const allocator = snapshot.allocator();
  let table = ReportTable{allocator};
  let self_index = Maybe<usize>{};
  let const self_process_id = os::get_current_process_id();
  for (usize index = 0; index < snapshot.count(); index++) {
    if (snapshot[index].process_id == self_process_id) {
      self_index = index;
      break;
    }
  }
  if (!self_index.has_value()) {
    append_cgroup_failure_report(output, should_color, snapshot);
    return;
  }

  let const &self = snapshot[*self_index];
  let rows = ArrayList<cgroup_report_row>{allocator};
  for (let const &membership : self.memberships) {
    rows.push(cgroup_report_row{
        membership.hierarchy.view(), membership.hierarchy_value,
        membership.controller.view(), membership.path.view(), self, "self",
        "available", allocator});
  }

  if (detail == eviliso_detail_mode::All) {
    for (let const &process : snapshot) {
      if (process.process_id == self_process_id) continue;
      for (let const &membership : process.memberships) {
        for (let const &self_membership : self.memberships) {
          if (membership.hierarchy != self_membership.hierarchy ||
              membership.controller != self_membership.controller ||
              membership.path != self_membership.path)
          {
            continue;
          }
          rows.push(cgroup_report_row{
              membership.hierarchy.view(), membership.hierarchy_value,
              membership.controller.view(), membership.path.view(), process,
              "other", "available", allocator});
          break;
        }
      }
    }
  }

  rows.sort([](const cgroup_report_row &left, const cgroup_report_row &right) {
    if (left.hierarchy_value != right.hierarchy_value)
      return left.hierarchy_value < right.hierarchy_value;
    if (left.controller != right.controller)
      return left.controller < right.controller;
    if (left.path != right.path) return left.path < right.path;
    return left.process_id_value < right.process_id_value;
  });

  let report = ReportTable{allocator};
  report.add_column("HIERARCHY", report_table_alignment::Right,
                    colors::ansi::BOLD_CYAN);
  report.add_column("CONTROLLER", report_table_alignment::Left,
                    colors::ansi::BOLD_CYAN);
  report.add_column("PATH", report_table_alignment::Left,
                    colors::ansi::BOLD_CYAN);
  report.add_column("PID", report_table_alignment::Right,
                    colors::ansi::BOLD_CYAN);
  report.add_column("NAME", report_table_alignment::Left,
                    colors::ansi::BOLD_CYAN);
  report.add_column("ROLE", report_table_alignment::Left,
                    colors::ansi::BOLD_CYAN);
  if (detail == eviliso_detail_mode::All)
    report.add_column("STATUS", report_table_alignment::Left,
                      colors::ansi::BOLD_CYAN);
  let cells = ArrayList<report_table_cell_view>{allocator};
  cells.reserve(7);
  for (let const &row : rows) {
    cells.clear();
    cells.push({row.hierarchy.view(), colors::ansi::BOLD_GREEN});
    cells.push({row.controller.view(), colors::ansi::RESET});
    cells.push({row.path.view(), colors::ansi::RESET});
    cells.push({row.process_id.view(), colors::ansi::BOLD_GREEN});
    cells.push({row.name.view(), colors::ansi::RESET});
    cells.push({row.role, colors::ansi::BOLD_MAGENTA});
    if (detail == eviliso_detail_mode::All)
      cells.push({row.status, row.status == "available"
                                  ? colors::ansi::BOLD_GREEN
                                  : colors::ansi::BOLD_YELLOW});
    report.add_row(cells);
  }
  append_titled_report_table(output, "Cgroup membership", report, should_color);
  append_cgroup_failure_report(output, should_color, snapshot);
}

fn eviliso_sessions() throws -> ArrayList<os::user_session>
{
#ifndef NDEBUG
  if (let const *path = std::getenv("KOSH_TEST_EVILISO_SESSIONS");
      path != nullptr && path[0] != '\0')
  {
    let sessions = ArrayList<os::user_session>{heap_allocator()};
    let const contents = Path{path}.read_entire_file();
    if (!contents.has_value()) return sessions;
    for (let const line : utils::split_lines(contents->view())) {
      let const user_end = line.find_character('|');
      if (!user_end.has_value()) continue;
      let const remainder = line.substring(*user_end + 1);
      let const terminal_end = remainder.find_character('|');
      if (!terminal_end.has_value()) continue;
      let const user = line.substring_of_length(0, *user_end);
      let const terminal = remainder.substring_of_length(0, *terminal_end);
      let const login_time = remainder.substring(*terminal_end + 1).to<i64>();
      if (user.is_empty() || terminal.is_empty() || login_time.is_error())
        continue;
      sessions.push({
          String{heap_allocator(), user    },
          String{heap_allocator(), terminal},
          login_time.value(),
      });
    }
    return sessions;
  }
#endif
  return os::logged_in_users();
}

struct session_report_row
{
  explicit session_report_row(Allocator allocator)
      : user(allocator), terminal(allocator), login_time(allocator)
  {}

  String user;
  String terminal;
  String login_time;
};

fn append_session_report(String &output, bool should_color, Allocator allocator,
                         eviliso_detail_mode detail) throws -> void
{
  let sessions = eviliso_sessions();
  sessions.sort(
      [](const os::user_session &left, const os::user_session &right) {
        if (left.user != right.user) return left.user < right.user;
        if (left.terminal != right.terminal)
          return left.terminal < right.terminal;
        return left.login_time < right.login_time;
      });

  let rows = ArrayList<session_report_row>{allocator};
  for (let const &session : sessions) {
    let login_time = String{allocator};
    if (detail == eviliso_detail_mode::All) {
      login_time =
          session.login_time == 0
              ? String{allocator, "unavailable"}
              : String{allocator, utils::format_unix_timestamp(
                                      session.login_time, "%Y-%m-%d %H:%M:%S")};
    }
    let row = session_report_row{allocator};
    row.user = String{allocator, session.user.view()};
    row.terminal = String{allocator, session.terminal.view()};
    row.login_time = steal(login_time);
    rows.push(steal(row));
  }

  let table = ReportTable{allocator};
  table.add_column("USER", report_table_alignment::Left,
                   colors::ansi::BOLD_CYAN);
  table.add_column("TERMINAL", report_table_alignment::Left,
                   colors::ansi::BOLD_CYAN);
  if (detail == eviliso_detail_mode::All) {
    table.add_column("LOGIN TIME", report_table_alignment::Left,
                     colors::ansi::BOLD_CYAN);
  }
  for (let const &row : rows) {
    let cells = ArrayList<report_table_cell_view>{allocator};
    cells.push({row.user.view(), colors::ansi::BOLD_GREEN});
    cells.push({row.terminal.view(), colors::ansi::RESET});
    if (detail == eviliso_detail_mode::All) {
      cells.push({row.login_time.view(), colors::ansi::RESET});
    }
    table.add_row(cells);
  }
  append_titled_report_table(output, "Sessions", table, should_color);
}

pure fn remote_state_name(os::network_socket_state state) wontthrow
    -> StringView
{
  switch (state) {
  case os::network_socket_state::Unconnected: return "UNCONN";
  case os::network_socket_state::Listen: return "LISTEN";
  case os::network_socket_state::SynSent: return "SYN-SENT";
  case os::network_socket_state::SynReceived: return "SYN-RECV";
  case os::network_socket_state::Established: return "ESTAB";
  case os::network_socket_state::CloseWait: return "CLOSE-WAIT";
  case os::network_socket_state::FinWait1: return "FIN-WAIT-1";
  case os::network_socket_state::Closing: return "CLOSING";
  case os::network_socket_state::LastAck: return "LAST-ACK";
  case os::network_socket_state::FinWait2: return "FIN-WAIT-2";
  case os::network_socket_state::TimeWait: return "TIME-WAIT";
  case os::network_socket_state::Closed: return "CLOSED";
  case os::network_socket_state::Unknown: return "UNKNOWN";
  }

  unreachable("unknown network socket state");
}

pure fn is_remote_socket(const os::network_socket_entry &socket) wontthrow
    -> bool
{
  return socket.protocol != os::network_socket_protocol::Unix &&
         !socket.peer_address.is_empty() && socket.peer_port != 0;
}

fn remote_endpoint(StringView address, u16 port, Allocator allocator,
                   os::network_address_family family) throws -> String
{
  let result = String{allocator};
  if (family == os::network_address_family::IPv6) result += "[";
  result += address.is_empty() ? StringView{"*"} : address;
  if (family == os::network_address_family::IPv6) result += "]";
  result += ":";
  result += port == 0 ? StringView{"*"} : String::from(port, allocator).view();
  return result;
}

fn remote_table_text(StringView text, usize maximum_cells,
                     Allocator allocator) throws -> String
{
  let result = String{allocator};
  let const byte_limit = maximum_cells * 8;
  let const copied_bytes = text.length < byte_limit ? text.length : byte_limit;
  result.reserve(copied_bytes);
  for (usize position = 0; position < copied_bytes; position++) {
    let const byte = text[position];
    result.push((static_cast<unsigned char>(byte) < 32 || byte == 127) ? ' '
                                                                       : byte);
  }
  if (copied_bytes < text.length &&
      (static_cast<unsigned char>(text[copied_bytes]) & 0xc0) == 0x80)
  {
    usize complete_bytes = copied_bytes;
    while (complete_bytes > 0 &&
           (static_cast<unsigned char>(text[complete_bytes - 1]) & 0xc0) ==
               0x80)
      complete_bytes--;
    if (complete_bytes > 0) complete_bytes--;
    result.truncate(complete_bytes);
  }
  if (copied_bytes < text.length ||
      toiletline::get_display_width(result.view()) > maximum_cells)
  {
    usize actual_cells = 0;
    let const kept_bytes =
        toiletline::get_byte_offset_at_or_before_display_cell(
            result.view(), maximum_cells - 3, actual_cells);
    result.truncate(kept_bytes);
    result += "...";
  }
  return result;
}

fn append_remote_report(String &output, bool should_color,
                        const ArrayList<process_cgroup_snapshot> &snapshot,
                        Allocator allocator, eviliso_remote_rows_mode rows_mode,
                        eviliso_detail_mode detail) throws -> void
{
  let table = ReportTable{allocator};
  if (!os::has_network_socket_listing()) {
    table.add_column("RESOURCE", report_table_alignment::Left,
                     colors::ansi::BOLD_CYAN);
    table.add_column("STATUS", report_table_alignment::Left,
                     colors::ansi::BOLD_CYAN);
    table.add_column("REASON", report_table_alignment::Left,
                     colors::ansi::BOLD_CYAN);
    let cells = ArrayList<report_table_cell_view>{allocator};
    cells.reserve(3);
    cells.push({"Sockets", colors::ansi::BOLD_CYAN});
    cells.push({"unavailable", colors::ansi::BOLD_YELLOW});
    cells.push({"platform does not expose socket records", colors::ansi::RESET});
    table.add_row(cells);
    append_titled_report_table(output, "Socket status failures", table,
                               should_color);
    return;
  }

  let sockets = os::network_sockets(
      detail == eviliso_detail_mode::All
          ? os::network_socket_process_mode::WithProcesses
          : os::network_socket_process_mode::WithoutProcesses);
  sockets.sort([](const os::network_socket_entry &left,
                  const os::network_socket_entry &right) {
    if (left.peer_address != right.peer_address) {
      return left.peer_address < right.peer_address;
    }
    if (left.peer_port != right.peer_port) {
      return left.peer_port < right.peer_port;
    }
    if (left.local_address != right.local_address) {
      return left.local_address < right.local_address;
    }
    if (left.local_port != right.local_port) {
      return left.local_port < right.local_port;
    }
    if (left.protocol != right.protocol) return left.protocol < right.protocol;
    if (left.state != right.state) return left.state < right.state;
    if (left.identity != right.identity) return left.identity < right.identity;
    return left.process_id < right.process_id;
  });

  let socket_identities = ArrayList<u64>{allocator};
  let remote_identities = ArrayList<u64>{allocator};
  usize zero_identity_count = 0;
  usize remote_zero_identity_count = 0;
  for (let const &socket : sockets) {
    let const is_remote = is_remote_socket(socket);
    if (socket.identity == 0) {
      zero_identity_count++;
      if (is_remote) remote_zero_identity_count++;
    } else {
      socket_identities.push(socket.identity);
      if (is_remote) remote_identities.push(socket.identity);
    }
  }
  socket_identities.sort();
  remote_identities.sort();
  let const do_count_unique = [](const ArrayList<u64> &identities) {
    usize count = 0;
    u64 previous = 0;
    for (let const identity : identities) {
      if (count == 0 || identity != previous) count++;
      previous = identity;
    }
    return count;
  };
  let const socket_count =
      zero_identity_count + do_count_unique(socket_identities);
  let const remote_count =
      remote_zero_identity_count + do_count_unique(remote_identities);
  table.add_column("FIELD", report_table_alignment::Left,
                   colors::ansi::BOLD_CYAN);
  table.add_column("VALUE", report_table_alignment::Right,
                   colors::ansi::BOLD_CYAN);
  let summary_cells = ArrayList<report_table_cell_view>{allocator};
  summary_cells.reserve(2);
  let const remote_socket_text = String::from(remote_count, allocator);
  summary_cells.push({"Remote sockets", colors::ansi::BOLD_CYAN});
  summary_cells.push({remote_socket_text.view(), colors::ansi::BOLD_GREEN});
  table.add_row(summary_cells);
  summary_cells.clear();
  let const total_socket_text = String::from(socket_count, allocator);
  summary_cells.push({"Total sockets", colors::ansi::BOLD_CYAN});
  summary_cells.push({total_socket_text.view(), colors::ansi::BOLD_GREEN});
  table.add_row(summary_cells);
  append_titled_report_table(output, "Socket summary", table, should_color);
  if (rows_mode == eviliso_remote_rows_mode::Hide) return;

  struct remote_peer_row
  {
    explicit remote_peer_row(Allocator allocator)
        : local(allocator), peer(allocator), socket_id(allocator),
          process_id(allocator), owner_id(allocator), user(allocator),
          name(allocator), command(allocator), net_namespace(allocator),
          orchestrator(allocator), runtime(allocator), container(allocator),
          cgroup(allocator)
    {}

    StringView family{};
    StringView protocol{};
    StringView state{};
    String local;
    String peer;
    String socket_id;
    String process_id;
    String owner_id;
    String user;
    String name;
    String command;
    String net_namespace;
    String orchestrator;
    String runtime;
    String container;
    String cgroup;
    u64 receive_queue_bytes{0};
    u64 send_queue_bytes{0};
  };

  struct remote_process_context
  {
    explicit remote_process_context(Allocator allocator)
        : name(allocator), command(allocator), net_namespace(allocator),
          orchestrator(allocator), runtime(allocator), container(allocator),
          cgroups(allocator)
    {}

    i64 process_id{0};
    u64 start_token{0};
    u32 owner_id{0};
    String name;
    String command;
    String net_namespace;
    String orchestrator;
    String runtime;
    String container;
    String cgroups;
    bool is_available{false};
  };

  struct remote_user_context
  {
    explicit remote_user_context(Allocator allocator) : name(allocator) {}

    u32 owner_id{0};
    String name;
  };

  let process_contexts = ArrayList<remote_process_context>{allocator};
  let user_contexts = ArrayList<remote_user_context>{allocator};
  let const self_net_namespace = os::read_symlink(
      eviliso_namespace_proc_path("self/ns/net", allocator), allocator);
  let const do_get_user = [&](u32 process_id, u32 owner_id) throws -> String {
    for (let const &context : user_contexts) {
      if (context.owner_id == owner_id) {
        return String{allocator, context.name.view()};
      }
    }
    let name = String::from(owner_id, allocator);
    if (let const resolved =
            os::process_owner_name(process_id, owner_id, allocator);
        resolved.has_value())
    {
      name = steal(*resolved);
    }
    let context = remote_user_context{allocator};
    context.owner_id = owner_id;
    context.name = steal(name);
    user_contexts.push(steal(context));
    return String{allocator,
                  user_contexts[user_contexts.count() - 1].name.view()};
  };
  let remote_rows = ArrayList<remote_peer_row>{allocator};
  u64 previous_identity = 0;
  u32 previous_process_id = 0;
  bool has_previous_owner = false;
  for (let const &socket : sockets) {
    if (!is_remote_socket(socket)) continue;
    if (socket.identity != 0 && has_previous_owner &&
        socket.identity == previous_identity &&
        socket.process_id == previous_process_id)
    {
      continue;
    }
    previous_identity = socket.identity;
    previous_process_id = socket.process_id;
    has_previous_owner = socket.identity != 0;

    remote_peer_row row{allocator};
    row.family =
        socket.family == os::network_address_family::IPv6 ? "IPv6" : "IPv4";
    row.protocol =
        socket.protocol == os::network_socket_protocol::Udp ? "UDP" : "TCP";
    row.state = remote_state_name(socket.state);
    row.local = remote_endpoint(socket.local_address.view(), socket.local_port,
                                allocator, socket.family);
    row.peer = remote_endpoint(socket.peer_address.view(), socket.peer_port,
                               allocator, socket.family);
    row.socket_id = socket.identity == 0
                        ? String{allocator, "-"}
                        : String::from(socket.identity, allocator);
    row.receive_queue_bytes = socket.receive_queue_bytes;
    row.send_queue_bytes = socket.send_queue_bytes;
    if (detail == eviliso_detail_mode::All) {
      row.process_id = socket.process_id == 0
                           ? String{allocator, "-"}
                           : String::from(socket.process_id, allocator);
      row.owner_id = socket.has_owner_id
                         ? String::from(socket.owner_id, allocator)
                         : String{allocator, "-"};
      row.user = "-";
      row.name = "-";
      row.command = "-";
      row.net_namespace = self_net_namespace.has_value()
                              ? String{allocator, self_net_namespace->view()}
                              : String{allocator, "-"};
      row.orchestrator = "-";
      row.runtime = "-";
      row.container = "-";
      row.cgroup = "-";
      if (socket.has_owner_id) {
        row.user = do_get_user(socket.process_id, socket.owner_id);
      }

      let process_context_index = Maybe<usize>{None};
      for (usize context_index = 0; context_index < process_contexts.count();
           context_index++)
      {
        let const &context = process_contexts[context_index];
        if (context.process_id == socket.process_id &&
            context.start_token == socket.owner_start_token)
        {
          process_context_index = context_index;
          break;
        }
      }
      if (!process_context_index.has_value() && socket.process_id != 0 &&
          socket.has_owner_start_token)
      {
        remote_process_context context{allocator};
        context.process_id = socket.process_id;
        context.start_token = socket.owner_start_token;
        for (let const &process : snapshot) {
          if (process.process_id != socket.process_id) continue;
          if (process.start_token != socket.owner_start_token ||
              !process_snapshot_identity_is_valid(process.status))
            break;
          context.owner_id = process.owner_id;
          context.name = remote_table_text(process.name.view(), 48, allocator);
          context.command =
              remote_table_text(process.command.view(), 96, allocator);
          let net_namespace_suffix = String::from(socket.process_id, allocator);
          net_namespace_suffix += "/ns/net";
          if (let net_namespace =
                  os::read_symlink(eviliso_namespace_proc_path(
                                       net_namespace_suffix.view(), allocator),
                                   allocator);
              net_namespace.has_value())
          {
            context.net_namespace = steal(*net_namespace);
          }
          for (usize index = 0; index < process.memberships.count(); index++) {
            let const &membership = process.memberships[index];
            bool is_known = false;
            for (usize known_index = 0; known_index < index; known_index++) {
              if (process.memberships[known_index].path == membership.path) {
                is_known = true;
                break;
              }
            }
            if (is_known) continue;
            constexpr usize cgroup_copy_limit = 120 * 8 + 1;
            if (context.cgroups.length() >= cgroup_copy_limit) break;
            if (!context.cgroups.is_empty()) context.cgroups += ',';
            let const remaining_bytes =
                cgroup_copy_limit - context.cgroups.length();
            let const path = membership.path.view();
            context.cgroups += path.substring_of_length(
                0,
                path.length < remaining_bytes ? path.length : remaining_bytes);
          }
          for (let const &evidence : process.evidence) {
            if (context.runtime.is_empty() && evidence.runtime != "-")
              context.runtime = evidence.runtime.clone();
            if (context.container.is_empty() && evidence.container_id != "-")
              context.container = evidence.container_id.clone();
            if (context.orchestrator.is_empty() && evidence.is_kubernetes)
              context.orchestrator = "kubernetes";
          }
          if (context.cgroups.is_empty()) context.cgroups = "-";
          if (context.orchestrator.is_empty()) context.orchestrator = "-";
          if (context.runtime.is_empty()) context.runtime = "-";
          if (context.container.is_empty()) context.container = "-";
          context.cgroups =
              remote_table_text(context.cgroups.view(), 120, allocator);
          context.is_available = true;
          break;
        }
        process_contexts.push(steal(context));
        process_context_index = process_contexts.count() - 1;
      }
      if (process_context_index.has_value() &&
          process_contexts[*process_context_index].is_available)
      {
        let const &process_context = process_contexts[*process_context_index];
        if (!socket.has_owner_id) {
          row.owner_id = String::from(process_context.owner_id, allocator);
          row.user = do_get_user(socket.process_id, process_context.owner_id);
        }
        row.name = process_context.name;
        row.command = process_context.command;
        if (!process_context.net_namespace.is_empty()) {
          row.net_namespace = process_context.net_namespace;
        }
        row.cgroup = process_context.cgroups;
        row.orchestrator = process_context.orchestrator;
        row.runtime = process_context.runtime;
        row.container = process_context.container;
      }
    }
    remote_rows.push(steal(row));
  }

  let peer_table = ReportTable{allocator};
  peer_table.add_column("FAMILY", report_table_alignment::Left,
                        colors::ansi::BOLD_CYAN);
  peer_table.add_column("PROTO", report_table_alignment::Left,
                        colors::ansi::BOLD_CYAN);
  peer_table.add_column("STATE", report_table_alignment::Left,
                        colors::ansi::BOLD_CYAN);
  peer_table.add_column("RECV-Q", report_table_alignment::Right,
                        colors::ansi::BOLD_CYAN);
  peer_table.add_column("SEND-Q", report_table_alignment::Right,
                        colors::ansi::BOLD_CYAN);
  peer_table.add_column("LOCAL", report_table_alignment::Left,
                        colors::ansi::BOLD_CYAN);
  peer_table.add_column("PEER", report_table_alignment::Left,
                        colors::ansi::BOLD_CYAN);
  if (detail == eviliso_detail_mode::All) {
    peer_table.add_column("SOCKET", report_table_alignment::Right,
                          colors::ansi::BOLD_CYAN);
    peer_table.add_column("PID", report_table_alignment::Right,
                          colors::ansi::BOLD_CYAN);
    peer_table.add_column("UID", report_table_alignment::Right,
                          colors::ansi::BOLD_CYAN);
    peer_table.add_column("USER", report_table_alignment::Left,
                          colors::ansi::BOLD_CYAN);
    peer_table.add_column("NAME", report_table_alignment::Left,
                          colors::ansi::BOLD_CYAN);
    peer_table.add_column("COMMAND", report_table_alignment::Left,
                          colors::ansi::BOLD_CYAN);
    peer_table.add_column("NETNS", report_table_alignment::Left,
                          colors::ansi::BOLD_CYAN);
    peer_table.add_column("ORCHESTRATOR", report_table_alignment::Left,
                          colors::ansi::BOLD_CYAN);
    peer_table.add_column("RUNTIME", report_table_alignment::Left,
                          colors::ansi::BOLD_CYAN);
    peer_table.add_column("CONTAINER", report_table_alignment::Left,
                          colors::ansi::BOLD_CYAN);
    peer_table.add_column("CGROUP", report_table_alignment::Left,
                          colors::ansi::BOLD_CYAN);
  }
  for (let const &row : remote_rows) {
    let receive = String::from(row.receive_queue_bytes, allocator);
    let send = String::from(row.send_queue_bytes, allocator);
    let cells = ArrayList<report_table_cell_view>{allocator};
    cells.push({row.family, colors::ansi::BOLD_MAGENTA});
    cells.push({row.protocol, colors::ansi::BOLD_MAGENTA});
    cells.push({row.state, colors::ansi::BOLD_GREEN});
    cells.push({receive.view(), colors::ansi::GREEN});
    cells.push({send.view(), colors::ansi::GREEN});
    cells.push({row.local.view(), colors::ansi::BOLD_CYAN});
    cells.push({row.peer.view(), colors::ansi::CYAN});
    if (detail == eviliso_detail_mode::All) {
      cells.push({row.socket_id.view(), colors::ansi::YELLOW});
      cells.push({row.process_id.view(), colors::ansi::YELLOW});
      cells.push({row.owner_id.view(), colors::ansi::RESET});
      cells.push({row.user.view(), colors::ansi::RESET});
      cells.push({row.name.view(), colors::ansi::RESET});
      cells.push({row.command.view(), colors::ansi::RESET});
      cells.push({row.net_namespace.view(), colors::ansi::RESET});
      cells.push({row.orchestrator.view(), colors::ansi::RESET});
      cells.push({row.runtime.view(), colors::ansi::RESET});
      cells.push({row.container.view(), colors::ansi::RESET});
      cells.push({row.cgroup.view(), colors::ansi::RESET});
    }
    peer_table.add_row(cells);
  }
  append_titled_report_table(output, "Remote peers", peer_table, should_color);
}

fn append_runtime_evidence_report(
    String &output, bool should_color,
    const ArrayList<process_cgroup_snapshot> &snapshot, Allocator allocator,
    eviliso_detail_mode detail) throws -> void
{
  struct runtime_report_row
  {
    String runtime;
    String source;
    i64 process_id_value{0};
    String process_id;
    String name;
    String role;
    String evidence;

    explicit runtime_report_row(Allocator allocator)
        : runtime(allocator), source(allocator), process_id(allocator),
          name(allocator), role(allocator), evidence(allocator)
    {}
  };

  let rows = ArrayList<runtime_report_row>{allocator};
  let const self_process_id = os::get_current_process_id();
  for (let const &process : snapshot) {
    for (usize index = 0; index < process.evidence.count(); index++) {
      let const &evidence = process.evidence[index];
      if (evidence.runtime == "-") continue;
      bool is_duplicate = false;
      for (let const &row : rows) {
        if (row.runtime != evidence.runtime || row.evidence != evidence.path)
          continue;
        if (detail != eviliso_detail_mode::All ||
            row.process_id_value == process.process_id)
        {
          is_duplicate = true;
          break;
        }
      }
      if (is_duplicate) continue;

      let row = runtime_report_row{allocator};
      row.runtime = evidence.runtime.clone();
      row.source = "cgroup";
      row.process_id_value = process.process_id;
      row.process_id = String::from(process.process_id, allocator);
      row.name = process.name.clone();
      row.role = process.process_id == self_process_id ? "self" : "other";
      row.evidence = evidence.path.clone();
      rows.push(steal(row));
    }
  }

  struct marker_runtime
  {
    StringView runtime;
    StringView path;
  };
  static constexpr marker_runtime MARKERS[] = {
      {"docker", ".dockerenv"       },
      {"podman", "run/.containerenv"},
  };
  for (let const &marker : MARKERS) {
    let const path = container_marker_path(marker.path, allocator);
    if (!Path{path.view(), allocator}.is_regular_file()) continue;
    let row = runtime_report_row{allocator};
    row.runtime = marker.runtime;
    row.source = "marker";
    row.process_id = "-";
    row.name = "-";
    row.role = "host";
    row.evidence = path.view();
    rows.push(steal(row));
  }

  rows.sort(
      [](const runtime_report_row &left, const runtime_report_row &right) {
        if (left.runtime != right.runtime) return left.runtime < right.runtime;
        if (left.source != right.source) return left.source < right.source;
        if (left.evidence != right.evidence)
          return left.evidence < right.evidence;
        return left.process_id_value < right.process_id_value;
      });

  let table = ReportTable{allocator};
  table.add_column("RUNTIME", report_table_alignment::Left,
                   colors::ansi::BOLD_CYAN);
  table.add_column("SOURCE", report_table_alignment::Left,
                   colors::ansi::BOLD_CYAN);
  if (detail == eviliso_detail_mode::All) {
    table.add_column("PID", report_table_alignment::Right,
                     colors::ansi::BOLD_CYAN);
    table.add_column("NAME", report_table_alignment::Left,
                     colors::ansi::BOLD_CYAN);
    table.add_column("ROLE", report_table_alignment::Left,
                     colors::ansi::BOLD_CYAN);
  }
  table.add_column("EVIDENCE", report_table_alignment::Left,
                   colors::ansi::BOLD_CYAN);
  for (let const &row : rows) {
    let cells = ArrayList<report_table_cell_view>{allocator};
    cells.push({row.runtime.view(), colors::ansi::BOLD_GREEN});
    cells.push({row.source.view(), colors::ansi::RESET});
    if (detail == eviliso_detail_mode::All) {
      cells.push({row.process_id.view(), colors::ansi::YELLOW});
      cells.push({row.name.view(), colors::ansi::RESET});
      cells.push({row.role.view(), colors::ansi::BOLD_MAGENTA});
    }
    cells.push({row.evidence.view(), colors::ansi::RESET});
    table.add_row(cells);
  }
  append_titled_report_table(output, "Container runtimes", table, should_color);
}

fn append_container_report(String &output, bool should_color,
                           const ArrayList<process_cgroup_snapshot> &snapshot,
                           Allocator allocator,
                           eviliso_detail_mode detail) throws -> void
{
  struct container_summary_row
  {
    String runtime;
    String identifier;
    usize process_count{0};

    explicit container_summary_row(Allocator allocator)
        : runtime(allocator), identifier(allocator)
    {}
  };
  struct container_detail_row
  {
    String runtime;
    String identifier;
    i64 process_id_value{0};
    String process_id;
    String name;
    String role;
    String cgroup;

    explicit container_detail_row(Allocator allocator)
        : runtime(allocator), identifier(allocator), process_id(allocator),
          name(allocator), role(allocator), cgroup(allocator)
    {}
  };

  let summary_rows = ArrayList<container_summary_row>{allocator};
  let detail_rows = ArrayList<container_detail_row>{allocator};
  let const self_process_id = os::get_current_process_id();
  for (let const &process : snapshot) {
    for (usize index = 0; index < process.evidence.count(); index++) {
      let const &evidence = process.evidence[index];
      if (evidence.container_id == "-") continue;
      bool is_process_duplicate = false;
      for (usize known_index = 0; known_index < index; known_index++) {
        let const &known = process.evidence[known_index];
        if (known.runtime == evidence.runtime &&
            known.container_id == evidence.container_id)
        {
          is_process_duplicate = true;
          break;
        }
      }
      if (!is_process_duplicate) {
        bool is_known = false;
        for (let &row : summary_rows) {
          if (row.runtime != evidence.runtime ||
              row.identifier != evidence.container_id)
            continue;
          row.process_count++;
          is_known = true;
          break;
        }
        if (!is_known) {
          let row = container_summary_row{allocator};
          row.runtime = evidence.runtime.clone();
          row.identifier = evidence.container_id.clone();
          row.process_count = 1;
          summary_rows.push(steal(row));
        }
      }

      if (detail != eviliso_detail_mode::All) continue;
      bool is_detail_duplicate = false;
      for (usize known_index = 0; known_index < index; known_index++) {
        let const &known = process.evidence[known_index];
        if (known.runtime == evidence.runtime &&
            known.container_id == evidence.container_id &&
            known.path == evidence.path)
        {
          is_detail_duplicate = true;
          break;
        }
      }
      if (is_detail_duplicate) continue;
      let row = container_detail_row{allocator};
      row.runtime = evidence.runtime.clone();
      row.identifier = evidence.container_id.clone();
      row.process_id_value = process.process_id;
      row.process_id = String::from(process.process_id, allocator);
      row.name = process.name.clone();
      row.role = process.process_id == self_process_id ? "self" : "other";
      row.cgroup = evidence.path.clone();
      detail_rows.push(steal(row));
    }
  }
  summary_rows.sort([](const container_summary_row &left,
                       const container_summary_row &right) {
    if (left.runtime != right.runtime) return left.runtime < right.runtime;
    return left.identifier < right.identifier;
  });
  detail_rows.sort(
      [](const container_detail_row &left, const container_detail_row &right) {
        if (left.runtime != right.runtime) return left.runtime < right.runtime;
        if (left.identifier != right.identifier)
          return left.identifier < right.identifier;
        if (left.process_id_value != right.process_id_value)
          return left.process_id_value < right.process_id_value;
        return left.cgroup < right.cgroup;
      });

  let table = ReportTable{allocator};
  table.add_column("RUNTIME", report_table_alignment::Left,
                   colors::ansi::BOLD_CYAN);
  table.add_column("CONTAINER", report_table_alignment::Left,
                   colors::ansi::BOLD_CYAN);
  if (detail != eviliso_detail_mode::All) {
    table.add_column("PROCESSES", report_table_alignment::Right,
                     colors::ansi::BOLD_CYAN);
    for (let const &row : summary_rows) {
      let count = String::from(row.process_count, allocator);
      let cells = ArrayList<report_table_cell_view>{allocator};
      cells.push({row.runtime.view(), colors::ansi::BOLD_GREEN});
      cells.push({row.identifier.view(), colors::ansi::CYAN});
      cells.push({count.view(), colors::ansi::YELLOW});
      table.add_row(cells);
    }
  } else {
    table.add_column("PID", report_table_alignment::Right,
                     colors::ansi::BOLD_CYAN);
    table.add_column("NAME", report_table_alignment::Left,
                     colors::ansi::BOLD_CYAN);
    table.add_column("ROLE", report_table_alignment::Left,
                     colors::ansi::BOLD_CYAN);
    table.add_column("CGROUP", report_table_alignment::Left,
                     colors::ansi::BOLD_CYAN);
    for (let const &row : detail_rows) {
      let cells = ArrayList<report_table_cell_view>{allocator};
      cells.push({row.runtime.view(), colors::ansi::BOLD_GREEN});
      cells.push({row.identifier.view(), colors::ansi::CYAN});
      cells.push({row.process_id.view(), colors::ansi::YELLOW});
      cells.push({row.name.view(), colors::ansi::RESET});
      cells.push({row.role.view(), colors::ansi::BOLD_MAGENTA});
      cells.push({row.cgroup.view(), colors::ansi::RESET});
      table.add_row(cells);
    }
  }
  append_titled_report_table(output, "Containers", table, should_color);
}

constexpr usize KUBERNETES_METADATA_BYTE_LIMIT = 256;

fn sanitize_kubernetes_metadata(StringView text, Allocator allocator) throws
    -> String
{
  let result = String{allocator};
  let const byte_count = text.length < KUBERNETES_METADATA_BYTE_LIMIT
                             ? text.length
                             : KUBERNETES_METADATA_BYTE_LIMIT;
  result.reserve(byte_count);
  for (usize index = 0; index < byte_count; index++) {
    let const byte = text[index];
    if (byte == '\r' || byte == '\n') break;
    result.push(static_cast<unsigned char>(byte) < 32 || byte == 127 ? ' '
                                                                     : byte);
  }
  return String{allocator, result.view().trim_blanks()};
}

fn read_kubernetes_metadata(StringView name, Allocator allocator) throws
    -> String
{
  let const path = kubernetes_service_account_path(name, allocator);
  let const descriptor =
      os::open_file_descriptor(path.view(), os::file_open_mode::Read);
  if (!descriptor.has_value()) return String{allocator};
  defer { unused(os::close_fd(*descriptor)); };

  char bytes[KUBERNETES_METADATA_BYTE_LIMIT]{};
  let const read_count = os::read_fd(*descriptor, bytes, sizeof(bytes));
  if (!read_count.has_value() || *read_count == 0) return String{allocator};
  return sanitize_kubernetes_metadata(StringView{bytes, *read_count},
                                      allocator);
}

fn append_kubernetes_report(String &output, bool should_color,
                            const ArrayList<process_cgroup_snapshot> &snapshot,
                            Allocator allocator,
                            eviliso_detail_mode detail) throws -> void
{
  let const self_process_id = os::get_current_process_id();
  let kubernetes_host = String{allocator};
  if (let const value = os::get_environment_variable("KUBERNETES_SERVICE_HOST");
      value.has_value())
    kubernetes_host = sanitize_kubernetes_metadata(value->view(), allocator);
  let namespace_name = String{allocator};
  namespace_name = read_kubernetes_metadata("namespace", allocator);
  bool has_kubepods = false;
  for (let const &process : snapshot) {
    for (let const &evidence : process.evidence)
      has_kubepods = has_kubepods || evidence.is_kubernetes;
  }
  let evidence_table = ReportTable{allocator};
  evidence_table.add_column("SOURCE", report_table_alignment::Left,
                            colors::ansi::BOLD_CYAN);
  evidence_table.add_column("HOST", report_table_alignment::Left,
                            colors::ansi::BOLD_CYAN);
  evidence_table.add_column("NAMESPACE", report_table_alignment::Left,
                            colors::ansi::BOLD_CYAN);
  evidence_table.add_column("EVIDENCE", report_table_alignment::Left,
                            colors::ansi::BOLD_CYAN);
  if (!kubernetes_host.is_empty()) {
    let cells = ArrayList<report_table_cell_view>{allocator};
    cells.push({"environment", colors::ansi::RESET});
    cells.push({kubernetes_host.view(), colors::ansi::BOLD_GREEN});
    cells.push(
        {namespace_name.is_empty() ? StringView{"-"} : namespace_name.view(),
         colors::ansi::RESET});
    cells.push({"KUBERNETES_SERVICE_HOST", colors::ansi::RESET});
    evidence_table.add_row(cells);
  }
  if (has_kubepods) {
    let cells = ArrayList<report_table_cell_view>{allocator};
    cells.push({"cgroup", colors::ansi::RESET});
    cells.push({"-", colors::ansi::RESET});
    cells.push(
        {namespace_name.is_empty() ? StringView{"-"} : namespace_name.view(),
         colors::ansi::RESET});
    cells.push({"kubepods component", colors::ansi::RESET});
    evidence_table.add_row(cells);
  }
  if (!namespace_name.is_empty()) {
    let cells = ArrayList<report_table_cell_view>{allocator};
    cells.push({"service-account", colors::ansi::RESET});
    cells.push({"-", colors::ansi::RESET});
    cells.push({namespace_name.view(), colors::ansi::BOLD_GREEN});
    cells.push({"namespace file", colors::ansi::RESET});
    evidence_table.add_row(cells);
  }
  append_titled_report_table(output, "Kubernetes", evidence_table,
                             should_color);

  struct kubernetes_row
  {
    String pod_uid;
    String qos;
    String runtime;
    String container_id;
    usize process_count{0};

    explicit kubernetes_row(Allocator allocator)
        : pod_uid(allocator), qos(allocator), runtime(allocator),
          container_id(allocator)
    {}
  };
  let rows = ArrayList<kubernetes_row>{allocator};
  for (let const &process : snapshot) {
    for (usize index = 0; index < process.evidence.count(); index++) {
      let const &evidence = process.evidence[index];
      if (!evidence.is_kubernetes || evidence.pod_uid == "-") continue;
      bool is_duplicate = false;
      for (usize known_index = 0; known_index < index; known_index++) {
        let const &known = process.evidence[known_index];
        if (known.pod_uid == evidence.pod_uid && known.qos == evidence.qos &&
            known.runtime == evidence.runtime &&
            known.container_id == evidence.container_id)
        {
          is_duplicate = true;
          break;
        }
      }
      if (is_duplicate) continue;
      bool is_known = false;
      for (let &row : rows) {
        if (row.pod_uid != evidence.pod_uid || row.qos != evidence.qos ||
            row.runtime != evidence.runtime ||
            row.container_id != evidence.container_id)
          continue;
        row.process_count++;
        is_known = true;
        break;
      }
      if (!is_known) {
        let row = kubernetes_row{allocator};
        row.pod_uid = evidence.pod_uid.clone();
        row.qos = evidence.qos.clone();
        row.runtime = evidence.runtime.clone();
        row.container_id = evidence.container_id.clone();
        row.process_count = 1;
        rows.push(steal(row));
      }
    }
  }
  rows.sort([](const kubernetes_row &left, const kubernetes_row &right) {
    if (left.pod_uid != right.pod_uid) return left.pod_uid < right.pod_uid;
    if (left.qos != right.qos) return left.qos < right.qos;
    if (left.runtime != right.runtime) return left.runtime < right.runtime;
    return left.container_id < right.container_id;
  });
  let workload_table = ReportTable{allocator};
  workload_table.add_column("SOURCE", report_table_alignment::Left,
                            colors::ansi::BOLD_CYAN);
  workload_table.add_column("POD UID", report_table_alignment::Left,
                            colors::ansi::BOLD_CYAN);
  workload_table.add_column("QOS", report_table_alignment::Left,
                            colors::ansi::BOLD_CYAN);
  workload_table.add_column("RUNTIME", report_table_alignment::Left,
                            colors::ansi::BOLD_CYAN);
  workload_table.add_column("CONTAINER", report_table_alignment::Left,
                            colors::ansi::BOLD_CYAN);
  if (detail != eviliso_detail_mode::All) {
    workload_table.add_column("PROCESSES", report_table_alignment::Right,
                              colors::ansi::BOLD_CYAN);
    for (let const &row : rows) {
      let count = String::from(row.process_count, allocator);
      let cells = ArrayList<report_table_cell_view>{allocator};
      cells.push({"cgroup", colors::ansi::RESET});
      cells.push({row.pod_uid.view(), colors::ansi::BOLD_GREEN});
      cells.push({row.qos.view(), colors::ansi::BOLD_MAGENTA});
      cells.push({row.runtime.view(), colors::ansi::RESET});
      cells.push({row.container_id.view(), colors::ansi::CYAN});
      cells.push({count.view(), colors::ansi::YELLOW});
      workload_table.add_row(cells);
    }
  } else {
    workload_table.add_column("PID", report_table_alignment::Right,
                              colors::ansi::BOLD_CYAN);
    workload_table.add_column("NAME", report_table_alignment::Left,
                              colors::ansi::BOLD_CYAN);
    workload_table.add_column("ROLE", report_table_alignment::Left,
                              colors::ansi::BOLD_CYAN);
    workload_table.add_column("CGROUP", report_table_alignment::Left,
                              colors::ansi::BOLD_CYAN);
    for (let const &process : snapshot) {
      for (usize index = 0; index < process.evidence.count(); index++) {
        let const &evidence = process.evidence[index];
        if (!evidence.is_kubernetes || evidence.pod_uid == "-") continue;
        bool is_duplicate = false;
        for (usize known_index = 0; known_index < index; known_index++) {
          let const &known = process.evidence[known_index];
          if (known.pod_uid == evidence.pod_uid && known.qos == evidence.qos &&
              known.runtime == evidence.runtime &&
              known.container_id == evidence.container_id &&
              known.path == evidence.path)
          {
            is_duplicate = true;
            break;
          }
        }
        if (is_duplicate) continue;
        let process_id = String::from(process.process_id, allocator);
        let cells = ArrayList<report_table_cell_view>{allocator};
        cells.push({"cgroup", colors::ansi::RESET});
        cells.push({evidence.pod_uid.view(), colors::ansi::BOLD_GREEN});
        cells.push({evidence.qos.view(), colors::ansi::BOLD_MAGENTA});
        cells.push({evidence.runtime.view(), colors::ansi::RESET});
        cells.push({evidence.container_id.view(), colors::ansi::CYAN});
        cells.push({process_id.view(), colors::ansi::YELLOW});
        cells.push({process.name.view(), colors::ansi::RESET});
        cells.push({process.process_id == self_process_id ? StringView{"self"}
                                                          : StringView{"other"},
                    colors::ansi::BOLD_MAGENTA});
        cells.push({evidence.path.view(), colors::ansi::RESET});
        workload_table.add_row(cells);
      }
    }
  }
  append_titled_report_table(output, "Kubernetes workloads", workload_table,
                             should_color);
}

} // namespace

EvilIso::EvilIso() = default;

pure fn EvilIso::kind() const wontthrow -> Utility::Kind
{
  return Kind::EvilIso;
}

fn EvilIso::execute(const ExecContext &ec, EvalContext &cxt,
                    const ArrayList<String> &args,
                    const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let const[operands, operand_locations] = parse_util_operands(
      FLAG_LIST, args, cxt.scratch_allocator(), &arg_locations);
  defer { reset_flags(FLAG_LIST); };
  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);
  if (!operands.is_empty()) {
    KOSHKIT_REPORT_ERROR_AT(operand_locations[0], "unexpected operand",
                            "pass only isolation section flags");
    return 1;
  }

  let const any_selector =
      FLAG_EVILISO_NAMESPACES.is_enabled() ||
      FLAG_EVILISO_CGROUPS.is_enabled() || FLAG_EVILISO_SESSIONS.is_enabled() ||
      FLAG_EVILISO_REMOTE.is_enabled() || FLAG_EVILISO_RUNTIME.is_enabled() ||
      FLAG_EVILISO_KUBERNETES.is_enabled() ||
      FLAG_EVILISO_CONTAINER.is_enabled() ||
      FLAG_EVILISO_CONTAINERS.is_enabled();
  let const show_namespaces =
      !any_selector || FLAG_EVILISO_NAMESPACES.is_enabled();
  let const show_cgroups = !any_selector || FLAG_EVILISO_CGROUPS.is_enabled();
  let const show_sessions = !any_selector || FLAG_EVILISO_SESSIONS.is_enabled();
  let const show_remote = !any_selector || FLAG_EVILISO_REMOTE.is_enabled();
  let const detail = FLAG_EVILISO_ALL.is_enabled()
                         ? eviliso_detail_mode::All
                         : eviliso_detail_mode::Summary;
  let const show_runtime = !any_selector || FLAG_EVILISO_RUNTIME.is_enabled();
  let const show_kubernetes =
      !any_selector || FLAG_EVILISO_KUBERNETES.is_enabled();
  let const show_container = !any_selector ||
                             FLAG_EVILISO_CONTAINER.is_enabled() ||
                             FLAG_EVILISO_CONTAINERS.is_enabled();
  let const should_color = koshkit_should_color();
  let output = String{cxt.scratch_allocator()};
  let process_cgroups =
      ArrayList<process_cgroup_snapshot>{cxt.scratch_allocator()};
  let const collection = show_cgroups || detail == eviliso_detail_mode::All ||
                                 show_runtime || show_kubernetes ||
                                 show_container
                             ? eviliso_collection_mode::Collect
                             : eviliso_collection_mode::Skip;
  if (show_namespaces || collection == eviliso_collection_mode::Collect) {
    process_cgroups =
        collect_process_cgroup_snapshot(cxt.scratch_allocator(), collection);
  }
  if (show_namespaces) {
    let namespace_processes =
        ArrayList<namespace_process>{cxt.scratch_allocator()};
    if (let override =
            eviliso_namespace_process_override(cxt.scratch_allocator());
        override.has_value())
    {
      namespace_processes = override.take();
    } else {
      let const self_process_id = os::get_current_process_id();
      namespace_processes.reserve(process_cgroups.count());
      for (let const &process : process_cgroups) {
        if (!process_snapshot_identity_is_valid(process.status)) continue;
        namespace_processes.push(namespace_process{
            process.process_id, process.name.view(),
            process.process_id == self_process_id, cxt.scratch_allocator()});
      }
    }
    append_namespace_report(output, should_color, steal(namespace_processes),
                            detail);
  }
  if (show_cgroups)
    append_cgroup_report(output, should_color, process_cgroups, detail);
  if (show_sessions)
    append_session_report(output, should_color, cxt.scratch_allocator(),
                          detail);
  if (show_remote)
    append_remote_report(output, should_color, process_cgroups,
                         cxt.scratch_allocator(),
                         eviliso_remote_rows_mode::Show, detail);
  if (show_runtime)
    append_runtime_evidence_report(output, should_color, process_cgroups,
                                   output.allocator(), detail);
  if (show_container)
    append_container_report(output, should_color, process_cgroups,
                            output.allocator(), detail);
  if (show_kubernetes)
    append_kubernetes_report(output, should_color, process_cgroups,
                             output.allocator(), detail);
  ec.print_to_stdout(output);
  return 0;
}

} // namespace koshka::koshkit
