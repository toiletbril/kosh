/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements and is responsible for the cut utility in koshkit.
 * The cut utility selects bytes, characters, or fields.
 */

#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Utils.hpp"
#include "TextProcessing.hpp"

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
  let const operands = parse_util_operands(FLAG_LIST, args, &arg_locations);
  defer { reset_flags(FLAG_LIST); };

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
  let const ranges = parse_text_position_ranges(list, cxt.scratch_allocator());
  if (!ranges.has_value())
    throw Error{
        "cut: invalid position list '" + String{cxt.scratch_allocator(), list}
          +
        "'"
    };

  char delimiter = '\t';
  if (FLAG_CUT_DELIMITER.is_set()) {
    if (FLAG_CUT_DELIMITER.value().length != 1)
      throw Error{"cut: the delimiter must be one byte"};
    delimiter = FLAG_CUT_DELIMITER.value()[0];
  }

  let const sources =
      source_list_from_operands(operands, cxt.scratch_allocator());
  let output = String{cxt.scratch_allocator()};
  i32 status = 0;

  for (let const source : sources) {
    let const input = open_named_or_stdin(ec, source);
    if (!input.has_value()) {
      report_soft_koshkit_error(ec, cxt,
                                "cut: cannot read '" +
                                    String{cxt.scratch_allocator(), source} +
                                    "': " + os::last_system_error_message());
      status = 1;
      continue;
    }
    defer
    {
      if (input->should_close) os::close_fd(input->descriptor);
    };

    let reader = utils::BufferedLineReader{input->descriptor};
    loop
    {
      let const result = reader.next();
      if (result == utils::BufferedLineReader::Result::End) break;
      if (result == utils::BufferedLineReader::Result::Error) {
        if (os::INTERRUPT_REQUESTED) return 130;
        report_soft_koshkit_error(ec, cxt,
                                  "cut: cannot read '" +
                                      String{cxt.scratch_allocator(), source} +
                                      "': " + os::last_system_error_message());
        status = 1;
        break;
      }

      let const line = reader.get_line();
      if (!FLAG_CUT_FIELDS.is_set()) {
        if (FLAG_CUT_BYTES.is_set()) {
          if (FLAG_CUT_NO_SPLIT.is_enabled()) {
            usize byte_position = 0;
            while (byte_position < line.length) {
              let const decoded = utils::decode_utf8(line, byte_position, 0);
              if (text_position_is_selected(byte_position + decoded.length,
                                            *ranges))
                output +=
                    line.substring_of_length(byte_position, decoded.length);
              byte_position += decoded.length;
            }
          } else {
            for (usize position = 0; position < line.length; position++)
              if (text_position_is_selected(position + 1, *ranges))
                output += line[position];
          }
        } else {
          usize byte_position = 0;
          usize character_position = 1;

          while (byte_position < line.length) {
            let const decoded = utils::decode_utf8(line, byte_position, 0);

            if (text_position_is_selected(character_position, *ranges))
              output += line.substring_of_length(byte_position, decoded.length);
            byte_position += decoded.length;
            character_position++;
          }
        }
        output += '\n';
      } else if (!line.find_character(delimiter).has_value()) {
        if (!FLAG_CUT_SUPPRESS.is_enabled()) {
          output += line;
          output += '\n';
        }
      } else {
        usize field_start = 0;
        usize field_number = 1;
        bool has_output_field = false;

        for (usize position = 0; position <= line.length; position++) {
          if (position != line.length && line[position] != delimiter) continue;
          if (text_position_is_selected(field_number, *ranges)) {
            if (has_output_field) output += delimiter;
            output +=
                line.substring_of_length(field_start, position - field_start);
            has_output_field = true;
          }
          field_start = position + 1;
          field_number++;
        }
        output += '\n';
      }

      if (output.length() >= 65536) {
        ec.print_to_stdout(output);
        output.clear();
      }
    }
  }

  ec.print_to_stdout(output);
  return status;
}

} // namespace koshka::koshkit
