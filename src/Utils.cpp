#include "Utils.hpp"

#include "Builtin.hpp"
#include "Cli.hpp"
#include "Containers.hpp"
#include "Debug.hpp"
#include "Errors.hpp"
#include "Eval.hpp"
#include "Koshkit.hpp"
#include "Lexer.hpp"
#include "Platform.hpp"
#include "Toiletline.hpp"
#include "Trace.hpp"

namespace koshka {

namespace utils {

static fn shell_word_expansion_end(StringView word,
                                   usize expansion_start) wontthrow -> usize
{
  if (expansion_start >= word.length) return expansion_start;

  if (word[expansion_start] == '`') {
    usize position = expansion_start + 1;
    while (position < word.length) {
      if (word[position] == '\\' && position + 1 < word.length) {
        position += 2;
        continue;
      }
      if (word[position] == '`') return position + 1;
      position++;
    }
    return word.length;
  }

  if (word[expansion_start] != '$' || expansion_start + 1 >= word.length) {
    return expansion_start;
  }

  let const next_byte = word[expansion_start + 1];
  if (next_byte == '{') {
    usize depth = 1;
    usize position = expansion_start + 2;
    while (position < word.length) {
      if (word[position] == '\\' && position + 1 < word.length) {
        position += 2;
        continue;
      }
      if (word[position] == '{') depth++;
      if (word[position] == '}' && --depth == 0) return position + 1;
      position++;
    }
    return word.length;
  }

  if (next_byte == '(') {
    usize depth = 0;
    char quote_character = 0;
    for (usize position = expansion_start + 1; position < word.length;
         position++)
    {
      let const byte = word[position];
      if (byte == '\\' && quote_character != '\'' && position + 1 < word.length)
      {
        position++;
        continue;
      }
      if ((byte == '\'' || byte == '"') && quote_character == 0) {
        quote_character = byte;
        continue;
      }
      if (byte == quote_character) {
        quote_character = 0;
        continue;
      }
      if (quote_character != 0) continue;
      if (byte == '(') depth++;
      if (byte == ')' && --depth == 0) return position + 1;
    }
    return word.length;
  }

  usize position = expansion_start + 1;
  if ((next_byte >= 'a' && next_byte <= 'z') ||
      (next_byte >= 'A' && next_byte <= 'Z') || next_byte == '_')
  {
    position++;
    while (position < word.length) {
      let const byte = word[position];
      if (!((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
            (byte >= '0' && byte <= '9') || byte == '_'))
      {
        break;
      }
      position++;
    }
    return position;
  }

  if ((next_byte >= '0' && next_byte <= '9') || next_byte == '@' ||
      next_byte == '*' || next_byte == '#' || next_byte == '?' ||
      next_byte == '-' || next_byte == '$' || next_byte == '!')
  {
    return expansion_start + 2;
  }

  return expansion_start;
}

hot fn decode_shell_word(StringView word, Allocator allocator,
                         bool should_map_source) throws -> decoded_shell_word
{
  let decoded = decoded_shell_word{allocator};
  if (should_map_source) decoded.raw_positions.push(0);

  char quote_character = 0;
  let is_scanning_tilde_prefix = false;
  let is_scanning_leading_variable = false;
  let leading_variable_is_braced = false;
  for (usize position = 0; position < word.length; position++) {
    let const byte = word[position];
    if (quote_character == 0 && (byte == '\'' || byte == '"')) {
      decoded.has_shell_syntax = true;
      if (is_scanning_tilde_prefix) {
        decoded.is_leading_tilde_active = false;
        is_scanning_tilde_prefix = false;
      }
      is_scanning_leading_variable = false;
      quote_character = byte;
      decoded.open_quote_content_start = position + 1;
      decoded.open_quote_decoded_start = decoded.text.length();
      decoded.last_quote_content_start = position + 1;
      decoded.last_quote_decoded_start = decoded.text.length();
      decoded.last_quote_character = byte;
      if (should_map_source) decoded.raw_positions.back() = position + 1;
      if (!decoded.text.is_empty() &&
          os::is_directory_separator(decoded.text.back()))
        decoded.raw_directory_end = position + 1;
      continue;
    }
    if (byte == quote_character) {
      if (is_scanning_tilde_prefix) {
        decoded.is_leading_tilde_active = false;
        is_scanning_tilde_prefix = false;
      }
      is_scanning_leading_variable = false;
      quote_character = 0;
      decoded.open_quote_content_start = 0;
      decoded.open_quote_decoded_start = 0;
      if (!decoded.text.is_empty() &&
          os::is_directory_separator(decoded.text.back()))
        decoded.raw_directory_end = position + 1;
      continue;
    }
    if (byte == '\\' && quote_character != '\'' && position + 1 < word.length) {
      let const escaped_byte = word[position + 1];
      if (quote_character != '"' || escaped_byte == '$' ||
          escaped_byte == '`' || escaped_byte == '"' || escaped_byte == '\\' ||
          escaped_byte == '\n')
      {
        decoded.has_shell_syntax = true;
        if (is_scanning_tilde_prefix) {
          decoded.is_leading_tilde_active = false;
          is_scanning_tilde_prefix = false;
        }
        is_scanning_leading_variable = false;
        position++;
        if (escaped_byte == '\n') {
          if (should_map_source) decoded.raw_positions.back() = position + 1;
          if (!decoded.text.is_empty() &&
              os::is_directory_separator(decoded.text.back()))
            decoded.raw_directory_end = position + 1;
          continue;
        }
        decoded.text.push(escaped_byte);
        decoded.glob_active.push(false);
        if (should_map_source) decoded.raw_positions.push(position + 1);
        if (os::is_directory_separator(escaped_byte))
          decoded.raw_directory_end = position + 1;
        continue;
      }
    }
    if (decoded.text.is_empty()) {
      decoded.is_leading_tilde_active = byte == '~' && quote_character == 0;
      decoded.is_leading_variable_active =
          byte == '$' && quote_character != '\'';
      is_scanning_tilde_prefix = decoded.is_leading_tilde_active;
      is_scanning_leading_variable = decoded.is_leading_variable_active;
      if (is_scanning_leading_variable)
        decoded.leading_variable_expansion_end = 1;
    } else if (is_scanning_leading_variable) {
      if (decoded.text.length() == 1 && byte == '{') {
        leading_variable_is_braced = true;
        decoded.leading_variable_expansion_end = decoded.text.length() + 1;
      } else if (lexer::is_variable_name(byte)) {
        decoded.leading_variable_expansion_end = decoded.text.length() + 1;
      } else if (leading_variable_is_braced && byte == '}') {
        decoded.leading_variable_expansion_end = decoded.text.length() + 1;
        is_scanning_leading_variable = false;
      } else {
        is_scanning_leading_variable = false;
      }
    }
    decoded.text.push(byte);
    decoded.glob_active.push(quote_character == 0 &&
                             (byte == '*' || byte == '?' || byte == '['));
    if (should_map_source) decoded.raw_positions.push(position + 1);
    if (os::is_directory_separator(byte)) {
      decoded.raw_directory_end = position + 1;
      is_scanning_tilde_prefix = false;
    }
  }
  decoded.quote_character = quote_character;

  if (!should_map_source) return decoded;

  char scan_quote = 0;
  usize decoded_position = 0;
  for (usize raw_start = 0; raw_start < word.length; raw_start++) {
    let const byte = word[raw_start];
    if (scan_quote == 0 && (byte == '\'' || byte == '"')) {
      scan_quote = byte;
      continue;
    }
    if (byte == scan_quote) {
      scan_quote = 0;
      continue;
    }
    if (byte == '\\' && scan_quote != '\'' && raw_start + 1 < word.length) {
      raw_start++;
      continue;
    }

    usize raw_end = raw_start;
    if (raw_start == 0 && byte == '~' && scan_quote == 0 &&
        decoded.is_leading_tilde_active)
    {
      raw_end = raw_start + 1;
    } else if ((byte == '$' && scan_quote != '\'') ||
               (byte == '`' && scan_quote == 0))
      raw_end = shell_word_expansion_end(word, raw_start);
    if (raw_end <= raw_start) continue;

    while (decoded_position < decoded.raw_positions.count() &&
           decoded.raw_positions[decoded_position] < raw_start)
      decoded_position++;
    let const decoded_start = decoded_position;
    while (decoded_position < decoded.raw_positions.count() &&
           decoded.raw_positions[decoded_position] < raw_end)
      decoded_position++;
    usize decoded_end = decoded_position;
    if (decoded_end > decoded.text.length())
      decoded_end = decoded.text.length();

    decoded.opaque_ranges.push(
        opaque_shell_word_range{decoded_start, decoded_end - decoded_start,
                                raw_start, raw_end - raw_start});
    raw_start = raw_end - 1;
  }

  return decoded;
}

struct path_source_component
{
  StringView text;
  usize decoded_start;
  usize decoded_end;
  bool is_opaque;
};

struct path_component_owner_range
{
  usize first_component_index;
  usize end_component_index;
};

static fn split_path_source_components(StringView path,
                                       const decoded_shell_word *decoded,
                                       Allocator allocator) throws
    -> ArrayList<path_source_component>
{
  let components = ArrayList<path_source_component>{allocator};
  usize position = os::path_root_length(path);
  if (position == 0 && os::path_is_drive_relative(path)) position = 2;
  usize opaque_range_position = 0;
  while (position < path.length) {
    let const component = Path::next_component(path, position);
    if (component.text.is_empty()) break;

    let is_opaque = false;
    if (decoded != nullptr) {
      while (opaque_range_position < decoded->opaque_ranges.count()) {
        let const &range = decoded->opaque_ranges[opaque_range_position];
        let const range_end = range.decoded_start + range.decoded_length;
        if (range_end <= component.start) {
          opaque_range_position++;
          continue;
        }
        if (range.decoded_start < component.end && range_end > component.start)
        {
          is_opaque = true;
        }
        break;
      }
    }

    components.push(path_source_component{component.text, component.start,
                                          component.end, is_opaque});
  }
  return components;
}

static fn path_component_owner(
    const ArrayList<path_source_component> &raw_components,
    const ArrayList<path_source_component> &expanded_components,
    usize expanded_component_index) wontthrow
    -> Maybe<path_component_owner_range>
{
  usize raw_left_index = 0;
  usize expanded_left_index = 0;
  while (raw_left_index < raw_components.count() &&
         expanded_left_index < expanded_components.count() &&
         !raw_components[raw_left_index].is_opaque &&
         raw_components[raw_left_index].text ==
             expanded_components[expanded_left_index].text)
  {
    if (expanded_left_index == expanded_component_index)
      return path_component_owner_range{raw_left_index, raw_left_index + 1};
    raw_left_index++;
    expanded_left_index++;
  }

  usize raw_right_index = raw_components.count();
  usize expanded_right_index = expanded_components.count();
  while (raw_right_index > raw_left_index &&
         expanded_right_index > expanded_left_index &&
         !raw_components[raw_right_index - 1].is_opaque &&
         raw_components[raw_right_index - 1].text ==
             expanded_components[expanded_right_index - 1].text)
  {
    if (expanded_right_index - 1 == expanded_component_index)
      return path_component_owner_range{raw_right_index - 1, raw_right_index};
    raw_right_index--;
    expanded_right_index--;
  }

  if (expanded_component_index >= expanded_left_index &&
      expanded_component_index < expanded_right_index &&
      raw_left_index < raw_right_index)
  {
    return path_component_owner_range{raw_left_index, raw_right_index};
  }
  return None;
}

fn locate_first_unavailable_path_component(const Path &target,
                                           StringView expanded_operand,
                                           StringView raw_operand,
                                           SourceLocation operand_location,
                                           Allocator allocator) throws
    -> Maybe<unavailable_path_source_component>
{
  let const unavailable = target.first_unavailable_component();
  if (!unavailable.has_value()) return None;

  let const decoded = decode_shell_word(raw_operand, allocator, true);
  let const raw_components =
      split_path_source_components(decoded.text.view(), &decoded, allocator);
  let const expanded_components =
      split_path_source_components(expanded_operand, nullptr, allocator);
  const usize remaining_component_count =
      unavailable->component_count - unavailable->component_index - 1;
  const usize expanded_component_index =
      expanded_components.count() > remaining_component_count
          ? expanded_components.count() - remaining_component_count - 1
          : 0;
  let const owner = path_component_owner(raw_components, expanded_components,
                                         expanded_component_index);

  let reported_prefix = String{allocator};
  if (expanded_component_index < expanded_components.count()) {
    reported_prefix.append(expanded_operand.substring_of_length(
        0, expanded_components[expanded_component_index].decoded_end));
  } else
    reported_prefix.append(expanded_operand);
  let typed_prefix = String{allocator};
  usize typed_component_start = 0;
  bool has_single_raw_component = false;
  if (owner.has_value()) {
    let const &first_component = raw_components[owner->first_component_index];
    let const &last_component = raw_components[owner->end_component_index - 1];
    typed_prefix.append(
        decoded.text.view().substring_of_length(0, last_component.decoded_end));
    operand_location.position +=
        decoded.raw_positions[first_component.decoded_start];
    operand_location.length =
        decoded.raw_positions[last_component.decoded_end] -
        decoded.raw_positions[first_component.decoded_start];
    typed_component_start = first_component.decoded_start;
    has_single_raw_component =
        owner->end_component_index == owner->first_component_index + 1;
  } else
    typed_prefix.append(expanded_operand);

  let const prefix = Path{
      target.text().view().substring_of_length(0, unavailable->component_end)};
  let const is_final_component =
      expanded_component_index + 1 >= expanded_components.count();

  return unavailable_path_source_component{prefix,
                                           operand_location,
                                           steal(reported_prefix),
                                           steal(typed_prefix),
                                           typed_component_start,
                                           unavailable->is_not_directory,
                                           has_single_raw_component,
                                           is_final_component};
}

fn file_content_identity(const Path &path, Allocator allocator) throws
    -> Maybe<String>
{
  let const file =
      os::open_file_descriptor(path.text().view(), os::file_open_mode::Read);
  if (!file.has_value()) return None;
  defer { os::close_fd(*file); };

  u32 crc = 0xFFFFFFFF;
  char buffer[65536];
  loop
  {
    let const read_count = os::read_fd(*file, buffer, sizeof(buffer));
    if (!read_count.has_value()) return None;
    if (*read_count == 0) break;
    crc = os::crc32c_update(crc, buffer, *read_count);
  }

  crc = ~crc;
  constexpr char HEX_DIGITS[] = "0123456789abcdef";
  let digest = String{allocator};
  digest.reserve(8);
  for (usize position = 0; position < 8; position++) {
    let const shift = static_cast<u32>((7 - position) * 4);
    digest.push(HEX_DIGITS[(crc >> shift) & 0xf]);
  }
  return digest;
}

namespace {

fn compute_kosh_identity(StringView fallback_path) throws -> Maybe<String>
{
  if (let const executable = os::current_executable_path();
      executable.has_value())
  {
    let const identity =
        file_content_identity(Path{executable->view()}, heap_allocator());
    if (identity.has_value()) {
      os::set_environment_variable("KOSH_IDENTITY", identity->view());
      return identity;
    }
  }
  let const identity =
      file_content_identity(Path{fallback_path}, heap_allocator());
  if (identity.has_value())
    os::set_environment_variable("KOSH_IDENTITY", identity->view());
  return identity;
}

} /* namespace */

fn kosh_identity(StringView fallback_path) throws -> Maybe<StringView>
{
  static const Maybe<String> cached = compute_kosh_identity(fallback_path);
  if (cached.has_value()) return cached->view();
  return None;
}

fn merge_tokens_to_string(const ArrayList<const Token *> &tokens) throws
    -> String
{
  let result = String{heap_allocator()};
  usize total_length = 0;
  for (usize i = 0; i < tokens.count(); i++) {
    let const token = tokens[i];
    ASSERT(token != nullptr);
    total_length += token->raw_string().count();
    if (i + 1 < tokens.count()) total_length++;
  }
  result.reserve(total_length);
  for (usize i = 0; i < tokens.count(); i++) {
    let const token = tokens[i];
    ASSERT(token != nullptr);
    result += token->raw_string();
    if (i + 1 < tokens.count()) {
      result += ' ';
    }
  }
  return result;
}

pure fn strip_sig_prefix(StringView name) wontthrow -> StringView
{
  if (name.starts_with("SIG")) return name.substring(3);
  return name;
}

pure fn decode_utf8(StringView source, usize position,
                    u32 invalid_codepoint) wontthrow -> decoded_codepoint
{
  let const first = static_cast<u8>(source[position]);
  if (first < 0x80) return {first, 1};

  usize length = 0;
  u32 value = 0;
  switch (first & 0xf8) {
  case 0xc0:
  case 0xc8:
  case 0xd0:
  case 0xd8:
    length = 2;
    value = first & 0x1fu;
    break;
  case 0xe0:
  case 0xe8:
    length = 3;
    value = first & 0x0fu;
    break;
  case 0xf0:
    length = 4;
    value = first & 0x07u;
    break;
  default: return {invalid_codepoint, 1};
  }

  if (position + length > source.length) return {invalid_codepoint, 1};

  for (usize index = 1; index < length; index++) {
    let const continuation = static_cast<u8>(source[position + index]);
    if ((continuation & 0xc0) != 0x80) return {invalid_codepoint, 1};
    value = (value << 6) | (continuation & 0x3fu);
  }

  return {value, length};
}

fn append_utf8(String &output, u32 codepoint) throws -> void
{
  if (codepoint <= 0x7f) {
    output.push(static_cast<char>(codepoint));
  } else if (codepoint <= 0x7ff) {
    output.push(static_cast<char>(0xc0 | (codepoint >> 6)));
    output.push(static_cast<char>(0x80 | (codepoint & 0x3f)));
  } else if (codepoint <= 0xffff) {
    output.push(static_cast<char>(0xe0 | (codepoint >> 12)));
    output.push(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
    output.push(static_cast<char>(0x80 | (codepoint & 0x3f)));
  } else {
    output.push(static_cast<char>(0xf0 | (codepoint >> 18)));
    output.push(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)));
    output.push(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
    output.push(static_cast<char>(0x80 | (codepoint & 0x3f)));
  }
}

fn split_lines(StringView text, Allocator allocator,
               bool should_keep_newlines) throws -> ArrayList<StringView>
{
  let lines = ArrayList<StringView>{allocator};
  usize position = 0;
  while (position < text.length) {
    let const line_start = position;
    let line = text.next_line(position);
    if (should_keep_newlines && position > line_start + line.length)
      line = text.substring_of_length(line_start, line.length + 1);
    lines.push(line);
  }
  if (!should_keep_newlines &&
      (text.is_empty() || text[text.length - 1] == '\n'))
    lines.push(text.substring(text.length));

  return lines;
}

fn format_unix_timestamp(i64 unix_time, const char *format) throws -> String
{
  return os::format_local_time(format, unix_time);
}

hot pure fn is_posix_reserved_word(StringView word) wontthrow -> bool
{
  static constexpr PackedStringKey RESERVED_WORD_KEYS[] = {
      SSK("!"),    SSK("{"),    SSK("}"),     SSK("case"),
      SSK("do"),   SSK("done"), SSK("elif"),  SSK("else"),
      SSK("esac"), SSK("fi"),   SSK("for"),   SSK("if"),
      SSK("in"),   SSK("then"), SSK("until"), SSK("while"),
  };
  static constexpr StaticStringSet RESERVED_WORDS{RESERVED_WORD_KEYS};
  return RESERVED_WORDS.contains(word);
}

} /* namespace utils */

} /* namespace koshka */
