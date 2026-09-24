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
#include "../base/Path.hpp"

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

enum class tail_unit : u8
{
  Lines,
  Bytes,
};

struct parsed_tail_count
{
  count_origin origin;
  i64 count;
};

static fn parse_tail_count(StringView spec) throws -> Maybe<parsed_tail_count>
{
  let origin = count_origin::FromEnd;
  let digits = spec;
  if (digits.length > 0 && digits[0] == '+') {
    origin = count_origin::FromStart;
    digits = digits.substring(1);
  } else if (digits.length > 0 && digits[0] == '-') {
    digits = digits.substring(1);
  }

  let const parsed = digits.to<i64>();
  if (parsed.is_error() || parsed.value() < 0) return None;

  return parsed_tail_count{origin, parsed.value()};
}

constexpr usize TAIL_BLOCK_BYTE_COUNT = 64 * 1024;
constexpr usize TAIL_ACTIVE_SOURCE_COUNT = 16;

struct tail_block
{
  String content;
  u64 offset;
};

struct regular_tail_state
{
  usize source_index{0};
  os::descriptor descriptor{KOSH_INVALID_FD};
  u64 file_size{0};
  u64 next_end{0};
  u64 start_offset{0};
  u64 remaining_newline_count{0};
  usize read_byte_count{0};
  tail_unit unit{tail_unit::Lines};
  bool has_boundary{false};
  bool is_done{false};
  bool has_error{false};
  ArrayList<char> buffer{heap_allocator()};
  ArrayList<tail_block> blocks{heap_allocator()};
};

static fn read_regular_tails(ArrayList<regular_tail_state> &states,
                             ArrayList<Maybe<String>> &outputs,
                             ArrayList<i32> &errors, Allocator allocator) throws
    -> void
{
  let batch = os::Batch{allocator};
  let results = ArrayList<os::batch_result>{allocator};
  let operation_states = ArrayList<usize>{allocator};
  defer
  {
    for (let &state : states)
      if (state.descriptor != KOSH_INVALID_FD)
        unused(os::close_fd(state.descriptor));
  };

  for (let &state : states) {
    if (state.file_size == 0 || state.remaining_newline_count == 0) {
      state.is_done = true;
      outputs[state.source_index] = String{allocator};
      continue;
    }

    if (state.unit == tail_unit::Bytes) {
      state.start_offset = state.file_size > state.remaining_newline_count
                               ? state.file_size - state.remaining_newline_count
                               : 0;
      state.has_boundary = true;
    }
  }

  loop
  {
    batch.clear();
    results.clear();
    operation_states.clear();
    let has_pending = false;
    for (usize state_index = 0; state_index < states.count(); state_index++) {
      let &state = states[state_index];
      if (state.is_done || state.has_error || state.next_end == 0) continue;

      let const block_size = state.next_end > TAIL_BLOCK_BYTE_COUNT
                                 ? TAIL_BLOCK_BYTE_COUNT
                                 : static_cast<usize>(state.next_end);
      state.read_byte_count = block_size;

      let const block_offset = state.next_end - block_size;
      batch.add(os::batch_operation::read(
          state.descriptor, state.buffer.begin(), block_size, block_offset));
      operation_states.push(state_index);
      has_pending = true;
    }
    if (!has_pending) break;

    batch.execute(results);
    if (os::INTERRUPT_REQUESTED) return;
    for (usize result_index = 0; result_index < results.count(); result_index++)
    {
      let &state = states[operation_states[result_index]];
      let const &result = results[result_index];
      if (result.error_number != 0) {
        state.has_error = true;
        state.is_done = true;
        errors[state.source_index] = result.error_number;
        continue;
      }

      let const transferred = result.transferred_byte_count;
      if (transferred == 0) {
        state.is_done = true;
        continue;
      }

      let const block_size = state.read_byte_count;
      let const block_offset = state.next_end - block_size;
      let block = String{allocator};
      block.append(StringView{state.buffer.begin(), transferred});
      state.blocks.push({steal(block), block_offset});

      if (state.unit != tail_unit::Bytes) {
        for (usize position = transferred; position > 0; position--) {
          let const absolute = block_offset + position - 1;
          if (absolute + 1 == state.file_size &&
              state.buffer[position - 1] == '\n')
            continue;
          if (state.buffer[position - 1] != '\n') continue;

          if (--state.remaining_newline_count == 0) {
            state.start_offset = absolute;
            state.has_boundary = true;
            state.is_done = true;
            break;
          }
        }
      }

      state.next_end = block_offset;
      if (transferred < block_size ||
          (state.has_boundary && state.next_end <= state.start_offset))
        state.is_done = true;
    }
  }

  for (let &state : states) {
    if (state.has_error) continue;
    let output = String{allocator};
    for (usize block_index = state.blocks.count(); block_index-- > 0;) {
      let const &block = state.blocks[block_index];
      if (block.offset + block.content.length() <= state.start_offset) continue;

      let const skip_count =
          state.start_offset > block.offset
              ? static_cast<usize>(state.start_offset - block.offset)
              : usize{0};
      output += block.content.substring(skip_count);
    }
    outputs[state.source_index] = steal(output);
  }
}

struct forward_tail_state
{
  usize source_index{0};
  os::descriptor descriptor{KOSH_INVALID_FD};
  u64 file_size{0};
  u64 next_offset{0};
  u64 skipped_newlines{0};
  usize read_byte_count{0};
  tail_unit unit{tail_unit::Lines};
  bool is_done{false};
  bool has_error{false};
  ArrayList<char> buffer{heap_allocator()};
};

static fn read_regular_forward_tails(ArrayList<forward_tail_state> &states,
                                     ArrayList<Maybe<String>> &outputs,
                                     ArrayList<i32> &errors,
                                     Allocator allocator) throws -> void
{
  let batch = os::Batch{allocator};
  let results = ArrayList<os::batch_result>{allocator};
  let operation_states = ArrayList<usize>{allocator};
  defer
  {
    for (let &state : states)
      if (state.descriptor != KOSH_INVALID_FD)
        unused(os::close_fd(state.descriptor));
  };

  for (let &state : states) {
    if (state.next_offset >= state.file_size) {
      state.is_done = true;
      outputs[state.source_index] = String{allocator};
    }
  }

  loop
  {
    batch.clear();
    results.clear();
    operation_states.clear();
    bool has_pending = false;
    for (usize state_index = 0; state_index < states.count(); state_index++) {
      let &state = states[state_index];
      if (state.is_done || state.has_error) continue;

      let const remaining = state.file_size - state.next_offset;
      let const block_size = remaining > TAIL_BLOCK_BYTE_COUNT
                                 ? TAIL_BLOCK_BYTE_COUNT
                                 : static_cast<usize>(remaining);
      state.read_byte_count = block_size;
      batch.add(os::batch_operation::read(state.descriptor,
                                          state.buffer.begin(), block_size,
                                          state.next_offset));
      operation_states.push(state_index);
      has_pending = true;
    }
    if (!has_pending) break;

    batch.execute(results);
    if (os::INTERRUPT_REQUESTED) return;
    for (usize result_index = 0; result_index < results.count(); result_index++)
    {
      let &state = states[operation_states[result_index]];
      let const &result = results[result_index];
      if (result.error_number != 0) {
        state.has_error = true;
        state.is_done = true;
        errors[state.source_index] = result.error_number;
        continue;
      }

      let const transferred = result.transferred_byte_count;
      if (transferred == 0) {
        state.is_done = true;
        continue;
      }

      usize append_start = 0;
      if (state.unit != tail_unit::Bytes && state.skipped_newlines != 0) {
        for (usize position = 0; position < transferred; position++) {
          if (state.buffer[position] != '\n') continue;
          state.skipped_newlines--;
          append_start = position + 1;
          if (state.skipped_newlines == 0) break;
        }
        if (state.skipped_newlines != 0) append_start = transferred;
      }

      if (!outputs[state.source_index].has_value())
        outputs[state.source_index] = String{allocator};
      if (append_start < transferred)
        outputs[state.source_index]->append(StringView{
            state.buffer.begin() + append_start, transferred - append_start});

      state.next_offset += transferred;
      if (transferred < state.read_byte_count ||
          state.next_offset >= state.file_size)
        state.is_done = true;
    }
  }
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
  let const unit =
      FLAG_TAIL_BYTES.is_set() ? tail_unit::Bytes : tail_unit::Lines;
  let parsed_count = Maybe<parsed_tail_count>{
      parsed_tail_count{count_origin::FromEnd, 10}
  };
  if (unit == tail_unit::Bytes) {
    parsed_count = parse_tail_count(FLAG_TAIL_BYTES.value());
    if (!parsed_count.has_value()) {
      throw ErrorWithDetails{
          "invalid byte count '" +
              String{cxt.scratch_allocator(), FLAG_TAIL_BYTES.value()}
              + "'",
          "The count must be a non-negative integer"
      };
    }
  } else if (FLAG_TAIL_LINES.is_set()) {
    parsed_count = parse_tail_count(FLAG_TAIL_LINES.value());
    if (!parsed_count.has_value()) {
      throw ErrorWithDetails{
          "invalid line count '" +
              String{cxt.scratch_allocator(), FLAG_TAIL_LINES.value()}
              + "'",
          "The count must be a non-negative integer"
      };
    }
  }
  let const[origin, count] = *parsed_count;

  let const sources =
      source_list_from_operands(operands, cxt.scratch_allocator());

  let const allocator = cxt.scratch_allocator();
  let paths = ArrayList<Path>{allocator};
  let statuses = ArrayList<os::file_status>{allocator};
  let metadata_errors = ArrayList<i32>{allocator};
  let metadata_source_indices = ArrayList<usize>{allocator};
  let metadata_results = ArrayList<os::batch_result>{allocator};
  let metadata_batch = os::Batch{allocator};
  paths.reserve(sources.count());
  statuses.reserve(sources.count());
  metadata_errors.reserve(sources.count());
  metadata_batch.reserve(sources.count());
  for (let const &source : sources) {
    paths.push(Path{source, allocator});
    statuses.push({});
    metadata_errors.push(0);
  }
  for (usize source_index = 0; source_index < sources.count(); source_index++) {
    if (sources[source_index] == "-") continue;
    metadata_source_indices.push(source_index);
    metadata_batch.add(
        os::batch_operation::stat(paths[source_index], statuses[source_index]));
  }
  if (metadata_batch.count() != 0) {
    metadata_results = metadata_batch.execute();
    for (usize result_index = 0; result_index < metadata_results.count();
         result_index++)
      metadata_errors[metadata_source_indices[result_index]] =
          metadata_results[result_index].error_number;
  }

  let positioned_contents = ArrayList<Maybe<String>>{allocator};
  let positioned_attempted = ArrayList<bool>{allocator};
  let positioned_errors = ArrayList<i32>{allocator};
  let regular_states = ArrayList<regular_tail_state>{allocator};
  let forward_states = ArrayList<forward_tail_state>{allocator};
  positioned_contents.reserve(sources.count());
  positioned_attempted.reserve(sources.count());
  positioned_errors.reserve(sources.count());
  for (usize source_index = 0; source_index < sources.count(); source_index++) {
    positioned_contents.push(None);
    positioned_attempted.push(false);
    positioned_errors.push(0);
  }
  if (origin == count_origin::FromEnd) {
    for (usize source_index = 0; source_index < sources.count(); source_index++)
    {
      if (sources[source_index] == "" || sources[source_index] == "-" ||
          metadata_errors[source_index] != 0 ||
          os::file_type_letter(statuses[source_index].mode) != '-')
        continue;

      let const descriptor = os::open_file_descriptor(sources[source_index],
                                                      os::file_open_mode::Read);
      if (!descriptor.has_value()) continue;
      let const file_size = os::regular_descriptor_file_size(*descriptor);
      if (!file_size.has_value()) {
        unused(os::close_fd(*descriptor));
        continue;
      }

      positioned_attempted[source_index] = true;
      regular_tail_state state{};
      state.source_index = source_index;
      state.descriptor = *descriptor;
      state.file_size = *file_size;
      state.next_end = *file_size;
      state.remaining_newline_count = static_cast<u64>(count);
      state.unit = unit;
      state.buffer = ArrayList<char>{allocator};
      state.blocks = ArrayList<tail_block>{allocator};
      state.buffer.reserve(TAIL_BLOCK_BYTE_COUNT);
      state.blocks.reserve(2);
      regular_states.push(steal(state));

      if (regular_states.count() == TAIL_ACTIVE_SOURCE_COUNT) {
        read_regular_tails(regular_states, positioned_contents,
                           positioned_errors, allocator);
        regular_states.clear();
        if (os::INTERRUPT_REQUESTED) return 130;
      }
    }
    if (regular_states.count() != 0)
      read_regular_tails(regular_states, positioned_contents, positioned_errors,
                         allocator);
    if (os::INTERRUPT_REQUESTED) return 130;
  } else {
    for (usize source_index = 0; source_index < sources.count(); source_index++)
    {
      if (sources[source_index] == "" || sources[source_index] == "-" ||
          metadata_errors[source_index] != 0 ||
          os::file_type_letter(statuses[source_index].mode) != '-')
        continue;

      let const descriptor = os::open_file_descriptor(sources[source_index],
                                                      os::file_open_mode::Read);
      if (!descriptor.has_value()) continue;
      let const file_size = os::regular_descriptor_file_size(*descriptor);
      if (!file_size.has_value()) {
        unused(os::close_fd(*descriptor));
        continue;
      }

      positioned_attempted[source_index] = true;
      forward_tail_state state{};
      state.source_index = source_index;
      state.descriptor = *descriptor;
      state.file_size = *file_size;
      state.unit = unit;
      state.next_offset =
          unit == tail_unit::Bytes
              ? (count == 0 ? 0
                            : (static_cast<u64>(count - 1) < *file_size
                                   ? static_cast<u64>(count - 1)
                                   : *file_size))
              : 0;
      state.skipped_newlines = unit != tail_unit::Bytes && count > 0
                                   ? static_cast<u64>(count - 1)
                                   : 0;
      state.buffer = ArrayList<char>{allocator};
      state.buffer.reserve(TAIL_BLOCK_BYTE_COUNT);
      forward_states.push(steal(state));

      if (forward_states.count() == TAIL_ACTIVE_SOURCE_COUNT) {
        read_regular_forward_tails(forward_states, positioned_contents,
                                   positioned_errors, allocator);
        forward_states.clear();
        if (os::INTERRUPT_REQUESTED) return 130;
      }
    }
    if (forward_states.count() != 0)
      read_regular_forward_tails(forward_states, positioned_contents,
                                 positioned_errors, allocator);
    if (os::INTERRUPT_REQUESTED) return 130;
  }

  let const should_print_headers = sources.count() > 1;
  let output = String{cxt.scratch_allocator()};
  i32 status = 0;
  for (usize source_index = 0; source_index < sources.count(); source_index++) {
    Maybe<String> content;
    bool did_use_positioned_read = false;
    if (positioned_attempted[source_index]) {
      content = steal(positioned_contents[source_index]);
      did_use_positioned_read = true;
    }
    if (!did_use_positioned_read)
      content = read_named_or_stdin(ec, sources[source_index]);
    if (os::INTERRUPT_REQUESTED) return 130;
    if (!content.has_value()) {
      if (positioned_errors[source_index] != 0)
        os::set_last_system_error(positioned_errors[source_index]);
      report_soft_koshkit_util_error(
          ec, cxt, args[0].view(),
          String{did_use_positioned_read ? "cannot read '" : "cannot open '"} +
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

    if (unit == tail_unit::Bytes) {
      let const wanted_count = static_cast<usize>(count);
      let const text = content->view();
      let start = origin == count_origin::FromStart
                      ? (did_use_positioned_read || count == 0
                             ? 0
                             : static_cast<usize>(count - 1))
                      : sub_sat(text.length, wanted_count);
      if (start > text.length) start = text.length;

      output += text.substring(start);
      continue;
    }

    let const text = content->view();
    let const wanted_count = static_cast<usize>(count);
    usize start = 0;
    if (origin == count_origin::FromStart && !did_use_positioned_read) {
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
