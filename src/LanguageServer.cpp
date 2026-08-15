#include "LanguageServer.hpp"

#include "Builtin.hpp"
#include "Completion.hpp"
#include "CompletionPolicy.hpp"
#include "Diagnostics.hpp"
#include "Expressions.hpp"
#include "Koshkit.hpp"
#include "Lexer.hpp"
#include "MimicMood.hpp"
#include "Parser.hpp"
#include "Path.hpp"
#include "Platform.hpp"
#include "StaticStringMap.hpp"

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

constexpr PackedStringKey FIX_ALL_KIND_KEYS[] = {
    SSK("source"),
    SSK("source.fixAll"),
    SSK("source.fixAll.kosh"),
};
constexpr StaticStringSet FIX_ALL_KINDS{FIX_ALL_KIND_KEYS};

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
  explicit JsonValue(json_kind value_kind)
      : kind(value_kind), array(heap_allocator()), object(heap_allocator())
  {}

  pure fn get(StringView name) const wontthrow -> const JsonValue *
  {
    for (let const &member : object)
      if (member.name == name) return member.value;

    return nullptr;
  }

  json_kind kind;
  bool boolean{false};
  String text{heap_allocator()};
  ArrayList<JsonValue *> array;
  ArrayList<json_member> object;
};

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

pure fn hex_value(char byte) wontthrow -> Maybe<u8>
{
  if (byte >= '0' && byte <= '9') return static_cast<u8>(byte - '0');
  if (byte >= 'a' && byte <= 'f') return static_cast<u8>(byte - 'a' + 10);
  if (byte >= 'A' && byte <= 'F') return static_cast<u8>(byte - 'A' + 10);

  return None;
}

class JsonParser
{
public:
  explicit JsonParser(StringView source)
      : m_source(source), m_values(heap_allocator())
  {}

  ~JsonParser()
  {
    for (let *value : m_values)
      delete value;
  }

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
    let *value = new JsonValue{kind};
    m_values.push(value);

    return value;
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
    let result = String{heap_allocator()};

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
          let const digit = hex_value(m_source[m_position++]);
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
            let const digit = hex_value(m_source[m_position++]);
            if (!digit.has_value()) return None;
            low = (low << 4) | *digit;
          }
          if (low < 0xdc00 || low > 0xdfff) return None;
          codepoint = 0x10000 + ((codepoint - 0xd800) << 10) + (low - 0xdc00);
        } else if (codepoint >= 0xdc00 && codepoint <= 0xdfff) {
          return None;
        }
        append_utf8(result, codepoint);
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
        String{m_source.substring_of_length(start, m_position - start)};

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
  ArrayList<JsonValue *> m_values;
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
  let header = String{"Content-Length: "};
  try {
    header.append(String::from(payload.length, heap_allocator()).view());
    header.append("\r\n\r\n");
  } catch (...) {
    return false;
  }

  return os::write_all(KOSH_STDOUT, header.view().data, header.count()) &&
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
  payload.append(String::from(code, heap_allocator()).view());
  payload.append(",\"message\":");
  append_json_string(payload, message);
  payload.append("}}");

  return send_payload(payload.view());
}

class ProtocolReader
{
public:
  fn read_message() throws -> Maybe<String>
  {
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

    let body =
        String{m_buffer.substring_of_length(body_start, *content_length)};
    m_offset = body_start + *content_length;
    compact();

    return body;
  }

private:
  static constexpr usize MAX_HEADER_LENGTH = 16 * 1024;
  static constexpr usize MAX_MESSAGE_LENGTH = 16 * 1024 * 1024;

  pure fn find_header_end() const wontthrow -> usize
  {
    for (usize position = m_offset; position + 3 < m_buffer.count(); position++)
      if (m_buffer[position] == '\r' && m_buffer[position + 1] == '\n' &&
          m_buffer[position + 2] == '\r' && m_buffer[position + 3] == '\n')
        return position;

    return static_cast<usize>(-1);
  }

  pure fn parse_content_length(StringView headers) const wontthrow
      -> Maybe<usize>
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
        usize length = 0;
        for (; position < line.length; position++) {
          if (line[position] < '0' || line[position] > '9') return None;
          let const digit = static_cast<usize>(line[position] - '0');
          if (length > (MAX_MESSAGE_LENGTH - digit) / 10) return None;
          length = length * 10 + digit;
        }
        return length;
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
};

pure fn string_field(const JsonValue *object, StringView name) wontthrow
    -> Maybe<StringView>
{
  if (object == nullptr || object->kind != json_kind::Object) return None;
  let const *value = object->get(name);
  if (value == nullptr || value->kind != json_kind::String) return None;

  return value->text.view();
}

pure fn integer_field(const JsonValue *object, StringView name) wontthrow
    -> Maybe<i64>
{
  if (object == nullptr || object->kind != json_kind::Object) return None;
  let const *value = object->get(name);
  if (value == nullptr || value->kind != json_kind::Number ||
      value->text.is_empty())
    return None;
  usize position = 0;
  bool is_negative = false;
  if (value->text[0] == '-') {
    is_negative = true;
    position++;
  }
  if (position == value->text.count()) return None;
  i64 result = 0;
  for (; position < value->text.count(); position++) {
    let const byte = value->text[position];
    if (byte < '0' || byte > '9') return None;
    result = result * 10 + static_cast<i64>(byte - '0');
  }

  return is_negative ? -result : result;
}

enum class position_encoding : u8
{
  Utf8,
  Utf16,
};

struct decoded_codepoint
{
  u32 value;
  usize length;
};

pure fn decode_utf8(StringView source, usize position) wontthrow
    -> decoded_codepoint
{
  let const first = static_cast<u8>(source[position]);
  if (first < 0x80) return {first, 1};
  usize length = 0;
  u32 value = 0;
  if ((first & 0xe0) == 0xc0) {
    length = 2;
    value = first & 0x1f;
  } else if ((first & 0xf0) == 0xe0) {
    length = 3;
    value = first & 0x0f;
  } else if ((first & 0xf8) == 0xf0) {
    length = 4;
    value = first & 0x07;
  } else {
    return {0xfffd, 1};
  }
  if (position + length > source.length) return {0xfffd, 1};

  for (usize index = 1; index < length; index++) {
    let const next = static_cast<u8>(source[position + index]);
    if ((next & 0xc0) != 0x80) return {0xfffd, 1};
    value = (value << 6) | (next & 0x3f);
  }

  return {value, length};
}

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
        original_source(source),
        normalized_source(source),
        version(document_version),
        line_starts(heap_allocator()),
        diagnostics(heap_allocator())
  {
    normalized_source.normalize_crlf_line_endings();
    rebuild_lines();
  }

  fn replace_source(StringView source, i64 document_version) throws -> void
  {
    original_source = String{source};
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
      let const decoded = decode_utf8(normalized_source.view(), position);
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
    usize line = 0;
    for (usize index = 1; index < line_starts.count(); index++) {
      if (line_starts[index] > byte_position) break;
      line = index;
    }
    let const start = line_starts[line];
    if (encoding == position_encoding::Utf8)
      return {line, byte_position - start};

    usize units = 0;
    usize position = start;
    while (position < byte_position) {
      let const decoded = decode_utf8(normalized_source.view(), position);
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
      let const decoded = decode_utf8(normalized_source.view(), start);
      units += decoded.value > 0xffff ? 2 : 1;
      start += decoded.length;
    }

    return units;
  }

  String uri;
  String language_id;
  String original_source;
  String normalized_source;
  i64 version;
  Maybe<Path> path;
  ArrayList<usize> line_starts;
  ArrayList<source_diagnostic> diagnostics;

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
    let const high = hex_value(path_text[position + 1]);
    let const low = hex_value(path_text[position + 2]);
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
  output.append(String::from(position.line, heap_allocator()).view());
  output.append(",\"character\":");
  output.append(String::from(position.character, heap_allocator()).view());
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

pure fn document_position(const JsonValue *position) wontthrow
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

class Server : public AnalysisSourceProvider
{
public:
  Server(EvalContext &context, BumpArena &ast_arena)
      : m_context(context), m_ast_arena(ast_arena),
        m_documents(heap_allocator()),
        m_workspace_root(Path::current_directory())
  {}

  fn run() throws -> int;
  fn read_source(const Path &canonical_path) throws -> Maybe<String> override;

private:
  fn dispatch(const JsonValue &message) throws -> bool;
  fn initialize(const JsonValue *id, const JsonValue *params) throws -> bool;
  fn open_document(const JsonValue *params) throws -> void;
  fn change_document(const JsonValue *params) throws -> void;
  fn close_document(const JsonValue *params) throws -> void;
  fn complete(const JsonValue *id, const JsonValue *params) throws -> bool;
  fn definition(const JsonValue *id, const JsonValue *params) throws -> bool;
  fn hover(const JsonValue *id, const JsonValue *params) throws -> bool;
  fn semantic_tokens(const JsonValue *id, const JsonValue *params) throws
      -> bool;
  fn code_actions(const JsonValue *id, const JsonValue *params) throws -> bool;
  fn publish_diagnostics(Document &document) throws -> bool;
  fn send_diagnostics(const Document &document,
                      const ArrayList<source_diagnostic> &diagnostics) throws
      -> bool;
  fn publish_auxiliary_diagnostics(
      const Document &root_document,
      const ArrayList<source_diagnostic> &diagnostics) throws -> bool;
  fn validate_all() throws -> bool;
  pure fn find_document(StringView uri) wontthrow -> Document *;
  fn append_diagnostic(String &output, const Document &document,
                       const source_diagnostic &diagnostic,
                       usize diagnostic_index, bool &is_first) throws -> void;
  fn select_document_mood(const Document &document) wontthrow -> void;
  fn symbol_at(Document &document, protocol_position position) throws
      -> Maybe<document_symbol>;
  fn definition_of(Document &document, const document_symbol &symbol) throws
      -> Maybe<document_symbol>;
  fn command_information(StringView command) throws -> Maybe<String>;

  EvalContext &m_context;
  BumpArena &m_ast_arena;
  ArrayList<Document> m_documents;
  ArrayList<String> m_published_auxiliary_uris{heap_allocator()};
  ArrayList<String> m_current_auxiliary_uris{heap_allocator()};
  Path m_workspace_root;
  position_encoding m_encoding{position_encoding::Utf16};
  bool m_is_initialized{false};
  bool m_is_shutdown{false};
  bool m_supports_document_changes{false};
  bool m_supports_code_action_literals{false};
  bool m_supports_diagnostic_data{false};
  bool m_supports_preferred_actions{false};
};

pure fn semantic_style(highlight_role role) wontthrow -> std::pair<u32, u32>
{
  static constexpr u32 DECLARATION = 1u << 0;
  static constexpr u32 READONLY = 1u << 1;
  static constexpr u32 INVALID = 1u << 2;
  static constexpr u32 UNRESOLVED = 1u << 3;
  static constexpr u32 PARTIAL = 1u << 4;
  static constexpr u32 PATH = 1u << 5;
  static constexpr u32 COMMAND = 1u << 6;
  static constexpr u32 HEREDOC = 1u << 7;
  static constexpr u32 URL = 1u << 8;

  switch (role) {
  case highlight_role::comment: return {0, 0};
  case highlight_role::operator_: return {1, 0};
  case highlight_role::string: return {2, 0};
  case highlight_role::heredoc: return {2, HEREDOC};
  case highlight_role::variable: return {3, 0};
  case highlight_role::assignment_name: return {3, DECLARATION};
  case highlight_role::unset_variable: return {3, UNRESOLVED};
  case highlight_role::flag: return {4, READONLY};
  case highlight_role::keyword: return {5, 0};
  case highlight_role::invalid_syntax: return {1, INVALID};
  case highlight_role::function_name: return {6, DECLARATION};
  case highlight_role::resolved_command: return {6, COMMAND};
  case highlight_role::partial_command: return {6, COMMAND | PARTIAL};
  case highlight_role::unknown_command: return {6, COMMAND | UNRESOLVED};
  case highlight_role::existing_path: return {2, PATH};
  case highlight_role::partial_path: return {2, PATH | PARTIAL};
  case highlight_role::invalid_path: return {2, PATH | INVALID};
  case highlight_role::url: return {2, URL};
  case highlight_role::glob: return {7, 0};
  case highlight_role::count: break;
  }

  return {1, 0};
}

pure fn Server::find_document(StringView uri) wontthrow -> Document *
{
  for (let &document : m_documents)
    if (document.uri == uri) return &document;

  return nullptr;
}

fn Server::select_document_mood(const Document &document) wontthrow -> void
{
  m_context.set_mood(mood_for(document));
  m_context.apply_strictness_for_mood();
  m_context.set_warning_level(3);
  m_context.set_diagnostics_disabled(false);
  m_context.set_annoying_diagnostics_enabled(true);
}

fn Server::read_source(const Path &canonical_path) throws -> Maybe<String>
{
  for (let const &document : m_documents) {
    if (!document.path.has_value()) continue;
    let const open_path = os::canonical_path(*document.path);
    if (open_path.has_value() && open_path->text() == canonical_path.text())
      return document.normalized_source.clone();
  }

  return None;
}

fn Server::initialize(const JsonValue *id, const JsonValue *params) throws
    -> bool
{
  if (params != nullptr && params->kind == json_kind::Object) {
    if (let const root_uri = string_field(params, "rootUri");
        root_uri.has_value())
    {
      if (let path = decode_file_uri(*root_uri); path.has_value())
        m_workspace_root = path.take();
    }
    let const *capabilities = params->get("capabilities");
    let const *workspace =
        capabilities != nullptr ? capabilities->get("workspace") : nullptr;
    let const *workspace_edit =
        workspace != nullptr ? workspace->get("workspaceEdit") : nullptr;
    let const *document_changes = workspace_edit != nullptr
                                      ? workspace_edit->get("documentChanges")
                                      : nullptr;
    m_supports_document_changes =
        document_changes != nullptr &&
        document_changes->kind == json_kind::Boolean &&
        document_changes->boolean;
    let const *text_document =
        capabilities != nullptr ? capabilities->get("textDocument") : nullptr;
    let const *code_action =
        text_document != nullptr ? text_document->get("codeAction") : nullptr;
    m_supports_code_action_literals =
        code_action != nullptr &&
        code_action->get("codeActionLiteralSupport") != nullptr;
    let const *preferred_support = code_action != nullptr
                                       ? code_action->get("isPreferredSupport")
                                       : nullptr;
    m_supports_preferred_actions =
        preferred_support != nullptr &&
        preferred_support->kind == json_kind::Boolean &&
        preferred_support->boolean;
    let const *publish_diagnostics =
        text_document != nullptr ? text_document->get("publishDiagnostics")
                                 : nullptr;
    let const *data_support = publish_diagnostics != nullptr
                                  ? publish_diagnostics->get("dataSupport")
                                  : nullptr;
    m_supports_diagnostic_data = data_support != nullptr &&
                                 data_support->kind == json_kind::Boolean &&
                                 data_support->boolean;
    let const *general =
        capabilities != nullptr ? capabilities->get("general") : nullptr;
    let const *encodings =
        general != nullptr ? general->get("positionEncodings") : nullptr;
    if (encodings != nullptr && encodings->kind == json_kind::Array) {
      for (let const *encoding : encodings->array) {
        if (encoding->kind == json_kind::String && encoding->text == "utf-8") {
          m_encoding = position_encoding::Utf8;
          break;
        }
      }
    }
  }
  m_is_initialized = true;
  let const encoding =
      m_encoding == position_encoding::Utf8 ? "utf-8" : "utf-16";
  let result = String{"{\"capabilities\":{\"positionEncoding\":"};
  append_json_string(result, encoding);
  result.append(",\"textDocumentSync\":{\"openClose\":true,\"change\":1},"
                "\"completionProvider\":{\"resolveProvider\":false},"
                "\"definitionProvider\":true,\"hoverProvider\":true,");
  if (m_supports_code_action_literals)
    result.append("\"codeActionProvider\":{\"codeActionKinds\":[\"quickfix\","
                  "\"source.fixAll.kosh\"],\"resolveProvider\":false},");
  result.append(
      "\"semanticTokensProvider\":{\"legend\":{\"tokenTypes\":["
      "\"comment\",\"operator\",\"string\",\"variable\",\"parameter\","
      "\"keyword\",\"function\",\"regexp\"],\"tokenModifiers\":["
      "\"declaration\",\"readonly\",\"invalid\",\"unresolved\","
      "\"partial\",\"path\",\"command\",\"heredoc\",\"url\"]},"
      "\"full\":true,\"range\":false}},\"serverInfo\":{\"name\":"
      "\"kosh\"}}");

  return send_result(id, result.view());
}

fn Server::open_document(const JsonValue *params) throws -> void
{
  let const *text_document =
      params != nullptr ? params->get("textDocument") : nullptr;
  let const uri = string_field(text_document, "uri");
  let const language_id = string_field(text_document, "languageId");
  let const text = string_field(text_document, "text");
  let const version = integer_field(text_document, "version");
  if (!uri.has_value() || !language_id.has_value() || !text.has_value()) return;
  let const document_version = version.value_or(0);
  if (let *existing = find_document(*uri); existing != nullptr) {
    existing->language_id = String{*language_id};
    existing->replace_source(*text, document_version);
    return;
  }
  let document = Document{*uri, *language_id, *text, document_version};
  document.path = decode_file_uri(*uri);
  m_documents.push(steal(document));
}

fn Server::change_document(const JsonValue *params) throws -> void
{
  let const *text_document =
      params != nullptr ? params->get("textDocument") : nullptr;
  let const uri = string_field(text_document, "uri");
  let const version = integer_field(text_document, "version");
  let const *changes =
      params != nullptr ? params->get("contentChanges") : nullptr;
  if (!uri.has_value() || changes == nullptr ||
      changes->kind != json_kind::Array || changes->array.is_empty())
    return;
  let *document = find_document(*uri);
  if (document == nullptr) return;
  let const text = string_field(changes->array.back(), "text");
  if (!text.has_value()) return;
  document->replace_source(*text, version.value_or(document->version + 1));
}

fn Server::close_document(const JsonValue *params) throws -> void
{
  let const *text_document =
      params != nullptr ? params->get("textDocument") : nullptr;
  let const uri = string_field(text_document, "uri");
  if (!uri.has_value()) return;
  let payload = String{"{\"jsonrpc\":\"2.0\",\"method\":"
                       "\"textDocument/publishDiagnostics\",\"params\":{"
                       "\"uri\":"};
  append_json_string(payload, *uri);
  payload.append(",\"diagnostics\":[]}}");
  send_payload(payload.view());

  for (usize index = 0; index < m_documents.count(); index++) {
    if (m_documents[index].uri != *uri) continue;
    m_documents.remove(index);
    break;
  }
}

fn Server::append_diagnostic(String &output, const Document &document,
                             const source_diagnostic &diagnostic,
                             usize diagnostic_index, bool &is_first) throws
    -> void
{
  if (!diagnostic.source_name.is_empty() && document.path.has_value() &&
      diagnostic.source_name != document.path->text())
    return;
  let const do_append_diagnostic =
      [&](SourceLocation location, u8 severity, StringView message,
          Maybe<diagnostic_id> id, bool has_fixes) throws -> void {
    if (!is_first) output.push(',');
    is_first = false;
    output.append("{\"range\":");
    append_protocol_range(output, document, location.position,
                          location.position + location.length, m_encoding);
    output.append(",\"severity\":");
    output.append(String::from(severity, heap_allocator()).view());
    if (id.has_value()) {
      output.append(",\"code\":");
      append_json_string(output, get_diagnostic_definition(*id).slug);
    }
    output.append(",\"source\":\"kosh\",\"message\":");
    append_json_string(output, message);
    if (has_fixes && m_supports_diagnostic_data && document.version >= 0) {
      output.append(",\"data\":{\"kind\":\"kosh.fix\","
                    "\"documentVersion\":");
      output.append(String::from(document.version, heap_allocator()).view());
      output.append(",\"diagnosticIndex\":");
      output.append(String::from(diagnostic_index, heap_allocator()).view());
      output.push('}');
    }
    output.push('}');
  };

  let const severity = diagnostic.severity == error_severity::Error     ? 1
                       : diagnostic.severity == error_severity::Warning ? 2
                                                                        : 3;
  if (!diagnostic.message.is_empty())
    do_append_diagnostic(diagnostic.location, severity,
                         diagnostic.message.view(), diagnostic.id,
                         !diagnostic.fixes.is_empty());
  if (diagnostic.id.has_value() &&
      (*diagnostic.id == diagnostic_id::unresolved_command ||
       *diagnostic.id == diagnostic_id::unresolved_command_uncertain))
  {
    do_append_diagnostic(
        diagnostic.location, 3,
        "This command may be defined dynamically or outside this script", None,
        false);
  }
  if (!diagnostic.suggestion.is_empty())
    do_append_diagnostic(diagnostic.location, 3, diagnostic.suggestion.view(),
                         None, false);
  if (diagnostic.related_location.has_value() &&
      !diagnostic.related_message.is_empty() &&
      (diagnostic.related_source_name.is_empty() ||
       !document.path.has_value() ||
       diagnostic.related_source_name == document.path->text()))
  {
    do_append_diagnostic(*diagnostic.related_location, 3,
                         diagnostic.related_message.view(), None, false);
  }
}

fn Server::publish_diagnostics(Document &document) throws -> bool
{
  select_document_mood(document);
  let const arena_mark = m_ast_arena.mark();
  defer { m_ast_arena.release(arena_mark); };
  let const filename = document.path.has_value() ? document.path->text().view()
                                                 : document.uri.view();
  let parser = Parser{
      Lexer{String{document.normalized_source.view()}, m_ast_arena, false,
            filename, m_context.mood()}
  };
  parser.set_should_collect_analysis_scopes(true);
  let rendered_errors = ArrayList<String>{heap_allocator()};
  let diagnostics = ArrayList<source_diagnostic>{heap_allocator()};
  let const ast =
      parser.construct_ast(rendered_errors, &m_context, &diagnostics);
  if (rendered_errors.is_empty()) {
    let const suppressions = parser.take_shellcheck_suppressions();
    let const scopes = parser.take_analysis_scope_definitions();
    let const directives = parser.take_shellcheck_directive_spans();
    let const heredoc_misses = parser.take_heredoc_terminator_misses();
    let const functions = m_context.function_names();
    let const aliases = m_context.alias_names();
    let followed_paths = HashSet{heap_allocator()};
    let source_effects = StringMap<followed_source_effects>{heap_allocator()};
    if (document.path.has_value()) {
      if (let canonical = os::canonical_path(*document.path);
          canonical.has_value())
        followed_paths.add(canonical->text().view());
    }
    analyze_ast(ast, document.normalized_source.view(), functions, aliases,
                &m_context, 3, false, m_context.mood() == mimic_mood::Default,
                true, suppressions, scopes, directives, heredoc_misses,
                document.path.has_value(), false, &followed_paths,
                &source_effects, nullptr, nullptr, true, true, nullptr,
                &diagnostics, this);
  }

  let const did_send = send_diagnostics(document, diagnostics) &&
                       publish_auxiliary_diagnostics(document, diagnostics);
  document.diagnostics = steal(diagnostics);

  return did_send;
}

fn Server::send_diagnostics(
    const Document &document,
    const ArrayList<source_diagnostic> &diagnostics) throws -> bool
{
  let payload = String{"{\"jsonrpc\":\"2.0\",\"method\":"
                       "\"textDocument/publishDiagnostics\",\"params\":{"
                       "\"uri\":"};
  append_json_string(payload, document.uri.view());
  if (document.version >= 0) {
    payload.append(",\"version\":");
    payload.append(String::from(document.version, heap_allocator()).view());
  }
  payload.append(",\"diagnostics\":[");
  bool is_first = true;
  for (usize diagnostic_index = 0; diagnostic_index < diagnostics.count();
       diagnostic_index++)
    append_diagnostic(payload, document, diagnostics[diagnostic_index],
                      diagnostic_index, is_first);
  payload.append("]}}");

  return send_payload(payload.view());
}

fn Server::publish_auxiliary_diagnostics(
    const Document &root_document,
    const ArrayList<source_diagnostic> &diagnostics) throws -> bool
{
  let const root_name = root_document.path.has_value()
                            ? root_document.path->text().view()
                            : root_document.uri.view();

  for (let const &diagnostic : diagnostics) {
    if (diagnostic.source_name.is_empty() ||
        diagnostic.source_name == root_name)
      continue;
    let const source_path = Path{diagnostic.source_name.view()};
    let const source_uri = file_uri_for_path(source_path);
    if (m_current_auxiliary_uris.find(source_uri).has_value()) continue;
    bool is_open_source = find_document(source_uri.view()) != nullptr;
    if (!is_open_source) {
      let const canonical_source = os::canonical_path(source_path);
      for (let const &document : m_documents) {
        if (!canonical_source.has_value() || !document.path.has_value())
          continue;
        let const canonical_document = os::canonical_path(*document.path);
        if (canonical_document.has_value() &&
            canonical_document->text() == canonical_source->text())
        {
          is_open_source = true;
          break;
        }
      }
    }
    if (is_open_source) continue;
    let source = read_source(source_path);
    if (!source.has_value()) source = source_path.read_entire_file();
    if (!source.has_value()) continue;
    let auxiliary =
        Document{source_uri.view(), "shellscript", source->view(), -1};
    auxiliary.path = source_path;
    if (!send_diagnostics(auxiliary, diagnostics)) return false;
    m_current_auxiliary_uris.push(source_uri.clone());
  }

  return true;
}

fn Server::validate_all() throws -> bool
{
  m_current_auxiliary_uris.clear();
  for (let &document : m_documents)
    if (!publish_diagnostics(document)) return false;

  for (let const &uri : m_published_auxiliary_uris) {
    if (m_current_auxiliary_uris.find(uri).has_value()) continue;
    let payload = String{"{\"jsonrpc\":\"2.0\",\"method\":"
                         "\"textDocument/publishDiagnostics\",\"params\":{"
                         "\"uri\":"};
    append_json_string(payload, uri.view());
    payload.append(",\"diagnostics\":[]}}");
    if (!send_payload(payload.view())) return false;
  }
  m_published_auxiliary_uris = m_current_auxiliary_uris;

  return true;
}

fn Server::complete(const JsonValue *id, const JsonValue *params) throws -> bool
{
  let const *text_document =
      params != nullptr ? params->get("textDocument") : nullptr;
  let const uri = string_field(text_document, "uri");
  let const *position_value =
      params != nullptr ? params->get("position") : nullptr;
  let const position = document_position(position_value);
  if (!uri.has_value() || !position.has_value()) return send_result(id, "[]");
  let *document = find_document(*uri);
  if (document == nullptr) return send_result(id, "[]");
  let const cursor =
      document->byte_position(position->line, position->character, m_encoding);
  if (!cursor.has_value()) return send_result(id, "[]");
  select_document_mood(*document);
  let base_directory = m_workspace_root;
  if (document->path.has_value()) base_directory = document->path->parent();
  let result = completion::complete(document->normalized_source.view(), *cursor,
                                    m_context, base_directory,
                                    completion::completion_mode::Listing);
  let response = String{"["};

  for (usize index = 0; index < result.candidates.count(); index++) {
    if (index != 0) response.push(',');
    let const &candidate = result.candidates[index];
    response.append("{\"label\":");
    append_json_string(response, candidate.view());
    if (let const *description = result.descriptions.find(candidate.view());
        description != nullptr)
    {
      response.append(",\"detail\":");
      append_json_string(response, description->view());
    }
    response.append(",\"textEdit\":{\"range\":");
    append_protocol_range(response, *document, result.token_start,
                          result.token_end, m_encoding);
    response.append(",\"newText\":");
    append_json_string(response, candidate.view());
    response.append("}}");
  }
  response.push(']');

  return send_result(id, response.view());
}

fn Server::code_actions(const JsonValue *id, const JsonValue *params) throws
    -> bool
{
  if (!m_supports_code_action_literals) return send_result(id, "[]");
  let const *text_document =
      params != nullptr ? params->get("textDocument") : nullptr;
  let const uri = string_field(text_document, "uri");
  if (!uri.has_value()) return send_result(id, "[]");
  let *document = find_document(*uri);
  if (document == nullptr) return send_result(id, "[]");

  usize range_start = 0;
  usize range_end = document->normalized_source.count();
  if (let const *range = params != nullptr ? params->get("range") : nullptr;
      range != nullptr)
  {
    let const start = document_position(range->get("start"));
    let const end = document_position(range->get("end"));
    if (!start.has_value() || !end.has_value()) return send_result(id, "[]");
    let const start_byte =
        document->byte_position(start->line, start->character, m_encoding);
    let const end_byte =
        document->byte_position(end->line, end->character, m_encoding);
    if (!start_byte.has_value() || !end_byte.has_value())
      return send_result(id, "[]");
    range_start = *start_byte;
    range_end = *end_byte;
    if (range_end < range_start) return send_result(id, "[]");
  }

  bool wants_quick_fixes = true;
  bool wants_fix_all = true;
  let const *context = params != nullptr ? params->get("context") : nullptr;
  let const *only = context != nullptr ? context->get("only") : nullptr;
  if (only != nullptr && only->kind == json_kind::Array) {
    wants_quick_fixes = false;
    wants_fix_all = false;
    for (let const *kind : only->array) {
      if (kind->kind != json_kind::String) continue;
      if (kind->text == "quickfix") wants_quick_fixes = true;
      if (FIX_ALL_KINDS.contains(kind->text)) wants_fix_all = true;
    }
  }

  let response = String{"["};
  bool is_first_action = true;
  let const diagnostic_belongs_to_document =
      [&](const source_diagnostic &diagnostic) wontthrow -> bool {
    if (diagnostic.source_name.is_empty()) return true;
    if (document->path.has_value())
      return diagnostic.source_name == document->path->text();
    return diagnostic.source_name == document->uri;
  };
  let const do_append_edit = [&](String &output, const source_edit &edit)
                                 throws -> void {
    output.append("{\"range\":");
    append_protocol_range(output, *document, edit.start, edit.end, m_encoding);
    output.append(",\"newText\":");
    append_json_string(output, edit.replacement.view());
    output.push('}');
  };
  let const do_append_workspace_edit =
      [&](String &output, const ArrayList<const source_edit *> &edits)
          throws -> void {
    output.append("\"edit\":{");
    if (m_supports_document_changes) {
      output.append("\"documentChanges\":[{\"textDocument\":{\"uri\":");
      append_json_string(output, document->uri.view());
      output.append(",\"version\":");
      output.append(String::from(document->version, heap_allocator()).view());
      output.append("},\"edits\":[");
    } else {
      output.append("\"changes\":{");
      append_json_string(output, document->uri.view());
      output.append(":[");
    }
    for (usize edit_index = 0; edit_index < edits.count(); edit_index++) {
      if (edit_index != 0) output.push(',');
      do_append_edit(output, *edits[edit_index]);
    }
    output.append(m_supports_document_changes ? "]}]}" : "]}}");
  };
  let const do_append_action = [&](StringView title, StringView kind,
                                   const ArrayList<const source_edit *> &edits,
                                   bool is_preferred) throws -> void {
    if (!is_first_action) response.push(',');
    is_first_action = false;
    response.append("{\"title\":");
    append_json_string(response, title);
    response.append(",\"kind\":");
    append_json_string(response, kind);
    if (is_preferred && m_supports_preferred_actions)
      response.append(",\"isPreferred\":true");
    response.push(',');
    do_append_workspace_edit(response, edits);
    response.push('}');
  };

  if (wants_quick_fixes) {
    for (let const &diagnostic : document->diagnostics) {
      if (!diagnostic_belongs_to_document(diagnostic)) continue;
      let const diagnostic_end =
          diagnostic.location.position + diagnostic.location.length;
      if (range_start == range_end) {
        if (range_start < diagnostic.location.position ||
            range_start > diagnostic_end)
          continue;
      } else if (diagnostic_end <= range_start ||
                 diagnostic.location.position >= range_end)
      {
        continue;
      }
      for (let const &fix : diagnostic.fixes) {
        let edits = ArrayList<const source_edit *>{heap_allocator()};
        for (let const &edit : fix.edits)
          edits.push(&edit);
        do_append_action(fix.title.view(), "quickfix", edits, fix.is_preferred);
      }
    }
  }

  if (wants_fix_all) {
    let candidates = ArrayList<const source_edit *>{heap_allocator()};
    for (let const &diagnostic : document->diagnostics) {
      if (!diagnostic_belongs_to_document(diagnostic)) continue;
      for (let const &fix : diagnostic.fixes) {
        if (!fix.is_safe_for_fix_all) continue;
        for (let const &edit : fix.edits)
          candidates.push(&edit);
      }
    }
    let const nonconflicting =
        select_nonconflicting_source_edits(steal(candidates));
    if (!nonconflicting.is_empty())
      do_append_action("Fix all safe kosh diagnostics", "source.fixAll.kosh",
                       nonconflicting, true);
  }
  response.push(']');

  return send_result(id, response.view());
}

fn Server::symbol_at(Document &document, protocol_position position) throws
    -> Maybe<document_symbol>
{
  let const byte_position =
      document.byte_position(position.line, position.character, m_encoding);
  if (!byte_position.has_value() ||
      position.line >= document.line_starts.count())
    return None;
  let const line_start = document.line_starts[position.line];
  let line_end = document.normalized_source.count();
  if (position.line + 1 < document.line_starts.count())
    line_end = document.line_starts[position.line + 1] - 1;
  let cache = completion::shell_highlight_cache{};
  let const *spans = cache.spans_for(document.normalized_source.view(),
                                     line_start, line_end, m_context);

  for (let const &span : *spans) {
    let const start = line_start + span.start;
    let const end = line_start + span.end;
    if (*byte_position < start || *byte_position > end) continue;

    return document_symbol{
        String{
            document.normalized_source.substring_of_length(start, end - start)},
        span.role, start, end};
  }

  return None;
}

pure fn variable_name_of(StringView text) wontthrow -> StringView
{
  usize start = 0;
  if (!text.is_empty() && text[0] == '$') start++;
  if (start < text.length && text[start] == '{') start++;
  let end = start;

  while (end < text.length) {
    let const byte = text[end];
    if (!((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
          (byte >= '0' && byte <= '9') || byte == '_'))
      break;
    end++;
  }

  return text.substring_of_length(start, end - start);
}

fn Server::definition_of(Document &document,
                         const document_symbol &symbol) throws
    -> Maybe<document_symbol>
{
  let const is_variable = symbol.role == highlight_role::variable ||
                          symbol.role == highlight_role::unset_variable ||
                          symbol.role == highlight_role::assignment_name;
  let const is_function = symbol.role == highlight_role::function_name ||
                          symbol.role == highlight_role::resolved_command ||
                          symbol.role == highlight_role::unknown_command;
  if (!is_variable && !is_function) return None;
  let const name =
      is_variable ? variable_name_of(symbol.text.view()) : symbol.text.view();
  if (name.is_empty()) return None;
  let cache = completion::shell_highlight_cache{};
  let result = Maybe<document_symbol>{};

  for (usize line = 0; line < document.line_starts.count(); line++) {
    let const line_start = document.line_starts[line];
    let line_end = document.normalized_source.count();
    if (line + 1 < document.line_starts.count())
      line_end = document.line_starts[line + 1] - 1;
    let const *spans = cache.spans_for(document.normalized_source.view(),
                                       line_start, line_end, m_context);

    for (let const &span : *spans) {
      let const role_matches =
          is_variable ? span.role == highlight_role::assignment_name
                      : span.role == highlight_role::function_name;
      if (!role_matches) continue;
      let const start = line_start + span.start;
      let const end = line_start + span.end;
      let const candidate =
          document.normalized_source.substring_of_length(start, end - start);
      if (candidate != name) continue;
      if (start > symbol.start) return result;

      result = document_symbol{String{candidate}, span.role, start, end};
    }
  }

  return result;
}

fn Server::definition(const JsonValue *id, const JsonValue *params) throws
    -> bool
{
  let const *text_document =
      params != nullptr ? params->get("textDocument") : nullptr;
  let const uri = string_field(text_document, "uri");
  let const position =
      document_position(params != nullptr ? params->get("position") : nullptr);
  if (!uri.has_value() || !position.has_value()) return send_result(id, "null");
  let *document = find_document(*uri);
  if (document == nullptr) return send_result(id, "null");
  select_document_mood(*document);
  let const symbol = symbol_at(*document, *position);
  if (!symbol.has_value()) return send_result(id, "null");
  let const target = definition_of(*document, *symbol);
  if (!target.has_value()) return send_result(id, "null");
  let response = String{"{\"uri\":"};
  append_json_string(response, document->uri.view());
  response.append(",\"range\":");
  append_protocol_range(response, *document, target->start, target->end,
                        m_encoding);
  response.push('}');

  return send_result(id, response.view());
}

fn Server::command_information(StringView command) throws -> Maybe<String>
{
  static constexpr u64 INFORMATION_TIMEOUT_NANOS = 1'000'000'000;
  let source = String{heap_allocator()};
  if (search_builtin(command).has_value()) {
    source.append("help ");
    source.append(command);
  } else if (koshkit::find_util(command).has_value()) {
    source.append("koshkit ");
    source.append(command);
    source.append(" --help");
  }
  if (!source.is_empty()) {
    let argv = ArrayList<String>{heap_allocator()};
    argv.push(String{m_context.shell_executable_path()});
    argv.push(String{"--clean"});
    argv.push(String{"-c"});
    argv.push(steal(source));
    let const output =
        os::capture_program_output(argv, INFORMATION_TIMEOUT_NANOS);
    if (output.has_value() && !output->is_empty()) return output;
    return None;
  }

  let const paths = m_context.get_program_resolver().search(
      command, ProgramResolver::SearchMode::First,
      ProgramResolver::Requirement::Runnable,
      ProgramResolver::CachePolicy::Bypass);
  if (paths.is_empty()) return None;
  let information = String{heap_allocator()};
  let const man_paths = m_context.get_program_resolver().search(
      "man", ProgramResolver::SearchMode::First,
      ProgramResolver::Requirement::Runnable,
      ProgramResolver::CachePolicy::Bypass);
  if (!man_paths.is_empty() &&
      os::directory_is_trusted_for_exec(man_paths[0].parent()))
  {
    let locate_argv = ArrayList<String>{heap_allocator()};
    locate_argv.push(String{man_paths[0].text().view()});
    locate_argv.push(String{"-w"});
    locate_argv.push(String{command});
    let const location =
        os::capture_program_output(locate_argv, INFORMATION_TIMEOUT_NANOS);
    if (location.has_value() &&
        location->view().find_character('/').has_value())
    {
      let man_argv = ArrayList<String>{heap_allocator()};
      man_argv.push(String{man_paths[0].text().view()});
      man_argv.push(String{command});
      if (let page =
              os::capture_program_output(man_argv, INFORMATION_TIMEOUT_NANOS);
          page.has_value() && !page->is_empty())
      {
        for (usize position = 0; position < page->length(); position++) {
          let const byte = page->view()[position];
          if (byte == '\b') {
            if (!information.is_empty()) information.pop_back();
            continue;
          }
          information.push(byte);
        }
      }
    }
  }
  if (information.is_empty()) {
    if (let const help_argument = completion::HELP_ALLOWLIST.find(command);
        help_argument.has_value() &&
        os::directory_is_trusted_for_exec(paths[0].parent()))
    {
      let help_argv = ArrayList<String>{heap_allocator()};
      help_argv.push(String{paths[0].text().view()});
      let argument = StringView{*help_argument};
      usize argument_start = 0;

      while (argument_start < argument.length) {
        while (argument_start < argument.length &&
               argument[argument_start] == ' ')
          argument_start++;
        let argument_end = argument_start;
        while (argument_end < argument.length && argument[argument_end] != ' ')
          argument_end++;
        if (argument_end > argument_start)
          help_argv.push(String{argument.substring_of_length(
              argument_start, argument_end - argument_start)});
        argument_start = argument_end;
      }
      if (let help =
              os::capture_program_output(help_argv, INFORMATION_TIMEOUT_NANOS);
          help.has_value() && !help->is_empty())
        information = help.take();
    }
  }
  if (!information.is_empty() &&
      information.view()[information.length() - 1] != '\n')
    information.push('\n');
  if (!information.is_empty()) information.push('\n');
  information.append("Path: ");
  information.append(paths[0].text().view());

  return information;
}

fn Server::hover(const JsonValue *id, const JsonValue *params) throws -> bool
{
  let const *text_document =
      params != nullptr ? params->get("textDocument") : nullptr;
  let const uri = string_field(text_document, "uri");
  let const position =
      document_position(params != nullptr ? params->get("position") : nullptr);
  if (!uri.has_value() || !position.has_value()) return send_result(id, "null");
  let *document = find_document(*uri);
  if (document == nullptr) return send_result(id, "null");
  select_document_mood(*document);
  let const symbol = symbol_at(*document, *position);
  if (!symbol.has_value()) return send_result(id, "null");
  let const is_command = symbol->role == highlight_role::resolved_command ||
                         symbol->role == highlight_role::partial_command ||
                         symbol->role == highlight_role::unknown_command;
  if (!is_command) return send_result(id, "null");
  if (definition_of(*document, *symbol).has_value())
    return send_result(id, "null");
  let const information = command_information(symbol->text.view());
  if (!information.has_value()) return send_result(id, "null");
  let response = String{"{\"contents\":{\"kind\":\"plaintext\",\"value\":"};
  append_json_string(response, information->view());
  response.append("},\"range\":");
  append_protocol_range(response, *document, symbol->start, symbol->end,
                        m_encoding);
  response.push('}');

  return send_result(id, response.view());
}

fn Server::semantic_tokens(const JsonValue *id, const JsonValue *params) throws
    -> bool
{
  let const *text_document =
      params != nullptr ? params->get("textDocument") : nullptr;
  let const uri = string_field(text_document, "uri");
  if (!uri.has_value()) return send_result(id, "{\"data\":[]}");
  let *document = find_document(*uri);
  if (document == nullptr) return send_result(id, "{\"data\":[]}");
  select_document_mood(*document);
  let cache = completion::shell_highlight_cache{};
  let response = String{"{\"data\":["};
  usize previous_line = 0;
  usize previous_character = 0;
  bool is_first = true;

  for (usize line = 0; line < document->line_starts.count(); line++) {
    let const line_start = document->line_starts[line];
    let line_end = document->normalized_source.count();
    if (line + 1 < document->line_starts.count())
      line_end = document->line_starts[line + 1] - 1;
    let const *spans = cache.spans_for(document->normalized_source.view(),
                                       line_start, line_end, m_context);
    for (let const &span : *spans) {
      let const absolute_start = line_start + span.start;
      let const absolute_end = line_start + span.end;
      let const start =
          document->protocol_position_at(absolute_start, m_encoding);
      let const delta_line = start.line - previous_line;
      let const delta_character = delta_line == 0
                                      ? start.character - previous_character
                                      : start.character;
      let const length =
          document->encoded_length(absolute_start, absolute_end, m_encoding);
      if (length == 0) continue;
      let const[type, modifiers] = semantic_style(span.role);
      if (!is_first) response.push(',');
      is_first = false;
      response.append(String::from(delta_line, heap_allocator()).view());
      response.push(',');
      response.append(String::from(delta_character, heap_allocator()).view());
      response.push(',');
      response.append(String::from(length, heap_allocator()).view());
      response.push(',');
      response.append(String::from(type, heap_allocator()).view());
      response.push(',');
      response.append(String::from(modifiers, heap_allocator()).view());
      previous_line = start.line;
      previous_character = start.character;
    }
  }
  response.append("]}");

  return send_result(id, response.view());
}

fn Server::dispatch(const JsonValue &message) throws -> bool
{
  if (message.kind != json_kind::Object) return true;
  let const *id = message.get("id");
  let const method = string_field(&message, "method");
  let const *params = message.get("params");
  if (!method.has_value()) {
    if (id != nullptr) return send_error(id, -32600, "Invalid Request");
    return true;
  }
  if (*method == "initialize") return initialize(id, params);
  if (*method == "initialized") return true;
  if (*method == "shutdown") {
    m_is_shutdown = true;
    return send_result(id, "null");
  }
  if (*method == "exit") return false;
  if (!m_is_initialized) {
    if (id != nullptr) return send_error(id, -32002, "Server not initialized");
    return true;
  }
  if (m_is_shutdown) {
    if (id != nullptr) return send_error(id, -32600, "Server has shut down");
    return true;
  }
  if (*method == "textDocument/didOpen") {
    open_document(params);
    return validate_all();
  }
  if (*method == "textDocument/didChange") {
    change_document(params);
    return validate_all();
  }
  if (*method == "textDocument/didClose") {
    close_document(params);
    return validate_all();
  }
  if (*method == "textDocument/completion") return complete(id, params);
  if (*method == "textDocument/codeAction") return code_actions(id, params);
  if (*method == "textDocument/definition") return definition(id, params);
  if (*method == "textDocument/hover") return hover(id, params);
  if (*method == "textDocument/semanticTokens/full")
    return semantic_tokens(id, params);
  if (id != nullptr) return send_error(id, -32601, "Method not found");

  return true;
}

fn Server::run() throws -> int
{
  let reader = ProtocolReader{};

  loop
  {
    let message = reader.read_message();
    if (!message.has_value()) return m_is_shutdown ? 0 : 1;
    let parser = JsonParser{message->view()};
    let *root = parser.parse();
    if (root == nullptr) {
      if (!send_error(nullptr, -32700, "Parse error")) return 1;
      continue;
    }
    if (!dispatch(*root)) return m_is_shutdown ? 0 : 1;
  }
}

} /* namespace */

fn run(EvalContext &context, BumpArena &ast_arena) throws -> int
{
  let server = Server{context, ast_arena};

  return server.run();
}

} /* namespace koshka::language_server */
