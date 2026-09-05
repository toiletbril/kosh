/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements the cksum utility. It streams each input through the
 * POSIX CRC algorithm and reports the complemented checksum and byte count.
 */

#include "../Cli.hpp"
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

Cksum::Cksum() = default;

pure fn Cksum::kind() const wontthrow -> Utility::Kind { return Kind::Cksum; }

fn Cksum::execute(const ExecContext &ec, EvalContext &cxt,
                  const ArrayList<String> &args,
                  const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let const operands = parse_util_operands(FLAG_LIST, args, &arg_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  let const sources =
      source_list_from_operands(operands, cxt.scratch_allocator());
  i32 status = 0;

  for (let const source : sources) {
    let const input = open_named_or_stdin(ec, source);
    if (!input.has_value()) {
      report_soft_koshkit_error(ec, cxt,
                                "cksum: cannot read '" +
                                    String{cxt.scratch_allocator(), source} +
                                    "': " + os::last_system_error_message());
      status = 1;
      continue;
    }
    defer
    {
      if (input->should_close) os::close_fd(input->descriptor);
    };

    u32 checksum = 0;
    u64 byte_count = 0;
    bool did_read_fail = false;
    char buffer[65536];

    loop
    {
      let const read_count =
          os::read_fd(input->descriptor, buffer, sizeof(buffer));
      if (!read_count.has_value()) {
        if (os::INTERRUPT_REQUESTED) return 130;
        report_soft_koshkit_error(ec, cxt,
                                  "cksum: cannot read '" +
                                      String{cxt.scratch_allocator(), source} +
                                      "': " + os::last_system_error_message());
        status = 1;
        did_read_fail = true;
        break;
      }
      if (*read_count == 0) break;

      checksum = update_checksum(checksum, buffer, *read_count);
      byte_count += *read_count;
    }

    if (did_read_fail) continue;

    for (u64 length = byte_count; length != 0; length >>= 8) {
      let const byte = static_cast<u8>(length);
      checksum =
          (checksum << 8) ^ CHECKSUM_TABLE.values[(checksum >> 24) ^ byte];
    }
    checksum = ~checksum;

    let output = String::from(checksum, cxt.scratch_allocator());
    output += ' ';
    output += String::from(byte_count, cxt.scratch_allocator());
    if (!operands.is_empty()) {
      output += ' ';
      output += source;
    }
    output += '\n';
    ec.print_to_stdout(output);
  }

  return status;
}

} // namespace koshka::koshkit
