#include "Cli.hpp"
#include "Common.hpp"
#include "Debug.hpp"
#include "Errors.hpp"
#include "Eval.hpp"
#include "Platform.hpp"
#include "Trace.hpp"
#include "Utils.hpp"

namespace koshka {
namespace os {

volatile sig_atomic_t INTERRUPT_REQUESTED = 0;
volatile sig_atomic_t CHILD_STATE_CHANGED = 0;
volatile sig_atomic_t SIGNAL_PENDING = 0;

static constexpr i32 SIGNAL_FLAG_COUNT = 128;
static volatile sig_atomic_t PENDING_SIGNAL_FLAGS[SIGNAL_FLAG_COUNT] = {};

static fn is_trappable_signal(i32 signal_number) wontthrow -> bool
{
  return signal_number > 0 && signal_number < SIGNAL_FLAG_COUNT;
}

fn take_pending_signal() wontthrow -> i32
{
  for (i32 number = 1; number < SIGNAL_FLAG_COUNT; number++) {
    if (PENDING_SIGNAL_FLAGS[number] != 0) {
      PENDING_SIGNAL_FLAGS[number] = 0;
      return number;
    }
  }
  return 0;
}

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

fn TempFileSet::track(Path path) throws -> void { unused(path); }
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

fn uid_to_username(u32 uid) throws -> Maybe<String>
{
  return lookup_name_by_id("/etc/passwd", uid, 2);
}

fn gid_to_groupname(u32 gid) throws -> Maybe<String>
{
  return lookup_name_by_id("/etc/group", gid, 2);
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

fn get_shell_process_id() wontthrow -> i64
{
  return static_cast<i64>(PARENT_SHELL_PID);
}

fn get_current_process_id() wontthrow -> i64
{
  return static_cast<i64>(getpid());
}

fn get_file_creation_mask() wontthrow -> u32
{
  /* umask reads only through a set, so it is read and put back. */
  let const previous_mask = KOSH_UMASK(0);
  KOSH_UMASK(previous_mask);

  return static_cast<u32>(previous_mask);
}

fn set_file_creation_mask(u32 mask) wontthrow -> void { KOSH_UMASK(mask); }

fn descriptor_is_shell_fd(os::descriptor fd, i32 shell_fd) wontthrow -> bool
{
  return fd == descriptor_for_shell_fd(shell_fd);
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
