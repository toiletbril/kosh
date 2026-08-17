#pragma once

#include "Builtin.hpp"
#include "Completion.hpp"
#include "CompletionPolicy.hpp"
#include "Diagnostics.hpp"
#include "Expressions.hpp"
#include "Koshkit.hpp"
#include "LanguageServer.hpp"
#include "Lexer.hpp"
#include "MimicMood.hpp"
#include "Parser.hpp"
#include "Path.hpp"
#include "Platform.hpp"
#include "StaticStringMap.hpp"
#include "Utils.hpp"

namespace koshka::language_server {

namespace {

constexpr static_string_entry<mimic_mood> LANGUAGE_MOOD_ENTRIES[] = {
    {SSK("bash"),       mimic_mood::Bash     },
    {SSK("bash-posix"), mimic_mood::BashPosix},
    {SSK("dash"),       mimic_mood::Posix    },
    {SSK("kosh"),       mimic_mood::Default  },
    {SSK("posix"),      mimic_mood::Posix    },
    {SSK("rbash"),      mimic_mood::Bash     },
    {SSK("sh"),         mimic_mood::Posix    },
    {SSK("shit"),       mimic_mood::Default  },
};
constexpr StaticStringMap LANGUAGE_MOODS{LANGUAGE_MOOD_ENTRIES};

pure fn code_action_kind_includes(StringView supported,
                                  StringView offered) wontthrow -> bool
{
  if (supported == offered) return true;

  return offered.length > supported.length && offered.starts_with(supported) &&
         offered[supported.length] == '.';
}

enum class json_kind : u8
{
  Null,
  Boolean,
  Number,
  String,
  Array,
  Object,
};

class JsonValue;

struct json_member
{
  String name;
  JsonValue *value;
};

class JsonValue
{
public:
  explicit JsonValue(json_kind value_kind, Allocator allocator)
      : kind(value_kind), text(allocator), array(allocator), object(allocator)
  {}

  pure fn get(StringView name) const wontthrow -> const JsonValue *
  {
    for (let const &member : object)
      if (member.name == name) return member.value;

    return nullptr;
  }

  json_kind kind;
  bool boolean{false};
  String text;
  ArrayList<JsonValue *> array;
  ArrayList<json_member> object;
};

class JsonParser
{
public:
  explicit JsonParser(StringView source) : m_source(source) {}

  fn parse() throws -> JsonValue *
  {
    skip_blanks();
    let *value = parse_value();
    skip_blanks();
    if (value == nullptr || m_position != m_source.length) return nullptr;

    return value;
  }

private:
  fn create(json_kind kind) throws -> JsonValue *
  {
    return m_arena.create<JsonValue>(kind, bump_allocator(m_arena));
  }

  fn skip_blanks() wontthrow -> void
  {
    while (m_position < m_source.length) {
      let const byte = m_source[m_position];
      if (byte != ' ' && byte != '\t' && byte != '\r' && byte != '\n') break;
      m_position++;
    }
  }

  fn parse_value() throws -> JsonValue *
  {
    if (m_position >= m_source.length) return nullptr;

    switch (m_source[m_position]) {
    case 'n': return parse_literal("null", json_kind::Null);
    case 't': return parse_literal("true", json_kind::Boolean, true);
    case 'f': return parse_literal("false", json_kind::Boolean, false);
    case '"': return parse_string_value();
    case '[': return parse_array();
    case '{': return parse_object();
    default: return parse_number();
    }
  }

  fn parse_literal(StringView spelling, json_kind kind,
                   bool boolean = false) throws -> JsonValue *
  {
    if (m_position + spelling.length > m_source.length ||
        m_source.substring_of_length(m_position, spelling.length) != spelling)
    {
      return nullptr;
    }
    m_position += spelling.length;
    let *value = create(kind);
    value->boolean = boolean;

    return value;
  }

  fn parse_string() throws -> Maybe<String>
  {
    if (m_position >= m_source.length || m_source[m_position] != '"')
      return None;
    m_position++;
    let result = String{bump_allocator(m_arena)};

    while (m_position < m_source.length) {
      let const byte = m_source[m_position++];
      if (byte == '"') return result;
      if (static_cast<u8>(byte) < 0x20) return None;
      if (byte != '\\') {
        result.push(byte);
        continue;
      }
      if (m_position >= m_source.length) return None;
      let const escaped = m_source[m_position++];
      switch (escaped) {
      case '"': result.push('"'); break;
      case '\\': result.push('\\'); break;
      case '/': result.push('/'); break;
      case 'b': result.push('\b'); break;
      case 'f': result.push('\f'); break;
      case 'n': result.push('\n'); break;
      case 'r': result.push('\r'); break;
      case 't': result.push('\t'); break;
      case 'u': {
        if (m_position + 4 > m_source.length) return None;
        u32 codepoint = 0;
        for (usize digit_index = 0; digit_index < 4; digit_index++) {
          let const digit = utils::hex_digit_value(m_source[m_position++]);
          if (!digit.has_value()) return None;
          codepoint = (codepoint << 4) | *digit;
        }
        if (codepoint >= 0xd800 && codepoint <= 0xdbff) {
          if (m_position + 6 > m_source.length ||
              m_source[m_position] != '\\' || m_source[m_position + 1] != 'u')
          {
            return None;
          }
          m_position += 2;
          u32 low = 0;
          for (usize digit_index = 0; digit_index < 4; digit_index++) {
            let const digit = utils::hex_digit_value(m_source[m_position++]);
            if (!digit.has_value()) return None;
            low = (low << 4) | *digit;
          }
          if (low < 0xdc00 || low > 0xdfff) return None;
          codepoint = 0x10000 + ((codepoint - 0xd800) << 10) + (low - 0xdc00);
        } else if (codepoint >= 0xdc00 && codepoint <= 0xdfff) {
          return None;
        }
        utils::append_utf8(result, codepoint);
        break;
      }
      default: return None;
      }
    }

    return None;
  }

  fn parse_string_value() throws -> JsonValue *
  {
    let parsed = parse_string();
    if (!parsed.has_value()) return nullptr;
    let *value = create(json_kind::String);
    value->text = parsed.take();

    return value;
  }

  fn parse_number() throws -> JsonValue *
  {
    let const start = m_position;
    if (m_position < m_source.length && m_source[m_position] == '-')
      m_position++;
    if (m_position >= m_source.length) return nullptr;
    if (m_source[m_position] == '0') {
      m_position++;
    } else {
      if (m_source[m_position] < '1' || m_source[m_position] > '9')
        return nullptr;
      while (m_position < m_source.length && m_source[m_position] >= '0' &&
             m_source[m_position] <= '9')
        m_position++;
    }
    if (m_position < m_source.length && m_source[m_position] == '.') {
      m_position++;
      if (m_position >= m_source.length || m_source[m_position] < '0' ||
          m_source[m_position] > '9')
        return nullptr;
      while (m_position < m_source.length && m_source[m_position] >= '0' &&
             m_source[m_position] <= '9')
        m_position++;
    }
    if (m_position < m_source.length &&
        (m_source[m_position] == 'e' || m_source[m_position] == 'E'))
    {
      m_position++;
      if (m_position < m_source.length &&
          (m_source[m_position] == '+' || m_source[m_position] == '-'))
        m_position++;
      if (m_position >= m_source.length || m_source[m_position] < '0' ||
          m_source[m_position] > '9')
        return nullptr;
      while (m_position < m_source.length && m_source[m_position] >= '0' &&
             m_source[m_position] <= '9')
        m_position++;
    }
    let *value = create(json_kind::Number);
    value->text =
        String{bump_allocator(m_arena),
               m_source.substring_of_length(start, m_position - start)};

    return value;
  }

  fn parse_array() throws -> JsonValue *
  {
    m_position++;
    let *value = create(json_kind::Array);
    skip_blanks();
    if (m_position < m_source.length && m_source[m_position] == ']') {
      m_position++;
      return value;
    }

    loop
    {
      skip_blanks();
      let *element = parse_value();
      if (element == nullptr) return nullptr;
      value->array.push(element);
      skip_blanks();
      if (m_position >= m_source.length) return nullptr;
      if (m_source[m_position] == ']') {
        m_position++;
        return value;
      }
      if (m_source[m_position++] != ',') return nullptr;
    }
  }

  fn parse_object() throws -> JsonValue *
  {
    m_position++;
    let *value = create(json_kind::Object);
    skip_blanks();
    if (m_position < m_source.length && m_source[m_position] == '}') {
      m_position++;
      return value;
    }

    loop
    {
      skip_blanks();
      let name = parse_string();
      if (!name.has_value()) return nullptr;
      skip_blanks();
      if (m_position >= m_source.length || m_source[m_position++] != ':')
        return nullptr;
      skip_blanks();
      let *member_value = parse_value();
      if (member_value == nullptr) return nullptr;
      value->object.push(json_member{name.take(), member_value});
      skip_blanks();
      if (m_position >= m_source.length) return nullptr;
      if (m_source[m_position] == '}') {
        m_position++;
        return value;
      }
      if (m_source[m_position++] != ',') return nullptr;
    }
  }

  StringView m_source;
  usize m_position{0};
  BumpArena m_arena;
};

fn append_json_string(String &output, StringView value) throws -> void
{
  output.push('"');
  static constexpr char HEX[] = "0123456789abcdef";

  for (usize position = 0; position < value.length; position++) {
    let const byte = static_cast<u8>(value[position]);
    switch (byte) {
    case '"': output.append("\\\""); break;
    case '\\': output.append("\\\\"); break;
    case '\b': output.append("\\b"); break;
    case '\f': output.append("\\f"); break;
    case '\n': output.append("\\n"); break;
    case '\r': output.append("\\r"); break;
    case '\t': output.append("\\t"); break;
    default:
      if (byte < 0x20) {
        output.append("\\u00");
        output.push(HEX[byte >> 4]);
        output.push(HEX[byte & 0xf]);
      } else {
        output.push(static_cast<char>(byte));
      }
    }
  }
  output.push('"');
}

fn append_json_integer(String &output, i64 value) throws -> void
{
  char buffer[21];
  output.append(utils::int_to_text_into(value, buffer, sizeof(buffer)));
}

fn append_json_integer(String &output, u64 value) throws -> void
{
  char buffer[20];
  output.append(utils::uint_to_text_into(value, buffer, sizeof(buffer)));
}

fn append_json_value(String &output, const JsonValue &value) throws -> void
{
  switch (value.kind) {
  case json_kind::Null: output.append("null"); break;
  case json_kind::Boolean:
    output.append(value.boolean ? "true" : "false");
    break;
  case json_kind::Number: output.append(value.text.view()); break;
  case json_kind::String: append_json_string(output, value.text.view()); break;
  case json_kind::Array:
    output.push('[');
    for (usize index = 0; index < value.array.count(); index++) {
      if (index != 0) output.push(',');
      append_json_value(output, *value.array[index]);
    }
    output.push(']');
    break;
  case json_kind::Object:
    output.push('{');
    for (usize index = 0; index < value.object.count(); index++) {
      if (index != 0) output.push(',');
      append_json_string(output, value.object[index].name.view());
      output.push(':');
      append_json_value(output, *value.object[index].value);
    }
    output.push('}');
    break;
  }
}

fn append_request_id(String &output, const JsonValue *id) throws -> void
{
  if (id == nullptr || id->kind == json_kind::Null) {
    output.append("null");
  } else if (id->kind == json_kind::String) {
    append_json_string(output, id->text.view());
  } else if (id->kind == json_kind::Number) {
    output.append(id->text.view());
  } else {
    output.append("null");
  }
}

fn send_payload(StringView payload) wontthrow -> bool
{
  static constexpr StringView PREFIX{"Content-Length: ", 16};
  static constexpr StringView SUFFIX{"\r\n\r\n", 4};
  char digits[20];
  let const length =
      utils::uint_to_text_into(payload.length, digits, sizeof(digits));
  char header[64];
  usize header_length = 0;
  __builtin_memcpy(header + header_length, PREFIX.data, PREFIX.length);
  header_length += PREFIX.length;
  __builtin_memcpy(header + header_length, length.data, length.length);
  header_length += length.length;
  __builtin_memcpy(header + header_length, SUFFIX.data, SUFFIX.length);
  header_length += SUFFIX.length;

  return os::write_all(KOSH_STDOUT, header, header_length) &&
         os::write_all(KOSH_STDOUT, payload.data, payload.length);
}

fn send_result(const JsonValue *id, StringView result) throws -> bool
{
  let payload = String{"{\"jsonrpc\":\"2.0\",\"id\":"};
  append_request_id(payload, id);
  payload.append(",\"result\":");
  payload.append(result);
  payload.push('}');

  return send_payload(payload.view());
}

fn send_error(const JsonValue *id, i64 code, StringView message) throws -> bool
{
  let payload = String{"{\"jsonrpc\":\"2.0\",\"id\":"};
  append_request_id(payload, id);
  payload.append(",\"error\":{\"code\":");
  append_json_integer(payload, code);
  payload.append(",\"message\":");
  append_json_string(payload, message);
  payload.append("}}");

  return send_payload(payload.view());
}

class ProtocolReader
{
public:
  fn read_message() throws -> Maybe<StringView>
  {
    compact();
    m_header_scan_position = m_offset;
    usize header_end = 0;

    loop
    {
      header_end = find_header_end();
      if (header_end != static_cast<usize>(-1)) break;
      if (!read_more()) return None;
      if (m_buffer.count() - m_offset > MAX_HEADER_LENGTH) return None;
    }

    let const headers =
        m_buffer.substring_of_length(m_offset, header_end - m_offset);
    let content_length = parse_content_length(headers);
    if (!content_length.has_value() || *content_length > MAX_MESSAGE_LENGTH)
      return None;
    let const body_start = header_end + 4;

    while (m_buffer.count() - body_start < *content_length)
      if (!read_more()) return None;

    let const body = m_buffer.substring_of_length(body_start, *content_length);
    m_offset = body_start + *content_length;

    return body;
  }

private:
  static constexpr usize MAX_HEADER_LENGTH = 16 * 1024;
  static constexpr usize MAX_MESSAGE_LENGTH = 16 * 1024 * 1024;

  fn find_header_end() wontthrow -> usize
  {
    for (usize position = m_header_scan_position;
         position + 3 < m_buffer.count(); position++)
      if (m_buffer[position] == '\r' && m_buffer[position + 1] == '\n' &&
          m_buffer[position + 2] == '\r' && m_buffer[position + 3] == '\n')
        return position;

    m_header_scan_position =
        m_buffer.count() > 3 ? m_buffer.count() - 3 : m_offset;

    return static_cast<usize>(-1);
  }

  pure fn parse_content_length(StringView headers) const throws -> Maybe<usize>
  {
    static const StringView PREFIX{"Content-Length:"};
    usize line_start = 0;

    while (line_start <= headers.length) {
      usize line_end = line_start;
      while (line_end < headers.length && headers[line_end] != '\r')
        line_end++;
      let const line =
          headers.substring_of_length(line_start, line_end - line_start);
      if (line.starts_with(PREFIX)) {
        usize position = PREFIX.length;
        while (position < line.length &&
               (line[position] == ' ' || line[position] == '\t'))
          position++;
        if (position == line.length) return None;
        let const parsed = utils::parse_decimal_u64(line.substring(position));
        if (parsed.is_error() || parsed.value() > MAX_MESSAGE_LENGTH)
          return None;

        return static_cast<usize>(parsed.value());
      }
      if (line_end == headers.length) break;
      line_start = line_end + 2;
    }

    return None;
  }

  fn read_more() throws -> bool
  {
    char bytes[8192];
    let const count = os::read_fd(KOSH_STDIN, bytes, sizeof(bytes));
    if (!count.has_value() || *count == 0) return false;
    m_buffer.append(StringView{bytes, *count});

    return true;
  }

  fn compact() throws -> void
  {
    if (m_offset == 0) return;
    if (m_offset == m_buffer.count()) {
      m_buffer.clear();
      m_offset = 0;
      return;
    }
    if (m_offset < 64 * 1024) return;
    m_buffer = String{
        m_buffer.substring_of_length(m_offset, m_buffer.count() - m_offset)};
    m_offset = 0;
  }

  String m_buffer{heap_allocator()};
  usize m_offset{0};
  usize m_header_scan_position{0};
};

pure fn string_field(const JsonValue *object, StringView name) wontthrow
    -> Maybe<StringView>
{
  if (object == nullptr || object->kind != json_kind::Object) return None;
  let const *value = object->get(name);
  if (value == nullptr || value->kind != json_kind::String) return None;

  return value->text.view();
}

pure fn integer_field(const JsonValue *object, StringView name) throws
    -> Maybe<i64>
{
  if (object == nullptr || object->kind != json_kind::Object) return None;
  let const *value = object->get(name);
  if (value == nullptr || value->kind != json_kind::Number ||
      value->text.is_empty())
    return None;
  let const parsed = utils::parse_decimal_i64(value->text.view());
  if (parsed.is_error()) return None;

  return parsed.value();
}

enum class position_encoding : u8
{
  Utf8,
  Utf16,
};

struct protocol_position
{
  usize line;
  usize character;
};

struct document_symbol
{
  String text;
  highlight_role role;
  usize start;
  usize end;
};

class Document
{
public:
  Document(StringView document_uri, StringView document_language,
           StringView source, i64 document_version) throws
      : uri(document_uri),
        language_id(document_language),
        normalized_source(source),
        version(document_version),
        line_starts(heap_allocator()),
        diagnostics(heap_allocator()),
        auxiliary_diagnostics(heap_allocator()),
        followed_paths(heap_allocator())
  {
    normalized_source.normalize_crlf_line_endings();
    rebuild_lines();
  }

  fn replace_source(StringView source, i64 document_version) throws -> void
  {
    normalized_source = String{source};
    normalized_source.normalize_crlf_line_endings();
    version = document_version;
    rebuild_lines();
  }

  pure fn byte_position(usize line, usize character,
                        position_encoding encoding) const wontthrow
      -> Maybe<usize>
  {
    if (line >= line_starts.count()) return None;
    let const start = line_starts[line];
    let end = normalized_source.count();
    if (line + 1 < line_starts.count()) end = line_starts[line + 1] - 1;
    if (encoding == position_encoding::Utf8) {
      if (character > end - start) return None;
      let const position = start + character;
      if (position < end &&
          (static_cast<u8>(normalized_source[position]) & 0xc0) == 0x80)
        return None;
      return position;
    }

    usize position = start;
    usize units = 0;
    while (position < end && units < character) {
      let const decoded =
          utils::decode_utf8(normalized_source.view(), position, 0xfffd);
      let const codepoint_units = decoded.value > 0xffff ? 2u : 1u;
      if (units + codepoint_units > character) return None;
      units += codepoint_units;
      position += decoded.length;
    }
    if (units != character) return None;

    return position;
  }

  pure fn protocol_position_at(usize byte_position,
                               position_encoding encoding) const wontthrow
      -> protocol_position
  {
    if (byte_position > normalized_source.count())
      byte_position = normalized_source.count();
    usize lower = 0;
    usize upper = line_starts.count();
    while (lower + 1 < upper) {
      let const middle = lower + (upper - lower) / 2;
      if (line_starts[middle] <= byte_position)
        lower = middle;
      else
        upper = middle;
    }
    let const line = lower;
    let const start = line_starts[line];
    if (encoding == position_encoding::Utf8)
      return {line, byte_position - start};

    usize units = 0;
    usize position = start;
    while (position < byte_position) {
      let const decoded =
          utils::decode_utf8(normalized_source.view(), position, 0xfffd);
      units += decoded.value > 0xffff ? 2 : 1;
      position += decoded.length;
    }

    return {line, units};
  }

  pure fn encoded_length(usize start, usize end,
                         position_encoding encoding) const wontthrow -> usize
  {
    if (end < start) return 0;
    if (end > normalized_source.count()) end = normalized_source.count();
    if (encoding == position_encoding::Utf8) return end - start;
    usize units = 0;

    while (start < end) {
      let const decoded =
          utils::decode_utf8(normalized_source.view(), start, 0xfffd);
      units += decoded.value > 0xffff ? 2 : 1;
      start += decoded.length;
    }

    return units;
  }

  String uri;
  String language_id;
  String normalized_source;
  i64 version;
  mimic_mood mood{mimic_mood::Default};
  u64 diagnostic_revision{0};
  Maybe<Path> path;
  Maybe<Path> canonical_path;
  ArrayList<usize> line_starts;
  ArrayList<source_diagnostic> diagnostics;
  ArrayList<source_diagnostic> auxiliary_diagnostics;
  HashSet followed_paths;

private:
  fn rebuild_lines() throws -> void
  {
    line_starts.clear();
    line_starts.push(0);

    for (usize position = 0; position < normalized_source.count(); position++)
      if (normalized_source[position] == '\n') line_starts.push(position + 1);
  }
};

fn decode_file_uri(StringView uri) throws -> Maybe<Path>
{
  static const StringView PREFIX{"file://"};
  if (!uri.starts_with(PREFIX)) return None;
  let path_text = uri.substring(PREFIX.length);
  if (path_text.starts_with("localhost/")) path_text = path_text.substring(9);
  let decoded = String{heap_allocator()};

  for (usize position = 0; position < path_text.length; position++) {
    if (path_text[position] != '%') {
      decoded.push(path_text[position]);
      continue;
    }
    if (position + 2 >= path_text.length) return None;
    let const high = utils::hex_digit_value(path_text[position + 1]);
    let const low = utils::hex_digit_value(path_text[position + 2]);
    if (!high.has_value() || !low.has_value()) return None;
    decoded.push(static_cast<char>((*high << 4) | *low));
    position += 2;
  }

  return Path{decoded.view()};
}

fn file_uri_for_path(const Path &path) throws -> String
{
  static constexpr char HEX[] = "0123456789ABCDEF";
  let uri = String{"file://"};

  for (usize position = 0; position < path.count(); position++) {
    let const byte = path.text()[position];
    if (os::is_directory_separator(byte)) {
      uri.push('/');
    } else if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
               (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' ||
               byte == '.' || byte == '~' || byte == ':')
    {
      uri.push(byte);
    } else {
      let const unsigned_byte = static_cast<u8>(byte);
      uri.push('%');
      uri.push(HEX[unsigned_byte >> 4]);
      uri.push(HEX[unsigned_byte & 0xf]);
    }
  }

  return uri;
}

fn append_protocol_position(String &output, protocol_position position) throws
    -> void
{
  output.append("{\"line\":");
  append_json_integer(output, static_cast<u64>(position.line));
  output.append(",\"character\":");
  append_json_integer(output, static_cast<u64>(position.character));
  output.push('}');
}

fn append_protocol_range(String &output, const Document &document, usize start,
                         usize end, position_encoding encoding) throws -> void
{
  output.append("{\"start\":");
  append_protocol_position(output,
                           document.protocol_position_at(start, encoding));
  output.append(",\"end\":");
  append_protocol_position(output,
                           document.protocol_position_at(end, encoding));
  output.push('}');
}

pure fn mood_for(const Document &document) throws -> mimic_mood
{
  let const id = document.language_id.view();
  if (let const mood = LANGUAGE_MOODS.find(id); mood.has_value()) return *mood;
  if (let const detected =
          detect_mimic_shell_from_source(document.normalized_source.view());
      detected.has_value())
    return *detected;

  return mimic_mood::Default;
}

pure fn document_position(const JsonValue *position) throws
    -> Maybe<protocol_position>
{
  let const line = integer_field(position, "line");
  let const character = integer_field(position, "character");
  if (!line.has_value() || !character.has_value() || *line < 0 ||
      *character < 0)
    return None;

  return protocol_position{static_cast<usize>(*line),
                           static_cast<usize>(*character)};
}

} /* namespace */

} /* namespace koshka::language_server */
