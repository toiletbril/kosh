/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements the tail utility. It selects trailing or offset-based
 * lines or bytes from each complete input while preserving source order.
 */

#include "../CLI.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-n count] [-c count] [file ...]");

HELP_DESCRIPTION_DECL("The tail utility writes the last lines of each file.");

FLAG(TAIL_LINES, String, 'n', "", "Write the last count lines.");
FLAG(TAIL_BYTES, String, 'c', "", "Write the last count bytes.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Tail);

namespace koshka {

namespace koshkit {

enum class count_origin : u8
{
  FromEnd,
  FromStart
};

static fn parse_tail_count(StringView spec, count_origin &origin_out,
                           i64 &count_out) throws -> bool
{
  origin_out = count_origin::FromEnd;
  let digits = spec;
  if (digits.length > 0 && digits[0] == '+') {
    origin_out = count_origin::FromStart;
    digits = digits.substring(1);
  } else if (digits.length > 0 && digits[0] == '-') {
    digits = digits.substring(1);
  }

  let const parsed = digits.to<i64>();
  if (parsed.is_error() || parsed.value() < 0) {
    return false;
  }

  count_out = parsed.value();
  return true;
}

constexpr usize TAIL_BLOCK_BYTE_COUNT = 64 * 1024;

struct tail_block
{
  String content;
  u64 offset;
};

static fn read_regular_tail(os::descriptor descriptor, u64 file_size,
                            bool is_byte_mode, u64 count, Allocator allocator)
    throws -> Maybe<String>
{
  let blocks = ArrayList<tail_block>{allocator};
  u64 next_end = file_size;
  u64 start_offset = 0;
  u64 remaining_newline_count = count;
  bool has_boundary = is_byte_mode;

  if (count == 0 || file_size == 0) return String{allocator};

  if (is_byte_mode)
    start_offset = file_size > count ? file_size - count : 0;

  while (next_end > 0 && (!has_boundary || next_end > start_offset)) {
    let const block_size = next_end > TAIL_BLOCK_BYTE_COUNT
                              ? TAIL_BLOCK_BYTE_COUNT
                              : static_cast<usize>(next_end);
    let const block_offset = next_end - block_size;
    char bytes[TAIL_BLOCK_BYTE_COUNT];
    let batch = os::Batch{allocator};
    let results = ArrayList<os::batch_result>{allocator};
    batch.add(os::batch_operation::read(descriptor, bytes, block_size,
                                        block_offset));
    batch.execute(results);
    if (results.is_empty() || results[0].error_number != 0) return None;

    let const transferred = results[0].transferred_byte_count;
    if (transferred == 0) break;

    let block = String{allocator};
    block.append(StringView{bytes, transferred});
    blocks.push({block.take(), block_offset});

    if (!is_byte_mode) {
      for (usize position = transferred; position > 0; position--) {
        let const absolute = block_offset + position - 1;
        if (absolute + 1 == file_size && bytes[position - 1] == '\n') continue;
        if (bytes[position - 1] != '\n') continue;

        if (--remaining_newline_count == 0) {
          start_offset = absolute;
          has_boundary = true;
          break;
        }
      }
    }

    next_end = block_offset;
    if (transferred < block_size) break;
  }

  let output = String{allocator};
  for (usize index = blocks.count(); index-- > 0;) {
    let const &block = blocks[index];
    if (block.offset + block.content.length() <= start_offset) continue;

    let const skip_count = start_offset > block.offset
                               ? static_cast<usize>(start_offset - block.offset)
                               : usize{0};
    output += block.content.substring(skip_count);
  }

  return output;
}

Tail::Tail() = default;

pure fn Tail::kind() const wontthrow -> Utility::Kind { return Kind::Tail; }

fn Tail::execute(const ExecContext &ec, EvalContext &cxt,
                 const ArrayList<String> &args,
                 const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let const[operands, operand_locations] = parse_util_operands(
      FLAG_LIST, args, cxt.scratch_allocator(), &arg_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  /* -c takes precedence over -n when both are given, matching GNU tail. */
  let const is_byte_mode = FLAG_TAIL_BYTES.is_set();
  let origin = count_origin::FromEnd;
  i64 count = 10;
  if (is_byte_mode) {
    if (!parse_tail_count(FLAG_TAIL_BYTES.value(), origin, count)) {
      throw ErrorWithDetails{
          "invalid byte count '" +
              String{cxt.scratch_allocator(), FLAG_TAIL_BYTES.value()}
              + "'",
          "The count must be a non-negative integer"
      };
    }
  } else if (FLAG_TAIL_LINES.is_set()) {
    if (!parse_tail_count(FLAG_TAIL_LINES.value(), origin, count)) {
      throw ErrorWithDetails{
          "invalid line count '" +
              String{cxt.scratch_allocator(), FLAG_TAIL_LINES.value()}
              + "'",
          "The count must be a non-negative integer"
      };
    }
  }

  let const sources =
      source_list_from_operands(operands, cxt.scratch_allocator());

  let const should_print_headers = sources.count() > 1;
  let output = String{cxt.scratch_allocator()};
  i32 status = 0;
  for (usize source_index = 0; source_index < sources.count(); source_index++) {
    Maybe<String> content;
    bool did_use_positioned_read = false;
    if (origin == count_origin::FromEnd && sources[source_index] != "-") {
      os::file_status source_status{};
      if (os::stat_path(sources[source_index], source_status) &&
          os::file_type_letter(source_status.mode) == '-')
      {
        let const descriptor = os::open_file_descriptor(
            sources[source_index], os::file_open_mode::Read);
        if (descriptor.has_value()) {
          let const file_size = os::regular_descriptor_file_size(*descriptor);
          if (file_size.has_value()) {
            content = read_regular_tail(
                *descriptor, *file_size, is_byte_mode,
                static_cast<u64>(count), cxt.scratch_allocator());
            did_use_positioned_read = true;
          }
          unused(os::close_fd(*descriptor));
        }
      }
    }
    if (!did_use_positioned_read)
      content = read_named_or_stdin(ec, sources[source_index]);
    if (os::INTERRUPT_REQUESTED) return 130;
    if (!content.has_value()) {
      report_soft_koshkit_util_error(
          ec, cxt, args[0].view(),
          "cannot open '" +
              String{cxt.scratch_allocator(), sources[source_index]} +
              "': " + os::last_system_error_message());
      status = 1;
      continue;
    }

    if (should_print_headers) {
      if (source_index > 0) output += '\n';
      output += "==> ";
      output += sources[source_index] == "-" ? StringView{"standard input"}
                                             : sources[source_index];
      output += " <==\n";
    }

    if (is_byte_mode) {
      let const wanted_count = static_cast<usize>(count);
      let const text = content->view();
      let start = origin == count_origin::FromStart
                      ? (count > 0 ? static_cast<usize>(count - 1) : 0)
                      : sub_sat(text.length, wanted_count);
      if (start > text.length) start = text.length;

      output += text.substring(start);
      continue;
    }

    let const text = content->view();
    let const wanted_count = static_cast<usize>(count);
    usize start = 0;
    if (origin == count_origin::FromStart) {
      usize remaining_newline_count = count > 0 ? wanted_count - 1 : 0;
      while (start < text.length && remaining_newline_count > 0) {
        if (text[start] == '\n') remaining_newline_count--;
        start++;
      }
      if (remaining_newline_count > 0) start = text.length;
    } else if (wanted_count == 0) {
      start = text.length;
    } else {
      start = text.length;
      usize remaining_newline_count = wanted_count;
      if (start > 0 && text[start - 1] == '\n') start--;
      while (start > 0) {
        if (text[start - 1] == '\n' && --remaining_newline_count == 0) break;
        start--;
      }
    }
    output += text.substring(start);
  }

  ec.print_to_stdout(output);
  return status;
}

} /* namespace koshkit */

} /* namespace koshka */
