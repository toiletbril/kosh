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
#include "../Path.hpp"
#include "../Platform.hpp"
#include "../Toiletline.hpp"
#include "../Utils.hpp"

#include <cstdlib>

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL(
    "[-a] [-n] [-c] [-s] [-r] [-k] [--kubernetes] [--container] [--containers]");

HELP_DESCRIPTION_DECL(
    "The eviliso utility reports namespaces, cgroups, sessions, and remote "
    "connections.");

FLAG(EVILISO_ALL, Bool, 'a', "all",
     "Include detailed rows for every selected section.");
FLAG(EVILISO_NAMESPACES, Bool, 'n', "namespaces", "Report process namespaces.");
FLAG(EVILISO_CGROUPS, Bool, 'c', "cgroups", "Report cgroup membership.");
FLAG(EVILISO_SESSIONS, Bool, 's', "sessions", "Report login sessions.");
FLAG(EVILISO_REMOTE, Bool, 'r', "remote",
     "List remote peers and their owning processes.");
FLAG(EVILISO_RUNTIME, Bool, 'k', "runtime",
     "Report container and Kubernetes runtime evidence.");
FLAG(EVILISO_KUBERNETES, Bool, '\0', "kubernetes",
     "Report Kubernetes runtime evidence.");
FLAG(EVILISO_CONTAINER, Bool, '\0', "container",
     "Report container runtime evidence.");
FLAG(EVILISO_CONTAINERS, Bool, '\0', "containers",
     "Report container runtime evidence.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(EvilIso);

namespace koshka::koshkit {

namespace {

struct namespace_process
{
  i64 process_id{0};
  String name{heap_allocator()};
  bool is_self{false};
};

struct namespace_relation
{
  StringView type;
  usize type_index{0};
  String identifier{heap_allocator()};
  u64 identifier_value{0};
  i64 process_id{0};
  String name{heap_allocator()};
  StringView role;
  bool is_identifier_numeric{false};
  bool is_available{false};
};

fn eviliso_namespace_processes() throws -> ArrayList<namespace_process>
{
#ifndef NDEBUG
  if (let const *path = std::getenv("KOSH_TEST_EVILISO_NAMESPACE_PROCESSES");
      path != nullptr && path[0] != '\0')
  {
    let processes = ArrayList<namespace_process>{heap_allocator()};
    let const contents = Path{path}.read_entire_file();
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
      processes.push({
          process_id.value(),
          String{heap_allocator(), name},
          role == "self",
      });
    }
    return processes;
  }
#endif
  let processes = ArrayList<namespace_process>{heap_allocator()};
  let const self_process_id = os::get_current_process_id();
  for (let const &process : os::enumerate_processes()) {
    processes.push({
        process.pid,
        process.name.is_empty() ? String{heap_allocator(), "-"}
                                : String{heap_allocator(), process.name.view()},
        process.pid == self_process_id,
    });
  }
  return processes;
}

fn eviliso_namespace_proc_path(StringView suffix,
                               Allocator allocator) throws -> String
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

fn namespace_identifier(StringView target, Allocator allocator) throws
    -> String
{
  let const open = target.find_character('[');
  if (!open.has_value()) return String{allocator, target};
  let const after_open = target.substring(*open + 1);
  let const close = after_open.find_character(']');
  if (!close.has_value()) return String{allocator, target};
  return String{allocator, after_open.substring_of_length(0, *close)};
}

fn append_namespace_report(String &output, bool should_color,
                           bool should_show_detail) throws -> void
{
  constexpr StringView names[] = {"cgroup", "ipc",  "mnt",  "net",
                                  "pid",    "time", "user", "uts"};
  constexpr usize NAME_COUNT = sizeof(names) / sizeof(*names);
  let processes = eviliso_namespace_processes();
  if (!Path{eviliso_namespace_proc_path("self/ns", heap_allocator())}
           .is_directory())
  {
    processes.clear();
    processes.push({
        os::get_current_process_id(),
        String{heap_allocator(), "-"},
        true,
    });
  }
  let relations = ArrayList<namespace_relation>{heap_allocator()};
  relations.reserve(processes.count() * NAME_COUNT);
  for (usize type_index = 0; type_index < NAME_COUNT; type_index++) {
    let const type = names[type_index];
    let const self_target = os::read_symlink(
        eviliso_namespace_proc_path(String{"self/ns/"} + type,
                                    heap_allocator()),
        heap_allocator());
    for (let const &process : processes) {
      let target = Maybe<String>{};
      if (process.is_self) {
        if (self_target.has_value()) {
          target = String{heap_allocator(), self_target->view()};
        }
      } else {
        target = os::read_symlink(
            eviliso_namespace_proc_path(
                String::from(process.process_id, heap_allocator()) + "/ns/" +
                    type,
                heap_allocator()),
            heap_allocator());
      }

      let identifier = target.has_value()
                           ? namespace_identifier(target->view(),
                                                  heap_allocator())
                           : String{heap_allocator(), "unavailable"};
      let const identifier_value = identifier.view().to<u64>();
      relations.push({
          type,
          type_index,
          steal(identifier),
          identifier_value.is_error() ? 0 : identifier_value.value(),
          process.process_id,
          String{heap_allocator(), process.name.view()},
          process.is_self ? StringView{"self"} : StringView{"other"},
          !identifier_value.is_error(),
          target.has_value(),
      });
    }
  }

  relations.sort([](const namespace_relation &left,
                    const namespace_relation &right) {
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

  let table = ReportTable{heap_allocator()};
  table.add_column("TYPE", report_table_alignment::Left,
                   colors::ansi::BOLD_CYAN);
  table.add_column("ID", report_table_alignment::Right,
                   colors::ansi::BOLD_CYAN);
  if (should_show_detail) {
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

  if (should_show_detail) {
    for (let const &relation : relations) {
      let process_id =
          String::from(relation.process_id, heap_allocator());
      let cells = ArrayList<report_table_cell_view>{heap_allocator()};
      cells.push({relation.type, colors::ansi::BOLD_MAGENTA});
      cells.push({relation.identifier.view(), colors::ansi::RESET});
      cells.push({process_id.view(), colors::ansi::BOLD_GREEN});
      cells.push({relation.name.view(), colors::ansi::RESET});
      cells.push({relation.role, colors::ansi::BOLD_MAGENTA});
      table.add_row(cells);
    }
    output += table.to_string(should_color, "").view();
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
    let count =
        String::from(group_end - relation_index, heap_allocator());
    let cells = ArrayList<report_table_cell_view>{heap_allocator()};
    cells.push({first.type, colors::ansi::BOLD_MAGENTA});
    cells.push({first.identifier.view(), colors::ansi::RESET});
    cells.push({count.view(), colors::ansi::BOLD_GREEN});
    table.add_row(cells);
    relation_index = group_end;
  }
  output += table.to_string(should_color, "").view();
}

struct cgroup_membership
{
  String hierarchy{heap_allocator()};
  u64 hierarchy_value{0};
  String controller{heap_allocator()};
  String path{heap_allocator()};
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
  if (let const *root =
          std::getenv("KOSH_TEST_EVILISO_SERVICE_ACCOUNT_ROOT");
      root != nullptr && root[0] != '\0')
  {
    let path = String{allocator, root};
    path += '/';
    path += name;
    return path;
  }
#endif
  let path = String{allocator,
                    "/var/run/secrets/kubernetes.io/serviceaccount/"};
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
    let const controller =
        remainder.substring_of_length(0, *second_separator);
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
    memberships.push({
        String{allocator, hierarchy},
        hierarchy_value.value(),
        String{allocator, controller_name},
        String{allocator, path},
    });
  }
  return memberships;
}

struct process_cgroup_snapshot
{
  i64 process_id{0};
  u64 start_token{0};
  String name{heap_allocator()};
  ArrayList<cgroup_membership> memberships{heap_allocator()};
  struct identity_evidence
  {
    String runtime{heap_allocator()};
    String container_id{heap_allocator()};
    String pod_uid{heap_allocator()};
    String qos{heap_allocator()};
    bool is_kubernetes{false};
    String path{heap_allocator()};
  };
  ArrayList<identity_evidence> evidence{heap_allocator()};
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
  let const identifier =
      component.substring_of_length(prefix.length, 64);
  return is_hexadecimal_id(identifier) ? Maybe<StringView>{identifier} : None;
}

pure fn normalized_pod_uid(StringView component, Allocator allocator) throws
    -> String
{
  usize start = 0;
  if (component.starts_with("pod")) {
    start = 3;
  } else if (let const marker = component.find_substring("-pod");
             marker.has_value())
  {
    start = *marker + 4;
  } else {
    return String{allocator, "-"};
  }
  let end = component.length;
  if (has_suffix(component, ".slice")) end -= 6;
  if (end - start != 36) return String{allocator, "-"};
  let result = String{allocator};
  result.reserve(36);
  for (usize index = start; index < end; index++) {
    let byte = component[index];
    if (byte == '_') byte = '-';
    let const uid_index = index - start;
    let const should_be_separator = uid_index == 8 || uid_index == 13 ||
                                     uid_index == 18 || uid_index == 23;
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
  let result = process_cgroup_snapshot::identity_evidence{
      String{allocator, "-"}, String{allocator, "-"},
      String{allocator, "-"}, String{allocator, "-"},
      false, String{allocator, path}};
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
    remaining = separator.has_value()
                    ? remaining.substring(*separator + 1)
                    : StringView{};

    if (component == "docker" || component == "containerd" ||
        component == "crio" || component == "libpod")
    {
      result.runtime =
          component == "crio"
              ? String{allocator, "cri-o"}
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
      let const identifier =
          scoped_container_id(component, candidate.prefix);
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
    if (pod_uid != "-") result.pod_uid = steal(pod_uid);
    if (component == "burstable" ||
        component.find_substring("-burstable-").has_value() ||
        component == "kubepods-burstable.slice")
      result.qos = "burstable";
    if (component == "besteffort" ||
        component.find_substring("-besteffort-").has_value() ||
        component == "kubepods-besteffort.slice")
      result.qos = "besteffort";
    if (component == "kubepods" || component.starts_with("kubepods-")) {
      result.is_kubernetes = true;
      has_kubernetes_component = true;
    }
    previous = component;
  }
  if (result.pod_uid != "-" && result.qos == "-") result.qos = "guaranteed";
  return result;
}

fn collect_process_cgroup_snapshot(Allocator allocator) throws
    -> ArrayList<process_cgroup_snapshot>
{
  let candidates =
      os::enumerate_processes(os::process_detail::ResourceStats);
  let pending = ArrayList<process_cgroup_snapshot>{allocator};
  for (let const &process : candidates) {
    let suffix = String::from(process.pid, allocator);
    suffix += "/cgroup";
    let const contents =
        Path{cgroup_proc_path(suffix.view(), allocator)}.read_entire_file();
    if (!contents.has_value()) continue;
    let memberships =
        parse_cgroup_memberships(contents->view(), allocator);
    if (memberships.is_empty()) continue;
    let evidence =
        ArrayList<process_cgroup_snapshot::identity_evidence>{allocator};
    evidence.reserve(memberships.count());
    for (let const &membership : memberships) {
      evidence.push(parse_cgroup_identity(membership.path.view(), allocator));
    }
    pending.push({
        process.pid,
        process.start_token,
        process.name.is_empty() ? String{allocator, "-"}
                                : String{allocator, process.name.view()},
        steal(memberships),
        steal(evidence),
    });
  }

  let current = os::enumerate_processes(os::process_detail::ResourceStats);
  let snapshot = ArrayList<process_cgroup_snapshot>{allocator};
  for (let &candidate : pending) {
    for (let const &process : current) {
      if (process.pid != candidate.process_id || process.start_token == 0 ||
          candidate.start_token == 0 ||
          process.start_token != candidate.start_token)
      {
        continue;
      }
      candidate.name = process.name.is_empty()
                           ? String{allocator, "-"}
                           : String{allocator, process.name.view()};
      snapshot.push(steal(candidate));
      break;
    }
  }
  return snapshot;
}

struct cgroup_report_row
{
  String hierarchy{heap_allocator()};
  u64 hierarchy_value{0};
  String controller{heap_allocator()};
  String path{heap_allocator()};
  String process_id{heap_allocator()};
  i64 process_id_value{0};
  u64 process_start_token{0};
  String name{heap_allocator()};
  StringView role;
};

fn append_cgroup_report(String &output, bool should_color,
                        bool should_show_detail,
                        const ArrayList<process_cgroup_snapshot> &snapshot)
    throws -> void
{
  let table = ReportTable{heap_allocator()};
  let self_index = Maybe<usize>{};
  let const self_process_id = os::get_current_process_id();
  for (usize index = 0; index < snapshot.count(); index++) {
    if (snapshot[index].process_id == self_process_id) {
      self_index = index;
      break;
    }
  }
  if (!self_index.has_value()) {
    table.add("Membership", "unavailable", colors::ansi::BOLD_CYAN);
    output += table.to_string(should_color, "");
    return;
  }

  let const &self = snapshot[*self_index];
  let rows = ArrayList<cgroup_report_row>{heap_allocator()};
  for (let const &membership : self.memberships) {
    rows.push({
        String{heap_allocator(), membership.hierarchy.view()},
        membership.hierarchy_value,
        String{heap_allocator(), membership.controller.view()},
        String{heap_allocator(), membership.path.view()},
        String::from(self_process_id, heap_allocator()),
        self_process_id,
        self.start_token,
        String{heap_allocator(), self.name.view()},
        "self",
    });
  }

  if (should_show_detail) {
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
          rows.push({
              String{heap_allocator(), membership.hierarchy.view()},
              membership.hierarchy_value,
              String{heap_allocator(), membership.controller.view()},
              String{heap_allocator(), membership.path.view()},
              String::from(process.process_id, heap_allocator()),
              process.process_id,
              process.start_token,
              String{heap_allocator(), process.name.view()},
              "other",
          });
          break;
        }
      }
    }
  }

  rows.sort([](const cgroup_report_row &left,
               const cgroup_report_row &right) {
    if (left.hierarchy_value != right.hierarchy_value)
      return left.hierarchy_value < right.hierarchy_value;
    if (left.controller != right.controller)
      return left.controller < right.controller;
    if (left.path != right.path) return left.path < right.path;
    return left.process_id_value < right.process_id_value;
  });

  let report = ReportTable{heap_allocator()};
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
  for (let const &row : rows) {
    let cells = ArrayList<report_table_cell_view>{heap_allocator()};
    cells.push({row.hierarchy.view(), colors::ansi::BOLD_GREEN});
    cells.push({row.controller.view(), colors::ansi::RESET});
    cells.push({row.path.view(), colors::ansi::RESET});
    cells.push({row.process_id.view(), colors::ansi::BOLD_GREEN});
    cells.push({row.name.view(), colors::ansi::RESET});
    cells.push({row.role, colors::ansi::BOLD_MAGENTA});
    report.add_row(cells);
  }
  output += report.to_string(should_color, "");
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
      let const terminal =
          remainder.substring_of_length(0, *terminal_end);
      let const login_time = remainder.substring(*terminal_end + 1).to<i64>();
      if (user.is_empty() || terminal.is_empty() || login_time.is_error())
        continue;
      sessions.push({
          String{heap_allocator(), user},
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
  String user{heap_allocator()};
  String terminal{heap_allocator()};
  String login_time{heap_allocator()};
};

fn append_session_report(String &output, bool should_color,
                         bool should_show_detail) throws -> void
{
  let sessions = eviliso_sessions();
  sessions.sort([](const os::user_session &left,
                   const os::user_session &right) {
    if (left.user != right.user) return left.user < right.user;
    if (left.terminal != right.terminal) return left.terminal < right.terminal;
    return left.login_time < right.login_time;
  });

  let rows = ArrayList<session_report_row>{heap_allocator()};
  usize user_width = 4;
  usize terminal_width = 8;
  usize login_time_width = 10;
  for (let const &session : sessions) {
    let login_time = String{heap_allocator()};
    if (should_show_detail) {
      login_time = session.login_time == 0
                       ? String{heap_allocator(), "unavailable"}
                       : utils::format_unix_timestamp(
                             session.login_time, "%Y-%m-%d %H:%M:%S");
    }
    rows.push({
        String{heap_allocator(), session.user.view()},
        String{heap_allocator(), session.terminal.view()},
        steal(login_time),
    });
    let const &row = rows[rows.count() - 1];
    if (row.user.length() > user_width) user_width = row.user.length();
    if (row.terminal.length() > terminal_width)
      terminal_width = row.terminal.length();
    if (row.login_time.length() > login_time_width)
      login_time_width = row.login_time.length();
  }

  append_report_column(output, "USER", user_width, false,
                       colors::ansi::BOLD_CYAN, should_color);
  output += "  ";
  append_report_column(output, "TERMINAL", terminal_width, false,
                       colors::ansi::BOLD_CYAN, should_color);
  if (should_show_detail) {
    output += "  ";
    append_report_column(output, "LOGIN TIME", login_time_width, false,
                         colors::ansi::BOLD_CYAN, should_color);
  }
  output += '\n';
  for (let const &row : rows) {
    append_report_column(output, row.user.view(), user_width, false,
                         colors::ansi::BOLD_GREEN, should_color);
    output += "  ";
    append_report_column(output, row.terminal.view(), terminal_width, false, {},
                         should_color);
    if (should_show_detail) {
      output += "  ";
      append_report_column(output, row.login_time.view(), login_time_width,
                           false, {}, should_color);
    }
    output += '\n';
  }
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

fn remote_endpoint(StringView address, u16 port,
                   os::network_address_family family,
                   Allocator allocator) throws -> String
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
  result.reserve(text.length);
  for (usize position = 0; position < text.length; position++) {
    let const byte = text[position];
    result.push((static_cast<unsigned char>(byte) < 32 || byte == 127) ? ' '
                                                                       : byte);
  }
  if (toiletline::display_width(result.view()) > maximum_cells) {
    usize actual_cells = 0;
    let const kept_bytes = toiletline::byte_offset_at_or_before_display_cell(
        result.view(), maximum_cells - 3, actual_cells);
    result.truncate(kept_bytes);
    result += "...";
  }
  return result;
}

fn append_remote_report(String &output, bool should_color,
                        bool should_show_rows, bool should_show_detail,
                        const ArrayList<process_cgroup_snapshot> &snapshot)
    throws -> void
{
  let table = ReportTable{heap_allocator()};
  if (!os::has_network_socket_listing()) {
    table.add("Sockets", "unavailable", colors::ansi::BOLD_CYAN);
    output += table.to_string(should_color, "");
    return;
  }

  let sockets = os::network_sockets(should_show_detail);
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

  let socket_identities = ArrayList<u64>{heap_allocator()};
  let remote_identities = ArrayList<u64>{heap_allocator()};
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
  table.add("Remote sockets",
            String::from(remote_count, heap_allocator()).view(),
            colors::ansi::BOLD_CYAN);
  table.add("Total sockets",
            String::from(socket_count, heap_allocator()).view(),
            colors::ansi::BOLD_CYAN);
  output += table.to_string(should_color, "");
  if (!should_show_rows) return;

  struct remote_peer_row
  {
    StringView family;
    StringView protocol;
    StringView state;
    String local{heap_allocator()};
    String peer{heap_allocator()};
    String socket_id{heap_allocator()};
    String process_id{heap_allocator()};
    String owner_id{heap_allocator()};
    String user{heap_allocator()};
    String name{heap_allocator()};
    String command{heap_allocator()};
    String net_namespace{heap_allocator()};
    String orchestrator{heap_allocator()};
    String runtime{heap_allocator()};
    String container{heap_allocator()};
    String cgroup{heap_allocator()};
    u64 receive_queue_bytes{0};
    u64 send_queue_bytes{0};
  };

  struct remote_process_context
  {
    i64 process_id{0};
    u64 start_token{0};
    u32 owner_id{0};
    String name{heap_allocator()};
    String command{heap_allocator()};
    String net_namespace{heap_allocator()};
    String orchestrator{heap_allocator()};
    String runtime{heap_allocator()};
    String container{heap_allocator()};
    String cgroups{heap_allocator()};
    bool is_available{false};
  };

  struct remote_user_context
  {
    u32 owner_id{0};
    String name{heap_allocator()};
  };

  let const processes =
      should_show_detail
          ? os::enumerate_processes(os::process_detail::ResourceStats)
          : ArrayList<os::process_entry>{heap_allocator()};
  let process_contexts = ArrayList<remote_process_context>{heap_allocator()};
  let user_contexts = ArrayList<remote_user_context>{heap_allocator()};
  let const self_net_namespace =
      os::read_symlink("/proc/self/ns/net", heap_allocator());
  let const do_get_user = [&](u32 process_id, u32 owner_id) throws -> String {
    for (let const &context : user_contexts) {
      if (context.owner_id == owner_id) {
        return String{heap_allocator(), context.name.view()};
      }
    }
    let name = String::from(owner_id, heap_allocator());
    if (let const resolved =
            os::process_owner_name(process_id, owner_id, heap_allocator());
        resolved.has_value())
    {
      name = steal(*resolved);
    }
    user_contexts.push(remote_user_context{owner_id, steal(name)});
    return String{heap_allocator(),
                  user_contexts[user_contexts.count() - 1].name.view()};
  };
  let remote_rows = ArrayList<remote_peer_row>{heap_allocator()};
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

    remote_peer_row row{};
    row.family =
        socket.family == os::network_address_family::IPv6 ? "IPv6" : "IPv4";
    row.protocol =
        socket.protocol == os::network_socket_protocol::Udp ? "UDP" : "TCP";
    row.state = remote_state_name(socket.state);
    row.local = remote_endpoint(socket.local_address.view(), socket.local_port,
                                socket.family, heap_allocator());
    row.peer = remote_endpoint(socket.peer_address.view(), socket.peer_port,
                               socket.family, heap_allocator());
    row.socket_id = socket.identity == 0
                        ? String{heap_allocator(), "-"}
                        : String::from(socket.identity, heap_allocator());
    row.receive_queue_bytes = socket.receive_queue_bytes;
    row.send_queue_bytes = socket.send_queue_bytes;
    if (should_show_detail) {
      row.process_id = socket.process_id == 0
                           ? String{heap_allocator(), "-"}
                           : String::from(socket.process_id, heap_allocator());
      row.owner_id = socket.has_owner_id
                         ? String::from(socket.owner_id, heap_allocator())
                         : String{heap_allocator(), "-"};
      row.user = "-";
      row.name = "-";
      row.command = "-";
      row.net_namespace =
          self_net_namespace.has_value()
              ? String{heap_allocator(), self_net_namespace->view()}
              : String{heap_allocator(), "-"};
      row.orchestrator = "-";
      row.runtime = "-";
      row.container = "-";
      row.cgroup = "-";
      if (socket.has_owner_id) {
        row.user = do_get_user(socket.process_id, socket.owner_id);
      }

      const remote_process_context *process_context = nullptr;
      for (let const &context : process_contexts) {
        if (context.process_id == socket.process_id &&
            context.start_token == socket.owner_start_token)
        {
          process_context = &context;
          break;
        }
      }
      if (process_context == nullptr && socket.process_id != 0 &&
          socket.has_owner_start_token)
      {
        remote_process_context context{};
        context.process_id = socket.process_id;
        context.start_token = socket.owner_start_token;
        for (let const &process : processes) {
          if (process.pid != socket.process_id) continue;
          if (process.start_token != socket.owner_start_token) break;
          context.owner_id = process.owner_id;
          context.name = process.name.is_empty()
                             ? String{heap_allocator(), "-"}
                             : remote_table_text(process.name.view(), 48,
                                                 heap_allocator());
          context.command = process.command_line.is_empty()
                                ? String{heap_allocator(), "-"}
                                : remote_table_text(process.command_line.view(),
                                                    96, heap_allocator());
          if (let net_namespace = os::read_symlink(
                  String{"/proc/"} +
                      String::from(socket.process_id, heap_allocator()) +
                      "/ns/net",
                  heap_allocator());
              net_namespace.has_value())
          {
            context.net_namespace = steal(*net_namespace);
          }
          for (let const &process_cgroups : snapshot) {
            if (process_cgroups.process_id != socket.process_id ||
                process_cgroups.start_token != socket.owner_start_token)
              continue;
            for (usize index = 0;
                 index < process_cgroups.memberships.count(); index++)
            {
              let const &membership = process_cgroups.memberships[index];
              bool is_known = false;
              for (usize known_index = 0; known_index < index; known_index++) {
                if (process_cgroups.memberships[known_index].path ==
                    membership.path)
                {
                  is_known = true;
                  break;
                }
              }
              if (is_known) continue;
              if (!context.cgroups.is_empty()) context.cgroups += ',';
              context.cgroups += membership.path.view();
            }
            for (let const &evidence : process_cgroups.evidence) {
              if (context.runtime.is_empty() && evidence.runtime != "-")
                context.runtime = evidence.runtime.clone();
              if (context.container.is_empty() && evidence.container_id != "-")
                context.container = evidence.container_id.clone();
              if (context.orchestrator.is_empty() && evidence.is_kubernetes)
                context.orchestrator = "kubernetes";
            }
            break;
          }
          if (context.cgroups.is_empty()) context.cgroups = "-";
          if (context.orchestrator.is_empty()) context.orchestrator = "-";
          if (context.runtime.is_empty()) context.runtime = "-";
          if (context.container.is_empty()) context.container = "-";
          context.cgroups =
              remote_table_text(context.cgroups.view(), 120, heap_allocator());
          context.is_available = true;
          break;
        }
        process_contexts.push(steal(context));
        process_context = &process_contexts[process_contexts.count() - 1];
      }
      if (process_context != nullptr && process_context->is_available) {
        if (!socket.has_owner_id) {
          row.owner_id =
              String::from(process_context->owner_id, heap_allocator());
          row.user = do_get_user(socket.process_id, process_context->owner_id);
        }
        row.name = process_context->name;
        row.command = process_context->command;
        if (!process_context->net_namespace.is_empty()) {
          row.net_namespace = process_context->net_namespace;
        }
        row.cgroup = process_context->cgroups;
        row.orchestrator = process_context->orchestrator;
        row.runtime = process_context->runtime;
        row.container = process_context->container;
      }
    }
    remote_rows.push(steal(row));
  }

  let peer_table = ReportTable{heap_allocator()};
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
  if (should_show_detail) {
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
    let receive = String::from(row.receive_queue_bytes, heap_allocator());
    let send = String::from(row.send_queue_bytes, heap_allocator());
    let cells = ArrayList<report_table_cell_view>{heap_allocator()};
    cells.push({row.family, colors::ansi::BOLD_MAGENTA});
    cells.push({row.protocol, colors::ansi::BOLD_MAGENTA});
    cells.push({row.state, colors::ansi::BOLD_GREEN});
    cells.push({receive.view(), colors::ansi::GREEN});
    cells.push({send.view(), colors::ansi::GREEN});
    cells.push({row.local.view(), colors::ansi::BOLD_CYAN});
    cells.push({row.peer.view(), colors::ansi::CYAN});
    if (should_show_detail) {
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
  output += peer_table.to_string(should_color, "").view();
}

fn append_runtime_report(String &output, bool should_color, bool show_runtime,
                         bool show_kubernetes, bool show_container,
                         bool should_show_detail,
                         const ArrayList<process_cgroup_snapshot> &snapshot)
    throws -> void
{
  let const self_process_id = os::get_current_process_id();
  if (show_runtime) {
    let table = ReportTable{heap_allocator()};
    table.add_column("RUNTIME", report_table_alignment::Left,
                     colors::ansi::BOLD_CYAN);
    table.add_column("SOURCE", report_table_alignment::Left,
                     colors::ansi::BOLD_CYAN);
    if (should_show_detail) {
      table.add_column("PID", report_table_alignment::Right,
                       colors::ansi::BOLD_CYAN);
      table.add_column("NAME", report_table_alignment::Left,
                       colors::ansi::BOLD_CYAN);
      table.add_column("ROLE", report_table_alignment::Left,
                       colors::ansi::BOLD_CYAN);
    }
    table.add_column("EVIDENCE", report_table_alignment::Left,
                     colors::ansi::BOLD_CYAN);
    struct runtime_key
    {
      String runtime{heap_allocator()};
      String evidence{heap_allocator()};
    };
    let summary_keys = ArrayList<runtime_key>{heap_allocator()};
    usize row_count = 0;
    for (let const &process : snapshot) {
      for (usize index = 0; index < process.evidence.count(); index++) {
        let const &evidence = process.evidence[index];
        if (evidence.runtime == "-") continue;
        bool is_duplicate = false;
        for (usize known_index = 0; known_index < index; known_index++) {
          let const &known = process.evidence[known_index];
          if (known.runtime == evidence.runtime && known.path == evidence.path) {
            is_duplicate = true;
            break;
          }
        }
        if (is_duplicate) continue;
        if (!should_show_detail) {
          bool is_known = false;
          for (let const &key : summary_keys) {
            if (key.runtime == evidence.runtime &&
                key.evidence == evidence.path)
            {
              is_known = true;
              break;
            }
          }
          if (is_known) continue;
          summary_keys.push({evidence.runtime.clone(), evidence.path.clone()});
        }
        let process_id = String::from(process.process_id, heap_allocator());
        let cells = ArrayList<report_table_cell_view>{heap_allocator()};
        cells.push({evidence.runtime.view(), colors::ansi::BOLD_GREEN});
        cells.push({"cgroup", colors::ansi::RESET});
        if (should_show_detail) {
          cells.push({process_id.view(), colors::ansi::YELLOW});
          cells.push({process.name.view(), colors::ansi::RESET});
          cells.push({process.process_id == self_process_id ? StringView{"self"}
                                                           : StringView{"other"},
                      colors::ansi::BOLD_MAGENTA});
        }
        cells.push({evidence.path.view(), colors::ansi::RESET});
        table.add_row(cells);
        row_count++;
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
      let const path = container_marker_path(marker.path, heap_allocator());
      if (!Path{path.view()}.is_regular_file()) continue;
      let cells = ArrayList<report_table_cell_view>{heap_allocator()};
      cells.push({marker.runtime, colors::ansi::BOLD_GREEN});
      cells.push({"marker", colors::ansi::RESET});
      if (should_show_detail) {
        cells.push({"-", colors::ansi::RESET});
        cells.push({"-", colors::ansi::RESET});
        cells.push({"host", colors::ansi::BOLD_MAGENTA});
      }
      cells.push({path.view(), colors::ansi::RESET});
      table.add_row(cells);
      row_count++;
    }
    if (row_count == 0) {
      let cells = ArrayList<report_table_cell_view>{heap_allocator()};
      cells.push({"-", colors::ansi::RESET});
      cells.push({"detection", colors::ansi::RESET});
      if (should_show_detail) {
        cells.push({"-", colors::ansi::RESET});
        cells.push({"-", colors::ansi::RESET});
        cells.push({"host", colors::ansi::BOLD_MAGENTA});
      }
      cells.push({"None detected", colors::ansi::BOLD_YELLOW});
      table.add_row(cells);
    }
    output += table.to_string(should_color, "").view();
  }

  if (show_container) {
    if (!output.is_empty()) output += '\n';
    struct container_row
    {
      String runtime{heap_allocator()};
      String identifier{heap_allocator()};
      usize process_count{0};
    };
    let rows = ArrayList<container_row>{heap_allocator()};
    for (let const &process : snapshot) {
      for (usize index = 0; index < process.evidence.count(); index++) {
        let const &evidence = process.evidence[index];
        if (evidence.container_id == "-") continue;
        bool is_duplicate = false;
        for (usize known_index = 0; known_index < index; known_index++) {
          let const &known = process.evidence[known_index];
          if (known.runtime == evidence.runtime &&
              known.container_id == evidence.container_id)
          {
            is_duplicate = true;
            break;
          }
        }
        if (is_duplicate) continue;
        bool is_known = false;
        for (let &row : rows) {
          if (row.runtime != evidence.runtime ||
              row.identifier != evidence.container_id)
            continue;
          row.process_count++;
          is_known = true;
          break;
        }
        if (!is_known) {
          rows.push({evidence.runtime.clone(), evidence.container_id.clone(),
                     1});
        }
      }
    }
    rows.sort([](const container_row &left, const container_row &right) {
      if (left.runtime != right.runtime) return left.runtime < right.runtime;
      return left.identifier < right.identifier;
    });
    let table = ReportTable{heap_allocator()};
    table.add_column("RUNTIME", report_table_alignment::Left,
                     colors::ansi::BOLD_CYAN);
    table.add_column("CONTAINER", report_table_alignment::Left,
                     colors::ansi::BOLD_CYAN);
    if (!should_show_detail) {
      table.add_column("PROCESSES", report_table_alignment::Right,
                       colors::ansi::BOLD_CYAN);
      for (let const &row : rows) {
        let count = String::from(row.process_count, heap_allocator());
        let cells = ArrayList<report_table_cell_view>{heap_allocator()};
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
      for (let const &process : snapshot) {
        for (usize index = 0; index < process.evidence.count(); index++) {
          let const &evidence = process.evidence[index];
          if (evidence.container_id == "-") continue;
          bool is_duplicate = false;
          for (usize known_index = 0; known_index < index; known_index++) {
            let const &known = process.evidence[known_index];
            if (known.runtime == evidence.runtime &&
                known.container_id == evidence.container_id &&
                known.path == evidence.path)
            {
              is_duplicate = true;
              break;
            }
          }
          if (is_duplicate) continue;
          let process_id =
              String::from(process.process_id, heap_allocator());
          let cells = ArrayList<report_table_cell_view>{heap_allocator()};
          cells.push({evidence.runtime.view(), colors::ansi::BOLD_GREEN});
          cells.push({evidence.container_id.view(), colors::ansi::CYAN});
          cells.push({process_id.view(), colors::ansi::YELLOW});
          cells.push({process.name.view(), colors::ansi::RESET});
          cells.push({process.process_id == self_process_id ? StringView{"self"}
                                                           : StringView{"other"},
                      colors::ansi::BOLD_MAGENTA});
          cells.push({evidence.path.view(), colors::ansi::RESET});
          table.add_row(cells);
        }
      }
    }
    if (rows.is_empty()) {
      let cells = ArrayList<report_table_cell_view>{heap_allocator()};
      cells.push({"-", colors::ansi::RESET});
      cells.push({"-", colors::ansi::RESET});
      if (!should_show_detail) {
        cells.push({"0", colors::ansi::YELLOW});
      } else {
        cells.push({"-", colors::ansi::RESET});
        cells.push({"-", colors::ansi::RESET});
        cells.push({"host", colors::ansi::BOLD_MAGENTA});
        cells.push({"None detected", colors::ansi::BOLD_YELLOW});
      }
      table.add_row(cells);
    }
    output += table.to_string(should_color, "").view();
  }

  if (!show_kubernetes) return;
  if (!output.is_empty()) output += '\n';
  let const kubernetes =
      os::get_environment_variable("KUBERNETES_SERVICE_HOST");
  let const namespace_file =
      Path{kubernetes_service_account_path("namespace", heap_allocator())}
          .read_entire_file();
  let namespace_name = String{heap_allocator()};
  if (namespace_file.has_value())
    namespace_name =
        String{heap_allocator(), namespace_file->view().trim_blanks()};
  bool has_kubepods = false;
  for (let const &process : snapshot) {
    for (let const &evidence : process.evidence)
      has_kubepods = has_kubepods || evidence.is_kubernetes;
  }
  let evidence_table = ReportTable{heap_allocator()};
  evidence_table.add_column("SOURCE", report_table_alignment::Left,
                            colors::ansi::BOLD_CYAN);
  evidence_table.add_column("HOST", report_table_alignment::Left,
                            colors::ansi::BOLD_CYAN);
  evidence_table.add_column("NAMESPACE", report_table_alignment::Left,
                            colors::ansi::BOLD_CYAN);
  evidence_table.add_column("EVIDENCE", report_table_alignment::Left,
                            colors::ansi::BOLD_CYAN);
  if (kubernetes.has_value()) {
    let cells = ArrayList<report_table_cell_view>{heap_allocator()};
    cells.push({"environment", colors::ansi::RESET});
    cells.push({kubernetes->view(), colors::ansi::BOLD_GREEN});
    cells.push({namespace_name.is_empty() ? StringView{"-"}
                                         : namespace_name.view(),
                colors::ansi::RESET});
    cells.push({"KUBERNETES_SERVICE_HOST", colors::ansi::RESET});
    evidence_table.add_row(cells);
  }
  if (has_kubepods) {
    let cells = ArrayList<report_table_cell_view>{heap_allocator()};
    cells.push({"cgroup", colors::ansi::RESET});
    cells.push({"-", colors::ansi::RESET});
    cells.push({namespace_name.is_empty() ? StringView{"-"}
                                         : namespace_name.view(),
                colors::ansi::RESET});
    cells.push({"kubepods component", colors::ansi::RESET});
    evidence_table.add_row(cells);
  }
  if (!namespace_name.is_empty()) {
    let cells = ArrayList<report_table_cell_view>{heap_allocator()};
    cells.push({"service-account", colors::ansi::RESET});
    cells.push({"-", colors::ansi::RESET});
    cells.push({namespace_name.view(), colors::ansi::BOLD_GREEN});
    cells.push({"namespace file", colors::ansi::RESET});
    evidence_table.add_row(cells);
  }
  if (!kubernetes.has_value() && !has_kubepods && namespace_name.is_empty()) {
    let cells = ArrayList<report_table_cell_view>{heap_allocator()};
    cells.push({"detection", colors::ansi::RESET});
    cells.push({"-", colors::ansi::RESET});
    cells.push({"-", colors::ansi::RESET});
    cells.push({"Not detected", colors::ansi::BOLD_YELLOW});
    evidence_table.add_row(cells);
  }
  output += evidence_table.to_string(should_color, "").view();

  struct kubernetes_row
  {
    String pod_uid{heap_allocator()};
    String qos{heap_allocator()};
    String runtime{heap_allocator()};
    String container_id{heap_allocator()};
    usize process_count{0};
  };
  let rows = ArrayList<kubernetes_row>{heap_allocator()};
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
        rows.push({evidence.pod_uid.clone(), evidence.qos.clone(),
                   evidence.runtime.clone(), evidence.container_id.clone(), 1});
      }
    }
  }
  if (rows.is_empty()) return;
  rows.sort([](const kubernetes_row &left, const kubernetes_row &right) {
    if (left.pod_uid != right.pod_uid) return left.pod_uid < right.pod_uid;
    if (left.qos != right.qos) return left.qos < right.qos;
    if (left.runtime != right.runtime) return left.runtime < right.runtime;
    return left.container_id < right.container_id;
  });
  output += '\n';
  let workload_table = ReportTable{heap_allocator()};
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
  if (!should_show_detail) {
    workload_table.add_column("PROCESSES", report_table_alignment::Right,
                              colors::ansi::BOLD_CYAN);
    for (let const &row : rows) {
      let count = String::from(row.process_count, heap_allocator());
      let cells = ArrayList<report_table_cell_view>{heap_allocator()};
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
        let process_id = String::from(process.process_id, heap_allocator());
        let cells = ArrayList<report_table_cell_view>{heap_allocator()};
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
  output += workload_table.to_string(should_color, "").view();
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
      FLAG_EVILISO_KUBERNETES.is_enabled() || FLAG_EVILISO_CONTAINER.is_enabled() ||
      FLAG_EVILISO_CONTAINERS.is_enabled();
  let const show_namespaces =
      !any_selector || FLAG_EVILISO_NAMESPACES.is_enabled();
  let const show_cgroups = !any_selector || FLAG_EVILISO_CGROUPS.is_enabled();
  let const show_sessions = !any_selector || FLAG_EVILISO_SESSIONS.is_enabled();
  let const show_remote = !any_selector || FLAG_EVILISO_REMOTE.is_enabled();
  let const show_runtime =
      !any_selector || FLAG_EVILISO_RUNTIME.is_enabled();
  let const show_kubernetes =
      !any_selector || FLAG_EVILISO_KUBERNETES.is_enabled();
  let const show_container =
      !any_selector || FLAG_EVILISO_CONTAINER.is_enabled() ||
      FLAG_EVILISO_CONTAINERS.is_enabled();
  let const should_color = koshkit_should_color();
  let output = String{cxt.scratch_allocator()};
  let process_cgroups =
      ArrayList<process_cgroup_snapshot>{cxt.scratch_allocator()};
  if (show_cgroups || show_remote || show_runtime || show_kubernetes ||
      show_container)
  {
    process_cgroups =
        collect_process_cgroup_snapshot(cxt.scratch_allocator());
  }
  if (show_namespaces)
    append_namespace_report(output, should_color,
                            FLAG_EVILISO_ALL.is_enabled());
  if (show_cgroups)
    append_cgroup_report(output, should_color, FLAG_EVILISO_ALL.is_enabled(),
                         process_cgroups);
  if (show_sessions)
    append_session_report(output, should_color, FLAG_EVILISO_ALL.is_enabled());
  if (show_remote)
    append_remote_report(
        output, should_color,
        FLAG_EVILISO_REMOTE.is_enabled() || FLAG_EVILISO_ALL.is_enabled(),
        FLAG_EVILISO_REMOTE.is_enabled() || FLAG_EVILISO_ALL.is_enabled(),
        process_cgroups);
  if (show_runtime || show_kubernetes || show_container)
    append_runtime_report(output, should_color, show_runtime, show_kubernetes,
                          show_container, FLAG_EVILISO_ALL.is_enabled(),
                          process_cgroups);
  ec.print_to_stdout(output);
  return 0;
}

} // namespace koshka::koshkit
