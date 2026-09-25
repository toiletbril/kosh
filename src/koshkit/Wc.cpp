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

enum class wc_count_selection : u8
{
  None = 0,
  Lines = 1,
  Words = 2,
  Bytes = 4,
};

enum class wc_scan_mode : u8
{
  None,
  Lines,
  Words,
  LinesWords,
};

constexpr fn operator|(wc_count_selection left, wc_count_selection right)
    wontthrow -> wc_count_selection
{
  return static_cast<wc_count_selection>(static_cast<u8>(left) |
                                         static_cast<u8>(right));
}

constexpr fn has_wc_count(wc_count_selection selection,
                          wc_count_selection count) wontthrow -> bool
{
  return (static_cast<u8>(selection) & static_cast<u8>(count)) != 0;
}

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
                           wc_count_selection selection) wontthrow
    -> void
{
  let const scan_mode =
      has_wc_count(selection, wc_count_selection::Lines) &&
              has_wc_count(selection, wc_count_selection::Words)
          ? wc_scan_mode::LinesWords
      : has_wc_count(selection, wc_count_selection::Lines)
          ? wc_scan_mode::Lines
      : has_wc_count(selection, wc_count_selection::Words)
          ? wc_scan_mode::Words
          : wc_scan_mode::None;
  if (has_wc_count(selection, wc_count_selection::Bytes))
    state.byte_count += content.length;

  switch (scan_mode) {
  case wc_scan_mode::None: break;
  case wc_scan_mode::Lines: {
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
  case wc_scan_mode::Words:
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
  case wc_scan_mode::LinesWords:
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
                        StringView name, usize field_width,
                        wc_count_selection selection) throws -> void
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

  if (has_wc_count(selection, wc_count_selection::Lines)) do_emit_field(lines);
  if (has_wc_count(selection, wc_count_selection::Words)) do_emit_field(words);
  if (has_wc_count(selection, wc_count_selection::Bytes)) do_emit_field(bytes);

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

  let const has_requested_selection = FLAG_WC_LINES.is_enabled() ||
                                      FLAG_WC_WORDS.is_enabled() ||
                                      FLAG_WC_BYTES.is_enabled();
  let const selection =
      !has_requested_selection
          ? wc_count_selection::Lines | wc_count_selection::Words |
                wc_count_selection::Bytes
      : (FLAG_WC_LINES.is_enabled() ? wc_count_selection::Lines
                                    : wc_count_selection::None) |
            (FLAG_WC_WORDS.is_enabled() ? wc_count_selection::Words
                                        : wc_count_selection::None) |
            (FLAG_WC_BYTES.is_enabled() ? wc_count_selection::Bytes
                                        : wc_count_selection::None);

  let const sources =
      source_list_from_operands(operands, cxt.scratch_allocator());
  let source_states = ArrayList<wc_source_state>{cxt.scratch_allocator()};
  source_states.reserve(sources.count());
  for (usize source_index = 0; source_index < sources.count(); source_index++)
    source_states.push({});

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
      update_wc_source(state, chunk.content, selection);
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
      report_soft_koshkit_util_error(
          ec, cxt, args[0].view(),
          String{cxt.scratch_allocator(), sources[source_index]} + ": " +
              os::last_system_error_message());
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
  if (has_wc_count(selection, wc_count_selection::Lines) &&
      total_lines > max_count) {
    max_count = total_lines;
  }
  if (has_wc_count(selection, wc_count_selection::Words) &&
      total_words > max_count) {
    max_count = total_words;
  }
  if (has_wc_count(selection, wc_count_selection::Bytes) &&
      total_bytes > max_count) {
    max_count = total_bytes;
  }

  let const field_width = decimal_digit_count(max_count);

  let output = String{cxt.scratch_allocator()};
  for (let const &row : rows)
    append_counts(output, row.line_count, row.word_count, row.byte_count,
                  row.name, field_width, selection);

  if (sources.count() > 1)
    append_counts(output, total_lines, total_words, total_bytes,
                  StringView{"total"}, field_width, selection);

  ec.print_to_stdout(output);
  return status;
}

} /* namespace koshkit */

} /* namespace koshka */
