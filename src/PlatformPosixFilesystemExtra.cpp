/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This routed POSIX source fragment implements filesystem operations whose
 * native interfaces differ across Linux, macOS, and BSD targets. It owns bulk
 * directory metadata, filesystem identity and counters, mount discovery,
 * platform sync behavior, and native batched I/O.
 */

#if defined __linux__
#include <linux/io_uring.h>
#include <mntent.h>
#include <sys/mman.h>
#elif defined __APPLE__
#include <aio.h>
#include <sys/attr.h>
#include <sys/event.h>
#include <sys/fsgetpath.h>
#include <sys/mount.h>
#include <sys/vnode.h>
#include <uuid/uuid.h>
#elif defined BSD
#include <sys/mount.h>
#endif

namespace koshka {
namespace os {

#if defined __APPLE__

template <class Value>
static fn read_bulk_attribute(const char *&field, const char *end,
                              Value &value) wontthrow -> bool
{
  if (static_cast<usize>(end - field) < sizeof(Value)) return false;
  __builtin_memcpy(&value, field, sizeof(Value));
  field += sizeof(Value);
  return true;
}

template <class Value>
static fn read_returned_bulk_attribute(attrgroup_t returned,
                                       attrgroup_t attribute,
                                       const char *&field, const char *end,
                                       Value &value) wontthrow -> bool
{
  if ((returned & attribute) == 0) return true;
  return read_bulk_attribute(field, end, value);
}

static fn mode_from_vnode_type(fsobj_type_t type) wontthrow -> u32
{
  switch (type) {
  case VREG: return S_IFREG;
  case VDIR: return S_IFDIR;
  case VBLK: return S_IFBLK;
  case VCHR: return S_IFCHR;
  case VLNK: return S_IFLNK;
  case VSOCK: return S_IFSOCK;
  case VFIFO: return S_IFIFO;
  default: return 0;
  }
}

static fn directory_kind_from_vnode_type(fsobj_type_t type) wontthrow
    -> Path::entry_kind
{
  switch (type) {
  case VDIR: return Path::entry_kind::Directory;
  case VREG: return Path::entry_kind::Regular;
  case VLNK: return Path::entry_kind::Symlink;
  case VNON: return Path::entry_kind::Unknown;
  default: return Path::entry_kind::Other;
  }
}

static fn fill_bulk_file_status(
    const attribute_set_t &returned, fsobj_type_t type, dev_t device,
    const timespec &modification_time, const timespec &change_time,
    const timespec &access_time, uid_t owner, gid_t group, u32 access_mode,
    u32 flags, u64 file_id, u32 directory_link_count,
    off_t directory_allocated_size, off_t directory_size, u32 file_link_count,
    off_t file_size, off_t file_allocated_size, u32 special_device,
    file_status &status) wontthrow -> bool
{
  constexpr attrgroup_t REQUIRED_COMMON =
      ATTR_CMN_DEVID | ATTR_CMN_OBJTYPE | ATTR_CMN_MODTIME | ATTR_CMN_CHGTIME |
      ATTR_CMN_ACCTIME | ATTR_CMN_OWNERID | ATTR_CMN_GRPID |
      ATTR_CMN_ACCESSMASK | ATTR_CMN_FLAGS | ATTR_CMN_FILEID;
  if ((returned.commonattr & REQUIRED_COMMON) != REQUIRED_COMMON) return false;

  status.device_id = static_cast<u64>(device);
  status.file_id = file_id;
  status.mode = mode_from_vnode_type(type) | (access_mode & ~S_IFMT);
  status.owner_id = static_cast<u32>(owner);
  status.group_id = static_cast<u32>(group);
  status.access_time = access_time.tv_sec;
  status.access_nanoseconds = static_cast<u32>(access_time.tv_nsec);
  status.modification_time = modification_time.tv_sec;
  status.modification_nanoseconds = static_cast<u32>(modification_time.tv_nsec);
  status.change_time = change_time.tv_sec;
  status.change_nanoseconds = static_cast<u32>(change_time.tv_nsec);
  status.has_file_identity = true;

  if (type == VDIR) {
    constexpr attrgroup_t REQUIRED_DIRECTORY =
        ATTR_DIR_LINKCOUNT | ATTR_DIR_ALLOCSIZE | ATTR_DIR_DATALENGTH;
    if ((returned.dirattr & REQUIRED_DIRECTORY) != REQUIRED_DIRECTORY)
      return false;
    status.link_count = directory_link_count;
    status.size = static_cast<u64>(directory_size);
    status.blocks = static_cast<u64>(directory_allocated_size + 511) / 512;
  } else {
    constexpr attrgroup_t REQUIRED_FILE =
        ATTR_FILE_LINKCOUNT | ATTR_FILE_TOTALSIZE | ATTR_FILE_ALLOCSIZE |
        ATTR_FILE_DEVTYPE;
    if ((returned.fileattr & REQUIRED_FILE) != REQUIRED_FILE) return false;
    status.link_count = file_link_count;
    status.size = static_cast<u64>(file_size);
    status.blocks = static_cast<u64>(file_allocated_size + 511) / 512;
    status.special_device_id = special_device;
  }

  return (flags & SF_FIRMLINK) == 0;
}

static fn list_directory_status_bulk(StringView dir, Allocator allocator) throws
    -> Maybe<ArrayList<directory_status_entry>>
{
  const String dir_string{dir};
  let const directory_descriptor = ::open(dir_string.c_str(), O_RDONLY);
  if (directory_descriptor < 0) return None;
  defer { ::close(directory_descriptor); };

  struct attrlist attributes{};
  attributes.bitmapcount = ATTR_BIT_MAP_COUNT;
  attributes.commonattr =
      ATTR_CMN_RETURNED_ATTRS | ATTR_CMN_NAME | ATTR_CMN_DEVID |
      ATTR_CMN_OBJTYPE | ATTR_CMN_MODTIME | ATTR_CMN_CHGTIME |
      ATTR_CMN_ACCTIME | ATTR_CMN_OWNERID | ATTR_CMN_GRPID |
      ATTR_CMN_ACCESSMASK | ATTR_CMN_FLAGS | ATTR_CMN_FILEID | ATTR_CMN_ERROR;
  attributes.dirattr = ATTR_DIR_LINKCOUNT | ATTR_DIR_MOUNTSTATUS |
                       ATTR_DIR_ALLOCSIZE | ATTR_DIR_DATALENGTH;
  attributes.fileattr = ATTR_FILE_LINKCOUNT | ATTR_FILE_TOTALSIZE |
                        ATTR_FILE_ALLOCSIZE | ATTR_FILE_DEVTYPE;

  constexpr usize BUFFER_BYTE_COUNT = 64 * 1024;
  alignas(8) char buffer[BUFFER_BYTE_COUNT];
  let entries = ArrayList<directory_status_entry>{allocator};
  loop
  {
    let const entry_count = ::getattrlistbulk(directory_descriptor, &attributes,
                                              buffer, sizeof(buffer), 0);
    if (entry_count < 0) return None;
    if (entry_count == 0) return entries;

    const char *record = buffer;
    for (i32 index = 0; index < entry_count; index++) {
      u32 record_byte_count = 0;
      __builtin_memcpy(&record_byte_count, record, sizeof(record_byte_count));
      if (record_byte_count < sizeof(record_byte_count) ||
          record_byte_count >
              static_cast<usize>(buffer + sizeof(buffer) - record))
      {
        return None;
      }

      let const end = record + record_byte_count;
      let field = record + sizeof(record_byte_count);
      attribute_set_t returned{};
      u32 error_number = 0;
      attrreference_t name_reference{};
      const char *name_reference_field = nullptr;
      dev_t device = 0;
      fsobj_type_t type = VNON;
      timespec modification_time{};
      timespec change_time{};
      timespec access_time{};
      uid_t owner = 0;
      gid_t group = 0;
      u32 access_mode = 0;
      u32 flags = 0;
      u64 file_id = 0;
      u32 directory_link_count = 0;
      u32 directory_mount_status = 0;
      off_t directory_allocated_size = 0;
      off_t directory_size = 0;
      u32 file_link_count = 0;
      off_t file_size = 0;
      off_t file_allocated_size = 0;
      u32 special_device = 0;

      if (!read_bulk_attribute(field, end, returned) ||
          !read_returned_bulk_attribute(returned.commonattr, ATTR_CMN_ERROR,
                                        field, end, error_number))
      {
        return None;
      }
      name_reference_field = field;
      if ((returned.commonattr & ATTR_CMN_NAME) == 0 ||
          !read_bulk_attribute(field, end, name_reference) ||
          !read_returned_bulk_attribute(returned.commonattr, ATTR_CMN_DEVID,
                                        field, end, device) ||
          !read_returned_bulk_attribute(returned.commonattr, ATTR_CMN_OBJTYPE,
                                        field, end, type) ||
          !read_returned_bulk_attribute(returned.commonattr, ATTR_CMN_MODTIME,
                                        field, end, modification_time) ||
          !read_returned_bulk_attribute(returned.commonattr, ATTR_CMN_CHGTIME,
                                        field, end, change_time) ||
          !read_returned_bulk_attribute(returned.commonattr, ATTR_CMN_ACCTIME,
                                        field, end, access_time) ||
          !read_returned_bulk_attribute(returned.commonattr, ATTR_CMN_OWNERID,
                                        field, end, owner) ||
          !read_returned_bulk_attribute(returned.commonattr, ATTR_CMN_GRPID,
                                        field, end, group) ||
          !read_returned_bulk_attribute(returned.commonattr,
                                        ATTR_CMN_ACCESSMASK, field, end,
                                        access_mode) ||
          !read_returned_bulk_attribute(returned.commonattr, ATTR_CMN_FLAGS,
                                        field, end, flags) ||
          !read_returned_bulk_attribute(returned.commonattr, ATTR_CMN_FILEID,
                                        field, end, file_id) ||
          !read_returned_bulk_attribute(returned.dirattr, ATTR_DIR_LINKCOUNT,
                                        field, end, directory_link_count) ||
          !read_returned_bulk_attribute(returned.dirattr, ATTR_DIR_MOUNTSTATUS,
                                        field, end, directory_mount_status) ||
          !read_returned_bulk_attribute(returned.dirattr, ATTR_DIR_ALLOCSIZE,
                                        field, end, directory_allocated_size) ||
          !read_returned_bulk_attribute(returned.dirattr, ATTR_DIR_DATALENGTH,
                                        field, end, directory_size) ||
          !read_returned_bulk_attribute(returned.fileattr, ATTR_FILE_LINKCOUNT,
                                        field, end, file_link_count) ||
          !read_returned_bulk_attribute(returned.fileattr, ATTR_FILE_TOTALSIZE,
                                        field, end, file_size) ||
          !read_returned_bulk_attribute(returned.fileattr, ATTR_FILE_ALLOCSIZE,
                                        field, end, file_allocated_size) ||
          !read_returned_bulk_attribute(returned.fileattr, ATTR_FILE_DEVTYPE,
                                        field, end, special_device))
      {
        return None;
      }

      let const name_start_offset =
          static_cast<usize>(name_reference_field - record);
      if (name_reference.attr_dataoffset < 0 ||
          static_cast<usize>(name_reference.attr_dataoffset) >
              record_byte_count - name_start_offset)
      {
        return None;
      }
      let const name_start =
          name_reference_field + name_reference.attr_dataoffset;
      let const available_name_byte_count = end - name_start;
      if (name_reference.attr_length == 0 ||
          name_reference.attr_length > available_name_byte_count)
      {
        return None;
      }
      let const name_length = name_start[name_reference.attr_length - 1] == '\0'
                                  ? name_reference.attr_length - 1
                                  : name_reference.attr_length;
      let const name = StringView{name_start, name_length};
      if (name == StringView{"."} || name == StringView{".."}) {
        record = end;
        continue;
      }

      directory_status_entry entry{
          Path::directory_child{String{allocator, name},
                                directory_kind_from_vnode_type(type)}
      };
      entry.has_status =
          error_number == 0 &&
          (directory_mount_status & DIR_MNTSTATUS_MNTPOINT) == 0 &&
          fill_bulk_file_status(
              returned, type, device, modification_time, change_time,
              access_time, owner, group, access_mode, flags, file_id,
              directory_link_count, directory_allocated_size, directory_size,
              file_link_count, file_size, file_allocated_size, special_device,
              entry.status);
      if (!entry.has_status) {
        struct stat info{};
        if (::fstatat(directory_descriptor, entry.child.name.c_str(), &info,
                      AT_SYMLINK_NOFOLLOW) == 0)
        {
          fill_file_status(info, entry.status);
          entry.has_status = true;
        }
      }
      entries.push(steal(entry));
      record = end;
    }
  }
}

static fn find_path_from_file_id(StringView filesystem_path,
                                 u64 file_id) wontthrow -> Maybe<Path>
{
  const String filesystem_path_string{filesystem_path};
  struct statfs filesystem{};
  if (::statfs(filesystem_path_string.c_str(), &filesystem) != 0) return None;

  constexpr usize PATH_BUFFER_BYTE_COUNT = 8192;
  char path_buffer[PATH_BUFFER_BYTE_COUNT];
  let const path_byte_count = ::fsgetpath(path_buffer, sizeof(path_buffer),
                                          &filesystem.f_fsid, file_id);
  if (path_byte_count <= 1) return None;

  return Path{
      StringView{path_buffer, static_cast<usize>(path_byte_count - 1)}
  };
}

#else

static fn list_directory_status_bulk(StringView dir, Allocator allocator) throws
    -> Maybe<ArrayList<directory_status_entry>>
{
  unused(dir);
  unused(allocator);
  return None;
}

static fn find_path_from_file_id(StringView filesystem_path,
                                 u64 file_id) wontthrow -> Maybe<Path>
{
  unused(filesystem_path);
  unused(file_id);
  return None;
}

#endif

cold fn list_directory_status(StringView dir, Allocator allocator) throws
    -> Maybe<ArrayList<directory_status_entry>>
{
  if (let entries = list_directory_status_bulk(dir, allocator);
      entries.has_value())
  {
    return entries;
  }

  return list_directory_status_fallback(dir, allocator);
}

fn path_from_file_id(StringView filesystem_path, u64 file_id) wontthrow
    -> Maybe<Path>
{
  return find_path_from_file_id(filesystem_path, file_id);
}

static fn copy_type_name(filesystem_status &status, StringView name) wontthrow
    -> void
{
  let const copied_length = name.length < sizeof(status.type_name) - 1
                                ? name.length
                                : sizeof(status.type_name) - 1;
  for (usize index = 0; index < copied_length; index++)
    status.type_name[index] = name[index];

  status.type_name[copied_length] = '\0';
}

#if defined __linux__

static pure fn linux_filesystem_type_name(u64 type_id) wontthrow -> StringView
{
  switch (type_id) {
  case 0x0000adf5u: return "adfs";
  case 0x00001cd1u: return "devpts";
  case 0x00001373u: return "devfs";
  case 0x00002011u: return "bpf";
  case 0x00004d44u: return "msdos";
  case 0x00006969u: return "nfs";
  case 0x00009fa0u: return "proc";
  case 0x00009fa2u: return "usbdevfs";
  case 0x0000ef53u: return "ext2/ext3/ext4";
  case 0x00726f6fu: return "ramfs";
  case 0x0027e0ebu: return "cgroupfs";
  case 0x01021994u: return "tmpfs";
  case 0x01021997u: return "v9fs";
  case 0x0bad1deau: return "fuse";
  case 0x1badface: return "bfs";
  case 0x2fc12fc1u: return "zfs";
  case 0x4244u: return "hfs";
  case 0x52654973u: return "reiserfs";
  case 0x5346414fu: return "afs";
  case 0x53464846u: return "wslfs";
  case 0x5346544eu: return "ntfs";
  case 0x58465342u: return "xfs";
  case 0x62656572u: return "sysfs";
  case 0x63677270u: return "cgroup2fs";
  case 0x64626720u: return "debugfs";
  case 0x65735546u: return "fuseblk";
  case 0x6e736673u: return "nsfs";
  case 0x73717368u: return "squashfs";
  case 0x794c7630u: return "overlayfs";
  case 0x9123683eu: return "btrfs";
  case 0xf2f52010u: return "f2fs";
  case 0xf995e849u: return "hpfs";
  default: break;
  }

  return "";
}

static fn fill_native_filesystem_identity(const String &path,
                                          filesystem_status &status) wontthrow
    -> void
{
  struct statfs type_info{};
  if (::statfs(path.c_str(), &type_info) != 0) return;

  status.type_id = static_cast<u64>(type_info.f_type);
  status.filesystem_id =
      (static_cast<u64>(static_cast<u32>(type_info.f_fsid.__val[0])) << 32u) |
      static_cast<u32>(type_info.f_fsid.__val[1]);
  copy_type_name(status, linux_filesystem_type_name(status.type_id));
}

#elif defined __APPLE__ || defined BSD

static fn fill_native_filesystem_identity(const String &path,
                                          filesystem_status &status) wontthrow
    -> void
{
  struct statfs type_info{};
  if (::statfs(path.c_str(), &type_info) != 0) return;

  copy_type_name(status, StringView{type_info.f_fstypename});
}

#else

static fn fill_native_filesystem_identity(const String &path,
                                          filesystem_status &status) wontthrow
    -> void
{
  unused(path);
  unused(status);
}

#endif

fn stat_filesystem(StringView path, filesystem_status &status) wontthrow -> bool
{
  const String path_string{path};
  struct statvfs info{};
  if (::statvfs(path_string.c_str(), &info) != 0) return false;
  status.block_size = info.f_bsize;
  status.fundamental_block_size =
      info.f_frsize == 0 ? info.f_bsize : info.f_frsize;
  status.total_blocks = info.f_blocks;
  status.free_blocks = info.f_bfree;
  status.available_blocks = info.f_bavail;
  status.total_files = info.f_files;
  status.free_files = info.f_ffree;
  status.name_max = info.f_namemax;
  status.filesystem_id = info.f_fsid;
  fill_native_filesystem_identity(path_string, status);

  return true;
}

#if defined __APPLE__ || defined BSD

static fn describe_mount_flags(u64 flags) throws -> String
{
  struct named_mount_flag
  {
    u64 bit;
    StringView name;
  };

  static constexpr named_mount_flag NAMED_FLAGS[] = {
      {MNT_SYNCHRONOUS, "sync"       },
      {MNT_NOEXEC,      "noexec"     },
      {MNT_NOSUID,      "nosuid"     },
      {MNT_NODEV,       "nodev"      },
      {MNT_UNION,       "union"      },
      {MNT_ASYNC,       "async"      },
      {MNT_NOATIME,     "noatime"    },
      {MNT_LOCAL,       "local"      },
      {MNT_QUOTA,       "quota"      },
      {MNT_ROOTFS,      "rootfs"     },
      {MNT_DONTBROWSE,  "nobrowse"   },
      {MNT_AUTOMOUNTED, "automounted"},
      {MNT_JOURNALED,   "journaled"  },
  };

  let options = String{(flags & MNT_RDONLY) != 0 ? "ro" : "rw"};
  for (let const &named : NAMED_FLAGS) {
    if ((flags & named.bit) == 0) continue;

    options += ",";
    options += named.name;
  }

  return options;
}

#if defined __APPLE__

static fn read_volume_identity(mounted_filesystem &filesystem) throws -> void
{
  const String target{filesystem.target.view()};
  struct attrlist attributes{};
  attributes.bitmapcount = ATTR_BIT_MAP_COUNT;

  struct volume_name_buffer
  {
    u32 length;
    attrreference_t reference;
    char storage[PATH_MAX];
  } __attribute__((aligned(4), packed));
  volume_name_buffer name{};
  attributes.volattr = ATTR_VOL_INFO | ATTR_VOL_NAME;
  if (::getattrlist(target.c_str(), &attributes, &name, sizeof(name), 0) == 0) {
    let const name_offset =
        static_cast<usize>(reinterpret_cast<const char *>(&name.reference) -
                           reinterpret_cast<const char *>(&name));
    if (name.reference.attr_dataoffset >= 0) {
      let const data_offset =
          name_offset + static_cast<usize>(name.reference.attr_dataoffset);
      let name_length = static_cast<usize>(name.reference.attr_length);
      if (data_offset <= sizeof(name) &&
          name_length <= sizeof(name) - data_offset)
      {
        let const data = reinterpret_cast<const char *>(&name) + data_offset;
        if (name_length > 0 && data[name_length - 1] == '\0') name_length--;
        filesystem.volume_name = String{
            StringView{data, name_length}
        };
      }
    }
  }

  struct volume_uuid_buffer
  {
    u32 length;
    uuid_t value;
  } __attribute__((aligned(4), packed));
  volume_uuid_buffer uuid{};
  attributes.volattr = ATTR_VOL_INFO | ATTR_VOL_UUID;
  if (::getattrlist(target.c_str(), &attributes, &uuid, sizeof(uuid), 0) == 0) {
    uuid_string_t text{};
    uuid_unparse_lower(uuid.value, text);
    filesystem.volume_uuid = String{text};
  }
}

#else

static fn read_volume_identity(mounted_filesystem &filesystem) wontthrow -> void
{
  unused(filesystem);
}

#endif

#endif

#if defined __linux__

static fn decode_udev_filename(StringView filename, Allocator allocator) throws
    -> String
{
  let decoded = String{allocator};
  decoded.reserve(filename.length);
  for (usize position = 0; position < filename.length; position++) {
    if (filename[position] != '\\' || position + 3 >= filename.length ||
        filename[position + 1] != 'x')
    {
      decoded.push(filename[position]);
      continue;
    }

    let const high = utils::hex_digit_value(filename[position + 2]);
    let const low = utils::hex_digit_value(filename[position + 3]);
    if (!high.has_value() || !low.has_value()) {
      decoded.push(filename[position]);
      continue;
    }

    decoded.push(static_cast<char>(*high * 16 + *low));
    position += 3;
  }

  return decoded;
}

static fn
populate_linux_volume_identity(ArrayList<mounted_filesystem> &filesystems,
                               const ArrayList<Path> &canonical_sources,
                               StringView directory, bool is_uuid) throws
    -> void
{
  let const allocator = heap_allocator();
  let const identity_directory = Path{directory, allocator};
  let const entries = Path::read_directory(identity_directory, allocator);
  if (!entries.has_value()) return;

  for (let const &entry : *entries) {
    let identity_path = identity_directory.clone();
    identity_path.append(entry.view());
    let const canonical_identity = canonical_path(identity_path);
    if (!canonical_identity.has_value()) continue;

    for (usize index = 0; index < filesystems.count(); index++) {
      if (canonical_sources[index].is_empty() ||
          canonical_sources[index] != *canonical_identity)
      {
        continue;
      }

      if (is_uuid) {
        if (filesystems[index].volume_uuid.is_empty())
          filesystems[index].volume_uuid = entry.clone();
      } else if (filesystems[index].volume_name.is_empty()) {
        filesystems[index].volume_name =
            decode_udev_filename(entry.view(), allocator);
      }
    }
  }
}

#endif

static fn
append_mounted_filesystems(ArrayList<mounted_filesystem> &result) throws -> void
{
#if defined __linux__
  let *mount_file = setmntent("/proc/self/mounts", "r");
  if (mount_file == nullptr) mount_file = setmntent("/etc/mtab", "r");
  if (mount_file == nullptr) return;
  defer { endmntent(mount_file); };
  mntent entry{};
  char buffer[8192];

  while (getmntent_r(mount_file, &entry, buffer, sizeof(buffer)) != nullptr) {
    let filesystem =
        mounted_filesystem{String{entry.mnt_fsname}, String{entry.mnt_dir},
                           String{entry.mnt_type}, String{entry.mnt_opts}};
    let const source = filesystem.source.view();
    if (source.starts_with("UUID="))
      filesystem.volume_uuid = String{source.substring(5)};
    else if (source.starts_with("LABEL="))
      filesystem.volume_name = String{source.substring(6)};
    result.push(steal(filesystem));
  }

  let canonical_sources = ArrayList<Path>{heap_allocator()};
  canonical_sources.reserve(result.count());
  for (let const &filesystem : result) {
    let const canonical_source = canonical_path(Path{filesystem.source.view()});
    canonical_sources.push(
        canonical_source.has_value() ? steal(*canonical_source) : Path{});
  }
  populate_linux_volume_identity(result, canonical_sources,
                                 "/dev/disk/by-label", false);
  populate_linux_volume_identity(result, canonical_sources, "/dev/disk/by-uuid",
                                 true);
#elif defined __APPLE__ || defined BSD
  struct statfs *entries = nullptr;
  let const entry_count = getmntinfo(&entries, MNT_NOWAIT);

  for (int index = 0; index < entry_count; index++) {
    let filesystem = mounted_filesystem{
        String{entries[index].f_mntfromname},
        String{entries[index].f_mntonname}, String{entries[index].f_fstypename},
        describe_mount_flags(static_cast<u64>(entries[index].f_flags))};
    read_volume_identity(filesystem);
    result.push(steal(filesystem));
  }
#else
  result.push(mounted_filesystem{String{"."}, String{"."},
                                 String{heap_allocator()},
                                 String{heap_allocator()}});
#endif
}

fn mounted_filesystems() throws -> ArrayList<mounted_filesystem>
{
  let result = ArrayList<mounted_filesystem>{heap_allocator()};
  append_mounted_filesystems(result);
  return result;
}

static fn
find_mounted_filesystem(StringView path,
                        const ArrayList<mounted_filesystem> &filesystems,
                        StringView required_type = {}) throws -> Maybe<usize>
{
  let const absolute_path = Path{path}.to_absolute();
  Maybe<usize> selected_index;
  for (usize index = 0; index < filesystems.count(); index++) {
    let const &filesystem = filesystems[index];
    if (!required_type.is_empty() && filesystem.type != required_type) continue;

    let const target = filesystem.target.view();
    let const subject = absolute_path.view();
    let const is_root = target == "/";
    let const is_same = subject == target;
    let const is_descendant = subject.starts_with(target) &&
                              subject.length > target.length &&
                              (is_root || subject[target.length] == '/');
    if (!is_same && !is_descendant) continue;
    if (!selected_index.has_value() ||
        target.length > filesystems[*selected_index].target.view().length)
    {
      selected_index = index;
    }
  }

  return selected_index;
}

#if defined __linux__

static fn read_native_filesystem_error_counters(
    StringView path, filesystem_error_counters &counters) throws -> bool
{
  let const filesystems = mounted_filesystems();
  let const selected_index =
      find_mounted_filesystem(path, filesystems, "btrfs");
  if (!selected_index.has_value()) return false;
  let const &selected = filesystems[*selected_index];

  let const sysfs_root = Path{"/sys/fs/btrfs"};
  let const filesystem_names = Path::read_directory(sysfs_root);
  if (!filesystem_names.has_value()) return false;

  let filesystem_root = Path{};
  for (let const &filesystem_name : *filesystem_names) {
    if (!selected.volume_uuid.is_empty() &&
        filesystem_name == selected.volume_uuid)
    {
      filesystem_root = sysfs_root.clone();
      filesystem_root.append(filesystem_name.view());
      break;
    }

    let const device_name = Path{selected.source.view()}.filename();
    if (device_name.is_empty()) continue;
    let device_path = sysfs_root.clone();
    device_path.append(filesystem_name.view());
    device_path.append("devices");
    device_path.append(device_name);
    if (!device_path.exists()) continue;
    filesystem_root = sysfs_root.clone();
    filesystem_root.append(filesystem_name.view());
    break;
  }
  if (filesystem_root.is_empty()) return false;

  let devinfo = filesystem_root.clone();
  devinfo.append("devinfo");
  let const device_ids = Path::read_directory(devinfo);
  if (!device_ids.has_value() || device_ids->is_empty()) return false;

  struct named_counter
  {
    StringView name;
    u64 filesystem_error_counters::*value;
  };
  static constexpr named_counter COUNTERS[] = {
      {"read_errs",       &filesystem_error_counters::read_count      },
      {"write_errs",      &filesystem_error_counters::write_count     },
      {"flush_errs",      &filesystem_error_counters::flush_count     },
      {"corruption_errs", &filesystem_error_counters::corruption_count},
      {"generation_errs", &filesystem_error_counters::generation_count},
  };
  let const do_add = [](u64 &total, u64 value) wontthrow -> bool {
    if (value > UINT64_MAX - total) return false;
    total += value;
    return true;
  };

  for (let const &device_id : *device_ids) {
    let stats_path = devinfo.clone();
    stats_path.append(device_id.view());
    stats_path.append("error_stats");
    let const contents = stats_path.read_entire_file();
    if (!contents.has_value()) return false;

    u8 seen_counters = 0;
    usize position = 0;
    while (position < contents->length()) {
      let const line = contents->view().next_line(position);
      usize word_position = 0;
      let const name = line.next_ascii_whitespace_word(word_position);
      let const value_text = line.next_ascii_whitespace_word(word_position);
      if (name.is_empty() || value_text.is_empty()) return false;

      let const parsed =
          utils::parse_integer_in_base_u64(value_text, int_base::decimal);
      if (parsed.is_error()) return false;
      bool was_known = false;
      for (usize index = 0; index < countof(COUNTERS); index++) {
        if (name != COUNTERS[index].name) continue;
        if (!do_add(counters.*COUNTERS[index].value, parsed.value())) {
          return false;
        }
        seen_counters |= static_cast<u8>(1u << index);
        was_known = true;
        break;
      }
      if (!was_known) return false;
    }
    if (seen_counters != (1u << countof(COUNTERS)) - 1u) return false;
  }

  return true;
}

static fn
read_ext4_recorded_error_count(const mounted_filesystem &filesystem) throws
    -> Maybe<u64>
{
  let const do_read_count = [](StringView device_name) throws -> Maybe<u64> {
    if (device_name.is_empty()) return None;

    let count_path = Path{"/sys/fs/ext4"};
    count_path.append(device_name);
    count_path.append("errors_count");
    let const contents = count_path.read_entire_file();
    if (!contents.has_value()) return None;

    let const text = contents->view().without_trailing_newline();
    if (!text.is_all_decimal_digits()) return None;
    let const parsed =
        utils::parse_integer_in_base_u64(text, int_base::decimal);
    if (parsed.is_error()) return None;

    return parsed.value();
  };

  let const source_path = Path{filesystem.source.view()};
  if (let const count = do_read_count(source_path.filename());
      count.has_value())
  {
    return count;
  }

  let const canonical_source = canonical_path(source_path);
  if (!canonical_source.has_value()) return None;
  return do_read_count(canonical_source->filename());
}

fn read_filesystem_integrity_evidence(StringView path) throws
    -> Maybe<filesystem_integrity_evidence>
{
  let const filesystems = mounted_filesystems();
  let const selected_index = find_mounted_filesystem(path, filesystems);
  if (!selected_index.has_value()) return None;
  let const &filesystem = filesystems[*selected_index];

  if (filesystem.type == "btrfs") {
    filesystem_error_counters counters{};
    if (!read_native_filesystem_error_counters(path, counters)) return None;

    return filesystem_integrity_evidence{
        filesystem_integrity_kind::BtrfsDeviceErrorCounters, counters};
  }

  if (filesystem.type == "ext4") {
    let const count = read_ext4_recorded_error_count(filesystem);
    if (!count.has_value()) return None;

    filesystem_integrity_evidence evidence{
        filesystem_integrity_kind::Ext4RecordedErrors};
    evidence.recorded_error_count = *count;
    return evidence;
  }

  return None;
}

#else

static fn read_native_filesystem_error_counters(
    StringView path, filesystem_error_counters &counters) wontthrow -> bool
{
  unused(path);
  unused(counters);
  return false;
}

fn read_filesystem_integrity_evidence(StringView path) throws
    -> Maybe<filesystem_integrity_evidence>
{
  unused(path);
  return None;
}

#endif

fn read_filesystem_error_counters(StringView path,
                                  filesystem_error_counters &counters) throws
    -> bool
{
  return read_native_filesystem_error_counters(path, counters);
}

fn verify_filesystem_integrity(StringView path, u64 timeout_nanoseconds) throws
    -> filesystem_verification_result
{
#if defined __APPLE__
  let const filesystems = mounted_filesystems();
  let const selected_index = find_mounted_filesystem(path, filesystems);
  if (!selected_index.has_value() ||
      filesystems[*selected_index].type != "apfs")
  {
    return filesystem_verification_result::Unsupported;
  }

  constexpr char DISKUTIL_PATH[] = "/usr/sbin/diskutil";
  if (::access(DISKUTIL_PATH, X_OK) != 0)
    return filesystem_verification_result::Unavailable;

  let const null_descriptor = ::open("/dev/null", O_RDWR);
  if (null_descriptor < 0) return filesystem_verification_result::Unavailable;

  posix_spawn_file_actions_t actions;
  if (posix_spawn_file_actions_init(&actions) != 0) {
    ::close(null_descriptor);
    return filesystem_verification_result::Unavailable;
  }
  defer { posix_spawn_file_actions_destroy(&actions); };
  let const did_prepare_actions =
      posix_spawn_file_actions_adddup2(&actions, null_descriptor,
                                       STDIN_FILENO) == 0 &&
      posix_spawn_file_actions_adddup2(&actions, null_descriptor,
                                       STDOUT_FILENO) == 0 &&
      posix_spawn_file_actions_adddup2(&actions, null_descriptor,
                                       STDERR_FILENO) == 0 &&
      posix_spawn_file_actions_addclose(&actions, null_descriptor) == 0;
  if (!did_prepare_actions) {
    ::close(null_descriptor);
    return filesystem_verification_result::Unavailable;
  }

  let const mount_path = filesystems[*selected_index].target.clone();
  char *arguments[] = {const_cast<char *>(DISKUTIL_PATH),
                       const_cast<char *>("verifyVolume"),
                       const_cast<char *>(mount_path.c_str()), nullptr};
  pid_t child = 0;
  let const spawn_result =
      posix_spawn(&child, DISKUTIL_PATH, &actions, nullptr, arguments, environ);
  ::close(null_descriptor);
  if (spawn_result != 0) return filesystem_verification_result::Unavailable;

  let const do_stop_child = [&]() wontthrow {
    unused(::kill(child, SIGKILL));
    while (::waitpid(child, nullptr, 0) == -1 && errno == EINTR) {}
  };
  let const started_at = monotonic_nanos();
  loop
  {
    int status = 0;
    let const waited = ::waitpid(child, &status, WNOHANG);
    if (waited == child) {
      return WIFEXITED(status) && WEXITSTATUS(status) == 0
                 ? filesystem_verification_result::Passed
                 : filesystem_verification_result::Failed;
    }
    if (waited == -1 && errno != EINTR) {
      do_stop_child();
      return filesystem_verification_result::Unavailable;
    }
    if (INTERRUPT_REQUESTED) {
      do_stop_child();
      return filesystem_verification_result::Interrupted;
    }
    let const now = monotonic_nanos();
    if (now - started_at >= timeout_nanoseconds) {
      do_stop_child();
      return filesystem_verification_result::TimedOut;
    }

    timespec pause{0, 50'000'000};
    while (::nanosleep(&pause, &pause) == -1 && errno == EINTR) {
      if (INTERRUPT_REQUESTED) break;
    }
  }
#else
  unused(path);
  unused(timeout_nanoseconds);
  return filesystem_verification_result::Unsupported;
#endif
}

#if defined __APPLE__

static fn sync_native_descriptor(descriptor fd, sync_mode mode) wontthrow
    -> bool
{
  unused(mode);
  return ::fsync(fd) == 0;
}

#else

static fn sync_native_descriptor(descriptor fd, sync_mode mode) wontthrow
    -> bool
{
  return (mode == sync_mode::DataOnly ? ::fdatasync(fd) : ::fsync(fd)) == 0;
}

#endif

fn sync_path(StringView path, sync_mode mode) wontthrow -> bool
{
  const String path_string{path};
  let const path_fd = ::open(path_string.c_str(), O_RDONLY | O_CLOEXEC);
  if (path_fd < 0) return false;
  defer { ::close(path_fd); };

  return sync_native_descriptor(path_fd, mode);
}

namespace batch_internal {

#if defined __linux__

static fn fill_relative_file_status(const struct stat &info,
                                    file_status &status) wontthrow -> void
{
  status.device_id = static_cast<u64>(info.st_dev);
  status.special_device_id = static_cast<u64>(info.st_rdev);
  status.file_id = static_cast<u64>(info.st_ino);
  status.link_count = static_cast<u64>(info.st_nlink);
  status.size = static_cast<u64>(info.st_size);
  status.access_time = info.st_atim.tv_sec;
  status.modification_time = info.st_mtim.tv_sec;
  status.change_time = info.st_ctim.tv_sec;
  status.blocks = static_cast<u64>(info.st_blocks);
  status.mode = static_cast<u32>(info.st_mode);
  status.owner_id = static_cast<u32>(info.st_uid);
  status.group_id = static_cast<u32>(info.st_gid);
  status.access_nanoseconds = static_cast<u32>(info.st_atim.tv_nsec);
  status.modification_nanoseconds = static_cast<u32>(info.st_mtim.tv_nsec);
  status.change_nanoseconds = static_cast<u32>(info.st_ctim.tv_nsec);
  status.has_file_identity = true;
}

#endif

static fn validate_batched_syscall(const batched_syscall &operation) wontthrow
    -> i32
{
  if (operation.byte_count > static_cast<usize>(SSIZE_MAX) ||
      operation.byte_offset > 0x7fffffffffffffffULL)
  {
    return EINVAL;
  }

  switch (batch_operation_access::get_kind(operation)) {
  case batched_syscall_id::Read:
    if (batch_operation_access::get_descriptor(operation) == KOSH_INVALID_FD ||
        (batch_operation_access::get_output_buffer(operation) == nullptr &&
         operation.byte_count != 0))
    {
      return EINVAL;
    }
    return 0;
  case batched_syscall_id::Write:
  case batched_syscall_id::WriteCurrent:
    if (batch_operation_access::get_descriptor(operation) == KOSH_INVALID_FD ||
        (batch_operation_access::get_input_buffer(operation) == nullptr &&
         operation.byte_count != 0))
    {
      return EINVAL;
    }
    return 0;
  case batched_syscall_id::Lstat:
  case batched_syscall_id::Stat:
    return batch_operation_access::get_path(operation) == nullptr ||
                   batch_operation_access::get_status(operation) == nullptr
               ? EINVAL
               : 0;
  case batched_syscall_id::LstatAt:
  case batched_syscall_id::StatAt:
    return batch_operation_access::get_directory_descriptor(operation) ==
                   KOSH_INVALID_FD ||
                   batch_operation_access::get_relative_name(operation) ==
                       nullptr ||
                   batch_operation_access::get_status(operation) == nullptr
               ? EINVAL
               : 0;
  case batched_syscall_id::Exists:
    return batch_operation_access::get_path(operation) == nullptr ? EINVAL : 0;
  case batched_syscall_id::Invalid: return EINVAL;
  }

  return EINVAL;
}

static fn
execute_batched_syscall_direct(const batched_syscall &operation,
                               batched_syscall_result &result) wontthrow -> void
{
  result = {operation.request_id, 0, validate_batched_syscall(operation)};
  if (result.error_number != 0) return;

  switch (batch_operation_access::get_kind(operation)) {
  case batched_syscall_id::Read:
    loop
    {
      let const transferred_byte_count = ::pread(
          batch_operation_access::get_descriptor(operation),
          batch_operation_access::get_output_buffer(operation),
          operation.byte_count, static_cast<off_t>(operation.byte_offset));
      if (transferred_byte_count >= 0) {
        result.transferred_byte_count =
            static_cast<usize>(transferred_byte_count);
        return;
      }
      if (errno != EINTR || INTERRUPT_REQUESTED) {
        result.error_number = errno;
        return;
      }
    }
  case batched_syscall_id::Write:
    loop
    {
      let const transferred_byte_count = ::pwrite(
          batch_operation_access::get_descriptor(operation),
          batch_operation_access::get_input_buffer(operation),
          operation.byte_count, static_cast<off_t>(operation.byte_offset));
      if (transferred_byte_count >= 0) {
        result.transferred_byte_count =
            static_cast<usize>(transferred_byte_count);
        return;
      }
      if (errno != EINTR || INTERRUPT_REQUESTED) {
        result.error_number = errno;
        return;
      }
    }
  case batched_syscall_id::WriteCurrent:
    loop
    {
      let const transferred_byte_count =
          ::write(batch_operation_access::get_descriptor(operation),
                  batch_operation_access::get_input_buffer(operation),
                  operation.byte_count);
      if (transferred_byte_count >= 0) {
        result.transferred_byte_count =
            static_cast<usize>(transferred_byte_count);
        return;
      }
      if (errno != EINTR || INTERRUPT_REQUESTED) {
        result.error_number = errno;
        return;
      }
    }
  case batched_syscall_id::Lstat:
    if (!stat_path(batch_operation_access::get_path(operation)->text().view(),
                   *batch_operation_access::get_status(operation)))
      result.error_number = errno;
    return;
  case batched_syscall_id::Stat:
    if (!stat_path_following(
            batch_operation_access::get_path(operation)->text().view(),
            *batch_operation_access::get_status(operation)))
      result.error_number = errno;
    return;
  case batched_syscall_id::LstatAt:
#if defined __linux__
    {
      struct stat info{};
      loop
      {
        if (::fstatat(batch_operation_access::get_directory_descriptor(operation),
                      batch_operation_access::get_relative_name(operation),
                      &info, AT_SYMLINK_NOFOLLOW) == 0)
        {
          fill_relative_file_status(
              info, *batch_operation_access::get_status(operation));
          return;
        }
        if (errno != EINTR || INTERRUPT_REQUESTED) {
          result.error_number = errno;
          return;
        }
      }
    }
#else
    result.error_number = ENOTSUP;
#endif
    return;
  case batched_syscall_id::StatAt:
#if defined __linux__
    {
      struct stat info{};
      loop
      {
        if (::fstatat(batch_operation_access::get_directory_descriptor(operation),
                      batch_operation_access::get_relative_name(operation),
                      &info, 0) == 0)
        {
          fill_relative_file_status(
              info, *batch_operation_access::get_status(operation));
          return;
        }
        if (errno != EINTR || INTERRUPT_REQUESTED) {
          result.error_number = errno;
          return;
        }
      }
    }
#else
    result.error_number = ENOTSUP;
#endif
    return;
  case batched_syscall_id::Exists:
    result.is_existing =
        path_exists(batch_operation_access::get_path(operation)->text().view());
    return;
  case batched_syscall_id::Invalid: result.error_number = EINVAL; return;
  }
}

#if defined __APPLE__

static pure fn find_directory_status_entry(
    const ArrayList<directory_status_entry> &entries, StringView name) wontthrow
    -> const directory_status_entry *
{
  usize lower = 0;
  usize upper = entries.count();
  while (lower < upper) {
    let const middle = lower + (upper - lower) / 2;
    if (entries[middle].child.name.view() < name)
      lower = middle + 1;
    else
      upper = middle;
  }
  if (lower == entries.count() || entries[lower].child.name.view() != name)
    return nullptr;

  return &entries[lower];
}

static fn
execute_getattrlistbulk_batch(const batched_syscall *operations,
                              usize operation_count,
                              batched_syscall_result *results) wontthrow -> bool
{
  if (operation_count < 2) return false;

  for (usize index = 0; index < operation_count; index++) {
    if (batch_operation_access::get_kind(operations[index]) !=
            batched_syscall_id::Lstat ||
        validate_batched_syscall(operations[index]) != 0)
    {
      return false;
    }
  }

  try {
    let parent_paths = ArrayList<Path>{heap_allocator()};
    let operation_positions = ArrayList<usize>{heap_allocator()};
    parent_paths.reserve(operation_count);
    operation_positions.reserve(operation_count);
    for (usize index = 0; index < operation_count; index++) {
      parent_paths.push(batch_operation_access::get_path(operations[index])
                            ->parent_or_current());
      operation_positions.push(index);
    }
    operation_positions.sort([&](usize left, usize right) {
      let const left_parent = parent_paths[left].view();
      let const right_parent = parent_paths[right].view();
      if (left_parent != right_parent) return left_parent < right_parent;
      return left < right;
    });

    usize group_start = 0;
    while (group_start < operation_count) {
      let const first_position = operation_positions[group_start];
      let const &group_parent = parent_paths[first_position];
      usize group_end = group_start + 1;
      while (group_end < operation_count) {
        let const candidate_position = operation_positions[group_end];
        if (parent_paths[candidate_position].view() != group_parent.view()) {
          break;
        }
        group_end++;
      }

      if (group_end - group_start == 1) {
        execute_batched_syscall_direct(operations[first_position],
                                       results[first_position]);
        group_start = group_end;
        continue;
      }

      let entries =
          list_directory_status_bulk(group_parent.view(), heap_allocator());
      if (entries.has_value()) {
        entries->sort([](const directory_status_entry &left,
                         const directory_status_entry &right) {
          return left.child.name.view() < right.child.name.view();
        });
      }
      for (usize group_position = group_start; group_position < group_end;
           group_position++)
      {
        let const operation_position = operation_positions[group_position];
        let const &operation = operations[operation_position];
        let &result = results[operation_position];
        result = {operation.request_id, 0, 0};

        bool has_status = false;
        if (entries.has_value()) {
          let const filename =
              batch_operation_access::get_path(operation)->filename();
          let const *entry = find_directory_status_entry(*entries, filename);
          if (entry != nullptr && entry->has_status) {
            *batch_operation_access::get_status(operation) = entry->status;
            has_status = true;
          }
        }
        if (!has_status) execute_batched_syscall_direct(operation, result);
      }

      group_start = group_end;
    }

    return true;
  } catch (...) {
    return false;
  }
}

static fn finish_suspended_aio(aiocb &control,
                               batched_syscall_result &result) wontthrow -> bool
{
  errno = 0;
  let const error_number = ::aio_error(&control);
  let const status_error_number = errno;
  if (error_number == EINPROGRESS) return false;

  errno = 0;
  let const transferred_byte_count = ::aio_return(&control);
  let const return_error_number = errno;
  if (transferred_byte_count >= 0) {
    result.transferred_byte_count = static_cast<usize>(transferred_byte_count);
  } else if (error_number > 0) {
    result.error_number = error_number;
  } else {
    result.error_number =
        return_error_number != 0 ? return_error_number : status_error_number;
  }

  return true;
}

static fn execute_kqueue_aio_batch(const batched_syscall *operations,
                                   usize operation_count,
                                   batched_syscall_result *results) wontthrow
    -> bool
{
  constexpr usize MINIMUM_AIO_BYTE_COUNT = 1024 * 1024;
  usize aio_operation_count = 0;
  usize aio_byte_count = 0;

  for (usize index = 0; index < operation_count; index++) {
    let const &operation = operations[index];
    switch (batch_operation_access::get_kind(operation)) {
    case batched_syscall_id::Read:
    case batched_syscall_id::Write:
      aio_operation_count++;
      if (operation.byte_count >= MINIMUM_AIO_BYTE_COUNT - aio_byte_count)
        aio_byte_count = MINIMUM_AIO_BYTE_COUNT;
      else
        aio_byte_count += operation.byte_count;
      break;
    case batched_syscall_id::WriteCurrent: return false;
    case batched_syscall_id::Lstat:
    case batched_syscall_id::Stat:
    case batched_syscall_id::LstatAt:
    case batched_syscall_id::StatAt:
    case batched_syscall_id::Exists: break;
    case batched_syscall_id::Invalid: return false;
    }
  }

  if (aio_operation_count < 2 || aio_byte_count < MINIMUM_AIO_BYTE_COUNT) {
    return false;
  }

  let const queue_descriptor = ::kqueue();
  if (queue_descriptor < 0) return false;
  defer { ::close(queue_descriptor); };

  usize operation_start = 0;
  while (operation_start < operation_count) {
    let const chunk_count = operation_count - operation_start > AIO_LISTIO_MAX
                                ? static_cast<usize>(AIO_LISTIO_MAX)
                                : operation_count - operation_start;
    aiocb controls[AIO_LISTIO_MAX]{};
    bool is_queued[AIO_LISTIO_MAX]{};
    bool is_completed[AIO_LISTIO_MAX]{};
    usize queued_count = 0;

    for (usize chunk_index = 0; chunk_index < chunk_count; chunk_index++) {
      let const operation_index = operation_start + chunk_index;
      let const &operation = operations[operation_index];
      let &result = results[operation_index];
      let const operation_kind = batch_operation_access::get_kind(operation);
      result = {operation.request_id, 0, validate_batched_syscall(operation)};
      if (result.error_number != 0) continue;

      switch (operation_kind) {
      case batched_syscall_id::Lstat:
      case batched_syscall_id::Stat:
      case batched_syscall_id::LstatAt:
      case batched_syscall_id::StatAt:
      case batched_syscall_id::Exists:
        execute_batched_syscall_direct(operation, result);
        continue;
      case batched_syscall_id::Read:
      case batched_syscall_id::Write:
      case batched_syscall_id::WriteCurrent: break;
      case batched_syscall_id::Invalid: continue;
      }

      let &control = controls[chunk_index];
      control.aio_fildes = batch_operation_access::get_descriptor(operation);
      control.aio_offset = static_cast<off_t>(operation.byte_offset);
      control.aio_buf =
          operation_kind == batched_syscall_id::Read
              ? batch_operation_access::get_output_buffer(operation)
              : const_cast<char *>(
                    batch_operation_access::get_input_buffer(operation));
      control.aio_nbytes = operation.byte_count;
      control.aio_sigevent.sigev_notify = SIGEV_KEVENT;
      control.aio_sigevent.sigev_signo = queue_descriptor;
      control.aio_sigevent.sigev_value.sival_ptr = &control;

      let const submission_result = operation_kind == batched_syscall_id::Read
                                        ? ::aio_read(&control)
                                        : ::aio_write(&control);
      if (submission_result != 0) {
        if (errno == EAGAIN || errno == ENOSYS) {
          execute_batched_syscall_direct(operation, result);
        } else {
          result.error_number = errno;
        }
        continue;
      }

      is_queued[chunk_index] = true;
      queued_count++;
    }

    usize completed_count = 0;
    let const do_interrupt_batch = [&]() wontthrow -> void {
      for (usize chunk_index = 0; chunk_index < chunk_count; chunk_index++) {
        if (!is_queued[chunk_index] || is_completed[chunk_index]) continue;
        unused(::aio_cancel(controls[chunk_index].aio_fildes,
                            &controls[chunk_index]));
      }
      for (usize chunk_index = 0; chunk_index < chunk_count; chunk_index++) {
        if (!is_queued[chunk_index] || is_completed[chunk_index]) continue;

        let &result = results[operation_start + chunk_index];
        const aiocb *pending[] = {&controls[chunk_index]};
        while (!finish_suspended_aio(controls[chunk_index], result))
          unused(::aio_suspend(pending, 1, nullptr));
        result = {operations[operation_start + chunk_index].request_id, 0,
                  EINTR};
        is_completed[chunk_index] = true;
        completed_count++;
      }
      for (usize index = operation_start + chunk_count; index < operation_count;
           index++)
      {
        results[index] = {operations[index].request_id, 0, EINTR};
      }
    };
    while (completed_count < queued_count) {
      if (INTERRUPT_REQUESTED) {
        do_interrupt_batch();
        return true;
      }

      struct kevent64_s events[AIO_LISTIO_MAX]{};
      let const event_count = ::kevent64(
          queue_descriptor, nullptr, 0, events,
          static_cast<i32>(queued_count - completed_count), 0, nullptr);
      if (event_count < 0) {
        if (errno == EINTR) {
          if (!INTERRUPT_REQUESTED) continue;
          do_interrupt_batch();
          return true;
        }

        for (usize chunk_index = 0; chunk_index < chunk_count; chunk_index++) {
          if (!is_queued[chunk_index] || is_completed[chunk_index]) continue;
          const aiocb *pending[] = {&controls[chunk_index]};
          while (!finish_suspended_aio(controls[chunk_index],
                                       results[operation_start + chunk_index]))
          {
            if (INTERRUPT_REQUESTED) {
              do_interrupt_batch();
              return true;
            }
            unused(::aio_suspend(pending, 1, nullptr));
          }
          is_completed[chunk_index] = true;
          completed_count++;
        }
        break;
      }

      for (i32 event_index = 0; event_index < event_count; event_index++) {
        let const control_address =
            static_cast<uintptr>(events[event_index].ident);
        let const controls_begin = reinterpret_cast<uintptr>(controls);
        let const controls_end =
            reinterpret_cast<uintptr>(controls + chunk_count);
        if (control_address < controls_begin || control_address >= controls_end)
          continue;
        let const byte_offset = control_address - controls_begin;
        if (byte_offset % sizeof(aiocb) != 0) continue;
        let const chunk_index = byte_offset / sizeof(aiocb);
        if (!is_queued[chunk_index] || is_completed[chunk_index]) continue;
        let &result = results[operation_start + chunk_index];
        const aiocb *pending[] = {&controls[chunk_index]};
        while (!finish_suspended_aio(controls[chunk_index], result))
          unused(::aio_suspend(pending, 1, nullptr));
        is_completed[chunk_index] = true;
        completed_count++;
      }
    }

    operation_start += chunk_count;
    if (INTERRUPT_REQUESTED) {
      for (usize index = operation_start; index < operation_count; index++)
        results[index] = {operations[index].request_id, 0, EINTR};
      return true;
    }
  }

  return true;
}

static fn execute_native_batch(const batched_syscall *operations,
                               usize operation_count,
                               batched_syscall_result *results) wontthrow
    -> bool
{
  if (execute_getattrlistbulk_batch(operations, operation_count, results))
    return true;

  return execute_kqueue_aio_batch(operations, operation_count, results);
}

#elif defined __linux__

static fn fill_file_status(const struct statx &info,
                           file_status &status) wontthrow -> void
{
  status.device_id =
      static_cast<u64>(makedev(info.stx_dev_major, info.stx_dev_minor));
  status.special_device_id =
      static_cast<u64>(makedev(info.stx_rdev_major, info.stx_rdev_minor));
  status.file_id = info.stx_ino;
  status.link_count = info.stx_nlink;
  status.size = info.stx_size;
  status.access_time = info.stx_atime.tv_sec;
  status.modification_time = info.stx_mtime.tv_sec;
  status.change_time = info.stx_ctime.tv_sec;
  status.blocks = info.stx_blocks;
  status.mode = info.stx_mode;
  status.owner_id = info.stx_uid;
  status.group_id = info.stx_gid;
  status.access_nanoseconds = info.stx_atime.tv_nsec;
  status.modification_nanoseconds = info.stx_mtime.tv_nsec;
  status.change_nanoseconds = info.stx_ctime.tv_nsec;
  status.has_file_identity = true;
}

static constexpr u32 IO_URING_ENTRY_COUNT = 512;

struct io_uring_batch
{
  io_uring_params parameters{};
  io_uring_sqe *submission_entries{nullptr};
  io_uring_cqe *completion_entries{nullptr};
  u32 *submission_head{nullptr};
  u32 *submission_tail{nullptr};
  u32 *submission_mask{nullptr};
  u32 *submission_count{nullptr};
  u32 *submission_array{nullptr};
  u32 *completion_head{nullptr};
  u32 *completion_tail{nullptr};
  u32 *completion_mask{nullptr};
  u32 *completion_count{nullptr};
  opaque *submission_mapping{MAP_FAILED};
  opaque *completion_mapping{MAP_FAILED};
  opaque *entry_mapping{MAP_FAILED};
  usize submission_mapping_size{0};
  usize completion_mapping_size{0};
  usize entry_mapping_size{0};
  i64 owner_process_id{-1};
  i32 descriptor{-1};
  bool has_shared_mapping{false};
  bool has_read{false};
  bool has_write{false};
  bool has_stat{false};

  ~io_uring_batch();
};

static fn close_io_uring_batch(io_uring_batch &ring) wontthrow -> void
{
  if (ring.descriptor >= 0) {
    ::close(ring.descriptor);
    ring.descriptor = -1;
  }
  if (ring.entry_mapping != MAP_FAILED) {
    ::munmap(ring.entry_mapping, ring.entry_mapping_size);
    ring.entry_mapping = MAP_FAILED;
  }
  if (ring.completion_mapping != MAP_FAILED && !ring.has_shared_mapping) {
    ::munmap(ring.completion_mapping, ring.completion_mapping_size);
    ring.completion_mapping = MAP_FAILED;
  }
  if (ring.submission_mapping != MAP_FAILED) {
    ::munmap(ring.submission_mapping, ring.submission_mapping_size);
    ring.submission_mapping = MAP_FAILED;
  }

  ring.parameters = {};
  ring.submission_entries = nullptr;
  ring.completion_entries = nullptr;
  ring.submission_head = nullptr;
  ring.submission_tail = nullptr;
  ring.submission_mask = nullptr;
  ring.submission_count = nullptr;
  ring.submission_array = nullptr;
  ring.completion_head = nullptr;
  ring.completion_tail = nullptr;
  ring.completion_mask = nullptr;
  ring.completion_count = nullptr;
  ring.completion_mapping = MAP_FAILED;
  ring.submission_mapping_size = 0;
  ring.completion_mapping_size = 0;
  ring.entry_mapping_size = 0;
  ring.owner_process_id = -1;
  ring.has_shared_mapping = false;
  ring.has_read = false;
  ring.has_write = false;
  ring.has_stat = false;
}

io_uring_batch::~io_uring_batch() { close_io_uring_batch(*this); }

static fn io_uring_supports(const io_uring_probe &probe, u8 opcode) wontthrow
    -> bool
{
  for (u8 index = 0; index < probe.ops_len; index++) {
    let const &operation = probe.ops[index];
    if (operation.op == opcode &&
        (operation.flags & IO_URING_OP_SUPPORTED) != 0)
    {
      return true;
    }
  }

  return false;
}

enum class io_uring_open_result : u8
{
  Opened,
  TransientFailure,
  Unavailable,
};

static pure fn classify_io_uring_open_failure(i32 error_number) wontthrow
    -> io_uring_open_result
{
  switch (error_number) {
  case EACCES:
  case EINVAL:
  case ENOSYS:
  case EOPNOTSUPP:
  case EPERM: return io_uring_open_result::Unavailable;
  default: return io_uring_open_result::TransientFailure;
  }
}

static fn open_io_uring_batch(io_uring_batch &ring) wontthrow
    -> io_uring_open_result
{
  ring.descriptor = static_cast<i32>(
      ::syscall(SYS_io_uring_setup, IO_URING_ENTRY_COUNT, &ring.parameters));
  if (ring.descriptor < 0) return classify_io_uring_open_failure(errno);
  ring.owner_process_id = get_current_process_id();

  alignas(io_uring_probe)
      u8 probe_storage[sizeof(io_uring_probe) +
                       (IORING_OP_LAST + 1) * sizeof(io_uring_probe_op)]{};
  let &probe = *reinterpret_cast<io_uring_probe *>(probe_storage);
  let const probe_result =
      ::syscall(SYS_io_uring_register, ring.descriptor, IORING_REGISTER_PROBE,
                &probe, static_cast<u32>(IORING_OP_LAST + 1));
  if (probe_result < 0) {
    let const error_number = errno;
    close_io_uring_batch(ring);
    return classify_io_uring_open_failure(error_number);
  }
  ring.has_read = io_uring_supports(probe, IORING_OP_READ);
  ring.has_write = io_uring_supports(probe, IORING_OP_WRITE);
  ring.has_stat = io_uring_supports(probe, IORING_OP_STATX);

  ring.submission_mapping_size =
      ring.parameters.sq_off.array + ring.parameters.sq_entries * sizeof(u32);
  ring.completion_mapping_size =
      ring.parameters.cq_off.cqes +
      ring.parameters.cq_entries * sizeof(io_uring_cqe);
  ring.entry_mapping_size = ring.parameters.sq_entries * sizeof(io_uring_sqe);
  ring.has_shared_mapping =
      (ring.parameters.features & IORING_FEAT_SINGLE_MMAP) != 0;
  if (ring.has_shared_mapping &&
      ring.completion_mapping_size > ring.submission_mapping_size)
  {
    ring.submission_mapping_size = ring.completion_mapping_size;
  }

  ring.submission_mapping =
      ::mmap(nullptr, ring.submission_mapping_size, PROT_READ | PROT_WRITE,
             MAP_SHARED, ring.descriptor, IORING_OFF_SQ_RING);
  if (ring.submission_mapping == MAP_FAILED) {
    close_io_uring_batch(ring);
    return io_uring_open_result::TransientFailure;
  }

  if (ring.has_shared_mapping) {
    ring.completion_mapping = ring.submission_mapping;
  } else {
    ring.completion_mapping =
        ::mmap(nullptr, ring.completion_mapping_size, PROT_READ | PROT_WRITE,
               MAP_SHARED, ring.descriptor, IORING_OFF_CQ_RING);
    if (ring.completion_mapping == MAP_FAILED) {
      close_io_uring_batch(ring);
      return io_uring_open_result::TransientFailure;
    }
  }

  ring.entry_mapping =
      ::mmap(nullptr, ring.entry_mapping_size, PROT_READ | PROT_WRITE,
             MAP_SHARED, ring.descriptor, IORING_OFF_SQES);
  if (ring.entry_mapping == MAP_FAILED) {
    close_io_uring_batch(ring);
    return io_uring_open_result::TransientFailure;
  }

  let *submission_bytes = static_cast<u8 *>(ring.submission_mapping);
  let *completion_bytes = static_cast<u8 *>(ring.completion_mapping);
  ring.submission_head =
      reinterpret_cast<u32 *>(submission_bytes + ring.parameters.sq_off.head);
  ring.submission_tail =
      reinterpret_cast<u32 *>(submission_bytes + ring.parameters.sq_off.tail);
  ring.submission_mask = reinterpret_cast<u32 *>(
      submission_bytes + ring.parameters.sq_off.ring_mask);
  ring.submission_count = reinterpret_cast<u32 *>(
      submission_bytes + ring.parameters.sq_off.ring_entries);
  ring.submission_array =
      reinterpret_cast<u32 *>(submission_bytes + ring.parameters.sq_off.array);
  ring.completion_head =
      reinterpret_cast<u32 *>(completion_bytes + ring.parameters.cq_off.head);
  ring.completion_tail =
      reinterpret_cast<u32 *>(completion_bytes + ring.parameters.cq_off.tail);
  ring.completion_mask = reinterpret_cast<u32 *>(
      completion_bytes + ring.parameters.cq_off.ring_mask);
  ring.completion_count = reinterpret_cast<u32 *>(
      completion_bytes + ring.parameters.cq_off.ring_entries);
  ring.completion_entries = reinterpret_cast<io_uring_cqe *>(
      completion_bytes + ring.parameters.cq_off.cqes);
  ring.submission_entries = static_cast<io_uring_sqe *>(ring.entry_mapping);

  let const is_usable = *ring.submission_count >= IO_URING_ENTRY_COUNT &&
                        *ring.completion_count >= IO_URING_ENTRY_COUNT;
  if (!is_usable) {
    close_io_uring_batch(ring);
    return io_uring_open_result::Unavailable;
  }
  return io_uring_open_result::Opened;
}

static fn io_uring_batch_supports_operations(const io_uring_batch &ring,
                                             const batched_syscall *operations,
                                             usize operation_count) wontthrow
    -> bool
{
  for (usize index = 0; index < operation_count; index++) {
    switch (batch_operation_access::get_kind(operations[index])) {
    case batched_syscall_id::Read:
      if (!ring.has_read) return false;
      break;
    case batched_syscall_id::Write:
      if (!ring.has_write) return false;
      break;
    case batched_syscall_id::WriteCurrent:
      if (!ring.has_write ||
          (ring.parameters.features & IORING_FEAT_RW_CUR_POS) == 0)
      {
        return false;
      }
      for (usize previous_index = 0; previous_index < index; previous_index++) {
        if (batch_operation_access::get_kind(operations[previous_index]) ==
                batched_syscall_id::WriteCurrent &&
            batch_operation_access::get_descriptor(
                operations[previous_index]) ==
                batch_operation_access::get_descriptor(operations[index]))
        {
          return false;
        }
      }
      break;
    case batched_syscall_id::Lstat:
    case batched_syscall_id::Stat:
    case batched_syscall_id::Exists:
      if (!ring.has_stat) return false;
      break;
    case batched_syscall_id::LstatAt:
    case batched_syscall_id::StatAt: return false;
    case batched_syscall_id::Invalid: return false;
    }
  }

  return true;
}

static fn execute_io_uring_batch(const batched_syscall *operations,
                                 usize operation_count,
                                 batched_syscall_result *results) wontthrow
    -> bool
{
  if (operation_count < 32) return false;
  for (usize index = 0; index < operation_count; index++) {
    let const &operation = operations[index];
    let const operation_kind = batch_operation_access::get_kind(operation);
    switch (operation_kind) {
    case batched_syscall_id::Read:
    case batched_syscall_id::Write:
    case batched_syscall_id::WriteCurrent:
      if (operation.byte_count > UINT32_MAX) return false;
      break;
    case batched_syscall_id::Lstat:
    case batched_syscall_id::Stat:
    case batched_syscall_id::Exists:
    case batched_syscall_id::LstatAt:
    case batched_syscall_id::StatAt:
    case batched_syscall_id::Invalid: break;
    }
  }

  static thread_local io_uring_batch ring{};
  static thread_local i64 unavailable_process_id = -1;
  let const process_id = get_current_process_id();
  if (ring.descriptor >= 0 && ring.owner_process_id != process_id)
    close_io_uring_batch(ring);
  if (ring.descriptor < 0) {
    if (unavailable_process_id == process_id) return false;

    let const open_result = open_io_uring_batch(ring);
    if (open_result == io_uring_open_result::Unavailable)
      unavailable_process_id = process_id;
    if (open_result != io_uring_open_result::Opened) return false;
    unavailable_process_id = -1;
  }
  if (!io_uring_batch_supports_operations(ring, operations, operation_count))
    return false;

  usize operation_start = 0;
  while (operation_start < operation_count) {
    if (INTERRUPT_REQUESTED) {
      for (usize remaining_index = operation_start;
           remaining_index < operation_count; remaining_index++)
      {
        results[remaining_index] = {operations[remaining_index].request_id, 0,
                                    EINTR};
      }
      return true;
    }

    let const available_count = static_cast<usize>(*ring.submission_count);
    let const chunk_count = operation_count - operation_start > available_count
                                ? available_count
                                : operation_count - operation_start;
    struct statx status_records[IO_URING_ENTRY_COUNT]{};
    bool was_queued[IO_URING_ENTRY_COUNT]{};
    bool was_completed[IO_URING_ENTRY_COUNT]{};
    let const initial_tail =
        __atomic_load_n(ring.submission_tail, __ATOMIC_ACQUIRE);
    u32 queued_count = 0;

    for (usize chunk_index = 0; chunk_index < chunk_count; chunk_index++) {
      let const operation_index = operation_start + chunk_index;
      let const &operation = operations[operation_index];
      results[operation_index] = {operation.request_id, 0,
                                  validate_batched_syscall(operation)};
      if (results[operation_index].error_number != 0) continue;

      let const submission_index =
          (initial_tail + queued_count) & *ring.submission_mask;
      let &entry = ring.submission_entries[submission_index];
      entry = {};
      entry.user_data = operation_index;
      switch (batch_operation_access::get_kind(operation)) {
      case batched_syscall_id::Read:
        entry.opcode = IORING_OP_READ;
        entry.fd = batch_operation_access::get_descriptor(operation);
        entry.off = operation.byte_offset;
        entry.addr = reinterpret_cast<u64>(
            batch_operation_access::get_output_buffer(operation));
        entry.len = static_cast<u32>(operation.byte_count);
        break;
      case batched_syscall_id::Write:
        entry.opcode = IORING_OP_WRITE;
        entry.fd = batch_operation_access::get_descriptor(operation);
        entry.off = operation.byte_offset;
        entry.addr = reinterpret_cast<u64>(
            batch_operation_access::get_input_buffer(operation));
        entry.len = static_cast<u32>(operation.byte_count);
        break;
      case batched_syscall_id::WriteCurrent:
        entry.opcode = IORING_OP_WRITE;
        entry.fd = batch_operation_access::get_descriptor(operation);
        entry.off = UINT64_MAX;
        entry.addr = reinterpret_cast<u64>(
            batch_operation_access::get_input_buffer(operation));
        entry.len = static_cast<u32>(operation.byte_count);
        break;
      case batched_syscall_id::Lstat:
      case batched_syscall_id::Stat:
      case batched_syscall_id::Exists:
        entry.opcode = IORING_OP_STATX;
        entry.fd = AT_FDCWD;
        entry.addr = reinterpret_cast<u64>(
            batch_operation_access::get_path(operation)->c_str());
        entry.len = STATX_BASIC_STATS;
        entry.statx_flags = batch_operation_access::get_kind(operation) ==
                                    batched_syscall_id::Lstat
                                ? AT_SYMLINK_NOFOLLOW
                                : 0;
        entry.addr2 = reinterpret_cast<u64>(&status_records[chunk_index]);
        break;
      case batched_syscall_id::LstatAt:
      case batched_syscall_id::StatAt:
        entry.opcode = IORING_OP_STATX;
        entry.fd = batch_operation_access::get_directory_descriptor(operation);
        entry.addr = reinterpret_cast<u64>(
            batch_operation_access::get_relative_name(operation));
        entry.len = STATX_BASIC_STATS;
        entry.statx_flags = batch_operation_access::get_kind(operation) ==
                                    batched_syscall_id::LstatAt
                                ? AT_SYMLINK_NOFOLLOW
                                : 0;
        entry.addr2 = reinterpret_cast<u64>(&status_records[chunk_index]);
        break;
      case batched_syscall_id::Invalid: continue;
      }
      ring.submission_array[submission_index] = submission_index;
      was_queued[chunk_index] = true;
      queued_count++;
    }

    if (queued_count == 0) {
      operation_start += chunk_count;
      continue;
    }

    let const published_tail = initial_tail + queued_count;
    __atomic_store_n(ring.submission_tail, published_tail, __ATOMIC_RELEASE);
    let const do_fail_unfinished = [&](i32 error_number) wontthrow -> void {
      close_io_uring_batch(ring);
      for (usize chunk_index = 0; chunk_index < chunk_count; chunk_index++) {
        if (!was_queued[chunk_index] || was_completed[chunk_index]) continue;
        results[operation_start + chunk_index].error_number = error_number;
      }
      for (usize remaining_index = operation_start + chunk_count;
           remaining_index < operation_count; remaining_index++)
      {
        results[remaining_index] = {operations[remaining_index].request_id, 0,
                                    error_number};
      }
    };
    while (__atomic_load_n(ring.submission_head, __ATOMIC_ACQUIRE) !=
           published_tail)
    {
      let const current_head =
          __atomic_load_n(ring.submission_head, __ATOMIC_ACQUIRE);
      let const pending_count = published_tail - current_head;
      let const submitted_count = ::syscall(SYS_io_uring_enter, ring.descriptor,
                                            pending_count, 0, 0, nullptr, 0);
      if (submitted_count < 0 && errno == EINTR) {
        if (!INTERRUPT_REQUESTED) continue;
        do_fail_unfinished(EINTR);
        return true;
      }
      if (submitted_count < 0) {
        let const error_number = errno;
        do_fail_unfinished(error_number);
        return true;
      }
    }

    u32 completed_count = 0;
    while (completed_count < queued_count) {
      let completion_head =
          __atomic_load_n(ring.completion_head, __ATOMIC_ACQUIRE);
      let const completion_tail =
          __atomic_load_n(ring.completion_tail, __ATOMIC_ACQUIRE);
      if (completion_head == completion_tail) {
        let const wait_result =
            ::syscall(SYS_io_uring_enter, ring.descriptor, 0,
                      static_cast<u32>(queued_count - completed_count),
                      IORING_ENTER_GETEVENTS, nullptr, 0);
        if (wait_result < 0 && errno == EINTR) {
          if (!INTERRUPT_REQUESTED) continue;
          do_fail_unfinished(EINTR);
          return true;
        }
        if (wait_result < 0) {
          let const error_number = errno;
          do_fail_unfinished(error_number);
          return true;
        }
        continue;
      }

      while (completion_head != completion_tail &&
             completed_count < queued_count)
      {
        let const &completion =
            ring.completion_entries[completion_head & *ring.completion_mask];
        let const operation_index = static_cast<usize>(completion.user_data);
        let const chunk_index = operation_index - operation_start;
        let &result = results[operation_index];
        let const operation_kind =
            batch_operation_access::get_kind(operations[operation_index]);
        if (completion.res < 0) {
          let const error_number = -completion.res;
          switch (operation_kind) {
          case batched_syscall_id::Exists:
            execute_batched_syscall_direct(operations[operation_index], result);
            break;
          case batched_syscall_id::Read:
          case batched_syscall_id::Write:
          case batched_syscall_id::WriteCurrent:
            if (error_number == EOPNOTSUPP || error_number == EINVAL ||
                error_number == ESPIPE)
            {
              execute_batched_syscall_direct(operations[operation_index],
                                             result);
            } else {
              result.error_number = error_number;
            }
            break;
          case batched_syscall_id::Lstat:
          case batched_syscall_id::Stat:
          case batched_syscall_id::LstatAt:
          case batched_syscall_id::StatAt:
            if (error_number == EOPNOTSUPP || error_number == EINVAL) {
              execute_batched_syscall_direct(operations[operation_index],
                                             result);
            } else {
              result.error_number = error_number;
            }
            break;
          case batched_syscall_id::Invalid:
            result.error_number = error_number;
            break;
          }
        } else {
          switch (operation_kind) {
          case batched_syscall_id::Exists: result.is_existing = true; break;
          case batched_syscall_id::Lstat:
          case batched_syscall_id::Stat:
          case batched_syscall_id::LstatAt:
          case batched_syscall_id::StatAt:
            fill_file_status(status_records[chunk_index],
                             *batch_operation_access::get_status(
                                 operations[operation_index]));
            break;
          case batched_syscall_id::Read:
          case batched_syscall_id::Write:
          case batched_syscall_id::WriteCurrent:
            result.transferred_byte_count = static_cast<usize>(completion.res);
            break;
          case batched_syscall_id::Invalid: break;
          }
        }
        was_completed[chunk_index] = true;
        completion_head++;
        completed_count++;
      }
      __atomic_store_n(ring.completion_head, completion_head, __ATOMIC_RELEASE);
    }

    operation_start += chunk_count;
  }

  return true;
}

static fn execute_native_batch(const batched_syscall *operations,
                               usize operation_count,
                               batched_syscall_result *results) wontthrow
    -> bool
{
  return execute_io_uring_batch(operations, operation_count, results);
}

#else

static fn execute_native_batch(const batched_syscall *operations,
                               usize operation_count,
                               batched_syscall_result *results) wontthrow
    -> bool
{
  unused(operations);
  unused(operation_count);
  unused(results);
  return false;
}

#endif

fn execute_batch_operations(const batched_syscall *operations,
                            usize operation_count,
                            batched_syscall_result *results) wontthrow -> void
{
  if (operation_count == 0) return;
  if (operations == nullptr || results == nullptr) return;

  let const saved_error = errno;
  defer { errno = saved_error; };

  if (INTERRUPT_REQUESTED) {
    for (usize index = 0; index < operation_count; index++)
      results[index] = {operations[index].request_id, 0, EINTR};
    return;
  }

  if (execute_native_batch(operations, operation_count, results)) return;

  for (usize index = 0; index < operation_count; index++) {
    execute_batched_syscall_direct(operations[index], results[index]);
    if (INTERRUPT_REQUESTED) {
      for (usize remaining_index = index + 1; remaining_index < operation_count;
           remaining_index++)
      {
        results[remaining_index] = {operations[remaining_index].request_id, 0,
                                    EINTR};
      }
      return;
    }
  }
}

} // namespace batch_internal

} // namespace os
} // namespace koshka
