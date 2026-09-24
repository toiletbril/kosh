/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements the cp utility. It copies files and directory trees,
 * handles overwrite policy, and optionally preserves modes and timestamps.
 */

#include "../CLI.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../base/Path.hpp"

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

namespace {

enum class cp_recursive_mode : u8
{
  SinglePath,
  Recursive,
};

}

static fn report_copy_error(const ExecContext &ec, EvalContext &cxt,
                            StringView utility_name,
                            const Error &error) throws -> void
{
  if (error.detail_message().is_empty()) {
    report_soft_koshkit_util_error(ec, cxt, utility_name,
                                   error.message().view());
    return;
  }

  report_soft_koshkit_util_error(ec, cxt, utility_name,
                                 error.message().view(),
                                 error.detail_message());
}

static fn copy_file(const ExecContext &ec, StringView source,
                    StringView destination, bool should_force, bool is_verbose,
                    Allocator allocator) throws -> void
{
  switch (copy_file_contents(source, destination, should_force)) {
  case copy_file_result::SourceOpenFailed:
    throw Error{
        "unable to open '" + String{allocator, source}
          +
        "': " + os::last_system_error_message()
    };
  case copy_file_result::DestinationOpenFailed:
    throw Error{
        "unable to create '" + String{allocator, destination}
          +
        "': " + os::last_system_error_message()
    };
  case copy_file_result::ReadFailed:
    throw Error{
        "a read of '" + String{allocator, source}
          +
        "' failed: " + os::last_system_error_message()
    };
  case copy_file_result::WriteFailed:
    throw Error{
        "a write to '" + String{allocator, destination}
          +
        "' failed: " + os::last_system_error_message()
    };
  case copy_file_result::Success: break;
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

static fn copy_path(const ExecContext &ec, EvalContext &cxt,
                    StringView utility_name, StringView source,
                    StringView destination,
                    bool should_force, bool should_preserve, bool is_verbose,
                    Allocator allocator,
                    const os::file_status *known_lstat,
                    cp_recursive_mode recursive_mode) throws
    -> bool
{
  let const source_path = Path{source};
  let const destination_path = Path{destination};
  if (destination_path.exists() &&
      source_path.is_same_file_as(destination_path))
  {
    throw Error{
        "'" + String{allocator, source     }
          + "' and '" +
        String{allocator, destination}
          + "' are the same file"
    };
  }
  let const is_source_symlink =
      known_lstat != nullptr
          ? os::file_type_letter(known_lstat->mode) == 'l'
          : source_path.is_symbolic_link();

  if (is_source_symlink && recursive_mode == cp_recursive_mode::Recursive) {
    if (let const target = os::read_symlink(source, allocator)) {
      /* Symlink creation fails when the path is already present, so an existing
         destination is removed first. */
      if ((destination_path.exists() || destination_path.is_symbolic_link()) &&
          !os::remove_file(destination))
      {
        throw Error{
            "unable to remove '" + String{allocator, destination}
              +
            "': " + os::last_system_error_message()
        };
      }
      if (!os::create_symlink(target->view(), destination)) {
        throw Error{
            "unable to create the symlink '" + String{allocator, destination}
              +
            "': " + os::last_system_error_message()
        };
      }

      if (is_verbose)
        ec.print_to_stdout("'" + String{allocator, source} + "' -> '" +
                           String{allocator, destination} + "'\n");

      return true;
    }
  }

  let source_status = Maybe<os::file_status>{};
  if (known_lstat != nullptr && !is_source_symlink)
    source_status = *known_lstat;
  else
    source_status = source_file_status(source);

  /* A symlink is excluded so a link back into the tree does not drive an
     unbounded walk. */
  let const is_source_directory =
      source_status.has_value()
          ? os::file_type_letter(source_status->mode) == 'd'
          : source_path.is_directory();
  if (is_source_directory && !is_source_symlink) {
    if (recursive_mode == cp_recursive_mode::SinglePath)
      throw Error{
          "'" + String{allocator, source}
            +
          "' is a directory, pass -r to copy it"
      };

    let const source_absolute = Path{source}.to_absolute().normalized();
    let const destination_absolute =
        Path{destination}.to_absolute().normalized();
    let source_prefix = source_absolute.text().clone();
    source_prefix.push(os::DIRECTORY_SEPARATOR);
    if (destination_absolute.view() == source_absolute.view() ||
        destination_absolute.view().starts_with(source_prefix.view()))
    {
      throw ErrorWithDetails{
          "cannot copy '" + String{allocator, source}
            + "' into itself",
          "The destination is inside the source directory"
      };
    }

    let const did_destination_exist = Path{destination}.is_directory();
    os::make_directory(destination, 0700);
    let const directory_scratch = cxt.scratch_mark();
    defer { cxt.scratch_release(directory_scratch); };
    let names = os::list_directory_status(source, allocator);
    if (!names.has_value())
      throw Error{
          "unable to read the directory '" + String{allocator, source}
            +
          "': " + os::last_system_error_message()
      };

    bool did_succeed = true;
    for (let const &entry : *names) {
      if (os::INTERRUPT_REQUESTED) return false;
      let const child_scratch = cxt.scratch_mark();
      defer { cxt.scratch_release(child_scratch); };
      let child_source = Path{source, allocator};
      child_source.append(entry.child.name.view());
      let child_destination = Path{destination, allocator};
      child_destination.append(entry.child.name.view());
      try {
        if (!copy_path(ec, cxt, utility_name, child_source.view(),
                       child_destination.view(), should_force, should_preserve,
                       is_verbose, allocator,
                       entry.has_status ? &entry.status : nullptr,
                       recursive_mode))
          did_succeed = false;
      } catch (const BrokenPipeExit &) {
        throw;
      } catch (const Error &error) {
        report_copy_error(ec, cxt, utility_name, error);
        did_succeed = false;
      }
      if (os::INTERRUPT_REQUESTED) return false;
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
          "unable to preserve timestamps for '" +
          String{allocator, destination}
          +
          "': " + os::last_system_error_message()
      };
    }

    return did_succeed;
  }

  /* A destination symlink is removed so the copy does not follow the link and
     truncate its target. */
  if (destination_path.is_symbolic_link() && !os::remove_file(destination)) {
    throw Error{
        "unable to remove '" + String{allocator, destination}
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
        "unable to preserve timestamps for '" + String{allocator, destination}
          +
        "': " + os::last_system_error_message()
    };
  }

  return true;
}

Cp::Cp() = default;

pure fn Cp::kind() const wontthrow -> Utility::Kind { return Kind::Cp; }

fn Cp::execute(const ExecContext &ec, EvalContext &cxt,
               const ArrayList<String> &args,
               const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let const[operands, operand_locations] = parse_util_operands(
      FLAG_LIST, args, cxt.scratch_allocator(), &arg_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  if (operands.count() < 2) return report_usage_error(ec, cxt, args[0].view());

  let const recursive_mode =
      FLAG_CP_RECURSIVE_R.is_enabled() || FLAG_CP_RECURSIVE_UPPER.is_enabled()
          ? cp_recursive_mode::Recursive
          : cp_recursive_mode::SinglePath;
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
        "the destination '" + String{cxt.scratch_allocator(), destination}
          +
        "' is not a directory, so it cannot hold several sources"
    };
  }

  i32 status = 0;
  for (usize i = 0; i + 1 < operands.count(); i++) {
    let const source = operands[i].view();
    let target = String{cxt.scratch_allocator(), destination};
    if (is_destination_directory) {
      /* The Path is held in a named local so the basename view does not dangle
         into a destroyed temporary. */
      let const source_path = Path{source};
      let const leaf = source_path.filename();
      let target_path = Path{destination};
      target_path.append(leaf);
      target = target_path.text();
    }

    if (should_prompt && Path{target.view()}.exists() &&
        !confirm_koshkit_action(ec, "overwrite '" + target + "'? "))
      continue;

    try {
      if (!copy_path(ec, cxt, args[0].view(), source, target.view(),
                     should_force, should_preserve, is_verbose,
                     cxt.scratch_allocator(), nullptr, recursive_mode))
        status = 1;
    } catch (const BrokenPipeExit &) {
      throw;
    } catch (const Error &error) {
      report_copy_error(ec, cxt, args[0].view(), error);
      status = 1;
    }
    if (os::INTERRUPT_REQUESTED) return 130;
  }

  return status;
}

} /* namespace koshkit */

} /* namespace koshka */
