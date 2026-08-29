#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-iv] pattern [file ...]");

HELP_DESCRIPTION_DECL(
    "The grep utility prints the lines of each file that match a pattern.");

FLAG(GREP_IGNORE_CASE, Bool, 'i', "", "Match without regard to letter case.");
FLAG(GREP_INVERT, Bool, 'v', "", "Print the lines that do not match.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Grep);

namespace koshka {

namespace koshkit {

Grep::Grep() = default;

pure fn Grep::kind() const wontthrow -> Utility::Kind { return Kind::Grep; }

fn Grep::execute(const ExecContext &ec, EvalContext &cxt,
                 const ArrayList<String> &args,
                 const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let operand_locations = ArrayList<SourceLocation>{cxt.scratch_allocator()};
  let const operands =
      parse_util_operands(FLAG_LIST, args, &arg_locations, &operand_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  if (operands.is_empty()) return report_usage_error(ec, cxt, args[0].view());

  let const pattern = operands[0].view();
  let const should_ignore_case = FLAG_GREP_IGNORE_CASE.is_enabled();
  let const should_invert = FLAG_GREP_INVERT.is_enabled();

  os::compiled_regex compiled;
  if (os::compile_search_regex(pattern,
                               should_ignore_case
                                   ? os::case_sensitivity::Insensitive
                                   : os::case_sensitivity::Sensitive,
                               compiled) != os::regex_compile_result::Ok)
  {
    report_soft_koshkit_util_error(
        ec, cxt, operand_locations[0], args[0].view(),
        "the pattern '" + operands[0] + "' is not a valid regex");
    return 2;
  }
  defer { os::free_regex(compiled); };

  let const sources =
      source_list_from_operands(operands, cxt.scratch_allocator(), 1);

  let const should_print_names = sources.count() > 1;
  let output = String{cxt.scratch_allocator()};
  bool has_any_match = false;
  i32 status = 0;
  for (usize source_position = 0; source_position < sources.count();
       source_position++)
  {
    let const source = sources[source_position];
    let const source_location = source_position + 1 < operand_locations.count()
                                    ? operand_locations[source_position + 1]
                                    : ec.source_location();
    let const input = open_named_or_stdin(ec, source);
    if (!input.has_value()) {
      report_soft_koshkit_util_error(ec, cxt, source_location, args[0].view(),
                                     String{cxt.scratch_allocator(), source} +
                                         ": " +
                                         os::last_system_error_message());
      status = 2;
      continue;
    }
    defer
    {
      if (input->should_close) os::close_fd(input->descriptor);
    };
    let const display_name =
        source == "-" ? StringView{"(standard input)"} : source;
    let reader = utils::BufferedLineReader{input->descriptor};
    loop
    {
      let const result = reader.next();
      if (result == utils::BufferedLineReader::Result::End) break;
      if (result == utils::BufferedLineReader::Result::Error) {
        if (os::INTERRUPT_REQUESTED) return 130;
        report_soft_koshkit_util_error(ec, cxt, source_location, args[0].view(),
                                       String{cxt.scratch_allocator(), source} +
                                           ": " +
                                           os::last_system_error_message());
        status = 2;
        break;
      }
      let const line = reader.get_line();
      let const is_match = os::regex_matches_null_terminated(compiled, line);
      if (is_match == should_invert) continue;

      has_any_match = true;
      if (should_print_names) {
        output += display_name;
        output += ':';
      }
      output += line;
      output += '\n';
      if (output.count() >= 65536) {
        ec.print_to_stdout(output);
        output.clear();
      }
    }
  }

  ec.print_to_stdout(output);
  if (status == 2) return 2;

  return has_any_match ? 0 : 1;
}

} /* namespace koshkit */

} /* namespace koshka */
