/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements the sort utility. It collects lines from files or
 * standard input, orders them by raw byte value, and optionally reverses the
 * result.
 */

#include "../CLI.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-r] [file ...]");

HELP_DESCRIPTION_DECL(
    "The sort utility writes the lines of its input in byte order.");

FLAG(SORT_REVERSE, Bool, 'r', "", "Reverse the order of the output.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Sort);

namespace koshka {

namespace koshkit {

Sort::Sort() = default;

pure fn Sort::kind() const wontthrow -> Utility::Kind { return Kind::Sort; }

fn Sort::execute(const ExecContext &ec, EvalContext &cxt,
                 const ArrayList<String> &args,
                 const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let const[operands, operand_locations] = parse_util_operands(
      FLAG_LIST, args, cxt.scratch_allocator(), &arg_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  let const sources =
      source_list_from_operands(operands, cxt.scratch_allocator());

  /* contents keeps each file's bytes alive for the line views, and the reserve
     stops a grow from dangling a view into a String's inline buffer. */
  ArrayList<String> contents{cxt.scratch_allocator()};
  contents.reserve(sources.count());
  ArrayList<StringView> lines{cxt.scratch_allocator()};
  i32 status = 0;
  let source_results =
      read_named_or_stdin_batch(ec, sources, cxt.scratch_allocator());
  if (os::INTERRUPT_REQUESTED) return 130;

  for (usize source_index = 0; source_index < sources.count(); source_index++) {
    let &source_result = source_results[source_index];
    if (!source_result.content.has_value()) {
      os::set_last_system_error(source_result.error_number);
      report_soft_koshkit_util_error(
          ec, cxt, args[0].view(),
          "cannot read '" +
              String{cxt.scratch_allocator(), sources[source_index]} +
              "': " + os::last_system_error_message());
      status = 2;
      continue;
    }

    contents.push(source_result.content.take());
    for (const StringView &line : utils::split_lines(
             contents.back().view(), cxt.scratch_allocator(), true))
    {
      lines.push(line.without_trailing_newline());
    }
  }

  lines.sort();

  let output = String{cxt.scratch_allocator()};
  if (FLAG_SORT_REVERSE.is_enabled())
    for (usize i = lines.count(); i > 0; i--) {
      output += lines[i - 1];
      output += '\n';
    }
  else
    for (const StringView &line : lines) {
      output += line;
      output += '\n';
    }

  ec.print_to_stdout(output);

  return status;
}

} /* namespace koshkit */

} /* namespace koshka */
