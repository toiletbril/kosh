#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-A base] [-j skip] [-N count] [-t type]... [file ...]");

HELP_DESCRIPTION_DECL("The od utility writes formatted file bytes.");

FLAG(OD_ADDRESS, String, 'A', "address-radix", "Use d, o, x, or n addresses.");
FLAG(OD_SKIP, String, 'j', "skip-bytes", "Skip this many input bytes.");
FLAG(OD_COUNT, String, 'N', "read-bytes", "Read at most this many bytes.");
FLAG(OD_TYPE, ManyStrings, 't', "format", "Add an output type.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Od);

namespace koshka::koshkit {

static fn append_od_padded(String &output, u64 value, usize width,
                           int_base base) throws -> void
{
  let const digits =
      String::from_in_base(value, false, base, output.allocator());
  for (usize position = digits.length(); position < width; position++)
    output += '0';
  output += digits.view();
}

static fn append_od_character(String &output, u8 byte) throws -> void
{
  static constexpr StringView NAMES[8] = {"nul", "soh", "stx", "etx",
                                          "eot", "enq", "ack", "bel"};
  if (byte < 8) {
    output += NAMES[byte];
  } else if (byte == '\b') {
    output += " \\b";
  } else if (byte == '\t') {
    output += " \\t";
  } else if (byte == '\n') {
    output += " \\n";
  } else if (byte == '\f') {
    output += " \\f";
  } else if (byte == '\r') {
    output += " \\r";
  } else if (byte >= 0x20 && byte <= 0x7e) {
    output += "  ";
    output += static_cast<char>(byte);
  } else {
    append_od_padded(output, byte, 3, int_base::octal);
  }
}

static fn od_base(char radix) wontthrow -> int_base
{
  return radix == 'x'   ? int_base::hex
         : radix == 'd' ? int_base::decimal
                        : int_base::octal;
}

Od::Od() = default;

pure fn Od::kind() const wontthrow -> Utility::Kind { return Kind::Od; }

fn Od::execute(const ExecContext &ec, EvalContext &cxt,
               const ArrayList<String> &args,
               const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let const operands = parse_util_operands(FLAG_LIST, args, &arg_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  char address_radix = 'o';
  if (FLAG_OD_ADDRESS.is_set()) {
    if (FLAG_OD_ADDRESS.value().length != 1 ||
        (FLAG_OD_ADDRESS.value()[0] != 'd' &&
         FLAG_OD_ADDRESS.value()[0] != 'o' &&
         FLAG_OD_ADDRESS.value()[0] != 'x' &&
         FLAG_OD_ADDRESS.value()[0] != 'n'))
      throw Error{"od: address radix must be d, o, x, or n"};
    address_radix = FLAG_OD_ADDRESS.value()[0];
  }

  u64 skip_count = 0;
  if (FLAG_OD_SKIP.is_set()) {
    let const parsed = utils::parse_decimal_u64(FLAG_OD_SKIP.value());
    if (parsed.is_error()) throw Error{"od: invalid skip count"};
    skip_count = parsed.value();
  }
  u64 byte_limit = UINT64_MAX;
  if (FLAG_OD_COUNT.is_set()) {
    let const parsed = utils::parse_decimal_u64(FLAG_OD_COUNT.value());
    if (parsed.is_error()) throw Error{"od: invalid byte count"};
    byte_limit = parsed.value();
  }

  let input_bytes = String{cxt.scratch_allocator()};
  let const sources =
      source_list_from_operands(operands, cxt.scratch_allocator());
  i32 status = 0;
  for (let const source : sources) {
    let const content = read_named_or_stdin(ec, source);
    if (!content.has_value()) {
      report_soft_koshkit_error(ec, cxt,
                                "od: cannot read '" +
                                    String{cxt.scratch_allocator(), source} +
                                    "': " + os::last_system_error_message());
      status = 1;
      continue;
    }
    input_bytes += content->view();
  }

  let const first = skip_count < input_bytes.length()
                        ? static_cast<usize>(skip_count)
                        : input_bytes.length();
  let available = input_bytes.length() - first;
  if (byte_limit < available) available = static_cast<usize>(byte_limit);
  let const bytes = input_bytes.view().substring_of_length(first, available);
  let output = String{cxt.scratch_allocator()};
  let type_count = FLAG_OD_TYPE.count();

  for (usize row_start = 0; row_start < bytes.length; row_start += 16) {
    let const row_length =
        bytes.length - row_start < 16 ? bytes.length - row_start : 16;
    let const format_count = type_count == 0 ? 1 : type_count;

    for (usize format_index = 0; format_index < format_count; format_index++) {
      if (address_radix != 'n' && format_index == 0) {
        append_od_padded(output, first + row_start, 7, od_base(address_radix));
      } else {
        output += "       ";
      }

      let const format =
          type_count == 0 ? StringView{"o2"} : FLAG_OD_TYPE.get(format_index);
      if (format == "c") {
        for (usize position = 0; position < row_length; position++) {
          output += ' ';
          append_od_character(output,
                              static_cast<u8>(bytes[row_start + position]));
        }
      } else {
        let const radix = format.is_empty() ? 'o' : format[0];
        usize unit_size = 2;
        if (format.length > 1 && format[1] >= '1' && format[1] <= '8')
          unit_size = static_cast<usize>(format[1] - '0');
        if (radix != 'd' && radix != 'o' && radix != 'u' && radix != 'x')
          throw Error{"od: unsupported output type '" + String{format} + "'"};
        if (unit_size != 1 && unit_size != 2 && unit_size != 4 &&
            unit_size != 8)
          throw Error{"od: unsupported integer width"};

        for (usize position = 0; position < row_length; position += unit_size) {
          u64 value = 0;
          let const current_size = row_length - position < unit_size
                                       ? row_length - position
                                       : unit_size;
          for (usize byte_index = 0; byte_index < current_size; byte_index++)
            value |= static_cast<u64>(static_cast<u8>(
                         bytes[row_start + position + byte_index]))
                     << (byte_index * 8);
          output += ' ';
          let const base = radix == 'x'   ? int_base::hex
                           : radix == 'o' ? int_base::octal
                                          : int_base::decimal;
          let const width = radix == 'x'   ? unit_size * 2
                            : radix == 'o' ? (unit_size * 8 + 2) / 3
                                           : unit_size * 3;
          append_od_padded(output, value, width, base);
        }
      }
      output += '\n';
    }
  }

  if (address_radix != 'n') {
    append_od_padded(output, first + bytes.length, 7, od_base(address_radix));
    output += '\n';
  }
  ec.print_to_stdout(output);
  return status;
}

} // namespace koshka::koshkit
