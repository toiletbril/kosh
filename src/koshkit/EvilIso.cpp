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

HELP_SYNOPSIS_DECL("[-a] [-n] [-c] [-s] [-r] [-k]");

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
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(EvilIso);

namespace koshka::koshkit {

namespace {

fn append_namespace_report(String &output, bool should_color,
                           bool should_show_detail) throws -> void
{
  let table = ReportTable{heap_allocator()};
  constexpr StringView names[] = {"cgroup", "ipc",  "mnt",  "net",
                                  "pid",    "time", "user", "uts"};
  let const processes = os::enumerate_processes();
  for (let const name : names) {
    let const target =
        os::read_symlink(String{"/proc/self/ns/"} + name, heap_allocator());
    table.add(name, target.has_value() ? target->view() : "unavailable",
              colors::ansi::BOLD_CYAN);
    if (!target.has_value()) continue;

    usize process_count = 0;
    for (let const &process : processes) {
      let const process_namespace = os::read_symlink(
          String{"/proc/"} + String::from(process.pid, heap_allocator()) +
              "/ns/" + name,
          heap_allocator());
      if (process_namespace.has_value() &&
          process_namespace->view() == target->view())
        process_count++;
    }
    let const process_field = String::from(process_count, heap_allocator());
    table.add(String{name} + " processes", process_field.view(),
              colors::ansi::BOLD_CYAN);
    if (!should_show_detail) continue;

    for (let const &process : processes) {
      let const process_namespace = os::read_symlink(
          String{"/proc/"} + String::from(process.pid, heap_allocator()) +
              "/ns/" + name,
          heap_allocator());
      if (!process_namespace.has_value() ||
          process_namespace->view() != target->view())
        continue;

      let identity = String::from(process.pid, heap_allocator());
      identity += " (";
      identity += process.name.view();
      identity += process.pid == os::get_current_process_id() ? ", self; "
                                                              : ", other; ";
      identity += name;
      identity += " namespace ";
      identity += target->view();
      identity += ")";
      table.add(String{name} + " process", identity.view(),
                colors::ansi::BOLD_CYAN);
    }
  }
  output += table.to_string(should_color, "");
  output += '\n';
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
                        bool should_show_detail) throws -> void
{
  let const contents =
      Path{cgroup_proc_path("self/cgroup", heap_allocator())}
          .read_entire_file();
  let table = ReportTable{heap_allocator()};
  if (!contents.has_value()) {
    table.add("Membership", "unavailable", colors::ansi::BOLD_CYAN);
    output += table.to_string(should_color, "");
    return;
  }

  let const self_memberships =
      parse_cgroup_memberships(contents->view(), heap_allocator());
  if (self_memberships.is_empty()) {
    table.add("Membership", "unavailable", colors::ansi::BOLD_CYAN);
    output += table.to_string(should_color, "");
    return;
  }

  let const self_process_id = os::get_current_process_id();
  let self_name = String{heap_allocator(), "-"};
  if (let const name =
          Path{cgroup_proc_path("self/comm", heap_allocator())}
              .read_entire_file();
      name.has_value())
  {
    for (let const line : utils::split_lines(name->view())) {
      if (!line.is_empty()) self_name = String{heap_allocator(), line};
      break;
    }
  }

  let rows = ArrayList<cgroup_report_row>{heap_allocator()};
  for (let const &membership : self_memberships) {
    rows.push({
        String{heap_allocator(), membership.hierarchy.view()},
        membership.hierarchy_value,
        String{heap_allocator(), membership.controller.view()},
        String{heap_allocator(), membership.path.view()},
        String::from(self_process_id, heap_allocator()),
        self_process_id,
        0,
        String{heap_allocator(), self_name.view()},
        "self",
    });
  }

  if (should_show_detail) {
    let const processes =
        os::enumerate_processes(os::process_detail::ResourceStats);
    let candidate_rows = ArrayList<cgroup_report_row>{heap_allocator()};
    for (let const &process : processes) {
      if (process.pid == self_process_id) continue;
      let const process_contents =
          Path{cgroup_proc_path(
                   String::from(process.pid, heap_allocator()) + "/cgroup",
                   heap_allocator())}
              .read_entire_file();
      if (!process_contents.has_value()) continue;
      let const memberships =
          parse_cgroup_memberships(process_contents->view(), heap_allocator());
      for (let const &membership : memberships) {
        for (let const &self_membership : self_memberships) {
          if (membership.hierarchy != self_membership.hierarchy ||
              membership.controller != self_membership.controller ||
              membership.path != self_membership.path)
          {
            continue;
          }
          candidate_rows.push({
              String{heap_allocator(), membership.hierarchy.view()},
              membership.hierarchy_value,
              String{heap_allocator(), membership.controller.view()},
              String{heap_allocator(), membership.path.view()},
              String::from(process.pid, heap_allocator()),
              process.pid,
              process.start_token,
              process.name.is_empty()
                  ? String{heap_allocator(), "-"}
                  : String{heap_allocator(), process.name.view()},
              "other",
          });
          break;
        }
      }
    }
    let const current_processes =
        os::enumerate_processes(os::process_detail::ResourceStats);
    for (let &row : candidate_rows) {
      if (row.process_start_token == 0) continue;
      for (let const &process : current_processes) {
        if (process.pid != row.process_id_value ||
            process.start_token == 0 ||
            process.start_token != row.process_start_token)
        {
          continue;
        }
        row.name = process.name.is_empty()
                       ? String{heap_allocator(), "-"}
                       : String{heap_allocator(), process.name.view()};
        rows.push(steal(row));
        break;
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

  usize hierarchy_width = 9;
  usize controller_width = 10;
  usize path_width = 4;
  usize process_width = 3;
  usize name_width = 4;
  for (let const &row : rows) {
    if (row.hierarchy.length() > hierarchy_width)
      hierarchy_width = row.hierarchy.length();
    if (row.controller.length() > controller_width)
      controller_width = row.controller.length();
    if (row.path.length() > path_width) path_width = row.path.length();
    if (row.process_id.length() > process_width)
      process_width = row.process_id.length();
    if (row.name.length() > name_width) name_width = row.name.length();
  }

  let const do_append_column = [&](StringView text, usize width,
                                   bool is_numeric,
                                   StringView style = {}) throws {
    append_report_column(output, text, width, is_numeric, style, should_color);
    output += "  ";
  };
  do_append_column("HIERARCHY", hierarchy_width, true,
                   colors::ansi::BOLD_CYAN);
  do_append_column("CONTROLLER", controller_width, false,
                   colors::ansi::BOLD_CYAN);
  do_append_column("PATH", path_width, false, colors::ansi::BOLD_CYAN);
  do_append_column("PID", process_width, true, colors::ansi::BOLD_CYAN);
  do_append_column("NAME", name_width, false, colors::ansi::BOLD_CYAN);
  append_report_text(output, "ROLE", colors::ansi::BOLD_CYAN, should_color);
  output += '\n';
  for (let const &row : rows) {
    do_append_column(row.hierarchy.view(), hierarchy_width, true,
                     colors::ansi::BOLD_GREEN);
    do_append_column(row.controller.view(), controller_width, false, {});
    do_append_column(row.path.view(), path_width, false, {});
    do_append_column(row.process_id.view(), process_width, true,
                     colors::ansi::BOLD_GREEN);
    do_append_column(row.name.view(), name_width, false, {});
    append_report_text(output, row.role, colors::ansi::BOLD_MAGENTA,
                       should_color);
    output += '\n';
  }
}

fn append_session_report(String &output, bool should_color,
                         bool should_show_detail) throws -> void
{
  let table = ReportTable{heap_allocator()};
  let const sessions = os::logged_in_users();
  table.add("Count", String::from(sessions.count(), heap_allocator()).view(),
            colors::ansi::BOLD_CYAN);
  for (let const &session : sessions) {
    let text = String{heap_allocator(), session.user.view()};
    text += '@';
    text += session.terminal.view();
    table.add("Session", text.view(), colors::ansi::BOLD_CYAN);
    if (!should_show_detail) continue;
    table.add(
        "Login time",
        utils::format_unix_timestamp(session.login_time, "%Y-%m-%d %H:%M:%S")
            .view(),
        colors::ansi::BOLD_CYAN);
  }
  output += table.to_string(should_color, "");
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
  let const contents =
      Path{String{"/proc/"} + String::from(process_id, allocator) + "/cgroup"}
          .read_entire_file();
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
  output += "\n";
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

fn append_runtime_report(String &output, bool should_color) throws -> void
{
  let table = ReportTable{heap_allocator()};
  let const cgroup = Path{"/proc/1/cgroup"}.read_entire_file();
  let const cgroup_text = cgroup.has_value() ? cgroup->view() : StringView{};
  let const kubernetes =
      os::get_environment_variable("KUBERNETES_SERVICE_HOST");
  let runtime = String{heap_allocator()};
  if (kubernetes.has_value() ||
      cgroup_text.find_substring("kubepods").has_value())
    runtime += "kubernetes";
  if (cgroup_text.find_substring("docker").has_value()) {
    if (!runtime.is_empty()) runtime += ", ";
    runtime += "docker";
  }
  if (cgroup_text.find_substring("containerd").has_value()) {
    if (!runtime.is_empty()) runtime += ", ";
    runtime += "containerd";
  }
  if (cgroup_text.find_substring("crio").has_value()) {
    if (!runtime.is_empty()) runtime += ", ";
    runtime += "cri-o";
  }
  if (runtime.is_empty() && Path{"/.dockerenv"}.is_regular_file())
    runtime = "docker";
  if (runtime.is_empty() && Path{"/run/.containerenv"}.is_regular_file())
    runtime = "podman";
  table.add("Runtime", runtime.is_empty() ? "none detected" : runtime.view(),
            colors::ansi::BOLD_CYAN);
  table.add("Kubernetes",
            kubernetes.has_value() ||
                    cgroup_text.find_substring("kubepods").has_value()
                ? "present"
                : "not detected",
            colors::ansi::BOLD_CYAN);
  output += table.to_string(should_color, "");
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
      FLAG_EVILISO_REMOTE.is_enabled() || FLAG_EVILISO_RUNTIME.is_enabled();
  let const show_namespaces =
      !any_selector || FLAG_EVILISO_NAMESPACES.is_enabled();
  let const show_cgroups = !any_selector || FLAG_EVILISO_CGROUPS.is_enabled();
  let const show_sessions = !any_selector || FLAG_EVILISO_SESSIONS.is_enabled();
  let const show_remote = !any_selector || FLAG_EVILISO_REMOTE.is_enabled();
  let const show_runtime = !any_selector || FLAG_EVILISO_RUNTIME.is_enabled();
  let const should_color = koshkit_should_color();
  let output = String{cxt.scratch_allocator()};
  if (show_namespaces)
    append_namespace_report(output, should_color,
                            FLAG_EVILISO_ALL.is_enabled());
  if (show_cgroups)
    append_cgroup_report(output, should_color, FLAG_EVILISO_ALL.is_enabled());
  if (show_sessions)
    append_session_report(output, should_color, FLAG_EVILISO_ALL.is_enabled());
  if (show_remote)
    append_remote_report(
        output, should_color,
        FLAG_EVILISO_REMOTE.is_enabled() || FLAG_EVILISO_ALL.is_enabled(),
        FLAG_EVILISO_REMOTE.is_enabled() || FLAG_EVILISO_ALL.is_enabled());
  if (show_runtime) append_runtime_report(output, should_color);
  ec.print_to_stdout(output);
  return 0;
}

} // namespace koshka::koshkit
