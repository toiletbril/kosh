/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements the seq utility. It parses signed integer bounds and
 * increments, detects unreachable or overflowing ranges, and writes each
 * generated value.
 */

#include "../CLI.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[first [increment]] last");

HELP_DESCRIPTION_DECL(
    "The seq utility prints a sequence of integers from first to last.");

FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Seq);

namespace koshka {

namespace koshkit {

static fn is_negative_number_token(StringView token) wontthrow -> bool
{
  return token.count() >= 2 && token[0] == '-' &&
         token.substring(1).is_all_decimal_digits();
}

static fn
find_leading_negative_position(const ArrayList<String> &args) wontthrow
    -> Maybe<usize>
{
  for (usize i = 1; i < args.count(); i++) {
    let const token = args[i].view();

    if (token == "--") return None;

    if (is_negative_number_token(token)) return i;

    let const is_flag_token = token.count() >= 2 && token[0] == '-';
    if (is_flag_token) continue;

    return None;
  }

  return None;
}

Seq::Seq() = default;

pure fn Seq::kind() const wontthrow -> Utility::Kind { return Kind::Seq; }

fn Seq::execute(const ExecContext &ec, EvalContext &cxt,
                const ArrayList<String> &args,
                const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  ArrayList<String> patched_args{cxt.scratch_allocator()};
  ArrayList<SourceLocation> patched_arg_locations{cxt.scratch_allocator()};
  let const negative_position = find_leading_negative_position(args);
  if (negative_position.has_value()) {
    patched_args.reserve(args.count() + 1);
    patched_arg_locations.reserve(arg_locations.count() + 1);
    for (usize i = 0; i < args.count(); i++) {
      if (i == *negative_position) {
        patched_args.push_managed(StringView{"--"});
        patched_arg_locations.push(SourceLocation{});
      }
      patched_args.push_managed(args[i].view());
      patched_arg_locations.push(arg_locations[i]);
    }
  }

  let const &effective_args =
      negative_position.has_value() ? patched_args : args;
  let const &effective_arg_locations =
      negative_position.has_value() ? patched_arg_locations : arg_locations;
  let const [operands, operand_locations] = parse_util_operands(
      FLAG_LIST, effective_args, cxt.scratch_allocator(),
      &effective_arg_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  if (operands.is_empty()) return report_usage_error(ec, cxt, args[0].view());

  i64 first = 1;
  i64 increment = 1;
  i64 last = 0;
  let const do_parse_integer = [&](usize operand_position, i64 &value)
                                   throws -> bool {
    let const parsed = operands[operand_position].view().to<i64>();
    if (parsed.is_error()) {
      KOSHKIT_REPORT_ERROR_AT(
          operand_locations[operand_position],
          "invalid integer argument '" + operands[operand_position] + "'",
          "use a decimal integer from -9223372036854775808 through "
          "9223372036854775807");
      return false;
    }

    value = parsed.value();
    return true;
  };

  if (operands.count() == 1) {
    if (!do_parse_integer(0, last)) return 1;
  } else if (operands.count() == 2) {
    if (!do_parse_integer(0, first) || !do_parse_integer(1, last)) return 1;
  } else if (operands.count() == 3) {
    if (!do_parse_integer(0, first) || !do_parse_integer(1, increment) ||
        !do_parse_integer(2, last))
    {
      return 1;
    }
  } else {
    KOSHKIT_REPORT_ERROR_AT(
        operand_locations[3], "extra operand '" + operands[3] + "'",
        "use `seq LAST`, `seq FIRST LAST`, or `seq FIRST STEP LAST`");
    return 1;
  }

  if (increment == 0) {
    KOSHKIT_REPORT_ERROR_AT(operand_locations[1],
                            "the increment must not be zero",
                            "use a nonzero step, such as `seq 1 2 10`");
    return 1;
  }

  let output = String{cxt.scratch_allocator()};
  static constexpr usize OUTPUT_BUFFER_LENGTH = 64 * 1024;
  output.reserve(OUTPUT_BUFFER_LENGTH);
  char value_text[21];
  /* The step is guarded against signed overflow before it is taken, so a range
     reaching the integer bounds ends rather than wrapping. */
  if (increment > 0)
    for (i64 value = first; value <= last; value += increment) {
      output += utils::int_to_text_into(value, value_text, sizeof(value_text));
      output += '\n';
      if (output.count() >= OUTPUT_BUFFER_LENGTH) {
        ec.print_to_stdout(output);
        output.clear();
      }
      if (value > INT64_MAX - increment) break;
    }
  else
    for (i64 value = first; value >= last; value += increment) {
      output += utils::int_to_text_into(value, value_text, sizeof(value_text));
      output += '\n';
      if (output.count() >= OUTPUT_BUFFER_LENGTH) {
        ec.print_to_stdout(output);
        output.clear();
      }
      if (value < INT64_MIN - increment) break;
    }

  ec.print_to_stdout(output);
  return 0;
}

} /* namespace koshkit */

} /* namespace koshka */
