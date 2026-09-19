/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements the cut utility. It parses position ranges and selects
 * bytes, decoded characters, or delimiter-separated fields from each input
 * line.
 */

#include "../CLI.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../TextProcessing.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL(
    "-b list [-n] | -c list | -f list [-d delim] [-s] [file ...]");

HELP_DESCRIPTION_DECL("The cut utility selects bytes, characters, or fields.");

FLAG(CUT_BYTES, String, 'b', "bytes", "Select byte positions.");
FLAG(CUT_CHARACTERS, String, 'c', "characters", "Select character positions.");
FLAG(CUT_FIELDS, String, 'f', "fields", "Select delimiter separated fields.");
FLAG(CUT_DELIMITER, String, 'd', "delimiter", "Use this field delimiter.");
FLAG(CUT_NO_SPLIT, Bool, 'n', "no-split", "Do not split multibyte characters.");
FLAG(CUT_SUPPRESS, Bool, 's', "only-delimited",
     "Suppress lines without a delimiter.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Cut);

namespace koshka::koshkit {

Cut::Cut() = default;

pure fn Cut::kind() const wontthrow -> Utility::Kind { return Kind::Cut; }

fn Cut::execute(const ExecContext &ec, EvalContext &cxt,
                const ArrayList<String> &args,
                const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let const operands = PARSE_KOSHKIT_ARGS(args, arg_locations);

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  let const selection_count = static_cast<usize>(FLAG_CUT_BYTES.is_set()) +
                              static_cast<usize>(FLAG_CUT_CHARACTERS.is_set()) +
                              static_cast<usize>(FLAG_CUT_FIELDS.is_set());
  if (selection_count != 1 ||
      (FLAG_CUT_DELIMITER.is_set() && !FLAG_CUT_FIELDS.is_set()) ||
      (FLAG_CUT_NO_SPLIT.is_enabled() && !FLAG_CUT_BYTES.is_set()))
    return report_usage_error(ec, cxt, args[0].view());

  let const list = FLAG_CUT_BYTES.is_set()        ? FLAG_CUT_BYTES.value()
                   : FLAG_CUT_CHARACTERS.is_set() ? FLAG_CUT_CHARACTERS.value()
                                                  : FLAG_CUT_FIELDS.value();
  let const list_location =
      FLAG_CUT_BYTES.is_set()        ? FLAG_CUT_BYTES.value_location()
      : FLAG_CUT_CHARACTERS.is_set() ? FLAG_CUT_CHARACTERS.value_location()
                                     : FLAG_CUT_FIELDS.value_location();
  let const ranges = parse_text_position_ranges(list, cxt.scratch_allocator());
  if (!ranges.has_value()) {
    KOSHKIT_REPORT_ERROR_AT(
        list_location,
        "invalid position list '" + String{cxt.scratch_allocator(), list} + "'",
        "positions start at 1 and use N, N-M, -M, or N- separated by commas "
        "or blanks");
    return 1;
  }

  char delimiter = '\t';
  if (FLAG_CUT_DELIMITER.is_set()) {
    if (FLAG_CUT_DELIMITER.value().length != 1) {
      KOSHKIT_REPORT_ERROR_AT(FLAG_CUT_DELIMITER.value_location(),
                              "the delimiter must be one byte");
      return 1;
    }
    delimiter = FLAG_CUT_DELIMITER.value()[0];
  }

  let const sources =
      source_list_from_operands(operands, cxt.scratch_allocator());
  let output = String{cxt.scratch_allocator()};
  let const is_field_mode = FLAG_CUT_FIELDS.is_set();
  let const is_byte_mode = FLAG_CUT_BYTES.is_set();
  let const should_keep_characters_whole = FLAG_CUT_NO_SPLIT.is_enabled();
  let const should_suppress_undelimited = FLAG_CUT_SUPPRESS.is_enabled();
  let line = String{cxt.scratch_allocator()};
  let reader = SourceBatchReader{ec, sources, cxt.scratch_allocator()};
  let chunks = ArrayList<SourceBatchReader::Chunk>{cxt.scratch_allocator()};
  i32 status = 0;
  let const do_process_line = [&](StringView line_view) throws -> void {
    if (!is_field_mode) {
      if (is_byte_mode) {
        if (should_keep_characters_whole) {
          usize byte_position = 0;
          while (byte_position < line_view.length) {
            let const decoded = utils::decode_utf8(line_view, byte_position, 0);
            if (text_position_is_selected(byte_position + decoded.length,
                                          *ranges))
            {
              output +=
                  line_view.substring_of_length(byte_position, decoded.length);
            }
            byte_position += decoded.length;
          }
        } else {
          for (usize position = 0; position < line_view.length; position++)
            if (text_position_is_selected(position + 1, *ranges))
              output += line_view[position];
        }
      } else {
        usize byte_position = 0;
        usize character_position = 1;

        while (byte_position < line_view.length) {
          let const decoded = utils::decode_utf8(line_view, byte_position, 0);

          if (text_position_is_selected(character_position, *ranges))
            output +=
                line_view.substring_of_length(byte_position, decoded.length);
          byte_position += decoded.length;
          character_position++;
        }
      }
      output += '\n';
    } else {
      let const first_delimiter = line_view.find_character(delimiter);
      if (!first_delimiter.has_value()) {
        if (!should_suppress_undelimited) {
          output += line_view;
          output += '\n';
        }
      } else {
        usize field_start = 0;
        usize field_number = 1;
        bool has_output_field = false;

        for (usize position = *first_delimiter; position <= line_view.length;
             position++)
        {
          if (position != line_view.length && line_view[position] != delimiter)
          {
            continue;
          }

          if (text_position_is_selected(field_number, *ranges)) {
            if (has_output_field) output += delimiter;

            output += line_view.substring_of_length(field_start,
                                                    position - field_start);
            has_output_field = true;
          }

          field_start = position + 1;
          field_number++;
        }

        output += '\n';
      }
    }

    if (output.length() >= 65536) {
      ec.print_to_stdout(output);
      output.clear();
    }
  };

  loop
  {
    let const read_result = reader.read_next_ordered(chunks);
    if (read_result == SourceBatchReader::ReadResult::Complete) break;
    if (read_result == SourceBatchReader::ReadResult::Interrupted) return 130;

    for (let const &chunk : chunks) {
      usize position = 0;
      while (position < chunk.content.length) {
        usize delimiter_position = position;
        while (delimiter_position < chunk.content.length &&
               chunk.content[delimiter_position] != '\n')
        {
          delimiter_position++;
        }

        line.append(chunk.content.substring_of_length(
            position, delimiter_position - position));
        position = delimiter_position;
        if (position == chunk.content.length) break;

        position++;
        do_process_line(line.view());
        line.clear();
      }

      if (!chunk.is_complete) continue;

      let const source = sources[chunk.source_index];
      if (chunk.error_number != 0) {
        line.clear();
        os::set_last_system_error(chunk.error_number);
        report_soft_koshkit_util_error(
            ec, cxt, args[0].view(),
            "cannot read '" + String{cxt.scratch_allocator(), source} +
                "': " + os::last_system_error_message());
        status = 1;
      } else if (!line.is_empty()) {
        do_process_line(line.view());
        line.clear();
      }
    }
  }

  ec.print_to_stdout(output);
  return status;
}

} // namespace koshka::koshkit
