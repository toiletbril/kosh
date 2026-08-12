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

fn TempFileSet::track(Path path) throws -> void { m_paths.push(steal(path)); }
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

  result.saved = GetStdHandle(*slot);
  result.was_open =
      result.saved != nullptr && result.saved != INVALID_HANDLE_VALUE;

  /* SetStdHandle does not copy, so the target is duplicated here and the dup
     stays valid until restore_descriptor closes it. */
  HANDLE duplicate = INVALID_HANDLE_VALUE;
  if (DuplicateHandle(GetCurrentProcess(), target, GetCurrentProcess(),
                      &duplicate, 0, TRUE, DUPLICATE_SAME_ACCESS) == 0)
  {
    result.is_dup2_ok = false;
    return result;
  }
  if (SetStdHandle(*slot, duplicate) == FALSE) {
    CloseHandle(duplicate);
    result.is_dup2_ok = false;
    return result;
  }
  result.replacement = duplicate;
  result.is_dup2_ok = true;
  note_descriptor_rebound();

  return result;
}

fn restore_descriptor(const saved_descriptor &saved) wontthrow -> void
{
  const Maybe<DWORD> slot = std_handle_slot_for_shell_fd(saved.shell_fd);
  if (!slot.has_value()) return;
  if (SetStdHandle(*slot, saved.was_open ? saved.saved
                                         : INVALID_HANDLE_VALUE) == FALSE)
    return;

  note_descriptor_rebound();

  if (saved.is_dup2_ok && saved.replacement != nullptr &&
      saved.replacement != INVALID_HANDLE_VALUE)
  {
    CloseHandle(saved.replacement);
  }
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
  result.saved = GetStdHandle(*slot);
  result.was_open =
      result.saved != nullptr && result.saved != INVALID_HANDLE_VALUE;
  result.replacement = INVALID_HANDLE_VALUE;
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

fn canonical_path(const Path &path) wontthrow -> Maybe<Path>
{
  let const do_resolve_direct =
      [](const Path &candidate, bool should_preserve_extended_prefix)
          wontthrow -> Maybe<Path> {
    let const handle = CreateFileA(
        candidate.c_str(), 0,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return koshka::None;
    defer { CloseHandle(handle); };

    char buffer[32768];
    let const length = GetFinalPathNameByHandleA(
        handle, buffer, sizeof(buffer), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (length == 0 || length >= sizeof(buffer)) return koshka::None;

    let const resolved = StringView{buffer, static_cast<usize>(length)};
    if (should_preserve_extended_prefix) return Path{resolved};
    if (resolved.starts_with(StringView{"\\\\?\\UNC\\"})) {
      let unc_path = String{"\\\\"};
      unc_path += resolved.substring(8);
      return Path{unc_path.view()};
    }
    if (resolved.starts_with(StringView{"\\\\?\\"}))
      return Path{resolved.substring(4)};
    return Path{resolved};
  };

  let const text = path.text().view();
  if (text.is_empty()) return koshka::None;
  let const has_extended_prefix =
      text.length >= 4 && is_directory_separator(text[0]) &&
      is_directory_separator(text[1]) && text[2] == '?' &&
      is_directory_separator(text[3]);

  let has_dot_component = false;
  usize scan_position = 0;
  while (scan_position < text.length) {
    while (scan_position < text.length &&
           is_directory_separator(text[scan_position]))
      scan_position++;
    let const component_start = scan_position;
    while (scan_position < text.length &&
           !is_directory_separator(text[scan_position]))
      scan_position++;
    let const component_length = scan_position - component_start;
    if ((component_length == 1 && text[component_start] == '.') ||
        (component_length == 2 && text[component_start] == '.' &&
         text[component_start + 1] == '.'))
    {
      has_dot_component = true;
      break;
    }
  }
  if (!has_dot_component) return do_resolve_direct(path, has_extended_prefix);

  char initial_path[32768];
  usize position = 0;
  let resolved = Path{};
  if (text.length >= 7 && is_directory_separator(text[0]) &&
      is_directory_separator(text[1]) && text[2] == '?' &&
      is_directory_separator(text[3]) && text[5] == ':' &&
      is_directory_separator(text[6]))
  {
    resolved = Path{text.substring_of_length(0, 7)};
    position = 7;
  } else if (text.length >= 8 && is_directory_separator(text[0]) &&
             is_directory_separator(text[1]) && text[2] == '?' &&
             is_directory_separator(text[3]) &&
             utils::ascii_to_lower(text[4]) == 'u' &&
             utils::ascii_to_lower(text[5]) == 'n' &&
             utils::ascii_to_lower(text[6]) == 'c' &&
             is_directory_separator(text[7]))
  {
    position = 8;
    while (position < text.length && !is_directory_separator(text[position]))
      position++;
    while (position < text.length && is_directory_separator(text[position]))
      position++;
    while (position < text.length && !is_directory_separator(text[position]))
      position++;
    resolved = Path{text.substring_of_length(0, position)};
  } else if (text.length >= 2 && is_directory_separator(text[0]) &&
             is_directory_separator(text[1]))
  {
    position = 2;
    while (position < text.length && !is_directory_separator(text[position]))
      position++;
    while (position < text.length && is_directory_separator(text[position]))
      position++;
    while (position < text.length && !is_directory_separator(text[position]))
      position++;
    resolved = Path{text.substring_of_length(0, position)};
  } else if (text.length >= 3 && text[1] == ':' &&
             is_directory_separator(text[2]))
  {
    resolved = Path{text.substring_of_length(0, 3)};
    position = 3;
  } else {
    let initial_length = DWORD{};
    if (text.length >= 2 && text[1] == ':') {
      char drive_directory[4]{text[0], ':', '.', '\0'};
      initial_length = GetFullPathNameA(drive_directory, countof(initial_path),
                                        initial_path, nullptr);
      position = 2;
    } else if (is_directory_separator(text[0])) {
      char drive_root[2]{text[0], '\0'};
      initial_length = GetFullPathNameA(drive_root, countof(initial_path),
                                        initial_path, nullptr);
      position = 1;
    } else {
      initial_length =
          GetCurrentDirectoryA(countof(initial_path), initial_path);
    }
    if (initial_length == 0 || initial_length >= countof(initial_path)) {
      return koshka::None;
    }
    resolved = Path{
        StringView{initial_path, static_cast<usize>(initial_length)}
    };
  }

  let initial_resolved = do_resolve_direct(resolved, has_extended_prefix);
  if (!initial_resolved.has_value()) return koshka::None;
  resolved = initial_resolved.take();

  while (position < text.length) {
    while (position < text.length && is_directory_separator(text[position]))
      position++;
    if (position >= text.length) break;

    let const component_start = position;
    while (position < text.length && !is_directory_separator(text[position]))
      position++;

    let candidate = resolved.clone();
    candidate.push_component(
        text.substring_of_length(component_start, position - component_start));
    let component_resolved = do_resolve_direct(candidate, has_extended_prefix);
    if (!component_resolved.has_value()) return koshka::None;
    resolved = component_resolved.take();
  }

  return resolved;
}

fn glob_matches(StringView pattern, Allocator allocator) throws
    -> ArrayList<String>
{
  let matches = ArrayList<String>{allocator};

  const String pattern_string{allocator, pattern};
  WIN32_FIND_DATAA find_data;
  const HANDLE handle = FindFirstFileA(pattern_string.c_str(), &find_data);
  if (handle == INVALID_HANDLE_VALUE) return matches;
  defer { FindClose(handle); };

  /* FindFirstFile yields bare names, so the directory prefix is kept to rebuild
     the path. */
  usize prefix_length = 0;
  for (usize i = 0; i < pattern.length; i++)
    if (pattern[i] == '/' || pattern[i] == '\\') prefix_length = i + 1;
  const StringView prefix = pattern.substring_of_length(0, prefix_length);

  do {
    const StringView name{find_data.cFileName};
    if (name == "." || name == "..") {
      continue;
    }

    let entry = String{allocator, prefix};
    entry += name;
    matches.push(steal(entry));
  } while (FindNextFileA(handle, &find_data) != 0);

  return matches;
}

fn directory_is_trusted_for_exec(const Path &directory) wontthrow -> bool
{
  /* PATH already grants programs in this directory execution authority. The
     POSIX writable-mode rejection has no faithful Windows equivalent because
     access is controlled by ordered ACLs rather than owner/group mode bits. */
  return directory.is_directory();
}

fn capture_program_output(const ArrayList<String> &argv,
                          u64 timeout_nanos) wontthrow -> Maybe<String>
{
  if (argv.is_empty()) return None;

  SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
  HANDLE read_end = INVALID_HANDLE_VALUE;
  HANDLE write_end = INVALID_HANDLE_VALUE;
  if (CreatePipe(&read_end, &write_end, &inheritable, 0) == FALSE) return None;
  defer { CloseHandle(read_end); };
  defer { CloseHandle(write_end); };
  if (SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0) == FALSE)
    return None;

  let const null_input =
      CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                  &inheritable, OPEN_EXISTING, 0, nullptr);
  if (null_input == INVALID_HANDLE_VALUE) return None;
  defer { CloseHandle(null_input); };

  let command_line = make_os_args(argv);
  STARTUPINFOA startup_info{};
  startup_info.cb = sizeof(startup_info);
  startup_info.dwFlags = STARTF_USESTDHANDLES;
  startup_info.hStdInput = null_input;
  startup_info.hStdOutput = write_end;
  startup_info.hStdError = write_end;

  PROCESS_INFORMATION process_info{};
  if (CreateProcessA(argv[0].c_str(), const_cast<LPSTR>(command_line.data()),
                     nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr,
                     &startup_info, &process_info) == FALSE)
  {
    return None;
  }
  defer { CloseHandle(process_info.hProcess); };
  defer { CloseHandle(process_info.hThread); };
  CloseHandle(write_end);
  write_end = INVALID_HANDLE_VALUE;

  let captured = String{heap_allocator()};
  const u64 deadline_nanos = monotonic_nanos() + timeout_nanos;
  bool was_timed_out = false;
  loop
  {
    DWORD available_byte_count = 0;
    if (PeekNamedPipe(read_end, nullptr, 0, nullptr, &available_byte_count,
                      nullptr) == FALSE)
    {
      let const error = GetLastError();
      if (error == ERROR_BROKEN_PIPE) break;
      return None;
    }

    if (available_byte_count != 0) {
      char buffer[4096];
      let const requested_count = available_byte_count < sizeof(buffer)
                                      ? available_byte_count
                                      : sizeof(buffer);
      DWORD read_count = 0;
      if (ReadFile(read_end, buffer, requested_count, &read_count, nullptr) ==
          FALSE)
      {
        return None;
      }
      captured.append(StringView{buffer, static_cast<usize>(read_count)});
      continue;
    }

    if (WaitForSingleObject(process_info.hProcess, 0) == WAIT_OBJECT_0) {
      /* The pipe can report empty just before the final child write arrives. */
      if (WaitForSingleObject(read_end, 1) == WAIT_OBJECT_0) continue;
      break;
    }
    if (monotonic_nanos() >= deadline_nanos) {
      was_timed_out = true;
      break;
    }
    Sleep(1);
  }

  if (was_timed_out) {
    TerminateProcess(process_info.hProcess, 1);
    WaitForSingleObject(process_info.hProcess, INFINITE);
    return None;
  }
  return captured;
}

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

  let const was_replaced = SetStdHandle(*slot, target) != FALSE;
  note_descriptor_rebound();

  return was_replaced;
}

fn close_shell_fd(i32 shell_fd) wontthrow -> bool
{
  const Maybe<DWORD> slot = std_handle_slot_for_shell_fd(shell_fd);
  if (!slot.has_value()) return false;
  const os::descriptor handle = GetStdHandle(*slot);
  if (handle == nullptr || handle == INVALID_HANDLE_VALUE) return false;
  if (SetStdHandle(*slot, INVALID_HANDLE_VALUE) == FALSE) return false;

  note_descriptor_rebound();

  return CloseHandle(handle) != FALSE;
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

static pure fn process_is_pid_reference(process p) wontthrow -> bool
{
  return (reinterpret_cast<uintptr_t>(p) & PROCESS_REFERENCE_MASK) ==
         PID_REFERENCE_TAG;
}

static pure fn process_is_group_reference(process p) wontthrow -> bool
{
  return (reinterpret_cast<uintptr_t>(p) & PROCESS_REFERENCE_MASK) ==
         PROCESS_GROUP_REFERENCE_TAG;
}

static pure fn pid_from_reference(process p) wontthrow -> DWORD
{
  return static_cast<DWORD>(reinterpret_cast<uintptr_t>(p) >> 2u);
}

static pure fn process_from_group_reference(process p) wontthrow -> process
{
  return reinterpret_cast<process>(reinterpret_cast<uintptr>(p) &
                                   ~PROCESS_REFERENCE_MASK);
}

fn is_child_process() wontthrow -> bool
{
  return GetCurrentProcessId() != PARENT_SHELL_PID;
}

/* Windows has no setuid or setgid notion. */
fn is_running_setuid() wontthrow -> bool { return false; }
fn drop_elevated_identity() wontthrow -> bool { return true; }

fn process_id_of(process p) wontthrow -> i64
{
  if (process_is_pid_reference(p)) return pid_from_reference(p);
  if (process_is_group_reference(p)) p = process_from_group_reference(p);
  return static_cast<i64>(GetProcessId(p));
}

fn process_group_of(process p) throws -> process
{
  HANDLE duplicate = INVALID_HANDLE_VALUE;
  if (DuplicateHandle(GetCurrentProcess(), p, GetCurrentProcess(), &duplicate,
                      0, FALSE, DUPLICATE_SAME_ACCESS) == 0)
  {
    let message = last_system_error_message();
    let const group = reinterpret_cast<process>(reinterpret_cast<uintptr>(p) |
                                                PROCESS_GROUP_REFERENCE_TAG);
    signal_process(group, 9);
    throw Error{"Could not retain the timeout process group: " + message};
  }

  return reinterpret_cast<process>(reinterpret_cast<uintptr>(duplicate) |
                                   PROCESS_GROUP_REFERENCE_TAG);
}

fn close_process_group(process group) wontthrow -> void
{
  if (group == nullptr || !process_is_group_reference(group)) return;
  CloseHandle(process_from_group_reference(group));
}
fn process_has_id(process p, i64 id) wontthrow -> bool
{
  return process_id_of(p) == id;
}

fn is_stdin_a_tty() wontthrow -> bool { return is_fd_a_tty(KOSH_STDIN); }

fn is_stdout_a_tty() wontthrow -> bool { return is_fd_a_tty(KOSH_STDOUT); }

fn is_stderr_a_tty() wontthrow -> bool { return is_fd_a_tty(KOSH_STDERR); }

fn is_fd_a_tty(descriptor fd) wontthrow -> bool
{
  DWORD console_mode = 0;
  return GetConsoleMode(fd, &console_mode) != FALSE;
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

pure fn path_is_absolute(StringView path) wontthrow -> bool
{
  if (path.length == 0) return false;
  if (is_directory_separator(path.data[0])) return true;
  return path.length >= 3 && path.data[1] == ':' &&
         is_directory_separator(path.data[2]);
}

pure fn path_is_drive_relative(StringView path) wontthrow -> bool
{
  return path.length >= 2 && path[1] == ':' &&
         (path.length == 2 || !is_directory_separator(path[2]));
}

fn resolve_drive_relative_path(StringView path) throws -> Maybe<Path>
{
  if (!path_is_drive_relative(path)) return None;

  char drive_current_directory[4]{path[0], ':', '.', '\0'};
  let const required =
      GetFullPathNameA(drive_current_directory, 0, nullptr, nullptr);
  if (required == 0) return None;
  let buffer = ArrayList<char>{heap_allocator()};
  buffer.reserve(static_cast<usize>(required));
  let const length = GetFullPathNameA(drive_current_directory, required,
                                      buffer.begin(), nullptr);
  if (length == 0 || length >= required) return None;
  let result = Path{
      StringView{buffer.begin(), static_cast<usize>(length)}
  };
  if (path.length > 2) result.push_component(path.substring(2));
  return result;
}

pure fn path_root_length(StringView path) wontthrow -> usize
{
  if (path.length >= 7 && is_directory_separator(path[0]) &&
      is_directory_separator(path[1]) && path[2] == '?' &&
      is_directory_separator(path[3]) && path[5] == ':' &&
      is_directory_separator(path[6]))
  {
    return 7;
  }
  if (path.length >= 8 && is_directory_separator(path[0]) &&
      is_directory_separator(path[1]) && path[2] == '?' &&
      is_directory_separator(path[3]) &&
      utils::ascii_to_lower(path[4]) == 'u' &&
      utils::ascii_to_lower(path[5]) == 'n' &&
      utils::ascii_to_lower(path[6]) == 'c' && is_directory_separator(path[7]))
  {
    usize position = 8;
    while (position < path.length && !is_directory_separator(path[position]))
      position++;
    while (position < path.length && is_directory_separator(path[position]))
      position++;
    while (position < path.length && !is_directory_separator(path[position]))
      position++;
    if (position < path.length && is_directory_separator(path[position])) {
      position++;
    }
    return position;
  }
  if (path.length >= 2 && is_directory_separator(path[0]) &&
      is_directory_separator(path[1]))
  {
    usize position = 2;
    while (position < path.length && !is_directory_separator(path[position]))
      position++;
    while (position < path.length && is_directory_separator(path[position]))
      position++;
    while (position < path.length && !is_directory_separator(path[position]))
      position++;
    if (position < path.length && is_directory_separator(path[position])) {
      position++;
    }
    return position;
  }
  if (path.length >= 3 && path[1] == ':' && is_directory_separator(path[2])) {
    return 3;
  }
  if (path.length >= 1 && is_directory_separator(path[0])) return 1;
  return 0;
}

fn temp_directory_path() throws -> String
{
  if (let from_environment = get_environment_variable("TEMP");
      from_environment.has_value())
    return from_environment.take();
  return String{"C:\\Windows\\Temp"};
}

cold fn path_exists(StringView path) wontthrow -> bool
{
  if (path == StringView{"/dev/null"}) return true;
  const String path_string{path};
  return GetFileAttributesA(path_string.c_str()) != INVALID_FILE_ATTRIBUTES;
}

cold fn path_is_directory(StringView path) wontthrow -> bool
{
  const String path_string{path};
  let const attributes = GetFileAttributesA(path_string.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES &&
         (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

fn path_is_regular_file(StringView path) wontthrow -> bool
{
  const String path_string{path};
  let const attributes = GetFileAttributesA(path_string.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES &&
         (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

fn path_is_symbolic_link(StringView path) wontthrow -> bool
{
  const String path_string{path};
  let const attributes = GetFileAttributesA(path_string.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES &&
         (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

/* Windows has no POSIX block, character, FIFO, or socket file type. */
fn path_is_block_device(StringView path) wontthrow -> bool
{
  unused(path);
  return false;
}
fn path_is_character_device(StringView path) wontthrow -> bool
{
  return path == StringView{"/dev/null"};
}
fn path_is_fifo(StringView path) wontthrow -> bool
{
  unused(path);
  return false;
}
fn path_is_socket(StringView path) wontthrow -> bool
{
  unused(path);
  return false;
}

/* Windows carries no setuid, setgid, sticky, or POSIX ownership bit. */
fn path_has_setuid_bit(StringView path) wontthrow -> bool
{
  unused(path);
  return false;
}
fn path_has_setgid_bit(StringView path) wontthrow -> bool
{
  unused(path);
  return false;
}
fn path_has_sticky_bit(StringView path) wontthrow -> bool
{
  unused(path);
  return false;
}
fn path_is_owned_by_effective_user(StringView path) wontthrow -> bool
{
  unused(path);
  return false;
}
fn path_is_owned_by_effective_group(StringView path) wontthrow -> bool
{
  unused(path);
  return false;
}

fn path_file_size(StringView path) wontthrow -> Maybe<u64>
{
  const String path_string{path};
  WIN32_FILE_ATTRIBUTE_DATA data{};
  if (GetFileAttributesExA(path_string.c_str(), GetFileExInfoStandard, &data) ==
      0)
    return None;
  if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) return None;
  return (static_cast<u64>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
}

fn path_modification_time(StringView path) wontthrow -> Maybe<i64>
{
  const String path_string{path};
  WIN32_FILE_ATTRIBUTE_DATA data{};
  if (GetFileAttributesExA(path_string.c_str(), GetFileExInfoStandard, &data) ==
      0)
    return None;
  let const windows_ticks =
      (static_cast<u64>(data.ftLastWriteTime.dwHighDateTime) << 32) |
      data.ftLastWriteTime.dwLowDateTime;
  constexpr u64 WINDOWS_TO_UNIX_EPOCH_TICKS = 116444736000000000ULL;
  if (windows_ticks < WINDOWS_TO_UNIX_EPOCH_TICKS) return i64{0};
  return static_cast<i64>((windows_ticks - WINDOWS_TO_UNIX_EPOCH_TICKS) /
                          10000000ULL);
}

fn paths_are_same_file(StringView first, StringView second) wontthrow -> bool
{
  const String first_string{first};
  const String second_string{second};
  /* FILE_FLAG_BACKUP_SEMANTICS lets a directory open too. */
  let const first_handle =
      CreateFileA(first_string.c_str(), 0,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                  nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (first_handle == INVALID_HANDLE_VALUE) return false;
  let const second_handle =
      CreateFileA(second_string.c_str(), 0,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                  nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (second_handle == INVALID_HANDLE_VALUE) {
    CloseHandle(first_handle);
    return false;
  }
  BY_HANDLE_FILE_INFORMATION first_info{}, second_info{};
  let const both_read =
      GetFileInformationByHandle(first_handle, &first_info) != 0 &&
      GetFileInformationByHandle(second_handle, &second_info) != 0;
  CloseHandle(first_handle);
  CloseHandle(second_handle);
  if (!both_read) return false;
  return first_info.dwVolumeSerialNumber == second_info.dwVolumeSerialNumber &&
         first_info.nFileIndexHigh == second_info.nFileIndexHigh &&
         first_info.nFileIndexLow == second_info.nFileIndexLow;
}

fn path_is_newer_than(StringView first, StringView second) wontthrow -> bool
{
  const String first_string{first};
  const String second_string{second};
  WIN32_FILE_ATTRIBUTE_DATA first_data{}, second_data{};
  if (GetFileAttributesExA(first_string.c_str(), GetFileExInfoStandard,
                           &first_data) == 0)
    return false;
  if (GetFileAttributesExA(second_string.c_str(), GetFileExInfoStandard,
                           &second_data) == 0)
    return false;
  return CompareFileTime(&first_data.ftLastWriteTime,
                         &second_data.ftLastWriteTime) > 0;
}

fn path_is_older_than(StringView first, StringView second) wontthrow -> bool
{
  return path_is_newer_than(second, first);
}

fn path_is_readable(StringView path) wontthrow -> bool
{
  const String path_string{path};
  return _access(path_string.c_str(), 4) == 0;
}

fn path_is_writable(StringView path) wontthrow -> bool
{
  const String path_string{path};
  return _access(path_string.c_str(), 2) == 0;
}

fn path_is_executable(StringView path) wontthrow -> bool
{
  /* Windows has no execute permission bit, so an existing file is runnable. */
  return path_is_regular_file(path);
}

cold fn read_current_directory() throws -> Path
{
  char buffer[4096];
  if (_getcwd(buffer, sizeof(buffer)) != nullptr)
    return Path{StringView{buffer}};
  return Path{};
}

fn change_current_directory(StringView path) throws -> ErrorOr<Ok>
{
  const String path_string{path};
  if (_chdir(path_string.c_str()) != 0)
    return Error{"Could not change directory to '" + path_string +
                 "': " + os::last_system_error_message()};
  return Success;
}

fn reference_current_directory() wontthrow -> DirectoryReference
{
  return DirectoryReference{CreateFileA(
      ".", 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr)};
}

fn restore_current_directory(const DirectoryReference &reference) wontthrow
    -> bool
{
  if (!reference.is_valid()) return false;

  char path[32768];
  let const length = GetFinalPathNameByHandleA(
      reference.get(), path, static_cast<DWORD>(countof(path)),
      FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  if (length == 0 || length >= countof(path)) return false;
  let restored_path = StringView{path, static_cast<usize>(length)};
  if (restored_path.starts_with(StringView{"\\\\?\\UNC\\"})) {
    let unc_path = String{"\\\\"};
    unc_path += restored_path.substring(8);
    return SetCurrentDirectoryA(unc_path.c_str()) != 0;
  }
  if (restored_path.starts_with(StringView{"\\\\?\\"}))
    restored_path = restored_path.substring(4);
  let const restored_path_string = String{restored_path};
  return SetCurrentDirectoryA(restored_path_string.c_str()) != 0;
}

cold fn list_directory(StringView dir) throws -> Maybe<ArrayList<String>>
{
  const String dir_string{dir};
  let entries = list_directory_typed(dir);
  if (!entries.has_value()) return None;
  let names = ArrayList<String>{heap_allocator()};
  names.reserve(entries->count());
  for (let &entry : *entries)
    names.push(steal(entry.name));
  LOG(All, "read %zu entries from the directory '%s'", names.count(),
      dir_string.c_str());
  return names;
}

cold fn list_directory_typed(StringView dir) throws
    -> Maybe<ArrayList<Path::directory_child>>
{
  const String dir_string{dir};
  let pattern = dir_string.clone();
  pattern.push(DIRECTORY_SEPARATOR);
  pattern.push('*');

  WIN32_FIND_DATAA data{};
  let const handle = FindFirstFileA(pattern.c_str(), &data);
  if (handle == INVALID_HANDLE_VALUE) return None;
  defer { FindClose(handle); };

  let entries = ArrayList<Path::directory_child>{heap_allocator()};
  do {
    let const name = StringView{data.cFileName};
    if (name == StringView{"."} || name == StringView{".."}) continue;

    Path::entry_kind kind = Path::entry_kind::Regular;
    if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
      kind = Path::entry_kind::Symlink;
    else if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
      kind = Path::entry_kind::Directory;
    entries.push(Path::directory_child{String{name}, kind});
  } while (FindNextFileA(handle, &data) != 0);
  return entries;
}

fn read_process_cpu_times() wontthrow -> cpu_times { return cpu_times{}; }

fn get_resource_limit(resource_kind kind, resource_limit &out) wontthrow -> bool
{
  unused(kind);
  out.soft = RESOURCE_UNLIMITED;
  out.hard = RESOURCE_UNLIMITED;
  return true;
}

fn set_resource_limit(resource_kind kind, const resource_limit &limit) wontthrow
    -> bool
{
  unused(kind);
  unused(limit);
  return true;
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

struct inherited_handle_state
{
  HANDLE handle{INVALID_HANDLE_VALUE};
  DWORD original_flags{0};
  bool should_restore{false};
};

static fn make_handle_inheritable(HANDLE handle,
                                  inherited_handle_state &state) wontthrow
    -> void
{
  if (handle == nullptr || handle == INVALID_HANDLE_VALUE) return;

  DWORD flags = 0;
  if (GetHandleInformation(handle, &flags) == FALSE) return;
  if ((flags & HANDLE_FLAG_INHERIT) != 0) return;
  if (SetHandleInformation(handle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT) ==
      FALSE)
    return;

  state.handle = handle;
  state.original_flags = flags;
  state.should_restore = true;
}

static fn
restore_handle_inheritance(const inherited_handle_state &state) wontthrow
    -> void
{
  if (!state.should_restore) return;
  SetHandleInformation(state.handle, HANDLE_FLAG_INHERIT,
                       state.original_flags & HANDLE_FLAG_INHERIT);
}

static fn timeout_job_name(HANDLE process_handle, char (&name)[64]) wontthrow
    -> bool
{
  FILETIME creation_time;
  FILETIME exit_time;
  FILETIME kernel_time;
  FILETIME user_time;
  if (GetProcessTimes(process_handle, &creation_time, &exit_time, &kernel_time,
                      &user_time) == FALSE)
    return false;

  let const creation_ticks =
      (static_cast<u64>(creation_time.dwHighDateTime) << 32u) |
      creation_time.dwLowDateTime;
  ::snprintf(name, sizeof(name), "kosh-timeout-%lu-%llu",
             static_cast<unsigned long>(GetProcessId(process_handle)),
             static_cast<unsigned long long>(creation_ticks));
  return true;
}

enum class timeout_job_lifetime : u8
{
  DescendantOwned,
  LeaderOwned,
};

static fn attach_timeout_job(const PROCESS_INFORMATION &process_info,
                             timeout_job_lifetime lifetime) throws -> void
{
  char job_name[64];
  if (!timeout_job_name(process_info.hProcess, job_name))
    throw Error{last_system_error_message()};
  let const job = CreateJobObjectA(nullptr, job_name);
  if (job == nullptr) throw Error{last_system_error_message()};
  defer { CloseHandle(job); };
  if (GetLastError() == ERROR_ALREADY_EXISTS)
    throw Error{"timeout job already exists"};

  if (lifetime == timeout_job_lifetime::LeaderOwned) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION job_limits{};
    job_limits.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                &job_limits, sizeof(job_limits)) == FALSE)
      throw Error{last_system_error_message()};
  }

  if (AssignProcessToJobObject(job, process_info.hProcess) == FALSE)
    throw Error{last_system_error_message()};

  HANDLE child_job_handle = nullptr;
  if (DuplicateHandle(GetCurrentProcess(), job, process_info.hProcess,
                      &child_job_handle, 0,
                      lifetime == timeout_job_lifetime::DescendantOwned,
                      DUPLICATE_SAME_ACCESS) == FALSE)
    throw Error{last_system_error_message()};
}

static fn append_windows_quoted_arg(String &out, StringView arg) throws -> void;

static pure fn is_batch_program(StringView path) wontthrow -> bool
{
  if (path.length < 4) return false;
  let const suffix = path.substring_of_length(path.length - 4, 4);
  return suffix[0] == '.' && utils::ascii_to_lower(suffix[1]) == 'b' &&
         utils::ascii_to_lower(suffix[2]) == 'a' &&
         utils::ascii_to_lower(suffix[3]) == 't';
}

fn execute_program(ExecContext &ec, script_fallback_policy fallback,
                   process_group_mode process_group, StringView,
                   terminal_handoff handoff, i64 process_group_id) -> process
{
  let const allow_script_fallback = fallback == script_fallback_policy::Allow;
  unused(process_group_id);
  let const should_create_new_process_group =
      process_group == process_group_mode::New ||
      process_group == process_group_mode::NewBackground ||
      process_group == process_group_mode::NewLeaderOwned;
  let const should_attach_timeout_job =
      should_create_new_process_group &&
      process_group != process_group_mode::NewBackground;
  let const job_lifetime = process_group == process_group_mode::NewLeaderOwned
                               ? timeout_job_lifetime::LeaderOwned
                               : timeout_job_lifetime::DescendantOwned;
  let const should_hand_off_controlling_terminal_before_start =
      handoff == terminal_handoff::BeforeStart;
  let const should_start_suspended =
      should_attach_timeout_job ||
      should_hand_off_controlling_terminal_before_start;
  LOG(Debug, "spawning '%s' with %zu arguments", ec.program_path().c_str(),
      ec.args().count());

  let application_path = ec.program_path().c_str();
  String resolved_program_path_storage{heap_allocator()};
  if (let resolved_program_path = canonical_path(ec.program_path())) {
    resolved_program_path_storage = resolved_program_path->text().clone();
    application_path = resolved_program_path_storage.c_str();
  }

  String application_path_storage{heap_allocator()};
  String working_directory_storage{heap_allocator()};
  const char *working_directory = nullptr;
  String command_line = make_os_args(ec.args());
  if (is_batch_program(ec.program_path().text().view())) {
    let batch_command = String{heap_allocator()};
    append_windows_quoted_arg(batch_command,
                              resolved_program_path_storage.is_empty()
                                  ? ec.program_path().text().view()
                                  : resolved_program_path_storage.view());
    for (usize argument_index = 1; argument_index < ec.args().count();
         argument_index++)
    {
      batch_command += ' ';
      append_windows_quoted_arg(batch_command,
                                ec.args()[argument_index].view());
    }

    let const command_processor = std::getenv("COMSPEC");
    application_path_storage =
        String{command_processor != nullptr ? command_processor : "cmd.exe"};
    application_path = application_path_storage.c_str();
    let processor_command_line = String{heap_allocator()};
    append_windows_quoted_arg(processor_command_line,
                              application_path_storage.view());
    processor_command_line += " /d /s /c \"";
    processor_command_line += batch_command;
    processor_command_line += '"';
    command_line = steal(processor_command_line);

    let const current_directory = read_current_directory();
    let current_directory_text = current_directory.text().view();
    if (current_directory_text.length >= 7 &&
        current_directory_text.starts_with(StringView{"\\\\?\\"}) &&
        current_directory_text[5] == ':' &&
        is_directory_separator(current_directory_text[6]))
    {
      current_directory_text = current_directory_text.substring(4);
      working_directory_storage.append(current_directory_text);
      working_directory = working_directory_storage.c_str();
    }
  }

  PROCESS_INFORMATION process_info{};
  STARTUPINFOA startup_info{};

  startup_info.cb = sizeof(startup_info);
  startup_info.hStdInput = ec.in_fd.value_or(GetStdHandle(STD_INPUT_HANDLE));
  startup_info.hStdOutput = ec.out_fd.value_or(GetStdHandle(STD_OUTPUT_HANDLE));
  startup_info.hStdError = ec.err_fd.value_or(GetStdHandle(STD_ERROR_HANDLE));

  /* Each dup reads the current target of its source, so the source order
     decides a mixed 2>&1 1>&2. */
  ec.apply_dup_routing(
      [&]() { startup_info.hStdError = startup_info.hStdOutput; },
      [&]() { startup_info.hStdOutput = startup_info.hStdError; });

  bool were_handles_handed_to_fallback = false;
  defer
  {
    if (!were_handles_handed_to_fallback) ec.close_fds();
  };

  inherited_handle_state input_inheritance{};
  inherited_handle_state output_inheritance{};
  inherited_handle_state error_inheritance{};
  let const has_command_redirection =
      ec.in_fd.has_value() || ec.out_fd.has_value() || ec.err_fd.has_value() ||
      ec.should_duplicate_error_to_output ||
      ec.should_duplicate_output_to_error;
  let const should_use_standard_handles =
      has_command_redirection || !is_fd_a_tty(startup_info.hStdInput) ||
      !is_fd_a_tty(startup_info.hStdOutput) ||
      !is_fd_a_tty(startup_info.hStdError);

  HANDLE null_handle = INVALID_HANDLE_VALUE;
  defer
  {
    if (null_handle != INVALID_HANDLE_VALUE) CloseHandle(null_handle);
  };
  if (should_use_standard_handles) {
    if (startup_info.hStdInput == nullptr ||
        startup_info.hStdInput == INVALID_HANDLE_VALUE ||
        startup_info.hStdOutput == nullptr ||
        startup_info.hStdOutput == INVALID_HANDLE_VALUE ||
        startup_info.hStdError == nullptr ||
        startup_info.hStdError == INVALID_HANDLE_VALUE)
    {
      SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr,
                                      TRUE};
      null_handle = CreateFileA("NUL", GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE,
                                &inheritable, OPEN_EXISTING, 0, nullptr);
      if (null_handle == INVALID_HANDLE_VALUE)
        throw ErrorWithLocation{ec.source_location(),
                                last_system_error_message()};
      if (startup_info.hStdInput == nullptr ||
          startup_info.hStdInput == INVALID_HANDLE_VALUE)
        startup_info.hStdInput = null_handle;
      if (startup_info.hStdOutput == nullptr ||
          startup_info.hStdOutput == INVALID_HANDLE_VALUE)
        startup_info.hStdOutput = null_handle;
      if (startup_info.hStdError == nullptr ||
          startup_info.hStdError == INVALID_HANDLE_VALUE)
        startup_info.hStdError = null_handle;
    }

    startup_info.dwFlags = STARTF_USESTDHANDLES;
    make_handle_inheritable(startup_info.hStdInput, input_inheritance);
    make_handle_inheritable(startup_info.hStdOutput, output_inheritance);
    make_handle_inheritable(startup_info.hStdError, error_inheritance);
  }
  defer { restore_handle_inheritance(input_inheritance); };
  defer { restore_handle_inheritance(output_inheritance); };
  defer { restore_handle_inheritance(error_inheritance); };

  /* An empty CreateProcess environment block is two nulls, a null pointer would
     inherit the shell's environment. */
  char empty_environment_block[] = {'\0', '\0'};
  LPVOID environment_block =
      ec.should_use_empty_environment ? empty_environment_block : nullptr;

  DWORD creation_flags =
      should_create_new_process_group ? CREATE_NEW_PROCESS_GROUP : 0;
  if (should_start_suspended) creation_flags |= CREATE_SUSPENDED;

  /* CreateProcessA may rewrite lpCommandLine in place, so it is passed mutable.
   */
  if (CreateProcessA(application_path, const_cast<LPSTR>(command_line.data()),
                     nullptr, nullptr, should_use_standard_handles,
                     creation_flags, environment_block, working_directory,
                     &startup_info, &process_info) == 0)
  {
    if (allow_script_fallback && GetLastError() == ERROR_BAD_EXE_FORMAT) {
      if (!resolved_program_path_storage.is_empty())
        ec.set_program_path(Path{resolved_program_path_storage.view()});
      were_handles_handed_to_fallback = true;
      return KOSH_INVALID_PROCESS;
    }
    throw ErrorWithLocation{ec.source_location(), last_system_error_message()};
  }

  if (should_attach_timeout_job) {
    try {
      attach_timeout_job(process_info, job_lifetime);
    } catch (const ErrorBase &error) {
      TerminateProcess(process_info.hProcess, 1);
      CloseHandle(process_info.hProcess);
      CloseHandle(process_info.hThread);
      relocate_error(error, ec.source_location());
    } catch (...) {
      TerminateProcess(process_info.hProcess, 1);
      CloseHandle(process_info.hProcess);
      CloseHandle(process_info.hThread);
      throw;
    }
  }
  if (should_hand_off_controlling_terminal_before_start)
    give_controlling_terminal_to(process_info.hProcess);
  if (should_start_suspended) {
    if (ResumeThread(process_info.hThread) == static_cast<DWORD>(-1)) {
      let const message = last_system_error_message();
      TerminateProcess(process_info.hProcess, 1);
      CloseHandle(process_info.hProcess);
      CloseHandle(process_info.hThread);
      throw ErrorWithLocation{ec.source_location(), steal(message)};
    }
  }

  CloseHandle(process_info.hThread);
  return process_info.hProcess;
}

struct process_substitution_temp_result
{
  String path{heap_allocator()};
  String diagnostic_output{heap_allocator()};
  bool has_shell_diagnostic{false};
};

static fn run_substitution_to_temp(StringView source, bool bash_compatible,
                                   bool source_traces_enabled) throws
    -> Maybe<process_substitution_temp_result>
{
  /* Windows has no fork, so <(cmd) spawns a fresh shell that writes its output
     into a temp file the consumer reads by path. The whole output is written
     before the path returns. */
  let const module_path = current_executable_path();
  if (!module_path.has_value()) return koshka::None;

  char temp_dir[MAX_PATH];
  let const temp_directory_length = GetTempPathA(MAX_PATH, temp_dir);
  if (temp_directory_length == 0 || temp_directory_length >= MAX_PATH) {
    return koshka::None;
  }
  char temp_path[MAX_PATH];
  if (GetTempFileNameA(temp_dir, "kos", 0, temp_path) == 0) return koshka::None;
  bool should_delete_temp_path = true;
  defer
  {
    if (should_delete_temp_path) DeleteFileA(temp_path);
  };

  SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
  const HANDLE temp_file =
      CreateFileA(temp_path, GENERIC_WRITE, FILE_SHARE_READ, &inheritable,
                  CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
  if (temp_file == INVALID_HANDLE_VALUE) return koshka::None;
  defer { CloseHandle(temp_file); };

  char diagnostic_path[MAX_PATH];
  if (GetTempFileNameA(temp_dir, "kos", 0, diagnostic_path) == 0)
    return koshka::None;
  defer { DeleteFileA(diagnostic_path); };
  const HANDLE diagnostic_file =
      CreateFileA(diagnostic_path, GENERIC_WRITE, FILE_SHARE_READ, &inheritable,
                  CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
  if (diagnostic_file == INVALID_HANDLE_VALUE) return koshka::None;
  bool is_diagnostic_file_open = true;
  defer
  {
    if (is_diagnostic_file_open) CloseHandle(diagnostic_file);
  };

  char diagnostic_marker_path[MAX_PATH];
  if (GetTempFileNameA(temp_dir, "kos", 0, diagnostic_marker_path) == 0)
    return koshka::None;
  defer { DeleteFileA(diagnostic_marker_path); };

  let arguments = ArrayList<String>{heap_allocator()};
  arguments.push(String{heap_allocator(), module_path->view()});
  if (bash_compatible) {
    arguments.push(String{heap_allocator(), StringView{"--mood"}});
    arguments.push(String{heap_allocator(), StringView{"bash"}});
  }
  arguments.push(String{heap_allocator(), StringView{"--no-diagnostics"}});
  if (!source_traces_enabled)
    arguments.push(String{heap_allocator(), StringView{"--no-traces"}});
  arguments.push(String{heap_allocator(), StringView{"-c"}});
  arguments.push(String{heap_allocator(), source});
  let command_line = make_os_args(arguments);

  STARTUPINFOA startup_info{};
  startup_info.cb = sizeof(startup_info);
  startup_info.dwFlags = STARTF_USESTDHANDLES;
  startup_info.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  startup_info.hStdOutput = temp_file;
  startup_info.hStdError = diagnostic_file;
  inherited_handle_state input_inheritance{};
  make_handle_inheritable(startup_info.hStdInput, input_inheritance);
  defer { restore_handle_inheritance(input_inheritance); };

  PROCESS_INFORMATION process_info{};
  let const previous_root_trace_marker =
      get_environment_variable("KOSH_INTERNAL_SUPPRESS_ROOT_TRACE");
  let const previous_diagnostic_marker =
      get_environment_variable("KOSH_INTERNAL_DIAGNOSTIC_MARKER");
  set_environment_variable("KOSH_INTERNAL_SUPPRESS_ROOT_TRACE", "1");
  set_environment_variable("KOSH_INTERNAL_DIAGNOSTIC_MARKER",
                           diagnostic_marker_path);
  defer
  {
    if (previous_root_trace_marker.has_value())
      set_environment_variable("KOSH_INTERNAL_SUPPRESS_ROOT_TRACE",
                               previous_root_trace_marker->view());
    else
      unset_environment_variable("KOSH_INTERNAL_SUPPRESS_ROOT_TRACE");
    if (previous_diagnostic_marker.has_value())
      set_environment_variable("KOSH_INTERNAL_DIAGNOSTIC_MARKER",
                               previous_diagnostic_marker->view());
    else
      unset_environment_variable("KOSH_INTERNAL_DIAGNOSTIC_MARKER");
  };
  if (CreateProcessA(module_path->c_str(),
                     const_cast<LPSTR>(command_line.data()), nullptr, nullptr,
                     TRUE, 0, nullptr, nullptr, &startup_info,
                     &process_info) == 0)
    return koshka::None;
  defer { CloseHandle(process_info.hProcess); };
  defer { CloseHandle(process_info.hThread); };
  WaitForSingleObject(process_info.hProcess, INFINITE);

  CloseHandle(diagnostic_file);
  is_diagnostic_file_open = false;
  let diagnostic_output = Path{diagnostic_path}.read_entire_file();

  /* A backslash would read as an escape in the target word, so slashes are
     returned, which CreateFile accepts the same. */
  let result = process_substitution_temp_result{};
  for (const char *byte = temp_path; *byte != '\0'; byte++)
    result.path += *byte == '\\' ? '/' : *byte;
  if (diagnostic_output.has_value())
    result.diagnostic_output = diagnostic_output.take();
  let const marker_size = Path{diagnostic_marker_path}.file_size();
  result.has_shell_diagnostic = marker_size.has_value() && *marker_size != 0;
  should_delete_temp_path = false;
  return result;
}

fn launch_process_substitution(StringView source, bool command_writes_pipe,
                               bool bash_compatible,
                               bool source_traces_enabled) throws
    -> process_substitution_launch
{
  if (!command_writes_pipe)
    throw Error{"Unable to run a >(cmd) process substitution because it is not "
                "supported on this platform"};

  let result =
      run_substitution_to_temp(source, bash_compatible, source_traces_enabled);
  if (!result.has_value())
    throw Error{"Unable to run the process substitution because the inner "
                "shell could not be spawned: " +
                last_system_error_message()};

  return process_substitution_launch{
      .path = steal(result->path),
      .diagnostic_output = steal(result->diagnostic_output),
      .is_temporary_file = true,
      .has_shell_diagnostic = result->has_shell_diagnostic,
  };
}

static fn spawn_subshell_stage(StringView source, Maybe<descriptor> in_fd,
                               Maybe<descriptor> out_fd,
                               Maybe<descriptor> err_fd, mimic_mood mood,
                               process_group_mode process_group) throws
    -> Maybe<process>
{
  /* Windows has no fork, so a compound pipeline stage re-parses its source in a
     fresh shell, returned unwaited for the pipeline to reap. */
  let const module_path = current_executable_path();
  if (!module_path.has_value()) return koshka::None;

  let arguments = ArrayList<String>{heap_allocator()};
  arguments.push(String{heap_allocator(), module_path->view()});
  arguments.push(String{heap_allocator(), StringView{"--privileged"}});
  if (mood != mimic_mood::Default) {
    arguments.push(String{heap_allocator(), StringView{"--mood"}});
    arguments.push(String{heap_allocator(), mood_name(mood)});
  }
  arguments.push(String{heap_allocator(), StringView{"--no-diagnostics"}});
  arguments.push(String{heap_allocator(), StringView{"-c"}});
  arguments.push(String{heap_allocator(), source});
  let command_line = make_os_args(arguments);

  STARTUPINFOA startup_info{};
  startup_info.cb = sizeof(startup_info);
  startup_info.dwFlags = STARTF_USESTDHANDLES;
  startup_info.hStdInput = in_fd ? *in_fd : GetStdHandle(STD_INPUT_HANDLE);
  startup_info.hStdOutput = out_fd ? *out_fd : GetStdHandle(STD_OUTPUT_HANDLE);
  startup_info.hStdError = err_fd ? *err_fd : GetStdHandle(STD_ERROR_HANDLE);
  inherited_handle_state input_inheritance{};
  inherited_handle_state output_inheritance{};
  inherited_handle_state error_inheritance{};
  make_handle_inheritable(startup_info.hStdInput, input_inheritance);
  make_handle_inheritable(startup_info.hStdOutput, output_inheritance);
  make_handle_inheritable(startup_info.hStdError, error_inheritance);
  defer { restore_handle_inheritance(input_inheritance); };
  defer { restore_handle_inheritance(output_inheritance); };
  defer { restore_handle_inheritance(error_inheritance); };

  PROCESS_INFORMATION process_info{};
  let const creation_flags = process_group == process_group_mode::Inherit
                                 ? static_cast<DWORD>(0)
                                 : CREATE_NEW_PROCESS_GROUP;
  if (CreateProcessA(module_path->c_str(),
                     const_cast<LPSTR>(command_line.data()), nullptr, nullptr,
                     TRUE, creation_flags, nullptr, nullptr, &startup_info,
                     &process_info) == 0)
    return koshka::None;
  CloseHandle(process_info.hThread);
  return process_info.hProcess;
}

fn try_fork_compound_stage(Maybe<descriptor> in_fd, Maybe<descriptor> out_fd,
                           Maybe<descriptor> err_fd, SourceLocation location,
                           StringView source, process_group_mode process_group,
                           i64 process_group_id) -> Maybe<process>
{
  unused(in_fd);
  unused(out_fd);
  unused(err_fd);
  unused(location);
  unused(source);
  unused(process_group);
  unused(process_group_id);
  return koshka::None;
}

fn try_fork_job_process() -> Maybe<process> { return koshka::None; }

fn can_fork_evaluator() wontthrow -> bool { return false; }

fn launch_compound_stage(StringView source, Maybe<descriptor> in_fd,
                         Maybe<descriptor> out_fd, Maybe<descriptor> err_fd,
                         mimic_mood mood, SourceLocation location,
                         StringView diagnostic_source,
                         process_group_mode process_group,
                         i64 process_group_id) throws -> compound_stage_launch
{
  unused(diagnostic_source);
  if (source.is_empty())
    throw ErrorWithLocation{
        steal(location),
        "A compound command in a pipeline is not supported on this platform"};

  unused(process_group_id);
  let child =
      spawn_subshell_stage(source, in_fd, out_fd, err_fd, mood, process_group);
  if (!child.has_value())
    throw ErrorWithLocation{steal(location),
                            "Could not spawn the compound pipeline stage"};

  return compound_stage_launch{
      .child = *child,
      .should_evaluate_child = false,
  };
}

[[noreturn]] fn exit_process_immediately(i32 status) wontthrow -> void
{
  ExitProcess(static_cast<UINT>(status));
  unreachable("ExitProcess returned while exiting immediately");
}

fn replace_process(ExecContext &&ec) -> void
{
  /* Windows cannot exec in place, so the program runs to completion and the
     shell exits with its status. */
  LOG(Debug, "running '%s' to completion in place of an exec",
      ec.program_path().c_str());
  process child = execute_program(ec, script_fallback_policy::Allow);
  if (child == KOSH_INVALID_PROCESS) {
    redirect_self(ec);
    ec.close_fds();
    return;
  }

  i32 status = wait_and_monitor_process(child);
  ExitProcess(static_cast<UINT>(status));
  unreachable("ExitProcess returned after process replacement");
}

fn redirect_self(const ExecContext &ec) -> void
{
  /* Duplicate each redirect handle into the standard slot, so the caller's
     close of the original handles leaves the shell's new standard handles
     valid for the rest of the session. */
  HANDLE self = GetCurrentProcess();
  let const do_replace_standard_handle = [&](DWORD standard_handle,
                                             HANDLE source_handle) {
    HANDLE duplicate_handle = nullptr;
    if (DuplicateHandle(self, source_handle, self, &duplicate_handle, 0, TRUE,
                        DUPLICATE_SAME_ACCESS) == 0)
      return;
    if (SetStdHandle(standard_handle, duplicate_handle) == FALSE)
      CloseHandle(duplicate_handle);
  };

  if (ec.in_fd) do_replace_standard_handle(STD_INPUT_HANDLE, *ec.in_fd);
  if (ec.out_fd) do_replace_standard_handle(STD_OUTPUT_HANDLE, *ec.out_fd);
  if (ec.err_fd) do_replace_standard_handle(STD_ERROR_HANDLE, *ec.err_fd);
  ec.apply_dup_routing(
      [&]() {
        do_replace_standard_handle(STD_ERROR_HANDLE,
                                   GetStdHandle(STD_OUTPUT_HANDLE));
      },
      [&]() {
        do_replace_standard_handle(STD_OUTPUT_HANDLE,
                                   GetStdHandle(STD_ERROR_HANDLE));
      });
}

fn make_pipe() wontthrow -> Maybe<Pipe>
{
  SECURITY_ATTRIBUTES attributes{};

  attributes.nLength = sizeof(SECURITY_ATTRIBUTES);
  /* Both ends non-inheritable, the child receives only what
     STARTF_USESTDHANDLES names. */
  attributes.bInheritHandle = FALSE;
  attributes.lpSecurityDescriptor = nullptr; /* NOLINT */

  HANDLE in = INVALID_HANDLE_VALUE;
  HANDLE out = INVALID_HANDLE_VALUE;

  if (CreatePipe(&in, &out, &attributes, 0) == 0) {
    if (in != INVALID_HANDLE_VALUE) close_fd(in);
    if (out != INVALID_HANDLE_VALUE) close_fd(out);

    return koshka::None;
  }

  return Pipe{in, out};
}

struct thread_start_context
{
  void (*entry)(opaque *);
  opaque *context;
};

fn thread_trampoline(LPVOID raw_context) -> DWORD
{
  let const start = static_cast<thread_start_context *>(raw_context);
  let const entry = start->entry;
  let const context = start->context;
  os::free_aligned(start);
  entry(context);
  return 0;
}

fn start_thread(void (*entry)(opaque *), opaque *context) wontthrow
    -> Maybe<thread>
{
  let const storage = os::allocate_aligned(sizeof(thread_start_context),
                                           alignof(thread_start_context));
  if (storage == nullptr) return koshka::None;
  let const start = new (storage) thread_start_context{entry, context};
  HANDLE handle =
      CreateThread(nullptr, 0, thread_trampoline, start, 0, nullptr);
  if (handle == nullptr) {
    os::free_aligned(start);
    return koshka::None;
  }
  return thread{handle};
}

fn join_thread(thread t) wontthrow -> void
{
  WaitForSingleObject(t.handle, INFINITE);
  CloseHandle(t.handle);
}

fn open_file_descriptor(StringView path, file_open_mode mode)
    -> Maybe<descriptor>
{
  DWORD access = (mode == file_open_mode::Read) ? GENERIC_READ : GENERIC_WRITE;
  if (mode == file_open_mode::ReadWrite) access = GENERIC_READ | GENERIC_WRITE;
  if (mode == file_open_mode::Append) access = FILE_APPEND_DATA;
  DWORD disposition = OPEN_EXISTING;
  switch (mode) {
  case file_open_mode::Truncate: disposition = CREATE_ALWAYS; break;
  case file_open_mode::TruncateNoClobber: disposition = CREATE_NEW; break;
  case file_open_mode::Append: disposition = OPEN_ALWAYS; break;
  case file_open_mode::Read: disposition = OPEN_EXISTING; break;
  case file_open_mode::ReadWrite: disposition = OPEN_ALWAYS; break;
  }

  /* Non-inheritable, execute_program flips it only while spawning the child. */
  SECURITY_ATTRIBUTES att{};
  att.nLength = sizeof(SECURITY_ATTRIBUTES);
  att.bInheritHandle = FALSE;
  att.lpSecurityDescriptor = nullptr; /* NOLINT */

  String path_string =
      path == StringView{"/dev/null"} ? String{"NUL"} : String{path};
  HANDLE handle = CreateFileA(path_string.c_str(), access,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, &att,
                              disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return koshka::None;

  return handle;
}

fn acquire_process_lock(StringView path) throws -> Maybe<descriptor>
{
  const String path_string{path};
  char absolute_path[32768];
  let const length = GetFullPathNameA(
      path_string.c_str(), countof(absolute_path), absolute_path, nullptr);
  if (length == 0 || length >= countof(absolute_path)) return None;

  let lock_path = String{
      StringView{absolute_path, static_cast<usize>(length)}
  };
  if (!is_directory_separator(lock_path.back())) lock_path += '\\';
  lock_path += ".kosh-flock.lock";
  SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
  loop
  {
    let const lock = CreateFileA(
        lock_path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, &attributes,
        OPEN_ALWAYS, FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_NORMAL, nullptr);
    if (lock != INVALID_HANDLE_VALUE) return lock;

    let const error = GetLastError();
    if (error != ERROR_SHARING_VIOLATION && error != ERROR_LOCK_VIOLATION) {
      return None;
    }
    Sleep(10);
  }
}

fn release_process_lock(descriptor lock) wontthrow -> void
{
  unused(close_fd(lock));
}

fn write_to_temp_file(StringView content) -> Maybe<descriptor>
{
  char temp_dir[MAX_PATH];
  let const temp_directory_length = GetTempPathA(MAX_PATH, temp_dir);
  if (temp_directory_length == 0 || temp_directory_length >= MAX_PATH) {
    return koshka::None;
  }

  char temp_path[MAX_PATH];
  if (GetTempFileNameA(temp_dir, "kos", 0, temp_path) == 0) return koshka::None;

  HANDLE handle = CreateFileA(
      temp_path, GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
      FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    DeleteFileA(temp_path);
    return koshka::None;
  }

  usize written_size = 0;
  while (written_size < content.count()) {
    let const write_size = write_fd(handle, content.data + written_size,
                                    content.count() - written_size);
    if (!write_size.has_value() || *write_size == 0) {
      close_fd(handle);
      return koshka::None;
    }
    written_size += *write_size;
  }

  LARGE_INTEGER beginning{};
  if (SetFilePointerEx(handle, beginning, nullptr, FILE_BEGIN) == FALSE) {
    close_fd(handle);
    return koshka::None;
  }

  return handle;
}

fn write_to_named_temp_file(const Path &directory, StringView prefix,
                            StringView content) -> Maybe<Path>
{
  if (prefix.find_character('\\').has_value() ||
      prefix.find_character('/').has_value())
  {
    return None;
  }

  let prefix_text = String{
      prefix.substring_of_length(0, prefix.length < 3 ? prefix.length : 3)};
  while (prefix_text.count() < 3)
    prefix_text.push('x');

  char temp_path[MAX_PATH];
  HANDLE handle = INVALID_HANDLE_VALUE;
  for (u32 attempt = 1; attempt <= 256; attempt++) {
    let unique = static_cast<UINT>(
        (GetCurrentProcessId() ^ GetTickCount() ^ attempt) & 0xffffU);
    if (unique == 0) unique = 1;
    if (GetTempFileNameA(directory.c_str(), prefix_text.c_str(), unique,
                         temp_path) == 0)
    {
      continue;
    }
    handle = CreateFileA(temp_path, GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                         FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (handle != INVALID_HANDLE_VALUE) break;
  }
  if (handle == INVALID_HANDLE_VALUE) return None;

  usize written_size = 0;
  while (written_size < content.count()) {
    let const written = write_fd(handle, content.data + written_size,
                                 content.count() - written_size);
    if (!written.has_value() || *written == 0) {
      unused(close_fd(handle));
      unused(DeleteFileA(temp_path));
      return None;
    }
    written_size += *written;
  }

  if (!close_fd(handle)) {
    unused(DeleteFileA(temp_path));
    return None;
  }

  return Path{StringView{temp_path}};
}

fn wait_and_monitor_process(process p, bool *was_stopped) -> i32
{
  unused(was_stopped);
  defer { CloseHandle(p); };
  if (WaitForSingleObject(p, INFINITE) != WAIT_OBJECT_0)
    throw Error{"Could not wait for the process to finish: " +
                last_system_error_message()};

  DWORD code = -1;
  if (GetExitCodeProcess(p, &code) == 0)
    throw Error{"Could not read the process exit code: " +
                last_system_error_message()};

  return code;
}

fn wait_for_child_state_change() wontthrow -> void {}

fn reap_process_quietly(process p) -> i32
{
  defer { CloseHandle(p); };
  if (WaitForSingleObject(p, INFINITE) != WAIT_OBJECT_0)
    throw Error{"Could not wait for the process to finish: " +
                last_system_error_message()};
  DWORD code = 1;
  GetExitCodeProcess(p, &code);
  return static_cast<i32>(code);
}

fn poll_process(process p, i32 &status_out) wontthrow -> process_state
{
  let const wait_result = WaitForSingleObject(p, 0);
  if (wait_result == WAIT_TIMEOUT) return process_state::Running;
  if (wait_result != WAIT_OBJECT_0) {
    status_out = 0;
    CloseHandle(p);
    return process_state::Exited;
  }

  DWORD code = 0;
  if (GetExitCodeProcess(p, &code) == 0) {
    status_out = 0;
    CloseHandle(p);
    return process_state::Exited;
  }
  status_out = static_cast<i32>(code);
  CloseHandle(p);
  return process_state::Exited;
}

fn signal_process(process p, i32 signal_number) wontthrow -> bool
{
  if (signal_number == 0) {
    if (process_is_group_reference(p)) {
      let const has_members = process_group_has_members(p);
      if (!has_members) SetLastError(ERROR_NOT_FOUND);
      return has_members;
    }
    if (!process_is_pid_reference(p)) {
      let const wait_result = WaitForSingleObject(p, 0);
      if (wait_result == WAIT_TIMEOUT) return true;
      if (wait_result == WAIT_OBJECT_0) SetLastError(ERROR_NOT_FOUND);
      return false;
    }

    let const target = OpenProcess(SYNCHRONIZE, FALSE, pid_from_reference(p));
    if (target == nullptr) return false;
    let const wait_result = WaitForSingleObject(target, 0);
    CloseHandle(target);
    if (wait_result == WAIT_TIMEOUT) return true;
    if (wait_result == WAIT_OBJECT_0) SetLastError(ERROR_NOT_FOUND);
    return false;
  }

  if (!is_process_signal_supported(signal_number)) {
    SetLastError(ERROR_NOT_SUPPORTED);
    return false;
  }

  if (process_is_group_reference(p)) {
    let const process_handle = process_from_group_reference(p);
    char job_name[64];
    if (!timeout_job_name(process_handle, job_name)) return false;
    let const job = OpenJobObjectA(JOB_OBJECT_TERMINATE, FALSE, job_name);
    if (job == nullptr) return false;
    let const did_terminate = TerminateJobObject(job, 1) != FALSE;
    CloseHandle(job);
    return did_terminate;
  }

  if (!process_is_pid_reference(p)) return TerminateProcess(p, 1) != 0;

  let const target =
      OpenProcess(PROCESS_TERMINATE, FALSE, pid_from_reference(p));
  if (target == nullptr) return false;
  let const did_terminate = TerminateProcess(target, 1) != 0;
  CloseHandle(target);
  return did_terminate;
}

fn process_group_has_members(process group) wontthrow -> bool
{
  if (!process_is_group_reference(group)) return false;

  let const process_handle = process_from_group_reference(group);
  char job_name[64];
  if (!timeout_job_name(process_handle, job_name)) return false;

  let const job = OpenJobObjectA(JOB_OBJECT_QUERY, FALSE, job_name);
  if (job == nullptr) return false;

  JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
  let const did_query =
      QueryInformationJobObject(job, JobObjectBasicAccountingInformation,
                                &accounting, sizeof(accounting), nullptr);
  CloseHandle(job);
  return did_query != FALSE && accounting.ActiveProcesses > 0;
}

fn is_process_signal_supported(i32 signal_number) wontthrow -> bool
{
  return signal_number == 0 || signal_number == 9 || signal_number == 15;
}

fn process_from_pid(i64 pid) wontthrow -> process
{
  if (pid <= 0 || static_cast<u64>(pid) > UINT32_MAX) return nullptr;
  let const encoded = (static_cast<uintptr_t>(pid) << 2u) | PID_REFERENCE_TAG;
  return reinterpret_cast<process>(encoded);
}

fn signal_number_from_name(StringView name) -> Maybe<i32>
{
  if (name.is_all_decimal_digits()) {
    const ErrorOr<i64> parsed_value = name.to<i64>();
    if (parsed_value.is_error() || parsed_value.value() < INT32_MIN ||
        parsed_value.value() > INT32_MAX)
      return koshka::None;
    return static_cast<i32>(parsed_value.value());
  }

  let const bare = utils::strip_sig_prefix(name);

  static constexpr static_string_entry<i32> NAME_ENTRIES[] = {
      {SSK("HUP"),  1 },
      {SSK("QUIT"), 3 },
      {SSK("KILL"), 9 },
      {SSK("TERM"), 15},
      {SSK("INT"),  2 },
  };
  static constexpr StaticStringMap NAMES{NAME_ENTRIES};
  return NAMES.find(bare);
}

fn signal_name_from_number(i32 number) -> Maybe<String>
{
  switch (number) {
  case 1: return String{"HUP"};
  case 2: return String{"INT"};
  case 3: return String{"QUIT"};
  case 9: return String{"KILL"};
  case 15: return String{"TERM"};
  }

  return None;
}

fn signal_names() throws -> const ArrayList<StringView> &
{
  static ArrayList<StringView> names = [] throws {
    let collected = ArrayList<StringView>{heap_allocator()};
    static const StringView WINDOWS_SIGNAL_NAMES[] = {"HUP", "INT", "QUIT",
                                                      "KILL", "TERM"};
    for (const StringView name : WINDOWS_SIGNAL_NAMES)
      collected.push(name);
    return collected;
  }();
  return names;
}

/* Quotes and escapes the way CommandLineToArgvW parses back, so an argument
   with a space, tab, or quote cannot inject further arguments. A backslash run
   is doubled only before a quote, an empty argument is quoted so it is kept. */
static fn append_windows_quoted_arg(String &out, StringView arg) -> void
{
  bool should_quote_arg = arg.count() == 0;
  for (usize i = 0; i < arg.count() && !should_quote_arg; i++) {
    const char c = arg[i];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '"') {
      should_quote_arg = true;
    }
  }
  if (!should_quote_arg) {
    out.append(arg);
    return;
  }

  out += '"';
  for (usize i = 0; i < arg.count();) {
    usize backslash_count = 0;
    while (i < arg.count() && arg[i] == '\\') {
      i++;
      backslash_count++;
    }
    if (i == arg.count()) {
      /* Trailing backslashes precede the closing quote, so they are doubled to
         stay literal rather than escaping the quote. */
      for (usize k = 0; k < backslash_count * 2; k++)
        out += '\\';
      break;
    }
    if (arg[i] == '"') {
      /* The backslashes before a quote are doubled and the quote is escaped. */
      for (usize k = 0; k < backslash_count * 2 + 1; k++)
        out += '\\';
      out += '"';
      i++;
    } else {
      for (usize k = 0; k < backslash_count; k++)
        out += '\\';
      out += arg[i];
      i++;
    }
  }
  out += '"';
}

fn make_os_args(const ArrayList<String> &args) -> os_args
{
  ASSERT(args.count() > 0);

  String command_line{heap_allocator()};
  append_windows_quoted_arg(command_line, args[0].view());
  for (usize i = 1; i < args.count(); i++) {
    command_line += ' ';
    append_windows_quoted_arg(command_line, args[i].view());
  }

  return command_line;
}

cold fn last_system_error_message() throws -> String
{
  LPSTR errno_str{};
  DWORD win_errno = GetLastError();

  DWORD ret = FormatMessageA(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, win_errno, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
      reinterpret_cast<LPSTR>(&errno_str), 0, nullptr); /* NOLINT */

  if (ret == 0) {
    return String::from(win_errno, heap_allocator()) +
           StringView{" (Error message could not be processed due to "
                      "a FormatMessage() failure)"};
  }
  defer { LocalFree(errno_str); };

  StringView view{static_cast<char *>(errno_str)};
  /* FormatMessage ends with a period, spacing, and a CRLF, trimmed here. */
  while (view.length > 0) {
    let const last_byte = view[view.length - 1];
    if (last_byte != '.' && last_byte != ' ' && last_byte != '\r' &&
        last_byte != '\n')
    {
      break;
    }
    view = view.substring_of_length(0, view.length - 1);
  }

  String err{heap_allocator()};
  for (usize i = 0; i < view.length; i++) {
    /* A %N placeholder is replaced with a word since no argument is passed. */
    if (view[i] == '%' && i + 1 < view.length && isdigit(view[i + 1])) {
      err += StringView{"input"};
      i++;
      continue;
    }
    err.push(view[i]);
  }

  if (err.length() > 0) {
    String capitalized{heap_allocator()};
    capitalized.push(static_cast<char>(toupper(err[0])));
    capitalized += err.substring(1);
    err = steal(capitalized);
  }

  return err;
}

fn last_system_error_is_missing_file() wontthrow -> bool
{
  let const error = GetLastError();
  return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

static fn handle_interrupt(int s) -> void
{
  unused(s);
  INTERRUPT_REQUESTED = 1;
  signal(SIGINT, handle_interrupt);
}

fn set_default_signal_handlers(signal_profile profile) -> void
{
  let const is_interactive = profile == signal_profile::Interactive;
  /* The interactive shell ignores SIGTERM so a stray terminate does not close
     the prompt. */
  if (is_interactive && signal(SIGTERM, SIG_IGN) == SIG_ERR) {
    throw Error{"Could not install the signal handlers: " +
                last_system_error_message()};
  }

  if (signal(SIGINT, handle_interrupt) == SIG_ERR) {
    throw Error{"Could not install the signal handlers: " +
                last_system_error_message()};
  }
}

fn reset_signal_handlers() -> void
{
  if (signal(SIGTERM, SIG_DFL) == SIG_ERR || signal(SIGINT, SIG_DFL) == SIG_ERR)
  {
    throw Error{"Could not restore the default signal handlers: " +
                last_system_error_message()};
  }

  /* A stale inherited flag would throw Interrupted before the child runs. */
  INTERRUPT_REQUESTED = 0;
}

static fn handle_trapped_signal(int signal_number) -> void
{
  if (is_trappable_signal(signal_number))
    PENDING_SIGNAL_FLAGS[signal_number] = 1;
  SIGNAL_PENDING = 1;
  /* The C runtime resets the disposition, so it is reinstalled for the next. */
  signal(signal_number, handle_trapped_signal);
}

fn set_trap_handler(i32 signal_number) -> void
{
  if (!is_trappable_signal(signal_number)) return;
  signal(signal_number, handle_trapped_signal);
}

fn set_trap_ignore(i32 signal_number) -> void
{
  if (!is_trappable_signal(signal_number)) return;
  signal(signal_number, SIG_IGN);
}

fn clear_trap_handler(i32 signal_number) -> void
{
  if (!is_trappable_signal(signal_number)) return;
  if (signal_number == SIGINT)
    signal(signal_number, handle_interrupt);
  else
    signal(signal_number, SIG_DFL);
}

fn monotonic_nanos() wontthrow -> u64
{
  static const LARGE_INTEGER frequency = [] {
    LARGE_INTEGER value{};
    QueryPerformanceFrequency(&value);
    return value;
  }();
  LARGE_INTEGER counter;
  if (frequency.QuadPart == 0) return 0;
  if (QueryPerformanceCounter(&counter) == 0) return 0;
  /* The counter is scaled to nanoseconds through the frequency, splitting the
     whole seconds from the remainder so the multiply never overflows the way a
     raw counter times a billion would. */
  const u64 whole_seconds = counter.QuadPart / frequency.QuadPart;
  const u64 remainder = counter.QuadPart % frequency.QuadPart;
  return whole_seconds * 1000000000ULL +
         (remainder * 1000000000ULL) / static_cast<u64>(frequency.QuadPart);
}

static fn query_parent_process_id() wontthrow -> i64
{
  let const snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return 0;
  defer { CloseHandle(snapshot); };

  PROCESSENTRY32 entry{};
  entry.dwSize = sizeof(entry);
  if (Process32First(snapshot, &entry) == FALSE) return 0;

  let const process_id = GetCurrentProcessId();
  do {
    if (entry.th32ProcessID == process_id)
      return static_cast<i64>(entry.th32ParentProcessID);
  } while (Process32Next(snapshot, &entry) != FALSE);

  return 0;
}

fn get_parent_process_id() wontthrow -> i64
{
  static const i64 parent_process_id = query_parent_process_id();
  return parent_process_id;
}

fn get_real_user_id() wontthrow -> i64 { return 0; }

fn get_effective_user_id() wontthrow -> i64 { return 0; }

fn get_real_group_id() wontthrow -> i64 { return 0; }

fn child_max() wontthrow -> i64 { return 0; }

fn machine_type() throws -> String { return String{"x86_64"}; }

fn executable_system_name() throws -> String { return String{"Windows"}; }

fn executable_machine_name() throws -> String
{
#if defined _M_ARM64 || defined __aarch64__
  return String{"arm64"};
#elif defined _M_X64 || defined __x86_64__
  return String{"x86_64"};
#else
  return machine_type();
#endif
}

fn realtime_microseconds() wontthrow -> u64
{
  FILETIME file_time;
  GetSystemTimePreciseAsFileTime(&file_time);
  ULARGE_INTEGER ticks;
  ticks.LowPart = file_time.dwLowDateTime;
  ticks.HighPart = file_time.dwHighDateTime;
  /* FILETIME counts 100ns intervals since 1601, so the 1970 offset is removed.
   */
  const u64 epoch_offset_100ns = 116444736000000000ULL;
  if (ticks.QuadPart < epoch_offset_100ns) return 0;
  return (ticks.QuadPart - epoch_offset_100ns) / 10ULL;
}

fn format_local_time(StringView format, i64 epoch) throws -> String
{
  const time_t when = epoch < 0 ? time(nullptr) : static_cast<time_t>(epoch);
  struct tm broken_down{};
  localtime_s(&broken_down, &when);
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
  /* Windows has no RUSAGE_CHILDREN, only the wall time is meaningful. */
  user_seconds = 0;
  system_seconds = 0;
}

fn children_peak_rss_bytes() wontthrow -> u64 { return 0; }

fn read_malloc_heap_stats(malloc_heap_stats &stats) wontthrow -> bool
{
  unused(stats);
  return false;
}

fn run_measured(const ArrayList<String> &argv, measured_output output,
                Maybe<descriptor> inherited_handle) throws
    -> Maybe<measured_result>
{
  let const suppress_output = output == measured_output::Suppress;
  if (argv.is_empty()) return None;

  /* Windows has no hardware perf counters, only wall time and peak working set.
   */
  measured_result result{};

  let command_line = make_os_args(argv);

  STARTUPINFOA startup{};
  startup.cb = sizeof(startup);
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
  startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);

  HANDLE null_handle = INVALID_HANDLE_VALUE;
  if (suppress_output) {
    SECURITY_ATTRIBUTES inherit_sa{};
    inherit_sa.nLength = sizeof(inherit_sa);
    inherit_sa.bInheritHandle = TRUE;
    null_handle = CreateFileA("NUL", GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit_sa,
                              OPEN_EXISTING, 0, nullptr);
    if (null_handle != INVALID_HANDLE_VALUE) {
      if (startup.hStdInput == nullptr ||
          startup.hStdInput == INVALID_HANDLE_VALUE)
        startup.hStdInput = null_handle;
      startup.hStdOutput = null_handle;
      startup.hStdError = null_handle;
    }
  }
  defer
  {
    if (null_handle != INVALID_HANDLE_VALUE) CloseHandle(null_handle);
  };

  PROCESS_INFORMATION process_info{};
  let const standard_handles_are_valid =
      startup.hStdInput != nullptr &&
      startup.hStdInput != INVALID_HANDLE_VALUE &&
      startup.hStdOutput != nullptr &&
      startup.hStdOutput != INVALID_HANDLE_VALUE &&
      startup.hStdError != nullptr && startup.hStdError != INVALID_HANDLE_VALUE;
  let const should_use_standard_handles =
      standard_handles_are_valid &&
      (suppress_output || inherited_handle.has_value());
  if (should_use_standard_handles) startup.dwFlags = STARTF_USESTDHANDLES;

  inherited_handle_state input_inheritance{};
  inherited_handle_state output_inheritance{};
  inherited_handle_state error_inheritance{};
  inherited_handle_state extra_inheritance{};
  if (should_use_standard_handles) {
    make_handle_inheritable(startup.hStdInput, input_inheritance);
    make_handle_inheritable(startup.hStdOutput, output_inheritance);
    make_handle_inheritable(startup.hStdError, error_inheritance);
  }
  if (inherited_handle.has_value())
    make_handle_inheritable(*inherited_handle, extra_inheritance);
  defer { restore_handle_inheritance(input_inheritance); };
  defer { restore_handle_inheritance(output_inheritance); };
  defer { restore_handle_inheritance(error_inheritance); };
  defer { restore_handle_inheritance(extra_inheritance); };

  const u64 start_nanos = monotonic_nanos();
  let const should_inherit_handles =
      should_use_standard_handles || inherited_handle.has_value();

  /* CreateProcessA may rewrite lpCommandLine in place. */
  if (CreateProcessA(nullptr, const_cast<LPSTR>(command_line.data()), nullptr,
                     nullptr, should_inherit_handles, 0, nullptr, nullptr,
                     &startup, &process_info) == 0)
    return None;
  defer { CloseHandle(process_info.hProcess); };
  defer { CloseHandle(process_info.hThread); };

  if (WaitForSingleObject(process_info.hProcess, INFINITE) != WAIT_OBJECT_0)
    return None;

  result.wall_nanos = monotonic_nanos() - start_nanos;

  DWORD exit_code = 0;
  if (GetExitCodeProcess(process_info.hProcess, &exit_code) == FALSE)
    return None;
  result.exit_status = static_cast<i64>(exit_code);

  PROCESS_MEMORY_COUNTERS memory_counters{};
  memory_counters.cb = sizeof(memory_counters);
  if (GetProcessMemoryInfo(process_info.hProcess, &memory_counters,
                           sizeof(memory_counters)) != 0)
    result.peak_rss_bytes =
        static_cast<u64>(memory_counters.PeakWorkingSetSize);

  return result;
}

fn make_directory(StringView path, u32 mode) wontthrow -> bool
{
  unused(mode);
  const String path_string{path};
  return CreateDirectoryA(path_string.c_str(), nullptr) != 0;
}

fn set_file_mode(StringView path, u32 mode) wontthrow -> bool
{
  /* Windows has no POSIX permission bits, so the mode is accepted and ignored.
   */
  unused(path);
  unused(mode);
  return true;
}

fn touch_file_times(StringView path) wontthrow -> bool
{
  const String path_string{path};
  HANDLE handle =
      CreateFileA(path_string.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ,
                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return false;

  FILETIME now;
  GetSystemTimeAsFileTime(&now);
  let const did_set = SetFileTime(handle, nullptr, &now, &now) != 0;
  CloseHandle(handle);
  return did_set;
}

fn remove_directory(StringView path) wontthrow -> bool
{
  const String path_string{path};
  return RemoveDirectoryA(path_string.c_str()) != 0;
}

fn remove_file(StringView path) wontthrow -> bool
{
  const String path_string{path};
  return DeleteFileA(path_string.c_str()) != 0;
}

fn rename_path(StringView from, StringView to) wontthrow -> bool
{
  const String from_string{from};
  const String to_string{to};
  return MoveFileExA(from_string.c_str(), to_string.c_str(),
                     MOVEFILE_REPLACE_EXISTING) != 0;
}

fn create_symlink(StringView target, StringView link_path) wontthrow -> bool
{
  const String target_string{target};
  const String link_string{link_path};
/* An older mingw SDK omits these flags, defined here when absent. */
#ifndef SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE
#define SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE 0x2
#endif
#ifndef SYMBOLIC_LINK_FLAG_DIRECTORY
#define SYMBOLIC_LINK_FLAG_DIRECTORY 0x1
#endif
  /* A directory target needs the directory flag, the unprivileged flag avoids
     elevation on developer-mode Windows. */
  DWORD flags = SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE;
  const DWORD attributes = GetFileAttributesA(target_string.c_str());
  if (attributes != INVALID_FILE_ATTRIBUTES &&
      (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
    flags |= SYMBOLIC_LINK_FLAG_DIRECTORY;
  return CreateSymbolicLinkA(link_string.c_str(), target_string.c_str(),
                             flags) != 0;
}

fn read_symlink(StringView path) wontthrow -> Maybe<String>
{
  struct symbolic_link_reparse_data
  {
    u32 tag;
    u16 data_length;
    u16 reserved;
    u16 substitute_name_offset;
    u16 substitute_name_length;
    u16 print_name_offset;
    u16 print_name_length;
    u32 flags;
    WCHAR path_buffer[1];
  };
  struct mount_point_reparse_data
  {
    u32 tag;
    u16 data_length;
    u16 reserved;
    u16 substitute_name_offset;
    u16 substitute_name_length;
    u16 print_name_offset;
    u16 print_name_length;
    WCHAR path_buffer[1];
  };

  const String path_string{path};
  let const handle = CreateFileA(
      path_string.c_str(), 0,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
      nullptr);
  if (handle == INVALID_HANDLE_VALUE) return koshka::None;
  defer { CloseHandle(handle); };

  alignas(void *) u8 buffer[MAXIMUM_REPARSE_DATA_BUFFER_SIZE];
  DWORD bytes_returned = 0;
  if (DeviceIoControl(handle, FSCTL_GET_REPARSE_POINT, nullptr, 0, buffer,
                      sizeof(buffer), &bytes_returned, nullptr) == FALSE)
  {
    return koshka::None;
  }

  const WCHAR *wide_target = nullptr;
  usize wide_target_length = 0;
  let const tag = *reinterpret_cast<const u32 *>(buffer);
  if (tag == IO_REPARSE_TAG_SYMLINK) {
    let const *data =
        reinterpret_cast<const symbolic_link_reparse_data *>(buffer);
    let const offset = data->print_name_length > 0
                           ? data->print_name_offset
                           : data->substitute_name_offset;
    let const length = data->print_name_length > 0
                           ? data->print_name_length
                           : data->substitute_name_length;
    wide_target = reinterpret_cast<const WCHAR *>(
        reinterpret_cast<const u8 *>(data->path_buffer) + offset);
    wide_target_length = length / sizeof(WCHAR);
  } else if (tag == IO_REPARSE_TAG_MOUNT_POINT) {
    let const *data =
        reinterpret_cast<const mount_point_reparse_data *>(buffer);
    let const offset = data->print_name_length > 0
                           ? data->print_name_offset
                           : data->substitute_name_offset;
    let const length = data->print_name_length > 0
                           ? data->print_name_length
                           : data->substitute_name_length;
    wide_target = reinterpret_cast<const WCHAR *>(
        reinterpret_cast<const u8 *>(data->path_buffer) + offset);
    wide_target_length = length / sizeof(WCHAR);
  } else {
    return koshka::None;
  }

  if (wide_target_length == 0) return String{StringView{}};
  let const utf8_length = WideCharToMultiByte(
      CP_UTF8, 0, wide_target, static_cast<int>(wide_target_length), nullptr, 0,
      nullptr, nullptr);
  if (utf8_length <= 0) return koshka::None;
  let utf8_target = ArrayList<char>{heap_allocator()};
  utf8_target.reserve(static_cast<usize>(utf8_length));
  if (WideCharToMultiByte(
          CP_UTF8, 0, wide_target, static_cast<int>(wide_target_length),
          utf8_target.begin(), utf8_length, nullptr, nullptr) != utf8_length)
  {
    return koshka::None;
  }
  return String{
      StringView{utf8_target.begin(), static_cast<usize>(utf8_length)}
  };
}

fn current_executable_path() wontthrow -> Maybe<String>
{
  char module_path[MAX_PATH];
  let const module_path_length =
      GetModuleFileNameA(nullptr, module_path, MAX_PATH);
  if (module_path_length == 0 || module_path_length == MAX_PATH) {
    return koshka::None;
  }

  return String{
      StringView{module_path, module_path_length}
  };
}

fn stat_path(StringView path, file_status &status) wontthrow -> bool
{
  if (path == StringView{"/dev/null"}) {
    status = {};
    status.mode = 0020666u;
    status.link_count = 1;
    return true;
  }

  const String path_string{path};
  let const attributes = GetFileAttributesA(path_string.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) return false;
  if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
    let const target = read_symlink(path);
    if (!target.has_value()) return false;

    status.device_id = 0;
    status.file_id = 0;
    status.has_file_identity = false;
    status.mode = 0120000u | 0777u;
    status.link_count = 1;
    status.owner_id = 0;
    status.group_id = 0;
    status.size = target->length();
    status.modification_time = 0;
    status.modification_nanoseconds = 0;
    status.change_time = 0;
    status.change_nanoseconds = 0;
    status.blocks = (status.size + 511) / 512;

    let const handle = CreateFileA(
        path_string.c_str(), 0,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle != INVALID_HANDLE_VALUE) {
      BY_HANDLE_FILE_INFORMATION identity{};
      if (GetFileInformationByHandle(handle, &identity)) {
        status.device_id = identity.dwVolumeSerialNumber;
        status.file_id = (static_cast<u64>(identity.nFileIndexHigh) << 32) |
                         identity.nFileIndexLow;
        status.has_file_identity = true;
        status.link_count = identity.nNumberOfLinks;
        ULARGE_INTEGER modification_ticks{};
        modification_ticks.LowPart = identity.ftLastWriteTime.dwLowDateTime;
        modification_ticks.HighPart = identity.ftLastWriteTime.dwHighDateTime;
        status.modification_time = static_cast<i64>(
            modification_ticks.QuadPart / 10000000ULL - 11644473600ULL);
        status.modification_nanoseconds = static_cast<u32>(
            modification_ticks.QuadPart % 10000000ULL * 100ULL);
        status.change_time = status.modification_time;
        status.change_nanoseconds = status.modification_nanoseconds;
      }
      CloseHandle(handle);
    }
    return true;
  }

  struct stat info{};
  if (::stat(path_string.c_str(), &info) != 0) return false;
  status.device_id = 0;
  status.file_id = 0;
  status.has_file_identity = false;
  let const handle =
      CreateFileA(path_string.c_str(), 0,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                  nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (handle != INVALID_HANDLE_VALUE) {
    BY_HANDLE_FILE_INFORMATION identity{};
    if (GetFileInformationByHandle(handle, &identity)) {
      status.device_id = identity.dwVolumeSerialNumber;
      status.file_id = (static_cast<u64>(identity.nFileIndexHigh) << 32) |
                       identity.nFileIndexLow;
      status.has_file_identity = true;
    }
    CloseHandle(handle);
  }
  status.mode = static_cast<u32>(info.st_mode);
  status.link_count = static_cast<u64>(info.st_nlink);
  status.owner_id = static_cast<u32>(info.st_uid);
  status.group_id = static_cast<u32>(info.st_gid);
  status.size = static_cast<u64>(info.st_size);
  status.modification_time = static_cast<i64>(info.st_mtime);
  status.change_time = status.modification_time;
  WIN32_FILE_ATTRIBUTE_DATA attribute_data{};
  if (GetFileAttributesExA(path_string.c_str(), GetFileExInfoStandard,
                           &attribute_data) != 0)
  {
    ULARGE_INTEGER modification_ticks{};
    modification_ticks.LowPart = attribute_data.ftLastWriteTime.dwLowDateTime;
    modification_ticks.HighPart = attribute_data.ftLastWriteTime.dwHighDateTime;
    status.modification_nanoseconds =
        static_cast<u32>(modification_ticks.QuadPart % 10000000ULL * 100ULL);
    status.change_nanoseconds = status.modification_nanoseconds;
  }
  /* Windows stat has no block count, so 512-byte blocks are derived from size.
   */
  status.blocks = (static_cast<u64>(info.st_size) + 511) / 512;
  return true;
}

fn stat_path_following(StringView path, file_status &status) wontthrow -> bool
{
  usize position = 0;
  while (position < path.length) {
    while (position < path.length && is_directory_separator(path[position]))
      position++;
    let const component_start = position;
    while (position < path.length && !is_directory_separator(path[position]))
      position++;
    let const component_length = position - component_start;
    if ((component_length == 1 && path[component_start] == '.') ||
        (component_length == 2 && path[component_start] == '.' &&
         path[component_start + 1] == '.'))
    {
      let const resolved = canonical_path(Path{path});
      return resolved.has_value() && stat_path(resolved->text().view(), status);
    }
  }

  return stat_path(path, status);
}

fn format_mode_string(u32 mode) throws -> String
{
  /* Windows stat exposes only the owner bits, mirrored across all three
   * triplets. */
  let const is_readable = (mode & 0000400u) != 0;
  let const is_writable = (mode & 0000200u) != 0;
  let const is_executable = (mode & 0000100u) != 0;

  String result{heap_allocator()};
  result.push(file_type_letter(mode));
  for (usize triplet = 0; triplet < 3; triplet++) {
    result.push(is_readable ? 'r' : '-');
    result.push(is_writable ? 'w' : '-');
    result.push(is_executable ? 'x' : '-');
  }
  return result;
}

fn file_type_letter(u32 mode) wontthrow -> char
{
  if ((mode & 0170000u) == 0120000u) return 'l';
  if ((mode & 0170000u) == 0020000u) return 'c';
  return (mode & 0040000u) != 0 ? 'd' : '-';
}

fn uid_to_username(u32 uid) throws -> Maybe<String>
{
  /* Windows names users through the security database, so ls uses the numeric
   * id. */
  unused(uid);
  return koshka::None;
}

fn gid_to_groupname(u32 gid) throws -> Maybe<String>
{
  unused(gid);
  return koshka::None;
}

fn sleep_for_seconds(double seconds) wontthrow -> void
{
  if (seconds <= 0.0) return;
  Sleep(static_cast<DWORD>(seconds * 1000.0));
}

fn enumerate_processes(process_detail detail) throws -> ArrayList<process_entry>
{
  /* The snapshot has no per-process resource stats, so the BSD columns stay
   * zero. */
  unused(detail);
  ArrayList<process_entry> processes{heap_allocator()};
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return processes;
  defer { CloseHandle(snapshot); };

  PROCESSENTRY32 entry{};
  entry.dwSize = sizeof(entry);
  if (Process32First(snapshot, &entry) == 0) return processes;
  do {
    process_entry process{};
    process.pid = static_cast<i64>(entry.th32ProcessID);
    process.name = String{entry.szExeFile};
    /* The snapshot exposes only the executable name, used as the command line.
     */
    process.command_line = process.name.clone();
    processes.push(steal(process));
  } while (Process32Next(snapshot, &entry) != 0);
  return processes;
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

fn get_shell_process_id() wontthrow -> i64
{
  return static_cast<i64>(PARENT_SHELL_PID);
}

fn get_current_process_id() wontthrow -> i64
{
  return static_cast<i64>(GetCurrentProcessId());
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

fn register_platform_flags(FlagList &flags) throws -> void { unused(flags); }

fn initialize_platform_runtime() wontthrow -> void {}

} /* namespace os */
} /* namespace koshka */
