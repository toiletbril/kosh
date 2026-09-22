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

  usize type_width = 4;
  usize identifier_width = 2;
  usize process_width = 3;
  usize name_width = 4;
  for (let const &relation : relations) {
    if (relation.type.length > type_width) type_width = relation.type.length;
    if (relation.identifier.length() > identifier_width)
      identifier_width = relation.identifier.length();
    let const process_id =
        String::from(relation.process_id, heap_allocator());
    if (process_id.length() > process_width) process_width = process_id.length();
    if (relation.name.length() > name_width) name_width = relation.name.length();
  }

  let const do_append_column = [&](StringView text, usize width,
                                   bool is_numeric,
                                   StringView style = {}) throws {
    append_report_column(output, text, width, is_numeric, style, should_color);
    output += "  ";
  };
  do_append_column("TYPE", type_width, false, colors::ansi::BOLD_CYAN);
  do_append_column("ID", identifier_width, true, colors::ansi::BOLD_CYAN);
  if (should_show_detail) {
    do_append_column("PID", process_width, true, colors::ansi::BOLD_CYAN);
    do_append_column("NAME", name_width, false, colors::ansi::BOLD_CYAN);
    append_report_text(output, "ROLE", colors::ansi::BOLD_CYAN, should_color);
  } else {
    append_report_text(output, "PROCESSES", colors::ansi::BOLD_CYAN,
                       should_color);
  }
  output += '\n';

  if (should_show_detail) {
    for (let const &relation : relations) {
      do_append_column(relation.type, type_width, false,
                       colors::ansi::BOLD_MAGENTA);
      do_append_column(relation.identifier.view(), identifier_width, true, {});
      do_append_column(String::from(relation.process_id, heap_allocator()),
                       process_width, true, colors::ansi::BOLD_GREEN);
      do_append_column(relation.name.view(), name_width, false, {});
      append_report_text(output, relation.role, colors::ansi::BOLD_MAGENTA,
                         should_color);
      output += '\n';
    }
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
    do_append_column(first.type, type_width, false,
                     colors::ansi::BOLD_MAGENTA);
    do_append_column(first.identifier.view(), identifier_width, true, {});
    append_report_text(
        output, String::from(group_end - relation_index, heap_allocator()),
        colors::ansi::BOLD_GREEN, should_color);
    output += '\n';
    relation_index = group_end;
  }
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

fn remote_process_cgroups(i64 process_id, Allocator allocator) throws -> String
{
  let suffix = String::from(process_id, allocator);
  suffix += "/cgroup";
  let const contents =
      Path{cgroup_proc_path(suffix.view(), allocator)}.read_entire_file();
  if (!contents.has_value()) return String{allocator, "-"};

  let paths = ArrayList<String>{allocator};
  for (let const line : utils::split_lines(contents->view())) {
    let const first_separator = line.find_character(':');
    if (!first_separator.has_value()) continue;
    let const remainder = line.substring(*first_separator + 1);
    let const second_separator = remainder.find_character(':');
    if (!second_separator.has_value()) continue;
    let const path = remainder.substring(*second_separator + 1);
    if (path.is_empty()) continue;
    bool is_known = false;
    for (let const &known : paths) {
      if (known.view() == path) {
        is_known = true;
        break;
      }
    }
    if (!is_known) paths.push(String{allocator, path});
  }

  if (paths.is_empty()) return String{allocator, "-"};
  let result = String{allocator};
  for (let const &path : paths) {
    if (!result.is_empty()) result += ",";
    result += path.view();
  }
  return result;
}

pure fn remote_runtime_name(StringView cgroup) wontthrow -> StringView
{
  if (cgroup.find_substring("containerd").has_value()) return "containerd";
  if (cgroup.find_substring("crio").has_value()) return "cri-o";
  if (cgroup.find_substring("docker").has_value()) return "docker";
  if (cgroup.find_substring("libpod").has_value()) return "podman";
  return "-";
}

pure fn remote_orchestrator_name(StringView cgroup) wontthrow -> StringView
{
  return cgroup.find_substring("kubepods").has_value() ? "kubernetes" : "-";
}

fn remote_container_id(StringView cgroups, Allocator allocator) throws -> String
{
  usize best_start = 0;
  usize best_length = 0;
  usize position = 0;
  while (position < cgroups.length) {
    let const byte = cgroups[position];
    let const is_hexadecimal = (byte >= '0' && byte <= '9') ||
                               (byte >= 'a' && byte <= 'f') ||
                               (byte >= 'A' && byte <= 'F');
    if (!is_hexadecimal) {
      position++;
      continue;
    }
    let const start = position;
    while (position < cgroups.length) {
      let const candidate = cgroups[position];
      if (!((candidate >= '0' && candidate <= '9') ||
            (candidate >= 'a' && candidate <= 'f') ||
            (candidate >= 'A' && candidate <= 'F')))
      {
        break;
      }
      position++;
    }
    let const length = position - start;
    let const prefix_start = start > 32 ? start - 32 : 0;
    let const prefix =
        cgroups.substring_of_length(prefix_start, start - prefix_start);
    let const has_runtime_marker =
        prefix.find_substring("docker").has_value() ||
        prefix.find_substring("containerd").has_value() ||
        prefix.find_substring("crio").has_value() ||
        prefix.find_substring("libpod").has_value();
    if (length == 64 && has_runtime_marker && length > best_length) {
      best_start = start;
      best_length = length;
    }
  }
  if (best_length == 0) return String{allocator, "-"};
  return String{allocator,
                cgroups.substring_of_length(best_start, best_length)};
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
                        bool should_show_rows, bool should_show_detail) throws
    -> void
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
          context.cgroups =
              remote_process_cgroups(socket.process_id, heap_allocator());
          context.orchestrator =
              remote_orchestrator_name(context.cgroups.view());
          context.runtime = remote_runtime_name(context.cgroups.view());
          context.container =
              remote_container_id(context.cgroups.view(), heap_allocator());
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

  usize local_width = 5;
  usize peer_width = 4;
  usize receive_width = 6;
  usize send_width = 6;
  usize process_width = 3;
  usize socket_width = 6;
  usize owner_width = 3;
  usize user_width = 4;
  usize name_width = 4;
  usize command_width = 7;
  usize namespace_width = 5;
  usize orchestrator_width = 12;
  usize runtime_width = 7;
  usize container_width = 9;
  usize cgroup_width = 6;
  for (let const &row : remote_rows) {
    if (row.local.length() > local_width) local_width = row.local.length();
    if (row.peer.length() > peer_width) peer_width = row.peer.length();
    let const receive = String::from(row.receive_queue_bytes, heap_allocator());
    let const send = String::from(row.send_queue_bytes, heap_allocator());
    if (receive.length() > receive_width) receive_width = receive.length();
    if (send.length() > send_width) send_width = send.length();
    if (!should_show_detail) continue;
    if (row.socket_id.length() > socket_width)
      socket_width = row.socket_id.length();
    if (row.process_id.length() > process_width)
      process_width = row.process_id.length();
    if (row.owner_id.length() > owner_width)
      owner_width = row.owner_id.length();
    if (row.user.length() > user_width) user_width = row.user.length();
    if (row.name.length() > name_width) name_width = row.name.length();
    if (row.command.length() > command_width)
      command_width = row.command.length();
    if (row.net_namespace.length() > namespace_width) {
      namespace_width = row.net_namespace.length();
    }
    if (row.orchestrator.length() > orchestrator_width)
      orchestrator_width = row.orchestrator.length();
    if (row.runtime.length() > runtime_width)
      runtime_width = row.runtime.length();
    if (row.container.length() > container_width)
      container_width = row.container.length();
    if (row.cgroup.length() > cgroup_width) cgroup_width = row.cgroup.length();
  }

  let const do_append_column = [&](StringView text, usize width,
                                   bool is_numeric,
                                   StringView style = {}) throws {
    output += "  ";
    append_report_column(output, text, width, is_numeric, style, should_color);
  };
  append_report_column(output, "FAMILY", 6, false, colors::ansi::BOLD_CYAN,
                       should_color);
  do_append_column("PROTO", 5, false, colors::ansi::BOLD_CYAN);
  do_append_column("STATE", 10, false, colors::ansi::BOLD_CYAN);
  do_append_column("RECV-Q", receive_width, true, colors::ansi::BOLD_CYAN);
  do_append_column("SEND-Q", send_width, true, colors::ansi::BOLD_CYAN);
  do_append_column("LOCAL", local_width, false, colors::ansi::BOLD_CYAN);
  do_append_column("PEER", peer_width, false, colors::ansi::BOLD_CYAN);
  if (should_show_detail) {
    do_append_column("SOCKET", socket_width, true, colors::ansi::BOLD_CYAN);
    do_append_column("PID", process_width, true, colors::ansi::BOLD_CYAN);
    do_append_column("UID", owner_width, true, colors::ansi::BOLD_CYAN);
    do_append_column("USER", user_width, false, colors::ansi::BOLD_CYAN);
    do_append_column("NAME", name_width, false, colors::ansi::BOLD_CYAN);
    do_append_column("COMMAND", command_width, false, colors::ansi::BOLD_CYAN);
    do_append_column("NETNS", namespace_width, false, colors::ansi::BOLD_CYAN);
    do_append_column("ORCHESTRATOR", orchestrator_width, false,
                     colors::ansi::BOLD_CYAN);
    do_append_column("RUNTIME", runtime_width, false, colors::ansi::BOLD_CYAN);
    do_append_column("CONTAINER", container_width, false,
                     colors::ansi::BOLD_CYAN);
    do_append_column("CGROUP", cgroup_width, false, colors::ansi::BOLD_CYAN);
  }
  output += "\n";

  for (let const &row : remote_rows) {
    append_report_column(output, row.family, 6, false,
                         colors::ansi::BOLD_MAGENTA, should_color);
    do_append_column(row.protocol, 5, false, colors::ansi::BOLD_MAGENTA);
    do_append_column(row.state, 10, false, colors::ansi::BOLD_GREEN);
    do_append_column(String::from(row.receive_queue_bytes, heap_allocator()),
                     receive_width, true, colors::ansi::GREEN);
    do_append_column(String::from(row.send_queue_bytes, heap_allocator()),
                     send_width, true, colors::ansi::GREEN);
    do_append_column(row.local.view(), local_width, false,
                     colors::ansi::BOLD_CYAN);
    do_append_column(row.peer.view(), peer_width, false, colors::ansi::CYAN);
    if (should_show_detail) {
      do_append_column(row.socket_id.view(), socket_width, true,
                       colors::ansi::YELLOW);
      do_append_column(row.process_id.view(), process_width, true,
                       colors::ansi::YELLOW);
      do_append_column(row.owner_id.view(), owner_width, true, {});
      do_append_column(row.user.view(), user_width, false, {});
      do_append_column(row.name.view(), name_width, false, {});
      do_append_column(row.command.view(), command_width, false, {});
      do_append_column(row.net_namespace.view(), namespace_width, false, {});
      do_append_column(row.orchestrator.view(), orchestrator_width, false, {});
      do_append_column(row.runtime.view(), runtime_width, false, {});
      do_append_column(row.container.view(), container_width, false, {});
      do_append_column(row.cgroup.view(), cgroup_width, false, {});
    }
    output += "\n";
  }
}

fn append_runtime_report(String &output, bool should_color,
                         bool show_kubernetes, bool show_container,
                         bool should_show_detail,
                         const ArrayList<process_cgroup_snapshot> &snapshot)
    throws
    -> void
{
  let table = ReportTable{heap_allocator()};
  let const kubernetes =
      os::get_environment_variable("KUBERNETES_SERVICE_HOST");
  bool has_kubepods = false;
  bool has_docker = false;
  bool has_containerd = false;
  bool has_crio = false;
  bool has_libpod = false;
  for (let const &process : snapshot) {
    for (let const &evidence : process.evidence) {
      has_kubepods = has_kubepods || evidence.is_kubernetes;
      has_docker = has_docker || evidence.runtime == "docker";
      has_containerd = has_containerd || evidence.runtime == "containerd";
      has_crio = has_crio || evidence.runtime == "cri-o";
      has_libpod = has_libpod || evidence.runtime == "podman";
    }
  }
  let runtime = String{heap_allocator()};
  if (kubernetes.has_value() || has_kubepods)
    runtime += "kubernetes";
  if (has_docker) {
    if (!runtime.is_empty()) runtime += ", ";
    runtime += "docker";
  }
  if (has_containerd) {
    if (!runtime.is_empty()) runtime += ", ";
    runtime += "containerd";
  }
  if (has_crio) {
    if (!runtime.is_empty()) runtime += ", ";
    runtime += "cri-o";
  }
  if (has_libpod) {
    if (!runtime.is_empty()) runtime += ", ";
    runtime += "podman";
  }
  if (runtime.is_empty() &&
      Path{container_marker_path(".dockerenv", heap_allocator())}
          .is_regular_file())
    runtime = "docker";
  if (runtime.is_empty() &&
      Path{container_marker_path("run/.containerenv", heap_allocator())}
          .is_regular_file())
    runtime = "podman";
  if (show_container)
    table.add("Runtime", runtime.is_empty() ? "none detected" : runtime.view(),
              colors::ansi::BOLD_CYAN);
  if (show_kubernetes)
    table.add("Kubernetes",
              kubernetes.has_value() || has_kubepods ? "present"
              : "not detected",
              colors::ansi::BOLD_CYAN);
  output += table.to_string(should_color, "");

  if (!should_show_detail) return;

  struct runtime_process_row
  {
    String runtime{heap_allocator()};
    String source{heap_allocator()};
    String process_id{heap_allocator()};
    String name{heap_allocator()};
    String container{heap_allocator()};
    String orchestrator{heap_allocator()};
  };
  let rows = ArrayList<runtime_process_row>{heap_allocator()};
  for (let const &process : snapshot) {
    let runtime_name = String{heap_allocator(), "-"};
    let container_id = String{heap_allocator(), "-"};
    bool is_kubernetes = false;
    for (let const &evidence : process.evidence) {
      if (runtime_name == "-" && evidence.runtime != "-")
        runtime_name = evidence.runtime.clone();
      if (container_id == "-" && evidence.container_id != "-")
        container_id = evidence.container_id.clone();
      is_kubernetes = is_kubernetes || evidence.is_kubernetes;
    }
    if (runtime_name == "-" && !is_kubernetes) continue;
    rows.push({
        steal(runtime_name),
        String{heap_allocator(), "cgroup"},
        String::from(process.process_id, heap_allocator()),
        String{heap_allocator(), process.name.view()},
        steal(container_id),
        String{heap_allocator(), is_kubernetes ? "kubernetes" : "-"},
    });
  }
  if (!rows.is_empty()) {
    output += "\n";
    let detail_table = ReportTable{heap_allocator()};
    detail_table.add_column("RUNTIME", report_table_alignment::Left,
                            colors::ansi::BOLD_CYAN);
    detail_table.add_column("SOURCE", report_table_alignment::Left,
                            colors::ansi::BOLD_CYAN);
    detail_table.add_column("PID", report_table_alignment::Right,
                            colors::ansi::BOLD_CYAN);
    detail_table.add_column("NAME", report_table_alignment::Left,
                            colors::ansi::BOLD_CYAN);
    detail_table.add_column("CONTAINER", report_table_alignment::Left,
                            colors::ansi::BOLD_CYAN);
    detail_table.add_column("ORCHESTRATOR", report_table_alignment::Left,
                            colors::ansi::BOLD_CYAN);
    for (let const &row : rows) {
      let cells = ArrayList<report_table_cell_view>{heap_allocator()};
      cells.push({row.runtime.view(), colors::ansi::BOLD_GREEN});
      cells.push({row.source.view(), {}});
      cells.push({row.process_id.view(), colors::ansi::YELLOW});
      cells.push({row.name.view(), {}});
      cells.push({row.container.view(), {}});
      cells.push({row.orchestrator.view(), {}});
      detail_table.add_row(cells);
    }
    output += detail_table.to_string(should_color, "").view();
  }

  if (!show_kubernetes) return;
  let const namespace_file =
      Path{kubernetes_service_account_path("namespace", heap_allocator())}
          .read_entire_file();
  let namespace_name = String{heap_allocator()};
  if (namespace_file.has_value()) {
    namespace_name =
        String{heap_allocator(), namespace_file->view().trim_blanks()};
  }
  if (!kubernetes.has_value() && !has_kubepods && namespace_name.is_empty())
    return;

  output += "\n";
  let kube_table = ReportTable{heap_allocator()};
  kube_table.add_column("SOURCE", report_table_alignment::Left,
                        colors::ansi::BOLD_CYAN);
  kube_table.add_column("HOST", report_table_alignment::Left,
                        colors::ansi::BOLD_CYAN);
  kube_table.add_column("NAMESPACE", report_table_alignment::Left,
                        colors::ansi::BOLD_CYAN);
  kube_table.add_column("EVIDENCE", report_table_alignment::Left,
                        colors::ansi::BOLD_CYAN);
  if (kubernetes.has_value()) {
    let cells = ArrayList<report_table_cell_view>{heap_allocator()};
    cells.push({"environment", {}});
    cells.push({kubernetes->view(), colors::ansi::BOLD_GREEN});
    cells.push({namespace_name.is_empty() ? StringView{"-"}
                                       : namespace_name.view(), {}});
    cells.push({"KUBERNETES_SERVICE_HOST", {}});
    kube_table.add_row(cells);
  }
  if (has_kubepods) {
    let cells = ArrayList<report_table_cell_view>{heap_allocator()};
    cells.push({"cgroup", {}});
    cells.push({"-", {}});
    cells.push({namespace_name.is_empty() ? StringView{"-"}
                                       : namespace_name.view(), {}});
    cells.push({"kubepods", {}});
    kube_table.add_row(cells);
  }
  if (!namespace_name.is_empty() && !kubernetes.has_value() && !has_kubepods) {
    let cells = ArrayList<report_table_cell_view>{heap_allocator()};
    cells.push({"service-account", {}});
    cells.push({"-", {}});
    cells.push({namespace_name.view(), {}});
    cells.push({"namespace file", {}});
    kube_table.add_row(cells);
  }
  output += kube_table.to_string(should_color, "").view();
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
      !any_selector || FLAG_EVILISO_RUNTIME.is_enabled() ||
      FLAG_EVILISO_KUBERNETES.is_enabled() || FLAG_EVILISO_CONTAINER.is_enabled() ||
      FLAG_EVILISO_CONTAINERS.is_enabled();
  let const show_kubernetes =
      !any_selector || FLAG_EVILISO_RUNTIME.is_enabled() ||
      FLAG_EVILISO_KUBERNETES.is_enabled();
  let const show_container =
      !any_selector || FLAG_EVILISO_RUNTIME.is_enabled() ||
      FLAG_EVILISO_CONTAINER.is_enabled() || FLAG_EVILISO_CONTAINERS.is_enabled();
  let const should_color = koshkit_should_color();
  let output = String{cxt.scratch_allocator()};
  let process_cgroups =
      ArrayList<process_cgroup_snapshot>{cxt.scratch_allocator()};
  if (show_cgroups || show_runtime) {
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
        FLAG_EVILISO_REMOTE.is_enabled() || FLAG_EVILISO_ALL.is_enabled());
  if (show_runtime)
    append_runtime_report(output, should_color, show_kubernetes,
                          show_container, FLAG_EVILISO_ALL.is_enabled(),
                          process_cgroups);
  ec.print_to_stdout(output);
  return 0;
}

} // namespace koshka::koshkit
