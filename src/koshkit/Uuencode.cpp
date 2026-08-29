#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Platform.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-m] [file] decode-path");

HELP_DESCRIPTION_DECL(
    "The uuencode utility converts binary data to printable text.");

FLAG(UUENCODE_BASE64, Bool, 'm', "base64", "Use base64 encoding.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Uuencode);

namespace koshka::koshkit {

static pure fn uuencode_character(u8 value) wontthrow -> char
{
  value &= 63u;
  return value == 0 ? '`' : static_cast<char>(value + 32u);
}

static fn append_historical_encoding(String &output, StringView input) throws
    -> void
{
  for (usize offset = 0; offset < input.length; offset += 45) {
    let const count = input.length - offset < 45 ? input.length - offset : 45;
    output += uuencode_character(static_cast<u8>(count));
    for (usize position = 0; position < count; position += 3) {
      let const first = static_cast<u8>(input[offset + position]);
      let const second = position + 1 < count
                             ? static_cast<u8>(input[offset + position + 1])
                             : 0;
      let const third = position + 2 < count
                            ? static_cast<u8>(input[offset + position + 2])
                            : 0;
      output += uuencode_character(first >> 2u);
      output += uuencode_character((first << 4u) | (second >> 4u));
      output += uuencode_character((second << 2u) | (third >> 6u));
      output += uuencode_character(third);
    }
    output += '\n';
  }
  output += "`\nend\n";
}

static fn append_base64_encoding(String &output, StringView input) throws
    -> void
{
  static constexpr char ALPHABET[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  usize line_length = 0;
  for (usize position = 0; position < input.length; position += 3) {
    let const remaining = input.length - position;
    let const first = static_cast<u8>(input[position]);
    let const second = remaining > 1 ? static_cast<u8>(input[position + 1]) : 0;
    let const third = remaining > 2 ? static_cast<u8>(input[position + 2]) : 0;
    const char encoded[] = {
        ALPHABET[first >> 2u],
        ALPHABET[((first & 3u) << 4u) | (second >> 4u)],
        remaining > 1 ? ALPHABET[((second & 15u) << 2u) | (third >> 6u)] : '=',
        remaining > 2 ? ALPHABET[third & 63u] : '=',
    };
    output += StringView{encoded, sizeof(encoded)};
    line_length += sizeof(encoded);
    if (line_length == 60) {
      output += '\n';
      line_length = 0;
    }
  }
  if (line_length != 0) output += '\n';
  output += "====\n";
}

Uuencode::Uuencode() = default;

pure fn Uuencode::kind() const wontthrow -> Utility::Kind
{
  return Kind::Uuencode;
}

fn Uuencode::execute(
    const ExecContext &ec, EvalContext &cxt, const ArrayList<String> &args,
    const ArrayList<SourceLocation> &arg_locations) const throws -> i32
{
  let const operands = parse_util_operands(FLAG_LIST, args, &arg_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  if (operands.is_empty() || operands.count() > 2)
    return report_usage_error(ec, cxt, args[0].view());

  let const source =
      operands.count() == 2 ? operands[0].view() : StringView{"-"};
  let const decode_path = operands.back().view();
  let const input = read_named_or_stdin(ec, source);
  if (!input.has_value()) {
    report_soft_koshkit_error(ec, cxt,
                              "uuencode: cannot read '" + String{source} +
                                  "': " + os::last_system_error_message());
    return 1;
  }

  u32 mode = 0666u & ~os::get_file_creation_mask();
  if (source != "-") {
    os::file_status status{};
    if (os::stat_path_following(source, status)) mode = status.mode & 0777u;
  }

  let output = String{cxt.scratch_allocator()};
  output += FLAG_UUENCODE_BASE64.is_enabled() ? "begin-base64 " : "begin ";
  char mode_text[4];
  mode_text[0] = static_cast<char>('0' + ((mode >> 6u) & 7u));
  mode_text[1] = static_cast<char>('0' + ((mode >> 3u) & 7u));
  mode_text[2] = static_cast<char>('0' + (mode & 7u));
  mode_text[3] = ' ';
  output += StringView{mode_text, sizeof(mode_text)};
  output += decode_path;
  output += '\n';
  if (FLAG_UUENCODE_BASE64.is_enabled())
    append_base64_encoding(output, input->view());
  else
    append_historical_encoding(output, input->view());
  ec.print_to_stdout(output);
  return 0;
}

} // namespace koshka::koshkit
