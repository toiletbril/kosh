#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Path.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("path");

HELP_DESCRIPTION_DECL(
    "The dirname utility prints the directory part of a path.");

FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Dirname);

namespace koshka {

namespace koshkit {

static pure fn is_directory_separator(char c) wontthrow -> bool
{
  return os::is_directory_separator(c);
}

static pure fn directory_part_of(StringView path) wontthrow -> StringView
{
  if (path.is_empty()) return StringView{"."};

  bool has_only_separators = true;
  for (usize i = 0; i < path.length; i++) {
    if (!is_directory_separator(path[i])) {
      has_only_separators = false;
      break;
    }
  }
  if (has_only_separators) return StringView{"/"};

  usize end_position = path.length;
  while (end_position > 0 && is_directory_separator(path[end_position - 1]))
    end_position--;

  bool has_separator = false;
  usize last_separator_position = 0;
  for (usize i = 0; i < end_position; i++) {
    if (is_directory_separator(path[i])) {
      has_separator = true;
      last_separator_position = i;
    }
  }
  if (!has_separator) return StringView{"."};

  end_position = last_separator_position;
  while (end_position > 0 && is_directory_separator(path[end_position - 1]))
    end_position--;

  if (end_position == 0) return StringView{"/"};

  return path.substring_of_length(0, end_position);
}

Dirname::Dirname() = default;

pure fn Dirname::kind() const wontthrow -> Utility::Kind
{
  return Kind::Dirname;
}

fn Dirname::execute(const ExecContext &ec, EvalContext &cxt,
                    const ArrayList<String> &args,
                    const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  unused(cxt);
  let const operands = parse_util_operands(FLAG_LIST, args, &arg_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  if (operands.is_empty()) {
    report_usage_error(ec, cxt, args[0].view());
    return 1;
  }

  let const text = directory_part_of(operands[0].view());
  ec.print_to_stdout(String{cxt.scratch_allocator(), text} + "\n");

  return 0;
}

} /* namespace koshkit */

} /* namespace koshka */
