#include "Cli.hpp"
#include "Common.hpp"
#include "Debug.hpp"
#include "Errors.hpp"
#include "Eval.hpp"
#include "Platform.hpp"
#include "Trace.hpp"
#include "Utils.hpp"

#include <syslog.h>
#include <utmpx.h>

namespace koshka {
namespace os {

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
  struct named_priority
  {
    StringView name;
    int value;
  };
  static constexpr named_priority PRIORITIES[] = {
      {"emerg",   LOG_EMERG  },
      {"alert",   LOG_ALERT  },
      {"crit",    LOG_CRIT   },
      {"err",     LOG_ERR    },
      {"warning", LOG_WARNING},
      {"notice",  LOG_NOTICE },
      {"info",    LOG_INFO   },
      {"debug",   LOG_DEBUG  },
  };
  let level = LOG_NOTICE;
  let name = priority;
  if (let const separator = priority.find_character('.'); separator.has_value())
    name = priority.substring(*separator + 1);
  for (let const &candidate : PRIORITIES)
    if (candidate.name == name) return candidate.value;
  return level;
}

fn write_system_log(StringView tag, StringView priority, StringView message,
                    bool should_include_pid,
                    bool should_copy_to_stderr) wontthrow -> bool
{
  let tag_text = String{heap_allocator(), tag};
  let message_text = String{heap_allocator(), message};
  let options = should_include_pid ? LOG_PID : 0;
  if (should_copy_to_stderr) options |= LOG_PERROR;
  openlog(tag_text.is_empty() ? nullptr : tag_text.c_str(), options, 0);
  syslog(system_log_priority(priority), "%s", message_text.c_str());
  closelog();
  return true;
}

volatile sig_atomic_t INTERRUPT_REQUESTED = 0;
volatile sig_atomic_t CHILD_STATE_CHANGED = 0;
volatile sig_atomic_t SIGNAL_PENDING = 0;

static constexpr i32 SIGNAL_FLAG_COUNT = 128;
static volatile sig_atomic_t PENDING_SIGNAL_FLAGS[SIGNAL_FLAG_COUNT] = {};

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
    /* A Ctrl-C returns to the caller, any other interrupting signal retries. */
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

fn rewind_descriptor(os::descriptor fd, usize byte_count) wontthrow -> bool
{
  return lseek(fd, -static_cast<off_t>(byte_count), SEEK_CUR) !=
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

fn TempFileSet::track(Path &&) throws -> void {}
fn TempFileSet::count() const wontthrow -> usize { return 0; }
fn TempFileSet::cleanup_from(usize mark) wontthrow -> void { unused(mark); }

fn redirect_stdout(os::descriptor target) wontthrow -> os::descriptor
{
  const os::descriptor saved = fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 0);
  dup2(target, STDOUT_FILENO);
  note_descriptor_rebound();

  if (const int flags = fcntl(target, F_GETFD); flags != -1)
    fcntl(target, F_SETFD, flags | FD_CLOEXEC);

  return saved;
}

fn restore_stdout(os::descriptor saved) wontthrow -> void
{
  dup2(saved, STDOUT_FILENO);
  note_descriptor_rebound();
  close(saved);
}

/* Backups live at or above this number so a script never sees them. */
constexpr int SHELL_BACKUP_FD_FLOOR = 10;

fn save_and_replace_descriptor(i32 shell_fd, os::descriptor target) wontthrow
    -> saved_descriptor
{
  saved_descriptor result{};
  result.shell_fd = shell_fd;

  const os::descriptor backup =
      fcntl(shell_fd, F_DUPFD_CLOEXEC, SHELL_BACKUP_FD_FLOOR);
  if (backup == -1 && errno != EBADF) {
    result.is_dup2_ok = false;
    return result;
  }
  result.was_open = backup != -1;
  result.saved = backup;

  result.is_dup2_ok = dup2(target, shell_fd) != -1;
  note_descriptor_rebound();

  return result;
}

fn restore_descriptor(const saved_descriptor &saved) wontthrow -> void
{
  if (!saved.is_dup2_ok) return;

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
  saved_descriptor result{};
  result.shell_fd = shell_fd;
  const os::descriptor backup =
      fcntl(shell_fd, F_DUPFD_CLOEXEC, SHELL_BACKUP_FD_FLOOR);
  result.was_open = backup != -1;
  result.saved = backup;
  result.is_dup2_ok = backup != -1 || errno == EBADF;
  return result;
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

fn descriptor_from_fd_number(i64 fd_number) wontthrow -> os::descriptor
{
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
  /* getpwuid is avoided so the static build does not pull in the glibc NSS
     modules. */
  if (const char *name = std::getenv("LOGNAME"); name != nullptr)
    return String{name};
  if (const char *name = std::getenv("USER"); name != nullptr)
    return String{name};

  return uid_to_username(static_cast<u32>(getuid()));
}

fn get_hostname() throws -> Maybe<String>
{
  char buffer[256];
  if (gethostname(buffer, sizeof(buffer)) != 0) return koshka::None;
  buffer[sizeof(buffer) - 1] = '\0';

  return String{buffer};
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

/* The colon field at index of an /etc/passwd line, empty when the line has too
   few fields. The format is name:passwd:uid:gid:gecos:home:shell. The database
   is read directly rather than through getpwnam, which a static build cannot
   call without the glibc NSS modules. A user defined only through NSS is not
   seen, the accepted tradeoff for the static build. */
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

fn enumerate_users() throws -> ArrayList<String>
{
  ArrayList<String> users{heap_allocator()};

  let const contents = Path{StringView{"/etc/passwd"}}.read_entire_file();
  if (!contents) return users;

  let const text = contents->view();
  for (let const &line : utils::split_lines(text)) {
    let const name = passwd_field(line, 0);
    if (!name.is_empty()) users.push(String{name});
  }
  return users;
}

static const pid_t PARENT_SHELL_PID = getpid();

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
                                         bool should_disable) wontthrow
    : m_input(input)
{
  if (!should_disable || !is_fd_a_tty(input)) return;
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

fn compile_regex(StringView pattern, case_sensitivity sensitivity,
                 compiled_regex &out) throws -> regex_compile_result
{
  let const is_case_insensitive = sensitivity == case_sensitivity::Insensitive;
  let const pattern_text = String{heap_allocator(), pattern};
  int compile_flags = REG_EXTENDED;
  if (is_case_insensitive) compile_flags |= REG_ICASE;

  if (regcomp(&out.re, pattern_text.c_str(), compile_flags) != 0)
    return regex_compile_result::Invalid;

  return regex_compile_result::Ok;
}

fn compile_basic_regex(StringView pattern, case_sensitivity sensitivity,
                       compiled_regex &out) throws -> regex_compile_result
{
  let const is_case_insensitive = sensitivity == case_sensitivity::Insensitive;
  let const pattern_text = String{heap_allocator(), pattern};
  int compile_flags = 0;
  if (is_case_insensitive) compile_flags |= REG_ICASE;

  if (regcomp(&out.re, pattern_text.c_str(), compile_flags) != 0)
    return regex_compile_result::Invalid;

  return regex_compile_result::Ok;
}

fn execute_regex(compiled_regex &compiled, StringView subject,
                 ArrayList<regex_span> &spans, String &error_message,
                 Allocator scratch) throws -> regex_match_result
{
  let const subject_text = String{scratch, subject};
  let const group_count = compiled.re.re_nsub + 1;
  let matches = ArrayList<regmatch_t>{scratch};
  matches.reserve(group_count);
  for (usize i = 0; i < group_count; i++)
    matches.push(regmatch_t{});

  const int match_result = regexec(&compiled.re, subject_text.c_str(),
                                   group_count, matches.begin(), 0);

  if (match_result == REG_NOMATCH) return regex_match_result::NoMatch;

  if (match_result != 0) {
    char error_text[256];
    regerror(match_result, &compiled.re, error_text, sizeof(error_text));
    error_message = String{heap_allocator(), StringView{error_text}};
    return regex_match_result::Error;
  }

  spans.reserve(group_count);
  for (usize i = 0; i < group_count; i++) {
    spans.push(regex_span{static_cast<i64>(matches[i].rm_so),
                          static_cast<i64>(matches[i].rm_eo)});
  }

  return regex_match_result::Matched;
}

fn free_regex(compiled_regex &compiled) wontthrow -> void
{
  regfree(&compiled.re);
}

fn compile_search_regex(StringView pattern, case_sensitivity sensitivity,
                        compiled_regex &out) throws -> regex_compile_result
{
  let const is_case_insensitive = sensitivity == case_sensitivity::Insensitive;
  const String pattern_text{heap_allocator(), pattern};
  int compile_flags = REG_NOSUB;
  if (is_case_insensitive) compile_flags |= REG_ICASE;

  if (regcomp(&out.re, pattern_text.c_str(), compile_flags) != 0)
    return regex_compile_result::Invalid;

  return regex_compile_result::Ok;
}

fn regex_matches(compiled_regex &compiled, StringView subject) throws -> bool
{
#if defined REG_STARTEND
  regmatch_t bounds[1];
  bounds[0].rm_so = 0;
  bounds[0].rm_eo = static_cast<regoff_t>(subject.length);
  return regexec(&compiled.re, subject.data, 1, bounds, REG_STARTEND) == 0;
#else
  const String null_terminated{heap_allocator(), subject};
  return regexec(&compiled.re, null_terminated.c_str(), 0, nullptr, 0) == 0;
#endif
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

fn get_resource_limit(resource_kind kind, resource_limit &out) wontthrow -> bool
{
  let const which = rlimit_resource_of(kind);
  if (!which.has_value()) return false;

  struct rlimit limit{};
  if (getrlimit(*which, &limit) != 0) return false;

  out.soft = limit.rlim_cur == RLIM_INFINITY ? RESOURCE_UNLIMITED
                                             : static_cast<u64>(limit.rlim_cur);
  out.hard = limit.rlim_max == RLIM_INFINITY ? RESOURCE_UNLIMITED
                                             : static_cast<u64>(limit.rlim_max);
  return true;
}

fn set_resource_limit(resource_kind kind, const resource_limit &limit) wontthrow
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

fn terminal_size(u32 &columns, u32 &rows, descriptor output) wontthrow -> bool
{
  LOG(Debug, "querying the terminal size");
  struct winsize window{};
  if (ioctl(output, TIOCGWINSZ, &window) != 0) return false;
  if (window.ws_col == 0 || window.ws_row == 0) return false;
  columns = window.ws_col;
  rows = window.ws_row;
  return true;
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
  struct terminal_speed
  {
    StringView name;
    speed_t value;
  };
  static constexpr terminal_speed SPEEDS[] = {
      {"0",      B0     },
      {"50",     B50    },
      {"75",     B75    },
      {"110",    B110   },
      {"134",    B134   },
      {"150",    B150   },
      {"200",    B200   },
      {"300",    B300   },
      {"600",    B600   },
      {"1200",   B1200  },
      {"1800",   B1800  },
      {"2400",   B2400  },
      {"4800",   B4800  },
      {"9600",   B9600  },
      {"19200",  B19200 },
      {"38400",  B38400 },
#ifdef B57600
      {"57600",  B57600 },
#endif
#ifdef B115200
      {"115200", B115200},
#endif
  };
  for (let const &speed : SPEEDS)
    if (speed.name == text) return speed.value;
  return static_cast<speed_t>(~speed_t{0});
}

struct terminal_flag
{
  const char *name;
  tcflag_t value;
  tcflag_t termios::*member;
};

static constexpr terminal_flag TERMINAL_FLAGS[] = {
    {"echo",   ECHO,   &termios::c_lflag},
    {"igncr",  IGNCR,  &termios::c_iflag},
    {"opost",  OPOST,  &termios::c_oflag},
    {"tostop", TOSTOP, &termios::c_lflag},
    {"icanon", ICANON, &termios::c_lflag},
    {"isig",   ISIG,   &termios::c_lflag},
    {"iexten", IEXTEN, &termios::c_lflag},
    {"echoe",  ECHOE,  &termios::c_lflag},
    {"echok",  ECHOK,  &termios::c_lflag},
    {"echonl", ECHONL, &termios::c_lflag},
    {"noflsh", NOFLSH, &termios::c_lflag},
    {"ignbrk", IGNBRK, &termios::c_iflag},
    {"brkint", BRKINT, &termios::c_iflag},
    {"ignpar", IGNPAR, &termios::c_iflag},
    {"parmrk", PARMRK, &termios::c_iflag},
    {"inpck",  INPCK,  &termios::c_iflag},
    {"istrip", ISTRIP, &termios::c_iflag},
    {"inlcr",  INLCR,  &termios::c_iflag},
    {"icrnl",  ICRNL,  &termios::c_iflag},
    {"ixon",   IXON,   &termios::c_iflag},
    {"ixoff",  IXOFF,  &termios::c_iflag},
    {"cstopb", CSTOPB, &termios::c_cflag},
    {"cread",  CREAD,  &termios::c_cflag},
    {"parenb", PARENB, &termios::c_cflag},
    {"parodd", PARODD, &termios::c_cflag},
    {"hupcl",  HUPCL,  &termios::c_cflag},
    {"clocal", CLOCAL, &termios::c_cflag},
#ifdef ONLCR
    {"onlcr",  ONLCR,  &termios::c_oflag},
#endif
};

struct terminal_control_character
{
  const char *name;
  usize index;
};

static constexpr terminal_control_character TERMINAL_CONTROL_CHARACTERS[] = {
    {"eof",   VEOF  },
    {"eol",   VEOL  },
    {"erase", VERASE},
    {"intr",  VINTR },
    {"kill",  VKILL },
    {"quit",  VQUIT },
    {"start", VSTART},
    {"stop",  VSTOP },
#ifdef VSUSP
    {"susp",  VSUSP },
#endif
};

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

fn terminal_settings(descriptor terminal, bool should_encode,
                     bool should_report_all, Allocator allocator) throws
    -> Maybe<String>
{
  termios state{};
  if (tcgetattr(terminal, &state) != 0) return None;
  let output = String{allocator};
  if (should_encode) {
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
  u32 columns = 0;
  u32 rows = 0;
  if (terminal_size(columns, rows, terminal)) {
    output += "rows ";
    output += String::from(rows, allocator);
    output += "; columns ";
    output += String::from(columns, allocator);
    output += "; ";
  }
  for (let const &flag : TERMINAL_FLAGS) {
    if ((state.*(flag.member) & flag.value) == 0) output += '-';
    output += flag.name;
    output += ' ';
  }
  if (should_report_all) {
    for (let const &character : TERMINAL_CONTROL_CHARACTERS) {
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

static pure fn parse_terminal_character(StringView text, cc_t &value) wontthrow
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
                           const ArrayList<String> &settings) wontthrow -> bool
{
  termios state{};
  if (tcgetattr(terminal, &state) != 0) return false;
  if (settings.count() == 1 &&
      settings[0].view().find_character(':').has_value())
  {
    if (!restore_encoded_terminal_settings(state, settings[0].view()))
      return false;
    return tcsetattr(terminal, TCSADRAIN, &state) == 0;
  }

  winsize window{};
  let has_window = ioctl(terminal, TIOCGWINSZ, &window) == 0;
  for (usize index = 0; index < settings.count(); index++) {
    let const setting = settings[index].view();
    let const is_disabled = setting.length > 1 && setting[0] == '-';
    let const name = is_disabled ? setting.substring(1) : setting;
    const terminal_flag *selected_flag = NULL;
    for (let const &candidate : TERMINAL_FLAGS)
      if (name == candidate.name) {
        selected_flag = &candidate;
        break;
      }
    if (selected_flag != NULL) {
      if (is_disabled)
        state.*(selected_flag->member) &= ~selected_flag->value;
      else
        state.*(selected_flag->member) |= selected_flag->value;
      continue;
    }

    const terminal_control_character *selected_character = NULL;
    for (let const &candidate : TERMINAL_CONTROL_CHARACTERS)
      if (name == candidate.name) {
        selected_character = &candidate;
        break;
      }
    if (selected_character != NULL) {
      if (is_disabled || ++index == settings.count()) return false;
      cc_t value = 0;
      if (!parse_terminal_character(settings[index].view(), value))
        return false;
      state.c_cc[selected_character->index] = value;
      continue;
    }

    if (name == "cs5" || name == "cs6" || name == "cs7" || name == "cs8") {
      if (is_disabled) return false;
      static constexpr tcflag_t CHARACTER_SIZES[] = {CS5, CS6, CS7, CS8};
      state.c_cflag = (state.c_cflag & ~CSIZE) | CHARACTER_SIZES[name[2] - '5'];
      continue;
    }
    if (name == "raw") {
      if (!is_disabled) {
        cfmakeraw(&state);
      } else {
        state.c_lflag |= ICANON | ISIG | ECHO;
        state.c_iflag |= ICRNL;
        state.c_oflag |= OPOST;
      }
      continue;
    } else if (name == "sane") {
      if (is_disabled) return false;
      state.c_lflag |= ICANON | ISIG | ECHO | IEXTEN;
      state.c_lflag &= ~(ECHONL | NOFLSH | TOSTOP);
      state.c_iflag |= BRKINT | ICRNL | IXON;
      state.c_iflag &= ~(IGNBRK | IGNCR | INLCR | IXOFF);
      state.c_oflag |= OPOST;
      continue;
    } else if (name == "ek") {
      if (is_disabled) return false;
      state.c_cc[VERASE] = CERASE;
      state.c_cc[VKILL] = CKILL;
      continue;
    } else if (name == "nl") {
      if (is_disabled)
        state.c_iflag &= ~ICRNL;
      else
        state.c_iflag |= ICRNL;
      continue;
    } else if (name == "evenp" || name == "parity") {
      state.c_cflag &= ~PARODD;
      if (is_disabled) {
        state.c_cflag &= ~PARENB;
        state.c_cflag = (state.c_cflag & ~CSIZE) | CS8;
      } else {
        state.c_cflag |= PARENB;
        state.c_cflag = (state.c_cflag & ~CSIZE) | CS7;
      }
      continue;
    } else if (name == "oddp") {
      if (is_disabled) {
        state.c_cflag &= ~(PARENB | PARODD);
        state.c_cflag = (state.c_cflag & ~CSIZE) | CS8;
      } else {
        state.c_cflag |= PARENB | PARODD;
        state.c_cflag = (state.c_cflag & ~CSIZE) | CS7;
      }
      continue;
    } else if (name == "rows" || name == "cols" || name == "columns") {
      if (++index == settings.count() || !has_window) return false;
      let const parsed = utils::parse_decimal_u64(settings[index].view());
      if (parsed.is_error() || parsed.value() > UINT16_MAX) return false;
      if (name == "rows")
        window.ws_row = static_cast<u16>(parsed.value());
      else
        window.ws_col = static_cast<u16>(parsed.value());
      continue;
    } else if (name == "min" || name == "time") {
      if (++index == settings.count()) return false;
      let const parsed = utils::parse_decimal_u64(settings[index].view());
      if (parsed.is_error() || parsed.value() > UCHAR_MAX) return false;
      state.c_cc[name == "min" ? VMIN : VTIME] =
          static_cast<cc_t>(parsed.value());
      continue;
    } else {
      let const speed = terminal_speed_value(name);
      if (speed == static_cast<speed_t>(~speed_t{0}) || is_disabled)
        return false;
      if (cfsetispeed(&state, speed) != 0 || cfsetospeed(&state, speed) != 0)
        return false;
      continue;
    }
  }
  if (tcsetattr(terminal, TCSADRAIN, &state) != 0) return false;
  return !has_window || ioctl(terminal, TIOCSWINSZ, &window) == 0;
}

fn make_fd_inheritable(descriptor fd) wontthrow -> void
{
  const int flags = fcntl(fd, F_GETFD);
  if (flags != -1) fcntl(fd, F_SETFD, flags & ~FD_CLOEXEC);
}

/* TODO replace with a runtime check, Cosmopolitan runs on Linux and Windows. */
#if KOSH_PLATFORM_ISNT KOSH_PLATFORM_COSMO
const ProgramSuffixList PROGRAM_SUFFIXES{POSIX_PROGRAM_SUFFIXES};

fn normalize_program_name(String &program_name) throws -> program_name_info
{
  return {program_extension::None, program_name.length()};
}
#endif /* !COSMO */

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
}

fn unset_environment_variable(StringView key) throws -> void
{
  LOG(All, "unsetting the environment variable '%.*s'",
      static_cast<int>(key.length), key.data);
  const String key_string{key};
  unsetenv(key_string.c_str());
}

fn signal_internal_diagnostic() wontthrow -> void {}

fn environment_names() throws -> ArrayList<String>
{
  ArrayList<String> names{heap_allocator()};
  if (environ == nullptr) return names;
  for (char **entry = environ; *entry != nullptr; entry++) {
    StringView pair{*entry};
    let const equals = pair.find_character('=');
    let const name =
        equals.has_value() ? pair.substring_of_length(0, *equals) : pair;
    names.push(String{name});
  }
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

fn last_system_error_is_missing_file() wontthrow -> bool
{
  return errno == ENOENT;
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

static fn sigchild_handler(int n, siginfo_t *siginfo, opaque *ctx) wontthrow
    -> void
{
  unused(n);
  unused(ctx);
  unused(siginfo);
  CHILD_STATE_CHANGED = 1;
}

static fn install_child_state_handler() throws -> void
{
  struct sigaction action = {};
  action.sa_flags = SA_SIGINFO;
  action.sa_sigaction = sigchild_handler;
  check_syscall(sigaction(SIGCHLD, &action, nullptr));
}

fn reset_signal_handlers() throws -> void
{
  LOG(Debug, "restoring signal dispositions for a child process");

  sigset_t sm;
  sigfillset(&sm);
  check_syscall(sigprocmask(SIG_UNBLOCK, &sm, nullptr));

  struct sigaction sa = {};
  sa.sa_handler = SIG_DFL;
  install_child_state_handler();
  check_syscall(sigaction(SIGINT, &sa, nullptr));

  /* The shell ignores SIGPIPE, the child restores the default so a producer
     dies on a broken pipe. */
  check_syscall(sigaction(SIGPIPE, &sa, nullptr));

  /* A stale inherited flag would throw Interrupted before the child runs. */
  INTERRUPT_REQUESTED = 0;
}

static fn handle_interrupt(int s) wontthrow -> void
{
  unused(s);
  INTERRUPT_REQUESTED = 1;
}

fn set_default_signal_handlers(signal_profile profile) throws -> void
{
  let const is_interactive = profile == signal_profile::Interactive;
  LOG(Info, "installing the shell signal handlers, interactive %d",
      is_interactive ? 1 : 0);

  /* SIGHUP stays default on purpose so a hangup ends the shell rather than
     leaving it reparented to init and spinning on a redirected loop. */
  if (is_interactive) {
    sigset_t sm = make_sigset(SIGTERM, SIGQUIT, SIGSTOP, SIGTSTP);
    check_syscall(sigprocmask(SIG_BLOCK, &sm, nullptr));
  }

  install_child_state_handler();

  struct sigaction si = {};
  si.sa_handler = handle_interrupt;
  check_syscall(sigaction(SIGINT, &si, nullptr));

  struct sigaction sp = {};
  sp.sa_handler = SIG_IGN;
  check_syscall(sigaction(SIGPIPE, &sp, nullptr));
}

static fn handle_trapped_signal(int signal_number) wontthrow -> void
{
  if (is_trappable_signal(signal_number))
    PENDING_SIGNAL_FLAGS[signal_number] = 1;
  SIGNAL_PENDING = 1;
}

fn set_trap_handler(i32 signal_number) throws -> void
{
  if (!is_trappable_signal(signal_number)) return;

  LOG(Info, "installing the trap handler for signal %d", signal_number);

  /* A signal the startup blocked must be unblocked so the handler runs. */
  sigset_t sm;
  sigemptyset(&sm);
  sigaddset(&sm, signal_number);
  check_syscall(sigprocmask(SIG_UNBLOCK, &sm, nullptr));

  struct sigaction sa = {};
  sa.sa_handler = handle_trapped_signal;
  check_syscall(sigaction(signal_number, &sa, nullptr));
}

fn set_trap_ignore(i32 signal_number) throws -> void
{
  if (!is_trappable_signal(signal_number)) return;
  LOG(Info, "ignoring signal %d", signal_number);
  struct sigaction sa = {};
  sa.sa_handler = SIG_IGN;
  check_syscall(sigaction(signal_number, &sa, nullptr));
}

fn clear_trap_handler(i32 signal_number) throws -> void
{
  if (!is_trappable_signal(signal_number)) return;
  LOG(Info, "clearing the trap for signal %d", signal_number);
  struct sigaction sa = {};
  /* SIGINT returns to the shell's handler so a Ctrl-C still aborts a loop. */
  if (signal_number == SIGINT)
    sa.sa_handler = handle_interrupt;
  else
    sa.sa_handler = SIG_DFL;
  check_syscall(sigaction(signal_number, &sa, nullptr));
}

/* The field 0 name of the first colon line whose field at id_field_index equals
   the wanted id. One reader serves both /etc/passwd and /etc/group. */
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
    if (!id.is_error() && id.value() >= 0 && id.value() <= UINT32_MAX)
      return static_cast<u32>(id.value());
  }
  return koshka::None;
}

fn uid_to_username(u32 uid) throws -> Maybe<String>
{
  return lookup_name_by_id("/etc/passwd", uid, 2);
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

fn system_configuration(system_configuration_key key) wontthrow -> Maybe<i64>
{
  int native_key = 0;
  switch (key) {
  case system_configuration_key::ArgMax: native_key = _SC_ARG_MAX; break;
  case system_configuration_key::ChildMax: native_key = _SC_CHILD_MAX; break;
  case system_configuration_key::ClockTicks: native_key = _SC_CLK_TCK; break;
  case system_configuration_key::GroupsMax: native_key = _SC_NGROUPS_MAX; break;
  case system_configuration_key::OpenMax: native_key = _SC_OPEN_MAX; break;
  case system_configuration_key::PageSize: native_key = _SC_PAGESIZE; break;
  case system_configuration_key::StreamMax: native_key = _SC_STREAM_MAX; break;
  case system_configuration_key::PosixVersion: native_key = _SC_VERSION; break;
  }

  errno = 0;
  let const value = sysconf(native_key);
  if (value == -1 && errno != 0) return None;
  return static_cast<i64>(value);
}

fn path_configuration(StringView path, path_configuration_key key) wontthrow
    -> Maybe<i64>
{
  int native_key = 0;
  switch (key) {
  case path_configuration_key::LinkMax: native_key = _PC_LINK_MAX; break;
  case path_configuration_key::MaxCanonical: native_key = _PC_MAX_CANON; break;
  case path_configuration_key::MaxInput: native_key = _PC_MAX_INPUT; break;
  case path_configuration_key::NameMax: native_key = _PC_NAME_MAX; break;
  case path_configuration_key::PathMax: native_key = _PC_PATH_MAX; break;
  case path_configuration_key::PipeBuffer: native_key = _PC_PIPE_BUF; break;
  case path_configuration_key::ChownRestricted:
    native_key = _PC_CHOWN_RESTRICTED;
    break;
  case path_configuration_key::NoTrunc: native_key = _PC_NO_TRUNC; break;
  case path_configuration_key::DisableCharacter:
    native_key = _PC_VDISABLE;
    break;
  }

  let const path_text = String{heap_allocator(), path};
  errno = 0;
  let const value = pathconf(path_text.c_str(), native_key);
  if (value == -1 && errno != 0) return None;
  return static_cast<i64>(value);
}

fn sleep_for_seconds(double seconds) wontthrow -> void
{
  if (seconds <= 0.0) return;
  struct timespec requested;
  requested.tv_sec = static_cast<time_t>(seconds);
  requested.tv_nsec = static_cast<long>(
      (seconds - static_cast<double>(requested.tv_sec)) * 1000000000.0);
  /* A Ctrl-C returns at once, any other signal sleeps the remaining time. */
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

#endif /* COSMO */

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
}

} /* namespace os */
} /* namespace koshka */
