#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Path.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("path [suffix]");

HELP_DESCRIPTION_DECL(
    "The basename utility prints the final component of a path.");

FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Basename);

namespace koshka {

namespace koshkit {

static fn basename_component(StringView path) wontthrow -> StringView
{
  usize end_position = path.length;
  while (end_position > 0 && os::is_directory_separator(path[end_position - 1]))
    end_position--;

  if (end_position == 0) {
    if (path.is_empty()) return path;
    return StringView{"/"};
  }

  usize start_position = end_position;
  while (start_position > 0 &&
         !os::is_directory_separator(path[start_position - 1]))
    start_position--;

  return path.substring_of_length(start_position,
                                  end_position - start_position);
}

Basename::Basename() = default;

pure fn Basename::kind() const wontthrow -> Utility::Kind
{
  return Kind::Basename;
}

fn Basename::execute(
    const ExecContext &ec, EvalContext &cxt, const ArrayList<String> &args,
    const ArrayList<SourceLocation> &arg_locations) const throws -> i32
{
  unused(cxt);
  let const operands = parse_util_operands(FLAG_LIST, args, &arg_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  if (operands.is_empty()) return report_usage_error(ec, cxt, args[0].view());
  if (operands.count() > 2) {
    report_soft_koshkit_error(
        ec, cxt, "basename: extra operand '" + operands[2] + "'",
        "basename takes a path and an optional suffix, e.g. `basename a.c .c`");
    return 1;
  }

  let name = basename_component(operands[0].view());
  if (operands.count() > 1) {
    let const suffix = operands[1].view();
    if (suffix.length < name.length &&
        name.substring_of_length(name.length - suffix.length, suffix.length) ==
            suffix)
    {
      name = name.substring_of_length(0, name.length - suffix.length);
    }
  }

  ec.print_to_stdout(String{cxt.scratch_allocator(), name} + "\n");
  return 0;
}

} // namespace koshkit

} // namespace koshka
