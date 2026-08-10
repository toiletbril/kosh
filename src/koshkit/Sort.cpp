#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"

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
  let const operands = parse_util_operands(FLAG_LIST, args, &arg_locations);
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
  for (const StringView &source : sources) {
    let content = read_named_or_stdin(ec, source);
    if (os::INTERRUPT_REQUESTED) return 130;
    if (!content.has_value()) {
      report_soft_koshkit_error(ec, cxt,
                                "sort: cannot read '" +
                                    String{cxt.scratch_allocator(), source} +
                                    "': " + os::last_system_error_message());
      status = 2;
      continue;
    }

    contents.push(steal(*content));
    for (const StringView &line : split_keep_newlines(contents.back().view())) {
      lines.push(line.without_trailing_newline());
    }
  }

  sort_stringview_list(lines);

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

} // namespace koshkit

} // namespace koshka
