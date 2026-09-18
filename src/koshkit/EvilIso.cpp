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
#include "../Utils.hpp"

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
FLAG(EVILISO_REMOTE, Bool, 'r', "remote", "Report remote-capable sockets.");
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

fn append_cgroup_report(String &output, bool should_color) throws -> void
{
  let const contents = Path{"/proc/self/cgroup"}.read_entire_file();
  let table = ReportTable{heap_allocator()};
  if (!contents.has_value()) {
    table.add("Membership", "unavailable", colors::ansi::BOLD_CYAN);
    output += table.to_string(should_color, "");
    return;
  }

  StringView text = contents->view();
  usize index = 0;
  while (!text.is_empty()) {
    usize line_end = 0;
    while (line_end < text.length && text[line_end] != '\n') line_end++;
    let const line = text.substring_of_length(0, line_end);
    if (line_end < text.length)
      text = text.substring(line_end + 1);
    else
      text = StringView{};
    if (line.is_empty()) continue;
    table.add(String{"Controller "} + String::from(index++, heap_allocator()),
              line, colors::ansi::BOLD_CYAN);
  }
  output += table.to_string(should_color, "");
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
    table.add("Login time",
              utils::format_unix_timestamp(session.login_time,
                                           "%Y-%m-%d %H:%M:%S")
                  .view(),
              colors::ansi::BOLD_CYAN);
  }
  output += table.to_string(should_color, "");
}

fn append_remote_report(String &output, bool should_color,
                        bool should_show_detail) throws -> void
{
  let table = ReportTable{heap_allocator()};
  if (!os::has_network_socket_listing()) {
    table.add("Sockets", "unavailable", colors::ansi::BOLD_CYAN);
    output += table.to_string(should_color, "");
    return;
  }

  let const sockets = os::network_sockets(false);
  usize remote_count = 0;
  for (let const &socket : sockets) {
    if (!socket.peer_address.is_empty() && socket.peer_port != 0)
      remote_count++;
  }
  table.add("Remote sockets",
            String::from(remote_count, heap_allocator()).view(),
            colors::ansi::BOLD_CYAN);
  table.add("Total sockets",
            String::from(sockets.count(), heap_allocator()).view(),
            colors::ansi::BOLD_CYAN);
  output += table.to_string(should_color, "");
  if (should_show_detail) {
    struct remote_peer_row
    {
      String peer{heap_allocator()};
      StringView state;
      StringView protocol;
      String process{heap_allocator()};
    };
    let remote_rows = ArrayList<remote_peer_row>{heap_allocator()};
    for (let const &socket : sockets) {
      if (socket.peer_address.is_empty() || socket.peer_port == 0) continue;
      let text = String{heap_allocator()};
      text += socket.peer_address.view();
      text += ":";
      text += String::from(socket.peer_port, heap_allocator()).view();
      StringView state = "-";
      switch (socket.state) {
      case os::network_socket_state::Established: state = "ESTABLISHED"; break;
      case os::network_socket_state::Listen: state = "LISTEN"; break;
      case os::network_socket_state::SynSent: state = "SYN-SENT"; break;
      case os::network_socket_state::SynReceived: state = "SYN-RECV"; break;
      case os::network_socket_state::CloseWait: state = "CLOSE-WAIT"; break;
      case os::network_socket_state::FinWait1: state = "FIN-WAIT-1"; break;
      case os::network_socket_state::FinWait2: state = "FIN-WAIT-2"; break;
      case os::network_socket_state::Closing: state = "CLOSING"; break;
      case os::network_socket_state::LastAck: state = "LAST-ACK"; break;
      case os::network_socket_state::TimeWait: state = "TIME-WAIT"; break;
      case os::network_socket_state::Closed: state = "CLOSED"; break;
      default: break;
      }
      remote_rows.push(remote_peer_row{
          steal(text),
          state,
          socket.protocol == os::network_socket_protocol::Udp
              ? StringView{"UDP"}
              : StringView{"TCP"},
          socket.process_id == 0
              ? String{heap_allocator(), "-"}
              : String::from(socket.process_id, heap_allocator())});
    }
    if (remote_rows.is_empty()) return;

    usize peer_width = 4;
    usize process_width = 7;
    for (let const &row : remote_rows) {
      if (row.peer.length() > peer_width) peer_width = row.peer.length();
      if (row.process.length() > process_width) {
        process_width = row.process.length();
      }
    }

    output += "\n";
    append_report_column(output, "PEER", peer_width, false,
                         colors::ansi::BOLD_CYAN, should_color);
    output += "  ";
    append_report_column(output, "STATE", 11, false, colors::ansi::BOLD_CYAN,
                         should_color);
    output += "  ";
    append_report_column(output, "PROTO", 5, false, colors::ansi::BOLD_CYAN,
                         should_color);
    output += "  ";
    append_report_column(output, "PROCESS", process_width, false,
                         colors::ansi::BOLD_CYAN, should_color);
    output += "\n";
    for (let const &row : remote_rows) {
      append_report_column(output, row.peer.view(), peer_width, false,
                           colors::ansi::BOLD_GREEN, should_color);
      output += "  ";
      append_report_column(output, row.state, 11, false,
                           colors::ansi::BOLD_MAGENTA, should_color);
      output += "  ";
      append_report_column(output, row.protocol, 5, false,
                           colors::ansi::BOLD_MAGENTA, should_color);
      output += "  ";
      append_report_column(output, row.process.view(), process_width, false, {},
                           should_color);
      output += "\n";
    }
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
  let const show_namespaces = !any_selector || FLAG_EVILISO_NAMESPACES.is_enabled();
  let const show_cgroups = !any_selector || FLAG_EVILISO_CGROUPS.is_enabled();
  let const show_sessions = !any_selector || FLAG_EVILISO_SESSIONS.is_enabled();
  let const show_remote = !any_selector || FLAG_EVILISO_REMOTE.is_enabled();
  let const show_runtime = !any_selector || FLAG_EVILISO_RUNTIME.is_enabled();
  let const should_color = koshkit_should_color();
  let output = String{cxt.scratch_allocator()};
  if (show_namespaces)
    append_namespace_report(output, should_color,
                            FLAG_EVILISO_ALL.is_enabled());
  if (show_cgroups) append_cgroup_report(output, should_color);
  if (show_sessions)
    append_session_report(output, should_color,
                          FLAG_EVILISO_ALL.is_enabled());
  if (show_remote)
    append_remote_report(output, should_color,
                         FLAG_EVILISO_ALL.is_enabled());
  if (show_runtime) append_runtime_report(output, should_color);
  ec.print_to_stdout(output);
  return 0;
}

} // namespace koshka::koshkit
