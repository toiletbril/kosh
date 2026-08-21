#include "Cli.hpp"
#include "Common.hpp"
#include "Debug.hpp"
#include "Errors.hpp"
#include "Eval.hpp"
#include "Platform.hpp"
#include "Trace.hpp"
#include "Utils.hpp"

#define KOSH_UMASK(mask) _umask(static_cast<int>(mask))

namespace koshka {

namespace os {

fn write_fd(os::descriptor fd, const opaque *buf, usize size) wontthrow
    -> Maybe<usize>
{
  let const requested_size =
      size > MAXDWORD ? MAXDWORD : static_cast<DWORD>(size);
  DWORD written_size = 0;
  if (WriteFile(fd, buf, requested_size, &written_size, nullptr) == FALSE) {
    errno = EIO;
    switch (GetLastError()) {
    case ERROR_BROKEN_PIPE:
    case ERROR_NO_DATA:
    case ERROR_PIPE_NOT_CONNECTED:
    case ERROR_NETNAME_DELETED: errno = EPIPE; break;
    default: break;
    }
    return koshka::None;
  }
  return static_cast<usize>(written_size);
}

fn write_to_numbered_fd(i64 fd_number, const opaque *buf, usize size) wontthrow
    -> Maybe<usize>
{
  let const handle = descriptor_from_fd_number(fd_number);
  if (handle == INVALID_HANDLE_VALUE) return koshka::None;
  return write_fd(handle, buf, size);
}

fn read_fd(os::descriptor fd, opaque *buf, usize size) wontthrow -> Maybe<usize>
{
  let const requested_size =
      size > MAXDWORD ? MAXDWORD : static_cast<DWORD>(size);
  DWORD read_size = 0;
  if (ReadFile(fd, buf, requested_size, &read_size, nullptr) == FALSE) {
    let const error = GetLastError();
    if (error == ERROR_BROKEN_PIPE || error == ERROR_NO_DATA ||
        error == ERROR_PIPE_NOT_CONNECTED || error == ERROR_NETNAME_DELETED ||
        error == ERROR_HANDLE_EOF)
    {
      return 0;
    }
    return koshka::None;
  }
  return static_cast<usize>(read_size);
}

fn descriptor_is_seekable(os::descriptor fd) wontthrow -> bool
{
  LARGE_INTEGER distance{};
  return SetFilePointerEx(fd, distance, nullptr, FILE_CURRENT) != FALSE;
}

fn rewind_descriptor(os::descriptor fd, usize byte_count) wontthrow -> bool
{
  LARGE_INTEGER distance{};
  distance.QuadPart = -static_cast<LONGLONG>(byte_count);
  return SetFilePointerEx(fd, distance, nullptr, FILE_CURRENT) != FALSE;
}

fn wait_for_fd_readable(os::descriptor fd, i64 timeout_nanos) wontthrow -> i32
{
  let const file_type = GetFileType(fd);
  if (file_type == FILE_TYPE_UNKNOWN && GetLastError() != NO_ERROR) return -1;
  if (file_type == FILE_TYPE_DISK) return 1;

  let const has_timeout = timeout_nanos >= 0;
  let const started_at = has_timeout ? monotonic_nanos() : 0;
  let const timeout = has_timeout ? static_cast<u64>(timeout_nanos) : 0;
  INPUT_RECORD *console_events = nullptr;
  DWORD console_event_capacity = 0;
  defer
  {
    if (console_events != nullptr)
      HeapFree(GetProcessHeap(), 0, console_events);
  };

  loop
  {
    if (file_type == FILE_TYPE_PIPE) {
      DWORD available_byte_count = 0;
      if (PeekNamedPipe(fd, nullptr, 0, nullptr, &available_byte_count,
                        nullptr) != FALSE)
      {
        if (available_byte_count != 0) return 1;
      } else {
        let const error = GetLastError();
        if (error == ERROR_BROKEN_PIPE || error == ERROR_NO_DATA) return 1;
        return -1;
      }
    } else if (file_type == FILE_TYPE_CHAR) {
      DWORD console_mode = 0;
      if (GetConsoleMode(fd, &console_mode) == FALSE) {
        let const wait_result = WaitForSingleObject(fd, 0);
        if (wait_result == WAIT_OBJECT_0) return 1;
        if (wait_result == WAIT_FAILED) return -1;
      } else {
        DWORD event_count = 0;
        if (GetNumberOfConsoleInputEvents(fd, &event_count) == FALSE) return -1;
        if (event_count == 0) {
          if (has_timeout && monotonic_nanos() - started_at >= timeout) {
            return 0;
          }
          Sleep(1);
          continue;
        }

        if (event_count > console_event_capacity) {
          let const resized =
              console_events == nullptr
                  ? HeapAlloc(GetProcessHeap(), 0,
                              static_cast<usize>(event_count) *
                                  sizeof(INPUT_RECORD))
                  : HeapReAlloc(GetProcessHeap(), 0, console_events,
                                static_cast<usize>(event_count) *
                                    sizeof(INPUT_RECORD));
          if (resized == nullptr) return -1;
          console_events = static_cast<INPUT_RECORD *>(resized);
          console_event_capacity = event_count;
        }
        DWORD peeked_event_count = 0;
        let const did_peek = PeekConsoleInputA(fd, console_events, event_count,
                                               &peeked_event_count);
        let const needs_complete_line = (console_mode & ENABLE_LINE_INPUT) != 0;
        bool is_readable = false;
        if (did_peek != FALSE) {
          for (DWORD event_index = 0; event_index < peeked_event_count;
               event_index++)
          {
            let const &event = console_events[event_index];
            if (event.EventType != KEY_EVENT ||
                event.Event.KeyEvent.bKeyDown == FALSE)
              continue;

            let const character = event.Event.KeyEvent.uChar.UnicodeChar;
            if ((!needs_complete_line && character != 0) ||
                (needs_complete_line &&
                 (character == '\r' || character == '\n' || character == 0x1a)))
            {
              is_readable = true;
              break;
            }
          }
        }
        if (did_peek == FALSE) return -1;
        if (is_readable) return 1;
      }
    } else {
      return -1;
    }

    if (has_timeout && monotonic_nanos() - started_at >= timeout) return 0;
    Sleep(1);
  }
}

fn close_fd(os::descriptor fd) wontthrow -> bool
{
  const DWORD prior_error = GetLastError();
  if (CloseHandle(fd) == FALSE) return false;
  SetLastError(prior_error);
  return true;
}

fn TempFileSet::track(Path &&path) throws -> void { m_paths.push(steal(path)); }
fn TempFileSet::count() const wontthrow -> usize { return m_paths.count(); }
fn TempFileSet::cleanup_from(usize mark) wontthrow -> void
{
  /* A failed delete keeps the path and retries once the descriptor closes. */
  usize kept = mark;
  for (usize i = mark; i < m_paths.count(); i++) {
    if (DeleteFileA(m_paths[i].c_str()) != FALSE) continue;
    if (kept != i) m_paths[kept] = steal(m_paths[i]);
    kept++;
  }
  while (m_paths.count() > kept)
    m_paths.remove(m_paths.count() - 1);
}

/* Windows inherits handles per CreateProcess, so this is a no-op. */
fn make_fd_inheritable(os::descriptor fd) wontthrow -> void { unused(fd); }

fn redirect_stdout(os::descriptor target) wontthrow -> os::descriptor
{
  os::descriptor saved = GetStdHandle(STD_OUTPUT_HANDLE);
  SetStdHandle(STD_OUTPUT_HANDLE, target);
  note_descriptor_rebound();

  return saved;
}

fn restore_stdout(os::descriptor saved) wontthrow -> void
{
  SetStdHandle(STD_OUTPUT_HANDLE, saved);
  note_descriptor_rebound();
}

/* Windows addresses only the three standard streams. */
static fn std_handle_slot_for_shell_fd(i32 shell_fd) -> Maybe<DWORD>
{
  switch (shell_fd) {
  case 0: return STD_INPUT_HANDLE;
  case 1: return STD_OUTPUT_HANDLE;
  case 2: return STD_ERROR_HANDLE;
  default: return koshka::None;
  }
}

static fn standard_handle_is_referenced(os::descriptor handle) wontthrow -> bool
{
  static constexpr DWORD STANDARD_HANDLE_SLOTS[] = {
      STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
  for (let const slot : STANDARD_HANDLE_SLOTS)
    if (GetStdHandle(slot) == handle) return true;
  return false;
}

static fn standard_handle_is_owned_by_runtime(os::descriptor handle) wontthrow
    -> bool
{
  for (i32 shell_fd = 0; shell_fd <= 2; shell_fd++)
    if (descriptor_from_fd_number(shell_fd) == handle) return true;
  return false;
}

fn save_and_replace_descriptor(i32 shell_fd, os::descriptor target) wontthrow
    -> saved_descriptor
{
  saved_descriptor result{};
  result.shell_fd = shell_fd;

  const Maybe<DWORD> slot = std_handle_slot_for_shell_fd(shell_fd);
  if (!slot.has_value()) {
    result.is_dup2_ok = false;
    return result;
  }

  if (target == nullptr || target == INVALID_HANDLE_VALUE) {
    result.is_dup2_ok = false;
    return result;
  }

  let const original = GetStdHandle(*slot);
  result.was_open = original != nullptr && original != INVALID_HANDLE_VALUE;
  if (result.was_open &&
      DuplicateHandle(GetCurrentProcess(), original, GetCurrentProcess(),
                      &result.saved, 0, FALSE, DUPLICATE_SAME_ACCESS) == 0)
  {
    result.is_dup2_ok = false;
    return result;
  }

  /* SetStdHandle does not copy, so the target is duplicated here and the dup
     stays valid until restore_descriptor closes it. */
  HANDLE duplicate = INVALID_HANDLE_VALUE;
  if (DuplicateHandle(GetCurrentProcess(), target, GetCurrentProcess(),
                      &duplicate, 0, TRUE, DUPLICATE_SAME_ACCESS) == 0)
  {
    if (result.was_open) CloseHandle(result.saved);
    result.is_dup2_ok = false;
    return result;
  }
  if (SetStdHandle(*slot, duplicate) == FALSE) {
    CloseHandle(duplicate);
    if (result.was_open) CloseHandle(result.saved);
    result.is_dup2_ok = false;
    return result;
  }
  result.is_dup2_ok = true;
  note_descriptor_rebound();

  return result;
}

fn restore_descriptor(const saved_descriptor &saved) wontthrow -> void
{
  if (!saved.is_dup2_ok) return;
  const Maybe<DWORD> slot = std_handle_slot_for_shell_fd(saved.shell_fd);
  if (!slot.has_value()) return;
  let const replaced = GetStdHandle(*slot);
  if (SetStdHandle(*slot, saved.was_open ? saved.saved
                                         : INVALID_HANDLE_VALUE) == FALSE)
    return;

  note_descriptor_rebound();

  if (replaced != nullptr && replaced != INVALID_HANDLE_VALUE &&
      !standard_handle_is_referenced(replaced) &&
      !standard_handle_is_owned_by_runtime(replaced))
    CloseHandle(replaced);
}

fn save_descriptor(i32 shell_fd) wontthrow -> saved_descriptor
{
  saved_descriptor result{};
  result.shell_fd = shell_fd;
  const Maybe<DWORD> slot = std_handle_slot_for_shell_fd(shell_fd);
  if (!slot.has_value()) {
    result.is_dup2_ok = false;
    return result;
  }
  let const original = GetStdHandle(*slot);
  result.was_open = original != nullptr && original != INVALID_HANDLE_VALUE;
  if (result.was_open &&
      DuplicateHandle(GetCurrentProcess(), original, GetCurrentProcess(),
                      &result.saved, 0, FALSE, DUPLICATE_SAME_ACCESS) == 0)
  {
    result.is_dup2_ok = false;
    return result;
  }
  result.is_dup2_ok = true;
  return result;
}

/* Windows has no /dev/tty rebind. */
fn reopen_terminal_as_stdin() wontthrow -> bool { return false; }

/* Windows has no POSIX process groups, so the terminal handoff is a no-op. */
fn shell_has_controlling_terminal() wontthrow -> bool { return false; }
fn give_controlling_terminal_to(process p) wontthrow -> void { unused(p); }
fn give_controlling_terminal_to_process_group(i64 process_group_id) wontthrow
    -> void
{
  unused(process_group_id);
}
fn reclaim_controlling_terminal() wontthrow -> void {}

fn descriptor_for_shell_fd(i32 shell_fd) wontthrow -> os::descriptor
{
  const Maybe<DWORD> slot = std_handle_slot_for_shell_fd(shell_fd);
  if (!slot.has_value()) return KOSH_INVALID_FD;
  let const handle = GetStdHandle(*slot);
  return handle != nullptr ? handle : KOSH_INVALID_FD;
}

fn descriptor_from_fd_number(i64 fd_number) wontthrow -> os::descriptor
{
  return reinterpret_cast<os::descriptor>(
      _get_osfhandle(static_cast<int>(fd_number)));
}

fn replace_descriptor(i32 shell_fd, os::descriptor target) wontthrow -> bool
{
  const Maybe<DWORD> slot = std_handle_slot_for_shell_fd(shell_fd);
  if (!slot.has_value()) return false;
  if (target == nullptr || target == INVALID_HANDLE_VALUE) return false;

  HANDLE duplicate = INVALID_HANDLE_VALUE;
  if (DuplicateHandle(GetCurrentProcess(), target, GetCurrentProcess(),
                      &duplicate, 0, TRUE, DUPLICATE_SAME_ACCESS) == 0)
    return false;

  let const previous = GetStdHandle(*slot);
  if (SetStdHandle(*slot, duplicate) == FALSE) {
    CloseHandle(duplicate);
    return false;
  }
  note_descriptor_rebound();

  if (previous != nullptr && previous != INVALID_HANDLE_VALUE &&
      !standard_handle_is_referenced(previous) &&
      !standard_handle_is_owned_by_runtime(previous))
    CloseHandle(previous);

  return true;
}

fn close_shell_fd(i32 shell_fd) wontthrow -> bool
{
  const Maybe<DWORD> slot = std_handle_slot_for_shell_fd(shell_fd);
  if (!slot.has_value()) return false;
  const os::descriptor handle = GetStdHandle(*slot);
  if (handle == nullptr || handle == INVALID_HANDLE_VALUE) return false;
  if (SetStdHandle(*slot, INVALID_HANDLE_VALUE) == FALSE) return false;

  note_descriptor_rebound();

  return standard_handle_is_referenced(handle) || CloseHandle(handle) != FALSE;
}

fn allocate_free_shell_fd(i32 floor_fd) wontthrow -> i32
{
  (void) floor_fd;
  return -1;
}

fn get_current_user() -> Maybe<String>
{
  DWORD size = 0;
  GetUserNameA(nullptr, &size);
  if (GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
    ArrayList<char> buffer{heap_allocator()};
    buffer.reserve(size);
    for (DWORD i = 0; i < size; i++)
      buffer.push('\0');
    if (GetUserNameA(buffer.begin(), &size))
      return String{
          StringView{buffer.begin(), size - 1}
      };
  }
  return koshka::None;
}

fn get_hostname() throws -> Maybe<String>
{
  char buffer[MAX_COMPUTERNAME_LENGTH + 1];
  DWORD size = sizeof(buffer);
  if (GetComputerNameA(buffer, &size))
    return String{
        StringView{buffer, size}
    };
  return koshka::None;
}

fn get_processor_counts() wontthrow -> processor_counts
{
  processor_counts counts{};
  let const online = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
  let const configured = GetMaximumProcessorCount(ALL_PROCESSOR_GROUPS);
  if (online != 0) counts.online_count = static_cast<usize>(online);
  if (configured != 0) counts.configured_count = static_cast<usize>(configured);
  ULONG cpu_set_count = 0;
  using get_process_default_cpu_sets_fn =
      BOOL(WINAPI *)(HANDLE, PULONG, ULONG, PULONG);
  static let const get_process_default_cpu_sets = [] {
    let const address = GetProcAddress(GetModuleHandleA("kernel32.dll"),
                                       "GetProcessDefaultCpuSets");
    get_process_default_cpu_sets_fn function = nullptr;
    static_assert(sizeof(function) == sizeof(address));
    __builtin_memcpy(&function, &address, sizeof(function));
    return function;
  }();
  if (get_process_default_cpu_sets != nullptr)
    unused(get_process_default_cpu_sets(GetCurrentProcess(), nullptr, 0,
                                        &cpu_set_count));
  if (cpu_set_count != 0) {
    let const selected_count = static_cast<usize>(cpu_set_count);
    if (selected_count < counts.online_count)
      counts.online_count = selected_count;
  } else {
    USHORT group_count = 64;
    USHORT groups[64];
    usize affinity_count = 0;
    if (GetProcessGroupAffinity(GetCurrentProcess(), &group_count, groups)) {
      if (group_count == 1) {
        DWORD_PTR process_affinity = 0;
        DWORD_PTR system_affinity = 0;
        if (GetProcessAffinityMask(GetCurrentProcess(), &process_affinity,
                                   &system_affinity))
        {
          while (process_affinity != 0) {
            affinity_count += process_affinity & 1;
            process_affinity >>= 1;
          }
        }
      } else {
        for (USHORT group_index = 0; group_index < group_count; group_index++)
          affinity_count += GetActiveProcessorCount(groups[group_index]);
      }
      if (affinity_count != 0) counts.online_count = affinity_count;
    }
  }
  if (counts.configured_count < counts.online_count)
    counts.configured_count = counts.online_count;
  return counts;
}

fn get_home_directory() -> Maybe<Path>
{
  if (Maybe<String> home = get_environment_variable("HOME"))
    return Path{StringView{*home}};
  if (Maybe<String> home = get_environment_variable("USERPROFILE"))
    return Path{StringView{*home}};
  return koshka::None;
}

/* Windows has no /etc/passwd, so ~user stays literal. */
fn get_home_for_user(StringView username) throws -> Maybe<Path>
{
  unused(username);
  return koshka::None;
}

fn enumerate_users() throws -> ArrayList<String>
{
  return ArrayList<String>{heap_allocator()};
}

static const DWORD PARENT_SHELL_PID = GetCurrentProcessId();
static constexpr uintptr PROCESS_REFERENCE_MASK = 3u;
static constexpr uintptr PID_REFERENCE_TAG = 1u;
static constexpr uintptr PROCESS_GROUP_REFERENCE_TAG = 3u;

fn is_stdin_a_tty() wontthrow -> bool { return is_fd_a_tty(KOSH_STDIN); }

fn is_stdout_a_tty() wontthrow -> bool { return is_fd_a_tty(KOSH_STDOUT); }

fn is_stderr_a_tty() wontthrow -> bool { return is_fd_a_tty(KOSH_STDERR); }

fn is_fd_a_tty(descriptor fd) wontthrow -> bool
{
  DWORD console_mode = 0;
  return GetConsoleMode(fd, &console_mode) != FALSE;
}

terminal_echo_guard::terminal_echo_guard(descriptor input,
                                         bool should_disable) wontthrow
    : m_input(input)
{
  if (!should_disable || !is_fd_a_tty(input)) return;
  if (GetConsoleMode(input, &m_original_mode) == FALSE) {
    m_did_succeed = false;
    return;
  }
  if (SetConsoleMode(input, m_original_mode & ~ENABLE_ECHO_INPUT) == FALSE) {
    m_did_succeed = false;
    return;
  }

  m_should_restore = true;
}

terminal_echo_guard::~terminal_echo_guard()
{
  if (m_should_restore) unused(SetConsoleMode(m_input, m_original_mode));
}

pure fn terminal_echo_guard::did_succeed() const wontthrow -> bool
{
  return m_did_succeed;
}

fn allocate_aligned(usize length, usize alignment) wontthrow -> opaque *
{
  return _aligned_malloc(length, alignment);
}

fn free_aligned(opaque *pointer) wontthrow -> void { _aligned_free(pointer); }

fn collate_compare(const String &left, const String &right) wontthrow -> int
{
  if (left < right) return -1;
  return right < left ? 1 : 0;
}

fn compile_regex(StringView pattern, case_sensitivity sensitivity,
                 compiled_regex &out) throws -> regex_compile_result
{
  unused(pattern);
  unused(sensitivity);
  unused(out);
  return regex_compile_result::Invalid;
}

fn execute_regex(compiled_regex &compiled, StringView subject,
                 ArrayList<regex_span> &spans, String &error_message,
                 Allocator scratch) throws -> regex_match_result
{
  unused(compiled);
  unused(subject);
  unused(spans);
  unused(error_message);
  unused(scratch);
  return regex_match_result::Error;
}

fn free_regex(compiled_regex &compiled) wontthrow -> void { unused(compiled); }

fn compile_search_regex(StringView pattern, case_sensitivity sensitivity,
                        compiled_regex &out) throws -> regex_compile_result
{
  out.pattern = String{heap_allocator(), pattern};
  out.is_case_insensitive = sensitivity == case_sensitivity::Insensitive;
  return regex_compile_result::Ok;
}

fn regex_matches(compiled_regex &compiled, StringView subject) throws -> bool
{
  const StringView needle = compiled.pattern.view();
  if (needle.length == 0) return true;
  if (needle.length > subject.length) return false;

  if (!compiled.is_case_insensitive) {
    usize start = 0;
    while (start + needle.length <= subject.length) {
      let const found = subject.substring(start).find_character(needle[0]);
      if (!found.has_value()) return false;
      start += *found;
      if (start + needle.length > subject.length) return false;

      bool is_matched = true;
      for (usize k = 1; k < needle.length; k++)
        if (subject[start + k] != needle[k]) {
          is_matched = false;
          break;
        }
      if (is_matched) return true;
      start++;
    }
    return false;
  }

  for (usize start = 0; start + needle.length <= subject.length; start++) {
    bool is_matched = true;
    for (usize k = 0; k < needle.length; k++) {
      if (utils::ascii_to_lower(subject[start + k]) !=
          utils::ascii_to_lower(needle[k]))
      {
        is_matched = false;
        break;
      }
    }
    if (is_matched) return true;
  }

  return false;
}

fn read_process_cpu_times() wontthrow -> cpu_times { return cpu_times{}; }

fn get_resource_limit(resource_kind kind, resource_limit &out) wontthrow -> bool
{
  unused(kind);
  unused(out);
  SetLastError(ERROR_NOT_SUPPORTED);
  return false;
}

fn set_resource_limit(resource_kind kind, const resource_limit &limit) wontthrow
    -> bool
{
  unused(kind);
  unused(limit);
  SetLastError(ERROR_NOT_SUPPORTED);
  return false;
}
fn shell_fd_is_a_tty(int shell_fd) wontthrow -> bool
{
  return is_fd_a_tty(reinterpret_cast<descriptor>(_get_osfhandle(shell_fd)));
}

pure fn is_directory_separator(char c) wontthrow -> bool
{
  return c == '/' || c == '\\';
}

fn terminal_size(u32 &columns, u32 &rows, descriptor output) wontthrow -> bool
{
  CONSOLE_SCREEN_BUFFER_INFO info;
  if (GetConsoleScreenBufferInfo(output, &info) == 0) return false;
  const i32 width = info.srWindow.Right - info.srWindow.Left + 1;
  const i32 height = info.srWindow.Bottom - info.srWindow.Top + 1;
  if (width <= 0 || height <= 0) return false;
  columns = static_cast<u32>(width);
  rows = static_cast<u32>(height);

  return true;
}

fn get_environment_variable(StringView key) -> Maybe<String>
{
  String key_string{key};
  char inline_buffer[256];
  SetLastError(ERROR_SUCCESS);
  let required_size =
      GetEnvironmentVariableA(key_string.c_str(), inline_buffer,
                              static_cast<DWORD>(countof(inline_buffer)));
  if (required_size == 0) {
    return GetLastError() == ERROR_ENVVAR_NOT_FOUND
               ? Maybe<String>{}
               : Maybe<String>{String{heap_allocator()}};
  }
  if (required_size < countof(inline_buffer))
    return String{
        StringView{inline_buffer, static_cast<usize>(required_size)}
    };

  let buffer = ArrayList<char>{heap_allocator()};
  buffer.reserve(static_cast<usize>(required_size));
  let const value_length = GetEnvironmentVariableA(
      key_string.c_str(), buffer.begin(), required_size);
  if (value_length == 0 || value_length >= required_size) return koshka::None;
  return String{
      StringView{buffer.begin(), static_cast<usize>(value_length)}
  };
}

fn set_environment_variable(StringView key, StringView value) -> void
{
  String key_string{key};
  String value_string{value};
  SetEnvironmentVariableA(key_string.c_str(), value_string.c_str());
}

fn unset_environment_variable(StringView key) -> void
{
  String key_string{key};
  SetEnvironmentVariableA(key_string.c_str(), nullptr);
}

fn signal_internal_diagnostic() wontthrow -> void
{
  char marker_path[MAX_PATH];
  let const marker_path_length = GetEnvironmentVariableA(
      "KOSH_INTERNAL_DIAGNOSTIC_MARKER", marker_path, countof(marker_path));
  if (marker_path_length == 0 || marker_path_length >= countof(marker_path)) {
    return;
  }

  let const marker =
      CreateFileA(marker_path, FILE_APPEND_DATA,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_TEMPORARY, nullptr);
  if (marker == INVALID_HANDLE_VALUE) return;
  DWORD written = 0;
  WriteFile(marker, "x", 1, &written, nullptr);
  CloseHandle(marker);
}

fn environment_names() -> ArrayList<String>
{
  ArrayList<String> names{heap_allocator()};
  char *block = GetEnvironmentStringsA();
  if (block == nullptr) return names;
  defer { FreeEnvironmentStringsA(block); };
  for (char *entry = block; *entry != '\0';) {
    StringView pair{entry};
    if (pair[0] == '=') {
      entry += pair.length + 1;
      continue;
    }
    let const equals = pair.find_character('=');
    let const split =
        equals.has_value() ? pair.substring_of_length(0, *equals) : pair;
    names.push(String{split});
    entry += pair.length + 1;
  }
  return names;
}

fn sleep_for_seconds(double seconds) wontthrow -> void
{
  if (seconds <= 0.0) return;
  Sleep(static_cast<DWORD>(seconds * 1000.0));
}

} /* namespace os */

} /* namespace koshka */

namespace koshka {

namespace os {

const ProgramSuffixList PROGRAM_SUFFIXES{WINDOWS_PROGRAM_SUFFIXES};

fn normalize_program_name(String &program_name) -> program_name_info
{
  return normalize_windows_program_name(program_name);
}

} /* namespace os */

} /* namespace koshka */

namespace koshka {
namespace os {

fn get_current_process_id() wontthrow -> i64
{
  return static_cast<i64>(GetCurrentProcessId());
}

fn register_platform_flags(FlagList &flags) throws -> void { unused(flags); }

fn initialize_platform_runtime() wontthrow -> void
{
  for (i32 shell_fd = 0; shell_fd <= 2; shell_fd++) {
    let const runtime_handle = descriptor_from_fd_number(shell_fd);
    if (runtime_handle == nullptr || runtime_handle == INVALID_HANDLE_VALUE)
      continue;
    if (descriptor_for_shell_fd(shell_fd) == runtime_handle) continue;
    unused(replace_descriptor(shell_fd, runtime_handle));
  }
}

} /* namespace os */
} /* namespace koshka */
