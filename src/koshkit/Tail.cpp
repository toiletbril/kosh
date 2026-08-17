#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-n count] [-c count] [file ...]");

HELP_DESCRIPTION_DECL("The tail utility writes the last lines of each file.");

FLAG(TAIL_LINES, String, 'n', "", "Write the last count lines.");
FLAG(TAIL_BYTES, String, 'c', "", "Write the last count bytes.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Tail);

namespace koshka {

namespace koshkit {

enum class count_origin : u8
{
  FromEnd,
  FromStart
};

static fn parse_tail_count(StringView spec, count_origin &origin_out,
                           i64 &count_out) throws -> bool
{
  origin_out = count_origin::FromEnd;
  let digits = spec;
  if (digits.length > 0 && digits[0] == '+') {
    origin_out = count_origin::FromStart;
    digits = digits.substring(1);
  } else if (digits.length > 0 && digits[0] == '-') {
    digits = digits.substring(1);
  }

  let const parsed = digits.to<i64>();
  if (parsed.is_error() || parsed.value() < 0) {
    return false;
  }

  count_out = parsed.value();
  return true;
}

Tail::Tail() = default;

pure fn Tail::kind() const wontthrow -> Utility::Kind { return Kind::Tail; }

fn Tail::execute(const ExecContext &ec, EvalContext &cxt,
                 const ArrayList<String> &args,
                 const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let const operands = parse_util_operands(FLAG_LIST, args, &arg_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  /* -c takes precedence over -n when both are given, matching GNU tail. */
  let const is_byte_mode = FLAG_TAIL_BYTES.is_set();
  let origin = count_origin::FromEnd;
  i64 count = 10;
  if (is_byte_mode) {
    if (!parse_tail_count(FLAG_TAIL_BYTES.value(), origin, count)) {
      throw ErrorWithDetails{
          "tail: invalid byte count '" +
              String{cxt.scratch_allocator(), FLAG_TAIL_BYTES.value()}
              + "'",
          "The count must be a non-negative integer"
      };
    }
  } else if (FLAG_TAIL_LINES.is_set()) {
    if (!parse_tail_count(FLAG_TAIL_LINES.value(), origin, count)) {
      throw ErrorWithDetails{
          "tail: invalid line count '" +
              String{cxt.scratch_allocator(), FLAG_TAIL_LINES.value()}
              + "'",
          "The count must be a non-negative integer"
      };
    }
  }

  let const sources =
      source_list_from_operands(operands, cxt.scratch_allocator());

  let const should_print_headers = sources.count() > 1;
  let output = String{cxt.scratch_allocator()};
  i32 status = 0;
  for (usize source_index = 0; source_index < sources.count(); source_index++) {
    let const content = read_named_or_stdin(ec, sources[source_index]);
    if (os::INTERRUPT_REQUESTED) return 130;
    if (!content.has_value()) {
      report_soft_koshkit_error(
          ec, cxt,
          "tail: cannot open '" +
              String{cxt.scratch_allocator(), sources[source_index]} +
              "': " + os::last_system_error_message());
      status = 1;
      continue;
    }

    if (should_print_headers) {
      if (source_index > 0) output += '\n';
      output += "==> ";
      output += sources[source_index] == "-" ? StringView{"standard input"}
                                             : sources[source_index];
      output += " <==\n";
    }

    if (is_byte_mode) {
      let const wanted_count = static_cast<usize>(count);
      let const text = content->view();
      let start = origin == count_origin::FromStart
                      ? (count > 0 ? static_cast<usize>(count - 1) : 0)
                      : sub_sat(text.length, wanted_count);
      if (start > text.length) start = text.length;

      output += text.substring(start);
      continue;
    }

    let const text = content->view();
    let const wanted_count = static_cast<usize>(count);
    usize start = 0;
    if (origin == count_origin::FromStart) {
      usize remaining_newline_count = count > 0 ? wanted_count - 1 : 0;
      while (start < text.length && remaining_newline_count > 0) {
        if (text[start] == '\n') remaining_newline_count--;
        start++;
      }
      if (remaining_newline_count > 0) start = text.length;
    } else if (wanted_count == 0) {
      start = text.length;
    } else {
      start = text.length;
      usize remaining_newline_count = wanted_count;
      if (start > 0 && text[start - 1] == '\n') start--;
      while (start > 0) {
        if (text[start - 1] == '\n' && --remaining_newline_count == 0) break;
        start--;
      }
    }
    output += text.substring(start);
  }

  ec.print_to_stdout(output);
  return status;
}

} /* namespace koshkit */

} /* namespace koshka */
