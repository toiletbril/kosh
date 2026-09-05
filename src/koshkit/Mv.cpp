/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements the mv utility. It handles multiple sources and
 * overwrite policy, uses atomic renames, and falls back to copy and removal
 * across filesystems.
 */

#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Path.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-fiv] source ... destination");

HELP_DESCRIPTION_DECL("The mv utility renames each source to the destination.");

FLAG(MV_FORCE, Bool, 'f', "", "Overwrite an existing destination.");
FLAG(MV_INTERACTIVE, Bool, 'i', "", "Ask before overwriting a destination.");
FLAG(MV_VERBOSE, Bool, 'v', "", "Print the name of each move as it happens.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Mv);

namespace koshka {

namespace koshkit {

static fn copy_file_contents(StringView source, StringView destination,
                             Allocator allocator) throws -> void
{
  let const in_fd = os::open_file_descriptor(source, os::file_open_mode::Read);
  if (!in_fd.has_value())
    throw Error{
        "unable to open '" + String{allocator, source}
          +
        "': " + os::last_system_error_message()
    };
  defer { os::close_fd(*in_fd); };

  let const out_fd =
      os::open_file_descriptor(destination, os::file_open_mode::Truncate);
  if (!out_fd.has_value())
    throw Error{
        "unable to create '" + String{allocator, destination}
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
          "a read of '" + String{allocator, source}
            +
          "' failed: " + os::last_system_error_message()
      };
    if (*read_count == 0) break;

    usize written_count = 0;
    while (written_count < *read_count) {
      let const chunk = os::write_fd(*out_fd, buffer + written_count,
                                     *read_count - written_count);
      if (!chunk.has_value() || *chunk == 0) {
        throw Error{
            "a write to '" + String{allocator, destination}
              +
            "' failed: " + os::last_system_error_message()
        };
      }

      written_count += *chunk;
    }
  }
}

static fn move_across_devices(StringView source, StringView target,
                              Allocator allocator) throws -> bool
{
  let const source_path = Path{source};
  let const is_source_symbolic_link = source_path.is_symbolic_link();
  if (!is_source_symbolic_link && source_path.is_directory()) return false;

  let const target_path = Path{target};
  let temporary_path = os::write_to_named_temp_file(target_path.parent(),
                                                    ".kosh_mv", StringView{});
  if (!temporary_path.has_value())
    throw Error{
        "unable to create a temporary file beside '" +
        String{allocator, target}
        + "': " + os::last_system_error_message()
    };
  defer { unused(os::remove_file(temporary_path->text().view())); };

  if (is_source_symbolic_link) {
    let const link_target = os::read_symlink(source, allocator);
    if (!link_target.has_value())
      throw Error{
          "unable to read the symlink '" + String{allocator, source}
            +
          "': " + os::last_system_error_message()
      };

    if (!os::remove_file(temporary_path->text().view()) ||
        !os::create_symlink(link_target->view(), temporary_path->text().view()))
    {
      throw Error{
          "unable to create a temporary symlink beside '" +
          String{allocator, target}
          + "': " + os::last_system_error_message()
      };
    }
  } else {
    copy_file_contents(source, temporary_path->text().view(), allocator);

    os::file_status source_status{};
    if (os::stat_path(source, source_status) &&
        !os::set_file_mode(temporary_path->text().view(), source_status.mode))
    {
      throw Error{
          "unable to preserve the mode of '" + String{allocator, source}
            +
          "': " + os::last_system_error_message()
      };
    }
  }

  if (!os::rename_path(temporary_path->text().view(), target))
    throw Error{
        "unable to publish '" + String{allocator, target}
          +
        "': " + os::last_system_error_message()
    };

  if (!os::remove_file(source))
    throw Error{
        "unable to remove '" + String{allocator, source}
          +
        "': " + os::last_system_error_message()
    };

  return true;
}

Mv::Mv() = default;

pure fn Mv::kind() const wontthrow -> Utility::Kind { return Kind::Mv; }

fn Mv::execute(const ExecContext &ec, EvalContext &cxt,
               const ArrayList<String> &args,
               const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let operand_locations = ArrayList<SourceLocation>{cxt.scratch_allocator()};
  let const operands =
      parse_util_operands(FLAG_LIST, args, &arg_locations, &operand_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  if (operands.count() < 2) return report_usage_error(ec, cxt, args[0].view());

  let const destination = operands[operands.count() - 1].view();
  let const is_destination_directory = Path{destination}.is_directory();
  let const should_prompt =
      FLAG_MV_INTERACTIVE.is_enabled() &&
      (!FLAG_MV_FORCE.is_enabled() ||
       FLAG_MV_INTERACTIVE.position() > FLAG_MV_FORCE.position());

  if (operands.count() > 2 && !is_destination_directory) {
    report_soft_koshkit_util_error(
        ec, cxt, operand_locations[operands.count() - 1], args[0].view(),
        "the destination '" + String{cxt.scratch_allocator(), destination} +
            "' is not a directory, so it cannot hold several sources");
    return 1;
  }

  let output = String{cxt.scratch_allocator()};
  i32 status = 0;
  for (usize i = 0; i + 1 < operands.count(); i++) {
    let const source = operands[i].view();
    let target = String{cxt.scratch_allocator(), destination};
    if (is_destination_directory)
      target = PathBuilder{destination}
                   .append(Path{source}.filename())
                   .build()
                   .text();

    if (Path{source}.is_same_file_as(Path{target.view()})) {
      report_soft_koshkit_util_error(
          ec, cxt, operand_locations[i], args[0].view(),
          "'" + String{cxt.scratch_allocator(), source} + "' and '" + target +
              "' are the same file");
      status = 1;
      continue;
    }

    if (should_prompt && Path{target.view()}.exists() &&
        !confirm_koshkit_action(ec, "mv: overwrite '" + target + "'? "))
      continue;

    try {
      if (!os::rename_path(source, target.view())) {
        let const rename_error_number = errno;
        if (rename_error_number != EXDEV ||
            !move_across_devices(source, target.view(),
                                 cxt.scratch_allocator()))
        {
          report_soft_koshkit_util_error(
              ec, cxt, operand_locations[i], args[0].view(),
              "unable to move '" + String{cxt.scratch_allocator(), source} +
                  "' to '" + target + "' because " +
                  os::last_system_error_message());
          status = 1;
          continue;
        }
      }
    } catch (Error &error) {
      report_soft_koshkit_util_error(ec, cxt, operand_locations[i],
                                     args[0].view(), error.message().view());
      status = 1;
      continue;
    }
    if (FLAG_MV_VERBOSE.is_enabled())
      output += "renamed '" + String{cxt.scratch_allocator(), source} +
                "' -> '" + target + "'\n";
  }
  ec.print_to_stdout(output);
  return status;
}

} /* namespace koshkit */

} /* namespace koshka */
