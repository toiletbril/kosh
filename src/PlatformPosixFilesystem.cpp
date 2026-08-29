#include "Cli.hpp"
#include "Common.hpp"
#include "Debug.hpp"
#include "Errors.hpp"
#include "Eval.hpp"
#include "Platform.hpp"
#include "Trace.hpp"
#include "Utils.hpp"

#if defined __linux__
#include <mntent.h>
#elif defined __APPLE__ || defined BSD
#include <sys/mount.h>
#endif

namespace koshka {

namespace os {

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
  let const is_owner_trusted =
      directory_stat.st_uid == 0 || directory_stat.st_uid == geteuid();
  if (!is_owner_trusted) {
    LOG(Debug,
        "trust check failed because '%s' is owned by uid %d, not root "
        "or the current user",
        directory.c_str(), directory_stat.st_uid);
    return false;
  }
  if ((directory_stat.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
    LOG(Debug, "trust check failed because '%s' permits shared writes",
        directory.c_str());
    return false;
  }

  LOG(Debug, "trust check passed for '%s'", directory.c_str());
  return true;
}

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

fn set_file_owner(StringView path, i64 owner_id, i64 group_id,
                  bool should_follow_symlink) wontthrow -> bool
{
  bool did_succeed;
  int saved_errno;
  {
    const String path_string{path};
    let const owner =
        owner_id < 0 ? static_cast<uid_t>(-1) : static_cast<uid_t>(owner_id);
    let const group =
        group_id < 0 ? static_cast<gid_t>(-1) : static_cast<gid_t>(group_id);
    did_succeed = should_follow_symlink
                      ? ::chown(path_string.c_str(), owner, group) == 0
                      : ::lchown(path_string.c_str(), owner, group) == 0;
    saved_errno = errno;
  }
  errno = saved_errno;
  return did_succeed;
}

fn create_hard_link(StringView target, StringView link_path) wontthrow -> bool
{
  bool did_succeed;
  int saved_errno;
  {
    const String target_string{target};
    const String link_string{link_path};
    did_succeed = ::link(target_string.c_str(), link_string.c_str()) == 0;
    saved_errno = errno;
  }
  errno = saved_errno;
  return did_succeed;
}

fn make_fifo(StringView path, u32 mode) wontthrow -> bool
{
  bool did_succeed;
  int saved_errno;
  {
    const String path_string{path};
    did_succeed = ::mkfifo(path_string.c_str(), static_cast<mode_t>(mode)) == 0;
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

fn set_file_times(StringView path, i64 access_time, u32 access_nanoseconds,
                  i64 modification_time, u32 modification_nanoseconds) wontthrow
    -> bool
{
  bool did_succeed;
  int saved_errno;
  {
    const String path_string{path};
    const struct timespec times[2] = {
        {static_cast<time_t>(access_time),
         static_cast<long>(access_nanoseconds)      },
        {static_cast<time_t>(modification_time),
         static_cast<long>(modification_nanoseconds)}
    };
    did_succeed = ::utimensat(AT_FDCWD, path_string.c_str(), times, 0) == 0;
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

fn stat_filesystem(StringView path, filesystem_status &status) wontthrow -> bool
{
  const String path_string{path};
  struct statvfs info{};
  if (::statvfs(path_string.c_str(), &info) != 0) return false;
  status.block_size = info.f_frsize == 0 ? info.f_bsize : info.f_frsize;
  status.total_blocks = info.f_blocks;
  status.free_blocks = info.f_bfree;
  status.available_blocks = info.f_bavail;
  return true;
}

fn mounted_filesystems() throws -> ArrayList<mounted_filesystem>
{
  let result = ArrayList<mounted_filesystem>{heap_allocator()};
#if defined __linux__
  let *mount_file = setmntent("/proc/self/mounts", "r");
  if (mount_file == nullptr) mount_file = setmntent("/etc/mtab", "r");
  if (mount_file == nullptr) return result;
  defer { endmntent(mount_file); };
  mntent entry{};
  char buffer[8192];

  while (getmntent_r(mount_file, &entry, buffer, sizeof(buffer)) != nullptr)
    result.push(
        mounted_filesystem{String{entry.mnt_fsname}, String{entry.mnt_dir}});
#elif defined __APPLE__ || defined BSD
  struct statfs *entries = nullptr;
  let const entry_count = getmntinfo(&entries, MNT_NOWAIT);

  for (int index = 0; index < entry_count; index++)
    result.push(mounted_filesystem{String{entries[index].f_mntfromname},
                                   String{entries[index].f_mntonname}});
#else
  result.push(mounted_filesystem{String{"."}, String{"."}});
#endif

  return result;
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
  status.access_time = static_cast<i64>(info.st_atime);
  status.access_nanoseconds = static_cast<u32>(info.st_atim.tv_nsec);
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

} /* namespace os */

} /* namespace koshka */
