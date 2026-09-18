/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements the wc utility. It streams each input, counts newlines,
 * whitespace-delimited words, and bytes, aligns columns, and computes totals.
 */

#include "../CLI.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-lwc] [file ...]");

HELP_DESCRIPTION_DECL(
    "The wc utility counts the lines, words, and bytes of each file.");

FLAG(WC_LINES, Bool, 'l', "", "Print the newline count.");
FLAG(WC_WORDS, Bool, 'w', "", "Print the word count.");
FLAG(WC_BYTES, Bool, 'c', "", "Print the byte count.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Wc);

namespace koshka {

namespace koshkit {

static fn is_blank(char c) wontthrow -> bool
{
  return c == ' ' || (c >= '\t' && c <= '\r');
}

struct wc_row
{
  StringView name;
  u64 line_count;
  u64 word_count;
  u64 byte_count;
};

struct wc_source_state
{
  u64 line_count{0};
  u64 word_count{0};
  u64 byte_count{0};
  i32 error_number{0};
  bool is_in_word{false};
};

static fn update_wc_source(wc_source_state &state, StringView content,
                           u8 scan_mode, bool should_count_bytes) wontthrow
    -> void
{
  if (should_count_bytes) state.byte_count += content.length;

  switch (scan_mode) {
  case 0: break;
  case 1: {
    let remaining = content;
    loop
    {
      let const newline = remaining.find_character('\n');
      if (!newline.has_value()) break;
      state.line_count++;
      remaining = remaining.substring(*newline + 1);
    }
    break;
  }
  case 2:
    for (usize byte_position = 0; byte_position < content.length;
         byte_position++)
    {
      let const byte = content[byte_position];
      if (is_blank(byte)) {
        state.is_in_word = false;
      } else if (!state.is_in_word) {
        state.is_in_word = true;
        state.word_count++;
      }
    }
    break;
  case 3:
    for (usize byte_position = 0; byte_position < content.length;
         byte_position++)
    {
      let const byte = content[byte_position];
      if (byte == '\n') state.line_count++;
      if (is_blank(byte)) {
        state.is_in_word = false;
      } else if (!state.is_in_word) {
        state.is_in_word = true;
        state.word_count++;
      }
    }
    break;
  }
}

static fn decimal_digit_count(u64 value) wontthrow -> usize
{
  usize digit_count = 1;

  while (value >= 10) {
    value /= 10;
    digit_count++;
  }

  return digit_count;
}

static fn append_counts(String &line, u64 lines, u64 words, u64 bytes,
                        bool should_show_lines, bool should_show_words,
                        bool should_show_bytes, StringView name,
                        usize field_width) throws -> void
{
  bool has_field = false;

  let const do_emit_field = [&line, &has_field, field_width](u64 value)
                                throws -> void {
    if (has_field) line += ' ';

    let const digits = String::from(value, line.allocator());
    if (digits.count() < field_width)
      line.append_repeated(' ', field_width - digits.count());

    line += digits.view();
    has_field = true;
  };

  if (should_show_lines) do_emit_field(lines);
  if (should_show_words) do_emit_field(words);
  if (should_show_bytes) do_emit_field(bytes);

  if (!name.is_empty()) {
    line += ' ';
    line += name;
  }

  line += '\n';
}

Wc::Wc() = default;

pure fn Wc::kind() const wontthrow -> Utility::Kind { return Kind::Wc; }

fn Wc::execute(const ExecContext &ec, EvalContext &cxt,
               const ArrayList<String> &args,
               const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let const[operands, operand_locations] = parse_util_operands(
      FLAG_LIST, args, cxt.scratch_allocator(), &arg_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  bool should_show_lines = FLAG_WC_LINES.is_enabled();
  bool should_show_words = FLAG_WC_WORDS.is_enabled();
  bool should_show_bytes = FLAG_WC_BYTES.is_enabled();
  if (!should_show_lines && !should_show_words && !should_show_bytes) {
    should_show_lines = true;
    should_show_words = true;
    should_show_bytes = true;
  }

  let const sources =
      source_list_from_operands(operands, cxt.scratch_allocator());
  let source_states = ArrayList<wc_source_state>{cxt.scratch_allocator()};
  source_states.reserve(sources.count());
  for (usize source_index = 0; source_index < sources.count(); source_index++)
    source_states.push({});

  let const scan_mode = static_cast<u8>((should_show_lines ? 1 : 0) |
                                        (should_show_words ? 2 : 0));
  let reader = SourceBatchReader{ec, sources, cxt.scratch_allocator()};
  let chunks = ArrayList<SourceBatchReader::Chunk>{cxt.scratch_allocator()};
  loop
  {
    let const read_result = reader.read_next(chunks);
    if (read_result == SourceBatchReader::ReadResult::Interrupted) return 130;
    if (read_result == SourceBatchReader::ReadResult::Complete) break;

    for (let const &chunk : chunks) {
      let &state = source_states[chunk.source_index];
      if (chunk.error_number != 0) {
        state.error_number = chunk.error_number;
        continue;
      }
      update_wc_source(state, chunk.content, scan_mode, should_show_bytes);
    }
  }

  ArrayList<wc_row> rows{cxt.scratch_allocator()};
  u64 total_lines = 0;
  u64 total_words = 0;
  u64 total_bytes = 0;
  i32 status = 0;
  for (usize source_index = 0; source_index < sources.count(); source_index++) {
    let const &state = source_states[source_index];
    if (state.error_number != 0) {
      os::set_last_system_error(state.error_number);
      report_soft_koshkit_error(
          ec, cxt,
          "wc: " + String{cxt.scratch_allocator(), sources[source_index]} +
              ": " + os::last_system_error_message());
      status = 1;
      continue;
    }

    total_lines += state.line_count;
    total_words += state.word_count;
    total_bytes += state.byte_count;

    let const name = operands.is_empty() ? StringView{} : sources[source_index];
    rows.push(
        wc_row{name, state.line_count, state.word_count, state.byte_count});
  }

  u64 max_count = 0;
  if (should_show_lines && total_lines > max_count) {
    max_count = total_lines;
  }
  if (should_show_words && total_words > max_count) {
    max_count = total_words;
  }
  if (should_show_bytes && total_bytes > max_count) {
    max_count = total_bytes;
  }

  let const field_width = decimal_digit_count(max_count);

  let output = String{cxt.scratch_allocator()};
  for (const wc_row &row : rows)
    append_counts(output, row.line_count, row.word_count, row.byte_count,
                  should_show_lines, should_show_words, should_show_bytes,
                  row.name, field_width);

  if (sources.count() > 1)
    append_counts(output, total_lines, total_words, total_bytes,
                  should_show_lines, should_show_words, should_show_bytes,
                  StringView{"total"}, field_width);

  ec.print_to_stdout(output);
  return status;
}

} /* namespace koshkit */

} /* namespace koshka */
