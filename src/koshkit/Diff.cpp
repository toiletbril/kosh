/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements the diff utility in koshkit. It computes bounded-memory
 * line edit scripts and renders normal or unified file differences without a
 * host diff executable.
 */

#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-uwa] [-L label] file1 file2");

HELP_DESCRIPTION_DECL("The diff utility compares two files line by line.");

FLAG(DIFF_UNIFIED, Bool, 'u', "unified", "Write unified differences.");
FLAG(DIFF_IGNORE_SPACE, Bool, 'w', "ignore-all-space",
     "Ignore whitespace differences.");
FLAG(DIFF_TEXT, Bool, 'a', "text", "Treat every file as text.");
FLAG(DIFF_LABEL, ManyStrings, 'L', "label", "Use this file label.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Diff);

namespace koshka::koshkit {

enum class diff_edit : u8
{
  Equal,
  Delete,
  Insert,
};

struct diff_edit_result
{
  ArrayList<diff_edit> edits;
  bool was_interrupted;
};

struct diff_lines
{
  StringView contents;
  ArrayList<u32> end_positions;

  pure fn count() const wontthrow -> usize { return end_positions.count(); }

  pure fn get(usize index) const wontthrow -> StringView
  {
    let const start = index == 0 ? 0 : end_positions[index - 1];
    let const end = end_positions[index];
    return contents.substring_of_length(start, end - start);
  }
};

static fn split_diff_lines(StringView contents, Allocator allocator) throws
    -> diff_lines
{
  if (contents.length > UINT32_MAX) throw std::bad_alloc{};

  let end_positions = ArrayList<u32>{allocator};
  for (usize position = 0; position < contents.length; position++) {
    if (os::INTERRUPT_REQUESTED) break;
    if (contents[position] == '\n')
      end_positions.push(static_cast<u32>(position + 1));
  }
  if (!contents.is_empty() && contents[contents.length - 1] != '\n')
    end_positions.push(static_cast<u32>(contents.length));

  return {contents, steal(end_positions)};
}

static pure fn diff_lines_equal(StringView left, StringView right,
                                bool should_ignore_space) wontthrow -> bool
{
  if (!should_ignore_space) return left == right;

  usize left_position = 0;
  usize right_position = 0;
  loop
  {
    if (os::INTERRUPT_REQUESTED) return false;

    while (left_position < left.length &&
           is_ascii_whitespace(left[left_position]))
      left_position++;
    while (right_position < right.length &&
           is_ascii_whitespace(right[right_position]))
      right_position++;

    if (left_position == left.length || right_position == right.length)
      return left_position == left.length && right_position == right.length;
    if (left[left_position] != right[right_position]) return false;

    left_position++;
    right_position++;
  }
}

static fn append_replacement_edits(ArrayList<diff_edit> &edits,
                                   usize equal_prefix_count,
                                   usize left_middle_count,
                                   usize right_middle_count) throws -> bool
{
  for (usize index = 0; index < equal_prefix_count; index++) {
    if (os::INTERRUPT_REQUESTED) return false;
    edits.push(diff_edit::Equal);
  }
  for (usize index = 0; index < left_middle_count; index++) {
    if (os::INTERRUPT_REQUESTED) return false;
    edits.push(diff_edit::Delete);
  }
  for (usize index = 0; index < right_middle_count; index++) {
    if (os::INTERRUPT_REQUESTED) return false;
    edits.push(diff_edit::Insert);
  }
  return true;
}

static fn make_diff_edits(const diff_lines &left_lines,
                          const diff_lines &right_lines,
                          bool should_ignore_space, Allocator allocator) throws
    -> diff_edit_result
{
  let edits = ArrayList<diff_edit>{allocator};
  usize equal_prefix_count = 0;
  while (equal_prefix_count < left_lines.count() &&
         equal_prefix_count < right_lines.count() &&
         diff_lines_equal(left_lines.get(equal_prefix_count),
                          right_lines.get(equal_prefix_count),
                          should_ignore_space))
  {
    if (os::INTERRUPT_REQUESTED) return {steal(edits), true};
    equal_prefix_count++;
  }

  let const left_middle_count = left_lines.count() - equal_prefix_count;
  let const right_middle_count = right_lines.count() - equal_prefix_count;

  if (left_middle_count == 0 || right_middle_count == 0) {
    if (!append_replacement_edits(edits, equal_prefix_count, left_middle_count,
                                  right_middle_count))
      return {steal(edits), true};
    return {steal(edits), false};
  }

  constexpr usize MAXIMUM_TRACE_ELEMENT_COUNT = 262144;
  let trace = ArrayList<u32>{allocator};
  let first_frontier = ArrayList<u32>{allocator};
  let second_frontier = ArrayList<u32>{allocator};
  let frontier = &first_frontier;
  let next_frontier = &second_frontier;
  let const maximum_distance = left_middle_count + right_middle_count;
  usize found_distance = maximum_distance + 1;

  for (usize distance = 0; distance <= maximum_distance; distance++) {
    if (os::INTERRUPT_REQUESTED) return {steal(edits), true};
    if (trace.count() > MAXIMUM_TRACE_ELEMENT_COUNT - (distance + 1)) {
      if (!append_replacement_edits(edits, equal_prefix_count,
                                    left_middle_count, right_middle_count))
        return {steal(edits), true};
      return {steal(edits), false};
    }

    next_frontier->clear();
    next_frontier->reserve(distance + 1);
    let const signed_distance = static_cast<i64>(distance);
    for (usize step = 0; step <= distance; step++) {
      let const diagonal = -signed_distance + static_cast<i64>(step) * 2;
      usize left_position = 0;
      if (distance != 0) {
        if (step == 0)
          left_position = (*frontier)[step];
        else if (step == distance)
          left_position = static_cast<usize>((*frontier)[step - 1]) + 1;
        else
          left_position = (*frontier)[step - 1] < (*frontier)[step]
                              ? (*frontier)[step]
                              : static_cast<usize>((*frontier)[step - 1]) + 1;
      }

      let const right_signed = static_cast<i64>(left_position) - diagonal;
      ASSERT(right_signed >= 0);
      usize right_position = static_cast<usize>(right_signed);
      while (
          left_position < left_middle_count &&
          right_position < right_middle_count &&
          diff_lines_equal(left_lines.get(equal_prefix_count + left_position),
                           right_lines.get(equal_prefix_count + right_position),
                           should_ignore_space))
      {
        if (os::INTERRUPT_REQUESTED) return {steal(edits), true};
        left_position++;
        right_position++;
      }

      ASSERT(left_position <= UINT32_MAX);
      next_frontier->push(static_cast<u32>(left_position));
      trace.push(static_cast<u32>(left_position));
      if (left_position == left_middle_count &&
          right_position == right_middle_count)
      {
        found_distance = distance;
        break;
      }
    }

    let saved_frontier = frontier;
    frontier = next_frontier;
    next_frontier = saved_frontier;
    if (found_distance != maximum_distance + 1) break;
  }

  ASSERT(found_distance <= maximum_distance);
  usize left_position = left_middle_count;
  usize right_position = right_middle_count;
  for (usize distance = found_distance; distance > 0; distance--) {
    if (os::INTERRUPT_REQUESTED) return {steal(edits), true};

    let const diagonal =
        static_cast<i64>(left_position) - static_cast<i64>(right_position);
    let const signed_distance = static_cast<i64>(distance);
    ASSERT(diagonal >= -signed_distance && diagonal <= signed_distance);
    let const step = static_cast<usize>((diagonal + signed_distance) / 2);
    let const previous_row = (distance - 1) * distance / 2;
    let const is_insertion =
        step == 0 || (step < distance && trace[previous_row + step - 1] <
                                             trace[previous_row + step]);
    let const previous_diagonal = diagonal + (is_insertion ? 1 : -1);
    let const previous_step =
        static_cast<usize>((previous_diagonal + signed_distance - 1) / 2);
    let const previous_left =
        static_cast<usize>(trace[previous_row + previous_step]);
    let const previous_right_signed =
        static_cast<i64>(previous_left) - previous_diagonal;
    ASSERT(previous_right_signed >= 0);
    let const previous_right = static_cast<usize>(previous_right_signed);

    while (left_position > previous_left && right_position > previous_right) {
      if (os::INTERRUPT_REQUESTED) return {steal(edits), true};
      edits.push(diff_edit::Equal);
      left_position--;
      right_position--;
    }

    if (is_insertion) {
      edits.push(diff_edit::Insert);
      right_position--;
    } else {
      edits.push(diff_edit::Delete);
      left_position--;
    }
  }

  while (left_position > 0 && right_position > 0) {
    if (os::INTERRUPT_REQUESTED) return {steal(edits), true};
    edits.push(diff_edit::Equal);
    left_position--;
    right_position--;
  }
  ASSERT(left_position == 0 && right_position == 0);

  for (usize index = 0; index < equal_prefix_count; index++) {
    if (os::INTERRUPT_REQUESTED) return {steal(edits), true};
    edits.push(diff_edit::Equal);
  }
  for (usize index = 0; index < edits.count() / 2; index++) {
    if (os::INTERRUPT_REQUESTED) return {steal(edits), true};
    let const opposite = edits.count() - index - 1;
    let const saved = edits[index];
    edits[index] = edits[opposite];
    edits[opposite] = saved;
  }

  return {steal(edits), false};
}

static fn append_diff_number(String &output, usize value) throws -> void
{
  output += String::from(static_cast<u64>(value), output.allocator());
}

static fn flush_diff_output(const ExecContext &ec, String &output) throws
    -> void
{
  constexpr usize OUTPUT_BUFFER_LENGTH = 64 * 1024;
  if (output.count() < OUTPUT_BUFFER_LENGTH) return;

  ec.print_to_stdout(output);
  output.clear();
}

static fn append_diff_line(const ExecContext &ec, String &output,
                           StringView prefix, StringView line) throws -> void
{
  output += prefix;
  output += line;
  if (line.is_empty() || line[line.length - 1] != '\n') {
    output += '\n';
    output += "\\ No newline at end of file\n";
  }
  flush_diff_output(ec, output);
}

static fn append_normal_range(String &output, usize start, usize count) throws
    -> void
{
  append_diff_number(output, start + 1);
  if (count > 1) {
    output += ',';
    append_diff_number(output, start + count);
  }
}

static fn append_normal_diff(const ExecContext &ec, String &output,
                             const diff_lines &left_lines,
                             const diff_lines &right_lines,
                             const ArrayList<diff_edit> &edits) throws -> bool
{
  usize edit_position = 0;
  usize left_position = 0;
  usize right_position = 0;
  while (edit_position < edits.count()) {
    if (os::INTERRUPT_REQUESTED) return false;

    while (edit_position < edits.count() &&
           edits[edit_position] == diff_edit::Equal)
    {
      edit_position++;
      left_position++;
      right_position++;
    }
    if (edit_position == edits.count()) break;

    let const block_start = edit_position;
    let const left_start = left_position;
    let const right_start = right_position;
    while (edit_position < edits.count() &&
           edits[edit_position] != diff_edit::Equal)
    {
      if (edits[edit_position] == diff_edit::Delete)
        left_position++;
      else
        right_position++;
      edit_position++;
    }
    let const left_count = left_position - left_start;
    let const right_count = right_position - right_start;

    if (left_count == 0)
      append_diff_number(output, left_start);
    else
      append_normal_range(output, left_start, left_count);
    output += left_count == 0 ? 'a' : right_count == 0 ? 'd' : 'c';
    if (right_count == 0)
      append_diff_number(output, right_start);
    else
      append_normal_range(output, right_start, right_count);
    output += '\n';
    flush_diff_output(ec, output);

    usize block_left = left_start;
    for (usize index = block_start; index < edit_position; index++) {
      if (os::INTERRUPT_REQUESTED) return false;
      if (edits[index] == diff_edit::Delete)
        append_diff_line(ec, output, "< ", left_lines.get(block_left++));
    }
    if (left_count != 0 && right_count != 0) output += "---\n";
    usize block_right = right_start;
    for (usize index = block_start; index < edit_position; index++) {
      if (os::INTERRUPT_REQUESTED) return false;
      if (edits[index] == diff_edit::Insert)
        append_diff_line(ec, output, "> ", right_lines.get(block_right++));
    }
  }

  return true;
}

static fn append_unified_range(String &output, usize start, usize count) throws
    -> void
{
  append_diff_number(output, count == 0 ? start : start + 1);
  if (count != 1) {
    output += ',';
    append_diff_number(output, count);
  }
}

static fn make_diff_header_name(StringView operand, Maybe<StringView> label,
                                Allocator allocator) throws -> String
{
  if (label.has_value()) return String{allocator, *label};

  let result = String{allocator, operand};
  if (operand != "-")
    if (let const modification_time = os::path_modification_time(operand)) {
      result += '\t';
      result +=
          utils::format_unix_timestamp(*modification_time, "%Y-%m-%d %H:%M:%S");
    }

  return result;
}

static fn advance_diff_position(diff_edit edit, usize &left_position,
                                usize &right_position) wontthrow -> void
{
  if (edit != diff_edit::Insert) left_position++;
  if (edit != diff_edit::Delete) right_position++;
}

static fn append_unified_diff(const ExecContext &ec, String &output,
                              StringView left_label, StringView right_label,
                              const diff_lines &left_lines,
                              const diff_lines &right_lines,
                              const ArrayList<diff_edit> &edits) throws -> bool
{
  output += "--- ";
  output += left_label;
  output += "\n+++ ";
  output += right_label;
  output += '\n';
  flush_diff_output(ec, output);

  usize next_search = 0;
  usize current_edit = 0;
  usize left_position = 0;
  usize right_position = 0;
  while (next_search < edits.count()) {
    if (os::INTERRUPT_REQUESTED) return false;

    while (next_search < edits.count() &&
           edits[next_search] == diff_edit::Equal)
      next_search++;
    if (next_search == edits.count()) break;

    let const first_change = next_search;
    usize last_change = first_change;
    for (usize index = first_change + 1; index < edits.count(); index++) {
      if (os::INTERRUPT_REQUESTED) return false;
      if (edits[index] == diff_edit::Equal) continue;
      if (index - last_change > 7) break;
      last_change = index;
    }
    let const hunk_start = first_change > 3 ? first_change - 3 : 0;
    let const hunk_end =
        last_change + 4 < edits.count() ? last_change + 4 : edits.count();

    while (current_edit < hunk_start) {
      advance_diff_position(edits[current_edit], left_position, right_position);
      current_edit++;
    }

    let const left_start = left_position;
    let const right_start = right_position;
    usize left_count = 0;
    usize right_count = 0;
    for (usize index = hunk_start; index < hunk_end; index++) {
      if (os::INTERRUPT_REQUESTED) return false;
      if (edits[index] != diff_edit::Insert) left_count++;
      if (edits[index] != diff_edit::Delete) right_count++;
    }

    output += "@@ -";
    append_unified_range(output, left_start, left_count);
    output += " +";
    append_unified_range(output, right_start, right_count);
    output += " @@\n";
    flush_diff_output(ec, output);
    while (current_edit < hunk_end) {
      if (os::INTERRUPT_REQUESTED) return false;
      switch (edits[current_edit]) {
      case diff_edit::Equal:
        append_diff_line(ec, output, " ", left_lines.get(left_position));
        break;
      case diff_edit::Delete:
        append_diff_line(ec, output, "-", left_lines.get(left_position));
        break;
      case diff_edit::Insert:
        append_diff_line(ec, output, "+", right_lines.get(right_position));
        break;
      }
      advance_diff_position(edits[current_edit], left_position, right_position);
      current_edit++;
    }
    next_search = hunk_end;
  }

  return true;
}

Diff::Diff() = default;

pure fn Diff::kind() const wontthrow -> Utility::Kind { return Kind::Diff; }

fn Diff::execute(const ExecContext &ec, EvalContext &cxt,
                 const ArrayList<String> &args,
                 const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let const operands = parse_util_operands(FLAG_LIST, args, &arg_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  if (operands.count() != 2 || FLAG_DIFF_LABEL.count() > 2)
    return report_usage_error(ec, cxt, args[0].view());

  if (operands[0] == "-" && operands[1] == "-") return 0;

  let left_contents = read_named_or_stdin(ec, operands[0].view());
  if (!left_contents.has_value()) {
    report_soft_koshkit_error(ec, cxt,
                              "diff: cannot read '" + operands[0] +
                                  "': " + os::last_system_error_message());
    return 2;
  }
  let right_contents = read_named_or_stdin(ec, operands[1].view());
  if (!right_contents.has_value()) {
    report_soft_koshkit_error(ec, cxt,
                              "diff: cannot read '" + operands[1] +
                                  "': " + os::last_system_error_message());
    return 2;
  }
  if (left_contents->view() == right_contents->view()) return 0;

  if (!FLAG_DIFF_TEXT.is_enabled() &&
      (left_contents->view().find_character('\0').has_value() ||
       right_contents->view().find_character('\0').has_value()))
  {
    let const left_name = FLAG_DIFF_LABEL.count() >= 1 ? FLAG_DIFF_LABEL.get(0)
                                                       : operands[0].view();
    let const right_name = FLAG_DIFF_LABEL.count() == 2 ? FLAG_DIFF_LABEL.get(1)
                                                        : operands[1].view();
    ec.print_to_stdout("Binary files " +
                       String{cxt.scratch_allocator(), left_name} + " and " +
                       right_name + " differ\n");
    return 1;
  }

  let const left_lines =
      split_diff_lines(left_contents->view(), cxt.scratch_allocator());
  if (os::INTERRUPT_REQUESTED) return 130;
  let const right_lines =
      split_diff_lines(right_contents->view(), cxt.scratch_allocator());
  if (os::INTERRUPT_REQUESTED) return 130;
  let const result = make_diff_edits(left_lines, right_lines,
                                     FLAG_DIFF_IGNORE_SPACE.is_enabled(),
                                     cxt.scratch_allocator());
  if (result.was_interrupted) return 130;

  bool has_difference = false;
  for (let const edit : result.edits) {
    if (os::INTERRUPT_REQUESTED) return 130;
    if (edit != diff_edit::Equal) {
      has_difference = true;
      break;
    }
  }
  if (!has_difference) return 0;

  let output = String{cxt.scratch_allocator()};
  if (FLAG_DIFF_UNIFIED.is_enabled()) {
    let const left_name = make_diff_header_name(
        operands[0].view(),
        FLAG_DIFF_LABEL.count() >= 1 ? Maybe<StringView>{FLAG_DIFF_LABEL.get(0)}
                                     : None,
        cxt.scratch_allocator());
    let const right_name = make_diff_header_name(
        operands[1].view(),
        FLAG_DIFF_LABEL.count() == 2 ? Maybe<StringView>{FLAG_DIFF_LABEL.get(1)}
                                     : None,
        cxt.scratch_allocator());
    if (!append_unified_diff(ec, output, left_name, right_name, left_lines,
                             right_lines, result.edits))
      return 130;
  } else {
    if (!append_normal_diff(ec, output, left_lines, right_lines, result.edits))
      return 130;
  }

  ec.print_to_stdout(output);
  return 1;
}

} /* namespace koshka::koshkit */
