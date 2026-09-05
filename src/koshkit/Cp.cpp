/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements the cp utility. It copies files and directory trees,
 * handles overwrite policy, and optionally preserves modes and timestamps.
 */

#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Path.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-fipRrv] source ... destination");

HELP_DESCRIPTION_DECL("The cp utility copies each source to the destination.");

FLAG(CP_RECURSIVE_R, Bool, 'r', "", "Copy directories and their contents.");
FLAG(CP_RECURSIVE_UPPER, Bool, 'R', "", "Copy directories and their contents.");
FLAG(CP_FORCE, Bool, 'f', "", "Remove a destination that cannot be opened.");
FLAG(CP_INTERACTIVE, Bool, 'i', "", "Ask before overwriting a destination.");
FLAG(CP_PRESERVE, Bool, 'p', "", "Preserve file mode and timestamps.");
FLAG(CP_VERBOSE, Bool, 'v', "", "Print the name of each copy as it happens.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Cp);

namespace koshka {

namespace koshkit {

static fn copy_file(const ExecContext &ec, StringView source,
                    StringView destination, bool should_force, bool is_verbose,
                    Allocator allocator) throws -> void
{
  let const in_fd = os::open_file_descriptor(source, os::file_open_mode::Read);
  if (!in_fd.has_value())
    throw Error{
        "cp: unable to open '" + String{allocator, source}
          +
        "': " + os::last_system_error_message()
    };
  defer { os::close_fd(*in_fd); };

  let out_fd =
      os::open_file_descriptor(destination, os::file_open_mode::Truncate);
  if (!out_fd.has_value() && should_force && os::remove_file(destination))
    out_fd =
        os::open_file_descriptor(destination, os::file_open_mode::Truncate);
  if (!out_fd.has_value())
    throw Error{
        "cp: unable to create '" + String{allocator, destination}
          +
        "': " + os::last_system_error_message()
    };
  defer { os::close_fd(*out_fd); };

  char buffer[4096];
  loop
  {
    let const read_count = os::read_fd(*in_fd, buffer, sizeof(buffer));
    if (!read_count.has_value())
      throw Error{
          "cp: a read of '" + String{allocator, source}
            +
          "' failed: " + os::last_system_error_message()
      };
    if (*read_count == 0) break;
    /* write_fd returns a single write's count that can fall short, so the
       remaining bytes are written in the loop. */
    usize written_count = 0;
    while (written_count < *read_count) {
      let const chunk = os::write_fd(*out_fd, buffer + written_count,
                                     *read_count - written_count);
      if (!chunk.has_value() || *chunk == 0) {
        throw Error{
            "cp: a write to '" + String{allocator, destination}
              +
            "' failed: " + os::last_system_error_message()
        };
      }

      written_count += *chunk;
    }
  }

  if (is_verbose)
    ec.print_to_stdout("'" + String{allocator, source} + "' -> '" +
                       String{allocator, destination} + "'\n");
}

static fn source_file_status(StringView source) throws -> Maybe<os::file_status>
{
  os::file_status status{};
  if (!os::stat_path_following(source, status)) return {};

  return status;
}

static fn copy_path(const ExecContext &ec, StringView source,
                    StringView destination, bool is_recursive,
                    bool should_force, bool should_preserve, bool is_verbose,
                    Allocator allocator) throws -> void
{
  let const source_path = Path{source};
  let const destination_path = Path{destination};
  if (destination_path.exists() &&
      source_path.is_same_file_as(destination_path))
  {
    throw Error{
        "cp: '" + String{allocator, source     }
          + "' and '" +
        String{allocator, destination}
          + "' are the same file"
    };
  }
  let const source_status = source_file_status(source);

  if (source_path.is_symbolic_link() && is_recursive) {
    if (let const target = os::read_symlink(source, allocator)) {
      /* Symlink creation fails when the path is already present, so an existing
         destination is removed first. */
      if ((destination_path.exists() || destination_path.is_symbolic_link()) &&
          !os::remove_file(destination))
      {
        throw Error{
            "cp: unable to remove '" + String{allocator, destination}
              +
            "': " + os::last_system_error_message()
        };
      }
      if (!os::create_symlink(target->view(), destination)) {
        throw Error{
            "cp: unable to create the symlink '" +
            String{allocator, destination}
            +
            "': " + os::last_system_error_message()
        };
      }

      if (is_verbose)
        ec.print_to_stdout("'" + String{allocator, source} + "' -> '" +
                           String{allocator, destination} + "'\n");

      return;
    }
  }

  /* A symlink is excluded so a link back into the tree does not drive an
     unbounded walk. */
  if (source_path.is_directory() && !source_path.is_symbolic_link()) {
    if (!is_recursive)
      throw Error{
          "cp: '" + String{allocator, source}
            +
          "' is a directory, pass -r to copy it"
      };

    let const source_absolute = Path{source}.to_absolute().normalized();
    let const destination_absolute =
        Path{destination}.to_absolute().normalized();
    let source_prefix = source_absolute.text().clone();
    source_prefix.push(os::DIRECTORY_SEPARATOR);
    if (destination_absolute.text().view() == source_absolute.text().view() ||
        destination_absolute.text().view().starts_with(source_prefix.view()))
    {
      throw ErrorWithDetails{
          "cp: cannot copy '" + String{allocator, source}
            + "' into itself",
          "The destination is inside the source directory"
      };
    }

    let const did_destination_exist = Path{destination}.is_directory();
    os::make_directory(destination, 0700);
    Maybe<ArrayList<String>> names = Path::read_directory(source_path);
    if (!names.has_value())
      throw Error{
          "cp: unable to read the directory '" + String{allocator, source}
            +
          "': " + os::last_system_error_message()
      };

    for (let const &name : *names) {
      let const child_source = PathBuilder{source}.append(name.view()).build();
      let const child_destination =
          PathBuilder{destination}.append(name.view()).build();
      copy_path(ec, child_source.text().view(), child_destination.text().view(),
                is_recursive, should_force, should_preserve, is_verbose,
                allocator);
    }

    if (source_status.has_value() &&
        (should_preserve || !did_destination_exist))
    {
      os::set_file_mode(destination, should_preserve
                                         ? source_status->mode & 0777
                                         : source_status->mode & 0777 &
                                               ~os::get_file_creation_mask());
    }
    if (source_status.has_value() && should_preserve &&
        !os::set_file_times(destination, source_status->access_time,
                            source_status->access_nanoseconds,
                            source_status->modification_time,
                            source_status->modification_nanoseconds))
    {
      throw Error{
          "cp: unable to preserve timestamps for '" +
          String{allocator, destination}
          +
          "': " + os::last_system_error_message()
      };
    }

    return;
  }

  /* A destination symlink is removed so the copy does not follow the link and
     truncate its target. */
  if (destination_path.is_symbolic_link() && !os::remove_file(destination)) {
    throw Error{
        "cp: unable to remove '" + String{allocator, destination}
          +
        "': " + os::last_system_error_message()
    };
  }

  let const did_destination_exist = Path{destination}.exists();
  copy_file(ec, source, destination, should_force, is_verbose, allocator);

  if (source_status.has_value() && (should_preserve || !did_destination_exist))
  {
    os::set_file_mode(destination, should_preserve
                                       ? source_status->mode & 0777
                                       : source_status->mode & 0777 &
                                             ~os::get_file_creation_mask());
  }
  if (source_status.has_value() && should_preserve &&
      !os::set_file_times(destination, source_status->access_time,
                          source_status->access_nanoseconds,
                          source_status->modification_time,
                          source_status->modification_nanoseconds))
  {
    throw Error{
        "cp: unable to preserve timestamps for '" +
        String{allocator, destination}
        +
        "': " + os::last_system_error_message()
    };
  }
}

Cp::Cp() = default;

pure fn Cp::kind() const wontthrow -> Utility::Kind { return Kind::Cp; }

fn Cp::execute(const ExecContext &ec, EvalContext &cxt,
               const ArrayList<String> &args,
               const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let const operands = parse_util_operands(FLAG_LIST, args, &arg_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  if (operands.count() < 2) return report_usage_error(ec, cxt, args[0].view());

  let const is_recursive =
      FLAG_CP_RECURSIVE_R.is_enabled() || FLAG_CP_RECURSIVE_UPPER.is_enabled();
  let const should_force = FLAG_CP_FORCE.is_enabled();
  let const should_preserve = FLAG_CP_PRESERVE.is_enabled();
  let const should_prompt = FLAG_CP_INTERACTIVE.is_enabled() &&
                            (!should_force || FLAG_CP_INTERACTIVE.position() >
                                                  FLAG_CP_FORCE.position());
  let const is_verbose = FLAG_CP_VERBOSE.is_enabled();
  let const destination = operands[operands.count() - 1].view();
  let const is_destination_directory = Path{destination}.is_directory();

  if (operands.count() > 2 && !is_destination_directory) {
    throw Error{
        "cp: the destination '" + String{cxt.scratch_allocator(), destination}
          +
        "' is not a directory, so it cannot hold several sources"
    };
  }

  for (usize i = 0; i + 1 < operands.count(); i++) {
    let const source = operands[i].view();
    let target = String{cxt.scratch_allocator(), destination};
    if (is_destination_directory) {
      /* The Path is held in a named local so the basename view does not dangle
         into a destroyed temporary. */
      let const source_path = Path{source};
      let const leaf = source_path.filename();
      target = PathBuilder{destination}.append(leaf).build().text();
    }

    if (should_prompt && Path{target.view()}.exists() &&
        !confirm_koshkit_action(ec, "cp: overwrite '" + target + "'? "))
      continue;

    copy_path(ec, source, target.view(), is_recursive, should_force,
              should_preserve, is_verbose, cxt.scratch_allocator());
  }

  return 0;
}

} /* namespace koshkit */

} /* namespace koshka */
