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
  result.was_open = backup != -1;
  result.saved = backup;

  result.is_dup2_ok = dup2(target, shell_fd) != -1;
  note_descriptor_rebound();

  return result;
}

fn restore_descriptor(const saved_descriptor &saved) wontthrow -> void
{
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

fn is_child_process() wontthrow -> bool { return getpid() != PARENT_SHELL_PID; }

fn is_running_setuid() wontthrow -> bool
{
  return geteuid() != getuid() || getegid() != getgid();
}

fn drop_elevated_identity() wontthrow -> bool
{
  let const real_group_id = getgid();
  let const real_user_id = getuid();
  if (setregid(real_group_id, real_group_id) != 0) return false;
  return setreuid(real_user_id, real_user_id) == 0;
}

fn process_id_of(process p) wontthrow -> i64 { return static_cast<i64>(p); }
fn process_group_of(process p) throws -> process { return -p; }
fn close_process_group(process group) wontthrow -> void { unused(group); }
fn process_has_id(process p, i64 id) wontthrow -> bool
{
  return p == static_cast<process>(id);
}

fn is_stdin_a_tty() wontthrow -> bool { return isatty(KOSH_STDIN); }

fn is_stdout_a_tty() wontthrow -> bool { return isatty(KOSH_STDOUT); }

fn is_stderr_a_tty() wontthrow -> bool { return isatty(KOSH_STDERR); }
fn is_fd_a_tty(descriptor fd) wontthrow -> bool { return isatty(fd); }

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

pure fn path_is_absolute(StringView path) wontthrow -> bool
{
  if (path.length == 0) return false;
  return is_directory_separator(path.data[0]);
}

pure fn path_is_drive_relative(StringView path) wontthrow -> bool
{
  unused(path);
  return false;
}

fn resolve_drive_relative_path(StringView path) throws -> Maybe<Path>
{
  unused(path);
  return None;
}

pure fn path_root_length(StringView path) wontthrow -> usize
{
  return path_is_absolute(path) ? 1 : 0;
}

fn temp_directory_path() throws -> String
{
  if (const char *from_env = std::getenv("TMPDIR"); from_env != nullptr)
    return String{from_env};
  return String{"/tmp"};
}

cold fn path_exists(StringView path) wontthrow -> bool
{
  const String path_string{path};
  LOG(Debug, "probing whether '%s' exists", path_string.c_str());
  struct stat info{};
  return ::stat(path_string.c_str(), &info) == 0;
}

/* A failed stat reads as the type not matching. */
static fn stat_matches_type(const char *path, mode_t expected_type) wontthrow
    -> bool
{
  struct stat info{};
  if (::stat(path, &info) != 0) return false;
  return (info.st_mode & S_IFMT) == expected_type;
}

cold fn path_is_directory(StringView path) wontthrow -> bool
{
  const String path_string{path};
  return stat_matches_type(path_string.c_str(), S_IFDIR);
}

fn path_is_regular_file(StringView path) wontthrow -> bool
{
  const String path_string{path};
  return stat_matches_type(path_string.c_str(), S_IFREG);
}

fn path_is_symbolic_link(StringView path) wontthrow -> bool
{
  const String path_string{path};
  struct stat info{};
  if (::lstat(path_string.c_str(), &info) != 0) return false;
  return S_ISLNK(info.st_mode);
}

fn path_is_block_device(StringView path) wontthrow -> bool
{
  const String path_string{path};
  return stat_matches_type(path_string.c_str(), S_IFBLK);
}

fn path_is_character_device(StringView path) wontthrow -> bool
{
  const String path_string{path};
  return stat_matches_type(path_string.c_str(), S_IFCHR);
}

fn path_is_fifo(StringView path) wontthrow -> bool
{
  const String path_string{path};
  return stat_matches_type(path_string.c_str(), S_IFIFO);
}

fn path_is_socket(StringView path) wontthrow -> bool
{
  const String path_string{path};
  return stat_matches_type(path_string.c_str(), S_IFSOCK);
}

static fn stat_mode_has_bits(const char *path, mode_t bits) wontthrow -> bool
{
  struct stat info{};
  if (::stat(path, &info) != 0) return false;
  return (info.st_mode & bits) != 0;
}

fn path_has_setuid_bit(StringView path) wontthrow -> bool
{
  const String path_string{path};
  return stat_mode_has_bits(path_string.c_str(), S_ISUID);
}

fn path_has_setgid_bit(StringView path) wontthrow -> bool
{
  const String path_string{path};
  return stat_mode_has_bits(path_string.c_str(), S_ISGID);
}

fn path_has_sticky_bit(StringView path) wontthrow -> bool
{
  const String path_string{path};
  return stat_mode_has_bits(path_string.c_str(), S_ISVTX);
}

fn path_is_owned_by_effective_user(StringView path) wontthrow -> bool
{
  const String path_string{path};
  struct stat info{};
  if (::stat(path_string.c_str(), &info) != 0) return false;
  return info.st_uid == ::geteuid();
}

fn path_is_owned_by_effective_group(StringView path) wontthrow -> bool
{
  const String path_string{path};
  struct stat info{};
  if (::stat(path_string.c_str(), &info) != 0) return false;
  return info.st_gid == ::getegid();
}

fn path_file_size(StringView path) wontthrow -> Maybe<u64>
{
  const String path_string{path};
  struct stat info{};
  if (::stat(path_string.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) {
    return None;
  }
  return static_cast<u64>(info.st_size);
}

fn path_modification_time(StringView path) wontthrow -> Maybe<i64>
{
  const String path_string{path};
  struct stat info{};
  if (::stat(path_string.c_str(), &info) != 0) return None;
  return static_cast<i64>(info.st_mtime);
}

fn paths_are_same_file(StringView first, StringView second) wontthrow -> bool
{
  const String first_string{first};
  const String second_string{second};
  struct stat first_info{}, second_info{};
  if (::stat(first_string.c_str(), &first_info) != 0) return false;
  if (::stat(second_string.c_str(), &second_info) != 0) return false;
  return first_info.st_dev == second_info.st_dev &&
         first_info.st_ino == second_info.st_ino;
}

fn path_is_newer_than(StringView first, StringView second) wontthrow -> bool
{
  const String first_string{first};
  const String second_string{second};
  struct stat first_info{}, second_info{};
  if (::stat(first_string.c_str(), &first_info) != 0) return false;
  if (::stat(second_string.c_str(), &second_info) != 0) return false;
  /* The nanoseconds break a same-second tie. */
  if (first_info.st_mtim.tv_sec != second_info.st_mtim.tv_sec)
    return first_info.st_mtim.tv_sec > second_info.st_mtim.tv_sec;
  return first_info.st_mtim.tv_nsec > second_info.st_mtim.tv_nsec;
}

fn path_is_older_than(StringView first, StringView second) wontthrow -> bool
{
  const String first_string{first};
  const String second_string{second};
  struct stat first_info{}, second_info{};
  if (::stat(first_string.c_str(), &first_info) != 0) return false;
  if (::stat(second_string.c_str(), &second_info) != 0) return false;
  if (first_info.st_mtim.tv_sec != second_info.st_mtim.tv_sec)
    return first_info.st_mtim.tv_sec < second_info.st_mtim.tv_sec;
  return first_info.st_mtim.tv_nsec < second_info.st_mtim.tv_nsec;
}

fn path_is_readable(StringView path) wontthrow -> bool
{
  const String path_string{path};
  return ::access(path_string.c_str(), R_OK) == 0;
}

fn path_is_writable(StringView path) wontthrow -> bool
{
  const String path_string{path};
  return ::access(path_string.c_str(), W_OK) == 0;
}

fn path_is_executable(StringView path) wontthrow -> bool
{
  const String path_string{path};
  return ::access(path_string.c_str(), X_OK) == 0;
}

cold fn read_current_directory() throws -> Path
{
  /* ERANGE means the buffer is too small, so it doubles. Any other errno ends
     the loop with an empty path. */
  LOG(Debug, "reading the current working directory");
  let buffer = ArrayList<char>{heap_allocator()};
  usize buffer_size = 4096;
  loop
  {
    buffer.reserve(buffer_size);
    errno = 0;
    if (::getcwd(buffer.begin(), buffer_size) != nullptr)
      return Path{StringView{buffer.begin()}};
    if (errno != ERANGE) return Path{};
    buffer_size *= 2;
  }
}

fn change_current_directory(StringView path) throws -> ErrorOr<Ok>
{
  const String path_string{path};
  LOG(Info, "changing the current directory to '%s'", path_string.c_str());
  if (::chdir(path_string.c_str()) != 0)
    return Error{"Could not change directory to '" + path_string + "'"};
  return Success;
}

fn reference_current_directory() wontthrow -> DirectoryReference
{
  return DirectoryReference{open_current_directory_reference()};
}

fn restore_current_directory(const DirectoryReference &reference) wontthrow
    -> bool
{
  return reference.is_valid() && ::fchdir(reference.get()) == 0;
}

cold fn list_directory(StringView dir) throws -> Maybe<ArrayList<String>>
{
  const String dir_string{dir};
  let const handle = ::opendir(dir_string.c_str());
  if (handle == nullptr) return None;

  let names = ArrayList<String>{heap_allocator()};
  loop
  {
    errno = 0;
    let const entry = ::readdir(handle);
    if (entry == nullptr) {
      if (errno != 0) {
        ::closedir(handle);
        return None;
      }
      break;
    }

    let const name = StringView{entry->d_name};
    if (name == StringView{"."} || name == StringView{".."}) continue;
    names.push(String{name});
  }

  ::closedir(handle);

  LOG(All, "read %zu entries from the directory '%s'", names.count(),
      dir_string.c_str());

  return names;
}

cold fn list_directory_typed(StringView dir) throws
    -> Maybe<ArrayList<Path::directory_child>>
{
  const String dir_string{dir};
  let const handle = ::opendir(dir_string.c_str());
  if (handle == nullptr) return None;

  let entries = ArrayList<Path::directory_child>{heap_allocator()};
  loop
  {
    errno = 0;
    let const entry = ::readdir(handle);
    if (entry == nullptr) {
      if (errno != 0) {
        ::closedir(handle);
        return None;
      }
      break;
    }

    let const name = StringView{entry->d_name};
    if (name == StringView{"."} || name == StringView{".."}) {
      continue;
    }

    Path::entry_kind kind = Path::entry_kind::Unknown;
    switch (entry->d_type) {
    case DT_DIR: kind = Path::entry_kind::Directory; break;
    case DT_REG: kind = Path::entry_kind::Regular; break;
    case DT_LNK: kind = Path::entry_kind::Symlink; break;
    case DT_UNKNOWN: kind = Path::entry_kind::Unknown; break;
    default: kind = Path::entry_kind::Other; break;
    }

    entries.push(Path::directory_child{String{name}, kind});
  }

  ::closedir(handle);
  return entries;
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
#if KOSH_PLATFORM_ISNT COSMO
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

/* posix_spawn reports an exec failure through its return value with no waitable
   pid, so a child is forked to give the caller the same pid and status. */
cold fn spawn_failure_child(SourceLocation location, const Path &program_path,
                            int spawn_error, StringView source,
                            process_group_mode process_group,
                            i64 process_group_id) throws -> process
{
  LOG(Debug, "forking a child to report the spawn failure for '%s'",
      program_path.c_str());

  const pid_t child_pid = check_syscall(fork());

  if (child_pid == 0) {
    if (process_group != process_group_mode::Inherit) {
      let const target_group = process_group == process_group_mode::Join
                                   ? static_cast<pid_t>(process_group_id)
                                   : 0;
      (void) setpgid(0, target_group);
    }
    errno = spawn_error;
    let error = ErrorWithLocation{steal(location),
                                  "Unable to execute `" + program_path.text() +
                                      "`: " + last_system_error_message()};
    koshka::show_message(error.to_string(source));
    koshka::flush();
    /* 127 for a missing file, 126 for a resolved but unexecutable program. */
    _exit(spawn_error == ENOENT ? 127 : 126);
  }

  if (process_group != process_group_mode::Inherit) {
    let const target_group = process_group == process_group_mode::Join
                                 ? static_cast<pid_t>(process_group_id)
                                 : child_pid;
    (void) setpgid(child_pid, target_group);
  }

  return child_pid;
}

hot fn execute_program(ExecContext &ec, script_fallback_policy fallback,
                       process_group_mode process_group, StringView source,
                       terminal_handoff handoff, i64 process_group_id) throws
    -> process
{
  let const allow_script_fallback = fallback == script_fallback_policy::Allow;
  let const new_process_group = process_group != process_group_mode::Inherit;
  let const should_hand_off_controlling_terminal_before_start =
      handoff == terminal_handoff::BeforeStart;
  ASSERT(ec.args().count() > 0, "a program needs at least argv[0]");

  LOG(Debug, "spawning '%s' with %zu arguments", ec.program_path().c_str(),
      ec.args().count());

  bool was_fds_handed_to_fallback = false;
  defer
  {
    if (!was_fds_handed_to_fallback) ec.close_fds();
  };

  if (should_hand_off_controlling_terminal_before_start) {
    let const start_pipe = os::make_pipe();
    let const outcome_pipe = os::make_pipe();
    if (!start_pipe.has_value() || !outcome_pipe.has_value()) {
      if (start_pipe.has_value()) {
        os::close_fd(start_pipe->in);
        os::close_fd(start_pipe->out);
      }
      if (outcome_pipe.has_value()) {
        os::close_fd(outcome_pipe->in);
        os::close_fd(outcome_pipe->out);
      }
      throw ErrorWithLocation{ec.source_location(),
                              "Could not open the program start gate"};
    }

    koshka::flush();
    let const child = fork_job_process();
    if (child == 0) {
      os::close_fd(start_pipe->out);
      os::close_fd(outcome_pipe->in);
      char start_byte = 0;
      let const start_read = os::read_fd(start_pipe->in, &start_byte, 1);
      os::close_fd(start_pipe->in);
      if (!start_read.has_value() || *start_read == 0) {
        os::exit_process_immediately(1);
      }

      try {
        os::replace_process(steal(ec));
        unused(os::write_fd(outcome_pipe->out, "f", 1));
        os::close_fd(outcome_pipe->out);
        os::exit_process_immediately(0);
      } catch (const ErrorBase &error) {
        show_message(error.to_string(source));
        flush();
        os::exit_process_immediately(static_cast<i32>(error.command_status()));
      } catch (...) {
        os::exit_process_immediately(1);
      }
    }

    os::close_fd(start_pipe->in);
    os::close_fd(outcome_pipe->out);
    os::give_controlling_terminal_to(child);
    let const start_write = os::write_fd(start_pipe->out, "x", 1);
    os::close_fd(start_pipe->out);
    if (!start_write.has_value()) {
      unused(os::signal_process(child, 9));
      os::reap_process_quietly(child);
      os::close_fd(outcome_pipe->in);
      throw ErrorWithLocation{ec.source_location(),
                              "Could not release the program start gate"};
    }

    char outcome = 0;
    let const outcome_read = os::read_fd(outcome_pipe->in, &outcome, 1);
    os::close_fd(outcome_pipe->in);
    if (!outcome_read.has_value()) {
      unused(os::signal_process(child, 9));
      os::reap_process_quietly(child);
      throw ErrorWithLocation{ec.source_location(),
                              "Could not read the program start outcome"};
    }
    if (*outcome_read == 1 && outcome == 'f' && allow_script_fallback) {
      os::reap_process_quietly(child);
      was_fds_handed_to_fallback = true;
      return KOSH_INVALID_PROCESS;
    }

    ec.close_fds();
    return child;
  }

  let const child_args = make_os_args(ec.args());

  posix_spawn_file_actions_t file_actions;
  posix_spawn_file_actions_init(&file_actions);
  defer { posix_spawn_file_actions_destroy(&file_actions); };

  /* A descriptor already on its target slot is left in place, the close would
     shut the live descriptor. */
  if (ec.in_fd && *ec.in_fd != STDIN_FILENO) {
    posix_spawn_file_actions_adddup2(&file_actions, *ec.in_fd, STDIN_FILENO);
    posix_spawn_file_actions_addclose(&file_actions, *ec.in_fd);
  }
  if (ec.out_fd && *ec.out_fd != STDOUT_FILENO) {
    posix_spawn_file_actions_adddup2(&file_actions, *ec.out_fd, STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&file_actions, *ec.out_fd);
  }
  if (ec.err_fd && *ec.err_fd != STDERR_FILENO) {
    posix_spawn_file_actions_adddup2(&file_actions, *ec.err_fd, STDERR_FILENO);
    posix_spawn_file_actions_addclose(&file_actions, *ec.err_fd);
  }
  /* The dups come after the files are placed, so 2>&1 sees the final stdout. */
  ec.apply_dup_routing(
      [&]() {
        posix_spawn_file_actions_adddup2(&file_actions, STDOUT_FILENO,
                                         STDERR_FILENO);
      },
      [&]() {
        posix_spawn_file_actions_adddup2(&file_actions, STDERR_FILENO,
                                         STDOUT_FILENO);
      });

  posix_spawnattr_t attr;
  posix_spawnattr_init(&attr);
  defer { posix_spawnattr_destroy(&attr); };

  sigset_t empty_mask;
  sigemptyset(&empty_mask);
  posix_spawnattr_setsigmask(&attr, &empty_mask);

  sigset_t default_signals;
  sigemptyset(&default_signals);
  sigaddset(&default_signals, SIGINT);
  sigaddset(&default_signals, SIGCHLD);
  /* SIGPIPE is reset so a pipe producer dies rather than inheriting the shell's
     ignore. */
  sigaddset(&default_signals, SIGPIPE);
  posix_spawnattr_setsigdefault(&attr, &default_signals);

  short spawn_flags = POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
  if (new_process_group) {
    ASSERT(process_group != process_group_mode::Join || process_group_id > 0);
    posix_spawnattr_setpgroup(&attr, process_group == process_group_mode::Join
                                         ? static_cast<pid_t>(process_group_id)
                                         : 0);
    spawn_flags |= POSIX_SPAWN_SETPGROUP;
  }
  posix_spawnattr_setflags(&attr, spawn_flags);

  pid_t child_pid = 0;
  char *const empty_environment[] = {nullptr};
  const int spawn_error =
      posix_spawn(&child_pid, ec.program_path().c_str(), &file_actions, &attr,
                  const_cast<char *const *>(child_args.begin()),
                  ec.should_use_empty_environment
                      ? const_cast<char *const *>(empty_environment)
                      : environ);

  /* An ENOEXEC file with no shebang runs as a shell script in place, the POSIX
     behavior. The check runs before the fds close so the script keeps them. */
  if (spawn_error == ENOEXEC && allow_script_fallback) {
    was_fds_handed_to_fallback = true;
    return KOSH_INVALID_PROCESS;
  }

  ec.close_fds();

  if (spawn_error != 0)
    return spawn_failure_child(ec.source_location(), ec.program_path(),
                               spawn_error, source, process_group,
                               process_group_id);

  return child_pid;
}

fn shell_has_controlling_terminal() wontthrow -> bool
{
  return isatty(STDIN_FILENO) == 1;
}

fn canonical_path(const Path &path) wontthrow -> Maybe<Path>
{
  char *resolved_path = realpath(path.c_str(), nullptr);
  if (resolved_path == nullptr) return None;
  Path result{StringView{resolved_path}};
  free(resolved_path);
  return result;
}

fn glob_matches(StringView pattern, Allocator allocator) throws
    -> ArrayList<String>
{
  let matches = ArrayList<String>{allocator};

  const String pattern_string{allocator, pattern};
  glob_t glob_result{};
  if (glob(pattern_string.c_str(), 0, nullptr, &glob_result) == 0) {
    for (usize i = 0; i < glob_result.gl_pathc; i++)
      matches.push(String{allocator, StringView{glob_result.gl_pathv[i]}});
  }
  globfree(&glob_result);

  return matches;
}

fn directory_is_trusted_for_exec(const Path &directory) wontthrow -> bool
{
  struct stat directory_stat;
  if (stat(directory.c_str(), &directory_stat) != 0) {
    LOG(Debug, "trust check failed because stat failed on '%s'",
        directory.c_str());
    return false;
  }
  /* Trusted means owned by root or the user, and write access is held by root,
     the user, or one of the user's own groups. A group-writable Homebrew
     install directory is owned by the user's own group, so it is trusted,
     while a world-writable directory stays untrusted. */
  let const is_owner_trusted =
      directory_stat.st_uid == 0 || directory_stat.st_uid == geteuid();
  if (!is_owner_trusted) {
    LOG(Debug,
        "trust check failed because '%s' is owned by uid %d, not root "
        "or the current user",
        directory.c_str(), directory_stat.st_uid);
    return false;
  }
  if ((directory_stat.st_mode & S_IWOTH) != 0) {
    LOG(Debug, "trust check failed because '%s' is world-writable",
        directory.c_str());
    return false;
  }
  if ((directory_stat.st_mode & S_IWGRP) == 0) {
    LOG(Debug, "trust check passed for '%s', owner-only write",
        directory.c_str());
    return true;
  }

  gid_t groups[NGROUPS_MAX];
  int group_count = getgroups(NGROUPS_MAX, groups);
  for (int i = 0; i < group_count; i++)
    if (groups[i] == directory_stat.st_gid) {
      LOG(Debug,
          "trust check passed for '%s', group-writable but gid %d is "
          "one of the user's groups",
          directory.c_str(), directory_stat.st_gid);
      return true;
    }
  LOG(Debug,
      "trust check failed because '%s' is group-writable by gid %d, "
      "not one of the user's groups",
      directory.c_str(), directory_stat.st_gid);
  return false;
}

fn capture_program_output(const ArrayList<String> &argv,
                          u64 timeout_nanos) wontthrow -> Maybe<String>
{
  if (argv.is_empty()) return None;

  int pipe_fds[2];
  if (pipe(pipe_fds) != 0) return None;
  const int read_end = pipe_fds[0];
  const int write_end = pipe_fds[1];

  const int devnull_fd = open("/dev/null", O_RDONLY);
  if (devnull_fd < 0) {
    close(read_end);
    close(write_end);
    return None;
  }

  posix_spawn_file_actions_t file_actions;
  posix_spawn_file_actions_init(&file_actions);
  posix_spawn_file_actions_adddup2(&file_actions, devnull_fd, STDIN_FILENO);
  posix_spawn_file_actions_adddup2(&file_actions, write_end, STDOUT_FILENO);
  posix_spawn_file_actions_adddup2(&file_actions, write_end, STDERR_FILENO);
  posix_spawn_file_actions_addclose(&file_actions, read_end);
  posix_spawn_file_actions_addclose(&file_actions, write_end);
  posix_spawn_file_actions_addclose(&file_actions, devnull_fd);

  let const raw_args = make_os_args(argv);

  /* The shell ignores SIGPIPE. The spawn restores the default in the child, so
     a child that keeps writing after the read end closes on the timeout dies on
     SIGPIPE rather than seeing EPIPE. */
  posix_spawnattr_t attr;
  posix_spawnattr_init(&attr);
  sigset_t default_signals;
  sigemptyset(&default_signals);
  sigaddset(&default_signals, SIGPIPE);
  posix_spawnattr_setsigdefault(&attr, &default_signals);
  posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGDEF);

  pid_t child_pid = 0;
  const int spawn_result =
      posix_spawn(&child_pid, raw_args[0], &file_actions, &attr,
                  const_cast<char *const *>(raw_args.begin()), environ);
  posix_spawnattr_destroy(&attr);
  posix_spawn_file_actions_destroy(&file_actions);
  close(write_end);
  close(devnull_fd);
  if (spawn_result != 0) {
    close(read_end);
    return None;
  }

  let captured = String{heap_allocator()};
  const u64 deadline_nanos = monotonic_nanos() + timeout_nanos;
  bool was_timed_out = false;
  loop
  {
    const u64 now_nanos = monotonic_nanos();
    if (now_nanos >= deadline_nanos) {
      was_timed_out = true;
      break;
    }
    int remaining_millis =
        static_cast<int>((deadline_nanos - now_nanos) / 1'000'000);
    if (remaining_millis <= 0) remaining_millis = 1;

    struct pollfd watch;
    watch.fd = read_end;
    watch.events = POLLIN;
    watch.revents = 0;
    const int ready = poll(&watch, 1, remaining_millis);
    if (ready < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (ready == 0) {
      was_timed_out = true;
      break;
    }

    char buffer[4096];
    const ssize_t read_count = read(read_end, buffer, sizeof(buffer));
    if (read_count < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (read_count == 0) break;
    captured.append(StringView{buffer, static_cast<usize>(read_count)});
  }
  close(read_end);

  if (was_timed_out) signal_process(child_pid, SIGKILL);
  int wait_status = 0;
  waitpid(child_pid, &wait_status, 0);

  if (was_timed_out) return None;
  return captured;
}

fn give_controlling_terminal_to(process p) wontthrow -> void
{
  if (!shell_has_controlling_terminal()) return;
  /* The handoff itself raises SIGTTOU, so it is ignored across the change. */
  void (*const previous)(int) = signal(SIGTTOU, SIG_IGN);
  tcsetpgrp(STDIN_FILENO, p);
  signal(SIGTTOU, previous);
}

fn give_controlling_terminal_to_process_group(i64 process_group_id) wontthrow
    -> void
{
  if (!shell_has_controlling_terminal() || process_group_id <= 0) return;
  void (*const previous)(int) = signal(SIGTTOU, SIG_IGN);
  tcsetpgrp(STDIN_FILENO, static_cast<pid_t>(process_group_id));
  signal(SIGTTOU, previous);
}

fn reclaim_controlling_terminal() wontthrow -> void
{
  if (!shell_has_controlling_terminal()) return;
  void (*const previous)(int) = signal(SIGTTOU, SIG_IGN);
  tcsetpgrp(STDIN_FILENO, getpgrp());
  signal(SIGTTOU, previous);
}

static fn fork_compound_stage(
    Maybe<descriptor> in_fd, Maybe<descriptor> out_fd, Maybe<descriptor> err_fd,
    SourceLocation location = {}, StringView source = {},
    process_group_mode process_group = process_group_mode::Inherit,
    i64 process_group_id = 0) throws -> process
{
  LOG(Debug, "forking a compound pipeline stage");

  const pid_t child_pid = check_syscall(fork());

  if (child_pid == 0) {
    /* A throw would unwind into the parent's evaluator, the child must exit
       directly. */
    try {
      if (process_group != process_group_mode::Inherit) {
        ASSERT(process_group != process_group_mode::Join ||
               process_group_id > 0);
        let const target_group = process_group == process_group_mode::Join
                                     ? static_cast<pid_t>(process_group_id)
                                     : 0;
        check_syscall(setpgid(0, target_group));
      }

      if (in_fd) {
        check_syscall(dup2(*in_fd, STDIN_FILENO));
        check_syscall(close(*in_fd));
      }
      if (out_fd) {
        check_syscall(dup2(*out_fd, STDOUT_FILENO));
        check_syscall(close(*out_fd));
      }
      if (err_fd) {
        check_syscall(dup2(*err_fd, STDERR_FILENO));
        check_syscall(close(*err_fd));
      }

      reset_signal_handlers();

#if defined KOSH_HAS_ADDRESS_SANITIZER
      __lsan_disable();
#endif
    } catch (const koshka::Error &e) {
      koshka::show_message(
          ErrorWithLocation{steal(location), e.message()}.to_string(source));
      koshka::flush();
      exit_process_immediately(1);
    } catch (...) {
      LOG(Debug,
          "swallowed an unknown error while preparing the forked stage child");
      exit_process_immediately(1);
    }
  }

  if (process_group != process_group_mode::Inherit) {
    let const target_group = process_group == process_group_mode::Join
                                 ? static_cast<pid_t>(process_group_id)
                                 : child_pid;
    (void) setpgid(child_pid, target_group);
  }

  return child_pid;
}

static fn fork_job_process() throws -> process
{
  LOG(Debug, "forking a mimicked job into its own process group");

  const pid_t child_pid = check_syscall(fork());

  if (child_pid == 0) {
    try {
      reset_signal_handlers();
      (void) setpgid(0, 0);

#if defined KOSH_HAS_ADDRESS_SANITIZER
      __lsan_disable();
#endif
    } catch (...) {
      exit_process_immediately(1);
    }
    return 0;
  }

  (void) setpgid(child_pid, child_pid);
  return child_pid;
}

fn try_fork_compound_stage(Maybe<descriptor> in_fd, Maybe<descriptor> out_fd,
                           Maybe<descriptor> err_fd, SourceLocation location,
                           StringView source, process_group_mode process_group,
                           i64 process_group_id) throws -> Maybe<process>
{
  return fork_compound_stage(in_fd, out_fd, err_fd, location, source,
                             process_group, process_group_id);
}

fn try_fork_job_process() throws -> Maybe<process>
{
  return fork_job_process();
}

fn can_fork_evaluator() wontthrow -> bool { return true; }

fn launch_process_substitution(StringView source, bool command_writes_pipe,
                               bool bash_compatible,
                               bool source_traces_enabled) throws
    -> process_substitution_launch
{
  unused(source);
  unused(bash_compatible);
  unused(source_traces_enabled);

  let const pipe = make_pipe();
  if (!pipe.has_value())
    throw Error{"Could not open a pipe for the process substitution: " +
                last_system_error_message()};

  bool was_pipe_handed_off = false;
  defer
  {
    if (!was_pipe_handed_off) {
      close_fd(pipe->in);
      close_fd(pipe->out);
    }
  };

  const process child = command_writes_pipe
                            ? fork_compound_stage(None, pipe->out, None)
                            : fork_compound_stage(pipe->in, None, None);
  was_pipe_handed_off = true;

  if (child == 0) {
    return process_substitution_launch{
        .child_close_fd = command_writes_pipe ? Maybe<descriptor>{pipe->in}
                                              : Maybe<descriptor>{pipe->out},
        .child = child,
        .should_evaluate_child = true,
    };
  }

  const descriptor retained_fd = command_writes_pipe ? pipe->in : pipe->out;
  close_fd(command_writes_pipe ? pipe->out : pipe->in);
  make_fd_inheritable(retained_fd);

  let path = String{"/dev/fd/"};
  path += String::from(static_cast<i64>(retained_fd), heap_allocator());
  return process_substitution_launch{
      .path = steal(path),
      .retained_fd = retained_fd,
      .child = child,
  };
}

fn launch_compound_stage(StringView source, Maybe<descriptor> in_fd,
                         Maybe<descriptor> out_fd, Maybe<descriptor> err_fd,
                         mimic_mood mood, SourceLocation location,
                         StringView diagnostic_source,
                         process_group_mode process_group,
                         i64 process_group_id) throws -> compound_stage_launch
{
  unused(source);
  unused(mood);
  const process child =
      fork_compound_stage(in_fd, out_fd, err_fd, location, diagnostic_source,
                          process_group, process_group_id);
  return compound_stage_launch{
      .child = child,
      .should_evaluate_child = child == 0,
  };
}

[[noreturn]] fn exit_process_immediately(i32 status) wontthrow -> void
{
  _exit(status);
}

fn replace_process(ExecContext &&ec) throws -> void
{
  ASSERT(ec.args().count() > 0, "a program needs at least argv[0]");

  LOG(Debug, "replacing the shell process with '%s'",
      ec.program_path().c_str());

  let const child_args = make_os_args(ec.args());

  if (ec.in_fd) {
    check_syscall(dup2(*ec.in_fd, STDIN_FILENO));
    if (*ec.in_fd != STDIN_FILENO) check_syscall(close(*ec.in_fd));
  }
  if (ec.out_fd) {
    check_syscall(dup2(*ec.out_fd, STDOUT_FILENO));
    if (*ec.out_fd != STDOUT_FILENO) check_syscall(close(*ec.out_fd));
  }
  if (ec.err_fd) {
    check_syscall(dup2(*ec.err_fd, STDERR_FILENO));
    if (*ec.err_fd != STDERR_FILENO) check_syscall(close(*ec.err_fd));
  }
  ec.apply_dup_routing(
      [&]() { check_syscall(dup2(STDOUT_FILENO, STDERR_FILENO)); },
      [&]() { check_syscall(dup2(STDERR_FILENO, STDOUT_FILENO)); });

  sigset_t saved_signal_mask;
  struct sigaction saved_sigchild_action = {};
  struct sigaction saved_sigint_action = {};
  struct sigaction saved_sigpipe_action = {};
  let const saved_interrupt_requested = INTERRUPT_REQUESTED;
  check_syscall(sigprocmask(SIG_SETMASK, nullptr, &saved_signal_mask));
  check_syscall(sigaction(SIGCHLD, nullptr, &saved_sigchild_action));
  check_syscall(sigaction(SIGINT, nullptr, &saved_sigint_action));
  check_syscall(sigaction(SIGPIPE, nullptr, &saved_sigpipe_action));
  defer
  {
    sigaction(SIGCHLD, &saved_sigchild_action, nullptr);
    sigaction(SIGINT, &saved_sigint_action, nullptr);
    sigaction(SIGPIPE, &saved_sigpipe_action, nullptr);
    INTERRUPT_REQUESTED = saved_interrupt_requested;
    sigprocmask(SIG_SETMASK, &saved_signal_mask, nullptr);
  };

  reset_signal_handlers();

  /* exec -c replaces the inherited environ with a single null, so the program
     starts with an empty environment. execve takes the envp explicitly where
     execv would have read environ. */
  char *const empty_environment[] = {nullptr};
  execve(ec.program_path().c_str(),
         const_cast<char *const *>(child_args.begin()),
         ec.should_use_empty_environment
             ? const_cast<char *const *>(empty_environment)
             : environ);

  let const exec_error = errno;
  if (exec_error == ENOEXEC) return;
  /* The reason is read before the concatenation, which allocates and could
     clobber errno. */
  errno = exec_error;
  let const reason = last_system_error_message();
  let error = koshka::ErrorWithLocation{
      ec.source_location(),
      "Unable to execute `" + ec.program_path().text() + "`: " + reason};
  error.set_command_status(exec_error == ENOENT ? 127 : 126);
  throw error;
}

fn redirect_self(const ExecContext &ec) throws -> void
{
  if (ec.in_fd) check_syscall(dup2(*ec.in_fd, STDIN_FILENO));
  if (ec.out_fd) check_syscall(dup2(*ec.out_fd, STDOUT_FILENO));
  if (ec.err_fd) check_syscall(dup2(*ec.err_fd, STDERR_FILENO));
}

fn make_pipe() wontthrow -> Maybe<Pipe>
{
  LOG(Debug, "opening a close-on-exec pipe");

  descriptor p[2] = {KOSH_INVALID_FD, KOSH_INVALID_FD};

  if (pipe(p) != 0) {
    return koshka::None;
  }

  for (descriptor end : p) {
    const int flags = fcntl(end, F_GETFD);
    if (flags != -1) fcntl(end, F_SETFD, flags | FD_CLOEXEC);
  }

  return Pipe{p[0], p[1]};
}

struct thread_start_context
{
  void (*entry)(opaque *);
  opaque *context;
};

fn thread_trampoline(opaque *raw_context) wontthrow -> opaque *
{
  let const start = static_cast<thread_start_context *>(raw_context);
  let const entry = start->entry;
  let const context = start->context;
  os::free_aligned(start);
  entry(context);
  return nullptr;
}

fn start_thread(void (*entry)(opaque *), opaque *context) wontthrow
    -> Maybe<thread>
{
  let const storage = os::allocate_aligned(sizeof(thread_start_context),
                                           alignof(thread_start_context));
  if (storage == nullptr) return koshka::None;
  let const start = new (storage) thread_start_context{entry, context};
  pthread_t handle{};
  if (pthread_create(&handle, nullptr, thread_trampoline, start) != 0) {
    os::free_aligned(start);
    return koshka::None;
  }
  return thread{handle};
}

fn join_thread(thread t) wontthrow -> void { pthread_join(t.handle, nullptr); }

fn open_file_descriptor(StringView path, file_open_mode mode) throws
    -> Maybe<descriptor>
{
  LOG(Debug, "opening '%.*s'", static_cast<int>(path.length), path.data);

  /* Left inheritable on purpose, exec 3>file keeps the fd open across an exec.
   */
  int flags = 0;
  switch (mode) {
  case file_open_mode::Truncate: flags = O_WRONLY | O_CREAT | O_TRUNC; break;
  case file_open_mode::TruncateNoClobber:
    /* O_EXCL fails atomically when the file exists, the way noclobber requires.
     */
    flags = O_WRONLY | O_CREAT | O_EXCL;
    break;
  case file_open_mode::Append: flags = O_WRONLY | O_CREAT | O_APPEND; break;
  case file_open_mode::Read: flags = O_RDONLY; break;
  case file_open_mode::ReadWrite: flags = O_RDWR | O_CREAT; break;
  }

  const String path_string{path};
  const int fd = ::open(path_string.c_str(), flags, 0666);
  if (fd < 0) return koshka::None;
  return fd;
}

fn acquire_process_lock(StringView path) throws -> Maybe<descriptor>
{
  const String path_string{path};
  let const lock = ::open(path_string.c_str(), O_RDONLY | O_DIRECTORY);
  if (lock < 0) return None;
  if (::flock(lock, LOCK_EX) == 0) return lock;
  ::close(lock);
  return None;
}

fn release_process_lock(descriptor lock) wontthrow -> void
{
  unused(::flock(lock, LOCK_UN));
  unused(::close(lock));
}

fn write_to_temp_file(StringView content) throws -> Maybe<descriptor>
{
  LOG(Debug, "writing %zu bytes into an anonymous temp file", content.count());

  let const temp_dir = Path::temp_directory();

  let const path_template_path =
      PathBuilder{temp_dir.text()}.append("kosh_heredoc_XXXXXX").build();

  /* mkstemp rewrites the XXXXXX suffix in place, so the template is mutable. */
  const String &path_template_text = path_template_path.text();
  ArrayList<char> path_template{heap_allocator()};
  path_template.reserve(path_template_text.count() + 1);
  for (usize i = 0; i < path_template_text.count(); i++)
    path_template.push(path_template_text.c_str()[i]);
  path_template.push('\0');

  const int fd = mkstemp(path_template.begin());
  if (fd < 0) return koshka::None;

  unlink(path_template.begin());

  if (!write_all(fd, content.data, content.count())) {
    close(fd);
    return koshka::None;
  }

  if (lseek(fd, 0, SEEK_SET) < 0) {
    close(fd);
    return koshka::None;
  }
  return fd;
}

fn write_to_named_temp_file(const Path &directory, StringView prefix,
                            StringView content) throws -> Maybe<Path>
{
  if (prefix.find_character('/').has_value()) return None;
  let file_name = String{heap_allocator(), prefix};
  file_name += "_XXXXXX";
  let const path_template_path =
      PathBuilder{directory.text()}.append(file_name).build();
  let path_template = ArrayList<char>{heap_allocator()};
  path_template.reserve(path_template_path.count() + 1);
  for (usize index = 0; index < path_template_path.count(); index++)
    path_template.push(path_template_path.c_str()[index]);
  path_template.push('\0');

  const int fd = ::mkstemp(path_template.begin());
  if (fd < 0) return None;

  if (!write_all(fd, content.data, content.count())) {
    unused(::close(fd));
    unused(::unlink(path_template.begin()));
    return None;
  }

  if (::close(fd) != 0) {
    unused(::unlink(path_template.begin()));
    return None;
  }

  return Path{StringView{path_template.begin()}};
}

fn wait_and_monitor_process(process pid, bool *was_stopped) throws -> i32
{
  ASSERT(pid >= 0);

  LOG(Debug, "waiting on process %lld", static_cast<long long>(pid));

  i32 status{};
  const int wait_flags = was_stopped != nullptr ? WUNTRACED : 0;
  pid_t changed_pid = 0;

  loop
  {
    changed_pid = waitpid(pid, &status, wait_flags);
    /* A signal interrupted the wait. Retry instead of failing. */
    if (changed_pid == -1 && errno == EINTR) {
      continue;
    }
    check_syscall(changed_pid);
    break;
  }

  if (was_stopped != nullptr && WIFSTOPPED(status)) {
    *was_stopped = true;
    return 128 + WSTOPSIG(status);
  }

  if (WIFSIGNALED(status)) {
    const i32 sig = WTERMSIG(status);
    const char *sig_str = strsignal(sig);
    const String sig_desc =
        (sig_str != nullptr) ? String{sig_str} : String{"Unknown"};

    /* SIGPIPE is reaped silently the way bash and dash do, Ctrl-C prints a bare
       newline, every other signal prints the located process message. */
    if (sig == SIGPIPE) {
    } else if (sig != SIGINT) {
      koshka::print("[Process " + String::from(changed_pid, heap_allocator()) +
                    ": " + sig_desc + ", signal " +
                    String::from(sig, heap_allocator()) + "]\n");
    } else {
      koshka::print("\n");
    }

    return 128 + sig;
  } else if (!WIFEXITED(status)) {
    throw koshka::Error{"The process did not exit, was not signalled, and did "
                        "not stop: " +
                        last_system_error_message()};
  } else {
    return WEXITSTATUS(status);
  }
}

fn wait_for_child_state_change() wontthrow -> void
{
  sigset_t blocked_signals;
  sigemptyset(&blocked_signals);
  sigaddset(&blocked_signals, SIGCHLD);

  sigset_t previous_mask;
  if (sigprocmask(SIG_BLOCK, &blocked_signals, &previous_mask) != 0) return;

  while (CHILD_STATE_CHANGED == 0) {
    let wait_mask = previous_mask;
    sigdelset(&wait_mask, SIGCHLD);
    sigsuspend(&wait_mask);
  }

  CHILD_STATE_CHANGED = 0;
  (void) sigprocmask(SIG_SETMASK, &previous_mask, nullptr);
}

fn reap_process_quietly(process pid) throws -> i32
{
  ASSERT(pid >= 0);

  LOG(Debug, "quietly reaping process %lld", static_cast<long long>(pid));

  i32 status{};
  loop
  {
    const pid_t w = waitpid(pid, &status, 0);
    if (w == -1 && errno == EINTR) {
      continue;
    }
    /* The SIGCHLD handler may already have reaped it, a missing child is fine.
     */
    if (w == -1 && errno == ECHILD) {
      return 0;
    }
    if (check_syscall(w) == pid) break;
  }

  if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
  if (WIFEXITED(status)) return WEXITSTATUS(status);
  return 1;
}

fn poll_process(process p, i32 &status_out) wontthrow -> process_state
{
  i32 status = 0;
  pid_t result;
  do {
    result = waitpid(p, &status, WNOHANG | WUNTRACED | WCONTINUED);
  } while (result == -1 && errno == EINTR);

  if (result == 0) return process_state::Unchanged;
  if (result == -1) {
    status_out = 0;
    return process_state::Exited;
  }

  if (WIFSTOPPED(status)) {
    status_out = 128 + WSTOPSIG(status);
    return process_state::Stopped;
  }
  if (WIFCONTINUED(status)) return process_state::Running;
  if (WIFSIGNALED(status)) {
    status_out = 128 + WTERMSIG(status);
    return process_state::Exited;
  }
  status_out = WEXITSTATUS(status);
  return process_state::Exited;
}

fn signal_process(process p, i32 signal_number) wontthrow -> bool
{
  return kill(p, signal_number) == 0;
}

fn process_group_has_members(process group) wontthrow -> bool
{
  if (kill(group, 0) == 0) return true;
  return errno == EPERM;
}

fn is_process_signal_supported(i32 signal_number) wontthrow -> bool
{
  return signal_number >= 0 && signal_number < NSIG;
}

fn process_from_pid(i64 pid) wontthrow -> process
{
  return static_cast<process>(pid);
}

struct signal_pair
{
  i32 number;
  StringView name;
};
static const signal_pair SIGNAL_PAIRS[] = {
    {SIGHUP,  "HUP" },
    {SIGINT,  "INT" },
    {SIGQUIT, "QUIT"},
    {SIGKILL, "KILL"},
    {SIGTERM, "TERM"},
    {SIGSTOP, "STOP"},
    {SIGTSTP, "TSTP"},
    {SIGCONT, "CONT"},
    {SIGUSR1, "USR1"},
    {SIGUSR2, "USR2"},
    {SIGABRT, "ABRT"},
    {SIGALRM, "ALRM"},
    {SIGPIPE, "PIPE"},
};

fn signal_number_from_name(StringView name) throws -> Maybe<i32>
{
  if (name.is_all_decimal_digits()) {
    const ErrorOr<i64> parsed_value = name.to<i64>();
    if (parsed_value.is_error() || parsed_value.value() < INT32_MIN ||
        parsed_value.value() > INT32_MAX)
      return koshka::None;
    return static_cast<i32>(parsed_value.value());
  }

  let const bare = utils::strip_sig_prefix(name);

  for (let const &pair : SIGNAL_PAIRS)
    if (pair.name == bare) return pair.number;
  return koshka::None;
}

fn signal_name_from_number(i32 number) throws -> Maybe<String>
{
  for (let const &pair : SIGNAL_PAIRS)
    if (pair.number == number) return String{pair.name};
  return koshka::None;
}

fn signal_names() throws -> const ArrayList<StringView> &
{
  static ArrayList<StringView> names = [] throws {
    let collected = ArrayList<StringView>{heap_allocator()};
    collected.reserve(sizeof(SIGNAL_PAIRS) / sizeof(SIGNAL_PAIRS[0]));
    for (let const &pair : SIGNAL_PAIRS)
      collected.push(pair.name);
    return collected;
  }();
  return names;
}

hot fn make_os_args(const ArrayList<String> &args) throws -> os_args
{
  ASSERT(args.count() > 0, "argv must carry at least the program name");

  os_args result{heap_allocator()};
  result.reserve(args.count() + 1);

  for (let const &arg : args)
    result.push(arg.c_str());

  result.push(nullptr);

  return result;
}

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

fn monotonic_nanos() wontthrow -> u64
{
  struct timespec now{};
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
  return static_cast<u64>(now.tv_sec) * 1000000000ULL +
         static_cast<u64>(now.tv_nsec);
}

fn get_parent_process_id() wontthrow -> i64
{
  return static_cast<i64>(getppid());
}

fn get_real_user_id() wontthrow -> i64 { return static_cast<i64>(getuid()); }

fn get_effective_user_id() wontthrow -> i64
{
  return static_cast<i64>(geteuid());
}

fn get_real_group_id() wontthrow -> i64 { return static_cast<i64>(getgid()); }

fn child_max() wontthrow -> i64
{
  return static_cast<i64>(sysconf(_SC_CHILD_MAX));
}

fn machine_type() throws -> String
{
  static const String cached = []() -> String {
    struct utsname info{};
    if (uname(&info) != 0) return String{"unknown"};
    return String{
        StringView{info.machine, std::strlen(info.machine)}
    };
  }();
  return cached;
}

fn executable_system_name() throws -> String
{
#if KOSH_PLATFORM_IS COSMO
  return String{"any"};
#elif defined __APPLE__
  return String{"Darwin"};
#elif defined __linux__
  return String{"Linux"};
#else
  struct utsname info{};
  if (uname(&info) != 0) return String{"unknown"};
  return String{
      StringView{info.sysname, std::strlen(info.sysname)}
  };
#endif
}

fn executable_machine_name() throws -> String
{
#if KOSH_PLATFORM_IS COSMO
  return String{"any"};
#elif defined __aarch64__ || defined __arm64__
  return String{"arm64"};
#elif defined __x86_64__ || defined __amd64__
  return String{"x86_64"};
#else
  return machine_type();
#endif
}

fn realtime_microseconds() wontthrow -> u64
{
  struct timespec now{};
  if (clock_gettime(CLOCK_REALTIME, &now) != 0) return 0;
  return static_cast<u64>(now.tv_sec) * 1000000ULL +
         static_cast<u64>(now.tv_nsec) / 1000ULL;
}

fn format_local_time(StringView format, i64 epoch) throws -> String
{
  /* A negative epoch is the current time, so a fixed value renders a fixed time
     while the bash -1 and -2 magic values track the clock. */
  const time_t when = epoch < 0 ? time(nullptr) : static_cast<time_t>(epoch);
  struct tm broken_down{};
  /* localtime_r returns null and leaves the struct unspecified for an epoch
     outside the representable range, so an unchecked struct would feed strftime
     garbage. An out-of-range time renders as empty rather than a wrong date. */
  if (localtime_r(&when, &broken_down) == nullptr)
    return String{heap_allocator()};
  let const format_string = String{format};
  char buffer[512];
  let const written =
      strftime(buffer, sizeof(buffer), format_string.c_str(), &broken_down);
  return String{
      StringView{buffer, written}
  };
}

fn children_cpu_seconds(double &user_seconds, double &system_seconds) wontthrow
    -> void
{
  struct rusage usage{};
  if (getrusage(RUSAGE_CHILDREN, &usage) != 0) {
    user_seconds = 0;
    system_seconds = 0;
    return;
  }
  user_seconds = static_cast<double>(usage.ru_utime.tv_sec) +
                 static_cast<double>(usage.ru_utime.tv_usec) / 1000000.0;
  system_seconds = static_cast<double>(usage.ru_stime.tv_sec) +
                   static_cast<double>(usage.ru_stime.tv_usec) / 1000000.0;
}

fn children_peak_rss_bytes() wontthrow -> u64
{
  struct rusage usage{};
  if (getrusage(RUSAGE_CHILDREN, &usage) != 0) return 0;

  return platform_peak_rss_bytes(usage.ru_maxrss);
}

namespace {

struct measured_child
{
  pid_t pid;
  int start_descriptor;
};

fn transfer_barrier_byte(int descriptor, bool should_write) wontthrow -> bool
{
  char byte = 1;
  ssize_t transfer_count;
  do {
    transfer_count =
        should_write ? write(descriptor, &byte, 1) : read(descriptor, &byte, 1);
  } while (transfer_count == -1 && errno == EINTR);

  return transfer_count == 1;
}

fn spawn_measured_child(const ArrayList<String> &argv, measured_output output,
                        measured_child &child_out) wontthrow -> bool
{
  let const raw_argv = make_os_args(argv);

  int ready_descriptors[2];
  if (pipe(ready_descriptors) != 0) return false;

  int start_descriptors[2];
  if (pipe(start_descriptors) != 0) {
    close(ready_descriptors[0]);
    close(ready_descriptors[1]);
    return false;
  }

  const pid_t child_pid = fork();
  if (child_pid == -1) {
    close(ready_descriptors[0]);
    close(ready_descriptors[1]);
    close(start_descriptors[0]);
    close(start_descriptors[1]);
    return false;
  }

  if (child_pid == 0) {
    close(ready_descriptors[0]);
    close(start_descriptors[1]);
    signal(SIGPIPE, SIG_DFL);
    if (output == measured_output::Suppress) {
      const int null_fd = open("/dev/null", O_WRONLY);
      if (null_fd != -1) {
        dup2(null_fd, STDOUT_FILENO);
        dup2(null_fd, STDERR_FILENO);
        if (null_fd != STDOUT_FILENO && null_fd != STDERR_FILENO) {
          close(null_fd);
        }
      }
    }

    let const is_ready = transfer_barrier_byte(ready_descriptors[1], true);
    close(ready_descriptors[1]);
    let const should_start = transfer_barrier_byte(start_descriptors[0], false);
    close(start_descriptors[0]);
    if (!is_ready || !should_start) _exit(127);

    execvp(raw_argv[0], const_cast<char *const *>(raw_argv.begin()));
    _exit(127);
  }

  close(ready_descriptors[1]);
  close(start_descriptors[0]);
  let const is_ready = transfer_barrier_byte(ready_descriptors[0], false);
  close(ready_descriptors[0]);
  if (!is_ready) {
    close(start_descriptors[1]);
    while (waitpid(child_pid, nullptr, 0) == -1 && errno == EINTR) {}
    return false;
  }

  child_out = {child_pid, start_descriptors[1]};
  return true;
}

fn wait_for_measured_child(pid_t child_pid, i64 &status_out,
                           u64 &peak_rss_out) wontthrow -> bool
{

  int status = 0;
  struct rusage usage{};
  pid_t waited = -1;
  loop
  {
    waited = wait4(child_pid, &status, 0, &usage);
    if (waited == -1 && errno == EINTR) {
      continue;
    }
    break;
  }

  if (waited != child_pid) {
    if (waited == -1 && errno != ECHILD) {
      kill(child_pid, SIGKILL);
      while (waitpid(child_pid, nullptr, 0) == -1 && errno == EINTR) {}
    }
    return false;
  }

  if (WIFEXITED(status))
    status_out = WEXITSTATUS(status);
  else if (WIFSIGNALED(status))
    status_out = 128 + WTERMSIG(status);
  else
    status_out = -1;

  peak_rss_out = platform_peak_rss_bytes(usage.ru_maxrss);

  return true;
}

} /* namespace */

fn run_measured(const ArrayList<String> &argv, measured_output output,
                Maybe<descriptor> inherited_handle) throws
    -> Maybe<measured_result>
{
  unused(inherited_handle);
  if (argv.is_empty()) return None;

  measured_result result{};

  measured_child child{};
  if (!spawn_measured_child(argv, output, child)) return None;

  PlatformPerfSession perf_session;
  bool has_perf = perf_session.prepare(child.pid);
  if (has_perf) has_perf = perf_session.start();
  if (!has_perf) perf_session.cancel();

  const u64 start_nanos = monotonic_nanos();
  let const did_release = transfer_barrier_byte(child.start_descriptor, true);
  close(child.start_descriptor);
  if (!did_release) {
    kill(child.pid, SIGKILL);
    while (waitpid(child.pid, nullptr, 0) == -1 && errno == EINTR) {}
    return None;
  }

  if (!wait_for_measured_child(child.pid, result.exit_status,
                               result.peak_rss_bytes))
    return None;
  result.wall_nanos = monotonic_nanos() - start_nanos;

  if (has_perf) {
    result.has_perf = perf_session.finish(result.perf);
    result.is_perf_system_wide =
        result.has_perf && perf_session.is_system_wide();
  }
  return result;
}

/* The String destructor may clobber errno, so each helper saves it across the
   inner scope that ends the String first. */
fn make_directory(StringView path, u32 mode) wontthrow -> bool
{
  bool did_succeed;
  int saved_errno;
  {
    const String path_string{path};
    did_succeed = ::mkdir(path_string.c_str(), mode) == 0;
    saved_errno = errno;
  }
  errno = saved_errno;
  return did_succeed;
}

fn set_file_mode(StringView path, u32 mode) wontthrow -> bool
{
  bool did_succeed;
  int saved_errno;
  {
    const String path_string{path};
    did_succeed = ::chmod(path_string.c_str(), mode) == 0;
    saved_errno = errno;
  }
  errno = saved_errno;
  return did_succeed;
}

fn touch_file_times(StringView path) wontthrow -> bool
{
  bool did_succeed;
  int saved_errno;
  {
    const String path_string{path};
    did_succeed = ::utimensat(AT_FDCWD, path_string.c_str(), nullptr, 0) == 0;
    saved_errno = errno;
  }
  errno = saved_errno;
  return did_succeed;
}

fn remove_directory(StringView path) wontthrow -> bool
{
  bool did_succeed;
  int saved_errno;
  {
    const String path_string{path};
    did_succeed = ::rmdir(path_string.c_str()) == 0;
    saved_errno = errno;
  }
  errno = saved_errno;
  return did_succeed;
}

fn remove_file(StringView path) wontthrow -> bool
{
  bool did_succeed;
  int saved_errno;
  {
    const String path_string{path};
    did_succeed = ::unlink(path_string.c_str()) == 0;
    saved_errno = errno;
  }
  errno = saved_errno;
  return did_succeed;
}

fn rename_path(StringView from, StringView to) wontthrow -> bool
{
  bool did_succeed;
  int saved_errno;
  {
    const String from_string{from};
    const String to_string{to};
    did_succeed = ::rename(from_string.c_str(), to_string.c_str()) == 0;
    saved_errno = errno;
  }
  errno = saved_errno;
  return did_succeed;
}

fn create_symlink(StringView target, StringView link_path) wontthrow -> bool
{
  bool did_succeed;
  int saved_errno;
  {
    const String target_string{target};
    const String link_string{link_path};
    did_succeed = ::symlink(target_string.c_str(), link_string.c_str()) == 0;
    saved_errno = errno;
  }
  errno = saved_errno;
  return did_succeed;
}

fn read_symlink(StringView path) wontthrow -> Maybe<String>
{
  const String path_string{path};
  /* readlink cannot flag truncation, so the buffer grows until it stops
     filling. */
  usize capacity = 256;
  loop
  {
    ArrayList<char> buffer{heap_allocator()};
    buffer.reserve(capacity);
    let const length =
        ::readlink(path_string.c_str(), buffer.begin(), capacity);
    if (length < 0) return koshka::None;
    if (static_cast<usize>(length) < capacity)
      return String{
          StringView{buffer.begin(), static_cast<usize>(length)}
      };

    if (capacity >= (1U << 20)) return koshka::None;
    capacity *= 2;
  }
}

static fn fill_file_status(const struct stat &info,
                           file_status &status) wontthrow -> void
{
  status.device_id = static_cast<u64>(info.st_dev);
  status.file_id = static_cast<u64>(info.st_ino);
  status.has_file_identity = true;
  status.mode = static_cast<u32>(info.st_mode);
  status.link_count = static_cast<u64>(info.st_nlink);
  status.owner_id = static_cast<u32>(info.st_uid);
  status.group_id = static_cast<u32>(info.st_gid);
  status.size = static_cast<u64>(info.st_size);
  status.modification_time = static_cast<i64>(info.st_mtime);
  status.modification_nanoseconds = static_cast<u32>(info.st_mtim.tv_nsec);
  status.change_time = static_cast<i64>(info.st_ctime);
  status.change_nanoseconds = static_cast<u32>(info.st_ctim.tv_nsec);
  status.blocks = static_cast<u64>(info.st_blocks);
}

fn stat_path(StringView path, file_status &status) wontthrow -> bool
{
  const String path_string{path};
  struct stat info{};
  /* lstat does not follow the symlink, so ls shows the l type without -L. */
  if (::lstat(path_string.c_str(), &info) != 0) return false;
  fill_file_status(info, status);
  return true;
}

fn stat_path_following(StringView path, file_status &status) wontthrow -> bool
{
  const String path_string{path};
  struct stat info{};
  if (::stat(path_string.c_str(), &info) != 0) return false;
  fill_file_status(info, status);
  return true;
}

fn file_type_letter(u32 mode) wontthrow -> char
{
  const mode_t bits = static_cast<mode_t>(mode);
  if (S_ISDIR(bits)) return 'd';
  if (S_ISLNK(bits)) return 'l';
  if (S_ISCHR(bits)) return 'c';
  if (S_ISBLK(bits)) return 'b';
  if (S_ISFIFO(bits)) return 'p';
  if (S_ISSOCK(bits)) return 's';
  return '-';
}

fn format_mode_string(u32 mode) throws -> String
{
  const mode_t bits = static_cast<mode_t>(mode);
  String result{heap_allocator()};
  result.push(file_type_letter(mode));
  result.push((bits & S_IRUSR) != 0 ? 'r' : '-');
  result.push((bits & S_IWUSR) != 0 ? 'w' : '-');
  result.push((bits & S_ISUID) != 0 ? ((bits & S_IXUSR) != 0 ? 's' : 'S')
                                    : ((bits & S_IXUSR) != 0 ? 'x' : '-'));
  result.push((bits & S_IRGRP) != 0 ? 'r' : '-');
  result.push((bits & S_IWGRP) != 0 ? 'w' : '-');
  result.push((bits & S_ISGID) != 0 ? ((bits & S_IXGRP) != 0 ? 's' : 'S')
                                    : ((bits & S_IXGRP) != 0 ? 'x' : '-'));
  result.push((bits & S_IROTH) != 0 ? 'r' : '-');
  result.push((bits & S_IWOTH) != 0 ? 'w' : '-');
  result.push((bits & S_ISVTX) != 0 ? ((bits & S_IXOTH) != 0 ? 't' : 'T')
                                    : ((bits & S_IXOTH) != 0 ? 'x' : '-'));
  return result;
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

#if KOSH_PLATFORM_IS COSMO

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
#if KOSH_PLATFORM_IS COSMO
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
#if KOSH_PLATFORM_IS COSMO
  ShowCrashReports();
#endif
}

} /* namespace os */
} /* namespace koshka */
