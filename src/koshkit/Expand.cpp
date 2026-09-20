/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements the expand utility. It parses explicit tab stops and
 * replaces input tabs according to the current display column.
 */

#include "../CLI.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../TextProcessing.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-t tablist] [file ...]");

HELP_DESCRIPTION_DECL("The expand utility converts tabs to spaces.");

FLAG(EXPAND_TABS, String, 't', "tabs", "Use these tab stops.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Expand);

namespace koshka::koshkit {

static pure fn is_expand_control(char byte) wontthrow -> bool
{
  return byte == '\t' || byte == '\n' || byte == '\r' || byte == '\b';
}

Expand::Expand() = default;

pure fn Expand::kind() const wontthrow -> Utility::Kind { return Kind::Expand; }

fn Expand::execute(const ExecContext &ec, EvalContext &cxt,
                   const ArrayList<String> &args,
                   const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let const operands = PARSE_KOSHKIT_ARGS(args, arg_locations);

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  let tab_stops = ArrayList<usize>{cxt.scratch_allocator()};
  if (FLAG_EXPAND_TABS.is_set()) {
    let const parsed =
        parse_tab_stop_list(FLAG_EXPAND_TABS.value(), cxt.scratch_allocator());
    if (!parsed.has_value()) {
      KOSHKIT_REPORT_ERROR_AT(
          FLAG_EXPAND_TABS.value_location(), "invalid tab list",
          "use increasing positive columns separated by commas or blanks");
      return 1;
    }
    tab_stops = steal(*parsed);
  }

  let const sources =
      source_list_from_operands(operands, cxt.scratch_allocator());
  let output = String{cxt.scratch_allocator()};
  i32 status = 0;

  let const do_append_source = [&](StringView text) throws -> void {
    usize column = 0;
    usize position = 0;

    while (position < text.length) {
      let const byte = text[position];
      switch (byte) {
      case '\t': {
        let const target = next_tab_column(column, tab_stops);
        if (target == column) {
          output += '\t';
        } else {
          output.append_repeated(' ', target - column);
          column = target;
        }

        position++;
      }
        continue;

      case '\n':
      case '\r':
        output += byte;
        column = 0;
        position++;
        continue;

      case '\b':
        output += byte;
        if (column > 0) column--;

        position++;
        continue;

      default: break;
      }

      usize run_end = position;
      while (run_end < text.length && !is_expand_control(text[run_end]))
        run_end++;

      output.append(text.substring_of_length(position, run_end - position));
      column += run_end - position;
      position = run_end;
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
