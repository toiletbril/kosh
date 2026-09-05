/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements the cat utility. It streams files or standard input,
 * numbers lines, and optionally applies the shared shell syntax highlighter.
 */

#include "../Cli.hpp"
#include "../CliColors.hpp"
#include "../Completion.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Path.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-n] [--syntax-highlighting] [file ...]");

HELP_DESCRIPTION_DECL("The cat utility writes each file to standard output.");

FLAG(CAT_NUMBER, Bool, 'n', "", "Number every output line, starting at one.");
FLAG(CAT_SYNTAX_HIGHLIGHTING, Bool, '\0', "syntax-highlighting",
     "Highlight detected shell source on a terminal.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Cat);

namespace koshka {

namespace koshkit {

static fn number_prefix(i64 line_number, Allocator allocator) throws -> String
{
  let const digits = String::from(line_number, allocator);
  String prefix{allocator};
  for (usize i = digits.count(); i < 6; i++)
    prefix += ' ';
  prefix += digits.view();
  prefix += '\t';
  return prefix;
}

static fn append_cat_source(String &output, StringView source,
                            bool should_number, bool should_highlight,
                            i64 &line_number, bool &is_at_output_line_start,
                            EvalContext &context) throws -> void
{
  if (!should_number && !should_highlight) {
    output += source;
    return;
  }
  if (!should_number) {
    completion::append_highlighted_source(
        output, source, context, colors::PRINTED_SOURCE_HIGHLIGHT_THEME);
    is_at_output_line_start =
        !source.is_empty() && source[source.length - 1] == '\n';
    return;
  }

  let highlight_cache = completion::shell_highlight_cache{};
  usize line_start = 0;
  while (line_start < source.length) {
    let line_end = line_start;
    while (line_end < source.length && source[line_end] != '\n')
      line_end++;
    if (line_end < source.length) line_end++;

    if (should_number && is_at_output_line_start) {
      output += number_prefix(line_number, context.scratch_allocator());
      line_number++;
    }

    let const line =
        source.substring_of_length(line_start, line_end - line_start);
    if (should_highlight) {
      let const *spans =
          highlight_cache.spans_for(source, line_start, line_end, context);
      completion::append_highlighted_range(
          output, line, *spans, 0, line.length,
          colors::PRINTED_SOURCE_HIGHLIGHT_THEME);
    } else {
      output += line;
    }
    is_at_output_line_start = !line.is_empty() && line[line.length - 1] == '\n';
    line_start = line_end;
  }
}

Cat::Cat() = default;

pure fn Cat::kind() const wontthrow -> Utility::Kind { return Kind::Cat; }

fn Cat::execute(const ExecContext &ec, EvalContext &cxt,
                const ArrayList<String> &args,
                const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let const operands = parse_util_operands(FLAG_LIST, args, &arg_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  let const sources =
      source_list_from_operands(operands, cxt.scratch_allocator());

  let output = String{cxt.scratch_allocator()};
  let const should_highlight_output =
      FLAG_CAT_SYNTAX_HIGHLIGHTING.is_enabled() && colors::stdout_wants_color();
  i64 line_number = 1;
  let is_at_output_line_start = true;
  i32 status = 0;
  for (let const &source : sources) {
    if (!FLAG_CAT_NUMBER.is_enabled() && !should_highlight_output) {
      let const input = open_named_or_stdin(ec, source);
      if (!input.has_value()) {
        report_soft_koshkit_error(
            ec, cxt,
            "cat: " + String{cxt.scratch_allocator(), source} + ": " +
                os::last_system_error_message());
        status = 1;
        continue;
      }
      defer
      {
        if (input->should_close) os::close_fd(input->descriptor);
      };
      char buffer[65536];

      loop
      {
        let const read_size =
            os::read_fd(input->descriptor, buffer, sizeof(buffer));
        if (!read_size.has_value()) {
          if (os::INTERRUPT_REQUESTED) return 130;
          report_soft_koshkit_error(
              ec, cxt,
              "cat: " + String{cxt.scratch_allocator(), source} + ": " +
                  os::last_system_error_message());
          status = 1;
          break;
        }
        if (*read_size == 0) break;
        ec.print_to_stdout(StringView{buffer, *read_size});
      }

      continue;
    }

    let const content = read_named_or_stdin(ec, source);
    if (os::INTERRUPT_REQUESTED) return 130;
    if (!content.has_value()) {
      report_soft_koshkit_error(
          ec, cxt,
          "cat: " + String{cxt.scratch_allocator(), source} + ": " +
              os::last_system_error_message());
      status = 1;
      continue;
    }
    let const should_highlight_source =
        should_highlight_output &&
        !content->view().find_character('\0').has_value() &&
        Path{source}.is_shell_source(content->view());
    append_cat_source(output, content->view(), FLAG_CAT_NUMBER.is_enabled(),
                      should_highlight_source, line_number,
                      is_at_output_line_start, cxt);
  }

  ec.print_to_stdout(output);
  return status;
}

} /* namespace koshkit */

} /* namespace koshka */
