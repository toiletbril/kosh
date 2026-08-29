#include "Ownership.hpp"

#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Platform.hpp"

namespace koshka::koshkit {

struct ownership_directory_identity
{
  u64 device_id;
  u64 file_id;
};

static fn parse_numeric_id(StringView text) wontthrow -> Maybe<u32>
{
  if (text.is_empty()) return None;
  u64 value = 0;
  for (usize position = 0; position < text.length; position++) {
    let const byte = text[position];
    if (byte < '0' || byte > '9') return None;
    value = value * 10 + static_cast<u64>(byte - '0');
    if (value > UINT32_MAX) return None;
  }
  return static_cast<u32>(value);
}

fn resolve_user_id(StringView text) throws -> Maybe<u32>
{
  if (let const numeric = parse_numeric_id(text); numeric.has_value())
    return numeric;
  return os::username_to_uid(text);
}

fn resolve_group_id(StringView text) throws -> Maybe<u32>
{
  if (let const numeric = parse_numeric_id(text); numeric.has_value())
    return numeric;
  return os::groupname_to_gid(text);
}

static fn change_path_ownership_recursive(
    const ExecContext &ec, EvalContext &cxt, StringView utility_name,
    const Path &path, i64 owner_id, i64 group_id, bool should_recurse,
    bool should_follow_symlink, bool should_follow_nested_symlinks,
    ArrayList<ownership_directory_identity> &active_directories) throws -> bool
{
  os::file_status path_status{};
  if (!os::stat_path(path.text().view(), path_status)) {
    report_soft_koshkit_error(ec, cxt,
                              utility_name + ": cannot access '" + path.text() +
                                  "': " + os::last_system_error_message());
    return false;
  }

  let const is_symlink = os::file_type_letter(path_status.mode) == 'l';
  let const does_follow = !is_symlink || should_follow_symlink;
  if (!os::set_file_owner(path.text().view(), owner_id, group_id, does_follow))
  {
    report_soft_koshkit_error(ec, cxt,
                              utility_name + ": cannot change ownership of '" +
                                  path.text() +
                                  "': " + os::last_system_error_message());
    return false;
  }

  if (!should_recurse) return true;
  os::file_status followed_status{};
  if (!os::stat_path_following(path.text().view(), followed_status))
    return true;
  if (os::file_type_letter(followed_status.mode) != 'd') return true;
  if (is_symlink && !should_follow_symlink) return true;

  if (followed_status.has_file_identity) {
    for (let const &identity : active_directories) {
      if (identity.device_id == followed_status.device_id &&
          identity.file_id == followed_status.file_id)
      {
        report_soft_koshkit_error(ec, cxt,
                                  utility_name +
                                      ": recursive directory loop at '" +
                                      path.text() + "'");
        return false;
      }
    }

    active_directories.push(
        {followed_status.device_id, followed_status.file_id});
  }
  defer
  {
    if (followed_status.has_file_identity) active_directories.pop_back();
  };

  let children = Path::read_directory(path);
  if (!children.has_value()) {
    report_soft_koshkit_error(ec, cxt,
                              utility_name + ": cannot read directory '" +
                                  path.text() +
                                  "': " + os::last_system_error_message());
    return false;
  }

  bool did_succeed = true;
  for (const String &name : *children) {
    let child = PathBuilder{path.text().view()}.append(name.view()).build();
    let const child_is_symlink = child.is_symbolic_link();
    if (!change_path_ownership_recursive(
            ec, cxt, utility_name, child, owner_id, group_id, true,
            child_is_symlink && should_follow_nested_symlinks,
            should_follow_nested_symlinks, active_directories))
      did_succeed = false;
  }

  return did_succeed;
}

fn change_path_ownership(const ExecContext &ec, EvalContext &cxt,
                         StringView utility_name, const Path &path,
                         i64 owner_id, i64 group_id, bool should_recurse,
                         bool should_follow_symlink,
                         bool should_follow_nested_symlinks) throws -> bool
{
  let active_directories =
      ArrayList<ownership_directory_identity>{cxt.scratch_allocator()};

  return change_path_ownership_recursive(
      ec, cxt, utility_name, path, owner_id, group_id, should_recurse,
      should_follow_symlink, should_follow_nested_symlinks, active_directories);
}

} // namespace koshka::koshkit
