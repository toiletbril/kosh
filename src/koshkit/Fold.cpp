#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-bs] [-w width] [file ...]");

HELP_DESCRIPTION_DECL("The fold utility wraps input lines.");

FLAG(FOLD_BYTES, Bool, 'b', "bytes", "Count bytes instead of columns.");
FLAG(FOLD_SPACES, Bool, 's', "spaces", "Break at blanks when possible.");
FLAG(FOLD_WIDTH, String, 'w', "width", "Use this maximum width.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Fold);

namespace koshka::koshkit {

static fn append_folded_line(String &output, StringView line, usize width,
                             bool should_break_at_blanks) throws -> void
{
  usize start = 0;

  while (line.length - start > width) {
    usize break_length = width;
    if (should_break_at_blanks) {
      for (usize offset = width; offset > 0; offset--)
        if (std::isspace(static_cast<u8>(line[start + offset - 1])) != 0) {
          break_length = offset;
          break;
        }
    }

    output += line.substring_of_length(start, break_length);
    output += '\n';
    start += break_length;
  }

  output += line.substring(start);
  output += '\n';
}

Fold::Fold() = default;

pure fn Fold::kind() const wontthrow -> Utility::Kind { return Kind::Fold; }

fn Fold::execute(const ExecContext &ec, EvalContext &cxt,
                 const ArrayList<String> &args,
                 const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let const operands = parse_util_operands(FLAG_LIST, args, &arg_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  u64 width_value = 80;
  if (FLAG_FOLD_WIDTH.is_set()) {
    let const parsed = utils::parse_decimal_u64(FLAG_FOLD_WIDTH.value());
    if (parsed.is_error() || parsed.value() == 0 || parsed.value() > SIZE_MAX)
      throw Error{
          "fold: invalid width '" +
          String{cxt.scratch_allocator(), FLAG_FOLD_WIDTH.value()}
          + "'"
      };
    width_value = parsed.value();
  }
  let const width = static_cast<usize>(width_value);
  let const sources =
      source_list_from_operands(operands, cxt.scratch_allocator());
  let output = String{cxt.scratch_allocator()};
  i32 status = 0;

  for (let const source : sources) {
    let const content = read_named_or_stdin(ec, source);
    if (!content.has_value()) {
      report_soft_koshkit_error(ec, cxt,
                                "fold: cannot read '" +
                                    String{cxt.scratch_allocator(), source} +
                                    "': " + os::last_system_error_message());
      status = 1;
      continue;
    }

    usize line_start = 0;
    while (line_start < content->length()) {
      usize line_end = line_start;
      while (line_end < content->length() && (*content)[line_end] != '\n')
        line_end++;
      append_folded_line(output,
                         content->view().substring_of_length(
                             line_start, line_end - line_start),
                         width, FLAG_FOLD_SPACES.is_enabled());
      line_start = line_end < content->length() ? line_end + 1 : line_end;
    }
  }

  ec.print_to_stdout(output);
  return status;
}

} // namespace koshka::koshkit
