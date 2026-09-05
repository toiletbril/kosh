/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements the file utility. It parses magic databases, inspects
 * file types and contents, controls symbolic-link traversal, and formats
 * matching descriptions.
 */

#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Path.hpp"
#include "../Platform.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-dhiL] [-m file] [-M file] [file ...]");

HELP_DESCRIPTION_DECL("The file utility classifies file operands.");

FLAG(FILE_DEFAULT_TESTS, Bool, 'd', "default-tests", "Apply default tests.");
FLAG(FILE_NO_FOLLOW, Bool, 'h', "no-dereference", "Classify symbolic links.");
FLAG(FILE_REGULAR_ONLY, Bool, 'i', "regular-only",
     "Identify regular files without content tests.");
FLAG(FILE_FOLLOW, Bool, 'L', "dereference", "Follow symbolic links.");
FLAG(FILE_MAGIC, ManyStrings, 'm', "magic-file",
     "Add position-sensitive tests from this file.");
FLAG(FILE_MAGIC_ONLY, ManyStrings, 'M', "magic-only",
     "Use position-sensitive tests from this file.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(File);

namespace koshka::koshkit {

enum class file_magic_kind : u8
{
  Signed,
  Unsigned,
  String,
};

struct file_magic_rule
{
  explicit file_magic_rule(Allocator allocator)
      : expected_text(allocator), message(allocator)
  {}

  u64 offset{0};
  u64 expected_number{0};
  u64 mask{~static_cast<u64>(0)};
  usize byte_count{0};
  char comparison{'='};
  bool is_continuation{false};
  file_magic_kind kind{file_magic_kind::String};
  String expected_text;
  String message;
};

struct file_magic_path
{
  usize position;
  StringView path;
  SourceLocation location;
};

static pure fn magic_digit(char byte) wontthrow -> u8
{
  if (byte >= '0' && byte <= '9') {
    return static_cast<u8>(byte - '0');
  }
  if (byte >= 'a' && byte <= 'f') {
    return static_cast<u8>(byte - 'a' + 10);
  }
  if (byte >= 'A' && byte <= 'F') {
    return static_cast<u8>(byte - 'A' + 10);
  }
  return 0xff;
}

static pure fn parse_magic_number(StringView text, u64 &value) wontthrow -> bool
{
  if (text.is_empty()) return false;
  usize position = 0;
  bool is_negative = false;
  if (text[position] == '-') {
    is_negative = true;
    position++;
  }
  if (position == text.length) return false;

  u8 base = 10;
  if (text.length - position > 2 && text[position] == '0' &&
      (text[position + 1] == 'x' || text[position + 1] == 'X'))
  {
    base = 16;
    position += 2;
  } else if (text.length - position > 1 && text[position] == '0') {
    base = 8;
  }
  if (position == text.length) return false;

  u64 parsed = 0;
  for (; position < text.length; position++) {
    let const digit = magic_digit(text[position]);
    if (digit >= base || parsed > (UINT64_MAX - digit) / base) {
      return false;
    }
    parsed = parsed * base + digit;
  }

  value = is_negative ? 0 - parsed : parsed;
  return true;
}

static fn decode_magic_text(StringView encoded, String &decoded) throws -> void
{
  for (usize position = 0; position < encoded.length; position++) {
    let const byte = encoded[position];
    if (byte != '\\' || position + 1 == encoded.length) {
      decoded.push(byte);
      continue;
    }

    let const escaped = encoded[++position];
    switch (escaped) {
    case '\\': decoded.push('\\'); break;
    case 'a': decoded.push('\a'); break;
    case 'b': decoded.push('\b'); break;
    case 'f': decoded.push('\f'); break;
    case 'n': decoded.push('\n'); break;
    case 'r': decoded.push('\r'); break;
    case 't': decoded.push('\t'); break;
    case 'v': decoded.push('\v'); break;
    case ' ': decoded.push(' '); break;
    case 'x': {
      u8 value = 0;
      usize digit_count = 0;
      while (position + 1 < encoded.length && digit_count < 2) {
        let const digit = magic_digit(encoded[position + 1]);
        if (digit >= 16) break;
        value = static_cast<u8>(value * 16 + digit);
        position++;
        digit_count++;
      }
      if (digit_count == 0) {
        decoded += "\\x";
      } else {
        decoded.push(static_cast<char>(value));
      }
      break;
    }
    default:
      if (escaped >= '0' && escaped <= '7') {
        u8 value = static_cast<u8>(escaped - '0');
        usize digit_count = 1;
        while (position + 1 < encoded.length && digit_count < 3 &&
               encoded[position + 1] >= '0' && encoded[position + 1] <= '7')
        {
          value = static_cast<u8>(value * 8 + encoded[++position] - '0');
          digit_count++;
        }
        decoded.push(static_cast<char>(value));
      } else {
        decoded.push('\\');
        decoded.push(escaped);
      }
      break;
    }
  }
}

static pure fn next_magic_field(StringView line, usize &position) wontthrow
    -> StringView
{
  while (position < line.length &&
         (line[position] == ' ' || line[position] == '\t'))
    position++;
  let const start = position;
  while (position < line.length && line[position] != ' ' &&
         line[position] != '\t')
    position++;
  return line.substring_of_length(start, position - start);
}

static fn parse_magic_type(StringView text, file_magic_rule &rule) throws
    -> bool
{
  usize position = 0;
  if (text.starts_with("string")) {
    rule.kind = file_magic_kind::String;
    position = 6;
  } else if (text.starts_with("byte")) {
    rule.kind = file_magic_kind::Signed;
    rule.byte_count = 1;
    position = 4;
  } else if (text.starts_with("short")) {
    rule.kind = file_magic_kind::Signed;
    rule.byte_count = 2;
    position = 5;
  } else if (text.starts_with("long")) {
    rule.kind = file_magic_kind::Signed;
    rule.byte_count = 4;
    position = 4;
  } else if (text[0] == 's') {
    rule.kind = file_magic_kind::String;
    position = 1;
  } else if (text[0] == 'd' || text[0] == 'u') {
    rule.kind =
        text[0] == 'd' ? file_magic_kind::Signed : file_magic_kind::Unsigned;
    position = 1;
    if (position == text.length || text[position] == '&') {
      rule.byte_count = 4;
    } else {
      switch (text[position]) {
      case 'C':
        rule.byte_count = 1;
        position++;
        break;
      case 'S':
        rule.byte_count = 2;
        position++;
        break;
      case 'I':
        rule.byte_count = 4;
        position++;
        break;
      case 'L':
        rule.byte_count = 8;
        position++;
        break;
      default: {
        let const size_start = position;
        while (position < text.length && text[position] >= '0' &&
               text[position] <= '9')
          position++;
        u64 byte_count = 0;
        if (size_start == position ||
            !parse_magic_number(
                text.substring_of_length(size_start, position - size_start),
                byte_count) ||
            byte_count == 0 || byte_count > 8)
          return false;
        rule.byte_count = static_cast<usize>(byte_count);
        break;
      }
      }
    }
  } else {
    return false;
  }

  if (rule.kind == file_magic_kind::String) return position == text.length;
  if (position < text.length && text[position] == '&') {
    position++;
    if (!parse_magic_number(text.substring(position), rule.mask)) return false;
    return true;
  }
  return position == text.length;
}

static fn parse_magic_rule(StringView line, Allocator allocator,
                           file_magic_rule &rule) throws -> bool
{
  line = line.trim_blanks();
  if (line.is_empty() || line[0] == '#') return false;

  usize position = 0;
  let offset = next_magic_field(line, position);
  let const type = next_magic_field(line, position);
  let value = next_magic_field(line, position);
  while (position < line.length &&
         (line[position] == ' ' || line[position] == '\t'))
    position++;
  let const message = line.substring(position);
  if (offset.is_empty() || type.is_empty() || value.is_empty() ||
      message.is_empty())
    return false;

  if (offset[0] == '>') {
    rule.is_continuation = true;
    offset = offset.substring(1);
  }
  if (offset.is_empty() || offset[0] == '-' ||
      !parse_magic_number(offset, rule.offset) || !parse_magic_type(type, rule))
    return false;

  if (rule.kind == file_magic_kind::String) {
    decode_magic_text(value, rule.expected_text);
  } else {
    if (value[0] == '=' || value[0] == '<' || value[0] == '>' ||
        value[0] == '&' || value[0] == '^')
    {
      rule.comparison = value[0];
      value = value.substring(1);
    } else if (value == "x") {
      rule.comparison = 'x';
      value = {};
    }
    if (rule.comparison != 'x' &&
        !parse_magic_number(value, rule.expected_number))
      return false;
  }

  rule.message = String{allocator, message};
  return true;
}

static fn append_magic_database(StringView path,
                                ArrayList<file_magic_rule> &rules,
                                Allocator allocator) throws -> bool
{
  let const contents = Path{path}.read_entire_file();
  if (!contents.has_value()) return false;

  usize position = 0;
  while (position < contents->count()) {
    let const line = contents->view().next_line(position);
    file_magic_rule rule{allocator};
    if (parse_magic_rule(line, allocator, rule)) rules.push(steal(rule));
  }

  return true;
}

static pure fn read_magic_number(const file_magic_rule &rule, StringView bytes,
                                 u64 &value) wontthrow -> bool
{
  if (rule.byte_count == 0 || rule.byte_count > 8 ||
      rule.offset > bytes.length ||
      rule.byte_count > bytes.length - rule.offset)
    return false;

  value =
      os::read_native_endian_bytes(bytes.data + rule.offset, rule.byte_count);
  value &= rule.mask;

  if (rule.kind == file_magic_kind::Signed && rule.byte_count < 8) {
    let const sign_bit = static_cast<u64>(1) << (rule.byte_count * 8 - 1);
    if ((value & sign_bit) != 0) value |= UINT64_MAX << (rule.byte_count * 8);
  }

  switch (rule.comparison) {
  case '=': return value == rule.expected_number;
  case '<':
    return rule.kind == file_magic_kind::Signed
               ? static_cast<i64>(value) <
                     static_cast<i64>(rule.expected_number)
               : value < rule.expected_number;
  case '>':
    return rule.kind == file_magic_kind::Signed
               ? static_cast<i64>(value) >
                     static_cast<i64>(rule.expected_number)
               : value > rule.expected_number;
  case '&': return (value & rule.expected_number) == rule.expected_number;
  case '^': return (rule.expected_number & ~value) != 0;
  case 'x': return true;
  default: return false;
  }
}

static pure fn magic_rule_matches(const file_magic_rule &rule, StringView bytes,
                                  u64 &number) wontthrow -> bool
{
  if (rule.kind != file_magic_kind::String)
    return read_magic_number(rule, bytes, number);
  if (rule.offset > bytes.length ||
      rule.expected_text.count() > bytes.length - rule.offset)
    return false;
  return __builtin_memcmp(bytes.data + rule.offset, rule.expected_text.data(),
                          rule.expected_text.count()) == 0;
}

static fn format_magic_message(const file_magic_rule &rule, u64 number,
                               Allocator allocator) throws -> String
{
  let output = String{allocator};
  let const message = rule.message.view();
  bool has_formatted_value = false;
  for (usize position = 0; position < message.length; position++) {
    if (message[position] != '%' || position + 1 == message.length) {
      output.push(message[position]);
      continue;
    }

    let const conversion = message[++position];
    if (conversion == '%') {
      output.push('%');
      continue;
    }
    if (has_formatted_value) {
      output.push('%');
      output.push(conversion);
      continue;
    }

    has_formatted_value = true;
    switch (conversion) {
    case 'd':
    case 'i':
      output += String::from(static_cast<i64>(number), allocator);
      break;
    case 'u': output += String::from(number, allocator); break;
    case 'x':
      output += String::from_in_base(number, false, int_base::hex, allocator);
      break;
    case 'X': {
      let formatted =
          String::from_in_base(number, false, int_base::hex, allocator);
      formatted.lowercase_ascii();
      for (usize index = 0; index < formatted.count(); index++)
        output.push(static_cast<char>(std::toupper(formatted[index])));
      break;
    }
    case 'o':
      output += String::from_in_base(number, false, int_base::octal, allocator);
      break;
    case 'c': output.push(static_cast<char>(number)); break;
    case 's': output += rule.expected_text.view(); break;
    default:
      output.push('%');
      output.push(conversion);
      has_formatted_value = false;
      break;
    }
  }
  return output;
}

static fn match_magic_rules(const ArrayList<file_magic_rule> &rules,
                            StringView bytes, Allocator allocator) throws
    -> Maybe<String>
{
  for (usize index = 0; index < rules.count(); index++) {
    let const &rule = rules[index];
    if (rule.is_continuation) continue;

    u64 number = 0;
    if (!magic_rule_matches(rule, bytes, number)) continue;
    let result = format_magic_message(rule, number, allocator);

    for (index++; index < rules.count() && rules[index].is_continuation;
         index++)
    {
      u64 child_number = 0;
      if (!magic_rule_matches(rules[index], bytes, child_number)) continue;
      result.push(' ');
      result += format_magic_message(rules[index], child_number, allocator);
    }

    return result;
  }
  return {};
}

static pure fn file_content_description(StringView bytes) wontthrow
    -> StringView
{
  if (bytes.is_empty()) return "empty";
  if (bytes.length >= 4 && static_cast<u8>(bytes[0]) == 0x7f &&
      bytes.substring_of_length(1, 3) == "ELF")
    return "ELF executable";
  if (bytes.length >= 2 && bytes[0] == '#' && bytes[1] == '!')
    return "script text executable";

  bool is_text = true;
  for (usize position = 0; position < bytes.length; position++) {
    let const byte = static_cast<u8>(bytes[position]);
    if (byte == 0 || (byte < 0x20 && byte != '\n' && byte != '\r' &&
                      byte != '\t' && byte != '\f' && byte != '\b'))
    {
      is_text = false;
      break;
    }
  }

  return is_text ? StringView{"text"} : StringView{"data"};
}

File::File() = default;

pure fn File::kind() const wontthrow -> Utility::Kind { return Kind::File; }

fn File::execute(const ExecContext &ec, EvalContext &cxt,
                 const ArrayList<String> &args,
                 const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let operand_locations = ArrayList<SourceLocation>{cxt.scratch_allocator()};
  let const operands =
      parse_util_operands(FLAG_LIST, args, &arg_locations, &operand_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  if (operands.is_empty()) return report_usage_error(ec, cxt, args[0].view());
  let const allocator = cxt.scratch_allocator();
  i32 status = 0;
  ArrayList<file_magic_path> magic_paths{allocator};
  for (usize index = 0; index < FLAG_FILE_MAGIC.count(); index++)
    magic_paths.push(file_magic_path{FLAG_FILE_MAGIC.get_position(index),
                                     FLAG_FILE_MAGIC.get(index),
                                     FLAG_FILE_MAGIC.get_location(index)});
  for (usize index = 0; index < FLAG_FILE_MAGIC_ONLY.count(); index++)
    magic_paths.push(file_magic_path{FLAG_FILE_MAGIC_ONLY.get_position(index),
                                     FLAG_FILE_MAGIC_ONLY.get(index),
                                     FLAG_FILE_MAGIC_ONLY.get_location(index)});
  for (usize index = 1; index < magic_paths.count(); index++) {
    let current = magic_paths[index];
    usize destination = index;
    while (destination > 0 &&
           magic_paths[destination - 1].position > current.position)
    {
      magic_paths[destination] = magic_paths[destination - 1];
      destination--;
    }
    magic_paths[destination] = current;
  }

  ArrayList<file_magic_rule> magic_rules{allocator};
  for (let const &magic_path : magic_paths) {
    if (!append_magic_database(magic_path.path, magic_rules, allocator)) {
      report_soft_koshkit_util_error(
          ec, cxt, magic_path.location, args[0].view(),
          "cannot open '" + String{allocator, magic_path.path} +
              "': " + os::last_system_error_message());
      status = 1;
    }
  }

  let const should_apply_default_tests =
      FLAG_FILE_DEFAULT_TESTS.is_enabled() || FLAG_FILE_MAGIC_ONLY.is_empty();
  let const should_follow =
      !FLAG_FILE_NO_FOLLOW.is_enabled() ||
      FLAG_FILE_FOLLOW.position() > FLAG_FILE_NO_FOLLOW.position();

  for (usize operand_position = 0; operand_position < operands.count();
       operand_position++)
  {
    let const &operand = operands[operand_position];
    os::file_status file_status{};
    if (!os::stat_path(operand.view(), file_status)) {
      report_soft_koshkit_util_error(
          ec, cxt, operand_locations[operand_position], args[0].view(),
          "cannot open '" + operand + "': " + os::last_system_error_message());
      status = 1;
      continue;
    }

    let const is_symbolic_link = os::file_type_letter(file_status.mode) == 'l';
    if (is_symbolic_link) {
      let const target = os::read_symlink(operand.view(), allocator);
      if (!should_follow ||
          !os::stat_path_following(operand.view(), file_status))
      {
        let description = operand + ": symbolic link";
        if (target.has_value()) description += " to " + *target;
        description += '\n';
        ec.print_to_stdout(description);
        continue;
      }
    }

    let description = String{allocator};
    switch (os::file_type_letter(file_status.mode)) {
    case 'd': description += "directory"; break;
    case 'l': description += "symbolic link"; break;
    case 'p': description += "fifo"; break;
    case 's': description += "socket"; break;
    case 'b': description += "block special file"; break;
    case 'c': description += "character special file"; break;
    default: {
      if (FLAG_FILE_REGULAR_ONLY.is_enabled()) {
        description += "regular file";
        break;
      }

      let const descriptor =
          os::open_file_descriptor(operand.view(), os::file_open_mode::Read);
      if (!descriptor.has_value()) {
        report_soft_koshkit_util_error(
            ec, cxt, operand_locations[operand_position], args[0].view(),
            "cannot open '" + operand +
                "': " + os::last_system_error_message());
        status = 1;
        continue;
      }
      defer { os::close_fd(*descriptor); };

      if (!magic_rules.is_empty()) {
        let const contents = os::read_fd_to_string(*descriptor, allocator);
        if (!contents.has_value()) {
          report_soft_koshkit_util_error(
              ec, cxt, operand_locations[operand_position], args[0].view(),
              "cannot read '" + operand +
                  "': " + os::last_system_error_message());
          status = 1;
          continue;
        }
        if (contents->is_empty()) {
          description += "empty";
          break;
        }
        let const magic_description =
            match_magic_rules(magic_rules, contents->view(), allocator);
        if (magic_description.has_value()) {
          description += magic_description->view();
        } else if (should_apply_default_tests) {
          description += file_content_description(contents->view());
        } else {
          description += "data";
        }
        break;
      }

      char buffer[8192];
      let const read_count = os::read_fd(*descriptor, buffer, sizeof(buffer));
      if (!read_count.has_value()) {
        report_soft_koshkit_util_error(
            ec, cxt, operand_locations[operand_position], args[0].view(),
            "cannot read '" + operand +
                "': " + os::last_system_error_message());
        status = 1;
        continue;
      }
      description += file_content_description(StringView{buffer, *read_count});
      break;
    }
    }

    ec.print_to_stdout(operand + ": " + description + "\n");
  }

  return status;
}

} // namespace koshka::koshkit
