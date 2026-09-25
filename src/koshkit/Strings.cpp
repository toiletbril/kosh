/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements the strings utility. It streams file bytes, collects
 * printable runs, enforces a minimum length, and optionally renders their
 * offsets in a selected radix.
 */

#include "../CLI.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-a] [-n number] [-t format] [file ...]");

HELP_DESCRIPTION_DECL("The strings utility writes printable byte sequences.");

FLAG(STRINGS_ALL, Bool, 'a', "all", "Scan the complete file.");
FLAG(STRINGS_MINIMUM, String, 'n', "bytes", "Use this minimum length.");
FLAG(STRINGS_RADIX, String, 't', "radix", "Print offsets in d, o, or x.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Strings);

namespace koshka::koshkit {

static pure fn is_strings_printable(u8 byte) wontthrow -> bool
{
  return (byte >= 0x20 && byte <= 0x7e) || byte == '\t';
}

static fn append_strings_record(String &output, StringView text, u64 offset,
                                char radix) throws -> void
{
  if (radix != '\0') {
    int_base base = int_base::decimal;
    switch (radix) {
    case 'o': base = int_base::octal; break;
    case 'x': base = int_base::hex; break;
    default: break;
    }

    let const digits =
        String::from_in_base(offset, false, base, output.allocator());
    if (digits.length() < 7) output.append_repeated(' ', 7 - digits.length());

    output += digits.view();
    output += ' ';
  }

  output += text;
  output += '\n';
}

Strings::Strings() = default;

pure fn Strings::kind() const wontthrow -> Utility::Kind
{
  return Kind::Strings;
}

fn Strings::execute(const ExecContext &ec, EvalContext &cxt,
                    const ArrayList<String> &args,
                    const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let const operands = PARSE_KOSHKIT_ARGS(args, arg_locations);

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  u64 minimum_length = 4;
  if (FLAG_STRINGS_MINIMUM.is_set()) {
    let const parsed = utils::parse_decimal_u64(FLAG_STRINGS_MINIMUM.value());
    if (parsed.is_error() || parsed.value() == 0 || parsed.value() > SIZE_MAX) {
      KOSHKIT_REPORT_ERROR_AT(
          FLAG_STRINGS_MINIMUM.value_location(), "invalid minimum length",
          "use a positive decimal integer within the platform size limit");
      return 1;
    }

    minimum_length = parsed.value();
  }
  char radix = '\0';
  if (FLAG_STRINGS_RADIX.is_set()) {
    let const radix_value = FLAG_STRINGS_RADIX.value();
    let is_valid_radix = false;

    if (radix_value.length == 1) {
      switch (radix_value[0]) {
      case 'd':
      case 'o':
      case 'x': is_valid_radix = true; break;

      default: break;
      }
    }

    if (!is_valid_radix) {
      KOSHKIT_REPORT_ERROR_AT(
          FLAG_STRINGS_RADIX.value_location(), "invalid offset radix",
          "use d for decimal, o for octal, or x for hexadecimal");
      return 1;
    }

    radix = radix_value[0];
  }

  let const sources =
      source_list_from_operands(operands, cxt.scratch_allocator());
  let output = String{cxt.scratch_allocator()};
  let run = String{cxt.scratch_allocator()};
  let reader = SourceBatchReader{ec, sources, cxt.scratch_allocator()};
  let chunks = ArrayList<SourceBatchReader::Chunk>{cxt.scratch_allocator()};
  u64 byte_offset = 0;
  u64 run_offset = 0;
  i32 status = 0;
  let const do_flush = [&]() throws -> void {
    if (run.length() >= minimum_length)
      append_strings_record(output, run.view(), run_offset, radix);
    run.clear();
  };

  loop
  {
    let const read_result = reader.read_next_ordered(chunks);
    if (read_result == SourceBatchReader::ReadResult::Complete) break;
    if (read_result == SourceBatchReader::ReadResult::Interrupted) return 130;

    for (let const &chunk : chunks) {
      usize position = 0;
      while (position < chunk.content.length) {
        if (!is_strings_printable(static_cast<u8>(chunk.content[position]))) {
          do_flush();
          position++;
          byte_offset++;
          continue;
        }

        usize run_end = position;
        while (run_end < chunk.content.length &&
               is_strings_printable(static_cast<u8>(chunk.content[run_end])))
        {
          run_end++;
        }

        if (run.is_empty()) run_offset = byte_offset;

        run.append(
            chunk.content.substring_of_length(position, run_end - position));
        byte_offset += run_end - position;
        position = run_end;
      }

      if (chunk.completion != source_completion_state::Complete) continue;

      let const source = sources[chunk.source_index];
      if (chunk.error_number != 0) {
        run.clear();
        os::set_last_system_error(chunk.error_number);
        report_soft_koshkit_util_error(
            ec, cxt, args[0].view(),
            "cannot read '" + String{cxt.scratch_allocator(), source} +
                "': " + os::last_system_error_message());
        status = 1;
      } else {
        do_flush();
      }

      byte_offset = 0;
    }
  }

  ec.print_to_stdout(output);
  return status;
}

} // namespace koshka::koshkit
