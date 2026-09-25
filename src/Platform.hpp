/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file declares the shared operating-system interface and selects the
 * native types used by its POSIX and Win32 implementations. This boundary
 * keeps platform headers and value conventions out of shell subsystems.
 */

#pragma once

#include "base/Common.hpp"

#define KOSH_PLATFORM_POSIX 0b1
#define KOSH_PLATFORM_WIN32 0b10
#define KOSH_PLATFORM_COSMO 0b100

/* clang-format off */
#if defined __linux__ || defined BSD || defined __APPLE__ ||                   \
    defined __COSMOPOLITAN__
#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <glob.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <pwd.h>
#include <regex.h>
#include <spawn.h>
#include <sys/ioctl.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
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
#include <sys/sysmacros.h>
#include <sys/syscall.h>
#include <sys/vfs.h>
#endif
#if defined __GLIBC__
#include <malloc.h>
#endif
#if defined __APPLE__
#pragma push_macro("cold")
#undef cold
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/storage/IOBlockStorageDriver.h>
#pragma pop_macro("cold")
#include <dlfcn.h>
#include <libproc.h>
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <malloc/malloc.h>
#include <net/if.h>
#include <net/if_mib.h>
#include <net/route.h>
#include <netinet/tcp_var.h>
#include <sys/proc.h>
#include <sys/proc_info.h>
#include <sys/sysctl.h>
#endif
#if defined KOSH_HAS_ADDRESS_SANITIZER
extern "C" void __lsan_disable(void);
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
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#ifndef WC_ERR_INVALID_CHARS
#define WC_ERR_INVALID_CHARS 0x00000080
#endif
#include <winioctl.h>
#include <direct.h>
#include <io.h>
#include <malloc.h>
#include <psapi.h>
#include <restartmanager.h>
#include <sys/stat.h>
#include <tlhelp32.h>
#include "../vendor/regex/regex.h"
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

hot alwaysinline fn read_native_endian_bytes(const char *bytes,
                                             usize count) wontthrow -> u64
{
  u64 value = 0;
#if defined __BYTE_ORDER__ && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  __builtin_memcpy(&value, bytes, count);
#else
  for (usize index = 0; index < count; index++)
    value = (value << 8) | static_cast<u8>(bytes[index]);
#endif

  return value;
}

} /* namespace koshka::os */

#include "base/ArrayList.hpp"
#include "base/Maybe.hpp"
#include "base/Path.hpp"
#include "base/String.hpp"

namespace koshka {

class ExecContext;
class Flag;
class FlagList;
enum class mimic_mood : u8;

namespace os {

enum class goodcore_capture_mode : u8
{
  Unsupported,
  Lldb,
  Gcore,
};

struct goodcore_platform_tools
{
  StringView dependency_program;
  StringView debugger_program;
  goodcore_capture_mode capture_mode;
  bool dependency_uses_library_flag;
};

pure fn goodcore_tools() wontthrow -> goodcore_platform_tools;

struct evildisk_platform_tools
{
  StringView smart_fallback_program;
  StringView smart_fallback_subcommand;
};

pure fn evildisk_tools() wontthrow -> evildisk_platform_tools;

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

enum class priority_target : u8
{
  Process,
  ProcessGroup,
  User,
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
constexpr bool ENVIRONMENT_IS_CASE_SENSITIVE = false;
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
constexpr bool ENVIRONMENT_IS_CASE_SENSITIVE = true;
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
  ReadNonblocking,
};

fn open_file_descriptor(StringView path, file_open_mode mode) throws
    -> Maybe<descriptor>;
fn acquire_process_lock(StringView path) throws -> Maybe<descriptor>;
fn release_process_lock(descriptor lock) wontthrow -> void;

fn write_to_temp_file(StringView content) throws -> Maybe<descriptor>;
fn write_to_named_temp_file(const Path &directory, StringView prefix,
                            StringView content) throws -> Maybe<Path>;
fn make_temp_directory(const Path &directory, StringView prefix) throws
    -> Maybe<Path>;

/* On a platform that leaves no temp file, such as POSIX, it holds nothing. */
class TempFileSet
{
public:
  fn track(Path &&path) throws -> void;
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
fn get_last_system_error_number() wontthrow -> i32;
fn last_system_error_is_missing_file() wontthrow -> bool;
fn last_system_error_is_permission_denied() wontthrow -> bool;
fn last_system_error_is_descriptor_quota() wontthrow -> bool;
fn set_last_system_error(i32 error_number) wontthrow -> void;

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
  i64 parent_pid{0};
  String name{heap_allocator()};
  String command_line{heap_allocator()};
  /* The BSD aux columns, filled only when resource stats are requested. */
  u64 virtual_kib{0};
  u64 resident_kib{0};
  u64 cpu_milliseconds{0};
  u64 start_token{0};
  u32 owner_id{0};
  char state{'?'};
};

static_assert(sizeof(usize) != 8 || sizeof(process_entry) == 168);

/* Every process the current user can see, for the koshkit pkill and killall
   utilities to match a name against. Empty on a platform with no process
   listing. The resource stats are read only when asked. */
fn enumerate_processes(process_detail detail = process_detail::Basic) throws
    -> ArrayList<process_entry>;

enum class process_file_use : u8
{
  Cwd = 1u << 0u,
  Root = 1u << 1u,
  Executable = 1u << 2u,
  Mapped = 1u << 3u,
  File = 1u << 4u,
};

struct process_file_query
{
  StringView path;
  u64 device_id;
  u64 file_id;
  u32 query_position;
  bool should_match_device;
};

struct process_file_user
{
  u32 pid;
  u32 owner_id;
  u32 query_position;
  u8 use_mask;
};

fn scan_process_file_users(const ArrayList<process_file_query> &queries,
                           ArrayList<process_file_user> &users,
                           Allocator scratch) throws -> Maybe<u32>;
fn process_owner_name(u32 pid, u32 owner_id, Allocator allocator) throws
    -> Maybe<String>;

struct process_open_file
{
  String path{heap_allocator()};
  i64 descriptor_number{-1};
  u64 size{0};
  u64 file_id{0};
  u64 offset{0};
  u32 mode{0};
  process_file_use use{process_file_use::File};
  char access{'u'};
  bool is_deleted{false};
  bool is_inaccessible{false};
  String socket_endpoint{heap_allocator()};
};

enum class process_open_file_detail : u8
{
  Basic,
  IncludeMappings,
};

fn has_process_open_file_listing() wontthrow -> bool;
fn list_process_open_files(i64 pid, Allocator allocator,
                           process_open_file_detail detail =
                               process_open_file_detail::Basic) throws
    -> ArrayList<process_open_file>;

fn make_directory(StringView path, u32 mode) wontthrow -> bool;
fn set_file_mode(StringView path, u32 mode) wontthrow -> bool;
fn set_file_owner(StringView path, i64 owner_id, i64 group_id,
                  bool should_follow_symlink) wontthrow -> bool;
fn create_hard_link(StringView target, StringView link_path) wontthrow -> bool;
fn make_fifo(StringView path, u32 mode) wontthrow -> bool;
fn make_device_node(StringView path, u32 mode, u32 major_number,
                    u32 minor_number) wontthrow -> bool;
fn touch_file_times(StringView path) wontthrow -> bool;
struct file_time_values
{
  i64 access_time{0};
  u32 access_nanoseconds{0};
  i64 modification_time{0};
  u32 modification_nanoseconds{0};
};

fn set_file_times(StringView path, const file_time_values &times) wontthrow
    -> bool;
fn remove_directory(StringView path) wontthrow -> bool;
fn remove_file(StringView path) wontthrow -> bool;
fn rename_path(StringView from, StringView to) wontthrow -> bool;
fn create_symlink(StringView target, StringView link_path) wontthrow -> bool;
fn read_symlink(StringView path, Allocator allocator) wontthrow
    -> Maybe<String>;

struct filesystem_status
{
  u64 block_size{0};
  u64 fundamental_block_size{0};
  u64 total_blocks{0};
  u64 free_blocks{0};
  u64 available_blocks{0};
  u64 total_files{0};
  u64 free_files{0};
  u64 name_max{0};
  u64 filesystem_id{0};
  u64 type_id{0};
  char type_name[16]{};
};

struct mounted_filesystem
{
  String source{heap_allocator()};
  String target{heap_allocator()};
  String type{heap_allocator()};
  String options{heap_allocator()};
  String volume_name{heap_allocator()};
  String volume_uuid{heap_allocator()};
};

fn stat_filesystem(StringView path, filesystem_status &status) wontthrow
    -> bool;
fn mounted_filesystems() throws -> ArrayList<mounted_filesystem>;

struct filesystem_error_counters
{
  u64 read_count{0};
  u64 write_count{0};
  u64 flush_count{0};
  u64 corruption_count{0};
  u64 generation_count{0};
};

fn read_filesystem_error_counters(StringView path,
                                  filesystem_error_counters &counters) throws
    -> bool;

enum class filesystem_integrity_kind : u8
{
  BtrfsDeviceErrorCounters,
  Ext4RecordedErrors,
  NtfsDirtyFlag,
};

struct filesystem_integrity_evidence
{
  filesystem_integrity_kind kind;
  filesystem_error_counters counters{};
  u64 recorded_error_count{0};
  bool is_dirty{false};
};

fn read_filesystem_integrity_evidence(StringView path) throws
    -> Maybe<filesystem_integrity_evidence>;

enum class filesystem_verification_result : u8
{
  Passed,
  Failed,
  Interrupted,
  TimedOut,
  Unsupported,
  Unavailable,
};

fn verify_filesystem_integrity(StringView path, u64 timeout_nanoseconds) throws
    -> filesystem_verification_result;

fn sync_filesystems() wontthrow -> bool;

enum class sync_mode : u8
{
  All,
  DataOnly,
};

fn sync_path(StringView path, sync_mode mode) wontthrow -> bool;

enum class memory_status_field : u32
{
  Total = 1u << 0,
  Available = 1u << 1,
  Free = 1u << 2,
  SwapTotal = 1u << 3,
  SwapFree = 1u << 4,
  Buffers = 1u << 5,
  Cached = 1u << 6,
  ReclaimableSlab = 1u << 7,
  Shared = 1u << 8,
  Slab = 1u << 9,
  Active = 1u << 10,
  Inactive = 1u << 11,
  CommitLimit = 1u << 12,
  Committed = 1u << 13,
  HugePagesTotal = 1u << 14,
  HugePagesFree = 1u << 15,
};

struct memory_status
{
  u64 total_kib{0};
  u64 available_kib{0};
  u64 free_kib{0};
  u64 swap_total_kib{0};
  u64 swap_free_kib{0};
  u64 buffer_kib{0};
  u64 cached_kib{0};
  u64 reclaimable_slab_kib{0};
  u64 shared_kib{0};
  u64 slab_kib{0};
  u64 active_kib{0};
  u64 inactive_kib{0};
  u64 commit_limit_kib{0};
  u64 committed_kib{0};
  u64 huge_page_total_count{0};
  u64 huge_page_free_count{0};
  u32 available_fields{0};

  pure fn has_field(memory_status_field field) const wontthrow -> bool
  {
    return (available_fields & static_cast<u32>(field)) != 0;
  }
};

fn read_memory_status(memory_status &status) wontthrow -> bool;

struct swap_status
{
  u64 total_bytes{0};
  u64 used_bytes{0};
  u64 free_bytes{0};
  u64 input_bytes{0};
  u64 output_bytes{0};
  bool has_activity{false};
  bool has_encryption_state{false};
  bool is_encrypted{false};
};

fn read_swap_status(swap_status &status) wontthrow -> bool;

struct process_io_status
{
  u64 read_bytes{0};
  u64 written_bytes{0};
  u64 read_operation_count{0};
  u64 write_operation_count{0};
  bool has_operation_counts{false};
};

fn read_process_io_status(i64 pid, process_io_status &status) wontthrow -> bool;
fn read_process_io_statuses(const ArrayList<i64> &process_ids,
                            ArrayList<process_io_status> &statuses,
                            ArrayList<u8> &availability) throws -> void;

enum class system_activity_field : u32
{
  Cpu = 1u << 0,
  CpuWait = 1u << 1,
  CpuStolen = 1u << 2,
  PageInput = 1u << 3,
  PageOutput = 1u << 4,
  Faults = 1u << 5,
  MajorFaults = 1u << 6,
  Runnable = 1u << 7,
  Blocked = 1u << 8,
  CpuSomeStall = 1u << 9,
  CpuFullStall = 1u << 10,
  MemorySomeStall = 1u << 11,
  MemoryFullStall = 1u << 12,
  IoSomeStall = 1u << 13,
  IoFullStall = 1u << 14,
  PageScan = 1u << 15,
  PageSteal = 1u << 16,
  DirectReclaim = 1u << 17,
  CompactionStall = 1u << 18,
  DirtyPages = 1u << 19,
  WritebackPages = 1u << 20,
  OomKills = 1u << 21,
  Interrupts = 1u << 22,
  ContextSwitches = 1u << 23,
  ProcessCreations = 1u << 24,
  SoftInterrupts = 1u << 25,
  NamespaceCount = 1u << 26,
  CgroupMembershipCount = 1u << 27,
};

struct system_activity_status
{
  u64 cpu_user_units{0};
  u64 cpu_system_units{0};
  u64 cpu_idle_units{0};
  u64 cpu_wait_units{0};
  u64 cpu_stolen_units{0};
  u64 page_input_bytes{0};
  u64 page_output_bytes{0};
  u64 page_fault_count{0};
  u64 major_page_fault_count{0};
  u64 runnable_process_count{0};
  u64 blocked_process_count{0};
  u64 cpu_some_stall_microseconds{0};
  u64 cpu_full_stall_microseconds{0};
  u64 memory_some_stall_microseconds{0};
  u64 memory_full_stall_microseconds{0};
  u64 io_some_stall_microseconds{0};
  u64 io_full_stall_microseconds{0};
  u64 page_scan_count{0};
  u64 page_steal_count{0};
  u64 direct_reclaim_count{0};
  u64 compaction_stall_count{0};
  u64 dirty_page_count{0};
  u64 writeback_page_count{0};
  u64 oom_kill_count{0};
  u64 interrupt_count{0};
  u64 context_switch_count{0};
  u64 process_creation_count{0};
  u64 soft_interrupt_count{0};
  u64 namespace_count{0};
  u64 cgroup_membership_count{0};
  u32 available_fields{0};
  bool has_isolation_probes{false};

  pure fn has_field(system_activity_field field) const wontthrow -> bool
  {
    return (available_fields & static_cast<u32>(field)) != 0;
  }
};

fn read_system_activity_status(system_activity_status &status) wontthrow
    -> bool;

enum class disk_io_field : u32
{
  ReadBytes = 1u << 0,
  WrittenBytes = 1u << 1,
  ReadOperations = 1u << 2,
  WriteOperations = 1u << 3,
  ReadTime = 1u << 4,
  WriteTime = 1u << 5,
  BusyTime = 1u << 6,
  IdleTime = 1u << 7,
  WeightedBusyTime = 1u << 8,
  QueueDepth = 1u << 9,
  ReadErrors = 1u << 10,
  WriteErrors = 1u << 11,
  ReadRetries = 1u << 12,
  WriteRetries = 1u << 13,
};

struct disk_io_status
{
  String name{heap_allocator()};
  u64 read_bytes{0};
  u64 written_bytes{0};
  u64 read_operation_count{0};
  u64 write_operation_count{0};
  u64 read_time_nanoseconds{0};
  u64 write_time_nanoseconds{0};
  u64 busy_time_nanoseconds{0};
  u64 idle_time_nanoseconds{0};
  u64 weighted_busy_time_nanoseconds{0};
  u64 queue_depth{0};
  u64 read_error_count{0};
  u64 write_error_count{0};
  u64 read_retry_count{0};
  u64 write_retry_count{0};
  u32 available_fields{0};

  pure fn has_field(disk_io_field field) const wontthrow -> bool
  {
    return (available_fields & static_cast<u32>(field)) != 0;
  }
};

struct disk_io_snapshot
{
  ArrayList<disk_io_status> disks{heap_allocator()};
  u64 sampled_at_nanoseconds{0};
};

fn read_disk_io_snapshot(Allocator allocator) throws -> disk_io_snapshot;

fn system_uptime_seconds() wontthrow -> Maybe<u64>;

fn processor_model_name(Allocator allocator) throws -> Maybe<String>;

struct u128_division_result
{
  u64 quotient{0};
  u64 remainder{0};
};

fn divide_u128_by_u64(u64 high, u64 low, u64 divisor) wontthrow
    -> u128_division_result;

fn current_executable_path() wontthrow -> Maybe<String>;

fn crc32c_update(u32 crc, const void *data, usize length) wontthrow -> u32;

/* The mode carries the type and permission bits in the POSIX st_mode layout. */
struct file_status
{
  u64 device_id{0};
  u64 special_device_id{0};
  u64 file_id{0};
  u64 link_count{0};
  u64 size{0};
  i64 access_time{0};
  i64 modification_time{0};
  i64 change_time{0};
  u64 blocks{0};
  u32 mode{0};
  u32 owner_id{0};
  u32 group_id{0};
  u32 access_nanoseconds{0};
  u32 modification_nanoseconds{0};
  u32 change_nanoseconds{0};
  bool has_file_identity{false};
};

static_assert(sizeof(usize) != 8 || sizeof(file_status) == 104);

struct directory_status_entry
{
  Path::directory_child child;
  file_status status{};
  bool has_status{false};
};

/* Two observations describe the same untouched file. The device and file
   identity is compared only when both observations carry it, because a
   filesystem that reports no identity would otherwise never match. */
pure constexpr fn file_status_matches(const file_status &expected,
                                      const file_status &actual) wontthrow
    -> bool
{
  if (expected.has_file_identity && actual.has_file_identity &&
      (expected.device_id != actual.device_id ||
       expected.file_id != actual.file_id))
  {
    return false;
  }

  return expected.mode == actual.mode && expected.size == actual.size &&
         expected.modification_time == actual.modification_time &&
         expected.modification_nanoseconds == actual.modification_nanoseconds &&
         expected.change_time == actual.change_time &&
         expected.change_nanoseconds == actual.change_nanoseconds;
}

fn stat_path(StringView path, file_status &status) wontthrow -> bool;
fn stat_path_following(StringView path, file_status &status) wontthrow -> bool;
fn process_file_query_is_supported(const file_status &status,
                                   bool should_match_filesystem) wontthrow
    -> bool;

fn format_mode_string(u32 mode) throws -> String;

fn file_type_letter(u32 mode) wontthrow -> char;

pure fn device_major(u64 device_id) wontthrow -> u32;
pure fn device_minor(u64 device_id) wontthrow -> u32;

/* The user name for a numeric uid and the group name for a numeric gid, read
   directly from /etc/passwd and /etc/group, so the static build stays free of
   getpwuid and getgrgid. */
fn uid_to_username(u32 uid) throws -> Maybe<String>;
fn gid_to_groupname(u32 gid) throws -> Maybe<String>;
fn username_to_uid(StringView username) throws -> Maybe<u32>;
fn groupname_to_gid(StringView groupname) throws -> Maybe<u32>;

enum class system_configuration_key : u8
{
  AioListIoMax,
  AioMax,
  AioPriorityDeltaMax,
  ArgMax,
  AtExitMax,
  BcBaseMax,
  BcDimensionMax,
  BcScaleMax,
  BcStringMax,
  ChildMax,
  ClockTicks,
  CollationWeightsMax,
  DelayTimerMax,
  ExpressionNestMax,
  GroupBufferSizeMax,
  GroupsMax,
  HostNameMax,
  IoVectorMax,
  LineMax,
  LoginNameMax,
  MessageQueueOpenMax,
  MessageQueuePriorityMax,
  OpenMax,
  PageSize,
  PasswordBufferSizeMax,
  PasswordMax,
  PhysicalPages,
  PosixVersion,
  ProcessorConfigured,
  ProcessorOnline,
  RegexDupMax,
  RealtimeSignalMax,
  SemaphoreCountMax,
  SemaphoreValueMax,
  SignalQueueMax,
  StreamMax,
  SymbolicLinkLoopMax,
  ThreadCountMax,
  ThreadDestructorIterations,
  ThreadKeysMax,
  ThreadStackMin,
  TimerMax,
  TtyNameMax,
  TimeZoneNameMax,
  AdvisoryInfo,
  AsynchronousIo,
  Barriers,
  ClockSelection,
  CpuTime,
  DeviceControl,
  FileSync,
  IpV6,
  JobControl,
  MappedFiles,
  MemoryLock,
  MemoryLockRange,
  MemoryProtection,
  MessagePassing,
  MonotonicClock,
  Posix2CBind,
  Posix2CDev,
  Posix2CharTerminal,
  Posix2FortranRun,
  Posix2LocaleDefinition,
  Posix2SoftwareDevelopment,
  Posix2UserPortabilityUtilities,
  Posix2Version,
  PrioritizedIo,
  PriorityScheduling,
  RawSockets,
  ReaderWriterLocks,
  RealtimeSignals,
  RegularExpressions,
  SavedIds,
  Semaphores,
  SharedMemoryObjects,
  Shell,
  Spawn,
  SpinLocks,
  SporadicServer,
  SynchronizedIo,
  ThreadAttributeStackAddress,
  ThreadAttributeStackSize,
  ThreadCpuTime,
  ThreadPriorityInherit,
  ThreadPriorityProtect,
  ThreadPriorityScheduling,
  ThreadProcessShared,
  ThreadRobustPriorityInherit,
  ThreadRobustPriorityProtect,
  ThreadSafeFunctions,
  ThreadSporadicServer,
  Threads,
  Timeouts,
  Timers,
  TypedMemoryObjects,
  V7Ilp32Off32,
  V7Ilp32OffBig,
  V7Lp64Off64,
  V7LpBigOffBig,
  V8Ilp32Off32,
  V8Ilp32OffBig,
  V8Lp64Off64,
  V8LpBigOffBig,
  XOpenCrypt,
  XOpenEnhancedInternationalization,
  XOpenRealtime,
  XOpenRealtimeThreads,
  XOpenSharedMemory,
  XOpenUnix,
  XOpenUucp,
  XOpenVersion,
  Count,
};

enum class path_configuration_key : u8
{
  AllocationSizeMin,
  AsyncIo,
  ChownRestricted,
  DisableCharacter,
  FileSizeBits,
  LinkMax,
  MaxCanonical,
  MaxInput,
  NameMax,
  NoTrunc,
  PathMax,
  PipeBuffer,
  PriorityIo,
  RecommendedIncrementTransferSize,
  RecommendedMaxTransferSize,
  RecommendedMinTransferSize,
  RecommendedTransferAlignment,
  SymbolicLinkMax,
  SyncIo,
  TwoSymbolicLinks,
  Fallocate,
  TextDomainMax,
  TimestampResolution,
  Count,
};

enum class string_configuration_key : u8
{
  Path,
  V7Ilp32Off32CFlags,
  V7Ilp32Off32LdFlags,
  V7Ilp32Off32Libs,
  V7Ilp32OffBigCFlags,
  V7Ilp32OffBigLdFlags,
  V7Ilp32OffBigLibs,
  V7Lp64Off64CFlags,
  V7Lp64Off64LdFlags,
  V7Lp64Off64Libs,
  V7LpBigOffBigCFlags,
  V7LpBigOffBigLdFlags,
  V7LpBigOffBigLibs,
  V7ThreadsCFlags,
  V7ThreadsLdFlags,
  V7WidthRestrictedEnvironments,
  V7Environment,
  V8Ilp32Off32CFlags,
  V8Ilp32Off32LdFlags,
  V8Ilp32Off32Libs,
  V8Ilp32OffBigCFlags,
  V8Ilp32OffBigLdFlags,
  V8Ilp32OffBigLibs,
  V8Lp64Off64CFlags,
  V8Lp64Off64LdFlags,
  V8Lp64Off64Libs,
  V8LpBigOffBigCFlags,
  V8LpBigOffBigLdFlags,
  V8LpBigOffBigLibs,
  V8ThreadsCFlags,
  V8ThreadsLdFlags,
  V8WidthRestrictedEnvironments,
  V8Environment,
  Count,
};

enum class configuration_query_status : u8
{
  Value,
  Undefined,
  Error,
};

struct numeric_configuration_result
{
  configuration_query_status status{configuration_query_status::Undefined};
  i64 value{0};
};

class StringConfigurationResult
{
public:
  explicit StringConfigurationResult(Allocator allocator) : value(allocator) {}

  configuration_query_status status{configuration_query_status::Undefined};
  String value;
};

fn query_system_configuration(system_configuration_key key) wontthrow
    -> numeric_configuration_result;
fn query_path_configuration(StringView path,
                            path_configuration_key key) wontthrow
    -> numeric_configuration_result;
fn query_string_configuration(string_configuration_key key,
                              Allocator allocator) throws
    -> StringConfigurationResult;

fn system_configuration(system_configuration_key key) wontthrow -> Maybe<i64>;
fn path_configuration(StringView path, path_configuration_key key) wontthrow
    -> Maybe<i64>;
fn string_configuration(string_configuration_key key,
                        Allocator allocator) throws -> Maybe<String>;
fn path_component_length(StringView component) wontthrow -> Maybe<usize>;

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
fn regular_descriptor_file_size(os::descriptor fd) wontthrow -> Maybe<u64>;
fn rewind_descriptor(os::descriptor fd, usize byte_count) wontthrow -> bool;
fn seek_descriptor_from_start(os::descriptor fd, u64 byte_offset) wontthrow
    -> bool;
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
  descriptor original{KOSH_INVALID_FD};
  /* A copy of the original descriptor, valid only when was_open is true. */
  descriptor saved{KOSH_INVALID_FD};
  /* False when shell_fd was not open before the redirection, so restore closes
     it instead of duplicating the backup back. */
  bool was_open{false};
  /* False when the dup2 onto shell_fd failed, as when the target descriptor was
     closed in a duplication like >&5 with fd 5 closed. The caller throws a
     located error and the restore puts shell_fd back unchanged. */
  bool is_dup2_ok{true};
};

fn save_and_replace_descriptor(i32 shell_fd, os::descriptor target) wontthrow
    -> saved_descriptor;
fn save_descriptor(i32 shell_fd) wontthrow -> saved_descriptor;

/* Take the same backup the pair above takes and put it where a script is not
   going to name it. Bash forks a subshell and execs a mimicked script, so it
   needs no such backup. Kosh runs both in process and has to keep this one
   alive underneath the script. */
fn save_and_replace_descriptor_out_of_reach(i32 shell_fd,
                                            os::descriptor target) wontthrow
    -> saved_descriptor;
fn save_descriptor_out_of_reach(i32 shell_fd) wontthrow -> saved_descriptor;
fn restore_descriptor(const saved_descriptor &saved) wontthrow -> void;

fn descriptor_for_shell_fd(i32 shell_fd) wontthrow -> os::descriptor;

/* Take an independent descriptor for the same open file the shell holds on
   shell_fd. The result is owned by the caller and closes on exec, and a
   shell_fd that is not open returns KOSH_INVALID_FD. */
fn duplicate_shell_fd(i32 shell_fd) wontthrow -> os::descriptor;

/* Move an owned descriptor onto a free shell fd at or above floor_fd and close
   the original. The result is the shell fd number a script can name, and a
   failure returns -1 with the original still open. */
fn move_descriptor_to_free_shell_fd(os::descriptor source,
                                    i32 floor_fd) wontthrow -> i32;
fn descriptors_refer_to_same_file(os::descriptor first,
                                  os::descriptor second) wontthrow -> bool;

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

/* An opaque compiled regular expression. Ownership is tracked by
   CompiledRegex, not here. */
struct compiled_regex
{
  regex_t re{};
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

constexpr bool HAS_REGEX_ENGINE = true;

enum class regex_start_position : u8
{
  Beginning,
  NotBeginning,
};

struct regex_execution_options
{
  StringView subject{};
  Allocator scratch;
  regex_start_position start_position{regex_start_position::Beginning};
};

struct regex_execution_report
{
  explicit regex_execution_report(Allocator allocator)
      : spans(allocator), error_message(allocator)
  {}

  regex_match_result result{regex_match_result::NoMatch};
  ArrayList<regex_span> spans;
  String error_message;
};

fn compile_regex(StringView pattern, compiled_regex &out,
                 case_sensitivity sensitivity) throws -> regex_compile_result;
fn compile_basic_regex(StringView pattern, compiled_regex &out,
                       case_sensitivity sensitivity) throws
    -> regex_compile_result;

fn execute_regex(compiled_regex &compiled,
                 const regex_execution_options &options) throws
    -> regex_execution_report;

fn free_regex(compiled_regex &compiled) wontthrow -> void;

/* Compiles a basic search pattern with no capture for line-at-a-time grep. */
fn compile_search_regex(StringView pattern, compiled_regex &out,
                        case_sensitivity sensitivity) throws
    -> regex_compile_result;

fn regex_matches(compiled_regex &compiled, StringView subject) throws -> bool;
fn regex_matches_null_terminated(compiled_regex &compiled,
                                 StringView subject) throws -> bool;

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
fn paths_match_for_history(StringView first, StringView second) wontthrow
    -> bool;
fn path_is_newer_than(StringView first, StringView second) wontthrow -> bool;
fn path_is_older_than(StringView first, StringView second) wontthrow -> bool;
fn path_is_readable(StringView path) wontthrow -> bool;
fn path_is_writable(StringView path) wontthrow -> bool;
fn path_is_executable(const Path &path) wontthrow -> bool;
cold fn read_current_directory() throws -> Path;
fn change_current_directory(StringView path) throws -> ErrorOr<Ok>;
fn reference_current_directory() wontthrow -> DirectoryReference;
fn restore_current_directory(const DirectoryReference &reference) wontthrow
    -> bool;
cold fn list_directory(StringView dir) throws -> Maybe<ArrayList<String>>;
cold fn list_directory(StringView dir, Allocator allocator) throws
    -> Maybe<ArrayList<String>>;
cold fn list_directory_typed(StringView dir) throws
    -> Maybe<ArrayList<Path::directory_child>>;
cold fn list_directory_typed(StringView dir, Allocator allocator) throws
    -> Maybe<ArrayList<Path::directory_child>>;
#if defined __linux__
cold fn list_directory_typed(descriptor directory, Allocator allocator) throws
    -> Maybe<ArrayList<Path::directory_child>>;
#endif
cold fn list_directory_status(StringView dir, Allocator allocator) throws
    -> Maybe<ArrayList<directory_status_entry>>;

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
fn get_resource_limit(resource_limit &out, resource_kind kind) wontthrow
    -> bool;
fn set_resource_limit(const resource_limit &limit, resource_kind kind) wontthrow
    -> bool;

/* On POSIX the number is the descriptor, and on Windows it maps to the C
   runtime handle. */
fn descriptor_from_fd_number(i64 fd_number) wontthrow -> os::descriptor;

fn replace_descriptor(i32 shell_fd, os::descriptor target) wontthrow -> bool;

fn close_shell_fd(i32 shell_fd) wontthrow -> bool;

fn allocate_free_shell_fd(i32 floor_fd) wontthrow -> i32;

fn has_environment_variable(StringView key) throws -> bool;
fn get_environment_variable(StringView key) throws -> Maybe<String>;
fn set_environment_variable(StringView key, StringView value) throws -> void;
fn unset_environment_variable(StringView key) throws -> void;
fn signal_internal_diagnostic() wontthrow -> void;

using environment_name_callback = void (*)(opaque *, StringView);
fn for_each_environment_name(opaque *context,
                             environment_name_callback callback) throws -> void;
fn environment_names() throws -> ArrayList<String>;

fn is_child_process() wontthrow -> bool;

fn get_shell_process_id() wontthrow -> i64;
fn set_shell_process_id(i64 pid) wontthrow -> void;
fn get_current_process_id() wontthrow -> i64;

fn get_parent_process_id() wontthrow -> i64;
fn get_real_user_id() wontthrow -> i64;
fn get_effective_user_id() wontthrow -> i64;
fn get_real_group_id() wontthrow -> i64;
fn get_effective_group_id() wontthrow -> i64;
fn get_supplementary_group_ids(Allocator allocator) throws -> ArrayList<u32>;

fn child_max() wontthrow -> i64;

fn machine_type() throws -> String;
fn executable_system_name() throws -> String;
fn executable_machine_name() throws -> String;
fn system_release_name() throws -> String;
fn system_version_name() throws -> String;
fn machine_target_name() throws -> String;
fn ostype_name() wontthrow -> StringView;

/* The shell skips its startup config files in the setuid or setgid case, so a
   file an attacker controls cannot run with the raised privileges. */
fn is_running_setuid() wontthrow -> bool;
fn drop_elevated_identity() wontthrow -> bool;

fn reopen_terminal_as_stdin() wontthrow -> bool;

fn process_id_of(process p) wontthrow -> i64;
fn process_group_of(process p) throws -> process;
fn close_process_reference(process p) wontthrow -> void;
fn process_has_id(process p, i64 id) wontthrow -> bool;

fn is_stdin_a_tty() wontthrow -> bool;
fn is_stdout_a_tty() wontthrow -> bool;
fn is_stderr_a_tty() wontthrow -> bool;
fn is_fd_a_tty(descriptor fd) wontthrow -> bool;
fn terminal_name(descriptor fd) throws -> Maybe<String>;
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

fn get_login_user() throws -> Maybe<String>;

fn get_hostname() throws -> Maybe<String>;

enum class network_address_family : u8
{
  IPv4,
  IPv6,
};

struct network_interface_address
{
  String interface_name{heap_allocator()};
  network_address_family family{network_address_family::IPv4};
  String address{heap_allocator()};
};

fn network_interface_addresses() throws -> ArrayList<network_interface_address>;
fn default_network_interface(Allocator allocator) throws -> Maybe<String>;

enum class network_statistics_field : u32
{
  ReceiveBytes = 1u << 0,
  TransmitBytes = 1u << 1,
  ReceivePackets = 1u << 2,
  TransmitPackets = 1u << 3,
  ReceiveErrors = 1u << 4,
  TransmitErrors = 1u << 5,
  ReceiveDrops = 1u << 6,
  TransmitDrops = 1u << 7,
  ReceiveLinkSpeed = 1u << 8,
  TransmitLinkSpeed = 1u << 9,
  TransmitQueueLength = 1u << 10,
  TransmitQueueLimit = 1u << 11,
};

struct network_interface_statistics_entry
{
  String interface_name{heap_allocator()};
  u64 receive_bytes{0};
  u64 transmit_bytes{0};
  u64 receive_packet_count{0};
  u64 transmit_packet_count{0};
  u64 receive_error_count{0};
  u64 transmit_error_count{0};
  u64 receive_drop_count{0};
  u64 transmit_drop_count{0};
  u64 receive_link_bits_per_second{0};
  u64 transmit_link_bits_per_second{0};
  u64 transmit_queue_length{0};
  u64 transmit_queue_limit{0};
  u32 available_fields{0};

  pure fn has_field(network_statistics_field field) const wontthrow -> bool
  {
    return (available_fields & static_cast<u32>(field)) != 0;
  }
};

fn read_network_interface_statistics() throws
    -> ArrayList<network_interface_statistics_entry>;

enum class tcp_statistics_field : u32
{
  ActiveOpens = 1u << 0,
  PassiveOpens = 1u << 1,
  ReceivedSegments = 1u << 2,
  SentSegments = 1u << 3,
  RetransmittedSegments = 1u << 4,
  InputErrors = 1u << 5,
  AttemptFailures = 1u << 6,
  EstablishedResets = 1u << 7,
  CurrentEstablished = 1u << 8,
  SentResets = 1u << 9,
  ConnectionDrops = 1u << 10,
  ReceiveMemoryDrops = 1u << 11,
  ListenDrops = 1u << 12,
  RetransmitTimeouts = 1u << 13,
  ListenOverflows = 1u << 14,
  SynRetransmits = 1u << 15,
  FastRetransmits = 1u << 16,
  SpuriousRetransmitTimeouts = 1u << 17,
  ReceiveQueueDrops = 1u << 18,
  BacklogDrops = 1u << 19,
  RequestQueueFullDrops = 1u << 20,
  RetransmitFailures = 1u << 21,
};

struct tcp_statistics
{
  u64 active_open_count{0};
  u64 passive_open_count{0};
  u64 received_segment_count{0};
  u64 sent_segment_count{0};
  u64 retransmitted_segment_count{0};
  u64 input_error_count{0};
  u64 attempt_failure_count{0};
  u64 established_reset_count{0};
  u64 current_established_count{0};
  u64 sent_reset_count{0};
  u64 connection_drop_count{0};
  u64 receive_memory_drop_count{0};
  u64 listen_drop_count{0};
  u64 retransmit_timeout_count{0};
  u64 listen_overflow_count{0};
  u64 syn_retransmit_count{0};
  u64 fast_retransmit_count{0};
  u64 spurious_retransmit_timeout_count{0};
  u64 receive_queue_drop_count{0};
  u64 backlog_drop_count{0};
  u64 request_queue_full_drop_count{0};
  u64 retransmit_failure_count{0};
  u32 available_fields{0};

  pure fn has_field(tcp_statistics_field field) const wontthrow -> bool
  {
    return (available_fields & static_cast<u32>(field)) != 0;
  }
};

fn read_tcp_statistics(tcp_statistics &statistics) wontthrow -> bool;

enum class network_socket_protocol : u8
{
  Tcp,
  Udp,
  Unix,
};

enum class network_socket_process_mode : u8
{
  WithoutProcesses,
  WithProcesses,
};

enum class network_unix_socket_type : u8
{
  Stream,
  Datagram,
  SequentialPacket,
};

enum class network_socket_state : u8
{
  Unconnected,
  Listen,
  SynSent,
  SynReceived,
  Established,
  CloseWait,
  FinWait1,
  Closing,
  LastAck,
  FinWait2,
  TimeWait,
  Closed,
  Unknown,
};

struct network_socket_entry
{
  String local_address{heap_allocator()};
  String peer_address{heap_allocator()};
  u64 identity{0};
  u64 peer_identity{0};
  u64 receive_queue_bytes{0};
  u64 send_queue_bytes{0};
  u64 owner_start_token{0};
  u32 process_id{0};
  u16 local_port{0};
  u16 peer_port{0};
  network_socket_protocol protocol{network_socket_protocol::Tcp};
  network_address_family family{network_address_family::IPv4};
  network_socket_state state{network_socket_state::Unknown};
  network_unix_socket_type unix_type{network_unix_socket_type::Stream};
  u32 owner_id{0};
  bool has_owner_start_token{false};
  bool has_owner_id{false};
};

fn has_network_socket_listing() wontthrow -> bool;
fn network_sockets(network_socket_process_mode process_mode) throws
    -> ArrayList<network_socket_entry>;

enum class connect_probe_result : u8
{
  Connected,
  Refused,
  TimedOut,
  Unreachable,
};

fn probe_tcp_connect(StringView host, u16 port,
                     u32 timeout_milliseconds) wontthrow
    -> connect_probe_result;

struct user_session
{
  String user{heap_allocator()};
  String terminal{heap_allocator()};
  i64 login_time{0};
};

fn logged_in_users() throws -> ArrayList<user_session>;

struct processor_counts
{
  usize online_count{1};
  usize configured_count{1};
};

fn get_processor_counts() wontthrow -> processor_counts;

fn get_home_directory() throws -> Maybe<Path>;

fn get_home_for_user(StringView username) throws -> Maybe<Path>;

fn enumerate_users() throws -> ArrayList<String>;

fn enumerate_groups() throws -> ArrayList<String>;

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

/* Raised while a CHLD trap action is installed. An arrival is recorded and the
   trap drain is woken only under this flag. A child that no action observes
   leaves a signal that is already queued for the next boundary of its own. */
extern volatile sig_atomic_t CHILD_TRAP_ARMED;

/* Arm or disarm the child wake from the trap table. The evaluator calls this
   whenever the table changes, because a platform whose signal table has no
   CHLD entry never reaches set_trap_handler for it. */
fn set_child_trap_armed(bool is_armed) wontthrow -> void;

/* A signal the startup blocked is unblocked here, after its disposition is in
   place. */
fn set_trap_handler(i32 signal_number) throws -> void;

/* Install the ignore disposition for a signal, for a trap with an empty action
   such as trap "" INT, so the signal is discarded. */
fn set_trap_ignore(i32 signal_number) throws -> void;

constexpr i32 ENTRY_IGNORED_SIGNAL_LIMIT = 64;

fn get_entry_ignored_signals() wontthrow -> u64;

/* Restore a signal's default disposition when its trap is removed. SIGINT
   returns to the shell's interrupt handler, every other signal to SIG_DFL. A
   signal the install unblocked is blocked again. */
fn clear_trap_handler(i32 signal_number) throws -> void;

fn take_pending_signal() wontthrow -> i32;

/* Report the lowest queued signal that is not the child signal, leaving its
   flag in place for the drain. A blocking wait breaks out on this and reports
   128 plus the number, the way bash reports an interrupted wait. */
mustuse fn peek_pending_signal_besides_child() wontthrow -> i32;

fn note_child_reaped() wontthrow -> void;
mustuse fn take_reaped_child_count() wontthrow -> u32;
mustuse fn has_reaped_child_arrival() wontthrow -> bool;
fn clear_reaped_child_arrival() wontthrow -> void;

fn monotonic_nanos() wontthrow -> u64;

fn realtime_microseconds() wontthrow -> u64;

/* A negative epoch renders the current time, the bash -1 and -2 forms. */
fn format_local_time(StringView format, i64 epoch) throws -> String;

struct terminal_dimensions
{
  u32 columns{0};
  u32 rows{0};
};

fn get_terminal_dimensions(descriptor output = KOSH_STDOUT) wontthrow
    -> Maybe<terminal_dimensions>;
enum class terminal_settings_output_mode : u8
{
  Normal,
  Encoded,
  All,
};

fn terminal_settings(descriptor terminal, Allocator allocator,
                     terminal_settings_output_mode mode) throws
    -> Maybe<String>;

enum class terminal_settings_apply_kind : u8
{
  Success,
  InvalidSetting,
  SystemError,
};

struct terminal_settings_apply_result
{
  terminal_settings_apply_kind kind;
  usize setting_position;
};

fn apply_terminal_settings(descriptor terminal,
                           const ArrayList<String> &settings) wontthrow
    -> terminal_settings_apply_result;

/* The user and system seconds this process's children have consumed so far,
   read from RUSAGE_CHILDREN. Windows has no equivalent and reports zero. */
struct child_cpu_times
{
  double user_seconds{0};
  double system_seconds{0};
};

fn read_child_cpu_times() wontthrow -> child_cpu_times;

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

fn run_measured(const ArrayList<String> &argv,
                const Maybe<descriptor> &inherited_handle = {},
                measured_output output = measured_output::Inherit) throws
    -> Maybe<measured_result>;
fn get_priority(i64 id, priority_target target) wontthrow -> Maybe<i32>;
fn set_priority(i64 id, i32 priority, priority_target target) wontthrow -> bool;
fn run_nice(const ArrayList<String> &argv, i32 increment) throws -> Maybe<i32>;
struct nohup_options
{
  descriptor input{KOSH_INVALID_FD};
  descriptor output{KOSH_INVALID_FD};
  descriptor error{KOSH_INVALID_FD};
  StringView home{};
};

fn run_nohup(const ArrayList<String> &argv, const nohup_options &options) throws
    -> Maybe<i32>;

struct system_log_options
{
  StringView tag{};
  StringView priority{};
  StringView message{};
  bool should_include_pid{false};
  bool should_copy_to_stderr{false};
};

fn write_system_log(const system_log_options &options) wontthrow -> bool;

struct program_execution_options
{
  StringView source{};
  i64 process_group_id{0};
  script_fallback_policy fallback{script_fallback_policy::Reject};
  terminal_handoff handoff{terminal_handoff::Keep};
  process_group_mode process_group{process_group_mode::Inherit};
};

/* Script fallback returns KOSH_INVALID_PROCESS when it is allowed and the file
   has no executable format. */
fn execute_program(ExecContext &ec,
                   const program_execution_options &options = {}) throws
    -> process;

fn shell_has_controlling_terminal() wontthrow -> bool;

fn canonical_path(const Path &path) wontthrow -> Maybe<Path>;
fn path_from_file_id(StringView filesystem_path, u64 file_id) wontthrow
    -> Maybe<Path>;

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
  opaque *cleanup{nullptr};
  bool should_evaluate_child{false};
};

enum class process_substitution_direction : u8
{
  CommandReads,
  CommandWrites,
};

struct subshell_bootstrap
{
  subshell_bootstrap() = default;
  subshell_bootstrap(subshell_bootstrap &&other) noexcept;
  ~subshell_bootstrap();

  fn operator=(subshell_bootstrap &&other) noexcept -> subshell_bootstrap &;
  subshell_bootstrap(const subshell_bootstrap &) = delete;
  fn operator=(const subshell_bootstrap &)->subshell_bootstrap & = delete;

  fn release_process_ownership() wontthrow -> void;

  String payload{heap_allocator()};
  ArrayList<process> processes{heap_allocator()};
  u32 source_length{0};
  root_evaluation_mode evaluation_mode{root_evaluation_mode::Normal};
  bool owns_processes{false};

private:
  fn close_owned_processes() wontthrow -> void;
};

struct process_substitution_options
{
  StringView source{};
  bool source_traces_enabled{true};
  const subshell_bootstrap *bootstrap{nullptr};
  StringView shell_name{};
  i32 previous_exit_status{0};
  i64 shell_process_id{0};
  usize subshell_depth{0};
  process_substitution_direction direction{
      process_substitution_direction::CommandReads};
  mimic_mood mood{static_cast<mimic_mood>(0)};
};

fn launch_process_substitution(const process_substitution_options &options)
    throws -> process_substitution_launch;
fn release_unused_process_substitution(opaque *cleanup) wontthrow -> void;

struct fork_compound_stage_options
{
  Maybe<descriptor> in_fd{None};
  Maybe<descriptor> out_fd{None};
  Maybe<descriptor> err_fd{None};
  SourceLocation location{};
  StringView diagnostic_source{};
  i64 process_group_id{0};
  process_group_mode process_group{process_group_mode::Inherit};
};

fn try_fork_compound_stage(const fork_compound_stage_options &options) throws
    -> Maybe<process>;

fn try_fork_job_process() throws -> Maybe<process>;
fn can_fork_evaluator() wontthrow -> bool;

struct compound_stage_launch
{
  process child{KOSH_INVALID_PROCESS};
  bool should_evaluate_child{false};
};

struct compound_stage_options
{
  StringView source{};
  Maybe<descriptor> in_fd{None};
  Maybe<descriptor> out_fd{None};
  Maybe<descriptor> err_fd{None};
  SourceLocation location{};
  StringView diagnostic_source{};
  i64 process_group_id{0};
  const subshell_bootstrap *bootstrap{nullptr};
  StringView shell_name{};
  i32 previous_exit_status{0};
  i64 shell_process_id{0};
  usize subshell_depth{0};
  mimic_mood mood{static_cast<mimic_mood>(0)};
  process_group_mode process_group{process_group_mode::Inherit};
};

fn launch_compound_stage(const compound_stage_options &options) throws
    -> compound_stage_launch;

fn register_platform_flags(FlagList &flags) throws -> void;
fn initialize_platform_runtime() wontthrow -> void;
fn take_subshell_bootstrap() wontthrow -> subshell_bootstrap;

/* A forked pipeline-stage child calls this so it never runs the parent's
   cleanup inside the duplicated process. */
[[noreturn]] fn exit_process_immediately(i32 status) wontthrow -> void;

/* It does not fork, so on success it never returns. */
fn replace_process(ExecContext &&ec) throws -> void;

fn redirect_self(const ExecContext &ec) throws -> void;

} /* namespace os */

} /* namespace koshka */

#include "PlatformBatch.hpp"
