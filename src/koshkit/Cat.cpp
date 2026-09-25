/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements the cat utility. It streams files or standard input,
 * numbers lines, and optionally applies the shared shell syntax highlighter.
 */

#include "../CLI.hpp"
#include "../CLIColors.hpp"
#include "../Completion.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Utils.hpp"
#include "../base/Path.hpp"

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

enum class cat_number_mode : u8
{
  Unnumbered,
  Numbered,
};

enum class cat_highlight_mode : u8
{
  Plain,
  Highlighted,
};

static fn append_number_prefix(String &output, i64 line_number,
                               Allocator allocator) throws -> void
{
  let const digits = String::from(line_number, allocator);
  if (digits.count() < 6) output.append_repeated(' ', 6 - digits.count());

  output += digits.view();
  output += '\t';
}

static fn append_cat_source(String &output, StringView source,
                            i64 &line_number, bool &is_at_output_line_start,
                            EvalContext &context, cat_number_mode number_mode,
                            cat_highlight_mode highlight_mode) throws -> void
{
  let const should_number = number_mode == cat_number_mode::Numbered;
  let const should_highlight =
      highlight_mode == cat_highlight_mode::Highlighted;
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
    let const remaining = source.substring(line_start);
    let const newline_offset = remaining.find_character('\n');
    let const line_end = newline_offset.has_value()
                             ? line_start + *newline_offset + 1
                             : source.length;

    if (is_at_output_line_start) {
      append_number_prefix(output, line_number, context.scratch_allocator());
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
  let const[operands, operand_locations] = parse_util_operands(
      FLAG_LIST, args, cxt.scratch_allocator(), &arg_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  let const sources =
      source_list_from_operands(operands, cxt.scratch_allocator());

  let const should_highlight_output =
      FLAG_CAT_SYNTAX_HIGHLIGHTING.is_enabled() && colors::stdout_wants_color();
  if (!FLAG_CAT_NUMBER.is_enabled() && !should_highlight_output) {
    let reader = SourceBatchReader{ec, sources, cxt.scratch_allocator()};
    let chunks = ArrayList<SourceBatchReader::Chunk>{cxt.scratch_allocator()};
    i32 status = 0;

    loop
    {
      let const read_result = reader.read_next_ordered(chunks);
      switch (read_result) {
      case SourceBatchReader::ReadResult::Chunks: break;
      case SourceBatchReader::ReadResult::Complete: return status;
      case SourceBatchReader::ReadResult::Interrupted: return 130;
      }

      for (let const &chunk : chunks) {
        if (!chunk.content.is_empty()) ec.print_to_stdout(chunk.content);
        if (chunk.completion != source_completion_state::Complete ||
            chunk.error_number == 0)
          continue;

        os::set_last_system_error(chunk.error_number);
        report_soft_koshkit_util_error(
            ec, cxt, args[0].view(),
            String{cxt.scratch_allocator(), sources[chunk.source_index]} +
                ": " + os::last_system_error_message());
        status = 1;
      }
    }
  }

  let output = String{cxt.scratch_allocator()};
  i64 line_number = 1;
  let is_at_output_line_start = true;
  i32 status = 0;

  let source_results = ArrayList<source_read_result>{cxt.scratch_allocator()};
  source_results.reserve(sources.count());
  for (usize source_index = 0; source_index < sources.count(); source_index++)
    source_results.push({None, 0, source_completion_state::Pending});

  let reader = SourceBatchReader{ec, sources, cxt.scratch_allocator()};
  let chunks = ArrayList<SourceBatchReader::Chunk>{cxt.scratch_allocator()};
  usize next_source_index = 0;
  loop
  {
    let const read_result = reader.read_next(chunks);
    bool is_reader_complete = false;
    switch (read_result) {
    case SourceBatchReader::ReadResult::Chunks: break;
    case SourceBatchReader::ReadResult::Complete:
      is_reader_complete = true;
      break;
    case SourceBatchReader::ReadResult::Interrupted: return 130;
    }

    for (let const &chunk : chunks) {
      let &result = source_results[chunk.source_index];
      result.completion = chunk.completion;
      if (chunk.error_number != 0) {
        result.content.reset();
        result.error_number = chunk.error_number;
        continue;
      }
      if (!result.content.has_value())
        result.content = String{heap_allocator()};
      result.content->append(chunk.content);
    }

    while (next_source_index < source_results.count() &&
           source_results[next_source_index].completion ==
               source_completion_state::Complete)
    {
      let &result = source_results[next_source_index];
      let const source = sources[next_source_index];
      if (!result.content.has_value()) {
        os::set_last_system_error(result.error_number);
        report_soft_koshkit_util_error(ec, cxt, args[0].view(),
                                       String{cxt.scratch_allocator(), source} +
                                           ": " +
                                           os::last_system_error_message());
        status = 1;
      } else {
        let const should_highlight_source =
            should_highlight_output &&
            Path{source}.is_shell_source(result.content->view()) &&
            !result.content->view().find_character('\0').has_value();
        let const number_mode = FLAG_CAT_NUMBER.is_enabled()
                                    ? cat_number_mode::Numbered
                                    : cat_number_mode::Unnumbered;
        let const highlight_mode = should_highlight_source
                                       ? cat_highlight_mode::Highlighted
                                       : cat_highlight_mode::Plain;
        append_cat_source(output, result.content->view(), line_number,
                          is_at_output_line_start, cxt, number_mode,
                          highlight_mode);
        result.content.reset();
      }
      next_source_index++;
    }

    if (is_reader_complete) break;
  }

  ec.print_to_stdout(output);
  return status;
}

} /* namespace koshkit */

} /* namespace koshka */
