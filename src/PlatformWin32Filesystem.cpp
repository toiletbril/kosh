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

static pure fn unc_share_end(StringView path, usize position,
                             bool should_include_trailing_separator) wontthrow
    -> usize
{
  unused(Path::next_component(path, position));
  unused(Path::next_component(path, position));
  if (should_include_trailing_separator && position < path.length &&
      is_directory_separator(path[position]))
  {
    position++;
  }

  return position;
}

fn canonical_path(const Path &path) wontthrow -> Maybe<Path>
{
  let const do_resolve_direct =
      [](const Path &candidate, bool should_preserve_extended_prefix)
          wontthrow -> Maybe<Path> {
    let const wide_candidate =
        utf8_to_wide(candidate.text().view(), heap_allocator());
    if (!wide_candidate.has_value()) return koshka::None;
    let const handle = CreateFileW(
        wide_candidate->begin(), 0,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return koshka::None;
    defer
    {
      let const error = GetLastError();
      CloseHandle(handle);
      SetLastError(error);
    };

    constexpr DWORD path_format = FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;
    let const required =
        GetFinalPathNameByHandleW(handle, nullptr, 0, path_format);
    if (required == 0) return koshka::None;
    let buffer = ArrayList<wchar_t>{heap_allocator()};
    buffer.reserve(required);
    let const length = GetFinalPathNameByHandleW(handle, buffer.begin(),
                                                 required, path_format);
    if (length == 0 || length >= required) return koshka::None;
    let const resolved_text = wide_to_utf8(
        buffer.begin(), static_cast<usize>(length), heap_allocator());
    if (!resolved_text.has_value()) return koshka::None;

    let const resolved = resolved_text->view();
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
    position = unc_share_end(text, 8, false);
    resolved = Path{text.substring_of_length(0, position)};
  } else if (text.length >= 2 && is_directory_separator(text[0]) &&
             is_directory_separator(text[1]))
  {
    position = unc_share_end(text, 2, false);
    resolved = Path{text.substring_of_length(0, position)};
  } else if (text.length >= 3 && text[1] == ':' &&
             is_directory_separator(text[2]))
  {
    resolved = Path{text.substring_of_length(0, 3)};
    position = 3;
  } else {
    wchar_t initial_input[4]{};
    bool should_read_current_directory = false;
    if (text.length >= 2 && text[1] == ':') {
      initial_input[0] = static_cast<wchar_t>(text[0]);
      initial_input[1] = L':';
      initial_input[2] = L'.';
      position = 2;
    } else if (is_directory_separator(text[0])) {
      initial_input[0] = static_cast<wchar_t>(text[0]);
      position = 1;
    } else {
      should_read_current_directory = true;
    }

    let const required =
        should_read_current_directory
            ? GetCurrentDirectoryW(0, nullptr)
            : GetFullPathNameW(initial_input, 0, nullptr, nullptr);
    if (required == 0) return koshka::None;
    let initial_path = ArrayList<wchar_t>{heap_allocator()};
    initial_path.reserve(required);
    let const initial_length =
        should_read_current_directory
            ? GetCurrentDirectoryW(required, initial_path.begin())
            : GetFullPathNameW(initial_input, required, initial_path.begin(),
                               nullptr);
    if (initial_length == 0 || initial_length >= required) return koshka::None;
    let const initial_text =
        wide_to_utf8(initial_path.begin(), static_cast<usize>(initial_length),
                     heap_allocator());
    if (!initial_text.has_value()) return koshka::None;
    resolved = Path{initial_text->view()};
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

  let const wide_pattern = utf8_to_wide(pattern, heap_allocator());
  if (!wide_pattern.has_value()) return matches;
  WIN32_FIND_DATAW find_data;
  const HANDLE handle = FindFirstFileW(wide_pattern->begin(), &find_data);
  if (handle == INVALID_HANDLE_VALUE) return matches;
  defer
  {
    let const error = GetLastError();
    FindClose(handle);
    SetLastError(error);
  };

  /* FindFirstFile yields bare names, so the directory prefix is kept to rebuild
     the path. */
  usize prefix_length = 0;
  for (usize i = 0; i < pattern.length; i++)
    if (pattern[i] == '/' || pattern[i] == '\\') prefix_length = i + 1;
  const StringView prefix = pattern.substring_of_length(0, prefix_length);

  do {
    let const name = wide_to_utf8(
        find_data.cFileName, static_cast<usize>(lstrlenW(find_data.cFileName)),
        allocator);
    if (!name.has_value()) return matches;
    if (name->view() == "." || name->view() == "..") {
      continue;
    }

    let entry = String{allocator, prefix};
    entry += name->view();
    matches.push(steal(entry));
  } while (FindNextFileW(handle, &find_data) != 0);

  return matches;
}

fn directory_is_trusted_for_exec(const Path &directory) wontthrow -> bool
{
  /* PATH already grants programs in this directory execution authority. The
     POSIX writable-mode rejection has no faithful Windows equivalent because
     access is controlled by ordered ACLs rather than owner/group mode bits. */
  return directory.is_directory();
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

  wchar_t drive_current_directory[4]{static_cast<wchar_t>(path[0]), L':', L'.',
                                     L'\0'};
  let const required =
      GetFullPathNameW(drive_current_directory, 0, nullptr, nullptr);
  if (required == 0) return None;
  let buffer = ArrayList<wchar_t>{heap_allocator()};
  buffer.reserve(static_cast<usize>(required));
  let const length = GetFullPathNameW(drive_current_directory, required,
                                      buffer.begin(), nullptr);
  if (length == 0 || length >= required) return None;
  let const text = wide_to_utf8(buffer.begin(), static_cast<usize>(length),
                                heap_allocator());
  if (!text.has_value()) return None;
  let result = Path{text->view()};
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
    return unc_share_end(path, 8, true);
  }
  if (path.length >= 2 && is_directory_separator(path[0]) &&
      is_directory_separator(path[1]))
  {
    return unc_share_end(path, 2, true);
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
  let const wide_path = utf8_to_wide(path, heap_allocator());
  return wide_path.has_value() &&
         GetFileAttributesW(wide_path->begin()) != INVALID_FILE_ATTRIBUTES;
}

cold fn path_is_directory(StringView path) wontthrow -> bool
{
  let const wide_path = utf8_to_wide(path, heap_allocator());
  if (!wide_path.has_value()) return false;
  let const attributes = GetFileAttributesW(wide_path->begin());
  return attributes != INVALID_FILE_ATTRIBUTES &&
         (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

fn path_is_regular_file(StringView path) wontthrow -> bool
{
  let const wide_path = utf8_to_wide(path, heap_allocator());
  if (!wide_path.has_value()) return false;
  let const attributes = GetFileAttributesW(wide_path->begin());
  return attributes != INVALID_FILE_ATTRIBUTES &&
         (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

fn path_is_symbolic_link(StringView path) wontthrow -> bool
{
  let const wide_path = utf8_to_wide(path, heap_allocator());
  if (!wide_path.has_value()) return false;
  let const attributes = GetFileAttributesW(wide_path->begin());
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
  let const wide_path = utf8_to_wide(path, heap_allocator());
  if (!wide_path.has_value()) return None;
  WIN32_FILE_ATTRIBUTE_DATA data{};
  if (GetFileAttributesExW(wide_path->begin(), GetFileExInfoStandard, &data) ==
      0)
    return None;
  if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) return None;
  return (static_cast<u64>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
}

fn path_modification_time(StringView path) wontthrow -> Maybe<i64>
{
  let const wide_path = utf8_to_wide(path, heap_allocator());
  if (!wide_path.has_value()) return None;
  WIN32_FILE_ATTRIBUTE_DATA data{};
  if (GetFileAttributesExW(wide_path->begin(), GetFileExInfoStandard, &data) ==
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
  let const wide_first = utf8_to_wide(first, heap_allocator());
  if (!wide_first.has_value()) return false;
  let const wide_second = utf8_to_wide(second, heap_allocator());
  if (!wide_second.has_value()) return false;
  /* FILE_FLAG_BACKUP_SEMANTICS lets a directory open too. */
  let const first_handle =
      CreateFileW(wide_first->begin(), 0,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                  nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (first_handle == INVALID_HANDLE_VALUE) return false;
  let const second_handle =
      CreateFileW(wide_second->begin(), 0,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                  nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (second_handle == INVALID_HANDLE_VALUE) {
    let const error = GetLastError();
    CloseHandle(first_handle);
    SetLastError(error);
    return false;
  }
  BY_HANDLE_FILE_INFORMATION first_info{}, second_info{};
  let const both_read =
      GetFileInformationByHandle(first_handle, &first_info) != 0 &&
      GetFileInformationByHandle(second_handle, &second_info) != 0;
  let const error = both_read ? ERROR_SUCCESS : GetLastError();
  CloseHandle(first_handle);
  CloseHandle(second_handle);
  if (!both_read) {
    SetLastError(error);
    return false;
  }
  return first_info.dwVolumeSerialNumber == second_info.dwVolumeSerialNumber &&
         first_info.nFileIndexHigh == second_info.nFileIndexHigh &&
         first_info.nFileIndexLow == second_info.nFileIndexLow;
}

fn paths_match_for_history(StringView first, StringView second) wontthrow
    -> bool
{
  return first == second || paths_are_same_file(first, second);
}

fn path_is_newer_than(StringView first, StringView second) wontthrow -> bool
{
  let const wide_first = utf8_to_wide(first, heap_allocator());
  if (!wide_first.has_value()) return false;
  let const wide_second = utf8_to_wide(second, heap_allocator());
  if (!wide_second.has_value()) return false;
  WIN32_FILE_ATTRIBUTE_DATA first_data{}, second_data{};
  if (GetFileAttributesExW(wide_first->begin(), GetFileExInfoStandard,
                           &first_data) == 0)
    return false;
  if (GetFileAttributesExW(wide_second->begin(), GetFileExInfoStandard,
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
  let const wide_path = utf8_to_wide(path, heap_allocator());
  return wide_path.has_value() && _waccess(wide_path->begin(), 4) == 0;
}

fn path_is_writable(StringView path) wontthrow -> bool
{
  let const wide_path = utf8_to_wide(path, heap_allocator());
  return wide_path.has_value() && _waccess(wide_path->begin(), 2) == 0;
}

fn path_is_executable(StringView path) wontthrow -> bool
{
  /* Windows has no execute permission bit, so an existing file is runnable. */
  return path_is_regular_file(path);
}

cold fn read_current_directory() throws -> Path
{
  let const required = GetCurrentDirectoryW(0, nullptr);
  if (required == 0) return Path{};
  let buffer = ArrayList<wchar_t>{heap_allocator()};
  buffer.reserve(required);
  let const length = GetCurrentDirectoryW(required, buffer.begin());
  if (length == 0 || length >= required) return Path{};
  let const path = wide_to_utf8(buffer.begin(), static_cast<usize>(length),
                                heap_allocator());
  if (!path.has_value()) return Path{};

  return Path{path->view()};
}

fn change_current_directory(StringView path) throws -> ErrorOr<Ok>
{
  const String path_string{path};
  let const wide_path = utf8_to_wide(path, heap_allocator());
  if (!wide_path.has_value() || SetCurrentDirectoryW(wide_path->begin()) == 0)
    return Error{"Could not change directory to '" + path_string +
                 "': " + os::last_system_error_message()};

  return Success;
}

fn reference_current_directory() wontthrow -> DirectoryReference
{
  return DirectoryReference{CreateFileW(
      L".", 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr)};
}

fn restore_current_directory(const DirectoryReference &reference) wontthrow
    -> bool
{
  if (!reference.is_valid()) return false;

  constexpr DWORD path_format = FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;
  let const required =
      GetFinalPathNameByHandleW(reference.get(), nullptr, 0, path_format);
  if (required == 0) return false;
  let path = ArrayList<wchar_t>{heap_allocator()};
  path.reserve(required);
  let const length = GetFinalPathNameByHandleW(reference.get(), path.begin(),
                                               required, path_format);
  if (length == 0 || length >= required) return false;
  if (length >= 8 &&
      std::memcmp(path.begin(), L"\\\\?\\UNC\\", 8 * sizeof(wchar_t)) == 0)
  {
    std::memmove(path.begin() + 2, path.begin() + 8,
                 (length - 7) * sizeof(wchar_t));
    path.begin()[0] = L'\\';
    path.begin()[1] = L'\\';
    return SetCurrentDirectoryW(path.begin()) != 0;
  }
  let const *restored_path =
      length >= 4 &&
              std::memcmp(path.begin(), L"\\\\?\\", 4 * sizeof(wchar_t)) == 0
          ? path.begin() + 4
          : path.begin();
  return SetCurrentDirectoryW(restored_path) != 0;
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

  let const wide_pattern = utf8_to_wide(pattern.view(), heap_allocator());
  if (!wide_pattern.has_value()) return None;
  WIN32_FIND_DATAW data{};
  let const handle = FindFirstFileW(wide_pattern->begin(), &data);
  if (handle == INVALID_HANDLE_VALUE) return None;
  defer
  {
    let const error = GetLastError();
    FindClose(handle);
    SetLastError(error);
  };

  let entries = ArrayList<Path::directory_child>{heap_allocator()};
  do {
    let name = wide_to_utf8(data.cFileName,
                            static_cast<usize>(lstrlenW(data.cFileName)),
                            heap_allocator());
    if (!name.has_value()) return None;
    if (name->view() == StringView{"."} || name->view() == StringView{".."})
      continue;

    Path::entry_kind kind = Path::entry_kind::Regular;
    if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
      kind = Path::entry_kind::Symlink;
    else if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
      kind = Path::entry_kind::Directory;
    entries.push(Path::directory_child{name.take(), kind});
  } while (FindNextFileW(handle, &data) != 0);
  return entries;
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
  if (path.starts_with(StringView{"\\\\.\\pipe\\"}))
    disposition = OPEN_EXISTING;

  /* Non-inheritable, execute_program flips it only while spawning the child. */
  SECURITY_ATTRIBUTES att{};
  att.nLength = sizeof(SECURITY_ATTRIBUTES);
  att.bInheritHandle = FALSE;
  att.lpSecurityDescriptor = nullptr; /* NOLINT */

  let const path_text =
      path == StringView{"/dev/null"} ? StringView{"NUL"} : path;
  let const wide_path = utf8_to_wide(path_text, heap_allocator());
  if (!wide_path.has_value()) return koshka::None;
  HANDLE handle = CreateFileW(wide_path->begin(), access,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, &att,
                              disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return koshka::None;

  return handle;
}

fn acquire_process_lock(StringView path) throws -> Maybe<descriptor>
{
  let const wide_path = utf8_to_wide(path, heap_allocator());
  if (!wide_path.has_value()) return None;
  constexpr wchar_t LOCK_NAME[] = L".kosh-flock.lock";
  constexpr usize LOCK_NAME_LENGTH = countof(LOCK_NAME) - 1;
  let const required =
      GetFullPathNameW(wide_path->begin(), 0, nullptr, nullptr);
  if (required == 0) return None;
  let absolute_path = ArrayList<wchar_t>{heap_allocator()};
  absolute_path.reserve(static_cast<usize>(required) + LOCK_NAME_LENGTH + 1);
  let length = GetFullPathNameW(wide_path->begin(), required,
                                absolute_path.begin(), nullptr);
  if (length == 0 || length >= required) return None;
  if (absolute_path.begin()[length - 1] != L'\\' &&
      absolute_path.begin()[length - 1] != L'/')
  {
    absolute_path.begin()[length++] = L'\\';
  }
  std::memcpy(absolute_path.begin() + length, LOCK_NAME, sizeof(LOCK_NAME));
  SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
  loop
  {
    let const lock = CreateFileW(
        absolute_path.begin(), GENERIC_READ | GENERIC_WRITE, 0, &attributes,
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
  wchar_t temp_dir[MAX_PATH];
  let const temp_directory_length = GetTempPathW(MAX_PATH, temp_dir);
  if (temp_directory_length == 0 || temp_directory_length >= MAX_PATH) {
    return koshka::None;
  }

  wchar_t temp_path[MAX_PATH];
  if (GetTempFileNameW(temp_dir, L"kos", 0, temp_path) == 0)
    return koshka::None;

  HANDLE handle = CreateFileW(
      temp_path, GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
      FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    let const error = GetLastError();
    DeleteFileW(temp_path);
    SetLastError(error);
    return koshka::None;
  }

  if (!write_all(handle, content.data, content.count())) {
    close_fd(handle);
    return koshka::None;
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

  let const wide_directory =
      utf8_to_wide(directory.text().view(), heap_allocator());
  if (!wide_directory.has_value()) return None;
  let const wide_prefix = utf8_to_wide(prefix, heap_allocator());
  if (!wide_prefix.has_value()) return None;
  wchar_t prefix_text[4]{L'x', L'x', L'x', L'\0'};
  let const prefix_length = static_cast<usize>(lstrlenW(wide_prefix->begin()));
  std::memcpy(prefix_text, wide_prefix->begin(),
              (prefix_length < 3 ? prefix_length : 3) * sizeof(wchar_t));

  wchar_t temp_path[MAX_PATH];
  HANDLE handle = INVALID_HANDLE_VALUE;
  for (u32 attempt = 1; attempt <= 256; attempt++) {
    let unique = static_cast<UINT>(
        (GetCurrentProcessId() ^ GetTickCount() ^ attempt) & 0xffffU);
    if (unique == 0) unique = 1;
    if (GetTempFileNameW(wide_directory->begin(), prefix_text, unique,
                         temp_path) == 0)
    {
      continue;
    }
    handle = CreateFileW(temp_path, GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                         FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (handle != INVALID_HANDLE_VALUE) break;
  }
  if (handle == INVALID_HANDLE_VALUE) return None;

  if (!write_all(handle, content.data, content.count())) {
    let const error = GetLastError();
    unused(close_fd(handle));
    unused(DeleteFileW(temp_path));
    SetLastError(error);
    return None;
  }

  if (!close_fd(handle)) {
    let const error = GetLastError();
    unused(DeleteFileW(temp_path));
    SetLastError(error);
    return None;
  }

  let const path = wide_to_utf8(
      temp_path, static_cast<usize>(lstrlenW(temp_path)), heap_allocator());
  if (!path.has_value()) {
    let const error = GetLastError();
    unused(DeleteFileW(temp_path));
    SetLastError(error);
    return None;
  }

  return Path{path->view()};
}

fn make_directory(StringView path, u32 mode) wontthrow -> bool
{
  unused(mode);
  let const wide_path = utf8_to_wide(path, heap_allocator());
  return wide_path.has_value() &&
         CreateDirectoryW(wide_path->begin(), nullptr) != 0;
}

fn set_file_mode(StringView path, u32 mode) wontthrow -> bool
{
  let const owner_is_writable = (mode & 0200u) != 0;
  let const wide_path = utf8_to_wide(path, heap_allocator());
  if (!wide_path.has_value()) return false;
  let attributes = GetFileAttributesW(wide_path->begin());
  if (attributes == INVALID_FILE_ATTRIBUTES) return false;
  if (owner_is_writable)
    attributes &= ~FILE_ATTRIBUTE_READONLY;
  else
    attributes |= FILE_ATTRIBUTE_READONLY;
  return SetFileAttributesW(wide_path->begin(), attributes) != FALSE;
}

fn set_file_owner(StringView path, i64 owner_id, i64 group_id,
                  bool should_follow_symlink) wontthrow -> bool
{
  unused(path);
  unused(owner_id);
  unused(group_id);
  unused(should_follow_symlink);
  SetLastError(ERROR_NOT_SUPPORTED);
  return false;
}

fn create_hard_link(StringView target, StringView link_path) wontthrow -> bool
{
  let const wide_target = utf8_to_wide(target, heap_allocator());
  if (!wide_target.has_value()) return false;
  let const wide_link_path = utf8_to_wide(link_path, heap_allocator());
  return wide_link_path.has_value() &&
         CreateHardLinkW(wide_link_path->begin(), wide_target->begin(),
                         nullptr) != 0;
}

fn make_fifo(StringView path, u32 mode) wontthrow -> bool
{
  unused(path);
  unused(mode);
  SetLastError(ERROR_NOT_SUPPORTED);
  return false;
}

fn touch_file_times(StringView path) wontthrow -> bool
{
  let const wide_path = utf8_to_wide(path, heap_allocator());
  if (!wide_path.has_value()) return false;
  HANDLE handle =
      CreateFileW(wide_path->begin(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ,
                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return false;

  FILETIME now;
  GetSystemTimeAsFileTime(&now);
  let const did_set = SetFileTime(handle, nullptr, &now, &now) != 0;
  let const error = did_set ? ERROR_SUCCESS : GetLastError();
  CloseHandle(handle);
  if (!did_set) SetLastError(error);

  return did_set;
}

static fn unix_time_to_file_time(i64 seconds, u32 nanoseconds,
                                 FILETIME &file_time) wontthrow -> bool
{
  constexpr i64 WINDOWS_TO_UNIX_EPOCH_SECONDS = 11644473600LL;
  let const windows_seconds = seconds + WINDOWS_TO_UNIX_EPOCH_SECONDS;
  if (windows_seconds < 0) return false;

  ULARGE_INTEGER ticks{};
  ticks.QuadPart =
      static_cast<u64>(windows_seconds) * 10000000ULL + nanoseconds / 100ULL;
  file_time.dwLowDateTime = ticks.LowPart;
  file_time.dwHighDateTime = ticks.HighPart;
  return true;
}

fn set_file_times(StringView path, i64 access_time, u32 access_nanoseconds,
                  i64 modification_time, u32 modification_nanoseconds) wontthrow
    -> bool
{
  FILETIME access_file_time{};
  FILETIME modification_file_time{};
  if (!unix_time_to_file_time(access_time, access_nanoseconds,
                              access_file_time) ||
      !unix_time_to_file_time(modification_time, modification_nanoseconds,
                              modification_file_time))
  {
    SetLastError(ERROR_INVALID_PARAMETER);
    return false;
  }

  let const wide_path = utf8_to_wide(path, heap_allocator());
  if (!wide_path.has_value()) return false;
  let const handle =
      CreateFileW(wide_path->begin(), FILE_WRITE_ATTRIBUTES,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                  nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return false;

  let const did_set = SetFileTime(handle, nullptr, &access_file_time,
                                  &modification_file_time) != 0;
  let const error = did_set ? ERROR_SUCCESS : GetLastError();
  CloseHandle(handle);
  if (!did_set) SetLastError(error);

  return did_set;
}

fn remove_directory(StringView path) wontthrow -> bool
{
  let const wide_path = utf8_to_wide(path, heap_allocator());
  return wide_path.has_value() && RemoveDirectoryW(wide_path->begin()) != 0;
}

fn remove_file(StringView path) wontthrow -> bool
{
  let const wide_path = utf8_to_wide(path, heap_allocator());
  return wide_path.has_value() && DeleteFileW(wide_path->begin()) != 0;
}

fn rename_path(StringView from, StringView to) wontthrow -> bool
{
  let const wide_from = utf8_to_wide(from, heap_allocator());
  if (!wide_from.has_value()) return false;
  let const wide_to = utf8_to_wide(to, heap_allocator());
  return wide_to.has_value() &&
         MoveFileExW(wide_from->begin(), wide_to->begin(),
                     MOVEFILE_REPLACE_EXISTING) != 0;
}

fn create_symlink(StringView target, StringView link_path) wontthrow -> bool
{
/* An older mingw SDK omits these flags, defined here when absent. */
#ifndef SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE
#define SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE 0x2
#endif
#ifndef SYMBOLIC_LINK_FLAG_DIRECTORY
#define SYMBOLIC_LINK_FLAG_DIRECTORY 0x1
#endif
  let const wide_target = utf8_to_wide(target, heap_allocator());
  if (!wide_target.has_value()) return false;
  let const wide_link_path = utf8_to_wide(link_path, heap_allocator());
  if (!wide_link_path.has_value()) return false;

  /* A directory target needs the directory flag, the unprivileged flag avoids
     elevation on developer-mode Windows. */
  DWORD flags = SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE;
  const DWORD attributes = GetFileAttributesW(wide_target->begin());
  if (attributes != INVALID_FILE_ATTRIBUTES &&
      (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
    flags |= SYMBOLIC_LINK_FLAG_DIRECTORY;
  return CreateSymbolicLinkW(wide_link_path->begin(), wide_target->begin(),
                             flags) != 0;
}

fn read_symlink(StringView path, Allocator allocator) wontthrow -> Maybe<String>
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

  let const wide_path = utf8_to_wide(path, heap_allocator());
  if (!wide_path.has_value()) return koshka::None;
  let const handle = CreateFileW(
      wide_path->begin(), 0,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
      nullptr);
  if (handle == INVALID_HANDLE_VALUE) return koshka::None;
  defer
  {
    let const error = GetLastError();
    CloseHandle(handle);
    SetLastError(error);
  };

  alignas(void *) u8 buffer[MAXIMUM_REPARSE_DATA_BUFFER_SIZE];
  DWORD bytes_returned = 0;
  if (DeviceIoControl(handle, FSCTL_GET_REPARSE_POINT, nullptr, 0, buffer,
                      sizeof(buffer), &bytes_returned, nullptr) == FALSE)
  {
    return koshka::None;
  }

  const WCHAR *wide_target = nullptr;
  usize wide_target_length = 0;
  bool is_substitute_name = false;
  let const do_select_target =
      [&](usize path_buffer_position, u16 data_length,
          u16 substitute_name_offset, u16 substitute_name_length,
          u16 print_name_offset, u16 print_name_length) -> bool {
    constexpr usize reparse_header_length = sizeof(u32) + sizeof(u16) * 2;
    if (path_buffer_position < reparse_header_length ||
        bytes_returned < path_buffer_position)
    {
      return false;
    }
    let const fixed_data_length = path_buffer_position - reparse_header_length;
    if (data_length < fixed_data_length ||
        reparse_header_length + data_length > bytes_returned)
    {
      return false;
    }
    let const offset =
        print_name_length > 0 ? print_name_offset : substitute_name_offset;
    let const length =
        print_name_length > 0 ? print_name_length : substitute_name_length;
    let const path_buffer_length = data_length - fixed_data_length;
    if ((offset % sizeof(WCHAR)) != 0 || (length % sizeof(WCHAR)) != 0 ||
        offset > path_buffer_length || length > path_buffer_length - offset)
    {
      return false;
    }
    wide_target =
        reinterpret_cast<const WCHAR *>(buffer + path_buffer_position + offset);
    wide_target_length = length / sizeof(WCHAR);
    is_substitute_name = print_name_length == 0;
    return true;
  };
  if (bytes_returned < sizeof(u32)) {
    SetLastError(ERROR_INVALID_DATA);
    return koshka::None;
  }
  let const tag = *reinterpret_cast<const u32 *>(buffer);
  if (tag == IO_REPARSE_TAG_SYMLINK) {
    constexpr usize path_buffer_position =
        offsetof(symbolic_link_reparse_data, path_buffer);
    if (bytes_returned < path_buffer_position) {
      SetLastError(ERROR_INVALID_DATA);
      return koshka::None;
    }
    let const *data =
        reinterpret_cast<const symbolic_link_reparse_data *>(buffer);
    if (!do_select_target(path_buffer_position, data->data_length,
                          data->substitute_name_offset,
                          data->substitute_name_length, data->print_name_offset,
                          data->print_name_length))
    {
      SetLastError(ERROR_INVALID_DATA);
      return koshka::None;
    }
  } else if (tag == IO_REPARSE_TAG_MOUNT_POINT) {
    constexpr usize path_buffer_position =
        offsetof(mount_point_reparse_data, path_buffer);
    if (bytes_returned < path_buffer_position) {
      SetLastError(ERROR_INVALID_DATA);
      return koshka::None;
    }
    let const *data =
        reinterpret_cast<const mount_point_reparse_data *>(buffer);
    if (!do_select_target(path_buffer_position, data->data_length,
                          data->substitute_name_offset,
                          data->substitute_name_length, data->print_name_offset,
                          data->print_name_length))
    {
      SetLastError(ERROR_INVALID_DATA);
      return koshka::None;
    }
  } else {
    SetLastError(ERROR_NOT_SUPPORTED);
    return koshka::None;
  }

  constexpr WCHAR UNC_NAMESPACE_PREFIX[] = L"\\??\\UNC\\";
  constexpr WCHAR NAMESPACE_PREFIX[] = L"\\??\\";
  bool is_unc_target = false;
  if (is_substitute_name && wide_target_length >= 8 &&
      std::memcmp(wide_target, UNC_NAMESPACE_PREFIX, 8 * sizeof(WCHAR)) == 0)
  {
    wide_target += 8;
    wide_target_length -= 8;
    is_unc_target = true;
  } else if (is_substitute_name && wide_target_length >= 4 &&
             std::memcmp(wide_target, NAMESPACE_PREFIX, 4 * sizeof(WCHAR)) == 0)
  {
    wide_target += 4;
    wide_target_length -= 4;
  }
  let target = wide_to_utf8(wide_target, wide_target_length, allocator);
  if (!target.has_value() || !is_unc_target) return target;
  let unc_target = String{allocator, "\\\\"};
  unc_target += target->view();
  return unc_target;
}

fn stat_filesystem(StringView path, filesystem_status &status) wontthrow -> bool
{
  let const wide_path = utf8_to_wide(path, heap_allocator());
  if (!wide_path.has_value()) return false;
  ULARGE_INTEGER available{};
  ULARGE_INTEGER total{};
  ULARGE_INTEGER free{};
  if (GetDiskFreeSpaceExW(wide_path->begin(), &available, &total, &free) == 0)
    return false;
  constexpr u64 block_size = 512;
  status.block_size = block_size;
  status.total_blocks = total.QuadPart / block_size;
  status.free_blocks = free.QuadPart / block_size;
  status.available_blocks = available.QuadPart / block_size;
  return true;
}

fn mounted_filesystems() throws -> ArrayList<mounted_filesystem>
{
  let result = ArrayList<mounted_filesystem>{heap_allocator()};
  wchar_t drives[512];
  let const length = GetLogicalDriveStringsW(countof(drives), drives);
  if (length == 0 || length >= countof(drives)) return result;
  usize position = 0;

  while (position < length && drives[position] != L'\0') {
    let const drive_length = static_cast<usize>(lstrlenW(drives + position));
    let drive = wide_to_utf8(drives + position, drive_length, heap_allocator());
    if (!drive.has_value()) return result;
    let target = drive.take();
    result.push(mounted_filesystem{target.clone(), steal(target)});
    position += drive_length + 1;
  }

  return result;
}

fn current_executable_path() wontthrow -> Maybe<String>
{
  ArrayList<wchar_t> module_path{heap_allocator()};
  DWORD capacity = MAX_PATH;

  loop
  {
    module_path.reserve(capacity);
    let const module_path_length =
        GetModuleFileNameW(nullptr, module_path.begin(), capacity);
    if (module_path_length == 0) return None;
    if (module_path_length < capacity)
      return wide_to_utf8(module_path.begin(), module_path_length,
                          heap_allocator());
    if (capacity >= 32768) return None;
    capacity = capacity > 16384 ? 32768 : capacity * 2;
  }
}

fn stat_path(StringView path, file_status &status) wontthrow -> bool
{
  if (path == StringView{"/dev/null"}) {
    status = {};
    status.mode = 0020666u;
    status.link_count = 1;
    return true;
  }

  let const wide_path = utf8_to_wide(path, heap_allocator());
  if (!wide_path.has_value()) return false;
  let const attributes = GetFileAttributesW(wide_path->begin());
  if (attributes == INVALID_FILE_ATTRIBUTES) return false;
  if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
    let const target = read_symlink(path, heap_allocator());
    if (!target.has_value()) return false;

    status.device_id = 0;
    status.file_id = 0;
    status.has_file_identity = false;
    status.mode = 0120000u | 0777u;
    status.link_count = 1;
    status.owner_id = 0;
    status.group_id = 0;
    status.size = target->length();
    status.access_time = 0;
    status.access_nanoseconds = 0;
    status.modification_time = 0;
    status.modification_nanoseconds = 0;
    status.change_time = 0;
    status.change_nanoseconds = 0;
    status.blocks = (status.size + 511) / 512;

    let const handle = CreateFileW(
        wide_path->begin(), 0,
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
        ULARGE_INTEGER access_ticks{};
        access_ticks.LowPart = identity.ftLastAccessTime.dwLowDateTime;
        access_ticks.HighPart = identity.ftLastAccessTime.dwHighDateTime;
        status.access_time = static_cast<i64>(
            access_ticks.QuadPart / 10000000ULL - 11644473600ULL);
        status.access_nanoseconds =
            static_cast<u32>(access_ticks.QuadPart % 10000000ULL * 100ULL);
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

  struct _stat64 info{};
  if (_wstat64(wide_path->begin(), &info) != 0) return false;
  status.device_id = 0;
  status.file_id = 0;
  status.has_file_identity = false;
  let const handle =
      CreateFileW(wide_path->begin(), 0,
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
  status.access_time = static_cast<i64>(info.st_atime);
  status.modification_time = static_cast<i64>(info.st_mtime);
  status.change_time = status.modification_time;
  WIN32_FILE_ATTRIBUTE_DATA attribute_data{};
  if (GetFileAttributesExW(wide_path->begin(), GetFileExInfoStandard,
                           &attribute_data) != 0)
  {
    ULARGE_INTEGER access_ticks{};
    access_ticks.LowPart = attribute_data.ftLastAccessTime.dwLowDateTime;
    access_ticks.HighPart = attribute_data.ftLastAccessTime.dwHighDateTime;
    status.access_nanoseconds =
        static_cast<u32>(access_ticks.QuadPart % 10000000ULL * 100ULL);
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
  let const resolved = canonical_path(Path{path});
  return resolved.has_value() && stat_path(resolved->text().view(), status);
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
  if (uid != 0) return None;
  return process_owner_name(static_cast<u32>(get_current_process_id()), uid,
                            heap_allocator());
}

fn gid_to_groupname(u32 gid) throws -> Maybe<String>
{
  unused(gid);
  return koshka::None;
}

fn username_to_uid(StringView username) throws -> Maybe<u32>
{
  let const current = get_current_user();
  if (current.has_value() && current->view() == username) return 0;
  return None;
}

fn groupname_to_gid(StringView groupname) throws -> Maybe<u32>
{
  unused(groupname);
  return koshka::None;
}

} /* namespace os */

} /* namespace koshka */
