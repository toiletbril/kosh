/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This routed POSIX source fragment implements descriptor and terminal
 * operations, signal handling, users and sessions, logging, clocks, resource
 * limits, system configuration, environment access, program-name
 * normalization, and platform initialization. Dedicated fragments contain
 * filesystem operations, process lifecycle code, and optional system
 * inspection facilities, keeping their specialized headers and conditionals
 * out of the general backend.
 */

#include "CLI.hpp"
#include "Errors.hpp"
#include "Eval.hpp"
#include "Platform.hpp"
#include "Utils.hpp"
#include "base/Common.hpp"
#include "base/Debug.hpp"
#include "base/StaticStringMap.hpp"
#include "base/Trace.hpp"

#include <locale.h>
#include <syslog.h>
#include <utmpx.h>
#include <wctype.h>

#if defined __APPLE__ || defined __FreeBSD__
#include <xlocale.h>
#endif

#if defined __linux__
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/sock_diag.h>
#include <linux/unix_diag.h>
#endif

namespace koshka {
namespace os {

#if defined __linux__

static pure fn linux_socket_state(u64 value) wontthrow -> network_socket_state
{
  switch (value) {
  case 1: return network_socket_state::Established;
  case 2: return network_socket_state::SynSent;
  case 3: return network_socket_state::SynReceived;
  case 4: return network_socket_state::FinWait1;
  case 5: return network_socket_state::FinWait2;
  case 6: return network_socket_state::TimeWait;
  case 7: return network_socket_state::Closed;
  case 8: return network_socket_state::CloseWait;
  case 9: return network_socket_state::LastAck;
  case 10: return network_socket_state::Listen;
  case 11: return network_socket_state::Closing;
  default: return network_socket_state::Unknown;
  }
}

static fn linux_socket_address(StringView encoded, Allocator allocator,
                               network_address_family family) throws
    -> Maybe<String>
{
  if (family == network_address_family::IPv6) {
    if (encoded.length != 32) return None;
    u8 bytes[16]{};
    for (usize word_index = 0; word_index < 4; word_index++) {
      let const word = utils::parse_integer_in_base_u64(
          encoded.substring_of_length(word_index * 8, 8), int_base::hex);
      if (word.is_error()) return None;
      let const value = word.value();
      for (usize byte_index = 0; byte_index < 4; byte_index++)
        bytes[word_index * 4 + byte_index] =
            static_cast<u8>((value >> (byte_index * 8)) & 0xff);
    }
    char text[INET6_ADDRSTRLEN]{};
    if (::inet_ntop(AF_INET6, bytes, text, sizeof(text)) == nullptr)
      return None;
    return String{allocator, StringView{text}};
  }

  if (encoded.length != 8) return None;
  let const value = utils::parse_integer_in_base_u64(encoded, int_base::hex);
  if (value.is_error()) return None;
  u32 address = static_cast<u32>(value.value());
  char text[INET_ADDRSTRLEN]{};
  if (::inet_ntop(AF_INET, &address, text, sizeof(text)) == nullptr)
    return None;
  return String{allocator, StringView{text}};
}

struct linux_socket_owner
{
  u64 inode{0};
  u64 start_token{0};
  u32 pid{0};
  bool has_start_token{false};
};

struct linux_unix_socket_peer
{
  u64 identity{0};
  u64 peer_identity{0};
  u32 receive_queue_bytes{0};
  u32 send_queue_bytes{0};
};

struct linux_unix_socket_peer_comparator
{
  pure fn operator()(const linux_unix_socket_peer &left,
                     const linux_unix_socket_peer &right) const wontthrow->bool
  {
    return left.identity < right.identity;
  }

  pure fn operator()(u64 left,
                     const linux_unix_socket_peer &right) const wontthrow->bool
  {
    return left < right.identity;
  }

  pure fn operator()(const linux_unix_socket_peer &left,
                     u64 right) const wontthrow->bool
  {
    return left.identity < right;
  }
};

using linux_unix_socket_peer_list =
    SortedArrayList<linux_unix_socket_peer, linux_unix_socket_peer_comparator>;

static fn linux_socket_proc_path(StringView suffix, Allocator allocator) throws
    -> String
{
  let path = String{allocator, "/proc"};
  path += suffix;
  return path;
}

static fn linux_unix_socket_peers(Allocator allocator) throws
    -> linux_unix_socket_peer_list
{
  let peers = ArrayList<linux_unix_socket_peer>{allocator};
  let const descriptor =
      ::socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_SOCK_DIAG);
  if (descriptor < 0)
    return linux_unix_socket_peer_list{allocator,
                                       linux_unix_socket_peer_comparator{}};
  defer { ::close(descriptor); };

  struct
  {
    struct nlmsghdr header;
    struct unix_diag_req request;
  } message{};
  message.header.nlmsg_len = NLMSG_LENGTH(sizeof(message.request));
  message.header.nlmsg_type = SOCK_DIAG_BY_FAMILY;
  message.header.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
  message.header.nlmsg_seq = 1;
  message.request.sdiag_family = AF_UNIX;
  message.request.udiag_states = UINT32_MAX;
  message.request.udiag_show = UDIAG_SHOW_PEER | UDIAG_SHOW_RQLEN;
  struct sockaddr_nl kernel{};
  kernel.nl_family = AF_NETLINK;
  if (::sendto(descriptor, &message, message.header.nlmsg_len, 0,
               reinterpret_cast<struct sockaddr *>(&kernel),
               sizeof(kernel)) < 0)
    return linux_unix_socket_peer_list{allocator,
                                       linux_unix_socket_peer_comparator{}};

  let const deadline_nanos = monotonic_nanos() + 1000000000;
  bool is_done = false;
  bool is_valid = true;
  while (!is_done && is_valid) {
    let const now_nanos = monotonic_nanos();
    if (now_nanos == 0 || now_nanos >= deadline_nanos) break;
    let const remaining_nanos = deadline_nanos - now_nanos;
    let const timeout_milliseconds =
        static_cast<int>((remaining_nanos + 999999) / 1000000);
    struct pollfd waited{};
    waited.fd = descriptor;
    waited.events = POLLIN;
    if (::poll(&waited, 1, timeout_milliseconds) <= 0 ||
        (waited.revents & POLLIN) == 0)
      break;

    alignas(struct nlmsghdr) char buffer[64 * 1024]{};
    struct iovec vector{buffer, sizeof(buffer)};
    struct msghdr response{};
    response.msg_iov = &vector;
    response.msg_iovlen = 1;
    let const received = ::recvmsg(descriptor, &response, MSG_TRUNC);
    if (received <= 0 || static_cast<usize>(received) > sizeof(buffer) ||
        (response.msg_flags & MSG_TRUNC) != 0)
    {
      is_valid = false;
      break;
    }
    u32 remaining = static_cast<u32>(received);
    for (let *header = reinterpret_cast<struct nlmsghdr *>(buffer);
         NLMSG_OK(header, remaining); header = NLMSG_NEXT(header, remaining))
    {
      if (header->nlmsg_seq != 1) continue;
      if ((header->nlmsg_flags & NLM_F_DUMP_INTR) != 0) {
        is_valid = false;
        break;
      }
      if (header->nlmsg_type == NLMSG_DONE) {
        is_done = true;
        break;
      }
      if (header->nlmsg_type == NLMSG_ERROR ||
          header->nlmsg_len < NLMSG_LENGTH(sizeof(struct unix_diag_msg)))
      {
        is_valid = false;
        break;
      }

      let *diagnostic =
          reinterpret_cast<struct unix_diag_msg *>(NLMSG_DATA(header));
      int attribute_length = static_cast<int>(
          header->nlmsg_len - NLMSG_LENGTH(sizeof(*diagnostic)));
      let *attribute = reinterpret_cast<struct rtattr *>(diagnostic + 1);
      linux_unix_socket_peer peer{diagnostic->udiag_ino, 0, 0, 0};
      for (; RTA_OK(attribute, attribute_length);
           attribute = RTA_NEXT(attribute, attribute_length))
      {
        if (attribute->rta_type == UNIX_DIAG_PEER &&
            RTA_PAYLOAD(attribute) >= sizeof(u32))
        {
          u32 peer_identity = 0;
          std::memcpy(&peer_identity, RTA_DATA(attribute),
                      sizeof(peer_identity));
          peer.peer_identity = peer_identity;
        }
        if (attribute->rta_type == UNIX_DIAG_RQLEN &&
            RTA_PAYLOAD(attribute) >= sizeof(struct unix_diag_rqlen))
        {
          struct unix_diag_rqlen queue_lengths{};
          std::memcpy(&queue_lengths, RTA_DATA(attribute),
                      sizeof(queue_lengths));
          peer.receive_queue_bytes = queue_lengths.udiag_rqueue;
          peer.send_queue_bytes = queue_lengths.udiag_wqueue;
        }
      }
      if (peer.peer_identity != 0 || peer.receive_queue_bytes != 0 ||
          peer.send_queue_bytes != 0)
      {
        peers.push(peer);
      }
    }
  }
  if (!is_done || !is_valid)
    return linux_unix_socket_peer_list{allocator,
                                       linux_unix_socket_peer_comparator{}};
  return steal(peers).make_sorted(linux_unix_socket_peer_comparator{});
}

static pure fn linux_unix_peer_record(const linux_unix_socket_peer_list &peers,
                                      u64 identity) wontthrow
    -> linux_unix_socket_peer
{
  if (let const index = peers.find(identity); index.has_value())
    return peers[*index];
  return linux_unix_socket_peer{identity, 0, 0, 0};
}

static fn linux_process_start_token(StringView process_directory) throws
    -> Maybe<u64>
{
  let const stat = Path{String{process_directory} + "/stat"}.read_entire_file();
  if (!stat.has_value()) return None;
  usize after_name_position = stat->count();
  for (usize position = stat->count(); position > 0; position--) {
    if (stat->view()[position - 1] == ')') {
      after_name_position = position;
      break;
    }
  }
  if (after_name_position >= stat->count()) return None;
  let const start_token =
      nth_space_field(stat->view().substring(after_name_position), 19)
          .to<u64>();
  if (start_token.is_error()) return None;
  return start_token.value();
}

static pure fn linux_unix_socket_field(StringView text, usize index) wontthrow
    -> StringView
{
  usize field = 0;
  usize position = 0;
  while (position < text.length) {
    while (position < text.length && text[position] == ' ')
      position++;
    if (position >= text.length) break;
    let const start = position;
    while (position < text.length && text[position] != ' ')
      position++;
    if (field == index)
      return text.substring_of_length(start, position - start);
    field++;
  }
  return StringView{};
}

template <typename Row>
static fn for_each_proc_net_row(StringView path, Row do_row) throws -> void
{
  let const contents = Path{path}.read_entire_file();
  if (!contents.has_value() || contents->is_empty()) return;

  let const text = contents->view();
  usize position = 0;
  bool is_header = true;
  while (position < text.length) {
    let const line = each_line(text, position);
    if (is_header) {
      is_header = false;
      continue;
    }
    do_row(line);
  }
}

template <typename Push>
static fn push_socket_for_owners(const ArrayList<linux_socket_owner> *owners,
                                 network_socket_process_mode process_mode,
                                 u64 inode, Push do_push_socket) throws -> void
{
  bool has_process_owner = false;
  if (process_mode == network_socket_process_mode::WithProcesses &&
      owners != nullptr)
  {
    for (let const &owner : *owners) {
      if (owner.inode == inode) {
        do_push_socket(owner.pid, owner.start_token, owner.has_start_token);
        has_process_owner = true;
      }
    }
  }
  if (!has_process_owner) do_push_socket(0, 0, false);
}

static fn linux_unix_sockets(const ArrayList<linux_socket_owner> *owners,
                             Allocator allocator,
                             network_socket_process_mode process_mode) throws
    -> ArrayList<network_socket_entry>
{
  let result = ArrayList<network_socket_entry>{allocator};
  let const peers = linux_unix_socket_peers(allocator);
  let const path = linux_socket_proc_path("/net/unix", allocator);
  for_each_proc_net_row(path.view(), [&](StringView line) throws {
    let const inode = linux_unix_socket_field(line, 6).to<u64>();
    if (inode.is_error()) return;
    let const flags = utils::parse_integer_in_base_u64(
        linux_unix_socket_field(line, 3), int_base::hex);
    let const type = linux_unix_socket_field(line, 4);
    let const state = linux_unix_socket_field(line, 5);
    let path = linux_unix_socket_field(line, 7);
    let const unix_type =
        type == "0002"
            ? network_unix_socket_type::Datagram
            : (type == "0005" ? network_unix_socket_type::SequentialPacket
                              : network_unix_socket_type::Stream);
    let const is_listener =
        !flags.is_error() && (flags.value() & 0x00010000u) != 0;
    let const row_state =
        is_listener ? network_socket_state::Listen
                    : (state == "03" ? network_socket_state::Established
                                     : network_socket_state::Unconnected);
    let const peer = linux_unix_peer_record(peers, inode.value());
    let const do_push_socket = [&](u32 process_id, u64 start_token,
                                   bool has_start_token) throws {
      let socket = network_socket_entry{
          String{allocator, path},
          String{allocator},
          inode.value(),
          peer.peer_identity,
          peer.receive_queue_bytes,
          peer.send_queue_bytes,
          0,
          process_id,
          0,
          0,
          network_socket_protocol::Unix,
          network_address_family::IPv4,
          row_state,
          unix_type
      };
      socket.owner_start_token = start_token;
      socket.has_owner_start_token = has_start_token;
      result.push(steal(socket));
    };
    push_socket_for_owners(owners, process_mode, inode.value(), do_push_socket);
  });

  return result;
}

static fn linux_socket_owners(Allocator allocator) throws
    -> ArrayList<linux_socket_owner>
{
  let owners = ArrayList<linux_socket_owner>{allocator};
  let const proc_path = linux_socket_proc_path({}, allocator);
  DIR *proc_directory = ::opendir(proc_path.c_str());
  if (proc_directory == nullptr) return owners;
  defer { ::closedir(proc_directory); };

  for (struct dirent *entry = ::readdir(proc_directory); entry != nullptr;
       entry = ::readdir(proc_directory))
  {
    let const name = StringView{entry->d_name};
    let const parsed_pid = name.to<u32>();
    if (parsed_pid.is_error()) continue;

    let const process_directory = proc_path + "/" + name;
    bool did_read_start_token = false;
    Maybe<u64> start_token = None;

    let const descriptor_path = process_directory + "/fd";
    DIR *descriptor_directory = ::opendir(descriptor_path.c_str());
    if (descriptor_directory == nullptr) continue;

    let const descriptor_directory_fd = ::dirfd(descriptor_directory);
    for (struct dirent *descriptor = ::readdir(descriptor_directory);
         descriptor != nullptr; descriptor = ::readdir(descriptor_directory))
    {
      if (descriptor->d_name[0] == '.') continue;
      char target[128];
      let const target_length =
          ::readlinkat(descriptor_directory_fd, descriptor->d_name, target,
                       sizeof(target) - 1);
      if (target_length <= 9) continue;
      target[target_length] = '\0';
      let const target_view =
          StringView{target, static_cast<usize>(target_length)};
      if (!target_view.starts_with("socket:[") ||
          target_view[target_view.length - 1] != ']')
        continue;
      let const inode =
          target_view.substring_of_length(8, target_view.length - 9).to<u64>();
      if (inode.is_error()) continue;
      bool is_known = false;
      for (let const &owner : owners) {
        if (owner.inode == inode.value() && owner.pid == parsed_pid.value()) {
          is_known = true;
          break;
        }
      }
      if (!is_known) {
        if (!did_read_start_token) {
          start_token = linux_process_start_token(process_directory.view());
          did_read_start_token = true;
        }
        owners.push(linux_socket_owner{inode.value(), start_token.value_or(0),
                                       parsed_pid.value(),
                                       start_token.has_value()});
      }
    }
    ::closedir(descriptor_directory);
  }

  return owners;
}

static fn linux_network_sockets_from_file(
    StringView path, network_socket_protocol protocol,
    const ArrayList<linux_socket_owner> *owners, Allocator allocator,
    network_address_family family,
    network_socket_process_mode process_mode) throws
    -> ArrayList<network_socket_entry>
{
  let result = ArrayList<network_socket_entry>{allocator};
  for_each_proc_net_row(path, [&](StringView line) throws {
    let const local = nth_space_field(line, 1);
    let const peer = nth_space_field(line, 2);
    let const state = nth_space_field(line, 3);
    let const queues = nth_space_field(line, 4);
    let const owner_word = nth_space_field(line, 7);
    let const inode_word = nth_space_field(line, 9);
    let const local_separator = local.find_character(':');
    let const peer_separator = peer.find_character(':');
    let const queue_separator = queues.find_character(':');
    if (!local_separator.has_value() || !peer_separator.has_value() ||
        !queue_separator.has_value())
      return;

    let const local_address = linux_socket_address(
        local.substring_of_length(0, *local_separator), allocator, family);
    let const peer_address = linux_socket_address(
        peer.substring_of_length(0, *peer_separator), allocator, family);
    let const local_port = utils::parse_integer_in_base_u64(
        local.substring(*local_separator + 1), int_base::hex);
    let const peer_port = utils::parse_integer_in_base_u64(
        peer.substring(*peer_separator + 1), int_base::hex);
    let const state_value =
        utils::parse_integer_in_base_u64(state, int_base::hex);
    let const receive_queue = utils::parse_integer_in_base_u64(
        queues.substring(*queue_separator + 1), int_base::hex);
    let const send_queue = utils::parse_integer_in_base_u64(
        queues.substring_of_length(0, *queue_separator), int_base::hex);
    let const owner_id = owner_word.to<u32>();
    let const inode = inode_word.to<u64>();
    if (!local_address.has_value() || !peer_address.has_value() ||
        local_port.is_error() || peer_port.is_error() ||
        state_value.is_error() || receive_queue.is_error() ||
        send_queue.is_error() || inode.is_error())
      return;

    let const do_push_socket = [&](u32 process_id, u64 start_token,
                                   bool has_start_token) throws {
      let socket = network_socket_entry{
          String{allocator, local_address->view()},
          String{allocator, peer_address->view() },
          inode.value(),
          0,
          receive_queue.value(),
          send_queue.value(),
          0,
          process_id,
          static_cast<u16>(local_port.value()),
          static_cast<u16>(peer_port.value()),
          protocol,
          family,
          protocol == network_socket_protocol::Udp
              ? network_socket_state::Unconnected
              : linux_socket_state(state_value.value()),
      };
      if (!owner_id.is_error()) {
        socket.owner_id = owner_id.value();
        socket.has_owner_id = true;
      }
      socket.owner_start_token = start_token;
      socket.has_owner_start_token = has_start_token;
      result.push(steal(socket));
    };

    push_socket_for_owners(owners, process_mode, inode.value(), do_push_socket);
  });

  return result;
}

#endif

fn logged_in_users() throws -> ArrayList<user_session>
{
  let result = ArrayList<user_session>{heap_allocator()};
  setutxent();
  defer { endutxent(); };
  const struct utmpx *entry;

  while ((entry = getutxent()) != nullptr) {
    if (entry->ut_type != USER_PROCESS || entry->ut_user[0] == '\0') continue;
    let const user_length = strnlen(entry->ut_user, sizeof(entry->ut_user));
    let const line_length = strnlen(entry->ut_line, sizeof(entry->ut_line));
    result.push(user_session{
        String{StringView{entry->ut_user, user_length}},
        String{StringView{entry->ut_line, line_length}},
        static_cast<i64>(entry->ut_tv.tv_sec),
    });
  }

  return result;
}

static pure fn system_log_priority(StringView priority) wontthrow -> int
{
  static constexpr static_string_entry<int> PRIORITY_ENTRIES[] = {
      {SSK("emerg"),   LOG_EMERG  },
      {SSK("alert"),   LOG_ALERT  },
      {SSK("crit"),    LOG_CRIT   },
      {SSK("err"),     LOG_ERR    },
      {SSK("warning"), LOG_WARNING},
      {SSK("notice"),  LOG_NOTICE },
      {SSK("info"),    LOG_INFO   },
      {SSK("debug"),   LOG_DEBUG  },
  };
  static constexpr StaticStringMap PRIORITIES{PRIORITY_ENTRIES};
  let name = priority;
  if (let const separator = priority.find_character('.'); separator.has_value())
    name = priority.substring(*separator + 1);
  return PRIORITIES.find(name).value_or(LOG_NOTICE);
}

fn write_system_log(const system_log_options &options) wontthrow -> bool
{
  let tag_text = String{heap_allocator(), options.tag};
  let message_text = String{heap_allocator(), options.message};
  let log_options = options.should_include_pid ? LOG_PID : 0;
  if (options.should_copy_to_stderr) log_options |= LOG_PERROR;
  openlog(tag_text.is_empty() ? nullptr : tag_text.c_str(), log_options, 0);
  syslog(system_log_priority(options.priority), "%s", message_text.c_str());
  closelog();
  return true;
}

volatile sig_atomic_t INTERRUPT_REQUESTED = 0;
volatile sig_atomic_t CHILD_STATE_CHANGED = 0;
volatile sig_atomic_t SIGNAL_PENDING = 0;

static constexpr i32 SIGNAL_FLAG_COUNT = 128;
static constexpr i32 CHILD_SIGNAL_NUMBER = SIGCHLD;
static volatile sig_atomic_t PENDING_SIGNAL_FLAGS[SIGNAL_FLAG_COUNT] = {};

static sigset_t SIGNALS_UNBLOCKED_BY_TRAP = {};

static sigset_t SIGNALS_WITH_TRAP_ACTION = {};

static bool IS_PIPE_SIGNAL_IGNORED_BY_TRAP = false;
static bool WAS_PIPE_SIGNAL_IGNORED_AT_ENTRY = false;

} /* namespace os */
} /* namespace koshka */

#define KOSH_UMASK(mask) umask(static_cast<mode_t>(mask))

namespace koshka {

namespace os {

static fn fork_job_process() throws -> process;

hot fn write_fd(os::descriptor fd, const opaque *buf, usize size) wontthrow
    -> Maybe<usize>
{
  loop
  {
    let written_count = write(fd, buf, size);
    if (written_count == -1 && errno == EINTR) {
      continue;
    }
    if (written_count == -1) return koshka::None;
    return static_cast<usize>(written_count);
  }
}

hot fn write_to_numbered_fd(i64 fd_number, const opaque *buf,
                            usize size) wontthrow -> Maybe<usize>
{
  return write_fd(static_cast<os::descriptor>(fd_number), buf, size);
}

hot fn read_fd(os::descriptor fd, opaque *buf, usize size) wontthrow
    -> Maybe<usize>
{
  loop
  {
    let read_count = read(fd, buf, size);
    if (read_count == -1 && errno == EINTR) {
      if (INTERRUPT_REQUESTED) return koshka::None;
      continue;
    }
    if (read_count == -1) return koshka::None;
    return static_cast<usize>(read_count);
  }
}

fn descriptor_is_seekable(os::descriptor fd) wontthrow -> bool
{
  return lseek(fd, 0, SEEK_CUR) != static_cast<off_t>(-1);
}

fn regular_descriptor_file_size(os::descriptor fd) wontthrow -> Maybe<u64>
{
  struct stat info{};
  if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size <= 0) {
    return None;
  }

  static let const page_byte_count = static_cast<u64>(sysconf(_SC_PAGESIZE));
  let const size = static_cast<u64>(info.st_size);
  let const is_page_multiple =
      page_byte_count != 0 && size % page_byte_count == 0;
  let const is_unbacked = is_page_multiple && info.st_blocks == 0;
  if (is_unbacked) return None;

  return size;
}

fn rewind_descriptor(os::descriptor fd, usize byte_count) wontthrow -> bool
{
  return lseek(fd, -static_cast<off_t>(byte_count), SEEK_CUR) !=
         static_cast<off_t>(-1);
}

fn seek_descriptor_from_start(os::descriptor fd, u64 byte_offset) wontthrow
    -> bool
{
  return lseek(fd, static_cast<off_t>(byte_offset), SEEK_SET) !=
         static_cast<off_t>(-1);
}

hot fn wait_for_fd_readable(os::descriptor fd, i64 timeout_nanos) wontthrow
    -> i32
{
  let const has_deadline = timeout_nanos > 0;
  let const start_nanos = monotonic_nanos();
  let const duration_nanos = static_cast<u64>(timeout_nanos);
  const u64 deadline_nanos = !has_deadline
                                 ? 0
                                 : (UINT64_MAX - start_nanos < duration_nanos
                                        ? UINT64_MAX
                                        : start_nanos + duration_nanos);
  loop
  {
    int timeout_millis = -1;
    if (timeout_nanos == 0) {
      timeout_millis = 0;
    } else if (has_deadline) {
      const u64 now_nanos = monotonic_nanos();
      if (now_nanos >= deadline_nanos) return 0;
      let const remaining_nanos = deadline_nanos - now_nanos;
      let remaining_millis = remaining_nanos / 1'000'000;
      if (remaining_nanos % 1'000'000 != 0) remaining_millis++;
      timeout_millis = static_cast<int>(
          remaining_millis > INT_MAX ? INT_MAX : remaining_millis);
    }

    struct pollfd watch;
    watch.fd = fd;
    watch.events = POLLIN;
    watch.revents = 0;
    const int ready = poll(&watch, 1, timeout_millis);
    if (ready < 0) {
      if (errno == EINTR) {
        if (INTERRUPT_REQUESTED) return -1;
        continue;
      }
      return -1;
    }
    if (ready == 0) {
      if (timeout_nanos == 0) return 0;
      continue;
    }
    if ((watch.revents & POLLNVAL) != 0) return -1;
    if ((watch.revents & (POLLIN | POLLHUP)) != 0) return 1;
    return -1;
  }
}

fn close_fd(os::descriptor fd) wontthrow -> bool
{
  const int prior_errno = errno;
  if (close(fd) == -1) return false;
  errno = prior_errno;
  return true;
}

fn redirect_stdout(os::descriptor target) wontthrow -> os::descriptor
{
  const os::descriptor saved = fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 0);
  dup2(target, STDOUT_FILENO);
  note_descriptor_rebound();

  if (const int flags = fcntl(target, F_GETFD);
      flags != -1 && (flags & FD_CLOEXEC) == 0)
  {
    fcntl(target, F_SETFD, flags | FD_CLOEXEC);
  }

  return saved;
}

fn restore_stdout(os::descriptor saved) wontthrow -> void
{
  dup2(saved, STDOUT_FILENO);
  note_descriptor_rebound();
  close(saved);
}

constexpr int SHELL_BACKUP_FD_FLOOR = 10;

constexpr int SHELL_HIDDEN_FD_CEILING = 255;

static fn highest_free_shell_fd() wontthrow -> int
{
  const int prior_errno = errno;
  int ceiling_fd = SHELL_HIDDEN_FD_CEILING;

  let const open_file_limit = get_resource_limit(resource_kind::OpenFiles);
  if (open_file_limit.has_value() &&
      open_file_limit->soft != RESOURCE_UNLIMITED &&
      open_file_limit->soft <= static_cast<u64>(ceiling_fd))
  {
    ceiling_fd = static_cast<int>(open_file_limit->soft) - 1;
  }

  int placement_fd = SHELL_BACKUP_FD_FLOOR;
  for (int candidate_fd = ceiling_fd; candidate_fd > SHELL_BACKUP_FD_FLOOR;
       candidate_fd--)
  {
    if (fcntl(candidate_fd, F_GETFD) == -1 && errno == EBADF) {
      placement_fd = candidate_fd;
      break;
    }
  }

  errno = prior_errno;

  return placement_fd;
}

static fn save_descriptor_at(i32 shell_fd, int floor_fd) wontthrow
    -> saved_descriptor
{
  saved_descriptor result{};
  result.shell_fd = shell_fd;

  os::descriptor backup = fcntl(shell_fd, F_DUPFD_CLOEXEC, floor_fd);

  if (backup == -1 && errno != EBADF && floor_fd != SHELL_BACKUP_FD_FLOOR) {
    backup = fcntl(shell_fd, F_DUPFD_CLOEXEC, SHELL_BACKUP_FD_FLOOR);
  }

  if (backup == -1 && errno != EBADF) {
    backup = fcntl(shell_fd, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
  }

  result.was_open = backup != -1;
  result.saved = backup;
  result.is_dup2_ok = backup != -1 || errno == EBADF;

  return result;
}

static fn save_and_replace_descriptor_at(i32 shell_fd, os::descriptor target,
                                         int floor_fd) wontthrow
    -> saved_descriptor
{
  if (fcntl(target, F_GETFD) == -1) {
    saved_descriptor failed{};
    failed.shell_fd = shell_fd;
    failed.is_dup2_ok = false;
    return failed;
  }

  let result = save_descriptor_at(shell_fd, floor_fd);
  if (!result.is_dup2_ok) return result;

  result.is_dup2_ok = dup2(target, shell_fd) != -1;
  note_descriptor_rebound();

  return result;
}

fn save_and_replace_descriptor(i32 shell_fd, os::descriptor target) wontthrow
    -> saved_descriptor
{
  return save_and_replace_descriptor_at(shell_fd, target,
                                        SHELL_BACKUP_FD_FLOOR);
}

fn save_and_replace_descriptor_out_of_reach(i32 shell_fd,
                                            os::descriptor target) wontthrow
    -> saved_descriptor
{
  return save_and_replace_descriptor_at(shell_fd, target,
                                        highest_free_shell_fd());
}

fn restore_descriptor(const saved_descriptor &saved) wontthrow -> void
{
  if (!saved.is_dup2_ok) {
    if (saved.was_open) close(saved.saved);
    return;
  }

  if (saved.was_open) {
    dup2(saved.saved, saved.shell_fd);
    close(saved.saved);
  } else {
    close(saved.shell_fd);
  }

  note_descriptor_rebound();
}

fn save_descriptor(i32 shell_fd) wontthrow -> saved_descriptor
{
  return save_descriptor_at(shell_fd, SHELL_BACKUP_FD_FLOOR);
}

fn save_descriptor_out_of_reach(i32 shell_fd) wontthrow -> saved_descriptor
{
  return save_descriptor_at(shell_fd, highest_free_shell_fd());
}

fn reopen_terminal_as_stdin() wontthrow -> bool
{
  const int tty_fd = open("/dev/tty", O_RDWR);
  if (tty_fd == -1) return false;
  LOG(Info, "reopening the controlling terminal onto fd 0");
  let const was_replaced = dup2(tty_fd, STDIN_FILENO) != -1;
  note_descriptor_rebound();
  close(tty_fd);

  return was_replaced && isatty(STDIN_FILENO) == 1;
}

fn descriptor_for_shell_fd(i32 shell_fd) wontthrow -> os::descriptor
{
  return shell_fd;
}

fn duplicate_shell_fd(i32 shell_fd) wontthrow -> os::descriptor
{
  const os::descriptor copy =
      fcntl(shell_fd, F_DUPFD_CLOEXEC, SHELL_BACKUP_FD_FLOOR);

  return copy != -1 ? copy : KOSH_INVALID_FD;
}

fn move_descriptor_to_free_shell_fd(os::descriptor source,
                                    i32 floor_fd) wontthrow -> i32
{
  let const moved = fcntl(source, F_DUPFD_CLOEXEC, floor_fd);
  if (moved == -1) return -1;

  close(source);
  note_descriptor_rebound();

  return moved;
}

fn descriptors_refer_to_same_file(os::descriptor first,
                                  os::descriptor second) wontthrow -> bool
{
  struct stat first_status{};
  struct stat second_status{};
  if (::fstat(first, &first_status) != 0 ||
      ::fstat(second, &second_status) != 0)
    return false;

  return first_status.st_dev == second_status.st_dev &&
         first_status.st_ino == second_status.st_ino;
}

fn descriptor_from_fd_number(i64 fd_number) wontthrow -> os::descriptor
{
  if (fd_number < 0 || fd_number > INT32_MAX) return KOSH_INVALID_FD;

  return static_cast<os::descriptor>(fd_number);
}

fn replace_descriptor(i32 shell_fd, os::descriptor target) wontthrow -> bool
{
  if (target == shell_fd) return true;

  let const was_replaced = dup2(target, shell_fd) != -1;
  note_descriptor_rebound();

  return was_replaced;
}

fn close_shell_fd(i32 shell_fd) wontthrow -> bool
{
  let const was_closed = close(shell_fd) != -1;
  note_descriptor_rebound();

  return was_closed;
}

fn allocate_free_shell_fd(i32 floor_fd) wontthrow -> i32
{
  const i32 probe_sources[] = {STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO};
  for (let const source : probe_sources) {
    const int allocated = fcntl(source, F_DUPFD_CLOEXEC, floor_fd);
    if (allocated != -1) {
      close(allocated);
      return allocated;
    }
  }

  return -1;
}

static fn passwd_field(StringView line, usize index) wontthrow -> StringView;

fn get_current_user() throws -> Maybe<String>
{
  if (const char *name = std::getenv("LOGNAME"); name != nullptr)
    return String{name};
  if (const char *name = std::getenv("USER"); name != nullptr)
    return String{name};

  return uid_to_username(static_cast<u32>(getuid()));
}

fn get_login_user() throws -> Maybe<String>
{
  char name[256];
  if (getlogin_r(name, sizeof(name)) == 0) return String{name};
  return uid_to_username(static_cast<u32>(getuid()));
}

fn get_hostname() throws -> Maybe<String>
{
  char buffer[256];
  if (gethostname(buffer, sizeof(buffer)) != 0) return koshka::None;
  buffer[sizeof(buffer) - 1] = '\0';

  return String{buffer};
}

fn network_interface_addresses() throws -> ArrayList<network_interface_address>
{
  let result = ArrayList<network_interface_address>{heap_allocator()};
  struct ifaddrs *interfaces = nullptr;
  if (::getifaddrs(&interfaces) != 0) return result;
  defer { ::freeifaddrs(interfaces); };

  for (let *current = interfaces; current != nullptr;
       current = current->ifa_next)
  {
    if (current->ifa_name == nullptr || current->ifa_addr == nullptr) continue;

    let const family = current->ifa_addr->sa_family;
    if (family != AF_INET && family != AF_INET6) continue;

    char address[NI_MAXHOST]{};
    let const address_length = family == AF_INET ? sizeof(struct sockaddr_in)
                                                 : sizeof(struct sockaddr_in6);
    if (::getnameinfo(current->ifa_addr, static_cast<socklen_t>(address_length),
                      address, sizeof(address), nullptr, 0,
                      NI_NUMERICHOST) != 0)
    {
      continue;
    }

    result.push(network_interface_address{
        String{current->ifa_name},
        family == AF_INET ? network_address_family::IPv4
                          : network_address_family::IPv6,
        String{address},
    });
  }

  return result;
}

fn default_network_interface(Allocator allocator) throws -> Maybe<String>
{
#if defined __linux__
  let contents = Path{"/proc/net/route"}.read_entire_file();
  if (!contents.has_value()) return None;

  usize position = 0;
  while (position < contents->length()) {
    let const line_start = position;
    while (position < contents->length() && contents->view()[position] != '\n')
      position++;
    let const line =
        contents->view().substring_of_length(line_start, position - line_start);
    if (position < contents->length()) position++;
    if (line.starts_with("Iface")) continue;

    StringView words[4]{};
    usize word_count = 0;
    usize word_position = 0;
    while (word_position < line.length && word_count < countof(words)) {
      while (word_position < line.length &&
             (line[word_position] == ' ' || line[word_position] == '\t'))
        word_position++;
      let const word_start = word_position;
      while (word_position < line.length && line[word_position] != ' ' &&
             line[word_position] != '\t')
        word_position++;
      if (word_position > word_start)
        words[word_count++] =
            line.substring_of_length(word_start, word_position - word_start);
    }
    let const has_up_flag =
        word_count >= 4 && words[3].length >= 4 &&
        (words[3][3] == '1' || words[3][3] == '3' || words[3][3] == '5' ||
         words[3][3] == '7' || words[3][3] == '9' || words[3][3] == 'b' ||
         words[3][3] == 'B' || words[3][3] == 'd' || words[3][3] == 'D' ||
         words[3][3] == 'f' || words[3][3] == 'F');
    if (word_count >= 4 && words[1] == "00000000" && has_up_flag)
      return String{allocator, words[0]};
  }
#else
  unused(allocator);
#endif
  return None;
}

#if defined __APPLE__

enum class socket_address_side : u8
{
  Local,
  Peer,
};

static pure fn socket_state_of(int state) wontthrow -> network_socket_state
{
  switch (state) {
  case TSI_S_CLOSED: return network_socket_state::Closed;
  case TSI_S_LISTEN: return network_socket_state::Listen;
  case TSI_S_SYN_SENT: return network_socket_state::SynSent;
  case TSI_S_SYN_RECEIVED: return network_socket_state::SynReceived;
  case TSI_S_ESTABLISHED: return network_socket_state::Established;
  case TSI_S__CLOSE_WAIT: return network_socket_state::CloseWait;
  case TSI_S_FIN_WAIT_1: return network_socket_state::FinWait1;
  case TSI_S_CLOSING: return network_socket_state::Closing;
  case TSI_S_LAST_ACK: return network_socket_state::LastAck;
  case TSI_S_FIN_WAIT_2: return network_socket_state::FinWait2;
  case TSI_S_TIME_WAIT: return network_socket_state::TimeWait;
  default: return network_socket_state::Unknown;
  }
}

static fn socket_address(const struct in_sockinfo &info,
                         network_address_family family,
                         socket_address_side side) throws -> String
{
  char buffer[INET6_ADDRSTRLEN]{};
  const opaque *address = nullptr;
  int native_family = AF_INET;
  if (family == network_address_family::IPv4) {
    switch (side) {
    case socket_address_side::Local:
      address = static_cast<const opaque *>(&info.insi_laddr.ina_46.i46a_addr4);
      break;
    case socket_address_side::Peer:
      address = static_cast<const opaque *>(&info.insi_faddr.ina_46.i46a_addr4);
      break;
    }
  } else {
    native_family = AF_INET6;
    switch (side) {
    case socket_address_side::Local:
      address = static_cast<const opaque *>(&info.insi_laddr.ina_6);
      break;
    case socket_address_side::Peer:
      address = static_cast<const opaque *>(&info.insi_faddr.ina_6);
      break;
    }
  }

  if (::inet_ntop(native_family, address, buffer, sizeof(buffer)) == nullptr) {
    return String{"*"};
  }

  return String{buffer};
}

#endif

fn has_network_socket_listing() wontthrow -> bool
{
#if defined __APPLE__ || defined __linux__
  return true;
#else
  return false;
#endif
}

fn network_sockets(network_socket_process_mode process_mode) throws
    -> ArrayList<network_socket_entry>
{
  let result = ArrayList<network_socket_entry>{heap_allocator()};

#if defined __APPLE__
  let const processes = enumerate_processes();
  for (let const &process : processes) {
    let const process_id = static_cast<pid_t>(process.pid);
    let descriptor_bytes =
        ::proc_pidinfo(process_id, PROC_PIDLISTFDS, 0, nullptr, 0);
    if (descriptor_bytes <= 0) continue;

    ArrayList<struct proc_fdinfo> descriptors{heap_allocator()};
    descriptors.reserve(static_cast<usize>(descriptor_bytes) /
                        sizeof(struct proc_fdinfo));
    descriptor_bytes = ::proc_pidinfo(process_id, PROC_PIDLISTFDS, 0,
                                      descriptors.begin(), descriptor_bytes);
    if (descriptor_bytes <= 0) continue;

    let const descriptor_count =
        static_cast<usize>(descriptor_bytes) / sizeof(struct proc_fdinfo);
    for (usize index = 0; index < descriptor_count; index++) {
      let const &descriptor = descriptors.begin()[index];
      if (descriptor.proc_fdtype != PROX_FDTYPE_SOCKET) continue;

      struct socket_fdinfo socket{};
      if (::proc_pidfdinfo(process_id, descriptor.proc_fd, PROC_PIDFDSOCKETINFO,
                           &socket, sizeof(socket)) != sizeof(socket))
      {
        continue;
      }

      let const &info = socket.psi;
      if (info.soi_family != AF_INET && info.soi_family != AF_INET6) continue;
      if (info.soi_protocol != IPPROTO_TCP && info.soi_protocol != IPPROTO_UDP)
      {
        continue;
      }

      let const protocol = info.soi_protocol == IPPROTO_TCP
                               ? network_socket_protocol::Tcp
                               : network_socket_protocol::Udp;
      let const family = info.soi_family == AF_INET
                             ? network_address_family::IPv4
                             : network_address_family::IPv6;
      let const &internet = protocol == network_socket_protocol::Tcp
                                ? info.soi_proto.pri_tcp.tcpsi_ini
                                : info.soi_proto.pri_in;
      let entry = network_socket_entry{
          socket_address(internet, family, socket_address_side::Local),
          socket_address(internet, family, socket_address_side::Peer),
          info.soi_so,
          0,
          info.soi_rcv.sbi_cc,
          info.soi_snd.sbi_cc,
          0,
          process_mode == network_socket_process_mode::WithProcesses
              ? static_cast<u32>(process.pid)
              : 0,
          ntohs(static_cast<u16>(internet.insi_lport)),
          ntohs(static_cast<u16>(internet.insi_fport)),
          protocol,
          family,
          protocol == network_socket_protocol::Tcp
              ? socket_state_of(info.soi_proto.pri_tcp.tcpsi_state)
              : network_socket_state::Unconnected,
          network_unix_socket_type::Stream,
      };
      entry.owner_start_token = process.start_token;
      entry.owner_id = process.owner_id;
      entry.has_owner_start_token = process.start_token != 0;
      entry.has_owner_id = true;
      result.push(steal(entry));
    }
  }
#elif defined __linux__
  let const allocator = heap_allocator();
  let owners = ArrayList<linux_socket_owner>{allocator};
  if (process_mode == network_socket_process_mode::WithProcesses)
    owners = linux_socket_owners(allocator);
  struct linux_socket_source
  {
    StringView suffix;
    network_socket_protocol protocol;
    network_address_family family;
  };
  constexpr linux_socket_source SOURCES[] = {
      {"/net/tcp",  network_socket_protocol::Tcp, network_address_family::IPv4},
      {"/net/tcp6", network_socket_protocol::Tcp, network_address_family::IPv6},
      {"/net/udp",  network_socket_protocol::Udp, network_address_family::IPv4},
      {"/net/udp6", network_socket_protocol::Udp, network_address_family::IPv6},
  };
  for (let const &source : SOURCES) {
    let const path = linux_socket_proc_path(source.suffix, allocator);
    let entries = linux_network_sockets_from_file(
        path.view(), source.protocol,
        process_mode == network_socket_process_mode::WithProcesses ? &owners
                                                                   : nullptr,
        allocator, source.family, process_mode);
    for (let &entry : entries)
      result.push(steal(entry));
  }
  let unix_entries = linux_unix_sockets(
      process_mode == network_socket_process_mode::WithProcesses ? &owners
                                                                 : nullptr,
      allocator, process_mode);
  for (let &entry : unix_entries)
    result.push(steal(entry));
#else
  unused(process_mode);
#endif

  return result;
}

fn get_processor_counts() wontthrow -> processor_counts
{
  let const online = sysconf(_SC_NPROCESSORS_ONLN);
  let const configured = sysconf(_SC_NPROCESSORS_CONF);
  processor_counts counts{};
  if (online > 0) counts.online_count = static_cast<usize>(online);
  if (configured > 0) counts.configured_count = static_cast<usize>(configured);
  counts.online_count =
      affinity_processor_count(counts.online_count, counts.configured_count);
  if (counts.configured_count < counts.online_count)
    counts.configured_count = counts.online_count;
  return counts;
}

fn get_home_directory() throws -> Maybe<Path>
{
  if (let const home = get_environment_variable("HOME"); home.has_value())
    return Path{StringView{*home}};
  return koshka::None;
}

fn get_system_koshconf_path() throws -> Maybe<Path>
{
  return Path{StringView{"/etc/kosh.conf"}};
}

static fn passwd_field(StringView line, usize index) wontthrow -> StringView
{
  usize field_start_position = 0;
  usize field_index = 0;
  for (usize i = 0; i <= line.length; i++) {
    if (i != line.length && line[i] != ':') continue;
    if (field_index == index)
      return line.substring_of_length(field_start_position,
                                      i - field_start_position);
    field_index++;
    field_start_position = i + 1;
  }
  return StringView{};
}

fn get_home_for_user(StringView username) throws -> Maybe<Path>
{
  if (username.is_empty()) return koshka::None;

  let const contents = Path{StringView{"/etc/passwd"}}.read_entire_file();
  if (!contents) return koshka::None;

  let const text = contents->view();
  for (let const &line : utils::split_lines(text)) {
    if (passwd_field(line, 0) != username) continue;
    let const home_field = passwd_field(line, 5);
    if (home_field.is_empty()) return koshka::None;
    return Path{home_field};
  }
  return koshka::None;
}

static fn enumerate_first_fields(const Path &database) throws
    -> ArrayList<String>
{
  ArrayList<String> names{heap_allocator()};
  let const contents = database.read_entire_file();
  if (!contents) return names;

  let const text = contents->view();
  for (let const &line : utils::split_lines(text)) {
    let const name = passwd_field(line, 0);
    if (!name.is_empty() && line.find_character(':').has_value()) {
      names.push(String{name});
    }
  }

  return names;
}

fn enumerate_users() throws -> ArrayList<String>
{
  return enumerate_first_fields(Path{"/etc/passwd"});
}

fn enumerate_groups() throws -> ArrayList<String>
{
  return enumerate_first_fields(Path{"/etc/group"});
}

static pid_t PARENT_SHELL_PID = getpid();
static pid_t PARENT_SHELL_PARENT_PID = getppid();

fn is_stdin_a_tty() wontthrow -> bool { return isatty(KOSH_STDIN); }

fn is_stdout_a_tty() wontthrow -> bool { return isatty(KOSH_STDOUT); }

fn is_stderr_a_tty() wontthrow -> bool { return isatty(KOSH_STDERR); }
fn is_fd_a_tty(descriptor fd) wontthrow -> bool { return isatty(fd); }

fn terminal_name(descriptor fd) throws -> Maybe<String>
{
  char buffer[1024];
  if (ttyname_r(fd, buffer, sizeof(buffer)) != 0) return None;
  return String{buffer};
}

terminal_echo_guard::terminal_echo_guard(descriptor input,
                                         terminal_echo_mode mode) wontthrow
    : m_input(input)
{
  if (mode != terminal_echo_mode::Disable || !is_fd_a_tty(input)) return;
  if (tcgetattr(input, &m_original_mode) != 0) {
    m_did_succeed = false;
    return;
  }

  let quiet_mode = m_original_mode;
  quiet_mode.c_lflag &= static_cast<tcflag_t>(~ECHO);
  if (tcsetattr(input, TCSANOW, &quiet_mode) != 0) {
    m_did_succeed = false;
    return;
  }

  m_should_restore = true;
}

terminal_echo_guard::~terminal_echo_guard()
{
  if (m_should_restore) unused(tcsetattr(m_input, TCSANOW, &m_original_mode));
}

pure fn terminal_echo_guard::did_succeed() const wontthrow -> bool
{
  return m_did_succeed;
}

terminal_raw_input_guard::terminal_raw_input_guard(descriptor input) wontthrow
    : m_input(input)
{
  if (!is_fd_a_tty(input)) return;
  if (tcgetattr(input, &m_original_mode) != 0) return;

  let raw_mode = m_original_mode;
  raw_mode.c_lflag &= static_cast<tcflag_t>(~(ECHO | ICANON));
  raw_mode.c_cc[VMIN] = 1;
  raw_mode.c_cc[VTIME] = 0;
  if (tcsetattr(input, TCSANOW, &raw_mode) != 0) return;

  m_should_restore = true;
}

terminal_raw_input_guard::~terminal_raw_input_guard()
{
  if (m_should_restore) unused(tcsetattr(m_input, TCSANOW, &m_original_mode));
}

pure fn terminal_raw_input_guard::is_active() const wontthrow -> bool
{
  return m_should_restore;
}

fn allocate_aligned(usize length, usize alignment) wontthrow -> opaque *
{
  return ::aligned_alloc(alignment, length);
}

fn free_aligned(opaque *pointer) wontthrow -> void { std::free(pointer); }

fn collate_compare(const String &left, const String &right) wontthrow -> int
{
  static const int did_bind_collate = (setlocale(LC_COLLATE, ""), 0);
  unused(did_bind_collate);
  return strcoll(left.c_str(), right.c_str());
}

static fn get_unicode_locale() wontthrow -> locale_t
{
  static const locale_t unicode_locale = [] {
    locale_t created = newlocale(LC_CTYPE_MASK, "C.UTF-8", nullptr);
    if (created == nullptr)
      created = newlocale(LC_CTYPE_MASK, "en_US.UTF-8", nullptr);
    return created;
  }();

  return unicode_locale;
}

fn code_point_to_upper(u32 code_point) wontthrow -> u32
{
  let const unicode_locale = get_unicode_locale();
  if (unicode_locale == nullptr) return code_point;

  return static_cast<u32>(
      towupper_l(static_cast<wint_t>(code_point), unicode_locale));
}

fn code_point_to_lower(u32 code_point) wontthrow -> u32
{
  let const unicode_locale = get_unicode_locale();
  if (unicode_locale == nullptr) return code_point;

  return static_cast<u32>(
      towlower_l(static_cast<wint_t>(code_point), unicode_locale));
}

fn locale_is_available(StringView locale_name) wontthrow -> bool
{
  char name[64];
  if (locale_name.length >= sizeof(name)) return false;

  std::memcpy(name, locale_name.data, locale_name.length);
  name[locale_name.length] = '\0';

  let const created = newlocale(LC_CTYPE_MASK, name, nullptr);
  if (created == nullptr) return false;

  freelocale(created);

  return true;
}

fn code_point_is_in_class(StringView class_name, u32 code_point) wontthrow
    -> bool
{
  let const unicode_locale = get_unicode_locale();

  char name[16];
  if (unicode_locale == nullptr || class_name.length >= sizeof(name)) {
    return false;
  }

  std::memcpy(name, class_name.data, class_name.length);
  name[class_name.length] = '\0';

  let const kind = wctype_l(name, unicode_locale);
  return kind != 0 &&
         iswctype_l(static_cast<wint_t>(code_point), kind, unicode_locale) != 0;
}

regex_utf8_scope::regex_utf8_scope(bool is_enabled) wontthrow
{
  if (!is_enabled) return;

  let const regex_locale = get_unicode_locale();
  if (regex_locale == nullptr) return;

  let const previous = uselocale(regex_locale);
  if (previous == nullptr) return;

  m_previous = previous;
  m_is_active = true;
}

regex_utf8_scope::~regex_utf8_scope()
{
  if (m_is_active) uselocale(static_cast<locale_t>(m_previous));
}

struct numeric_locale_entry
{
  char name[64];
  u8 name_length;
  bool has_dot_decimal_point;
  locale_t numeric_locale;
};

static fn locale_has_dot_decimal_point(locale_t numeric_locale) wontthrow
    -> bool
{
  let const previous = uselocale(numeric_locale);
  if (previous == nullptr) return false;

  char half[8];
  let const length = std::snprintf(half, sizeof(half), "%.1f", 0.5);
  uselocale(previous);

  return length == 3 && half[1] == '.';
}

static fn get_numeric_locale(StringView locale_name) wontthrow
    -> const numeric_locale_entry *
{
  constexpr usize CACHE_ENTRY_COUNT = 4;
  static thread_local numeric_locale_entry cache[CACHE_ENTRY_COUNT]{};
  static thread_local usize next_slot = 0;

  if (locale_name.length >= sizeof(cache[0].name)) return nullptr;

  for (let const &entry : cache) {
    if (entry.name_length == locale_name.length &&
        std::memcmp(entry.name, locale_name.data, locale_name.length) == 0)
    {
      return &entry;
    }
  }

  let &slot = cache[next_slot];
  next_slot = (next_slot + 1) % CACHE_ENTRY_COUNT;
  if (slot.numeric_locale != nullptr) freelocale(slot.numeric_locale);

  std::memcpy(slot.name, locale_name.data, locale_name.length);
  slot.name[locale_name.length] = '\0';
  slot.name_length = static_cast<u8>(locale_name.length);

#if defined KOSH_HAS_ADDRESS_SANITIZER
  __lsan_disable();
#endif
  slot.numeric_locale = newlocale(LC_NUMERIC_MASK, slot.name, nullptr);
#if defined KOSH_HAS_ADDRESS_SANITIZER
  __lsan_enable();
#endif
  slot.has_dot_decimal_point =
      slot.numeric_locale == nullptr ||
      locale_has_dot_decimal_point(slot.numeric_locale);

  return &slot;
}

fn numeric_locale_scope::activate(StringView locale_name,
                                  bool is_grouping) wontthrow -> void
{
  let const entry = get_numeric_locale(locale_name);
  if (entry == nullptr || entry->numeric_locale == nullptr) {
    return;
  }

  if (entry->has_dot_decimal_point && !is_grouping) {
    return;
  }

  let const previous = uselocale(entry->numeric_locale);
  if (previous == nullptr) return;

  m_previous = previous;
  m_is_active = true;
}

fn numeric_locale_scope::deactivate() wontthrow -> void
{
  uselocale(static_cast<locale_t>(m_previous));
}

fn read_process_cpu_times() wontthrow -> cpu_times
{
  cpu_times result{};
  struct tms accounting{};
  if (times(&accounting) != static_cast<clock_t>(-1)) {
    let const ticks = static_cast<double>(sysconf(_SC_CLK_TCK));
    if (ticks > 0) {
      result.self_user_seconds =
          static_cast<double>(accounting.tms_utime) / ticks;
      result.self_system_seconds =
          static_cast<double>(accounting.tms_stime) / ticks;
      result.child_user_seconds =
          static_cast<double>(accounting.tms_cutime) / ticks;
      result.child_system_seconds =
          static_cast<double>(accounting.tms_cstime) / ticks;
    }
  }
  return result;
}

static fn rlimit_resource_of(resource_kind kind) wontthrow -> Maybe<int>
{
  switch (kind) {
  case resource_kind::CpuSeconds: return RLIMIT_CPU;
  case resource_kind::FileBlocks: return RLIMIT_FSIZE;
  case resource_kind::DataKbytes: return RLIMIT_DATA;
  case resource_kind::StackKbytes: return RLIMIT_STACK;
  case resource_kind::CoreBlocks: return RLIMIT_CORE;
  case resource_kind::OpenFiles: return RLIMIT_NOFILE;
#ifdef RLIMIT_RSS
  case resource_kind::ResidentKbytes: return RLIMIT_RSS;
#endif
#ifdef RLIMIT_MEMLOCK
  case resource_kind::LockedMemoryKbytes: return RLIMIT_MEMLOCK;
#endif
#ifdef RLIMIT_NPROC
  case resource_kind::Processes: return RLIMIT_NPROC;
#endif
#ifdef RLIMIT_AS
  case resource_kind::VirtualMemoryKbytes: return RLIMIT_AS;
#endif
#ifdef RLIMIT_LOCKS
  case resource_kind::FileLocks: return RLIMIT_LOCKS;
#endif
#ifdef RLIMIT_RTPRIO
  case resource_kind::RealtimePriority: return RLIMIT_RTPRIO;
#endif
  default: return koshka::None;
  }
}

fn get_resource_limit(resource_kind kind) wontthrow -> Maybe<resource_limit>
{
  let const which = rlimit_resource_of(kind);
  if (!which.has_value()) return None;

  struct rlimit limit{};
  if (getrlimit(*which, &limit) != 0) return None;

  resource_limit result{};
  result.soft = limit.rlim_cur == RLIM_INFINITY
                    ? RESOURCE_UNLIMITED
                    : static_cast<u64>(limit.rlim_cur);
  result.hard = limit.rlim_max == RLIM_INFINITY
                    ? RESOURCE_UNLIMITED
                    : static_cast<u64>(limit.rlim_max);
  return result;
}

fn set_resource_limit(const resource_limit &limit, resource_kind kind) wontthrow
    -> bool
{
  let const which = rlimit_resource_of(kind);
  if (!which.has_value()) return false;

  struct rlimit target{};
  target.rlim_cur = limit.soft == RESOURCE_UNLIMITED
                        ? RLIM_INFINITY
                        : static_cast<rlim_t>(limit.soft);
  target.rlim_max = limit.hard == RESOURCE_UNLIMITED
                        ? RLIM_INFINITY
                        : static_cast<rlim_t>(limit.hard);
  return setrlimit(*which, &target) == 0;
}

fn shell_fd_is_a_tty(int shell_fd) wontthrow -> bool
{
  return is_fd_a_tty(static_cast<descriptor>(shell_fd));
}

pure fn is_directory_separator(char c) wontthrow -> bool { return c == '/'; }

fn get_terminal_dimensions(descriptor output) wontthrow
    -> Maybe<terminal_dimensions>
{
  LOG(Debug, "querying the terminal size");
  struct winsize window{};
  if (ioctl(output, TIOCGWINSZ, &window) != 0) return None;
  if (window.ws_col == 0 || window.ws_row == 0) return None;
  return terminal_dimensions{window.ws_col, window.ws_row};
}

static pure fn terminal_speed_number(speed_t speed) wontthrow -> u32
{
  switch (speed) {
  case B0: return 0;
  case B50: return 50;
  case B75: return 75;
  case B110: return 110;
  case B134: return 134;
  case B150: return 150;
  case B200: return 200;
  case B300: return 300;
  case B600: return 600;
  case B1200: return 1200;
  case B1800: return 1800;
  case B2400: return 2400;
  case B4800: return 4800;
  case B9600: return 9600;
  case B19200: return 19200;
  case B38400: return 38400;
#ifdef B57600
  case B57600: return 57600;
#endif
#ifdef B115200
  case B115200: return 115200;
#endif
  default: return 0;
  }
}

static pure fn terminal_speed_value(StringView text) wontthrow -> speed_t
{
  static constexpr static_string_entry<speed_t> SPEED_ENTRIES[] = {
      {SSK("0"),      B0     },
      {SSK("50"),     B50    },
      {SSK("75"),     B75    },
      {SSK("110"),    B110   },
      {SSK("134"),    B134   },
      {SSK("150"),    B150   },
      {SSK("200"),    B200   },
      {SSK("300"),    B300   },
      {SSK("600"),    B600   },
      {SSK("1200"),   B1200  },
      {SSK("1800"),   B1800  },
      {SSK("2400"),   B2400  },
      {SSK("4800"),   B4800  },
      {SSK("9600"),   B9600  },
      {SSK("19200"),  B19200 },
      {SSK("38400"),  B38400 },
#ifdef B57600
      {SSK("57600"),  B57600 },
#endif
#ifdef B115200
      {SSK("115200"), B115200},
#endif
  };
  static constexpr StaticStringMap SPEEDS{SPEED_ENTRIES};
  return SPEEDS.find(text).value_or(static_cast<speed_t>(~speed_t{0}));
}

struct terminal_flag
{
  const char *name;
  tcflag_t value;
  tcflag_t termios::*member;
};

static constexpr static_string_entry<terminal_flag> TERMINAL_FLAG_ENTRIES[] = {
    {SSK("echo"),   {"echo", ECHO, &termios::c_lflag}    },
    {SSK("igncr"),  {"igncr", IGNCR, &termios::c_iflag}  },
    {SSK("opost"),  {"opost", OPOST, &termios::c_oflag}  },
    {SSK("tostop"), {"tostop", TOSTOP, &termios::c_lflag}},
    {SSK("icanon"), {"icanon", ICANON, &termios::c_lflag}},
    {SSK("isig"),   {"isig", ISIG, &termios::c_lflag}    },
    {SSK("iexten"), {"iexten", IEXTEN, &termios::c_lflag}},
    {SSK("echoe"),  {"echoe", ECHOE, &termios::c_lflag}  },
    {SSK("echok"),  {"echok", ECHOK, &termios::c_lflag}  },
    {SSK("echonl"), {"echonl", ECHONL, &termios::c_lflag}},
    {SSK("noflsh"), {"noflsh", NOFLSH, &termios::c_lflag}},
    {SSK("ignbrk"), {"ignbrk", IGNBRK, &termios::c_iflag}},
    {SSK("brkint"), {"brkint", BRKINT, &termios::c_iflag}},
    {SSK("ignpar"), {"ignpar", IGNPAR, &termios::c_iflag}},
    {SSK("parmrk"), {"parmrk", PARMRK, &termios::c_iflag}},
    {SSK("inpck"),  {"inpck", INPCK, &termios::c_iflag}  },
    {SSK("istrip"), {"istrip", ISTRIP, &termios::c_iflag}},
    {SSK("inlcr"),  {"inlcr", INLCR, &termios::c_iflag}  },
    {SSK("icrnl"),  {"icrnl", ICRNL, &termios::c_iflag}  },
    {SSK("ixon"),   {"ixon", IXON, &termios::c_iflag}    },
    {SSK("ixoff"),  {"ixoff", IXOFF, &termios::c_iflag}  },
    {SSK("cstopb"), {"cstopb", CSTOPB, &termios::c_cflag}},
    {SSK("cread"),  {"cread", CREAD, &termios::c_cflag}  },
    {SSK("parenb"), {"parenb", PARENB, &termios::c_cflag}},
    {SSK("parodd"), {"parodd", PARODD, &termios::c_cflag}},
    {SSK("hupcl"),  {"hupcl", HUPCL, &termios::c_cflag}  },
    {SSK("clocal"), {"clocal", CLOCAL, &termios::c_cflag}},
#ifdef ONLCR
    {SSK("onlcr"),  {"onlcr", ONLCR, &termios::c_oflag}  },
#endif
};
static constexpr StaticStringMap TERMINAL_FLAGS{TERMINAL_FLAG_ENTRIES};

struct terminal_control_character
{
  const char *name;
  usize index;
};

static constexpr static_string_entry<terminal_control_character>
    TERMINAL_CONTROL_CHARACTER_ENTRIES[] = {
        {SSK("eof"),   {"eof", VEOF}    },
        {SSK("eol"),   {"eol", VEOL}    },
        {SSK("erase"), {"erase", VERASE}},
        {SSK("intr"),  {"intr", VINTR}  },
        {SSK("kill"),  {"kill", VKILL}  },
        {SSK("quit"),  {"quit", VQUIT}  },
        {SSK("start"), {"start", VSTART}},
        {SSK("stop"),  {"stop", VSTOP}  },
#ifdef VSUSP
        {SSK("susp"),  {"susp", VSUSP}  },
#endif
};
static constexpr StaticStringMap TERMINAL_CONTROL_CHARACTERS{
    TERMINAL_CONTROL_CHARACTER_ENTRIES};

enum class terminal_setting_kind : uchar
{
  CharacterSize,
  Raw,
  Sane,
  EraseKill,
  Newline,
  EvenParity,
  OddParity,
  Rows,
  Columns,
  Minimum,
  Time,
};

static constexpr static_string_entry<terminal_setting_kind>
    TERMINAL_SETTING_ENTRIES[] = {
        {SSK("cs5"),     terminal_setting_kind::CharacterSize},
        {SSK("cs6"),     terminal_setting_kind::CharacterSize},
        {SSK("cs7"),     terminal_setting_kind::CharacterSize},
        {SSK("cs8"),     terminal_setting_kind::CharacterSize},
        {SSK("raw"),     terminal_setting_kind::Raw          },
        {SSK("sane"),    terminal_setting_kind::Sane         },
        {SSK("ek"),      terminal_setting_kind::EraseKill    },
        {SSK("nl"),      terminal_setting_kind::Newline      },
        {SSK("evenp"),   terminal_setting_kind::EvenParity   },
        {SSK("parity"),  terminal_setting_kind::EvenParity   },
        {SSK("oddp"),    terminal_setting_kind::OddParity    },
        {SSK("rows"),    terminal_setting_kind::Rows         },
        {SSK("cols"),    terminal_setting_kind::Columns      },
        {SSK("columns"), terminal_setting_kind::Columns      },
        {SSK("min"),     terminal_setting_kind::Minimum      },
        {SSK("time"),    terminal_setting_kind::Time         },
};
static constexpr StaticStringMap TERMINAL_SETTINGS{TERMINAL_SETTING_ENTRIES};

static fn append_terminal_character(String &output, cc_t value) throws -> void
{
  if (value == _POSIX_VDISABLE) {
    output += "undef";
  } else if (value == 127) {
    output += "^?";
  } else if (value < 32) {
    output += '^';
    output += static_cast<char>(value + '@');
  } else {
    output += static_cast<char>(value);
  }
}

fn terminal_settings(descriptor terminal, Allocator allocator,
                     terminal_settings_output_mode mode) throws -> Maybe<String>
{
  termios state{};
  if (tcgetattr(terminal, &state) != 0) return None;
  let output = String{allocator};
  if (mode == terminal_settings_output_mode::Encoded) {
    bool is_first_field = true;
    let do_append_field = [&](u64 value) throws {
      if (!is_first_field) output += ':';
      output += String::from_in_base(value, false, int_base::hex, allocator);
      is_first_field = false;
    };
    do_append_field(state.c_iflag);
    do_append_field(state.c_oflag);
    do_append_field(state.c_cflag);
    do_append_field(state.c_lflag);
    do_append_field(cfgetispeed(&state));
    do_append_field(cfgetospeed(&state));
    for (usize index = 0; index < NCCS; index++)
      do_append_field(state.c_cc[index]);
    output += '\n';
    return output;
  }
  output += "speed ";
  output += String::from(terminal_speed_number(cfgetospeed(&state)), allocator);
  output += " baud; ";
  if (let const dimensions = get_terminal_dimensions(terminal)) {
    output += "rows ";
    output += String::from(dimensions->rows, allocator);
    output += "; columns ";
    output += String::from(dimensions->columns, allocator);
    output += "; ";
  }
  for (let const &entry : TERMINAL_FLAG_ENTRIES) {
    let const &flag = entry.value;
    if ((state.*(flag.member) & flag.value) == 0) output += '-';
    output += flag.name;
    output += ' ';
  }
  if (mode == terminal_settings_output_mode::All) {
    for (let const &entry : TERMINAL_CONTROL_CHARACTER_ENTRIES) {
      let const &character = entry.value;
      output += character.name;
      output += " = ";
      append_terminal_character(output, state.c_cc[character.index]);
      output += "; ";
    }
  }
  output += '\n';
  return output;
}

static fn restore_encoded_terminal_settings(termios &state,
                                            StringView encoded) wontthrow
    -> bool
{
  usize position = 0;
  let do_read_field = [&](u64 &value) wontthrow -> bool {
    if (position > encoded.length) return false;
    usize end = position;
    while (end < encoded.length && encoded[end] != ':')
      end++;
    if (end == position) return false;
    let const parsed = utils::parse_integer_in_base_u64(
        encoded.substring_of_length(position, end - position), int_base::hex);
    if (parsed.is_error()) return false;
    value = parsed.value();
    position = end < encoded.length ? end + 1 : encoded.length + 1;
    return true;
  };

  u64 input_flags = 0;
  u64 output_flags = 0;
  u64 control_flags = 0;
  u64 local_flags = 0;
  u64 input_speed = 0;
  u64 output_speed = 0;
  if (!do_read_field(input_flags) || !do_read_field(output_flags) ||
      !do_read_field(control_flags) || !do_read_field(local_flags) ||
      !do_read_field(input_speed) || !do_read_field(output_speed))
  {
    return false;
  }

  if (input_flags > std::numeric_limits<tcflag_t>::max() ||
      output_flags > std::numeric_limits<tcflag_t>::max() ||
      control_flags > std::numeric_limits<tcflag_t>::max() ||
      local_flags > std::numeric_limits<tcflag_t>::max() ||
      input_speed > std::numeric_limits<speed_t>::max() ||
      output_speed > std::numeric_limits<speed_t>::max())
  {
    return false;
  }

  state.c_iflag = static_cast<tcflag_t>(input_flags);
  state.c_oflag = static_cast<tcflag_t>(output_flags);
  state.c_cflag = static_cast<tcflag_t>(control_flags);
  state.c_lflag = static_cast<tcflag_t>(local_flags);
  if (cfsetispeed(&state, static_cast<speed_t>(input_speed)) != 0 ||
      cfsetospeed(&state, static_cast<speed_t>(output_speed)) != 0)
  {
    return false;
  }

  for (usize index = 0; index < NCCS; index++) {
    u64 value = 0;
    if (!do_read_field(value) || value > UCHAR_MAX) return false;
    state.c_cc[index] = static_cast<cc_t>(value);
  }

  return position == encoded.length + 1;
}

static fn parse_terminal_character(StringView text, cc_t &value) wontthrow
    -> bool
{
  if (text == "undef" || text == "^-") {
    value = _POSIX_VDISABLE;
    return true;
  }
  if (text == "^?") {
    value = 127;
    return true;
  }
  if (text.length == 2 && text[0] == '^') {
    value = static_cast<cc_t>(text[1] & 31);
    return true;
  }
  if (text.length != 1) return false;
  value = static_cast<cc_t>(text[0]);
  return true;
}

fn apply_terminal_settings(descriptor terminal,
                           const ArrayList<String> &settings) wontthrow
    -> terminal_settings_apply_result
{
  termios state{};
  if (tcgetattr(terminal, &state) != 0)
    return {terminal_settings_apply_kind::SystemError, 0};
  if (settings.count() == 1 &&
      settings[0].view().find_character(':').has_value())
  {
    if (!restore_encoded_terminal_settings(state, settings[0].view()))
      return {terminal_settings_apply_kind::InvalidSetting, 0};
    if (tcsetattr(terminal, TCSADRAIN, &state) != 0)
      return {terminal_settings_apply_kind::SystemError, 0};
    return {terminal_settings_apply_kind::Success, 0};
  }

  winsize window{};
  let has_window = ioctl(terminal, TIOCGWINSZ, &window) == 0;
  for (usize index = 0; index < settings.count(); index++) {
    let const setting = settings[index].view();
    let const is_disabled = setting.length > 1 && setting[0] == '-';
    let const name = is_disabled ? setting.substring(1) : setting;
    if (let const selected_flag = TERMINAL_FLAGS.find(name);
        selected_flag.has_value())
    {
      if (is_disabled)
        state.*(selected_flag->member) &= ~selected_flag->value;
      else
        state.*(selected_flag->member) |= selected_flag->value;
      continue;
    }

    if (let const selected_character = TERMINAL_CONTROL_CHARACTERS.find(name);
        selected_character.has_value())
    {
      if (is_disabled || ++index == settings.count()) {
        return {terminal_settings_apply_kind::InvalidSetting, index - 1};
      }

      cc_t value = 0;
      if (!parse_terminal_character(settings[index].view(), value))
        return {terminal_settings_apply_kind::InvalidSetting, index};
      state.c_cc[selected_character->index] = value;
      continue;
    }

    let const selected_setting = TERMINAL_SETTINGS.find(name);
    if (!selected_setting.has_value()) {
      let const speed = terminal_speed_value(name);
      if (speed == static_cast<speed_t>(~speed_t{0}) || is_disabled) {
        return {terminal_settings_apply_kind::InvalidSetting, index};
      }

      if (cfsetispeed(&state, speed) != 0 || cfsetospeed(&state, speed) != 0) {
        return {terminal_settings_apply_kind::InvalidSetting, index};
      }

      continue;
    }

    switch (*selected_setting) {
    case terminal_setting_kind::CharacterSize: {
      if (is_disabled)
        return {terminal_settings_apply_kind::InvalidSetting, index};
      static constexpr tcflag_t CHARACTER_SIZES[] = {CS5, CS6, CS7, CS8};
      state.c_cflag = (state.c_cflag & ~CSIZE) | CHARACTER_SIZES[name[2] - '5'];
      continue;
    }
    case terminal_setting_kind::Raw:
      if (!is_disabled) {
        cfmakeraw(&state);
      } else {
        state.c_lflag |= ICANON | ISIG | ECHO;
        state.c_iflag |= ICRNL;
        state.c_oflag |= OPOST;
      }
      continue;
    case terminal_setting_kind::Sane:
      if (is_disabled)
        return {terminal_settings_apply_kind::InvalidSetting, index};
      state.c_lflag |= ICANON | ISIG | ECHO | IEXTEN;
      state.c_lflag &= ~(ECHONL | NOFLSH | TOSTOP);
      state.c_iflag |= BRKINT | ICRNL | IXON;
      state.c_iflag &= ~(IGNBRK | IGNCR | INLCR | IXOFF);
      state.c_oflag |= OPOST;
      continue;
    case terminal_setting_kind::EraseKill:
      if (is_disabled)
        return {terminal_settings_apply_kind::InvalidSetting, index};
      state.c_cc[VERASE] = '\177';
      state.c_cc[VKILL] = '\025';
      continue;
    case terminal_setting_kind::Newline:
      if (is_disabled)
        state.c_iflag &= ~ICRNL;
      else
        state.c_iflag |= ICRNL;
      continue;
    case terminal_setting_kind::EvenParity:
      state.c_cflag &= ~PARODD;
      if (is_disabled) {
        state.c_cflag &= ~PARENB;
        state.c_cflag = (state.c_cflag & ~CSIZE) | CS8;
      } else {
        state.c_cflag |= PARENB;
        state.c_cflag = (state.c_cflag & ~CSIZE) | CS7;
      }
      continue;
    case terminal_setting_kind::OddParity:
      if (is_disabled) {
        state.c_cflag &= ~(PARENB | PARODD);
        state.c_cflag = (state.c_cflag & ~CSIZE) | CS8;
      } else {
        state.c_cflag |= PARENB | PARODD;
        state.c_cflag = (state.c_cflag & ~CSIZE) | CS7;
      }
      continue;
    case terminal_setting_kind::Rows:
    case terminal_setting_kind::Columns: {
      if (++index == settings.count() || !has_window) {
        return {terminal_settings_apply_kind::InvalidSetting, index - 1};
      }

      let const parsed = utils::parse_decimal_u64(settings[index].view());
      if (parsed.is_error() || parsed.value() > UINT16_MAX) {
        return {terminal_settings_apply_kind::InvalidSetting, index};
      }

      if (*selected_setting == terminal_setting_kind::Rows) {
        window.ws_row = static_cast<u16>(parsed.value());
      } else {
        window.ws_col = static_cast<u16>(parsed.value());
      }

      continue;
    }
    case terminal_setting_kind::Minimum:
    case terminal_setting_kind::Time: {
      if (++index == settings.count())
        return {terminal_settings_apply_kind::InvalidSetting, index - 1};
      let const parsed = utils::parse_decimal_u64(settings[index].view());
      if (parsed.is_error() || parsed.value() > UCHAR_MAX) {
        return {terminal_settings_apply_kind::InvalidSetting, index};
      }

      state.c_cc[*selected_setting == terminal_setting_kind::Minimum ? VMIN
                                                                     : VTIME] =
          static_cast<cc_t>(parsed.value());
      continue;
    }
    }
  }

  if (tcsetattr(terminal, TCSADRAIN, &state) != 0)
    return {terminal_settings_apply_kind::SystemError, 0};

  if (has_window && ioctl(terminal, TIOCSWINSZ, &window) != 0) {
    return {terminal_settings_apply_kind::SystemError, 0};
  }

  return {terminal_settings_apply_kind::Success, 0};
}

static fn make_fd_inheritable(descriptor fd) wontthrow -> void
{
  const int flags = fcntl(fd, F_GETFD);
  if (flags != -1) fcntl(fd, F_SETFD, flags & ~FD_CLOEXEC);
}

#if KOSH_PLATFORM_ISNT KOSH_PLATFORM_COSMO
const ProgramSuffixList PROGRAM_SUFFIXES{POSIX_PROGRAM_SUFFIXES};

fn normalize_program_name(String &program_name) throws -> program_name_info
{
  return {program_extension::None, program_name.length()};
}
#endif

fn has_environment_variable(StringView key) throws -> bool
{
  const String key_string{key};
  return std::getenv(key_string.c_str()) != nullptr;
}

fn get_environment_variable(StringView key) throws -> Maybe<String>
{
  LOG(All, "reading the environment variable '%.*s'",
      static_cast<int>(key.length), key.data);
  const String key_string{key};
  const char *e = std::getenv(key_string.c_str());
  if (e != nullptr) return String{e};
  return koshka::None;
}

fn set_environment_variable(StringView key, StringView value) throws -> void
{
  LOG(All, "setting the environment variable '%.*s'",
      static_cast<int>(key.length), key.data);
  const String key_string{key};
  const String value_string{value};
  setenv(key_string.c_str(), value_string.c_str(), 1);
  ENVIRONMENT_EPOCH++;
}

fn unset_environment_variable(StringView key) throws -> void
{
  LOG(All, "unsetting the environment variable '%.*s'",
      static_cast<int>(key.length), key.data);
  const String key_string{key};
  unsetenv(key_string.c_str());
  ENVIRONMENT_EPOCH++;
}

fn get_environment_spelling(StringView key) throws -> String
{
  return String{key};
}

fn signal_internal_diagnostic() wontthrow -> void {}

fn for_each_environment_name(opaque *context,
                             environment_name_callback callback) throws -> void
{
  if (environ == nullptr) return;
  for (char **entry = environ; *entry != nullptr; entry++) {
    StringView pair{*entry};
    let const equals = pair.find_character('=');
    let const name =
        equals.has_value() ? pair.substring_of_length(0, *equals) : pair;
    callback(context, name);
  }
}

fn environment_names() throws -> ArrayList<String>
{
  ArrayList<String> names{heap_allocator()};
  for_each_environment_name(&names, [](opaque *context, StringView name) {
    static_cast<ArrayList<String> *>(context)->push(String{name});
  });
  return names;
}

fn check_syscall_impl(i32 status, StringView invocation) throws -> i32
{
  if (status == -1) {
    throw koshka::Error{"'" + invocation +
                        "' fail: " + last_system_error_message()};
  }

  return status;
}

#define check_syscall(call) check_syscall_impl(call, #call)

cold fn last_system_error_message() throws -> String
{
  return String{strerror(errno)};
}

fn get_last_system_error_number() wontthrow -> i32 { return errno; }

fn last_system_error_is_missing_file() wontthrow -> bool
{
  return errno == ENOENT;
}

fn last_system_error_is_permission_denied() wontthrow -> bool
{
  return errno == EACCES || errno == EPERM;
}

fn last_system_error_is_descriptor_quota() wontthrow -> bool
{
  return errno == EMFILE || errno == ENFILE;
}

fn set_last_system_error(i32 error_number) wontthrow -> void
{
  errno = error_number;
}

static fn make_sigset_impl(int first, ...) wontthrow -> sigset_t
{
  va_list va;

  sigset_t sm;
  sigemptyset(&sm);

  va_start(va, first);
  for (int sig = first; sig != -1; sig = va_arg(va, int))
    sigaddset(&sm, sig);
  va_end(va);

  return sm;
}

#define make_sigset(...) make_sigset_impl(__VA_ARGS__, -1)

static fn sigchild_handler(int signal_number, siginfo_t *siginfo,
                           opaque *context) wontthrow -> void
{
  unused(context);
  unused(siginfo);
  CHILD_STATE_CHANGED = 1;

  if (CHILD_TRAP_ARMED == 0) return;

  if (is_trappable_signal(signal_number))
    PENDING_SIGNAL_FLAGS[signal_number] = 1;
  SIGNAL_PENDING = 1;
}

static fn install_child_state_handler() throws -> void
{
  struct sigaction action = {};
  check_syscall(sigemptyset(&action.sa_mask));
  action.sa_flags = SA_SIGINFO;
  action.sa_sigaction = sigchild_handler;
  check_syscall(sigaction(SIGCHLD, &action, nullptr));
}

static fn reset_signal_handlers() throws -> void
{
  LOG(Debug, "restoring signal dispositions for a child process");

  sigset_t sm;
  sigfillset(&sm);
  check_syscall(sigprocmask(SIG_UNBLOCK, &sm, nullptr));
  sigemptyset(&SIGNALS_UNBLOCKED_BY_TRAP);

  struct sigaction sa = {};
  sa.sa_handler = SIG_DFL;
  CHILD_TRAP_ARMED = 0;
  install_child_state_handler();
  check_syscall(sigaction(SIGINT, &sa, nullptr));

  for (i32 signal_number = 1; signal_number < SIGNAL_FLAG_COUNT;
       signal_number++)
  {
    if (sigismember(&SIGNALS_WITH_TRAP_ACTION, signal_number) != 1) continue;

    LOG(Debug, "restoring the default action for trapped signal %d",
        signal_number);
    (void) sigaction(signal_number, &sa, nullptr);
  }

  sigemptyset(&SIGNALS_WITH_TRAP_ACTION);

  if (IS_PIPE_SIGNAL_IGNORED_BY_TRAP || WAS_PIPE_SIGNAL_IGNORED_AT_ENTRY) {
    struct sigaction ignore = {};
    ignore.sa_handler = SIG_IGN;
    check_syscall(sigaction(SIGPIPE, &ignore, nullptr));
  } else {
    check_syscall(sigaction(SIGPIPE, &sa, nullptr));
  }

  INTERRUPT_REQUESTED = 0;
}

static fn handle_interrupt(int s) wontthrow -> void
{
  unused(s);
  INTERRUPT_REQUESTED = 1;
}

static u64 ENTRY_IGNORED_SIGNALS = 0;

static fn capture_entry_ignored_signals() wontthrow -> void
{
  for (i32 signal_number = 1; signal_number <= ENTRY_IGNORED_SIGNAL_LIMIT;
       signal_number++)
  {
    if (signal_number == SIGKILL || signal_number == SIGSTOP) continue;

    struct sigaction sa = {};
    if (sigaction(signal_number, nullptr, &sa) != 0) continue;
    if (sa.sa_handler != SIG_IGN) continue;

    LOG(Info, "signal %d is already ignored at shell entry", signal_number);
    ENTRY_IGNORED_SIGNALS |= u64{1} << (signal_number - 1);
    if (signal_number == SIGPIPE) WAS_PIPE_SIGNAL_IGNORED_AT_ENTRY = true;
  }
}

fn get_entry_ignored_signals() wontthrow -> u64
{
  return ENTRY_IGNORED_SIGNALS;
}

fn set_default_signal_handlers(signal_profile profile) throws -> void
{
  let const is_interactive = profile == signal_profile::Interactive;
  LOG(Info, "installing the shell signal handlers, interactive %d",
      is_interactive ? 1 : 0);

  if (is_interactive) {
    sigset_t sm = make_sigset(SIGTERM, SIGQUIT, SIGSTOP, SIGTSTP);
    check_syscall(sigprocmask(SIG_BLOCK, &sm, nullptr));
  }

  install_child_state_handler();

  if ((ENTRY_IGNORED_SIGNALS & (u64{1} << (SIGINT - 1))) == 0) {
    struct sigaction si = {};
    si.sa_handler = handle_interrupt;
    check_syscall(sigaction(SIGINT, &si, nullptr));
  }

  struct sigaction sp = {};
  sp.sa_handler = SIG_IGN;
  check_syscall(sigaction(SIGPIPE, &sp, nullptr));
}

static constexpr int FATAL_SIGNALS[] = {SIGABRT, SIGBUS,  SIGFPE,
                                        SIGILL,  SIGSEGV, SIGTRAP};
static struct sigaction FATAL_PREVIOUS_ACTIONS[countof(FATAL_SIGNALS)];
static void (*FATAL_EXIT_HOOK)() = nullptr;
static pid_t FATAL_EXIT_HOOK_OWNER = 0;

static fn run_fatal_exit_hook() wontthrow -> void
{
  if (FATAL_EXIT_HOOK != nullptr && getpid() == FATAL_EXIT_HOOK_OWNER) {
    FATAL_EXIT_HOOK();
  }
}

static fn handle_fatal_signal(int signal_number, siginfo_t *siginfo,
                              opaque *context) wontthrow -> void
{
  unused(context);
  run_fatal_exit_hook();

  for (usize i = 0; i < countof(FATAL_SIGNALS); i++) {
    if (FATAL_SIGNALS[i] == signal_number) {
      (void) sigaction(signal_number, &FATAL_PREVIOUS_ACTIONS[i], nullptr);
    }
  }

  if (signal_number == SIGTRAP || siginfo == nullptr || siginfo->si_code <= 0) {
    (void) raise(signal_number);
  }
}

fn install_fatal_exit_hook(void (*hook)()) throws -> void
{
  let const was_installed = FATAL_EXIT_HOOK != nullptr;
  FATAL_EXIT_HOOK = hook;
  FATAL_EXIT_HOOK_OWNER = getpid();
  if (was_installed) return;

  LOG(Info, "installing the fatal exit hook");
  if (std::atexit(run_fatal_exit_hook) != 0) {
    throw Error{"Could not install the exit hook"};
  }

  struct sigaction action = {};
  check_syscall(sigemptyset(&action.sa_mask));
  action.sa_flags = SA_SIGINFO;
  action.sa_sigaction = handle_fatal_signal;
  for (usize i = 0; i < countof(FATAL_SIGNALS); i++) {
    check_syscall(
        sigaction(FATAL_SIGNALS[i], &action, &FATAL_PREVIOUS_ACTIONS[i]));
  }
}

static fn handle_trapped_signal(int signal_number) wontthrow -> void
{
  if (is_trappable_signal(signal_number))
    PENDING_SIGNAL_FLAGS[signal_number] = 1;
  SIGNAL_PENDING = 1;
}

static fn unblock_signal_for_trap(i32 signal_number) throws -> void
{
  sigset_t wanted;
  sigemptyset(&wanted);
  sigaddset(&wanted, signal_number);

  sigset_t previous;
  sigemptyset(&previous);
  check_syscall(sigprocmask(SIG_UNBLOCK, &wanted, &previous));

  if (sigismember(&previous, signal_number) == 1) {
    LOG(Info, "signal %d was blocked, clearing its trap will block it again",
        signal_number);
    sigaddset(&SIGNALS_UNBLOCKED_BY_TRAP, signal_number);
  }
}

static fn reblock_signal_after_trap(i32 signal_number) throws -> void
{
  if (sigismember(&SIGNALS_UNBLOCKED_BY_TRAP, signal_number) != 1) return;

  LOG(Info, "blocking signal %d again, its trap install had unblocked it",
      signal_number);

  sigset_t wanted;
  sigemptyset(&wanted);
  sigaddset(&wanted, signal_number);
  check_syscall(sigprocmask(SIG_BLOCK, &wanted, nullptr));
  sigdelset(&SIGNALS_UNBLOCKED_BY_TRAP, signal_number);
}

static fn install_signal_disposition(i32 signal_number,
                                     void (*handler)(int)) throws -> void
{
  struct sigaction sa = {};
  check_syscall(sigemptyset(&sa.sa_mask));
  sa.sa_handler = handler;
  check_syscall(sigaction(signal_number, &sa, nullptr));
}

fn set_trap_handler(i32 signal_number) throws -> void
{
  if (!is_trappable_signal(signal_number)) return;

  LOG(Info, "installing the trap handler for signal %d", signal_number);

  if (signal_number == SIGPIPE) IS_PIPE_SIGNAL_IGNORED_BY_TRAP = false;
  if (signal_number == SIGCHLD) {
    install_child_state_handler();
  } else {
    install_signal_disposition(signal_number, handle_trapped_signal);
    sigaddset(&SIGNALS_WITH_TRAP_ACTION, signal_number);
  }

  unblock_signal_for_trap(signal_number);
}

fn set_trap_ignore(i32 signal_number) throws -> void
{
  if (!is_trappable_signal(signal_number)) return;
  LOG(Info, "ignoring signal %d", signal_number);
  if (signal_number == SIGCHLD) {
    install_child_state_handler();
    return;
  }

  install_signal_disposition(signal_number, SIG_IGN);
  sigdelset(&SIGNALS_WITH_TRAP_ACTION, signal_number);
  if (signal_number == SIGPIPE) IS_PIPE_SIGNAL_IGNORED_BY_TRAP = true;
}

fn clear_trap_handler(i32 signal_number) throws -> void
{
  if (!is_trappable_signal(signal_number)) return;
  LOG(Info, "clearing the trap for signal %d", signal_number);
  if (signal_number == SIGPIPE) IS_PIPE_SIGNAL_IGNORED_BY_TRAP = false;

  reblock_signal_after_trap(signal_number);

  if (signal_number == SIGCHLD) {
    install_child_state_handler();
    return;
  }

  install_signal_disposition(
      signal_number, signal_number == SIGINT ? handle_interrupt : SIG_DFL);
  sigdelset(&SIGNALS_WITH_TRAP_ACTION, signal_number);
}

static fn lookup_name_by_id(StringView database_path, u32 wanted_id,
                            usize id_field_index) throws -> Maybe<String>
{
  let const contents = Path{database_path}.read_entire_file();
  if (!contents) return koshka::None;
  let const wanted =
      String::from(static_cast<u64>(wanted_id), heap_allocator());
  let const text = contents->view();
  for (let const &line : utils::split_lines(text)) {
    if (passwd_field(line, id_field_index) != wanted.view()) continue;
    let const name = passwd_field(line, 0);
    if (!name.is_empty()) return String{name};
  }
  return koshka::None;
}

static fn lookup_id_by_name(StringView database_path, StringView wanted_name,
                            usize id_field_index) throws -> Maybe<u32>
{
  let const contents = Path{database_path}.read_entire_file();
  if (!contents) return koshka::None;
  for (let const &line : utils::split_lines(contents->view())) {
    if (passwd_field(line, 0) != wanted_name) continue;
    let const id = passwd_field(line, id_field_index).to<i64>();
    if (!id.is_error() && id.value() >= 0 && id.value() <= UINT32_MAX) {
      return static_cast<u32>(id.value());
    }
  }

  return koshka::None;
}

fn uid_to_username(u32 uid) throws -> Maybe<String>
{
#if defined __APPLE__
  struct passwd entry{};
  struct passwd *result = nullptr;
  char buffer[16384];
  if (getpwuid_r(static_cast<uid_t>(uid), &entry, buffer, sizeof(buffer),
                 &result) != 0 ||
      result == nullptr || result->pw_name == nullptr)
    return None;
  return String{result->pw_name};
#else
  return lookup_name_by_id("/etc/passwd", uid, 2);
#endif
}

fn gid_to_groupname(u32 gid) throws -> Maybe<String>
{
  return lookup_name_by_id("/etc/group", gid, 2);
}

fn username_to_uid(StringView username) throws -> Maybe<u32>
{
  if (let const current = get_current_user();
      current.has_value() && current->view() == username)
    return static_cast<u32>(get_real_user_id());
  return lookup_id_by_name("/etc/passwd", username, 2);
}

fn groupname_to_gid(StringView groupname) throws -> Maybe<u32>
{
  return lookup_id_by_name("/etc/group", groupname, 2);
}

static constexpr int SYSTEM_CONFIGURATION_KEYS[] = {
    _SC_AIO_LISTIO_MAX,
    _SC_AIO_MAX,
    _SC_AIO_PRIO_DELTA_MAX,
    _SC_ARG_MAX,
    _SC_ATEXIT_MAX,
    _SC_BC_BASE_MAX,
    _SC_BC_DIM_MAX,
    _SC_BC_SCALE_MAX,
    _SC_BC_STRING_MAX,
    _SC_CHILD_MAX,
    _SC_CLK_TCK,
    _SC_COLL_WEIGHTS_MAX,
    _SC_DELAYTIMER_MAX,
    _SC_EXPR_NEST_MAX,
    _SC_GETGR_R_SIZE_MAX,
    _SC_NGROUPS_MAX,
    _SC_HOST_NAME_MAX,
    _SC_IOV_MAX,
    _SC_LINE_MAX,
    _SC_LOGIN_NAME_MAX,
    _SC_MQ_OPEN_MAX,
    _SC_MQ_PRIO_MAX,
    _SC_OPEN_MAX,
    _SC_PAGESIZE,
    _SC_GETPW_R_SIZE_MAX,
    _SC_PASS_MAX,
    _SC_PHYS_PAGES,
    _SC_VERSION,
    _SC_NPROCESSORS_CONF,
    _SC_NPROCESSORS_ONLN,
    _SC_RE_DUP_MAX,
    _SC_RTSIG_MAX,
    _SC_SEM_NSEMS_MAX,
    _SC_SEM_VALUE_MAX,
    _SC_SIGQUEUE_MAX,
    _SC_STREAM_MAX,
    _SC_SYMLOOP_MAX,
    _SC_THREAD_THREADS_MAX,
    _SC_THREAD_DESTRUCTOR_ITERATIONS,
    _SC_THREAD_KEYS_MAX,
    _SC_THREAD_STACK_MIN,
    _SC_TIMER_MAX,
    _SC_TTY_NAME_MAX,
    _SC_TZNAME_MAX,
    _SC_ADVISORY_INFO,
    _SC_ASYNCHRONOUS_IO,
    _SC_BARRIERS,
    _SC_CLOCK_SELECTION,
    _SC_CPUTIME,
#ifdef _SC_DEVICE_CONTROL
    _SC_DEVICE_CONTROL,
#else
    -1,
#endif
    _SC_FSYNC,
    _SC_IPV6,
    _SC_JOB_CONTROL,
    _SC_MAPPED_FILES,
    _SC_MEMLOCK,
    _SC_MEMLOCK_RANGE,
    _SC_MEMORY_PROTECTION,
    _SC_MESSAGE_PASSING,
    _SC_MONOTONIC_CLOCK,
    _SC_2_C_BIND,
    _SC_2_C_DEV,
    _SC_2_CHAR_TERM,
    _SC_2_FORT_RUN,
    _SC_2_LOCALEDEF,
    _SC_2_SW_DEV,
    _SC_2_UPE,
    _SC_2_VERSION,
    _SC_PRIORITIZED_IO,
    _SC_PRIORITY_SCHEDULING,
    _SC_RAW_SOCKETS,
    _SC_READER_WRITER_LOCKS,
    _SC_REALTIME_SIGNALS,
    _SC_REGEXP,
    _SC_SAVED_IDS,
    _SC_SEMAPHORES,
    _SC_SHARED_MEMORY_OBJECTS,
    _SC_SHELL,
    _SC_SPAWN,
    _SC_SPIN_LOCKS,
    _SC_SPORADIC_SERVER,
    _SC_SYNCHRONIZED_IO,
    _SC_THREAD_ATTR_STACKADDR,
    _SC_THREAD_ATTR_STACKSIZE,
    _SC_THREAD_CPUTIME,
    _SC_THREAD_PRIO_INHERIT,
    _SC_THREAD_PRIO_PROTECT,
    _SC_THREAD_PRIORITY_SCHEDULING,
    _SC_THREAD_PROCESS_SHARED,
#ifdef _SC_THREAD_ROBUST_PRIO_INHERIT
    _SC_THREAD_ROBUST_PRIO_INHERIT,
    _SC_THREAD_ROBUST_PRIO_PROTECT,
#else
    -1,
    -1,
#endif
    _SC_THREAD_SAFE_FUNCTIONS,
    _SC_THREAD_SPORADIC_SERVER,
    _SC_THREADS,
    _SC_TIMEOUTS,
    _SC_TIMERS,
    _SC_TYPED_MEMORY_OBJECTS,
#ifdef _SC_V7_ILP32_OFF32
    _SC_V7_ILP32_OFF32,
    _SC_V7_ILP32_OFFBIG,
    _SC_V7_LP64_OFF64,
    _SC_V7_LPBIG_OFFBIG,
#else
    -1,
    -1,
    -1,
    -1,
#endif
#ifdef _SC_V8_ILP32_OFF32
    _SC_V8_ILP32_OFF32,
#else
    -1,
#endif
#ifdef _SC_V8_ILP32_OFFBIG
    _SC_V8_ILP32_OFFBIG,
#else
    -1,
#endif
#ifdef _SC_V8_LP64_OFF64
    _SC_V8_LP64_OFF64,
#else
    -1,
#endif
#ifdef _SC_V8_LPBIG_OFFBIG
    _SC_V8_LPBIG_OFFBIG,
#else
    -1,
#endif
    _SC_XOPEN_CRYPT,
    _SC_XOPEN_ENH_I18N,
    _SC_XOPEN_REALTIME,
    _SC_XOPEN_REALTIME_THREADS,
    _SC_XOPEN_SHM,
    _SC_XOPEN_UNIX,
#ifdef _SC_XOPEN_UUCP
    _SC_XOPEN_UUCP,
#else
    -1,
#endif
    _SC_XOPEN_VERSION,
};
static_assert(countof(SYSTEM_CONFIGURATION_KEYS) ==
              static_cast<usize>(system_configuration_key::Count));

static fn failed_configuration_query() wontthrow -> numeric_configuration_result
{
  return {errno == 0 ? configuration_query_status::Undefined
                     : configuration_query_status::Error,
          0};
}

fn query_system_configuration(system_configuration_key key) wontthrow
    -> numeric_configuration_result
{
  if (key == system_configuration_key::Count)
    return {configuration_query_status::Undefined, 0};
  let const native_key = SYSTEM_CONFIGURATION_KEYS[static_cast<usize>(key)];
  if (native_key < 0) return {configuration_query_status::Undefined, 0};

  errno = 0;
  let const value = sysconf(native_key);
  if (value == -1) return failed_configuration_query();
  return {configuration_query_status::Value, static_cast<i64>(value)};
}

static constexpr int STRING_CONFIGURATION_KEYS[] = {
    _CS_PATH,
#ifdef _CS_POSIX_V7_ILP32_OFF32_CFLAGS
    _CS_POSIX_V7_ILP32_OFF32_CFLAGS,
    _CS_POSIX_V7_ILP32_OFF32_LDFLAGS,
    _CS_POSIX_V7_ILP32_OFF32_LIBS,
    _CS_POSIX_V7_ILP32_OFFBIG_CFLAGS,
    _CS_POSIX_V7_ILP32_OFFBIG_LDFLAGS,
    _CS_POSIX_V7_ILP32_OFFBIG_LIBS,
    _CS_POSIX_V7_LP64_OFF64_CFLAGS,
    _CS_POSIX_V7_LP64_OFF64_LDFLAGS,
    _CS_POSIX_V7_LP64_OFF64_LIBS,
    _CS_POSIX_V7_LPBIG_OFFBIG_CFLAGS,
    _CS_POSIX_V7_LPBIG_OFFBIG_LDFLAGS,
    _CS_POSIX_V7_LPBIG_OFFBIG_LIBS,
#ifdef _CS_POSIX_V7_THREADS_CFLAGS
    _CS_POSIX_V7_THREADS_CFLAGS,
#else
    -1,
#endif
#ifdef _CS_POSIX_V7_THREADS_LDFLAGS
    _CS_POSIX_V7_THREADS_LDFLAGS,
#else
    -1,
#endif
    _CS_POSIX_V7_WIDTH_RESTRICTED_ENVS,
    _CS_V7_ENV,
#else
    -1,       -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
#endif
#ifdef _CS_POSIX_V8_ILP32_OFF32_CFLAGS
    _CS_POSIX_V8_ILP32_OFF32_CFLAGS,
    _CS_POSIX_V8_ILP32_OFF32_LDFLAGS,
    _CS_POSIX_V8_ILP32_OFF32_LIBS,
    _CS_POSIX_V8_ILP32_OFFBIG_CFLAGS,
    _CS_POSIX_V8_ILP32_OFFBIG_LDFLAGS,
    _CS_POSIX_V8_ILP32_OFFBIG_LIBS,
    _CS_POSIX_V8_LP64_OFF64_CFLAGS,
    _CS_POSIX_V8_LP64_OFF64_LDFLAGS,
    _CS_POSIX_V8_LP64_OFF64_LIBS,
    _CS_POSIX_V8_LPBIG_OFFBIG_CFLAGS,
    _CS_POSIX_V8_LPBIG_OFFBIG_LDFLAGS,
    _CS_POSIX_V8_LPBIG_OFFBIG_LIBS,
    _CS_POSIX_V8_THREADS_CFLAGS,
    _CS_POSIX_V8_THREADS_LDFLAGS,
    _CS_POSIX_V8_WIDTH_RESTRICTED_ENVS,
    _CS_V8_ENV,
#else
    -1,       -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
#endif
};
static_assert(countof(STRING_CONFIGURATION_KEYS) ==
              static_cast<usize>(string_configuration_key::Count));

fn query_string_configuration(string_configuration_key key,
                              Allocator allocator) throws
    -> StringConfigurationResult
{
  let result = StringConfigurationResult{allocator};
  if (key == string_configuration_key::Count) return result;
  let const native_key = STRING_CONFIGURATION_KEYS[static_cast<usize>(key)];
  if (native_key < 0) return result;

  errno = 0;
  usize required_size = confstr(native_key, nullptr, 0);
  if (required_size == 0) {
    if (errno != 0) result.status = configuration_query_status::Error;
    return result;
  }

  ArrayList<char> buffer{allocator};
  for (usize attempt_count = 0; attempt_count < 4; attempt_count++) {
    buffer.reserve(required_size);
    errno = 0;
    let const actual_size = confstr(native_key, buffer.begin(), required_size);
    if (actual_size == 0) {
      if (errno != 0) result.status = configuration_query_status::Error;
      return result;
    }
    if (actual_size > required_size) {
      required_size = actual_size;
      continue;
    }

    result.status = configuration_query_status::Value;
    result.value = String{
        allocator, StringView{buffer.begin(), actual_size - 1}
    };
    return result;
  }
  errno = EAGAIN;
  result.status = configuration_query_status::Error;
  return result;
}

static constexpr int PATH_CONFIGURATION_KEYS[] = {
    _PC_ALLOC_SIZE_MIN,
    _PC_ASYNC_IO,
    _PC_CHOWN_RESTRICTED,
    _PC_VDISABLE,
    _PC_FILESIZEBITS,
    _PC_LINK_MAX,
    _PC_MAX_CANON,
    _PC_MAX_INPUT,
    _PC_NAME_MAX,
    _PC_NO_TRUNC,
    _PC_PATH_MAX,
    _PC_PIPE_BUF,
    _PC_PRIO_IO,
    _PC_REC_INCR_XFER_SIZE,
    _PC_REC_MAX_XFER_SIZE,
    _PC_REC_MIN_XFER_SIZE,
    _PC_REC_XFER_ALIGN,
    _PC_SYMLINK_MAX,
    _PC_SYNC_IO,
    _PC_2_SYMLINKS,
#ifdef _PC_FALLOC
    _PC_FALLOC,
#else
    -1,
#endif
#ifdef _PC_TEXTDOMAIN_MAX
    _PC_TEXTDOMAIN_MAX,
#else
    -1,
#endif
#ifdef _PC_TIMESTAMP_RESOLUTION
    _PC_TIMESTAMP_RESOLUTION,
#else
    -1,
#endif
};
static_assert(countof(PATH_CONFIGURATION_KEYS) ==
              static_cast<usize>(path_configuration_key::Count));

fn query_path_configuration(StringView path,
                            path_configuration_key key) wontthrow
    -> numeric_configuration_result
{
  if (key == path_configuration_key::Count)
    return {configuration_query_status::Undefined, 0};
  let const native_key = PATH_CONFIGURATION_KEYS[static_cast<usize>(key)];
  if (native_key < 0) return {configuration_query_status::Undefined, 0};

  let const path_text = String{heap_allocator(), path};
  errno = 0;
  let const value = pathconf(path_text.c_str(), native_key);
  if (value == -1) return failed_configuration_query();
  return {configuration_query_status::Value, static_cast<i64>(value)};
}

fn path_component_length(StringView component) wontthrow -> Maybe<usize>
{
  return component.length;
}

fn sleep_for_seconds(double seconds) wontthrow -> void
{
  if (seconds <= 0.0) return;
  struct timespec requested;
  requested.tv_sec = static_cast<time_t>(seconds);
  requested.tv_nsec = static_cast<long>(
      (seconds - static_cast<double>(requested.tv_sec)) * 1000000000.0);
  struct timespec remaining;
  while (nanosleep(&requested, &remaining) == -1 && errno == EINTR) {
    if (INTERRUPT_REQUESTED) break;
    requested = remaining;
  }
}

} /* namespace os */

} /* namespace koshka */

#if KOSH_PLATFORM_IS KOSH_PLATFORM_COSMO

namespace koshka {

namespace os {

const ProgramSuffixList PROGRAM_SUFFIXES = []() {
  if (IsWindows()) return ProgramSuffixList{WINDOWS_PROGRAM_SUFFIXES};
  return ProgramSuffixList{POSIX_PROGRAM_SUFFIXES};
}();

fn normalize_program_name(String &program_name) -> program_name_info
{
  if (!IsWindows()) return {program_extension::None, program_name.length()};
  return normalize_windows_program_name(program_name);
}

} /* namespace os */

} /* namespace koshka */

#endif

namespace koshka {
namespace os {

fn get_current_process_id() wontthrow -> i64
{
  return static_cast<i64>(getpid());
}

fn register_platform_flags(FlagList &flags) throws -> void
{
#if KOSH_PLATFORM_IS KOSH_PLATFORM_COSMO
  static FlagBool ftrace{'\0', "ftrace", flag_section::Debug,
                         "Trace functions under Cosmopolitan."};
  static FlagBool strace{'\0', "strace", flag_section::Debug,
                         "Trace system calls under Cosmopolitan."};
  flags.push(&ftrace);
  flags.push(&strace);
#else
  unused(flags);
#endif
}

fn initialize_platform_runtime() wontthrow -> void
{
#if KOSH_PLATFORM_IS KOSH_PLATFORM_COSMO
  ShowCrashReports();
#endif
  capture_entry_ignored_signals();
}

} /* namespace os */
} /* namespace koshka */
