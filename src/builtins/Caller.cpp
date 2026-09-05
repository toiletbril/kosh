/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements and is responsible for the caller builtin. The caller
 * builtin prints the calling context of a function or a sourced file, and
 * returns zero in a call and one at the top level.
 */

#include "../Builtin.hpp"
#include "../Cli.hpp"
#include "../Eval.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[expr]");

HELP_DESCRIPTION_DECL(
    "The caller builtin prints the calling context of a function or a "
    "sourced file, and returns zero in a call and one at the top level.");

FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_BUILTIN_FLAGS(Caller);

namespace koshka {

Caller::Caller() = default;

pure fn Caller::kind() const wontthrow -> Builtin::Kind { return Kind::Caller; }

fn Caller::execute(ExecContext &ec, EvalContext &cxt) const throws -> i32
{
  let operand_locations = ArrayList<SourceLocation>{cxt.scratch_allocator()};
  let const args = PARSE_BUILTIN_ARGS_WITH_LOCATIONS(ec, operand_locations);

  if (FLAG_HELP.is_enabled()) SHOW_BUILTIN_HELP_AND_RETURN(ec);

  ASSERT(!args.is_empty());

  usize frame_index = 0;
  if (args.count() > 1) {
    let const parsed = args[1].to<i64>();
    if (parsed.is_error()) {
      throw make_error_for_arg(
          ec, 1, StringView{"'"} + args[1] + "' is not a whole number",
          "The frame index must be a whole number such as `caller 0`");
    }
    if (parsed.value() < 0) {
      throw make_error_for_arg(
          ec, 1, StringView{"'"} + args[1] + "' is a negative frame number",
          "The frame index must not be negative");
    }
    frame_index = static_cast<usize>(parsed.value());
  }

  if (frame_index >= cxt.funcname_frame_count()) return 1;

  let const line = cxt.funcname_line_at(frame_index);
  let const source = cxt.funcname_source_at(frame_index);

  let out = String{cxt.scratch_allocator()};
  out += String::from(line, cxt.scratch_allocator());
  out += ' ';
  out.append(source);
  out += '\n';
  ec.print_to_stdout(out);

  return 0;
}

} /* namespace koshka */
