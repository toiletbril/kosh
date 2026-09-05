/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements the csplit utility. It splits input at numeric or
 * regular-expression boundaries, expands repeated patterns, and manages
 * numbered output files.
 */

#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-ks] [-f prefix] [-n digits] file pattern ...");

HELP_DESCRIPTION_DECL("The csplit utility divides a file at selected lines.");

FLAG(CSPLIT_KEEP, Bool, 'k', "keep-files", "Keep output files after an error.");
FLAG(CSPLIT_PREFIX, String, 'f', "prefix", "Use this output prefix.");
FLAG(CSPLIT_DIGITS, String, 'n', "digits", "Use this many suffix digits.");
FLAG(CSPLIT_SILENT, Bool, 's', "silent", "Suppress byte counts.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Csplit);

namespace koshka::koshkit {

static fn csplit_output_name(StringView prefix, usize width, u64 index,
                             Allocator allocator) throws -> String
{
  let const digits = String::from(index, allocator);
  if (digits.length() > width)
    throw Error{"csplit: output file suffixes are exhausted"};

  String name{allocator, prefix};
  for (usize position = digits.length(); position < width; position++)
    name += '0';
  name += digits.view();
  return name;
}

static fn write_csplit_part(const ExecContext &ec, EvalContext &cxt,
                            StringView prefix, usize digit_count,
                            u64 output_index,
                            const ArrayList<StringView> &lines, usize first,
                            usize last, bool should_be_silent,
                            ArrayList<String> &output_paths) throws -> bool
{
  let const name = csplit_output_name(prefix, digit_count, output_index,
                                      cxt.scratch_allocator());
  let const descriptor =
      os::open_file_descriptor(name.view(), os::file_open_mode::Truncate);
  if (!descriptor.has_value()) {
    report_soft_koshkit_error(ec, cxt,
                              "csplit: cannot create '" + name +
                                  "': " + os::last_system_error_message());
    return false;
  }
  defer { os::close_fd(*descriptor); };
  output_paths.push(name.clone());

  u64 byte_count = 0;
  for (usize line_index = first; line_index < last; line_index++) {
    if (!os::write_all(*descriptor, lines[line_index].data,
                       lines[line_index].length))
    {
      report_soft_koshkit_error(ec, cxt,
                                "csplit: cannot write '" + name +
                                    "': " + os::last_system_error_message());
      return false;
    }
    byte_count += lines[line_index].length;
  }

  if (!should_be_silent)
    ec.print_to_stdout(String::from(byte_count, cxt.scratch_allocator()) +
                       "\n");
  return true;
}

Csplit::Csplit() = default;

pure fn Csplit::kind() const wontthrow -> Utility::Kind { return Kind::Csplit; }

fn Csplit::execute(const ExecContext &ec, EvalContext &cxt,
                   const ArrayList<String> &args,
                   const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let const operands = parse_util_operands(FLAG_LIST, args, &arg_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  if (operands.count() < 2) return report_usage_error(ec, cxt, args[0].view());

  u64 digit_count_value = 2;
  if (FLAG_CSPLIT_DIGITS.is_set()) {
    let const parsed = utils::parse_decimal_u64(FLAG_CSPLIT_DIGITS.value());
    if (parsed.is_error() || parsed.value() == 0 || parsed.value() > SIZE_MAX)
      throw Error{"csplit: invalid suffix width"};
    digit_count_value = parsed.value();
  }

  let const content = read_named_or_stdin(ec, operands[0].view());
  if (!content.has_value()) {
    report_soft_koshkit_error(ec, cxt,
                              "csplit: cannot read '" + operands[0] +
                                  "': " + os::last_system_error_message());
    return 1;
  }
  let lines =
      utils::split_lines(content->view(), cxt.scratch_allocator(), true);
  let const prefix = FLAG_CSPLIT_PREFIX.is_set() ? FLAG_CSPLIT_PREFIX.value()
                                                 : StringView{"xx"};
  let const digit_count = static_cast<usize>(digit_count_value);
  usize current_line = 0;
  u64 output_index = 0;
  let output_paths = ArrayList<String>{cxt.scratch_allocator()};
  defer
  {
    if (!FLAG_CSPLIT_KEEP.is_enabled())
      for (let const &path : output_paths)
        unused(os::remove_file(path.view()));
  };
  let previous_pattern = String{cxt.scratch_allocator()};

  let const do_apply_pattern =
      [&](StringView pattern, bool is_repeat, bool should_allow_exhaustion)
          throws -> bool {
    usize target_line = SIZE_MAX;
    bool should_write_part = true;
    let const numeric = utils::parse_decimal_u64(pattern);
    if (!numeric.is_error()) {
      if (numeric.value() == 0)
        throw Error{"csplit: line number is outside the input"};
      let const numeric_target =
          is_repeat ? static_cast<u64>(current_line) + numeric.value()
                    : numeric.value() - 1;
      if (numeric_target > lines.count()) {
        if (should_allow_exhaustion) return false;
        throw Error{"csplit: line number is outside the input"};
      }
      target_line = static_cast<usize>(numeric_target);
    } else if (pattern.length >= 2 && (pattern[0] == '/' || pattern[0] == '%'))
    {
      let const delimiter = pattern[0];
      usize delimiter_position = 1;

      while (delimiter_position < pattern.length) {
        if (pattern[delimiter_position] == '\\') {
          delimiter_position += 2;
          continue;
        }
        if (pattern[delimiter_position] == delimiter) break;
        delimiter_position++;
      }
      if (delimiter_position >= pattern.length)
        throw Error{"csplit: unterminated regular expression"};

      should_write_part = pattern[0] == '/';
      let const expression =
          pattern.substring_of_length(1, delimiter_position - 1);
      i64 offset = 0;
      if (delimiter_position + 1 < pattern.length) {
        let const parsed_offset =
            utils::parse_decimal_i64(pattern.substring(delimiter_position + 1));
        if (parsed_offset.is_error())
          throw Error{"csplit: invalid regular expression offset"};
        offset = parsed_offset.value();
      }
      os::compiled_regex compiled;
      if (os::compile_basic_regex(expression, os::case_sensitivity::Sensitive,
                                  compiled) != os::regex_compile_result::Ok)
        throw Error{"csplit: invalid regular expression"};
      defer { os::free_regex(compiled); };

      let const search_start =
          current_line +
          static_cast<usize>(is_repeat && current_line < lines.count());
      usize matched_line = SIZE_MAX;

      for (usize line_index = search_start; line_index < lines.count();
           line_index++)
      {
        if (os::regex_matches(compiled,
                              lines[line_index].without_trailing_newline()))
        {
          matched_line = line_index;
          break;
        }
      }
      if (matched_line == SIZE_MAX) {
        if (should_allow_exhaustion) return false;
        throw Error{"csplit: regular expression did not match"};
      }
      let const adjusted_line = static_cast<i128>(matched_line) + offset;
      if (adjusted_line < 0 || adjusted_line > lines.count()) {
        if (should_allow_exhaustion) return false;
        throw Error{"csplit: regular expression offset is outside the input"};
      }
      target_line = static_cast<usize>(adjusted_line);
    } else {
      throw Error{"csplit: invalid pattern '" + String{pattern} + "'"};
    }

    if (target_line < current_line) {
      if (should_allow_exhaustion) return false;
      throw Error{"csplit: split point precedes the current segment"};
    }
    if (should_write_part) {
      if (!write_csplit_part(ec, cxt, prefix, digit_count, output_index++,
                             lines, current_line, target_line,
                             FLAG_CSPLIT_SILENT.is_enabled(), output_paths))
        throw Error{"csplit: cannot write output"};
    }
    current_line = target_line;
    return true;
  };

  for (usize pattern_index = 1; pattern_index < operands.count();
       pattern_index++)
  {
    let const pattern = operands[pattern_index].view();
    if (pattern.length >= 2 && pattern[0] == '{' &&
        pattern[pattern.length - 1] == '}')
    {
      if (previous_pattern.is_empty())
        throw Error{"csplit: repeat has no preceding pattern"};
      let const repeat = pattern.substring_of_length(1, pattern.length - 2);
      if (repeat == "*") {
        while (do_apply_pattern(previous_pattern.view(), true, true)) {}
      } else {
        let const repeat_count = utils::parse_decimal_u64(repeat);
        if (repeat_count.is_error())
          throw Error{"csplit: invalid repeat count"};

        for (u64 repetition = 0; repetition < repeat_count.value();
             repetition++)
          unused(do_apply_pattern(previous_pattern.view(), true, false));
      }
      continue;
    }
    previous_pattern = operands[pattern_index].clone();
    unused(do_apply_pattern(pattern, false, false));
  }

  if (!write_csplit_part(ec, cxt, prefix, digit_count, output_index, lines,
                         current_line, lines.count(),
                         FLAG_CSPLIT_SILENT.is_enabled(), output_paths))
    return 1;
  output_paths.clear();
  return 0;
}

} // namespace koshka::koshkit
