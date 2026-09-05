/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements general Win32 text conversion, descriptors, named
 * pipes, signals, users, clocks, environment access, and regex allocation.
 * Filesystem and process-launch services stay in separate translation units.
 */

#include "Cli.hpp"
#include "Common.hpp"
#include "Debug.hpp"
#include "Errors.hpp"
#include "Eval.hpp"
#include "EvalVariablesInternal.hpp"
#include "Platform.hpp"
#include "Trace.hpp"
#include "Utils.hpp"

#include <fcntl.h>

#define KOSH_UMASK(mask) _umask(static_cast<int>(mask))

struct alignas(max_align_t) regex_allocation_header
{
  size_t allocation_length;
  size_t payload_length;
};

extern "C" void *kosh_regex_allocate(size_t length)
{
  if (length > SIZE_MAX - sizeof(regex_allocation_header)) return nullptr;
  let const allocation_length = sizeof(regex_allocation_header) + length;
  regex_allocation_header *header = nullptr;
  try {
    header = static_cast<regex_allocation_header *>(
        koshka::uncached_heap_allocator().raw_alloc(
            allocation_length, alignof(regex_allocation_header)));
  } catch (...) {
    return nullptr;
  }
  header->allocation_length = allocation_length;
  header->payload_length = length;

  return header + 1;
}

extern "C" void *kosh_regex_allocate_zeroed(size_t count, size_t length)
{
  if (length != 0 && count > SIZE_MAX / length) {
    return nullptr;
  }
  let const payload_length = count * length;
  let pointer = kosh_regex_allocate(payload_length);
  if (pointer != nullptr) std::memset(pointer, 0, payload_length);

  return pointer;
}

extern "C" void kosh_regex_release(void *pointer)
{
  if (pointer == nullptr) return;
  let header = static_cast<regex_allocation_header *>(pointer) - 1;
  koshka::uncached_heap_allocator().raw_free(header, header->allocation_length,
                                             alignof(regex_allocation_header));
}

extern "C" void *kosh_regex_reallocate(void *pointer, size_t length)
{
  if (pointer == nullptr) return kosh_regex_allocate(length);
  if (length == 0) {
    kosh_regex_release(pointer);
    return nullptr;
  }

  if (length > SIZE_MAX - sizeof(regex_allocation_header)) return nullptr;
  let old_header = static_cast<regex_allocation_header *>(pointer) - 1;
  let const allocation_length = sizeof(regex_allocation_header) + length;
  try {
    let header = static_cast<regex_allocation_header *>(
        koshka::uncached_heap_allocator().raw_realloc(
            old_header, old_header->allocation_length, allocation_length,
            alignof(regex_allocation_header)));
    header->allocation_length = allocation_length;
    header->payload_length = length;
    return header + 1;
  } catch (...) {
    return nullptr;
  }
}

namespace koshka {

namespace os {

static i32 HIGHEST_OPEN_SHELL_FD = 2;
static bool HAS_SCANNED_INHERITED_SHELL_FDS = false;

static fn scan_inherited_shell_fds() wontthrow -> void
{
  if (HAS_SCANNED_INHERITED_SHELL_FDS) return;
  HAS_SCANNED_INHERITED_SHELL_FDS = true;
  let const maximum_fd = _getmaxstdio();

  for (i32 shell_fd = 3; shell_fd < maximum_fd; shell_fd++)
    if (descriptor_from_fd_number(shell_fd) != KOSH_INVALID_FD)
      HIGHEST_OPEN_SHELL_FD = shell_fd;
}

static fn note_shell_fd_opened(i32 shell_fd) wontthrow -> void
{
  if (shell_fd > HIGHEST_OPEN_SHELL_FD) HIGHEST_OPEN_SHELL_FD = shell_fd;
}

static fn note_shell_fd_closed(i32 shell_fd) wontthrow -> void
{
  if (shell_fd != HIGHEST_OPEN_SHELL_FD) return;
  while (HIGHEST_OPEN_SHELL_FD > 2 &&
         descriptor_from_fd_number(HIGHEST_OPEN_SHELL_FD) == KOSH_INVALID_FD)
  {
    HIGHEST_OPEN_SHELL_FD--;
  }
}

static fn utf8_to_wide(StringView text, Allocator allocator) throws
    -> Maybe<ArrayList<wchar_t>>
{
  if (text.length > static_cast<usize>(INT_MAX)) {
    SetLastError(ERROR_FILENAME_EXCED_RANGE);
    return None;
  }

  ArrayList<wchar_t> wide{allocator};
  if (text.is_empty()) {
    wide.reserve(1);
    wide.begin()[0] = L'\0';
    return wide;
  }

  let const wide_length =
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data,
                          static_cast<int>(text.length), nullptr, 0);
  if (wide_length <= 0) return None;
  wide.reserve(static_cast<usize>(wide_length) + 1);
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data,
                          static_cast<int>(text.length), wide.begin(),
                          wide_length) != wide_length)
    return None;
  wide.begin()[wide_length] = L'\0';
  return wide;
}

static fn wide_to_utf8(const wchar_t *text, usize length,
                       Allocator allocator) throws -> Maybe<String>
{
  if (length == 0) return String{allocator};
  if (length > static_cast<usize>(INT_MAX)) {
    SetLastError(ERROR_FILENAME_EXCED_RANGE);
    return None;
  }
  let const utf8_length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                              text, static_cast<int>(length),
                                              nullptr, 0, nullptr, nullptr);
  if (utf8_length <= 0) return None;
  ArrayList<char> utf8{allocator};
  utf8.reserve(static_cast<usize>(utf8_length));
  if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text,
                          static_cast<int>(length), utf8.begin(), utf8_length,
                          nullptr, nullptr) != utf8_length)
    return None;
  return String{
      allocator, StringView{utf8.begin(), static_cast<usize>(utf8_length)}
  };
}

fn logged_in_users() throws -> ArrayList<user_session>
{
  let result = ArrayList<user_session>{heap_allocator()};
  let const user = get_current_user();
  let const terminal = terminal_name(KOSH_STDIN);
  if (user.has_value() && terminal.has_value())
    result.push(user_session{steal(*user), steal(*terminal), 0});
  return result;
}

fn write_system_log(StringView tag, StringView priority, StringView message,
                    bool should_include_pid,
                    bool should_copy_to_stderr) wontthrow -> bool
{
  let output = String{heap_allocator(), tag};
  if (should_include_pid) {
    output += '[';
    output += String::from(GetCurrentProcessId(), heap_allocator());
    output += ']';
  }
  if (!output.is_empty()) output += ": ";
  output += message;
  output += '\n';

  WORD event_type = EVENTLOG_INFORMATION_TYPE;
  let const dot = priority.find_character('.');
  let const severity =
      dot.has_value() ? priority.substring(*dot + 1) : priority;
  if (severity == "emerg" || severity == "alert" || severity == "crit" ||
      severity == "err")
  {
    event_type = EVENTLOG_ERROR_TYPE;
  } else if (severity == "warning" || severity == "notice") {
    event_type = EVENTLOG_WARNING_TYPE;
  } else if (severity != "info" && severity != "debug") {
    SetLastError(ERROR_INVALID_PARAMETER);
    return false;
  }

  let const source_name =
      String{heap_allocator(), tag.is_empty() ? "kosh" : tag};
  let const event_source = RegisterEventSourceA(nullptr, source_name.c_str());
  if (event_source == nullptr) return false;
  defer { DeregisterEventSource(event_source); };
  const char *event_strings[] = {output.c_str()};
  let const was_reported = ReportEventA(event_source, event_type, 0, 0, nullptr,
                                        1, 0, event_strings, nullptr) != FALSE;
  if (should_copy_to_stderr) {
    unused(write_fd(KOSH_STDERR, output.view().data, output.length()));
  }

  return was_reported;
}

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
  if (GetFileType(fd) != FILE_TYPE_DISK) return false;
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
    try {
      let const wide_path =
          utf8_to_wide(m_paths[i].text().view(), heap_allocator());
      if (wide_path.has_value() && DeleteFileW(wide_path->begin()) != FALSE)
        continue;
    } catch (...) {}
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
    let const original = descriptor_for_shell_fd(shell_fd);
    result.original = original;
    result.was_open = original != nullptr && original != INVALID_HANDLE_VALUE;
    if (result.was_open &&
        DuplicateHandle(GetCurrentProcess(), original, GetCurrentProcess(),
                        &result.saved, 0, FALSE,
                        DUPLICATE_SAME_ACCESS) == FALSE)
    {
      result.is_dup2_ok = false;
      return result;
    }
    result.is_dup2_ok = replace_descriptor(shell_fd, target);
    if (!result.is_dup2_ok && result.was_open) {
      CloseHandle(result.saved);
    }
    return result;
  }

  if (target == nullptr || target == INVALID_HANDLE_VALUE) {
    result.is_dup2_ok = false;
    return result;
  }

  let const original = GetStdHandle(*slot);
  result.original = original;
  result.was_open = original != nullptr && original != INVALID_HANDLE_VALUE;
  result.saved = original;

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
  result.is_dup2_ok = true;
  note_descriptor_rebound();

  return result;
}

fn restore_descriptor(const saved_descriptor &saved) wontthrow -> void
{
  if (!saved.is_dup2_ok) return;
  const Maybe<DWORD> slot = std_handle_slot_for_shell_fd(saved.shell_fd);
  if (!slot.has_value()) {
    if (saved.was_open) {
      replace_descriptor(saved.shell_fd, saved.saved);
      CloseHandle(saved.saved);
    } else {
      close_shell_fd(saved.shell_fd);
    }
    return;
  }
  let const replaced = GetStdHandle(*slot);
  if (saved.was_open && replaced == saved.original) {
    CloseHandle(saved.saved);
    return;
  }
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
    let const original = descriptor_for_shell_fd(shell_fd);
    result.original = original;
    result.was_open = original != nullptr && original != INVALID_HANDLE_VALUE;
    if (result.was_open &&
        DuplicateHandle(GetCurrentProcess(), original, GetCurrentProcess(),
                        &result.saved, 0, FALSE,
                        DUPLICATE_SAME_ACCESS) == FALSE)
    {
      result.is_dup2_ok = false;
    }
    return result;
  }
  let const original = GetStdHandle(*slot);
  result.original = original;
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

fn reopen_terminal_as_stdin() wontthrow -> bool
{
  let const terminal = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                   OPEN_EXISTING, 0, nullptr);
  if (terminal == INVALID_HANDLE_VALUE) return false;
  defer { CloseHandle(terminal); };

  return replace_descriptor(0, terminal) && shell_fd_is_a_tty(0);
}

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
  if (!slot.has_value()) return descriptor_from_fd_number(shell_fd);
  let const handle = GetStdHandle(*slot);
  return handle != nullptr ? handle : KOSH_INVALID_FD;
}

fn descriptors_refer_to_same_file(os::descriptor first,
                                  os::descriptor second) wontthrow -> bool
{
  if (first == KOSH_INVALID_FD || second == KOSH_INVALID_FD) return false;
  if (first == second) return true;

  BY_HANDLE_FILE_INFORMATION first_info{};
  BY_HANDLE_FILE_INFORMATION second_info{};
  if (GetFileInformationByHandle(first, &first_info) == 0 ||
      GetFileInformationByHandle(second, &second_info) == 0)
    return false;

  return first_info.dwVolumeSerialNumber == second_info.dwVolumeSerialNumber &&
         first_info.nFileIndexHigh == second_info.nFileIndexHigh &&
         first_info.nFileIndexLow == second_info.nFileIndexLow;
}

fn descriptor_from_fd_number(i64 fd_number) wontthrow -> os::descriptor
{
  return reinterpret_cast<os::descriptor>(
      _get_osfhandle(static_cast<int>(fd_number)));
}

fn replace_descriptor(i32 shell_fd, os::descriptor target) wontthrow -> bool
{
  const Maybe<DWORD> slot = std_handle_slot_for_shell_fd(shell_fd);
  if (target == nullptr || target == INVALID_HANDLE_VALUE) return false;

  if (!slot.has_value()) {
    HANDLE duplicate = INVALID_HANDLE_VALUE;
    if (DuplicateHandle(GetCurrentProcess(), target, GetCurrentProcess(),
                        &duplicate, 0, TRUE, DUPLICATE_SAME_ACCESS) == FALSE)
      return false;
    let const temporary_fd = _open_osfhandle(
        reinterpret_cast<intptr_t>(duplicate), O_BINARY | O_RDWR);
    if (temporary_fd == -1) {
      CloseHandle(duplicate);
      return false;
    }
    if (temporary_fd == shell_fd) {
      note_descriptor_rebound();
      note_shell_fd_opened(shell_fd);
      return true;
    }
    let const was_replaced = _dup2(temporary_fd, shell_fd) == 0;
    _close(temporary_fd);
    if (was_replaced) {
      note_descriptor_rebound();
      note_shell_fd_opened(shell_fd);
    }
    return was_replaced;
  }

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
  if (!slot.has_value()) {
    let const was_closed = _close(shell_fd) == 0;
    if (was_closed) {
      note_descriptor_rebound();
      note_shell_fd_closed(shell_fd);
    }
    return was_closed;
  }
  const os::descriptor handle = GetStdHandle(*slot);
  if (handle == nullptr || handle == INVALID_HANDLE_VALUE) return false;
  if (SetStdHandle(*slot, INVALID_HANDLE_VALUE) == FALSE) return false;

  note_descriptor_rebound();

  return standard_handle_is_referenced(handle) || CloseHandle(handle) != FALSE;
}

fn allocate_free_shell_fd(i32 floor_fd) wontthrow -> i32
{
  let const maximum_fd = _getmaxstdio();
  for (i32 shell_fd = floor_fd; shell_fd < maximum_fd; shell_fd++)
    if (descriptor_from_fd_number(shell_fd) == KOSH_INVALID_FD) return shell_fd;
  return -1;
}

fn get_current_user() -> Maybe<String>
{
  DWORD size = 0;
  GetUserNameW(nullptr, &size);
  if (GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
    ArrayList<wchar_t> buffer{heap_allocator()};
    buffer.reserve(size);
    for (DWORD i = 0; i < size; i++)
      buffer.push(L'\0');
    if (GetUserNameW(buffer.begin(), &size))
      return wide_to_utf8(buffer.begin(), size - 1, heap_allocator());
  }
  return koshka::None;
}

fn get_login_user() throws -> Maybe<String> { return get_current_user(); }

fn get_hostname() throws -> Maybe<String>
{
  wchar_t buffer[MAX_COMPUTERNAME_LENGTH + 1];
  DWORD size = countof(buffer);
  if (GetComputerNameW(buffer, &size))
    return wide_to_utf8(buffer, size, heap_allocator());
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

fn get_home_for_user(StringView username) throws -> Maybe<Path>
{
  let const current_user = get_current_user();
  if (!current_user.has_value() || current_user->count() != username.length)
    return koshka::None;

  for (usize position = 0; position < username.length; position++)
    if (utils::ascii_to_lower((*current_user)[position]) !=
        utils::ascii_to_lower(username[position]))
      return koshka::None;

  return get_home_directory();
}

fn enumerate_users() throws -> ArrayList<String>
{
  let users = ArrayList<String>{heap_allocator()};
  let current_user = get_current_user();
  if (current_user.has_value()) users.push(current_user.take());

  return users;
}

static DWORD PARENT_SHELL_PID = GetCurrentProcessId();
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

fn terminal_name(descriptor fd) throws -> Maybe<String>
{
  if (!is_fd_a_tty(fd)) return None;
  return String{"console"};
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

fn system_configuration(system_configuration_key key) wontthrow -> Maybe<i64>
{
  SYSTEM_INFO system_info{};
  GetSystemInfo(&system_info);
  switch (key) {
  case system_configuration_key::ArgMax: return 32767;
  case system_configuration_key::ChildMax: return None;
  case system_configuration_key::ClockTicks: return None;
  case system_configuration_key::GroupsMax: return None;
  case system_configuration_key::OpenMax: return _getmaxstdio();
  case system_configuration_key::PageSize:
    return static_cast<i64>(system_info.dwPageSize);
  case system_configuration_key::StreamMax: return _getmaxstdio();
  case system_configuration_key::PosixVersion: return None;
  }
  return None;
}

fn path_configuration(StringView path, path_configuration_key key) wontthrow
    -> Maybe<i64>
{
  let const path_text = utf8_to_wide(path, heap_allocator());
  if (!path_text.has_value() ||
      GetFileAttributesW(path_text->begin()) == INVALID_FILE_ATTRIBUTES)
    return None;
  switch (key) {
  case path_configuration_key::LinkMax: return 1024;
  case path_configuration_key::MaxCanonical: return None;
  case path_configuration_key::MaxInput: return None;
  case path_configuration_key::NameMax: {
    wchar_t volume_path[MAX_PATH];
    if (GetVolumePathNameW(path_text->begin(), volume_path,
                           countof(volume_path)) == FALSE)
    {
      return None;
    }
    DWORD maximum_component_length = 0;
    if (GetVolumeInformationW(volume_path, nullptr, 0, nullptr,
                              &maximum_component_length, nullptr, nullptr,
                              0) == FALSE)
    {
      return None;
    }
    return static_cast<i64>(maximum_component_length);
  }
  case path_configuration_key::PathMax: return 32767;
  case path_configuration_key::PipeBuffer: return None;
  case path_configuration_key::ChownRestricted: return 1;
  case path_configuration_key::NoTrunc: return 1;
  case path_configuration_key::DisableCharacter: return None;
  }
  return None;
}

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

fn terminal_settings(descriptor terminal, bool should_encode,
                     bool should_report_all, Allocator allocator) throws
    -> Maybe<String>
{
  DWORD mode = 0;
  if (GetConsoleMode(terminal, &mode) == FALSE) return None;
  if (should_encode) {
    char encoded[32];
    let const length = std::snprintf(encoded, sizeof(encoded), "win32:%08lx\n",
                                     static_cast<unsigned long>(mode));
    return String{
        allocator, StringView{encoded, static_cast<usize>(length)}
    };
  }
  let output = String{allocator, "speed 0 baud; "};
  if ((mode & ENABLE_ECHO_INPUT) == 0) output += '-';
  output += "echo ";
  if ((mode & ENABLE_LINE_INPUT) == 0) output += '-';
  output += "icanon ";
  if ((mode & ENABLE_PROCESSED_INPUT) == 0) output += '-';
  output += "isig";
  if (should_report_all) output += "; rows 0; columns 0";
  output += '\n';
  return output;
}

fn apply_terminal_settings(descriptor terminal,
                           const ArrayList<String> &settings) wontthrow
    -> terminal_settings_apply_result
{
  DWORD mode = 0;
  if (GetConsoleMode(terminal, &mode) == FALSE)
    return {terminal_settings_apply_kind::SystemError, 0};
  for (usize setting_position = 0; setting_position < settings.count();
       setting_position++)
  {
    let const &setting_text = settings[setting_position];
    let const setting = setting_text.view();
    if (setting.starts_with(StringView{"win32:"})) {
      let const encoded = String{heap_allocator(), setting.substring(6)};
      char *end = nullptr;
      let const parsed = std::strtoul(encoded.c_str(), &end, 16);
      if (end == encoded.c_str() || *end != '\0' || parsed > MAXDWORD) {
        return {terminal_settings_apply_kind::InvalidSetting, setting_position};
      }
      mode = static_cast<DWORD>(parsed);
      continue;
    }
    let is_disabled = false;
    let name = setting;
    if (!name.is_empty() && name[0] == '-') {
      is_disabled = true;
      name = name.substring(1);
    }
    DWORD flag = 0;
    if (name == "echo")
      flag = ENABLE_ECHO_INPUT;
    else if (name == "icanon")
      flag = ENABLE_LINE_INPUT;
    else if (name == "isig")
      flag = ENABLE_PROCESSED_INPUT;
    else
      return {terminal_settings_apply_kind::InvalidSetting, setting_position};
    if (is_disabled)
      mode &= ~flag;
    else
      mode |= flag;
  }
  if (SetConsoleMode(terminal, mode) == FALSE)
    return {terminal_settings_apply_kind::SystemError, 0};
  return {terminal_settings_apply_kind::Success, 0};
}

fn get_environment_variable(StringView key) -> Maybe<String>
{
  let const wide_key = utf8_to_wide(key, heap_allocator());
  if (!wide_key.has_value()) return None;

  wchar_t inline_buffer[256];
  SetLastError(ERROR_SUCCESS);
  let required_size =
      GetEnvironmentVariableW(wide_key->begin(), inline_buffer,
                              static_cast<DWORD>(countof(inline_buffer)));
  if (required_size == 0) {
    return GetLastError() == ERROR_ENVVAR_NOT_FOUND
               ? Maybe<String>{}
               : Maybe<String>{String{heap_allocator()}};
  }
  if (required_size < countof(inline_buffer))
    return wide_to_utf8(inline_buffer, static_cast<usize>(required_size),
                        heap_allocator());

  let buffer = ArrayList<wchar_t>{heap_allocator()};
  buffer.reserve(static_cast<usize>(required_size));
  let const value_length =
      GetEnvironmentVariableW(wide_key->begin(), buffer.begin(), required_size);
  if (value_length == 0 || value_length >= required_size) return koshka::None;
  return wide_to_utf8(buffer.begin(), static_cast<usize>(value_length),
                      heap_allocator());
}

fn set_environment_variable(StringView key, StringView value) -> void
{
  let const wide_key = utf8_to_wide(key, heap_allocator());
  let const wide_value = utf8_to_wide(value, heap_allocator());
  if (!wide_key.has_value() || !wide_value.has_value()) return;

  SetEnvironmentVariableW(wide_key->begin(), wide_value->begin());
}

fn unset_environment_variable(StringView key) -> void
{
  let const wide_key = utf8_to_wide(key, heap_allocator());
  if (!wide_key.has_value()) return;

  SetEnvironmentVariableW(wide_key->begin(), nullptr);
}

fn signal_internal_diagnostic() wontthrow -> void
{
  wchar_t marker_path[MAX_PATH];
  let const marker_path_length = GetEnvironmentVariableW(
      internal::DIAGNOSTIC_MARKER_WIDE, marker_path, countof(marker_path));
  if (marker_path_length == 0 || marker_path_length >= countof(marker_path)) {
    return;
  }

  let const marker =
      CreateFileW(marker_path, FILE_APPEND_DATA,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_TEMPORARY, nullptr);
  if (marker == INVALID_HANDLE_VALUE) return;
  DWORD written = 0;
  WriteFile(marker, "x", 1, &written, nullptr);
  CloseHandle(marker);
}

fn for_each_environment_name(opaque *context,
                             environment_name_callback callback) throws -> void
{
  wchar_t *block = GetEnvironmentStringsW();
  if (block == nullptr) return;
  defer { FreeEnvironmentStringsW(block); };

  for (wchar_t *entry = block; *entry != L'\0';) {
    usize pair_length = 0;
    while (entry[pair_length] != L'\0')
      pair_length++;

    if (entry[0] != L'=') {
      usize name_length = 0;
      while (name_length < pair_length && entry[name_length] != L'=')
        name_length++;
      let name = wide_to_utf8(entry, name_length, heap_allocator());
      if (name.has_value()) callback(context, name->view());
    }

    entry += pair_length + 1;
  }
}

fn environment_names() -> ArrayList<String>
{
  ArrayList<String> names{heap_allocator()};
  for_each_environment_name(&names, [](opaque *context, StringView name) {
    static_cast<ArrayList<String> *>(context)->push(String{name});
  });
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

static constexpr u32 SUBSHELL_TRANSPORT_MAGIC = 0x4b535442U;
static constexpr u32 SUBSHELL_TRANSPORT_VERSION = 1U;
static constexpr usize SUBSHELL_TRANSPORT_HEADER_LENGTH = 20;
static constexpr usize MAXIMUM_SUBSHELL_TRANSPORT_LENGTH = 16 * 1024 * 1024;
static subshell_bootstrap SUBSHELL_BOOTSTRAP{};

static fn read_subshell_transport_exact(descriptor pipe, opaque *output,
                                        usize length) wontthrow -> bool
{
  let bytes = static_cast<char *>(output);
  usize position = 0;
  while (position < length) {
    let const read_count = read_fd(pipe, bytes + position, length - position);
    if (!read_count.has_value() || *read_count == 0) return false;
    position += *read_count;
  }
  return true;
}

static fn decode_subshell_transport_u32(const char *bytes) wontthrow -> u32
{
  u32 value = 0;
  for (usize byte_position = 0; byte_position < sizeof(value); byte_position++)
    value |= static_cast<u32>(static_cast<u8>(bytes[byte_position]))
             << (byte_position * 8U);
  return value;
}

static fn decode_subshell_transport_u64(const char *bytes) wontthrow -> u64
{
  u64 value = 0;
  for (usize byte_position = 0; byte_position < sizeof(value); byte_position++)
    value |= static_cast<u64>(static_cast<u8>(bytes[byte_position]))
             << (byte_position * 8U);
  return value;
}

static fn receive_subshell_bootstrap() wontthrow -> void
{
  wchar_t path[256]{};
  let const path_length = GetEnvironmentVariableW(
      internal::STATE_NAMED_PIPE_WIDE, path, countof(path));
  if (path_length == 0 || path_length >= countof(path)) return;
  SetEnvironmentVariableW(internal::STATE_NAMED_PIPE_WIDE, nullptr);
  let const pipe =
      CreateNamedPipeW(path, PIPE_ACCESS_INBOUND,
                       PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1,
                       65536, 65536, 0, nullptr);
  if (pipe == INVALID_HANDLE_VALUE) ExitProcess(1);
  if (ConnectNamedPipe(pipe, nullptr) == FALSE &&
      GetLastError() != ERROR_PIPE_CONNECTED)
  {
    CloseHandle(pipe);
    ExitProcess(1);
  }
  ULONG client_process_id = 0;
  if (GetNamedPipeClientProcessId(pipe, &client_process_id) == FALSE ||
      static_cast<i64>(client_process_id) != get_parent_process_id())
  {
    CloseHandle(pipe);
    ExitProcess(1);
  }

  char header[SUBSHELL_TRANSPORT_HEADER_LENGTH];
  if (!read_subshell_transport_exact(pipe, header, sizeof(header))) {
    CloseHandle(pipe);
    ExitProcess(1);
  }
  let const magic = decode_subshell_transport_u32(header);
  let const version = decode_subshell_transport_u32(header + 4);
  let const payload_length =
      static_cast<usize>(decode_subshell_transport_u32(header + 8));
  let const source_length = decode_subshell_transport_u32(header + 12);
  let const process_count =
      static_cast<usize>(decode_subshell_transport_u32(header + 16));
  if (magic != SUBSHELL_TRANSPORT_MAGIC ||
      version != SUBSHELL_TRANSPORT_VERSION || source_length > payload_length ||
      payload_length > MAXIMUM_SUBSHELL_TRANSPORT_LENGTH ||
      process_count >
          (MAXIMUM_SUBSHELL_TRANSPORT_LENGTH - payload_length) / sizeof(u64))
  {
    CloseHandle(pipe);
    ExitProcess(1);
  }

  try {
    SUBSHELL_BOOTSTRAP.payload.reserve(payload_length);
    char buffer[4096];
    usize remaining_payload_length = payload_length;
    while (remaining_payload_length > 0) {
      let const requested_length = remaining_payload_length < sizeof(buffer)
                                       ? remaining_payload_length
                                       : sizeof(buffer);
      if (!read_subshell_transport_exact(pipe, buffer, requested_length)) {
        CloseHandle(pipe);
        ExitProcess(1);
      }
      SUBSHELL_BOOTSTRAP.payload.append(StringView{buffer, requested_length});
      remaining_payload_length -= requested_length;
    }

    SUBSHELL_BOOTSTRAP.processes.reserve(process_count);
    for (usize process_index = 0; process_index < process_count;
         process_index++)
    {
      char encoded_process[sizeof(u64)];
      if (!read_subshell_transport_exact(pipe, encoded_process,
                                         sizeof(encoded_process)))
      {
        CloseHandle(pipe);
        ExitProcess(1);
      }
      let const process_value = decode_subshell_transport_u64(encoded_process);
      SUBSHELL_BOOTSTRAP.processes.push(
          reinterpret_cast<process>(static_cast<uintptr>(process_value)));
    }
    SUBSHELL_BOOTSTRAP.source_length = source_length;
    SUBSHELL_BOOTSTRAP.owns_processes = true;
  } catch (...) {
    CloseHandle(pipe);
    ExitProcess(1);
  }

  char trailing_byte = 0;
  let const trailing_length = read_fd(pipe, &trailing_byte, 1);
  CloseHandle(pipe);
  if (!trailing_length.has_value() || *trailing_length != 0) ExitProcess(1);
}

fn initialize_platform_runtime() wontthrow -> void
{
  bool is_internal_child = false;
  try {
    let const parent_text =
        get_environment_variable(internal::PARENT_PROCESS_ID);
    if (parent_text.has_value()) {
      let const parent = parent_text->view().to<u64>();
      is_internal_child =
          !parent.is_error() &&
          parent.value() == static_cast<u64>(get_parent_process_id());
    }
  } catch (...) {}
  SetEnvironmentVariableW(internal::PARENT_PROCESS_ID_WIDE, nullptr);
  if (!is_internal_child) {
    SetEnvironmentVariableW(internal::STATE_NAMED_PIPE_WIDE, nullptr);
    SetEnvironmentVariableW(internal::CONNECT_NAMED_PIPE_WIDE, nullptr);
    unset_environment_variable(internal::PREVIOUS_EXIT_STATUS);
    unset_environment_variable(internal::SHELL_PROCESS_ID);
    unset_environment_variable(internal::SUBSHELL_DEPTH);
    return;
  }

  receive_subshell_bootstrap();
  wchar_t connection[256]{};
  let const connection_length = GetEnvironmentVariableW(
      internal::CONNECT_NAMED_PIPE_WIDE, connection, countof(connection));
  if (connection_length == 0 || connection_length >= countof(connection)) {
    return;
  }
  SetEnvironmentVariableW(internal::CONNECT_NAMED_PIPE_WIDE, nullptr);
  wchar_t *path = connection;
  while (*path != L'\0' && *path != L':') {
    path++;
  }
  if (*path != L':') return;
  *path++ = L'\0';
  let const is_input = lstrcmpW(connection, L"stdin") == 0;
  if (!is_input && lstrcmpW(connection, L"stdout") != 0) {
    return;
  }
  let const pipe = CreateNamedPipeW(
      path, is_input ? PIPE_ACCESS_INBOUND : PIPE_ACCESS_OUTBOUND,
      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 65536, 65536, 0,
      nullptr);
  if (pipe == INVALID_HANDLE_VALUE) ExitProcess(1);
  SetStdHandle(is_input ? STD_INPUT_HANDLE : STD_OUTPUT_HANDLE, pipe);
  if (ConnectNamedPipe(pipe, nullptr) == FALSE &&
      GetLastError() != ERROR_PIPE_CONNECTED)
  {
    ExitProcess(1);
  }
}

fn take_subshell_bootstrap() wontthrow -> subshell_bootstrap
{
  return steal(SUBSHELL_BOOTSTRAP);
}

} /* namespace os */
} /* namespace koshka */

fn kosh_main(int argc, char **argv) -> int;

fn wmain(int argc, wchar_t **wide_argv) -> int
{
  let narrow_arguments =
      koshka::ArrayList<koshka::String>{koshka::heap_allocator()};
  narrow_arguments.reserve(static_cast<usize>(argc));
  for (int argument_position = 0; argument_position < argc; argument_position++)
  {
    usize wide_length = 0;
    while (wide_argv[argument_position][wide_length] != L'\0')
      wide_length++;
    let utf8 = koshka::os::wide_to_utf8(wide_argv[argument_position],
                                        wide_length, koshka::heap_allocator());
    if (!utf8.has_value()) return 1;
    narrow_arguments.push(utf8.take());
  }

  let narrow_argv = koshka::ArrayList<char *>{koshka::heap_allocator()};
  narrow_argv.reserve(static_cast<usize>(argc) + 1);
  for (koshka::String &argument : narrow_arguments)
    narrow_argv.push(const_cast<char *>(argument.c_str()));
  narrow_argv.push(nullptr);
  return kosh_main(argc, narrow_argv.begin());
}
