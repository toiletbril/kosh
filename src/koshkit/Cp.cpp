#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Path.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-rRv] source ... destination");

HELP_DESCRIPTION_DECL("The cp utility copies each source to the destination.");

FLAG(CP_RECURSIVE_R, Bool, 'r', "", "Copy directories and their contents.");
FLAG(CP_RECURSIVE_UPPER, Bool, 'R', "", "Copy directories and their contents.");
FLAG(CP_VERBOSE, Bool, 'v', "", "Print the name of each copy as it happens.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Cp);

namespace koshka {

namespace koshkit {

static fn copy_file(const ExecContext &ec, StringView source,
                    StringView destination, bool is_verbose,
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

  let const out_fd =
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

static fn source_permission_bits(const Path &source_path,
                                 StringView source) throws -> Maybe<u32>
{
  os::file_status status{};
  StringView stat_target = source;

  Maybe<Path> resolved;
  if (source_path.is_symbolic_link()) {
    resolved = os::canonical_path(source_path);
    if (resolved.has_value()) stat_target = resolved->text().view();
  }

  if (!os::stat_path(stat_target, status)) return {};

  return status.mode & 0777;
}

static fn copy_path(const ExecContext &ec, StringView source,
                    StringView destination, bool is_recursive, bool is_verbose,
                    Allocator allocator) throws -> void
{
  let const source_path = Path{source};
  let const source_mode = source_permission_bits(source_path, source);

  if (source_path.is_symbolic_link() && is_recursive) {
    if (let const target = os::read_symlink(source)) {
      /* Symlink creation fails when the path is already present, so an existing
         destination is removed first. */
      os::remove_file(destination);
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

    let const destination_existed = Path{destination}.is_directory();
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
                is_recursive, is_verbose, allocator);
    }

    if (source_mode.has_value() && !destination_existed) {
      os::set_file_mode(destination,
                        *source_mode & ~os::get_file_creation_mask());
    }

    return;
  }

  /* A destination symlink is removed so the copy does not follow the link and
     truncate its target. */
  if (Path{destination}.is_symbolic_link()) os::remove_file(destination);

  let const destination_existed = Path{destination}.exists();
  copy_file(ec, source, destination, is_verbose, allocator);

  if (source_mode.has_value() && !destination_existed) {
    os::set_file_mode(destination,
                      *source_mode & ~os::get_file_creation_mask());
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
  let const is_verbose = FLAG_CP_VERBOSE.is_enabled();
  let const destination = operands[operands.count() - 1].view();
  let const destination_is_directory = Path{destination}.is_directory();

  if (operands.count() > 2 && !destination_is_directory) {
    throw Error{
        "cp: the destination '" + String{cxt.scratch_allocator(), destination}
          +
        "' is not a directory, so it cannot hold several sources"
    };
  }

  for (usize i = 0; i + 1 < operands.count(); i++) {
    let const source = operands[i].view();
    if (destination_is_directory) {
      /* The Path is held in a named local so the basename view does not dangle
         into a destroyed temporary. */
      let const source_path = Path{source};
      let const leaf = source_path.filename();
      let const target = PathBuilder{destination}.append(leaf).build();
      copy_path(ec, source, target.text().view(), is_recursive, is_verbose,
                cxt.scratch_allocator());
    } else {
      copy_path(ec, source, destination, is_recursive, is_verbose,
                cxt.scratch_allocator());
    }
  }

  return 0;
}

} // namespace koshkit

} // namespace koshka
