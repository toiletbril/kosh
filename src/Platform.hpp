#pragma once

#include "Common.hpp"

#define KOSH_PLATFORM_POSIX 0b1
#define KOSH_PLATFORM_WIN32 0b10
#define KOSH_PLATFORM_COSMO 0b100

/* clang-format off */
#if defined __linux__ || defined BSD || defined __APPLE__ ||                   \
    defined __COSMOPOLITAN__
#include <dirent.h>
#include <fcntl.h>
#include <glob.h>
#include <poll.h>
#include <pthread.h>
#include <pwd.h>
#include <regex.h>
#include <spawn.h>
#include <sys/ioctl.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#if defined CEOF
#undef CEOF
#endif
#if defined __linux__
#include <linux/perf_event.h>
#include <sched.h>
#include <sys/syscall.h>
#endif
#if defined __GLIBC__
#include <malloc.h>
#endif
#if defined __APPLE__
#include <dlfcn.h>
#include <libproc.h>
#include <mach-o/dyld.h>
#include <sys/proc.h>
#include <sys/proc_info.h>
#include <sys/sysctl.h>
#endif
#if defined __SANITIZE_ADDRESS__
extern "C" void __lsan_disable(void);
#define KOSH_HAS_ADDRESS_SANITIZER 1
#endif
#if defined __COSMOPOLITAN__
#include <libc/dce.h>
#include <libc/runtime/runtime.h>
#define KOSH_SUPPORT_VECTOR (KOSH_PLATFORM_COSMO | KOSH_PLATFORM_POSIX)
#else
#define KOSH_SUPPORT_VECTOR (KOSH_PLATFORM_POSIX)
#endif
#elif defined _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#include <direct.h>
#include <io.h>
#include <malloc.h>
#include <psapi.h>
#include <sys/stat.h>
#include <tlhelp32.h>
#define KOSH_SUPPORT_VECTOR (KOSH_PLATFORM_WIN32)
#endif
/* clang-format on */

#define KOSH_PLATFORM_IS   (KOSH_SUPPORT_VECTOR) &
#define KOSH_PLATFORM_ISNT (~KOSH_SUPPORT_VECTOR) &

#if defined SIGPIPE
#define KOSH_BROKEN_PIPE_EXIT_STATUS (128 + SIGPIPE)
#else
#define KOSH_BROKEN_PIPE_EXIT_STATUS 141
#endif

#if KOSH_PLATFORM_IS KOSH_PLATFORM_POSIX
extern char **environ;
#endif

namespace koshka::os {

hot alwaysinline fn pack_little_endian_bytes(u64 *words, const char *bytes,
                                             usize count) wontthrow -> void
{
#if defined __BYTE_ORDER__ && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  __builtin_memcpy(reinterpret_cast<char *>(words), bytes, count);
#else
  for (usize index = 0; index < count; index++)
    words[index / 8] |= static_cast<u64>(static_cast<u8>(bytes[index]))
                        << (8 * (index % 8));
#endif
}

} /* namespace koshka::os */

#include "ArrayList.hpp"
#include "Maybe.hpp"
#include "Path.hpp"
#include "String.hpp"

namespace koshka {

class ExecContext;
class Flag;
class FlagList;
enum class mimic_mood : u8;

namespace os {

enum class process_detail : u8
{
  Basic,
  ResourceStats,
};

enum class case_sensitivity : u8
{
  Sensitive,
  Insensitive,
};

enum class signal_profile : u8
{
  NonInteractive,
  Interactive,
};

enum class measured_output : u8
{
  Inherit,
  Suppress,
};

enum class script_fallback_policy : u8
{
  Reject,
  Allow,
};

enum class process_group_mode : u8
{
  Inherit,
  New,
  NewBackground,
  Join,
  NewLeaderOwned,
};

pure constexpr fn background_process_group_mode(i64 process_group_id) wontthrow
    -> process_group_mode
{
#if KOSH_PLATFORM_IS KOSH_PLATFORM_WIN32
  unused(process_group_id);
  return process_group_mode::NewBackground;
#else
  return process_group_id == 0 ? process_group_mode::NewBackground
                               : process_group_mode::Join;
#endif
}

enum class terminal_handoff : u8
{
  Keep,
  BeforeStart,
};

#if KOSH_PLATFORM_IS KOSH_PLATFORM_WIN32
constexpr char PATH_DELIMITER = ';';
constexpr char DIRECTORY_SEPARATOR = '\\';
constexpr bool FILESYSTEM_IS_CASE_SENSITIVE = false;
constexpr bool HAS_CHILD_STATE_CHANGE_WAIT = false;

using process = HANDLE;
using descriptor = HANDLE;
using os_args = String;

#define KOSH_INVALID_FD      INVALID_HANDLE_VALUE
#define KOSH_INVALID_PROCESS INVALID_HANDLE_VALUE

#define KOSH_STDIN  GetStdHandle(STD_INPUT_HANDLE)
#define KOSH_STDOUT GetStdHandle(STD_OUTPUT_HANDLE)
#define KOSH_STDERR GetStdHandle(STD_ERROR_HANDLE)

#elif KOSH_PLATFORM_IS KOSH_PLATFORM_POSIX
constexpr char PATH_DELIMITER = ':';
constexpr char DIRECTORY_SEPARATOR = '/';
constexpr bool FILESYSTEM_IS_CASE_SENSITIVE = true;
constexpr bool HAS_CHILD_STATE_CHANGE_WAIT = true;

using process = pid_t;
using descriptor = int;
using os_args = ArrayList<const char *>;

#define KOSH_INVALID_FD      -1
#define KOSH_INVALID_PROCESS -1

#define KOSH_STDIN  STDIN_FILENO
#define KOSH_STDOUT STDOUT_FILENO
#define KOSH_STDERR STDERR_FILENO
#endif

class terminal_echo_guard
{
public:
  terminal_echo_guard(descriptor input, bool should_disable) wontthrow;
  ~terminal_echo_guard();

  terminal_echo_guard(const terminal_echo_guard &) = delete;
  fn operator=(const terminal_echo_guard &)->terminal_echo_guard & = delete;

  mustuse fn did_succeed() const wontthrow -> bool;

private:
  descriptor m_input{KOSH_INVALID_FD};
#if KOSH_PLATFORM_IS KOSH_PLATFORM_WIN32
  DWORD m_original_mode{0};
#else
  termios m_original_mode{};
#endif
  bool m_should_restore{false};
  bool m_did_succeed{true};
};

enum class program_extension : u8
{
  None,
  Exe,
  Com,
  Scr,
  Bat,
};

struct program_name_info
{
  program_extension extension{program_extension::None};
  usize stem_length{0};
};

struct program_suffix
{
  program_extension extension{program_extension::None};
  u32 packed_name{0};
  StringView text{};
};

inline constexpr program_suffix WINDOWS_PROGRAM_SUFFIXES[] = {
    {program_extension::None, 0,                                     StringView{"", 0}},
    {program_extension::Exe,
     static_cast<u32>('e') << 16 | static_cast<u32>('x') << 8 | 'e',
     StringView{".exe", 4}                                                            },
    {program_extension::Com,
     static_cast<u32>('c') << 16 | static_cast<u32>('o') << 8 | 'm',
     StringView{".com", 4}                                                            },
    {program_extension::Scr,
     static_cast<u32>('s') << 16 | static_cast<u32>('c') << 8 | 'r',
     StringView{".scr", 4}                                                            },
    {program_extension::Bat,
     static_cast<u32>('b') << 16 | static_cast<u32>('a') << 8 | 't',
     StringView{".bat", 4}                                                            },
};

inline constexpr program_suffix POSIX_PROGRAM_SUFFIXES[] = {
    {program_extension::None, 0, StringView{"", 0}}
};

class ProgramSuffixList
{
public:
  template <usize suffix_count>
  constexpr ProgramSuffixList(const program_suffix (&data)[suffix_count])
      : m_data(data), m_count(suffix_count)
  {}

  pure constexpr fn count() const wontthrow -> usize { return m_count; }

  pure constexpr fn
  operator[](usize position) const wontthrow->const program_suffix &
  {
    return m_data[position];
  }

  pure constexpr fn begin() const wontthrow -> const program_suffix *
  {
    return m_data;
  }

  pure constexpr fn end() const wontthrow -> const program_suffix *
  {
    return m_data + m_count;
  }

private:
  const program_suffix *m_data{nullptr};
  usize m_count{0};
};

inline fn normalize_windows_program_name(String &program_name) throws
    -> program_name_info
{
  program_name.lowercase_ascii();
  let const length = program_name.length();
  program_extension extension = program_extension::None;
  if (length >= 4 && program_name[length - 4] == '.') {
    let const packed_suffix = static_cast<u32>(program_name[length - 3]) << 16 |
                              static_cast<u32>(program_name[length - 2]) << 8 |
                              static_cast<u32>(program_name[length - 1]);

    for (let const &suffix : WINDOWS_PROGRAM_SUFFIXES) {
      if (suffix.packed_name == packed_suffix) {
        extension = suffix.extension;
        break;
      }
    }
  }

  return {extension,
          extension == program_extension::None ? length : length - 4};
}

struct Pipe
{
  descriptor in{KOSH_INVALID_FD};
  descriptor out{KOSH_INVALID_FD};
};

fn make_pipe() wontthrow -> Maybe<Pipe>;

struct thread
{
#if KOSH_PLATFORM_IS KOSH_PLATFORM_WIN32
  HANDLE handle{nullptr};
#elif KOSH_PLATFORM_IS KOSH_PLATFORM_POSIX
  pthread_t handle{};
#endif
};

fn start_thread(void (*entry)(opaque *), opaque *context) wontthrow
    -> Maybe<thread>;

fn join_thread(thread t) wontthrow -> void;

enum class file_open_mode : u8
{
  Truncate,          /* >  create or truncate for writing */
  TruncateNoClobber, /* >  under noclobber, fail if the file exists */
  Append,            /* >> create or append for writing */
  Read,              /* <  open an existing file for reading */
  ReadWrite,         /* <> create or open for reading and writing */
};

fn open_file_descriptor(StringView path, file_open_mode mode) throws
    -> Maybe<descriptor>;
fn acquire_process_lock(StringView path) throws -> Maybe<descriptor>;
fn release_process_lock(descriptor lock) wontthrow -> void;

fn write_to_temp_file(StringView content) throws -> Maybe<descriptor>;
fn write_to_named_temp_file(const Path &directory, StringView prefix,
                            StringView content) throws -> Maybe<Path>;

/* On a platform that leaves no temp file, such as POSIX, it holds nothing. */
class TempFileSet
{
public:
  fn track(Path path) throws -> void;
  mustuse fn count() const wontthrow -> usize;
  /* Delete every file tracked at or after the mark on a best-effort basis, and
     keep one still held open by a reader for a later retry. */
  fn cleanup_from(usize mark) wontthrow -> void;

private:
#if KOSH_PLATFORM_IS KOSH_PLATFORM_WIN32
  ArrayList<Path> m_paths{heap_allocator()};
#endif
};

fn get_file_creation_mask() wontthrow -> u32;
fn set_file_creation_mask(u32 mask) wontthrow -> void;

fn make_os_args(const ArrayList<String> &args) throws -> os_args;

fn last_system_error_message() throws -> String;
fn last_system_error_is_missing_file() wontthrow -> bool;

fn wait_and_monitor_process(process p, bool *was_stopped = nullptr) throws
    -> i32;
fn wait_for_child_state_change() wontthrow -> void;

fn reap_process_quietly(process p) throws -> i32;

/* Unchanged means the poll reported no new transition, so the caller keeps the
   state it already recorded. */
enum class process_state : u8
{
  Running,
  Exited,
  Stopped,
  Unchanged,
};

fn poll_process(process p, i32 &status_out) wontthrow -> process_state;

fn signal_process(process p, i32 signal_number) wontthrow -> bool;
fn process_group_has_members(process group) wontthrow -> bool;
fn is_process_signal_supported(i32 signal_number) wontthrow -> bool;

fn signal_number_from_name(StringView name) throws -> Maybe<i32>;

fn signal_name_from_number(i32 number) throws -> Maybe<String>;

fn signal_names() throws -> const ArrayList<StringView> &;

/* On Windows a handle is opened for it, which may be the invalid handle when
   the process is gone or not permitted. */
fn process_from_pid(i64 pid) wontthrow -> process;

/* One live process the koshkit pkill, killall, and ps utilities read, its
   numeric id, the basename of its command, the owner uid, and the full command
   line. The command line is empty for a process that exposes none. */
struct process_entry
{
  i64 pid{0};
  String name{heap_allocator()};
  u32 owner_id{0};
  String command_line{heap_allocator()};
  /* The BSD aux columns, filled only when resource stats are requested. */
  u64 virtual_kib{0};
  u64 resident_kib{0};
  char state{'?'};
  u64 cpu_ticks{0};
};

/* Every process the current user can see, for the koshkit pkill and killall
   utilities to match a name against. Empty on a platform with no process
   listing. The resource stats are read only when asked. */
fn enumerate_processes(process_detail detail = process_detail::Basic) throws
    -> ArrayList<process_entry>;

fn make_directory(StringView path, u32 mode) wontthrow -> bool;
fn set_file_mode(StringView path, u32 mode) wontthrow -> bool;
fn touch_file_times(StringView path) wontthrow -> bool;
fn remove_directory(StringView path) wontthrow -> bool;
fn remove_file(StringView path) wontthrow -> bool;
fn rename_path(StringView from, StringView to) wontthrow -> bool;
fn create_symlink(StringView target, StringView link_path) wontthrow -> bool;
fn read_symlink(StringView path) wontthrow -> Maybe<String>;

fn current_executable_path() wontthrow -> Maybe<String>;

fn crc32c_update(u32 crc, const void *data, usize length) wontthrow -> u32;

/* The mode carries the type and permission bits in the POSIX st_mode layout. */
struct file_status
{
  u64 device_id{0};
  u64 file_id{0};
  bool has_file_identity{false};
  u32 mode{0};
  u64 link_count{0};
  u32 owner_id{0};
  u32 group_id{0};
  u64 size{0};
  i64 modification_time{0};
  u32 modification_nanoseconds{0};
  i64 change_time{0};
  u32 change_nanoseconds{0};
  u64 blocks{0};
};

fn stat_path(StringView path, file_status &status) wontthrow -> bool;
fn stat_path_following(StringView path, file_status &status) wontthrow -> bool;

fn format_mode_string(u32 mode) throws -> String;

fn file_type_letter(u32 mode) wontthrow -> char;

/* The user name for a numeric uid and the group name for a numeric gid, read
   directly from /etc/passwd and /etc/group, so the static build stays free of
   getpwuid and getgrgid. */
fn uid_to_username(u32 uid) throws -> Maybe<String>;
fn gid_to_groupname(u32 gid) throws -> Maybe<String>;

fn sleep_for_seconds(double seconds) wontthrow -> void;

extern const ProgramSuffixList PROGRAM_SUFFIXES;

fn write_fd(os::descriptor fd, const opaque *buf, usize size) wontthrow
    -> Maybe<usize>;
inline fn write_all(os::descriptor fd, const opaque *buf, usize size) wontthrow
    -> bool
{
  usize written_size = 0;

  while (written_size < size) {
    let const result = write_fd(fd, static_cast<const u8 *>(buf) + written_size,
                                size - written_size);
    if (!result.has_value() || *result == 0) return false;

    written_size += *result;
  }

  return true;
}
fn write_to_numbered_fd(i64 fd_number, const opaque *buf, usize size) wontthrow
    -> Maybe<usize>;
fn read_fd(os::descriptor fd, opaque *buf, usize size) wontthrow
    -> Maybe<usize>;
fn descriptor_is_seekable(os::descriptor fd) wontthrow -> bool;
fn rewind_descriptor(os::descriptor fd, usize byte_count) wontthrow -> bool;
fn read_fd_to_string(os::descriptor fd, Allocator allocator) throws
    -> Maybe<String>;

fn wait_for_fd_readable(os::descriptor fd, i64 timeout_nanos) wontthrow -> i32;

fn close_fd(os::descriptor fd) wontthrow -> bool;

class DirectoryReference
{
public:
  DirectoryReference() = default;
  explicit DirectoryReference(descriptor descriptor) : m_descriptor(descriptor)
  {}
  DirectoryReference(const DirectoryReference &) = delete;
  DirectoryReference &operator=(const DirectoryReference &) = delete;
  DirectoryReference(DirectoryReference &&other) wontthrow
      : m_descriptor(other.take())
  {}
  DirectoryReference &operator=(DirectoryReference &&other) wontthrow
  {
    if (this == &other) return *this;
    if (is_valid()) close_fd(m_descriptor);
    m_descriptor = other.take();
    return *this;
  }
  ~DirectoryReference()
  {
    if (is_valid()) close_fd(m_descriptor);
  }

  pure fn is_valid() const wontthrow -> bool
  {
    return m_descriptor != KOSH_INVALID_FD;
  }
  pure fn get() const wontthrow -> descriptor { return m_descriptor; }
  fn take() wontthrow -> descriptor
  {
    let const result = m_descriptor;
    m_descriptor = KOSH_INVALID_FD;
    return result;
  }

private:
  descriptor m_descriptor{KOSH_INVALID_FD};
};

/* Every rebinding of a standard descriptor bumps this counter. A cached answer
   about a standard stream is refreshed when the value changes. */
pure fn get_descriptor_epoch() wontthrow -> u64;
fn note_descriptor_rebound() wontthrow -> void;

fn redirect_stdout(os::descriptor target) wontthrow -> os::descriptor;
fn restore_stdout(os::descriptor saved) wontthrow -> void;

struct saved_descriptor
{
  i32 shell_fd{-1};
  /* A copy of the original descriptor, valid only when was_open is true. */
  descriptor saved{KOSH_INVALID_FD};
  /* False when shell_fd was not open before the redirection, so restore closes
     it instead of duplicating the backup back. */
  bool was_open{false};
  /* False when the dup2 onto shell_fd failed, as when the target descriptor was
     closed in a duplication like >&5 with fd 5 closed. The caller throws a
     located error and the restore puts shell_fd back unchanged. */
  bool is_dup2_ok{true};
  /* On Windows, the handle this redirection installed in the standard-handle
     slot, so restore closes that exact handle rather than whatever the slot
     holds at restore time. Unused on POSIX, which restores by dup2. */
  descriptor replacement{KOSH_INVALID_FD};
};

fn save_and_replace_descriptor(i32 shell_fd, os::descriptor target) wontthrow
    -> saved_descriptor;
fn save_descriptor(i32 shell_fd) wontthrow -> saved_descriptor;
fn restore_descriptor(const saved_descriptor &saved) wontthrow -> void;

fn descriptor_for_shell_fd(i32 shell_fd) wontthrow -> os::descriptor;

/* On POSIX the descriptor is the shell fd number, on Windows it is the handle
   that occupies the shell's standard-handle slot. */
fn descriptor_is_shell_fd(os::descriptor fd, i32 shell_fd) wontthrow -> bool;

/* Orders two strings by the locale collation on POSIX and by byte value on a
   platform without one. The sign follows strcmp. */
fn collate_compare(const String &left, const String &right) wontthrow -> int;

/* One capture group's byte span in the subject. A group that did not
   participate carries a negative start. */
struct regex_span
{
  i64 start{-1};
  i64 end{-1};
};

/* An opaque compiled extended regex. On POSIX it holds a regex_t, on a platform
   with no regex engine it holds nothing. Ownership is tracked by CompiledRegex,
   not here. */
struct compiled_regex
{
#if KOSH_PLATFORM_IS KOSH_PLATFORM_POSIX
  regex_t re{};
#else
  String pattern{heap_allocator()};
  bool is_case_insensitive{false};
#endif
};

enum class regex_compile_result : u8
{
  Ok,
  Invalid,
};

enum class regex_match_result : u8
{
  Matched,
  NoMatch,
  Error,
};

/* False on a platform with no regex engine, so [[ =~ ]] reports it is
   unsupported rather than matching. */
constexpr bool HAS_REGEX_ENGINE =
    ((KOSH_SUPPORT_VECTOR) &KOSH_PLATFORM_POSIX) != 0;

fn compile_regex(StringView pattern, case_sensitivity sensitivity,
                 compiled_regex &out) throws -> regex_compile_result;

fn execute_regex(compiled_regex &compiled, StringView subject,
                 ArrayList<regex_span> &spans, String &error_message,
                 Allocator scratch) throws -> regex_match_result;

fn free_regex(compiled_regex &compiled) wontthrow -> void;

/* Compiles a search pattern for a line-at-a-time grep. On POSIX it is a basic
   regex with no capture, on a platform with no engine it is a literal
   substring. */
fn compile_search_regex(StringView pattern, case_sensitivity sensitivity,
                        compiled_regex &out) throws -> regex_compile_result;

fn regex_matches(compiled_regex &compiled, StringView subject) throws -> bool;

pure fn path_is_absolute(StringView path) wontthrow -> bool;
pure fn path_is_drive_relative(StringView path) wontthrow -> bool;
fn resolve_drive_relative_path(StringView path) throws -> Maybe<Path>;
fn temp_directory_path() throws -> String;

cold fn path_exists(StringView path) wontthrow -> bool;
cold fn path_is_directory(StringView path) wontthrow -> bool;
fn path_is_regular_file(StringView path) wontthrow -> bool;
fn path_is_symbolic_link(StringView path) wontthrow -> bool;
fn path_is_block_device(StringView path) wontthrow -> bool;
fn path_is_character_device(StringView path) wontthrow -> bool;
fn path_is_fifo(StringView path) wontthrow -> bool;
fn path_is_socket(StringView path) wontthrow -> bool;
fn path_has_setuid_bit(StringView path) wontthrow -> bool;
fn path_has_setgid_bit(StringView path) wontthrow -> bool;
fn path_has_sticky_bit(StringView path) wontthrow -> bool;
fn path_is_owned_by_effective_user(StringView path) wontthrow -> bool;
fn path_is_owned_by_effective_group(StringView path) wontthrow -> bool;
fn path_file_size(StringView path) wontthrow -> Maybe<u64>;
fn path_modification_time(StringView path) wontthrow -> Maybe<i64>;
fn paths_are_same_file(StringView first, StringView second) wontthrow -> bool;
fn path_is_newer_than(StringView first, StringView second) wontthrow -> bool;
fn path_is_older_than(StringView first, StringView second) wontthrow -> bool;
fn path_is_readable(StringView path) wontthrow -> bool;
fn path_is_writable(StringView path) wontthrow -> bool;
fn path_is_executable(StringView path) wontthrow -> bool;
cold fn read_current_directory() throws -> Path;
fn change_current_directory(StringView path) throws -> ErrorOr<Ok>;
fn reference_current_directory() wontthrow -> DirectoryReference;
fn restore_current_directory(const DirectoryReference &reference) wontthrow
    -> bool;
cold fn list_directory(StringView dir) throws -> Maybe<ArrayList<String>>;
cold fn list_directory_typed(StringView dir) throws
    -> Maybe<ArrayList<Path::directory_child>>;

/* The user and system seconds the shell and its children have consumed. Every
   field is zero on a platform with no process accounting. */
struct cpu_times
{
  double self_user_seconds{0};
  double self_system_seconds{0};
  double child_user_seconds{0};
  double child_system_seconds{0};
};

fn read_process_cpu_times() wontthrow -> cpu_times;

enum class resource_kind : u8
{
  CpuSeconds,
  FileBlocks,
  DataKbytes,
  StackKbytes,
  CoreBlocks,
  ResidentKbytes,
  LockedMemoryKbytes,
  Processes,
  OpenFiles,
  VirtualMemoryKbytes,
  FileLocks,
  RealtimePriority,
};

/* The shell-level stand-in for an infinite limit, mapped to the platform's own
   sentinel by the wrappers. */
constexpr u64 RESOURCE_UNLIMITED = ~static_cast<u64>(0);

struct resource_limit
{
  u64 soft{RESOURCE_UNLIMITED};
  u64 hard{RESOURCE_UNLIMITED};
};

/* False when the platform carries no such limit. */
fn get_resource_limit(resource_kind kind, resource_limit &out) wontthrow
    -> bool;
fn set_resource_limit(resource_kind kind, const resource_limit &limit) wontthrow
    -> bool;

/* On POSIX the number is the descriptor, and on Windows it maps to the C
   runtime handle. */
fn descriptor_from_fd_number(i64 fd_number) wontthrow -> os::descriptor;

fn replace_descriptor(i32 shell_fd, os::descriptor target) wontthrow -> bool;

fn close_shell_fd(i32 shell_fd) wontthrow -> bool;

fn allocate_free_shell_fd(i32 floor_fd) wontthrow -> i32;

fn get_environment_variable(StringView key) throws -> Maybe<String>;
fn set_environment_variable(StringView key, StringView value) throws -> void;
fn unset_environment_variable(StringView key) throws -> void;
fn signal_internal_diagnostic() wontthrow -> void;

fn environment_names() throws -> ArrayList<String>;

fn is_child_process() wontthrow -> bool;

fn get_shell_process_id() wontthrow -> i64;
fn get_current_process_id() wontthrow -> i64;

fn get_parent_process_id() wontthrow -> i64;
fn get_real_user_id() wontthrow -> i64;
fn get_effective_user_id() wontthrow -> i64;
fn get_real_group_id() wontthrow -> i64;

fn child_max() wontthrow -> i64;

fn machine_type() throws -> String;
fn executable_system_name() throws -> String;
fn executable_machine_name() throws -> String;

inline fn ostype_name() wontthrow -> StringView
{
#if defined(_WIN32)
  return "msys";
#elif defined(__APPLE__)
  return "darwin";
#else
  return "linux-gnu";
#endif
}

/* The shell skips its startup config files in the setuid or setgid case, so a
   file an attacker controls cannot run with the raised privileges. */
fn is_running_setuid() wontthrow -> bool;
fn drop_elevated_identity() wontthrow -> bool;

fn reopen_terminal_as_stdin() wontthrow -> bool;

fn process_id_of(process p) wontthrow -> i64;
fn process_group_of(process p) throws -> process;
fn close_process_group(process group) wontthrow -> void;
fn process_has_id(process p, i64 id) wontthrow -> bool;

fn is_stdin_a_tty() wontthrow -> bool;
fn is_stdout_a_tty() wontthrow -> bool;
fn is_stderr_a_tty() wontthrow -> bool;
fn is_fd_a_tty(descriptor fd) wontthrow -> bool;
fn shell_fd_is_a_tty(int shell_fd) wontthrow -> bool;

pure fn is_directory_separator(char c) wontthrow -> bool;
pure inline fn has_directory_separator(StringView path) wontthrow -> bool
{
#if KOSH_PLATFORM_IS KOSH_PLATFORM_WIN32
  return path.find_character('/').has_value() ||
         path.find_character('\\').has_value();
#else
  return path.find_character('/').has_value();
#endif
}

pure fn path_root_length(StringView path) wontthrow -> usize;

fn make_fd_inheritable(descriptor fd) wontthrow -> void;

fn normalize_program_name(String &program_name) throws -> program_name_info;

fn get_current_user() throws -> Maybe<String>;

fn get_hostname() throws -> Maybe<String>;

struct processor_counts
{
  usize online_count{1};
  usize configured_count{1};
};

fn get_processor_counts() wontthrow -> processor_counts;

fn get_home_directory() throws -> Maybe<Path>;

fn get_home_for_user(StringView username) throws -> Maybe<Path>;

fn enumerate_users() throws -> ArrayList<String>;

/* The interactive shell blocks the terminal-generated signals, a
   non-interactive script leaves those at their default. SIGINT routes to the
   polled handler in both modes. */
fn set_default_signal_handlers(signal_profile profile) throws -> void;

fn reset_signal_handlers() throws -> void;

/* Set to one by the SIGINT handler and polled by the evaluator, so a Ctrl-C
   aborts the running command. The main loop clears it before each interactive
   command. */
extern volatile sig_atomic_t INTERRUPT_REQUESTED;

/* Raised by the SIGCHLD handler when a child changes state, read and cleared
   by the prompt's wake hook so set -b reports a finished job immediately. */
extern volatile sig_atomic_t CHILD_STATE_CHANGED;

/* Set to one whenever any trapped signal arrives, so the evaluator's hot poll
   is a single read. The drain at the command boundary clears it as it consumes
   the per-signal flags. */
extern volatile sig_atomic_t SIGNAL_PENDING;

/* A signal the startup blocked is unblocked here. */
fn set_trap_handler(i32 signal_number) throws -> void;

/* Install the ignore disposition for a signal, for a trap with an empty action
   such as trap "" INT, so the signal is discarded. */
fn set_trap_ignore(i32 signal_number) throws -> void;

/* Restore a signal's default disposition when its trap is removed. SIGINT
   returns to the shell's interrupt handler, every other signal to SIG_DFL. */
fn clear_trap_handler(i32 signal_number) throws -> void;

fn take_pending_signal() wontthrow -> i32;

fn monotonic_nanos() wontthrow -> u64;

fn realtime_microseconds() wontthrow -> u64;

/* A negative epoch renders the current time, the bash -1 and -2 forms. */
fn format_local_time(StringView format, i64 epoch) throws -> String;

fn terminal_size(u32 &columns, u32 &rows,
                 descriptor output = KOSH_STDOUT) wontthrow -> bool;

/* The user and system seconds this process's children have consumed so far,
   read from RUSAGE_CHILDREN. Windows has no equivalent and reports zero. */
fn children_cpu_seconds(double &user_seconds, double &system_seconds) wontthrow
    -> void;

fn children_peak_rss_bytes() wontthrow -> u64;

struct malloc_heap_stats
{
  usize bytes_in_use{0};
  usize arena_bytes{0};
  usize mapped_bytes{0};
};

fn read_malloc_heap_stats(malloc_heap_stats &stats) wontthrow -> bool;

struct perf_counts
{
  u64 cpu_cycles{0};
  u64 instructions{0};
  u64 cache_references{0};
  u64 cache_misses{0};
  u64 branch_misses{0};
};

/* The perf counts are filled only when has_perf is true. */
struct measured_result
{
  u64 wall_nanos{0};
  u64 peak_rss_bytes{0};
  i64 exit_status{0};
  bool has_perf{false};
  bool is_perf_system_wide{false};
  perf_counts perf{};
};

fn run_measured(const ArrayList<String> &argv, measured_output output,
                Maybe<descriptor> inherited_handle = {}) throws
    -> Maybe<measured_result>;

/* Script fallback returns KOSH_INVALID_PROCESS when it is allowed and the file
   has no executable format. */
fn execute_program(
    ExecContext &ec,
    script_fallback_policy fallback = script_fallback_policy::Reject,
    process_group_mode process_group = process_group_mode::Inherit,
    StringView source = {}, terminal_handoff handoff = terminal_handoff::Keep,
    i64 process_group_id = 0) throws -> process;

fn shell_has_controlling_terminal() wontthrow -> bool;

fn canonical_path(const Path &path) wontthrow -> Maybe<Path>;

/* On Windows the wildcard applies to the last path component the way
   FindFirstFile expands it. */
fn glob_matches(StringView pattern, Allocator allocator) throws
    -> ArrayList<String>;

/* Whether the directory is safe to run a binary from for its --help text, so it
   is owned by root or the current user and is not writable by group or other,
   rejecting a world-writable directory such as /tmp. On Windows a PATH
   directory is trusted because Windows uses ACLs rather than owner/group mode
   bits. */
fn directory_is_trusted_for_exec(const Path &directory) wontthrow -> bool;

fn capture_program_output(const ArrayList<String> &argv,
                          u64 timeout_nanos) wontthrow -> Maybe<String>;

/* Ignores SIGTTOU across the change. A no-op without a controlling terminal. */
fn give_controlling_terminal_to(process p) wontthrow -> void;
fn give_controlling_terminal_to_process_group(i64 process_group_id) wontthrow
    -> void;
fn reclaim_controlling_terminal() wontthrow -> void;

struct process_substitution_launch
{
  String path{heap_allocator()};
  Maybe<descriptor> retained_fd{};
  Maybe<descriptor> child_close_fd{};
  process child{KOSH_INVALID_PROCESS};
  String diagnostic_output{heap_allocator()};
  bool should_evaluate_child{false};
  bool is_temporary_file{false};
  bool has_shell_diagnostic{false};
};

fn launch_process_substitution(StringView source, bool command_writes_pipe,
                               bool bash_compatible,
                               bool source_traces_enabled) throws
    -> process_substitution_launch;

fn try_fork_compound_stage(
    Maybe<descriptor> in_fd, Maybe<descriptor> out_fd, Maybe<descriptor> err_fd,
    SourceLocation location = {}, StringView source = {},
    process_group_mode process_group = process_group_mode::Inherit,
    i64 process_group_id = 0) throws -> Maybe<process>;

fn try_fork_job_process() throws -> Maybe<process>;
fn can_fork_evaluator() wontthrow -> bool;

struct compound_stage_launch
{
  process child{KOSH_INVALID_PROCESS};
  bool should_evaluate_child{false};
};

fn launch_compound_stage(
    StringView source, Maybe<descriptor> in_fd, Maybe<descriptor> out_fd,
    Maybe<descriptor> err_fd, mimic_mood mood, SourceLocation location = {},
    StringView diagnostic_source = {},
    process_group_mode process_group = process_group_mode::Inherit,
    i64 process_group_id = 0) throws -> compound_stage_launch;

fn register_platform_flags(FlagList &flags) throws -> void;
fn initialize_platform_runtime() wontthrow -> void;

/* A forked pipeline-stage child calls this so it never runs the parent's
   cleanup inside the duplicated process. */
[[noreturn]] fn exit_process_immediately(i32 status) wontthrow -> void;

/* It does not fork, so on success it never returns. */
fn replace_process(ExecContext &&ec) throws -> void;

fn redirect_self(const ExecContext &ec) throws -> void;

} /* namespace os */

} /* namespace koshka */
