/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This native test helper holds deterministic IPv4 and optional IPv6 loopback
 * connections while EvilIso inspects the Linux socket and process tables. It
 * reports the assigned endpoints and socket identities, then waits on a
 * bounded release FIFO so the CLI fixture needs no timing sleeps.
 */

#include "Common.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pwd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

struct loopback_connection
{
  int listener{-1};
  int client{-1};
  int accepted{-1};
  uint16_t server_port{0};
  uint16_t client_port{0};
  ino_t server_inode{0};
  ino_t client_inode{0};
};

struct process_context
{
  char orchestrator[16]{"-"};
  char runtime[16]{"-"};
  char container[65]{"-"};
  char cgroups[121]{"-"};
};

struct unix_socket_set
{
  int listener{-1};
  int client{-1};
  int accepted{-1};
  int unconnected{-1};
  ino_t listener_inode{0};
  ino_t client_inode{0};
  ino_t accepted_inode{0};
  ino_t unconnected_inode{0};
};

constexpr char UNIX_LISTENER_PATH[] = "eviliso-unix-listener";
constexpr char UNIX_CLIENT_PATH[] = "eviliso-unix-client";
constexpr char UNIX_UNCONNECTED_PATH[] = "eviliso-unix-unconnected";

fn close_connection(loopback_connection &connection) -> void
{
  if (connection.accepted >= 0) ::close(connection.accepted);
  if (connection.client >= 0) ::close(connection.client);
  if (connection.listener >= 0) ::close(connection.listener);
  connection = {};
}

fn close_unix_sockets(unix_socket_set &sockets) -> void
{
  if (sockets.unconnected >= 0) ::close(sockets.unconnected);
  if (sockets.accepted >= 0) ::close(sockets.accepted);
  if (sockets.client >= 0) ::close(sockets.client);
  if (sockets.listener >= 0) ::close(sockets.listener);
  ::unlink(UNIX_UNCONNECTED_PATH);
  ::unlink(UNIX_CLIENT_PATH);
  ::unlink(UNIX_LISTENER_PATH);
  sockets = {};
}

fn socket_port(const struct sockaddr_storage &address) -> uint16_t
{
  if (address.ss_family == AF_INET) {
    let const *ipv4 = reinterpret_cast<const struct sockaddr_in *>(&address);
    return ntohs(ipv4->sin_port);
  }
  let const *ipv6 = reinterpret_cast<const struct sockaddr_in6 *>(&address);
  return ntohs(ipv6->sin6_port);
}

fn open_loopback(int family, loopback_connection &connection, int &error_number)
    -> bool
{
  connection.listener = ::socket(family, SOCK_STREAM, 0);
  if (connection.listener < 0) {
    error_number = errno;
    return false;
  }

  int enabled = 1;
  if (family == AF_INET6 &&
      ::setsockopt(connection.listener, IPPROTO_IPV6, IPV6_V6ONLY, &enabled,
                   sizeof(enabled)) != 0)
  {
    error_number = errno;
    close_connection(connection);
    return false;
  }

  struct sockaddr_storage address{};
  socklen_t address_length = 0;
  if (family == AF_INET) {
    let *ipv4 = reinterpret_cast<struct sockaddr_in *>(&address);
    ipv4->sin_family = AF_INET;
    ipv4->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address_length = sizeof(*ipv4);
  } else {
    let *ipv6 = reinterpret_cast<struct sockaddr_in6 *>(&address);
    ipv6->sin6_family = AF_INET6;
    ipv6->sin6_addr = in6addr_loopback;
    address_length = sizeof(*ipv6);
  }

  if (::bind(connection.listener,
             reinterpret_cast<const struct sockaddr *>(&address),
             address_length) != 0 ||
      ::listen(connection.listener, 1) != 0)
  {
    error_number = errno;
    close_connection(connection);
    return false;
  }

  if (::getsockname(connection.listener,
                    reinterpret_cast<struct sockaddr *>(&address),
                    &address_length) != 0)
  {
    error_number = errno;
    close_connection(connection);
    return false;
  }
  connection.server_port = socket_port(address);

  connection.client = ::socket(family, SOCK_STREAM, 0);
  if (connection.client < 0 ||
      ::connect(connection.client,
                reinterpret_cast<const struct sockaddr *>(&address),
                address_length) != 0)
  {
    error_number = errno;
    close_connection(connection);
    return false;
  }

  connection.accepted = ::accept(connection.listener, nullptr, nullptr);
  if (connection.accepted < 0) {
    error_number = errno;
    close_connection(connection);
    return false;
  }

  struct sockaddr_storage client_address{};
  socklen_t client_address_length = sizeof(client_address);
  if (::getsockname(connection.client,
                    reinterpret_cast<struct sockaddr *>(&client_address),
                    &client_address_length) != 0)
  {
    error_number = errno;
    close_connection(connection);
    return false;
  }
  connection.client_port = socket_port(client_address);

  struct stat server_status{};
  struct stat client_status{};
  if (::fstat(connection.accepted, &server_status) != 0 ||
      ::fstat(connection.client, &client_status) != 0)
  {
    error_number = errno;
    close_connection(connection);
    return false;
  }
  connection.server_inode = server_status.st_ino;
  connection.client_inode = client_status.st_ino;
  return true;
}

fn is_optional_ipv6_error(int error_number) -> bool
{
  return error_number == EAFNOSUPPORT || error_number == EPROTONOSUPPORT ||
         error_number == EADDRNOTAVAIL || error_number == ENETUNREACH;
}

fn forward_marker(const loopback_connection &input,
                  const loopback_connection &output) -> bool
{
  char marker = 'F';
  char received = 0;
  if (::send(input.client, &marker, 1, 0) != 1 ||
      ::recv(input.accepted, &received, 1, MSG_WAITALL) != 1 ||
      received != marker || ::send(output.client, &received, 1, 0) != 1)
    return false;
  received = 0;
  return ::recv(output.accepted, &received, 1, MSG_WAITALL) == 1 &&
         received == marker;
}

fn bind_unix_socket(int descriptor, const char *path) -> bool
{
  struct sockaddr_un address{};
  address.sun_family = AF_UNIX;
  let const length = std::strlen(path);
  if (length >= sizeof(address.sun_path)) return false;
  std::memcpy(address.sun_path, path, length + 1);
  return ::bind(descriptor, reinterpret_cast<struct sockaddr *>(&address),
                sizeof(address)) == 0;
}

fn open_unix_sockets(unix_socket_set &sockets) -> bool
{
  ::unlink(UNIX_UNCONNECTED_PATH);
  ::unlink(UNIX_CLIENT_PATH);
  ::unlink(UNIX_LISTENER_PATH);

  sockets.listener = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (sockets.listener < 0 ||
      !bind_unix_socket(sockets.listener, UNIX_LISTENER_PATH) ||
      ::listen(sockets.listener, 1) != 0)
  {
    close_unix_sockets(sockets);
    return false;
  }

  sockets.client = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (sockets.client < 0 || !bind_unix_socket(sockets.client, UNIX_CLIENT_PATH))
  {
    close_unix_sockets(sockets);
    return false;
  }
  struct sockaddr_un listener_address{};
  listener_address.sun_family = AF_UNIX;
  std::memcpy(listener_address.sun_path, UNIX_LISTENER_PATH,
              sizeof(UNIX_LISTENER_PATH));
  if (::connect(sockets.client,
                reinterpret_cast<struct sockaddr *>(&listener_address),
                sizeof(listener_address)) != 0)
  {
    close_unix_sockets(sockets);
    return false;
  }
  sockets.accepted = ::accept(sockets.listener, nullptr, nullptr);
  if (sockets.accepted < 0) {
    close_unix_sockets(sockets);
    return false;
  }

  sockets.unconnected = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (sockets.unconnected < 0 ||
      !bind_unix_socket(sockets.unconnected, UNIX_UNCONNECTED_PATH))
  {
    close_unix_sockets(sockets);
    return false;
  }

  struct stat listener_status{};
  struct stat client_status{};
  struct stat accepted_status{};
  struct stat unconnected_status{};
  if (::fstat(sockets.listener, &listener_status) != 0 ||
      ::fstat(sockets.client, &client_status) != 0 ||
      ::fstat(sockets.accepted, &accepted_status) != 0 ||
      ::fstat(sockets.unconnected, &unconnected_status) != 0)
  {
    close_unix_sockets(sockets);
    return false;
  }
  sockets.listener_inode = listener_status.st_ino;
  sockets.client_inode = client_status.st_ino;
  sockets.accepted_inode = accepted_status.st_ino;
  sockets.unconnected_inode = unconnected_status.st_ino;
  return true;
}

fn is_hexadecimal(char byte) -> bool
{
  return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f') ||
         (byte >= 'A' && byte <= 'F');
}

fn read_process_context(process_context &context) -> bool
{
  let *file = std::fopen("/proc/self/cgroup", "r");
  if (file == nullptr) return false;

  char paths[64][1024]{};
  usize path_count = 0;
  char line[2048]{};
  while (path_count < 64 && std::fgets(line, sizeof(line), file) != nullptr) {
    let *first_separator = std::strchr(line, ':');
    if (first_separator == nullptr) continue;
    let *second_separator = std::strchr(first_separator + 1, ':');
    if (second_separator == nullptr) continue;
    let *path = second_separator + 1;
    let const newline = std::strchr(path, '\n');
    if (newline != nullptr) *newline = '\0';
    if (path[0] == '\0') continue;

    bool is_known = false;
    for (usize index = 0; index < path_count; index++) {
      if (std::strcmp(paths[index], path) == 0) {
        is_known = true;
        break;
      }
    }
    if (is_known) continue;
    std::snprintf(paths[path_count], sizeof(paths[path_count]), "%s", path);
    path_count++;
  }
  let const close_status = std::fclose(file);
  if (close_status != 0) return false;

  char all_cgroups[8192]{};
  if (path_count == 0) {
    std::snprintf(all_cgroups, sizeof(all_cgroups), "-");
  } else {
    usize length = 0;
    for (usize index = 0; index < path_count; index++) {
      let const separator = index == 0 ? "" : ",";
      let const written =
          std::snprintf(all_cgroups + length, sizeof(all_cgroups) - length,
                        "%s%s", separator, paths[index]);
      if (written < 0 ||
          static_cast<usize>(written) >= sizeof(all_cgroups) - length)
        return false;
      length += static_cast<usize>(written);
    }
  }

  if (std::strstr(all_cgroups, "kubepods") != nullptr)
    std::snprintf(context.orchestrator, sizeof(context.orchestrator),
                  "kubernetes");
  if (std::strstr(all_cgroups, "containerd") != nullptr)
    std::snprintf(context.runtime, sizeof(context.runtime), "containerd");
  else if (std::strstr(all_cgroups, "crio") != nullptr)
    std::snprintf(context.runtime, sizeof(context.runtime), "cri-o");
  else if (std::strstr(all_cgroups, "docker") != nullptr)
    std::snprintf(context.runtime, sizeof(context.runtime), "docker");
  else if (std::strstr(all_cgroups, "libpod") != nullptr)
    std::snprintf(context.runtime, sizeof(context.runtime), "podman");

  let const cgroup_length = std::strlen(all_cgroups);
  for (usize position = 0; position < cgroup_length;) {
    if (!is_hexadecimal(all_cgroups[position])) {
      position++;
      continue;
    }
    let const start = position;
    while (position < cgroup_length && is_hexadecimal(all_cgroups[position]))
      position++;
    if (position - start != 64) continue;
    let const prefix_start = start > 32 ? start - 32 : 0;
    char prefix[33]{};
    std::memcpy(prefix, all_cgroups + prefix_start, start - prefix_start);
    if (std::strstr(prefix, "docker") == nullptr &&
        std::strstr(prefix, "containerd") == nullptr &&
        std::strstr(prefix, "crio") == nullptr &&
        std::strstr(prefix, "libpod") == nullptr)
      continue;
    std::memcpy(context.container, all_cgroups + start, 64);
    context.container[64] = '\0';
    break;
  }

  if (cgroup_length <= 120) {
    std::snprintf(context.cgroups, sizeof(context.cgroups), "%s", all_cgroups);
  } else {
    std::memcpy(context.cgroups, all_cgroups, 117);
    std::memcpy(context.cgroups + 117, "...", 4);
  }
  return true;
}

}

fn main(int argument_count, char **) -> int
{
  if (argument_count != 1) return 2;
  let const *release_path = std::getenv("EVILISO_RELEASE");
  if (release_path == nullptr || release_path[0] == '\0') return 2;

  let const release_descriptor =
      ::open(release_path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
  if (release_descriptor < 0) return 3;

  loopback_connection ipv4{};
  int error_number = 0;
  if (!open_loopback(AF_INET, ipv4, error_number)) {
    ::close(release_descriptor);
    return 4;
  }

  loopback_connection ipv6{};
  let const has_ipv6 = open_loopback(AF_INET6, ipv6, error_number);
  if (!has_ipv6 && !is_optional_ipv6_error(error_number)) {
    close_connection(ipv4);
    ::close(release_descriptor);
    return 5;
  }

  loopback_connection forward_input{};
  if (!open_loopback(AF_INET, forward_input, error_number)) {
    close_connection(ipv6);
    close_connection(ipv4);
    ::close(release_descriptor);
    return 6;
  }
  loopback_connection forward_output{};
  if (!open_loopback(AF_INET, forward_output, error_number) ||
      !forward_marker(forward_input, forward_output))
  {
    close_connection(forward_output);
    close_connection(forward_input);
    close_connection(ipv6);
    close_connection(ipv4);
    ::close(release_descriptor);
    return 7;
  }

  unix_socket_set unix_sockets{};
  if (!open_unix_sockets(unix_sockets)) {
    close_connection(forward_output);
    close_connection(forward_input);
    close_connection(ipv6);
    close_connection(ipv4);
    ::close(release_descriptor);
    return 8;
  }

  char user_name[128]{};
  let const user_id = ::getuid();
  if (let const *password = ::getpwuid(user_id); password != nullptr) {
    std::snprintf(user_name, sizeof(user_name), "%s", password->pw_name);
  } else {
    std::snprintf(user_name, sizeof(user_name), "%u",
                  static_cast<unsigned>(user_id));
  }

  char network_namespace[128]{};
  let const namespace_length = ::readlink(
      "/proc/self/ns/net", network_namespace, sizeof(network_namespace) - 1);
  if (namespace_length <= 0) {
    std::snprintf(network_namespace, sizeof(network_namespace), "-");
  } else {
    network_namespace[namespace_length] = '\0';
  }

  process_context context{};
  if (!read_process_context(context)) {
    close_unix_sockets(unix_sockets);
    close_connection(forward_output);
    close_connection(forward_input);
    close_connection(ipv6);
    close_connection(ipv4);
    ::close(release_descriptor);
    return 9;
  }

  std::printf("READY %ld %u %s %s %u %u %llu %llu %u %u %u %llu %llu "
              "%u %u %llu %llu %u %u %llu %llu "
              "%s %llu %s %llu %llu %s %llu "
              "%s %s %s %s\n",
              static_cast<long>(::getpid()), static_cast<unsigned>(user_id),
              user_name, network_namespace,
              static_cast<unsigned>(ipv4.server_port),
              static_cast<unsigned>(ipv4.client_port),
              static_cast<unsigned long long>(ipv4.server_inode),
              static_cast<unsigned long long>(ipv4.client_inode),
              has_ipv6 ? 1u : 0u, static_cast<unsigned>(ipv6.server_port),
              static_cast<unsigned>(ipv6.client_port),
              static_cast<unsigned long long>(ipv6.server_inode),
              static_cast<unsigned long long>(ipv6.client_inode),
              static_cast<unsigned>(forward_input.server_port),
              static_cast<unsigned>(forward_input.client_port),
              static_cast<unsigned long long>(forward_input.server_inode),
              static_cast<unsigned long long>(forward_input.client_inode),
              static_cast<unsigned>(forward_output.server_port),
              static_cast<unsigned>(forward_output.client_port),
              static_cast<unsigned long long>(forward_output.server_inode),
              static_cast<unsigned long long>(forward_output.client_inode),
              UNIX_LISTENER_PATH,
              static_cast<unsigned long long>(unix_sockets.listener_inode),
              UNIX_CLIENT_PATH,
              static_cast<unsigned long long>(unix_sockets.client_inode),
              static_cast<unsigned long long>(unix_sockets.accepted_inode),
              UNIX_UNCONNECTED_PATH,
              static_cast<unsigned long long>(unix_sockets.unconnected_inode),
              context.orchestrator, context.runtime, context.container,
              context.cgroups);
  if (std::fflush(stdout) != 0) {
    close_unix_sockets(unix_sockets);
    close_connection(forward_output);
    close_connection(forward_input);
    close_connection(ipv6);
    close_connection(ipv4);
    ::close(release_descriptor);
    return 10;
  }

  struct pollfd waited{};
  waited.fd = release_descriptor;
  waited.events = POLLIN;
  let const poll_status = ::poll(&waited, 1, 30000);
  char release_byte = 0;
  let const was_released = poll_status > 0 && (waited.revents & POLLIN) != 0 &&
                           ::read(release_descriptor, &release_byte, 1) == 1;

  close_unix_sockets(unix_sockets);
  close_connection(forward_output);
  close_connection(forward_input);
  close_connection(ipv6);
  close_connection(ipv4);
  ::close(release_descriptor);
  return was_released ? 0 : 11;
}
