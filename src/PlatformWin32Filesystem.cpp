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

  if (!write_all(handle, content.data, content.count())) {
    unused(close_fd(handle));
    unused(DeleteFileA(temp_path));
    return None;
  }

  if (!close_fd(handle)) {
    unused(DeleteFileA(temp_path));
    return None;
  }

  return Path{StringView{temp_path}};
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
  const String target_string{target};
  const String link_string{link_path};
  return CreateHardLinkA(link_string.c_str(), target_string.c_str(), nullptr) !=
         0;
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

  const String path_string{path};
  let const handle =
      CreateFileA(path_string.c_str(), FILE_WRITE_ATTRIBUTES,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                  OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
  if (handle == INVALID_HANDLE_VALUE) return false;

  let const did_set = SetFileTime(handle, NULL, &access_file_time,
                                  &modification_file_time) != 0;
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

fn stat_filesystem(StringView path, filesystem_status &status) wontthrow -> bool
{
  const String path_string{path};
  ULARGE_INTEGER available{};
  ULARGE_INTEGER total{};
  ULARGE_INTEGER free{};
  if (GetDiskFreeSpaceExA(path_string.c_str(), &available, &total, &free) == 0)
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
  char drives[512];
  let const length = GetLogicalDriveStringsA(sizeof(drives), drives);
  if (length == 0 || length >= sizeof(drives)) return result;
  usize position = 0;

  while (position < length && drives[position] != '\0') {
    let const drive = StringView{drives + position};
    result.push(mounted_filesystem{String{drive}, String{drive}});
    position += drive.length + 1;
  }

  return result;
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
    status.access_time = 0;
    status.access_nanoseconds = 0;
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
  status.access_time = static_cast<i64>(info.st_atime);
  status.modification_time = static_cast<i64>(info.st_mtime);
  status.change_time = status.modification_time;
  WIN32_FILE_ATTRIBUTE_DATA attribute_data{};
  if (GetFileAttributesExA(path_string.c_str(), GetFileExInfoStandard,
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

fn username_to_uid(StringView username) throws -> Maybe<u32>
{
  unused(username);
  return koshka::None;
}

fn groupname_to_gid(StringView groupname) throws -> Maybe<u32>
{
  unused(groupname);
  return koshka::None;
}

} /* namespace os */

} /* namespace koshka */
