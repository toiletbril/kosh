/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements the cmp utility. It compares buffered byte streams and
 * reports the first mismatch, every mismatch in octal, or only the result
 * status.
 */

#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-l|-s] file1 file2");

HELP_DESCRIPTION_DECL("The cmp utility compares two files byte by byte.");

FLAG(CMP_LIST, Bool, 'l', "verbose", "List every differing byte.");
FLAG(CMP_SILENT, Bool, 's', "silent", "Write no output.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Cmp);

namespace koshka::koshkit {

enum class byte_read_result : u8
{
  Byte,
  End,
  Error,
};

struct buffered_byte_reader
{
  os::descriptor descriptor;
  usize position{0};
  usize length{0};
  char buffer[65536];

  hot fn next(u8 &byte) wontthrow -> byte_read_result
  {
    if (position == length) {
      let const read_count = os::read_fd(descriptor, buffer, sizeof(buffer));
      if (!read_count.has_value()) return byte_read_result::Error;
      if (*read_count == 0) return byte_read_result::End;
      position = 0;
      length = *read_count;
    }

    byte = static_cast<u8>(buffer[position++]);
    return byte_read_result::Byte;
  }
};

static fn append_padded_number(String &output, u64 value, usize width,
                               int_base base) throws -> void
{
  let const digits =
      String::from_in_base(value, false, base, output.allocator());

  for (usize position = digits.length(); position < width; position++)
    output += ' ';

  output += digits.view();
}

Cmp::Cmp() = default;

pure fn Cmp::kind() const wontthrow -> Utility::Kind { return Kind::Cmp; }

fn Cmp::execute(const ExecContext &ec, EvalContext &cxt,
                const ArrayList<String> &args,
                const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let const operands = parse_util_operands(FLAG_LIST, args, &arg_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  if (operands.count() != 2 ||
      (FLAG_CMP_LIST.is_enabled() && FLAG_CMP_SILENT.is_enabled()))
    return report_usage_error(ec, cxt, args[0].view());

  let const left_input = open_named_or_stdin(ec, operands[0].view());
  if (!left_input.has_value()) {
    if (!FLAG_CMP_SILENT.is_enabled())
      report_soft_koshkit_error(ec, cxt,
                                "cmp: cannot read '" + operands[0] +
                                    "': " + os::last_system_error_message());
    return 2;
  }
  defer
  {
    if (left_input->should_close) os::close_fd(left_input->descriptor);
  };

  let const right_input = open_named_or_stdin(ec, operands[1].view());
  if (!right_input.has_value()) {
    if (!FLAG_CMP_SILENT.is_enabled())
      report_soft_koshkit_error(ec, cxt,
                                "cmp: cannot read '" + operands[1] +
                                    "': " + os::last_system_error_message());
    return 2;
  }
  defer
  {
    if (right_input->should_close) os::close_fd(right_input->descriptor);
  };

  buffered_byte_reader left{left_input->descriptor, 0, 0, {}};
  buffered_byte_reader right{right_input->descriptor, 0, 0, {}};
  u64 byte_position = 1;
  u64 line_number = 1;
  bool has_difference = false;
  let output = String{cxt.scratch_allocator()};

  loop
  {
    u8 left_byte = 0;
    u8 right_byte = 0;
    let const left_result = left.next(left_byte);
    let const right_result = right.next(right_byte);

    if (left_result == byte_read_result::Error ||
        right_result == byte_read_result::Error)
    {
      if (os::INTERRUPT_REQUESTED) return 130;
      if (!FLAG_CMP_SILENT.is_enabled())
        report_soft_koshkit_error(ec, cxt,
                                  "cmp: " + os::last_system_error_message());
      return 2;
    }

    if (left_result == byte_read_result::End ||
        right_result == byte_read_result::End)
    {
      if (left_result == right_result) break;
      has_difference = true;
      if (!FLAG_CMP_SILENT.is_enabled())
        report_soft_koshkit_error(ec, cxt,
                                  "cmp: end of file on '" +
                                      (left_result == byte_read_result::End
                                           ? operands[0]
                                           : operands[1]) +
                                      "'");
      break;
    }

    if (left_byte != right_byte) {
      has_difference = true;
      if (FLAG_CMP_SILENT.is_enabled()) return 1;

      if (!FLAG_CMP_LIST.is_enabled()) {
        ec.print_to_stdout(
            operands[0] + " " + operands[1] + " differ: byte " +
            String::from(byte_position, cxt.scratch_allocator()) + ", line " +
            String::from(line_number, cxt.scratch_allocator()) + "\n");
        return 1;
      }

      append_padded_number(output, byte_position, 6, int_base::decimal);
      output += ' ';
      append_padded_number(output, left_byte, 3, int_base::octal);
      output += ' ';
      append_padded_number(output, right_byte, 3, int_base::octal);
      output += '\n';
      if (output.length() >= 65536) {
        ec.print_to_stdout(output);
        output.clear();
      }
    }

    if (left_byte == '\n') line_number++;
    byte_position++;
  }

  ec.print_to_stdout(output);
  return has_difference ? 1 : 0;
}

} // namespace koshka::koshkit
