#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Platform.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[file]");

HELP_DESCRIPTION_DECL(
    "The uudecode utility restores data produced by uuencode.");

FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Uudecode);

namespace koshka::koshkit {

static pure fn decoded_historical_value(char byte) wontthrow -> u8
{
  return static_cast<u8>((static_cast<u8>(byte) - 32u) & 63u);
}

static fn decode_historical_line(StringView line, String &output) throws -> bool
{
  if (line.is_empty()) return false;
  let const byte_count = decoded_historical_value(line[0]);
  if (byte_count == 0) return true;
  let const encoded_count = ((static_cast<usize>(byte_count) + 2) / 3) * 4;
  if (line.length < encoded_count + 1)
    throw Error{"uudecode: truncated encoded line"};
  usize written_count = 0;
  for (usize position = 1; written_count < byte_count; position += 4) {
    let const first = decoded_historical_value(line[position]);
    let const second = decoded_historical_value(line[position + 1]);
    let const third = decoded_historical_value(line[position + 2]);
    let const fourth = decoded_historical_value(line[position + 3]);
    output += static_cast<char>((first << 2u) | (second >> 4u));
    written_count++;
    if (written_count < byte_count) {
      output += static_cast<char>((second << 4u) | (third >> 2u));
      written_count++;
    }
    if (written_count < byte_count) {
      output += static_cast<char>((third << 6u) | fourth);
      written_count++;
    }
  }
  return false;
}

static pure fn base64_value(char byte) wontthrow -> i32
{
  if (byte >= 'A' && byte <= 'Z') return byte - 'A';
  if (byte >= 'a' && byte <= 'z') return byte - 'a' + 26;
  if (byte >= '0' && byte <= '9') return byte - '0' + 52;
  if (byte == '+') return 62;
  if (byte == '/') return 63;
  return -1;
}

static fn decode_base64_line(StringView line, String &output) throws -> bool
{
  if (line == "====") return true;
  if ((line.length & 3u) != 0)
    throw Error{"uudecode: invalid base64 line length"};
  for (usize position = 0; position < line.length; position += 4) {
    let const first = base64_value(line[position]);
    let const second = base64_value(line[position + 1]);
    if (first < 0 || second < 0) throw Error{"uudecode: invalid base64 data"};
    let const third =
        line[position + 2] == '=' ? -2 : base64_value(line[position + 2]);
    let const fourth =
        line[position + 3] == '=' ? -2 : base64_value(line[position + 3]);
    if (third == -1 || fourth == -1 || (third == -2 && fourth != -2))
      throw Error{"uudecode: invalid base64 data"};
    output += static_cast<char>((first << 2u) | (second >> 4u));
    if (third >= 0) {
      output += static_cast<char>((second << 4u) | (third >> 2u));
      if (fourth >= 0) output += static_cast<char>((third << 6u) | fourth);
    }
  }
  return false;
}

static fn write_decoded_file(StringView path, StringView content,
                             u32 mode) throws -> bool
{
  let const descriptor =
      os::open_file_descriptor(path, os::file_open_mode::Truncate);
  if (!descriptor.has_value()) return false;
  defer { os::close_fd(*descriptor); };
  usize written_count = 0;
  while (written_count < content.length) {
    let const count = os::write_fd(*descriptor, content.data + written_count,
                                   content.length - written_count);
    if (!count.has_value() || *count == 0) return false;
    written_count += *count;
  }
  return os::set_file_mode(path, mode);
}

Uudecode::Uudecode() = default;

pure fn Uudecode::kind() const wontthrow -> Utility::Kind
{
  return Kind::Uudecode;
}

fn Uudecode::execute(
    const ExecContext &ec, EvalContext &cxt, const ArrayList<String> &args,
    const ArrayList<SourceLocation> &arg_locations) const throws -> i32
{
  let const operands = parse_util_operands(FLAG_LIST, args, &arg_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  if (operands.count() > 1) return report_usage_error(ec, cxt, args[0].view());
  let const source = operands.is_empty() ? StringView{"-"} : operands[0].view();
  let const input = read_named_or_stdin(ec, source);
  if (!input.has_value()) {
    report_soft_koshkit_error(ec, cxt,
                              "uudecode: cannot read '" + String{source} +
                                  "': " + os::last_system_error_message());
    return 1;
  }

  usize position = 0;
  StringView header;
  bool is_base64 = false;
  while (position < input->length()) {
    let const remaining = input->view().substring(position);
    let const end =
        position + remaining.find_character('\n').value_or(remaining.length);
    let const line =
        input->view().substring_of_length(position, end - position);
    if (line.starts_with("begin ") || line.starts_with("begin-base64 ")) {
      header = line;
      is_base64 = line.starts_with("begin-base64 ");
      position = end < input->length() ? end + 1 : end;
      break;
    }
    position = end < input->length() ? end + 1 : end;
  }
  if (header.is_empty()) throw Error{"uudecode: no encoded header found"};

  let const prefix_length = is_base64 ? usize{13} : usize{6};
  if (header.length < prefix_length + 5)
    throw Error{"uudecode: invalid encoded header"};
  u32 mode = 0;
  for (usize index = 0; index < 3; index++) {
    let const digit = header[prefix_length + index];
    if (digit < '0' || digit > '7')
      throw Error{"uudecode: invalid output mode"};
    mode = (mode << 3u) | static_cast<u32>(digit - '0');
  }
  if (header[prefix_length + 3] != ' ')
    throw Error{"uudecode: missing output path"};
  let const output_path = header.substring(prefix_length + 4);
  if (output_path.is_empty()) throw Error{"uudecode: missing output path"};

  let decoded = String{cxt.scratch_allocator()};
  bool did_finish = false;
  while (position < input->length()) {
    let const remaining = input->view().substring(position);
    let const end =
        position + remaining.find_character('\n').value_or(remaining.length);
    let const line =
        input->view().substring_of_length(position, end - position);
    did_finish = is_base64 ? decode_base64_line(line, decoded)
                           : decode_historical_line(line, decoded);
    position = end < input->length() ? end + 1 : end;
    if (did_finish) break;
  }
  if (!did_finish) throw Error{"uudecode: encoded data is unfinished"};
  if (!is_base64) {
    let const remaining = input->view().substring(position);
    let const end =
        position + remaining.find_character('\n').value_or(remaining.length);
    if (input->view().substring_of_length(position, end - position) != "end")
      throw Error{"uudecode: missing end line"};
  }

  if (!write_decoded_file(output_path, decoded.view(), mode)) {
    report_soft_koshkit_error(ec, cxt,
                              "uudecode: cannot write '" + String{output_path} +
                                  "': " + os::last_system_error_message());
    return 1;
  }
  return 0;
}

} // namespace koshka::koshkit
