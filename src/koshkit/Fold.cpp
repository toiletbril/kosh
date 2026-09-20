/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements the fold utility. It wraps lines by byte or
 * display-column width and can choose blank boundaries for each break.
 */

#include "../CLI.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-bs] [-w width] [file ...]");

HELP_DESCRIPTION_DECL("The fold utility wraps input lines.");

FLAG(FOLD_BYTES, Bool, 'b', "bytes", "Count bytes instead of columns.");
FLAG(FOLD_SPACES, Bool, 's', "spaces", "Break at blanks when possible.");
FLAG(FOLD_WIDTH, String, 'w', "width", "Use this maximum width.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Fold);

namespace koshka::koshkit {

static fn append_folded_line(String &output, StringView line, usize width,
                             bool should_break_at_blanks) throws -> void
{
  usize start = 0;

  while (line.length - start > width) {
    usize break_length = width;
    if (should_break_at_blanks) {
      for (usize offset = width; offset > 0; offset--)
        if (std::isspace(static_cast<u8>(line[start + offset - 1])) != 0) {
          break_length = offset;
          break;
        }
    }

    output += line.substring_of_length(start, break_length);
    output += '\n';
    start += break_length;
  }

  output += line.substring(start);
  output += '\n';
}

Fold::Fold() = default;

pure fn Fold::kind() const wontthrow -> Utility::Kind { return Kind::Fold; }

fn Fold::execute(const ExecContext &ec, EvalContext &cxt,
                 const ArrayList<String> &args,
                 const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let const operands = PARSE_KOSHKIT_ARGS(args, arg_locations);

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  u64 width_value = 80;
  if (FLAG_FOLD_WIDTH.is_set()) {
    let const parsed = utils::parse_decimal_u64(FLAG_FOLD_WIDTH.value());
    if (parsed.is_error() || parsed.value() == 0 || parsed.value() > SIZE_MAX) {
      KOSHKIT_REPORT_ERROR_AT(
          FLAG_FOLD_WIDTH.value_location(),
          "invalid width '" +
              String{cxt.scratch_allocator(), FLAG_FOLD_WIDTH.value()} + "'",
          "use a positive decimal width");
      return 1;
    }
    width_value = parsed.value();
  }
  let const width = static_cast<usize>(width_value);
  let const sources =
      source_list_from_operands(operands, cxt.scratch_allocator());
  let output = String{cxt.scratch_allocator()};
  i32 status = 0;

  let const do_append_source = [&](StringView content) throws -> void {
    usize line_start = 0;
    while (line_start < content.length) {
      usize line_end = line_start;
      while (line_end < content.length && content[line_end] != '\n')
        line_end++;
      append_folded_line(
          output,
          content.substring_of_length(line_start, line_end - line_start), width,
          FLAG_FOLD_SPACES.is_enabled());
      line_start = line_end < content.length ? line_end + 1 : line_end;
    }
  };

  let source_results = ArrayList<source_read_result>{cxt.scratch_allocator()};
  source_results.reserve(sources.count());
  for (usize source_index = 0; source_index < sources.count(); source_index++)
    source_results.push({None, 0, false});

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
      result.is_complete = chunk.is_complete;
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
           source_results[next_source_index].is_complete)
    {
      let &result = source_results[next_source_index];
      if (!result.content.has_value()) {
        os::set_last_system_error(result.error_number);
        report_soft_koshkit_util_error(
            ec, cxt, args[0].view(),
            "cannot read '" +
                String{cxt.scratch_allocator(), sources[next_source_index]} +
                "': " + os::last_system_error_message());
        status = 1;
      } else {
        do_append_source(result.content->view());
        result.content.reset();
      }
      next_source_index++;
    }

    if (is_reader_complete) break;
  }

  ec.print_to_stdout(output);
  return status;
}

} // namespace koshka::koshkit
