/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements the cksum utility. It streams each input through the
 * POSIX CRC algorithm and reports the complemented checksum and byte count.
 */

#include "../CLI.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[file ...]");

HELP_DESCRIPTION_DECL("The cksum utility writes a CRC and byte count.");

FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Cksum);

namespace koshka::koshkit {

struct checksum_table
{
  u32 values[256];
};

static consteval fn make_checksum_table() -> checksum_table
{
  checksum_table table{};

  for (u32 byte = 0; byte < 256; byte++) {
    u32 value = byte << 24;

    for (u32 bit = 0; bit < 8; bit++)
      value =
          (value & 0x80000000u) != 0 ? (value << 1) ^ 0x04c11db7u : value << 1;

    table.values[byte] = value;
  }

  return table;
}

inline constexpr checksum_table CHECKSUM_TABLE = make_checksum_table();

static t__forceinline fn update_checksum(u32 checksum, const char *bytes,
                                         usize byte_count) wontthrow -> u32
{
  for (usize position = 0; position < byte_count; position++) {
    let const byte = static_cast<u8>(bytes[position]);
    checksum = (checksum << 8) ^ CHECKSUM_TABLE.values[(checksum >> 24) ^ byte];
  }

  return checksum;
}

struct checksum_source_state
{
  u32 checksum{0};
  u64 byte_count{0};
  i32 error_number{0};
  bool is_complete{false};
};

static fn finish_checksum(u32 checksum, u64 byte_count) wontthrow -> u32
{
  for (u64 length = byte_count; length != 0; length >>= 8) {
    let const byte = static_cast<u8>(length);
    checksum = (checksum << 8) ^ CHECKSUM_TABLE.values[(checksum >> 24) ^ byte];
  }

  return ~checksum;
}

Cksum::Cksum() = default;

pure fn Cksum::kind() const wontthrow -> Utility::Kind { return Kind::Cksum; }

fn Cksum::execute(const ExecContext &ec, EvalContext &cxt,
                  const ArrayList<String> &args,
                  const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let const[operands, operand_locations] = parse_util_operands(
      FLAG_LIST, args, cxt.scratch_allocator(), &arg_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  let const sources =
      source_list_from_operands(operands, cxt.scratch_allocator());
  let source_states = ArrayList<checksum_source_state>{cxt.scratch_allocator()};
  source_states.reserve(sources.count());
  for (usize source_index = 0; source_index < sources.count(); source_index++)
    source_states.push({});

  let reader = SourceBatchReader{ec, sources, cxt.scratch_allocator()};
  let chunks = ArrayList<SourceBatchReader::Chunk>{cxt.scratch_allocator()};
  usize next_output_index = 0;
  i32 status = 0;
  loop
  {
    let const read_result = reader.read_next(chunks);
    if (read_result == SourceBatchReader::ReadResult::Interrupted) return 130;

    for (let const &chunk : chunks) {
      let &state = source_states[chunk.source_index];
      if (chunk.error_number != 0) {
        state.error_number = chunk.error_number;
        state.is_complete = true;
        continue;
      }

      state.checksum = update_checksum(state.checksum, chunk.content.data,
                                       chunk.content.length);
      state.byte_count += chunk.content.length;
      state.is_complete = chunk.is_complete;
    }

    while (next_output_index < sources.count() &&
           source_states[next_output_index].is_complete)
    {
      let const &state = source_states[next_output_index];
      let const source = sources[next_output_index];
      if (state.error_number != 0) {
        os::set_last_system_error(state.error_number);
        report_soft_koshkit_util_error(
            ec, cxt, args[0].view(),
            "cannot read '" + String{cxt.scratch_allocator(), source} +
                "': " + os::last_system_error_message());
        status = 1;
      } else {
        let output =
            String::from(finish_checksum(state.checksum, state.byte_count),
                         cxt.scratch_allocator());
        output += ' ';
        output += String::from(state.byte_count, cxt.scratch_allocator());
        if (!operands.is_empty()) {
          output += ' ';
          output += source;
        }
        output += '\n';
        ec.print_to_stdout(output);
      }
      next_output_index++;
    }

    if (read_result == SourceBatchReader::ReadResult::Complete) break;
  }

  return status;
}

} // namespace koshka::koshkit
