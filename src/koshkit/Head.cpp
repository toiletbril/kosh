/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements the head utility. It streams leading lines or bytes and
 * supports negative counts that omit a suffix from seekable or buffered input.
 */

#include "../CLI.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Utils.hpp"
#include "../base/Path.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-n count] [-c count] [file ...]");

HELP_DESCRIPTION_DECL("The head utility writes the first lines of each file.");

FLAG(HEAD_LINES, String, 'n', "", "Write the first count lines.");
FLAG(HEAD_BYTES, String, 'c', "", "Write the first count bytes.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Head);

namespace koshka {

namespace koshkit {

enum class head_unit : u8
{
  Lines,
  Bytes,
};

static fn read_all(os::descriptor fd, Allocator allocator) throws
    -> Maybe<String>
{
  if (os::INTERRUPT_REQUESTED) return String{allocator};
  return os::read_fd_to_string(fd, allocator);
}

static fn line_prefix_length_dropping_last(StringView text,
                                           u64 drop_count) wontthrow -> usize
{
  u64 total_line_count = 0;
  for (usize i = 0; i < text.length; i++) {
    if (text[i] == '\n') total_line_count++;
  }

  if (text.length > 0 && text[text.length - 1] != '\n') {
    total_line_count++;
  }

  if (drop_count >= total_line_count) return 0;
  let const keep_count = total_line_count - drop_count;

  u64 lines_seen = 0;
  for (usize i = 0; i < text.length; i++) {
    if (text[i] == '\n') {
      lines_seen++;
      if (lines_seen == keep_count) return i + 1;
    }
  }

  return text.length;
}

static fn byte_prefix_length_dropping_last(StringView text,
                                           u64 drop_count) wontthrow -> usize
{
  if (drop_count >= text.length) return 0;

  return text.length - static_cast<usize>(drop_count);
}

static fn read_all_but_last(os::descriptor fd, u64 drop_count,
                            Allocator allocator, head_unit unit) throws
    -> Maybe<String>
{
  let text = read_all(fd, allocator);
  if (!text.has_value()) return None;

  let const keep_length =
      unit == head_unit::Bytes
          ? byte_prefix_length_dropping_last(text->view(), drop_count)
          : line_prefix_length_dropping_last(text->view(), drop_count);
  text->truncate(keep_length);
  return text;
}

static fn read_regular_all_but_last(os::descriptor fd, u64 file_size,
                                    u64 drop_count, Allocator allocator,
                                    head_unit unit) throws -> Maybe<String>
{
  constexpr usize block_byte_count = 64 * 1024;
  constexpr usize batch_block_count = 16;
  char block[block_byte_count];
  let batch = os::Batch{allocator};
  let results = ArrayList<os::batch_result>{allocator};
  let buffers = ArrayList<ArrayList<char>>{allocator};
  let byte_counts = ArrayList<usize>{allocator};
  batch.reserve(batch_block_count);
  results.reserve(batch_block_count);
  buffers.reserve(batch_block_count);
  byte_counts.reserve(batch_block_count);

  let const do_read_block = [&](u64 offset,
                                usize byte_count) -> os::batch_result {
    batch.clear();
    batch.add(os::batch_operation::read(fd, block, byte_count, offset));
    batch.execute(results);
    return results[0];
  };

  u64 prefix_end = file_size;
  if (unit == head_unit::Bytes) {
    prefix_end = drop_count >= file_size ? 0 : file_size - drop_count;
  } else if (drop_count != 0) {
    u64 remaining_lines = drop_count;
    u64 next_end = file_size;
    prefix_end = 0;
    while (next_end != 0) {
      if (os::INTERRUPT_REQUESTED) return String{allocator};

      let const byte_count = next_end > block_byte_count
                                 ? block_byte_count
                                 : static_cast<usize>(next_end);
      let const offset = next_end - byte_count;
      let const read_result = do_read_block(offset, byte_count);
      if (read_result.error_number != 0) {
        os::set_last_system_error(read_result.error_number);
        return None;
      }
      if (read_result.transferred_byte_count != byte_count)
        return read_all_but_last(fd, drop_count, allocator, unit);

      for (usize position = byte_count; position > 0; position--) {
        if (block[position - 1] != '\n') continue;
        let const absolute = offset + position - 1;
        if (absolute + 1 == file_size) continue;
        if (--remaining_lines == 0) {
          prefix_end = absolute + 1;
          break;
        }
      }
      if (remaining_lines == 0) break;
      next_end = offset;
    }
  }

  let result = String{allocator};
  u64 next_offset = 0;
  while (next_offset < prefix_end) {
    if (os::INTERRUPT_REQUESTED) return String{allocator};

    batch.clear();
    results.clear();
    buffers.clear();
    byte_counts.clear();
    while (next_offset < prefix_end &&
           byte_counts.count() < batch_block_count)
    {
      let const remaining = prefix_end - next_offset;
      let const byte_count = remaining > block_byte_count
                                 ? block_byte_count
                                 : static_cast<usize>(remaining);
      buffers.push(ArrayList<char>{allocator});
      buffers.back().reserve(byte_count);
      byte_counts.push(byte_count);
      batch.add(os::batch_operation::read(fd, buffers.back().begin(),
                                          byte_count, next_offset));
      next_offset += byte_count;
    }

    batch.execute(results);
    if (os::INTERRUPT_REQUESTED) return String{allocator};
    for (usize result_index = 0; result_index < results.count();
         result_index++)
    {
      let const &read_result = results[result_index];
      if (read_result.error_number != 0) {
        os::set_last_system_error(read_result.error_number);
        return None;
      }
      if (read_result.transferred_byte_count != byte_counts[result_index])
        return read_all_but_last(fd, drop_count, allocator, unit);
    }

    for (usize result_index = 0; result_index < results.count();
         result_index++)
      result.append(StringView{buffers[result_index].begin(),
                               byte_counts[result_index]});
  }

  return result;
}

Head::Head() = default;

pure fn Head::kind() const wontthrow -> Utility::Kind { return Kind::Head; }

fn Head::execute(const ExecContext &ec, EvalContext &cxt,
                 const ArrayList<String> &args,
                 const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let const[operands, operand_locations] = parse_util_operands(
      FLAG_LIST, args, cxt.scratch_allocator(), &arg_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  let const has_bytes_flag = FLAG_HEAD_BYTES.is_set();
  let const has_lines_flag = FLAG_HEAD_LINES.is_set();
  let const is_byte_mode =
      has_bytes_flag && (!has_lines_flag || FLAG_HEAD_BYTES.position() >
                                                FLAG_HEAD_LINES.position());
  let const unit = is_byte_mode ? head_unit::Bytes : head_unit::Lines;

  u64 count = 10;
  bool is_all_but_last = false;
  if (is_byte_mode) {
    let const raw = FLAG_HEAD_BYTES.value();
    is_all_but_last = raw.length > 0 && raw[0] == '-';
    let const magnitude = is_all_but_last ? raw.substring(1) : raw;
    let const parsed_value = utils::parse_decimal_u64(magnitude);
    if (parsed_value.is_error()) {
      throw ErrorWithDetails{
          "invalid byte count '" + String{cxt.scratch_allocator(), raw}
            + "'",
          "The count must be an integer"
      };
    }
    count = parsed_value.value();
  } else if (has_lines_flag) {
    let const raw = FLAG_HEAD_LINES.value();
    is_all_but_last = raw.length > 0 && raw[0] == '-';
    let const magnitude = is_all_but_last ? raw.substring(1) : raw;
    let const parsed_value = utils::parse_decimal_u64(magnitude);
    if (parsed_value.is_error()) {
      throw ErrorWithDetails{
          "invalid line count '" + String{cxt.scratch_allocator(), raw}
            + "'",
          "The count must be an integer"
      };
    }
    count = parsed_value.value();
  }

  let const sources =
      source_list_from_operands(operands, cxt.scratch_allocator());

  let const should_print_headers = sources.count() > 1;
  let const do_print_source = [&](usize source_index, StringView text)
                                  throws -> void {
    let output = String{cxt.scratch_allocator()};
    if (should_print_headers) {
      if (source_index > 0) output += '\n';
      output += "==> ";
      output += sources[source_index];
      output += " <==\n";
    }
    output += text;
    ec.print_to_stdout(output);
  };

  i32 status = 0;
  if (is_all_but_last) {
    for (usize source_index = 0; source_index < sources.count(); source_index++)
    {
      os::descriptor fd;
      bool was_opened = false;
      if (sources[source_index] == "-") {
        fd = ec.in_fd.value_or(KOSH_STDIN);
      } else {
        let const opened_fd = os::open_file_descriptor(
            sources[source_index], os::file_open_mode::Read);
        if (!opened_fd.has_value()) {
          if (os::INTERRUPT_REQUESTED) return 130;

          report_soft_koshkit_util_error(
              ec, cxt, args[0].view(),
              "cannot open '" +
                  String{cxt.scratch_allocator(), sources[source_index]} +
                  "': " + os::last_system_error_message());
          status = 1;
          continue;
        }
        fd = *opened_fd;
        was_opened = true;
      }

      let const file_size =
          was_opened ? os::regular_descriptor_file_size(fd) : Maybe<u64>{};
      let const text =
          file_size.has_value()
              ? read_regular_all_but_last(fd, *file_size, count,
                                          cxt.scratch_allocator(), unit)
              : read_all_but_last(fd, count, cxt.scratch_allocator(), unit);
      let const read_error = os::get_last_system_error_number();
      if (was_opened) os::close_fd(fd);
      if (os::INTERRUPT_REQUESTED) return 130;
      if (!text.has_value()) {
        os::set_last_system_error(read_error);
        report_soft_koshkit_util_error(
            ec, cxt, args[0].view(),
            "cannot read '" +
                String{cxt.scratch_allocator(), sources[source_index]} +
                "': " + os::last_system_error_message());
        status = 1;
        continue;
      }

      do_print_source(source_index, text->view());
    }

    return status;
  }

  usize read_byte_count = 4096;
  if (count == 0) {
    read_byte_count = 0;
  } else if (is_byte_mode && count < read_byte_count) {
    read_byte_count = static_cast<usize>(count);
  }

  let source_results = ArrayList<source_read_result>{cxt.scratch_allocator()};
  let line_counts = ArrayList<u64>{cxt.scratch_allocator()};
  let open_error_flags = ArrayList<u8>{cxt.scratch_allocator()};
  source_results.reserve(sources.count());
  line_counts.reserve(sources.count());
  open_error_flags.reserve(sources.count());
  for (usize source_index = 0; source_index < sources.count(); source_index++) {
    source_results.push({None, 0, false});
    line_counts.push(0);
    open_error_flags.push(0);
  }

  let reader =
      SourceBatchReader{ec, sources, cxt.scratch_allocator(), read_byte_count};
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
        open_error_flags[chunk.source_index] = chunk.was_open_error ? 1 : 0;
        continue;
      }
      if (!result.content.has_value())
        result.content = String{heap_allocator()};

      usize append_count = chunk.content.length;
      if (is_byte_mode) {
        let const accumulated_count =
            static_cast<u64>(result.content->length());
        let const remaining_count = count - accumulated_count;
        if (remaining_count < append_count)
          append_count = static_cast<usize>(remaining_count);
      } else {
        for (usize byte_index = 0; byte_index < chunk.content.length;
             byte_index++)
        {
          if (chunk.content[byte_index] != '\n') continue;

          line_counts[chunk.source_index]++;
          if (line_counts[chunk.source_index] == count) {
            append_count = byte_index + 1;
            break;
          }
        }
      }
      result.content->append(
          chunk.content.substring_of_length(0, append_count));

      let const has_reached_limit =
          is_byte_mode ? static_cast<u64>(result.content->length()) == count
                       : line_counts[chunk.source_index] == count;
      if (has_reached_limit && !result.is_complete) {
        reader.finish_source(chunk.source_index);
        result.is_complete = true;
      } else if (is_byte_mode && !result.is_complete) {
        let const remaining_count =
            count - static_cast<u64>(result.content->length());
        reader.set_source_read_byte_count(
            chunk.source_index, remaining_count < read_byte_count
                                    ? static_cast<usize>(remaining_count)
                                    : read_byte_count);
      }
    }

    while (next_source_index < source_results.count() &&
           source_results[next_source_index].is_complete)
    {
      let &result = source_results[next_source_index];
      if (!result.content.has_value()) {
        os::set_last_system_error(result.error_number);
        report_soft_koshkit_util_error(
            ec, cxt, args[0].view(),
            String{open_error_flags[next_source_index] != 0 ? "cannot open '"
                                                            : "cannot read '"} +
                sources[next_source_index] +
                "': " + os::last_system_error_message());
        status = 1;
      } else {
        do_print_source(next_source_index, result.content->view());
        result.content.reset();
      }
      next_source_index++;
    }

    if (is_reader_complete) break;
  }

  return status;
}

} /* namespace koshkit */

} /* namespace koshka */
