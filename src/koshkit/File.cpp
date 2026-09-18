/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements the file utility. It parses magic databases, inspects
 * file types and contents, controls symbolic-link traversal, and formats
 * matching descriptions.
 */

#include "../Bitset.hpp"
#include "../CLI.hpp"
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

static fn parse_magic_number(StringView text, u64 &value) wontthrow -> bool
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
    if (byte != '\\') {
      let const remaining = encoded.substring(position);
      let const next_escape = remaining.find_character('\\');
      let const run_length =
          next_escape.has_value() ? *next_escape : remaining.length;

      decoded.append(remaining.substring_of_length(0, run_length));
      position += run_length - 1;
      continue;
    }

    if (position + 1 == encoded.length) {
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

static fn next_magic_field(StringView line, usize &position) wontthrow
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
  switch (text[0]) {
  case 's':
    if (text.starts_with("string")) {
      rule.kind = file_magic_kind::String;
      position = 6;
    } else if (text.starts_with("short")) {
      rule.kind = file_magic_kind::Signed;
      rule.byte_count = 2;
      position = 5;
    } else {
      rule.kind = file_magic_kind::String;
      position = 1;
    }
    break;

  case 'b':
    if (!text.starts_with("byte")) return false;

    rule.kind = file_magic_kind::Signed;
    rule.byte_count = 1;
    position = 4;
    break;

  case 'l':
    if (!text.starts_with("long")) return false;

    rule.kind = file_magic_kind::Signed;
    rule.byte_count = 4;
    position = 4;
    break;

  case 'd':
  case 'u': {
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
  } break;

  default: return false;
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

static fn read_magic_number(const file_magic_rule &rule, StringView bytes,
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

static fn magic_rule_matches(const file_magic_rule &rule, StringView bytes,
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
      formatted.uppercase_ascii();
      output += formatted.view();
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

struct builtin_file_signature
{
  usize offset;
  StringView bytes;
  StringView description;
};

template <usize byte_count>
consteval fn make_builtin_file_signature(usize offset,
                                         const char (&bytes)[byte_count],
                                         StringView description)
    -> builtin_file_signature
{
  return {
      offset, StringView{bytes, byte_count - 1},
       description
  };
}

static pure fn matches_builtin_file_signature(
    StringView bytes, const builtin_file_signature &signature) wontthrow -> bool
{
  if (signature.offset > bytes.length ||
      signature.bytes.length > bytes.length - signature.offset)
    return false;

  if (signature.bytes.is_empty()) return true;

  let const candidate = bytes.data + signature.offset;
  if (candidate[0] != signature.bytes[0]) return false;

  return signature.bytes.length == 1 ||
         byte_scan::are_bytes_equal(candidate + 1, signature.bytes.data + 1,
                                    signature.bytes.length - 1);
}

static pure fn file_content_description(StringView bytes) wontthrow
    -> StringView
{
  if (bytes.is_empty()) return "empty";

  if (bytes.length >= 0x40 && bytes.starts_with("MZ")) {
    let const pe_offset =
        static_cast<usize>(static_cast<u8>(bytes[0x3c])) |
        static_cast<usize>(static_cast<u8>(bytes[0x3d])) << 8 |
        static_cast<usize>(static_cast<u8>(bytes[0x3e])) << 16 |
        static_cast<usize>(static_cast<u8>(bytes[0x3f])) << 24;
    if (pe_offset <= bytes.length && 4 <= bytes.length - pe_offset &&
        bytes.substring_of_length(pe_offset, 4) == StringView{"PE\0\0", 4})
      return "PE executable";
  }

  if (bytes.length >= 8 &&
      bytes.substring_of_length(0, 4) == StringView{"\xca\xfe\xba\xbe", 4})
  {
    let const major_version = static_cast<u16>(static_cast<u8>(bytes[6]) << 8) |
                              static_cast<u8>(bytes[7]);
    return major_version >= 45 ? StringView{"Java class"}
                               : StringView{"Mach-O universal binary"};
  }

  if (bytes.length >= 12 && bytes.substring_of_length(0, 4) == "RIFF") {
    let const kind = bytes.substring_of_length(8, 4);
    if (kind == "WAVE") return "WAVE audio";
    if (kind == "AVI ") return "AVI video";
    if (kind == "WEBP") return "WebP image";
    if (kind == "ACON") return "Windows animated cursor";
  }

  if (bytes.length >= 12 && bytes.substring_of_length(0, 4) == "FORM") {
    let const kind = bytes.substring_of_length(8, 4);
    if (kind == "AIFF" || kind == "AIFC") return "AIFF audio";
  }

  if (bytes.length >= 16 && bytes.substring_of_length(0, 8) == "AT&TFORM") {
    let const kind = bytes.substring_of_length(12, 4);
    if (kind == "DJVU" || kind == "DJVM" || kind == "DJVI" || kind == "THUM")
      return "DjVu document";
  }

  if (bytes.length >= 12 && bytes.substring_of_length(4, 4) == "ftyp") {
    let const brand = bytes.substring_of_length(8, 4);
    if (brand == "avif" || brand == "avis") return "AVIF image";
    if (brand == "heic" || brand == "heix" || brand == "hevc" ||
        brand == "hevx" || brand == "mif1" || brand == "msf1")
      return "HEIF image";
  }

  if (bytes.length >= 4 &&
      bytes.substring_of_length(0, 4) == StringView{"\x1a\x45\xdf\xa3", 4})
  {
    let const header_length = bytes.length < 256 ? bytes.length : 256;
    let const header = bytes.substring_of_length(4, header_length - 4);
    if (header.find_substring("matroska").has_value()) return "Matroska video";
    if (header.find_substring("webm").has_value()) return "WebM video";
  }

  if (bytes.length > 376 && bytes[0] == 'G' && bytes[188] == 'G' &&
      bytes[376] == 'G')
    return "MPEG transport stream";

  constexpr builtin_file_signature SIGNATURES[] = {
      make_builtin_file_signature(0, "\x89PNG\r\n\x1a\n", "PNG image"),
      make_builtin_file_signature(0, "\xff\xd8\xff", "JPEG image"),
      make_builtin_file_signature(0, "GIF87a", "GIF image"),
      make_builtin_file_signature(0, "GIF89a", "GIF image"),
      make_builtin_file_signature(0, "BM", "BMP image"),
      make_builtin_file_signature(0, "II\x2a\0", "TIFF image"),
      make_builtin_file_signature(0, "MM\0\x2a", "TIFF image"),
      make_builtin_file_signature(0, "II\x2b\0", "BigTIFF image"),
      make_builtin_file_signature(0, "MM\0\x2b", "BigTIFF image"),
      make_builtin_file_signature(0, "\xff\x0a", "JPEG XL image"),
      make_builtin_file_signature(0, "\xff\x4f\xff\x51", "JPEG 2000 image"),
      make_builtin_file_signature(0, "\0\0\0\x0cJXL \r\n\x87\n",
                                  "JPEG XL image"),
      make_builtin_file_signature(0, "\0\0\0\x0cjP  \r\n\x87\n",
                                  "JPEG 2000 image"),
      make_builtin_file_signature(0, "\0\0\x01\0", "Windows icon"),
      make_builtin_file_signature(0, "\0\0\x02\0", "Windows cursor"),
      make_builtin_file_signature(0, "8BPS\0\x01", "Photoshop document"),
      make_builtin_file_signature(0, "\x76\x2f\x31\x01", "OpenEXR image"),
      make_builtin_file_signature(0, "#?RADIANCE", "Radiance HDR image"),
      make_builtin_file_signature(0, "qoif", "QOI image"),
      make_builtin_file_signature(0, "DDS ", "DDS image"),
      make_builtin_file_signature(0, "\xabKTX 20\xbb\r\n\x1a\n", "KTX2 image"),
      make_builtin_file_signature(0, "\xabKTX 11\xbb\r\n\x1a\n", "KTX image"),
      make_builtin_file_signature(0, "PVR\x03", "PVR image"),
      make_builtin_file_signature(0, "\x03RVP", "PVR image"),
      make_builtin_file_signature(0, "\x13\xab\xa1\x5c", "ASTC image"),
      make_builtin_file_signature(0, "BPG\xfb", "BPG image"),
      make_builtin_file_signature(0, "FLIF", "FLIF image"),
      make_builtin_file_signature(0, "gimp xcf ", "GIMP XCF image"),
      make_builtin_file_signature(0, "farbfeld", "farbfeld image"),
      make_builtin_file_signature(0, "\x59\xa6\x6a\x95", "Sun raster image"),
      make_builtin_file_signature(0,
                                  "\x7f"
                                  "ELF",
                                  "ELF executable"),
      make_builtin_file_signature(0, "#!AMR-WB\n", "AMR-WB audio"),
      make_builtin_file_signature(0, "#!AMR\n", "AMR audio"),
      make_builtin_file_signature(0, "#!", "script text executable"),
      make_builtin_file_signature(0, "MZ", "DOS executable"),
      make_builtin_file_signature(0, "\xfe\xed\xfa\xce", "Mach-O executable"),
      make_builtin_file_signature(0, "\xce\xfa\xed\xfe", "Mach-O executable"),
      make_builtin_file_signature(0, "\xfe\xed\xfa\xcf", "Mach-O executable"),
      make_builtin_file_signature(0, "\xcf\xfa\xed\xfe", "Mach-O executable"),
      make_builtin_file_signature(0, "\xbe\xba\xfe\xca",
                                  "Mach-O universal binary"),
      make_builtin_file_signature(0, "\xca\xfe\xba\xbf",
                                  "Mach-O universal binary"),
      make_builtin_file_signature(0, "\xbf\xba\xfe\xca",
                                  "Mach-O universal binary"),
      make_builtin_file_signature(0, "\0asm", "WebAssembly module"),
      make_builtin_file_signature(0, "BC\xc0\xde", "LLVM bitcode"),
      make_builtin_file_signature(0, "!<arch>\n", "Unix archive"),
      make_builtin_file_signature(0, "dex\n", "Dalvik executable"),
      make_builtin_file_signature(0, "oat\n", "Android OAT file"),
      make_builtin_file_signature(0, "vdex", "Android VDEX file"),
      make_builtin_file_signature(0, "ANDROID!", "Android boot image"),
      make_builtin_file_signature(0, "VNDRBOOT", "Android vendor boot image"),
      make_builtin_file_signature(0, "\x27\x05\x19\x56", "U-Boot image"),
      make_builtin_file_signature(
          0, "L\0\0\0\x01\x14\x02\0\0\0\0\0\xc0\0\0\0\0\0\0F",
          "Windows shortcut"),
      make_builtin_file_signature(0, "MDMP", "Windows minidump"),
      make_builtin_file_signature(0, "\xac\xed\0\x05", "Java serialization"),
      make_builtin_file_signature(0, "\x1bLua", "Lua bytecode"),
      make_builtin_file_signature(0, "PK\x03\x04", "ZIP archive"),
      make_builtin_file_signature(0, "PK\x05\x06", "ZIP archive"),
      make_builtin_file_signature(0, "PK\x07\x08", "ZIP archive"),
      make_builtin_file_signature(0, "Rar!\x1a\x07", "RAR archive"),
      make_builtin_file_signature(0, "7z\xbc\xaf\x27\x1c", "7-zip archive"),
      make_builtin_file_signature(0, "\x1f\x8b", "gzip compressed data"),
      make_builtin_file_signature(0, "BZh", "bzip2 compressed data"),
      make_builtin_file_signature(0,
                                  "\xfd"
                                  "7zXZ\0",
                                  "XZ compressed data"),
      make_builtin_file_signature(0, "\x28\xb5\x2f\xfd",
                                  "Zstandard compressed data"),
      make_builtin_file_signature(0, "\x04\x22\x4d\x18", "LZ4 frame"),
      make_builtin_file_signature(0, "LZIP", "lzip compressed data"),
      make_builtin_file_signature(0, "\x1f\x9d", "Unix compressed data"),
      make_builtin_file_signature(0, "MSCF", "Microsoft Cabinet archive"),
      make_builtin_file_signature(0, "MSWIM\0\0\0", "Windows Imaging file"),
      make_builtin_file_signature(0, "070701", "cpio archive"),
      make_builtin_file_signature(0, "070702", "cpio archive"),
      make_builtin_file_signature(0, "070707", "cpio archive"),
      make_builtin_file_signature(257, "ustar", "tar archive"),
      make_builtin_file_signature(0, "\xed\xab\xee\xdb", "RPM package"),
      make_builtin_file_signature(0, "hsqs", "SquashFS filesystem"),
      make_builtin_file_signature(0, "\x45\x3d\xcd\x28", "cramfs filesystem"),
      make_builtin_file_signature(0, "xar!", "XAR archive"),
      make_builtin_file_signature(0, "\x60\xea", "ARJ archive"),
      make_builtin_file_signature(7, "**ACE**", "ACE archive"),
      make_builtin_file_signature(2, "-lh", "LHA archive"),
      make_builtin_file_signature(0, "bvx2", "LZFSE compressed data"),
      make_builtin_file_signature(
          0, "7kSt\xa0\x31\x83\xd3\x8c\xb2\x28\xb0\xd3zPQ", "ZPAQ archive"),
      make_builtin_file_signature(0, "\xff\x06\0\0sNaPpY",
                                  "Snappy framed data"),
      make_builtin_file_signature(0, "%PDF-", "PDF document"),
      make_builtin_file_signature(0, "%!PS", "PostScript document"),
      make_builtin_file_signature(0, "{\\rtf", "RTF document"),
      make_builtin_file_signature(0, "<!DOCTYPE html", "HTML document"),
      make_builtin_file_signature(0, "<html", "HTML document"),
      make_builtin_file_signature(0, "<?xml", "XML document"),
      make_builtin_file_signature(0, "SQLite format 3\0", "SQLite database"),
      make_builtin_file_signature(0, "\x37\x7f\x06\x82",
                                  "SQLite write-ahead log"),
      make_builtin_file_signature(0, "\x37\x7f\x06\x83",
                                  "SQLite write-ahead log"),
      make_builtin_file_signature(0, "\xd9\xd5\x05\xf9 \xa1\x63\xd7",
                                  "SQLite rollback journal"),
      make_builtin_file_signature(0, "PAR1", "Apache Parquet data"),
      make_builtin_file_signature(0, "ORC", "Apache ORC data"),
      make_builtin_file_signature(0, "Obj\x01", "Apache Avro object"),
      make_builtin_file_signature(0, "ARROW1\0\0", "Apache Arrow file"),
      make_builtin_file_signature(0, "\x89HDF\r\n\x1a\n", "HDF5 data"),
      make_builtin_file_signature(0, "CDF\x01", "NetCDF data"),
      make_builtin_file_signature(0, "CDF\x02", "NetCDF data"),
      make_builtin_file_signature(0, "CDF\x05", "NetCDF data"),
      make_builtin_file_signature(0, "SIMPLE  =", "FITS data"),
      make_builtin_file_signature(0, "\xa1\xb2\xc3\xd4", "pcap capture"),
      make_builtin_file_signature(0, "\xd4\xc3\xb2\xa1", "pcap capture"),
      make_builtin_file_signature(0, "\xa1\xb2\x3c\x4d", "pcap capture"),
      make_builtin_file_signature(0, "\x4d\x3c\xb2\xa1", "pcap capture"),
      make_builtin_file_signature(0, "\x0a\x0d\x0d\x0a", "pcapng capture"),
      make_builtin_file_signature(0, "REDIS", "Redis database"),
      make_builtin_file_signature(0, "PGDMP", "PostgreSQL custom dump"),
      make_builtin_file_signature(0, "PACK", "Git pack"),
      make_builtin_file_signature(0, "DIRC", "Git index"),
      make_builtin_file_signature(0, "MIDX", "Git multi-pack index"),
      make_builtin_file_signature(0, "CGPH", "Git commit graph"),
      make_builtin_file_signature(128, "DICM", "DICOM medical image"),
      make_builtin_file_signature(0, "\xd0\xcf\x11\xe0\xa1\xb1\x1a\xe1",
                                  "OLE compound document"),
      make_builtin_file_signature(0, "ITSF", "Compiled HTML help"),
      make_builtin_file_signature(0, "fLaC", "FLAC audio"),
      make_builtin_file_signature(0, "OggS", "Ogg data"),
      make_builtin_file_signature(0, "ID3", "MP3 audio"),
      make_builtin_file_signature(0, "MThd\0\0\0\x06", "MIDI audio"),
      make_builtin_file_signature(0, ".snd", "Sun audio"),
      make_builtin_file_signature(0, "MAC ", "Monkey's Audio"),
      make_builtin_file_signature(0, "MPCK", "Musepack audio"),
      make_builtin_file_signature(0, "wvpk", "WavPack audio"),
      make_builtin_file_signature(0, "DSD ", "DSF audio"),
      make_builtin_file_signature(0, "FRM8", "DSDIFF audio"),
      make_builtin_file_signature(0, "Creative Voice File\x1a",
                                  "Creative Voice audio"),
      make_builtin_file_signature(0, "Extended Module: ", "FastTracker module"),
      make_builtin_file_signature(0, "IMPM", "Impulse Tracker module"),
      make_builtin_file_signature(44, "SCRM", "Scream Tracker module"),
      make_builtin_file_signature(1080, "M.K.", "ProTracker module"),
      make_builtin_file_signature(0, "FLV\x01", "Flash video"),
      make_builtin_file_signature(0, "\0\0\x01\xba", "MPEG program stream"),
      make_builtin_file_signature(
          0, "\x30\x26\xb2\x75\x8e\x66\xcf\x11\xa6\xd9\0\xaa\0b\xce\x6c",
          "ASF media"),
      make_builtin_file_signature(0, ".RMF", "RealMedia file"),
      make_builtin_file_signature(0, "FWS", "Flash file"),
      make_builtin_file_signature(0, "CWS", "Flash file"),
      make_builtin_file_signature(0, "ZWS", "Flash file"),
  };
  static_assert(sizeof(SIGNATURES) / sizeof(*SIGNATURES) >= 100);

  for (let const &signature : SIGNATURES) {
    if (matches_builtin_file_signature(bytes, signature)) {
      return signature.description;
    }
  }

  static constexpr u32 TEXT_CONTROL_MASK =
      (1U << '\b') | (1U << '\t') | (1U << '\n') | (1U << '\f') | (1U << '\r');

  bool is_text = true;
  for (usize position = 0; position < bytes.length; position++) {
    let const byte = static_cast<u8>(bytes[position]);
    if (byte < 0x20 && ((TEXT_CONTROL_MASK >> byte) & 1U) == 0) {
      is_text = false;
      break;
    }
  }

  return is_text ? StringView{"text"} : StringView{"data"};
}

constexpr usize FILE_CONTENT_SAMPLE_BYTE_COUNT = 8192;
constexpr usize FILE_CONTENT_SAMPLE_BATCH_COUNT = 32;

fn describe_file_type(StringView path, const os::file_status &status,
                      Allocator allocator) throws -> Maybe<String>
{
  switch (os::file_type_letter(status.mode)) {
  case 'd': return String{allocator, "directory"};
  case 'l': return String{allocator, "symbolic link"};
  case 'p': return String{allocator, "fifo"};
  case 's': return String{allocator, "socket"};
  case 'b': return String{allocator, "block special file"};
  case 'c': return String{allocator, "character special file"};
  default: break;
  }

  let const descriptor =
      os::open_file_descriptor(path, os::file_open_mode::Read);
  if (!descriptor.has_value()) return None;
  defer { os::close_fd(*descriptor); };

  char buffer[FILE_CONTENT_SAMPLE_BYTE_COUNT];
  let const read_count = os::read_fd(*descriptor, buffer, sizeof(buffer));
  if (!read_count.has_value()) return None;

  return String{allocator,
                file_content_description(StringView{buffer, *read_count})};
}

struct file_content_probe
{
  os::descriptor descriptor{KOSH_INVALID_FD};
  usize operand_position{0};
  char bytes[FILE_CONTENT_SAMPLE_BYTE_COUNT]{};
};

File::File() = default;

pure fn File::kind() const wontthrow -> Utility::Kind { return Kind::File; }

fn File::execute(const ExecContext &ec, EvalContext &cxt,
                 const ArrayList<String> &args,
                 const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let const[operands, operand_locations] = parse_util_operands(
      FLAG_LIST, args, cxt.scratch_allocator(), &arg_locations);
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

  let operand_paths = ArrayList<Path>{allocator};
  let file_statuses = ArrayList<os::file_status>{allocator};
  let metadata_batch = os::Batch{allocator};
  operand_paths.reserve(operands.count());
  file_statuses.reserve(operands.count());
  metadata_batch.reserve(operands.count());
  for (let const &operand : operands) {
    operand_paths.push(Path{operand.view()});
    file_statuses.push({});
  }
  for (usize operand_position = 0; operand_position < operands.count();
       operand_position++)
  {
    metadata_batch.add(os::batch_operation::lstat(
        operand_paths[operand_position], file_statuses[operand_position]));
  }
  let const metadata_results = metadata_batch.execute();

  let default_samples = ArrayList<String>{allocator};
  let default_sample_errors = Bitset{allocator};
  let const should_batch_default_samples =
      !FLAG_FILE_REGULAR_ONLY.is_enabled() && magic_rules.is_empty();
  if (should_batch_default_samples) {
    default_samples.reserve(operands.count());
    for (usize operand_position = 0; operand_position < operands.count();
         operand_position++)
    {
      default_samples.push(String{allocator});
    }
    default_sample_errors.reset(operands.count());

    file_content_probe probes[FILE_CONTENT_SAMPLE_BATCH_COUNT]{};
    usize probe_count = 0;
    let content_batch = os::Batch{allocator};
    let content_results = ArrayList<os::batch_result>{allocator};
    content_batch.reserve(FILE_CONTENT_SAMPLE_BATCH_COUNT);
    content_results.reserve(FILE_CONTENT_SAMPLE_BATCH_COUNT);
    defer
    {
      for (usize index = 0; index < probe_count; index++)
        if (probes[index].descriptor != KOSH_INVALID_FD)
          unused(os::close_fd(probes[index].descriptor));
    };
    let const do_flush_probes = [&]() throws -> void {
      content_batch.clear();
      for (usize index = 0; index < probe_count; index++) {
        content_batch.add(os::batch_operation::read(
            probes[index].descriptor, probes[index].bytes,
            FILE_CONTENT_SAMPLE_BYTE_COUNT));
      }
      content_batch.execute(content_results);
      for (usize index = 0; index < probe_count; index++) {
        let const operand_position = probes[index].operand_position;
        let const &result = content_results[index];
        if (result.error_number == 0) {
          default_samples[operand_position] =
              String{allocator,
                     file_content_description(StringView{
                         probes[index].bytes, result.transferred_byte_count})};
        } else {
          os::set_last_system_error(result.error_number);
          default_samples[operand_position] = os::last_system_error_message();
          default_sample_errors.set(operand_position);
        }
        unused(os::close_fd(probes[index].descriptor));
        probes[index].descriptor = KOSH_INVALID_FD;
      }
      probe_count = 0;
    };

    for (usize operand_position = 0; operand_position < operands.count();
         operand_position++)
    {
      if (metadata_results[operand_position].error_number != 0 ||
          os::file_type_letter(file_statuses[operand_position].mode) != '-')
      {
        continue;
      }

      let const descriptor = os::open_file_descriptor(
          operands[operand_position].view(), os::file_open_mode::Read);
      if (!descriptor.has_value()) {
        default_samples[operand_position] = os::last_system_error_message();
        default_sample_errors.set(operand_position);
        continue;
      }

      probes[probe_count].descriptor = *descriptor;
      probes[probe_count].operand_position = operand_position;
      probe_count++;
      if (probe_count == FILE_CONTENT_SAMPLE_BATCH_COUNT) do_flush_probes();
    }
    if (probe_count != 0) do_flush_probes();
  }

  for (usize operand_position = 0; operand_position < operands.count();
       operand_position++)
  {
    let const &operand = operands[operand_position];
    let &file_status = file_statuses[operand_position];
    if (metadata_results[operand_position].error_number != 0) {
      os::set_last_system_error(
          metadata_results[operand_position].error_number);
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
    let const is_regular = os::file_type_letter(file_status.mode) == '-';
    if (is_regular && FLAG_FILE_REGULAR_ONLY.is_enabled()) {
      description += "regular file";
    } else if (is_regular && magic_rules.is_empty() &&
               !default_samples[operand_position].is_empty())
    {
      if (default_sample_errors[operand_position]) {
        report_soft_koshkit_util_error(
            ec, cxt, operand_locations[operand_position], args[0].view(),
            "cannot read '" + operand +
                "': " + default_samples[operand_position]);
        status = 1;
        continue;
      }
      description += default_samples[operand_position].view();
    } else if (!is_regular || magic_rules.is_empty()) {
      let const default_description =
          describe_file_type(operand.view(), file_status, allocator);
      if (!default_description.has_value()) {
        report_soft_koshkit_util_error(
            ec, cxt, operand_locations[operand_position], args[0].view(),
            "cannot read '" + operand +
                "': " + os::last_system_error_message());
        status = 1;
        continue;
      }
      description += default_description->view();
    } else {
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
      } else if (let const magic_description = match_magic_rules(
                     magic_rules, contents->view(), allocator);
                 magic_description.has_value())
      {
        description += magic_description->view();
      } else if (should_apply_default_tests) {
        description += file_content_description(contents->view());
      } else {
        description += "data";
      }
    }

    ec.print_to_stdout(operand + ": " + description + "\n");
  }

  return status;
}

} // namespace koshka::koshkit
