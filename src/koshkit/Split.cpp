/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements the split utility. It divides input by byte or line
 * count and generates bounded alphabetic output suffixes of the requested
 * width.
 */

#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-a suffix-length] [-b byte-count | -l line-count] "
                   "[file [prefix]]");

HELP_DESCRIPTION_DECL("The split utility divides input into output files.");

FLAG(SPLIT_SUFFIX_LENGTH, String, 'a', "suffix-length",
     "Use this many suffix letters.");
FLAG(SPLIT_BYTES, String, 'b', "bytes", "Write this many bytes per file.");
FLAG(SPLIT_LINES, String, 'l', "lines", "Write this many lines per file.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Split);

namespace koshka::koshkit {

static fn parse_split_count(StringView text, StringView name) throws -> u64
{
  let const parsed = utils::parse_decimal_u64(text);
  if (parsed.is_error() || parsed.value() == 0)
    throw Error{"split: invalid " + String{name} + " '" + String{text} + "'"};
  return parsed.value();
}

static fn split_output_name(StringView prefix, usize suffix_length, u64 index,
                            Allocator allocator) throws -> String
{
  if (suffix_length > 64) throw Error{"split: suffix length is too large"};

  char suffix[64];
  for (usize position = 0; position < suffix_length; position++)
    suffix[position] = 'a';
  for (usize position = suffix_length; position > 0 && index != 0; position--) {
    suffix[position - 1] = static_cast<char>('a' + index % 26);
    index /= 26;
  }
  if (index != 0) throw Error{"split: output file suffixes are exhausted"};

  String name{allocator, prefix};
  name += StringView{suffix, suffix_length};
  return name;
}

Split::Split() = default;

pure fn Split::kind() const wontthrow -> Utility::Kind { return Kind::Split; }

fn Split::execute(const ExecContext &ec, EvalContext &cxt,
                  const ArrayList<String> &args,
                  const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let const operands = parse_util_operands(FLAG_LIST, args, &arg_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  if (operands.count() > 2 ||
      (FLAG_SPLIT_BYTES.is_set() && FLAG_SPLIT_LINES.is_set()))
    return report_usage_error(ec, cxt, args[0].view());

  let const source = operands.is_empty() ? StringView{"-"} : operands[0].view();
  let const prefix =
      operands.count() < 2 ? StringView{"x"} : operands[1].view();
  let const suffix_length =
      FLAG_SPLIT_SUFFIX_LENGTH.is_set()
          ? parse_split_count(FLAG_SPLIT_SUFFIX_LENGTH.value(), "suffix length")
          : 2;
  if (suffix_length > SIZE_MAX)
    throw Error{"split: suffix length is too large"};

  let const input = open_named_or_stdin(ec, source);
  if (!input.has_value()) {
    report_soft_koshkit_error(ec, cxt,
                              "split: cannot read '" +
                                  String{cxt.scratch_allocator(), source} +
                                  "': " + os::last_system_error_message());
    return 1;
  }
  defer
  {
    if (input->should_close) os::close_fd(input->descriptor);
  };

  let const is_byte_mode = FLAG_SPLIT_BYTES.is_set();
  let const unit_limit =
      is_byte_mode ? parse_split_count(FLAG_SPLIT_BYTES.value(), "byte count")
      : FLAG_SPLIT_LINES.is_set()
          ? parse_split_count(FLAG_SPLIT_LINES.value(), "line count")
          : 1000;
  os::descriptor output_descriptor = KOSH_INVALID_FD;
  defer
  {
    if (output_descriptor != KOSH_INVALID_FD) os::close_fd(output_descriptor);
  };
  u64 output_index = 0;
  u64 output_unit_count = 0;

  let const do_open_output = [&]() throws -> bool {
    if (output_descriptor != KOSH_INVALID_FD) return true;
    let const name =
        split_output_name(prefix, static_cast<usize>(suffix_length),
                          output_index++, cxt.scratch_allocator());
    let const descriptor =
        os::open_file_descriptor(name.view(), os::file_open_mode::Truncate);
    if (!descriptor.has_value()) {
      report_soft_koshkit_error(ec, cxt,
                                "split: cannot create '" + name +
                                    "': " + os::last_system_error_message());
      return false;
    }
    output_descriptor = *descriptor;
    output_unit_count = 0;
    return true;
  };
  let const do_rotate_output = [&]() wontthrow -> void {
    if (output_descriptor != KOSH_INVALID_FD) os::close_fd(output_descriptor);
    output_descriptor = KOSH_INVALID_FD;
  };

  char buffer[65536];
  loop
  {
    let const read_count =
        os::read_fd(input->descriptor, buffer, sizeof(buffer));
    if (!read_count.has_value()) {
      if (os::INTERRUPT_REQUESTED) return 130;
      report_soft_koshkit_error(ec, cxt,
                                "split: " + os::last_system_error_message());
      return 1;
    }
    if (*read_count == 0) break;

    if (is_byte_mode) {
      usize position = 0;
      while (position < *read_count) {
        if (!do_open_output()) return 1;
        let const remaining = unit_limit - output_unit_count;
        let const write_count = remaining < *read_count - position
                                    ? static_cast<usize>(remaining)
                                    : *read_count - position;
        if (!os::write_all(output_descriptor, buffer + position, write_count)) {
          report_soft_koshkit_error(
              ec, cxt, "split: " + os::last_system_error_message());
          return 1;
        }
        position += write_count;
        output_unit_count += write_count;
        if (output_unit_count == unit_limit) do_rotate_output();
      }
    } else {
      usize segment_start = 0;
      for (usize position = 0; position < *read_count; position++) {
        if (buffer[position] != '\n') continue;
        if (!do_open_output()) return 1;
        if (!os::write_all(output_descriptor, buffer + segment_start,
                           position + 1 - segment_start))
        {
          report_soft_koshkit_error(
              ec, cxt, "split: " + os::last_system_error_message());
          return 1;
        }
        segment_start = position + 1;
        output_unit_count++;
        if (output_unit_count == unit_limit) do_rotate_output();
      }
      if (segment_start < *read_count) {
        if (!do_open_output()) return 1;
        if (!os::write_all(output_descriptor, buffer + segment_start,
                           *read_count - segment_start))
        {
          report_soft_koshkit_error(
              ec, cxt, "split: " + os::last_system_error_message());
          return 1;
        }
      }
    }
  }

  return 0;
}

} // namespace koshka::koshkit
