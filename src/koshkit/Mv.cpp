#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Path.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-fv] source ... destination");

HELP_DESCRIPTION_DECL("The mv utility renames each source to the destination.");

FLAG(MV_FORCE, Bool, 'f', "", "Overwrite an existing destination.");
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
        "mv: unable to open '" + String{allocator, source}
          +
        "': " + os::last_system_error_message()
    };
  defer { os::close_fd(*in_fd); };

  let const out_fd =
      os::open_file_descriptor(destination, os::file_open_mode::Truncate);
  if (!out_fd.has_value())
    throw Error{
        "mv: unable to create '" + String{allocator, destination}
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
          "mv: a read of '" + String{allocator, source}
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
            "mv: a write to '" + String{allocator, destination}
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

  if (source_path.is_symbolic_link()) {
    let const link_target = os::read_symlink(source);
    if (!link_target.has_value()) return false;

    os::remove_file(target);
    if (!os::create_symlink(link_target->view(), target)) return false;

    return os::remove_file(source);
  }

  if (source_path.is_directory()) return false;

  copy_file_contents(source, target, allocator);

  os::file_status source_status{};
  if (os::stat_path(source, source_status))
    os::set_file_mode(target, source_status.mode);

  return os::remove_file(source);
}

Mv::Mv() = default;

pure fn Mv::kind() const wontthrow -> Utility::Kind { return Kind::Mv; }

fn Mv::execute(const ExecContext &ec, EvalContext &cxt,
               const ArrayList<String> &args,
               const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let const operands = parse_util_operands(FLAG_LIST, args, &arg_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  if (operands.count() < 2) return report_usage_error(ec, cxt, args[0].view());

  let const destination = operands[operands.count() - 1].view();
  let const destination_is_directory = Path{destination}.is_directory();

  if (operands.count() > 2 && !destination_is_directory) {
    throw Error{
        "mv: the destination '" + String{cxt.scratch_allocator(), destination}
          +
        "' is not a directory, so it cannot hold several sources"
    };
  }

  let output = String{cxt.scratch_allocator()};
  i32 status = 0;
  for (usize i = 0; i + 1 < operands.count(); i++) {
    let const source = operands[i].view();
    let target = String{cxt.scratch_allocator(), destination};
    if (destination_is_directory)
      target = PathBuilder{destination}
                   .append(Path{source}.filename())
                   .build()
                   .text();

    if (Path{source}.is_same_file_as(Path{target.view()})) {
      report_soft_koshkit_error(ec, cxt,
                                "mv: '" +
                                    String{cxt.scratch_allocator(), source} +
                                    "' and '" + target + "' are the same file");
      status = 1;
      continue;
    }

    if (!os::rename_path(source, target.view())) {
      let const rename_error_number = errno;
      if (rename_error_number != EXDEV ||
          !move_across_devices(source, target.view(), cxt.scratch_allocator()))
      {
        report_soft_koshkit_error(ec, cxt,
                                  "mv: unable to move '" +
                                      String{cxt.scratch_allocator(), source} +
                                      "' to '" + target + "' because " +
                                      os::last_system_error_message());
        status = 1;
        continue;
      }
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
