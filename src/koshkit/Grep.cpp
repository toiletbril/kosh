/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements the grep utility. It compiles the requested regular
 * expression and streams matching or inverted lines with optional ASCII case
 * folding.
 */

#include "../CLI.hpp"
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

static pure fn is_literal_search_pattern(StringView pattern) wontthrow -> bool
{
  for (usize index = 0; index < pattern.length; index++) {
    switch (pattern[index]) {
      case '.':
      case '^':
      case '$':
      case '*':
      case '+':
      case '?':
      case '(':
      case ')':
      case '[':
      case ']':
      case '{':
      case '}':
      case '|':
      case '\\': return false;
      default: break;
    }
  }

  return true;
}

static pure fn contains_case_insensitive_ascii(
    StringView value, StringView folded_pattern) wontthrow
    -> bool
{
  if (folded_pattern.is_empty()) return true;
  if (folded_pattern.length > value.length) return false;

  let const last_start = value.length - folded_pattern.length;
  for (usize start = 0; start <= last_start; start++) {
    bool is_match = true;
    for (usize index = 0; index < folded_pattern.length; index++) {
      if (utils::ascii_to_lower(value[start + index]) !=
          folded_pattern[index]) {
        is_match = false;
        break;
      }
    }
    if (is_match) return true;
  }

  return false;
}

Grep::Grep() = default;

pure fn Grep::kind() const wontthrow -> Utility::Kind { return Kind::Grep; }

fn Grep::execute(const ExecContext &ec, EvalContext &cxt,
                 const ArrayList<String> &args,
                 const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let const[operands, operand_locations] = parse_util_operands(
      FLAG_LIST, args, cxt.scratch_allocator(), &arg_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  if (operands.is_empty()) return report_usage_error(ec, cxt, args[0].view());

  let const pattern = operands[0].view();
  let const should_ignore_case = FLAG_GREP_IGNORE_CASE.is_enabled();
  let const should_invert = FLAG_GREP_INVERT.is_enabled();
  let const should_use_literal_search = is_literal_search_pattern(pattern);

  let folded_pattern = String{cxt.scratch_allocator()};
  if (should_use_literal_search && should_ignore_case) {
    for (usize index = 0; index < pattern.length; index++)
      folded_pattern.push(utils::ascii_to_lower(pattern[index]));
  }

  os::compiled_regex compiled;
  if (!should_use_literal_search) {
    if (os::compile_search_regex(
            pattern,
            should_ignore_case ? os::case_sensitivity::Insensitive
                               : os::case_sensitivity::Sensitive,
            compiled) != os::regex_compile_result::Ok) {
      report_soft_koshkit_util_error(
          ec, cxt, operand_locations[0], args[0].view(),
          "the pattern '" + operands[0] + "' is not a valid regex");
      return 2;
    }
    defer { os::free_regex(compiled); };
  }

  let const sources =
      source_list_from_operands(operands, cxt.scratch_allocator(), 1);

  let const should_print_names = sources.count() > 1;
  let output = String{cxt.scratch_allocator()};
  let line = String{cxt.scratch_allocator()};
  let reader = SourceBatchReader{ec, sources, cxt.scratch_allocator()};
  let chunks = ArrayList<SourceBatchReader::Chunk>{cxt.scratch_allocator()};
  bool has_any_match = false;
  i32 status = 0;
  let const do_process_line = [&](StringView source, StringView value) throws
      -> void {
    let const is_match = should_use_literal_search
                             ? (should_ignore_case
                                    ? contains_case_insensitive_ascii(
                                          value, folded_pattern.view())
                                    : value.find_substring(pattern).has_value())
                             : os::regex_matches_null_terminated(compiled, value);
    if (is_match != should_invert) {
      has_any_match = true;
      if (should_print_names) {
        output += source == "-" ? StringView{"(standard input)"} : source;
        output += ':';
      }
      output += value;
      output += '\n';
      if (output.count() >= 65536) {
        ec.print_to_stdout(output);
        output.clear();
      }
    }
    line.clear();
  };

  loop
  {
    let const read_result = reader.read_next_ordered(chunks);
    if (read_result == SourceBatchReader::ReadResult::Complete) break;
    if (read_result == SourceBatchReader::ReadResult::Interrupted) return 130;

    for (let const &chunk : chunks) {
      let const source = sources[chunk.source_index];
      usize position = 0;
      while (position < chunk.content.length) {
        let delimiter_position = position;
        while (delimiter_position < chunk.content.length &&
               chunk.content[delimiter_position] != '\n')
        {
          delimiter_position++;
        }

        let const segment = chunk.content.substring_of_length(
            position, delimiter_position - position);
        if (delimiter_position == chunk.content.length) {
          line.append(segment);
          break;
        }

        if (should_use_literal_search && line.is_empty())
          do_process_line(source, segment);
        else {
          line.append(segment);
          do_process_line(source, line.view());
        }
        position = delimiter_position;
        position++;
      }

      if (!chunk.is_complete) continue;
      if (chunk.error_number != 0) {
        line.clear();
        os::set_last_system_error(chunk.error_number);
        let const source_location =
            chunk.source_index + 1 < operand_locations.count()
                ? operand_locations[chunk.source_index + 1]
                : ec.source_location();
        report_soft_koshkit_util_error(ec, cxt, source_location, args[0].view(),
                                       String{cxt.scratch_allocator(), source} +
                                           ": " +
                                           os::last_system_error_message());
        status = 2;
        continue;
      }

      if (!line.is_empty()) do_process_line(source, line.view());
    }
  }

  ec.print_to_stdout(output);
  if (status == 2) return 2;

  return has_any_match ? 0 : 1;
}

} /* namespace koshkit */

} /* namespace koshka */
