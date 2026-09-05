/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements numeric execution helpers. It provides shared
 * low-level operations for commands, streams, numbers, globbing, and
 * resolution without duplicating policy owners.
 */

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
#include "Utils.hpp"

namespace koshka {

namespace utils {

/* Turn an accumulated magnitude and sign into a saturating signed result. The
   per-base parsers share this so only the digit loop stays base-specific. */
static pure fn saturate_signed_magnitude(u64 magnitude, bool is_negative,
                                         bool has_overflowed) wontthrow -> i64
{
  if (is_negative) {
    if (has_overflowed || magnitude > static_cast<u64>(INT64_MAX) + 1) {
      return INT64_MIN;
    }
    return static_cast<i64>(~magnitude + 1u);
  }
  if (has_overflowed || magnitude > static_cast<u64>(INT64_MAX)) {
    return INT64_MAX;
  }
  return static_cast<i64>(magnitude);
}

static fn not_an_integer_error(StringView text) throws -> Error
{
  return Error{"'" + text + "' is not a valid integer"};
}

fn int_to_text_into(i64 value, char *buffer, usize buffer_size) wontthrow
    -> StringView
{
  /* The digits are written from the least significant end of the buffer, the
     same scheme String::from uses, then a leading minus is prepended. A u64
     never needs more than twenty digits, so twenty-one bytes hold any i64. */
  ASSERT(buffer_size >= 21, "the buffer must hold a sign and twenty digits");
  let const is_negative = value < 0;
  u64 magnitude =
      is_negative ? ~static_cast<u64>(value) + 1 : static_cast<u64>(value);
  usize offset = buffer_size;
  do {
    buffer[--offset] = static_cast<char>('0' + magnitude % 10);
    magnitude /= 10;
  } while (magnitude > 0);
  if (is_negative) buffer[--offset] = '-';
  return StringView{buffer + offset, buffer_size - offset};
}

fn uint_to_text_into(u64 value, char *buffer, usize buffer_size) wontthrow
    -> StringView
{
  ASSERT(buffer_size >= 20, "the buffer must hold twenty digits");
  usize offset = buffer_size;
  do {
    buffer[--offset] = static_cast<char>('0' + value % 10);
    value /= 10;
  } while (value > 0);

  return StringView{buffer + offset, buffer_size - offset};
}

fn format_minutes_seconds(double seconds) throws -> String
{
  /* An rusage subtraction can go backwards, a negative clamps to zero to avoid
     a doubled sign like -0m-0.001s. */
  if (seconds < 0.0) seconds = 0.0;
  const i64 minutes = static_cast<i64>(seconds) / 60;
  const double remainder = seconds - static_cast<double>(minutes * 60);
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%ldm%.3fs", static_cast<long>(minutes),
                remainder);
  return String{buffer};
}

static fn format_time_report_posix(double real_seconds, double user_seconds,
                                   double system_seconds) throws -> String
{
  char buffer[64];
  let report = String{heap_allocator()};
  std::snprintf(buffer, sizeof(buffer), "real %.2f\n",
                real_seconds < 0.0 ? 0.0 : real_seconds);
  report += buffer;
  std::snprintf(buffer, sizeof(buffer), "user %.2f\n",
                user_seconds < 0.0 ? 0.0 : user_seconds);
  report += buffer;
  std::snprintf(buffer, sizeof(buffer), "sys %.2f\n",
                system_seconds < 0.0 ? 0.0 : system_seconds);
  report += buffer;
  return report;
}

static fn format_time_report_pretty(double real_seconds, double user_seconds,
                                    double system_seconds) throws -> String
{
  const double cpu_percent =
      real_seconds > 0.0
          ? (user_seconds + system_seconds) / real_seconds * 100.0
          : 0.0;
  char buffer[64];
  let report = String{heap_allocator()};
  report += "\n";
  report += "  real   " + format_minutes_seconds(real_seconds) + "\n";
  report += "  user   " + format_minutes_seconds(user_seconds) + "\n";
  report += "  sys    " + format_minutes_seconds(system_seconds) + "\n";
  std::snprintf(buffer, sizeof(buffer), "  cpu    %.0f%%\n", cpu_percent);
  report += buffer;

  return report;
}

static fn format_time_report_custom(StringView format, double real_seconds,
                                    double user_seconds,
                                    double system_seconds) throws -> String
{
  let report = String{heap_allocator()};

  for (usize i = 0; i < format.length; i++) {
    if (format[i] != '%') {
      report.push(format[i]);
      continue;
    }

    i++;
    if (i >= format.length) {
      report.push('%');
      break;
    }
    if (format[i] == '%') {
      report.push('%');
      continue;
    }

    /* A precision digit and the l flag may precede the conversion, %3lR is
       three digits in minutes form, precision clamped to six. */
    usize precision = 3;
    if (format[i] >= '0' && format[i] <= '9') {
      precision = static_cast<usize>(format[i] - '0');
      if (precision > 6) precision = 6;
      i++;
    }

    bool is_long_format = false;
    if (i < format.length && format[i] == 'l') {
      is_long_format = true;
      i++;
    }

    if (i >= format.length) {
      report.push('%');
      break;
    }

    char buffer[64];
    let const code = format[i];

    double value = 0.0;
    switch (code) {
    case 'R': value = real_seconds; break;
    case 'U': value = user_seconds; break;
    case 'S': value = system_seconds; break;

    case 'P': {
      const double cpu_percent =
          real_seconds > 0.0
              ? (user_seconds + system_seconds) / real_seconds * 100.0
              : 0.0;
      std::snprintf(buffer, sizeof(buffer), "%.2f", cpu_percent);
      report += buffer;
      continue;
    }

    default:
      report.push('%');
      report.push(code);
      continue;
    }

    if (value < 0.0) value = 0.0;

    if (is_long_format) {
      const i64 minutes = static_cast<i64>(value) / 60;
      const double remainder = value - static_cast<double>(minutes * 60);
      std::snprintf(buffer, sizeof(buffer), "%ldm%.*fs",
                    static_cast<long>(minutes), static_cast<int>(precision),
                    remainder);
    } else {
      std::snprintf(buffer, sizeof(buffer), "%.*f", static_cast<int>(precision),
                    value);
    }
    report += buffer;
  }

  report.push('\n');
  return report;
}

fn format_time_report(bool should_use_posix_format, bool should_report_rss,
                      const Maybe<String> &time_format, double real_seconds,
                      double user_seconds, double system_seconds,
                      u64 peak_rss_bytes) throws -> String
{
  let report = String{heap_allocator()};
  let const should_use_pretty_format =
      !should_use_posix_format && !time_format.has_value();

  if (should_use_posix_format) {
    report =
        format_time_report_posix(real_seconds, user_seconds, system_seconds);
  } else if (should_use_pretty_format) {
    report =
        format_time_report_pretty(real_seconds, user_seconds, system_seconds);
  } else if (!time_format->is_empty()) {
    report = format_time_report_custom(time_format->view(), real_seconds,
                                       user_seconds, system_seconds);
  }

  if (should_report_rss || (should_use_pretty_format && peak_rss_bytes > 0)) {
    report += "  rss    " +
              koshkit::format_human_size(peak_rss_bytes, heap_allocator()) +
              "\n";
  }

  return report;
}

/* A newline offset table cached on one source, keyed on the source pointer and
   length, so a $LINENO lookup is a binary search over the newlines. */
class LineNumberCache
{
public:
  LineNumberCache() : m_newline_offsets(heap_allocator()) {}

  fn ensure_built_for(StringView source) throws -> void
  {
    if (m_source_data == source.data && m_source_length == source.count()) {
      return;
    }

    m_source_data = source.data;
    m_source_length = source.count();
    m_newline_offsets.clear();

    /* An offset is 32-bit, matching every other source offset the shell
       carries, so a source beyond four gigabytes is indexed up to that point
       and everything past it reads as the last line. */
    let const indexable_length =
        source.count() < UINT32_MAX ? source.count() : usize{UINT32_MAX};
    let const indexable = source.substring_of_length(0, indexable_length);

    /* The count pass is one memchr sweep and it makes the offset table an
       exact allocation, where geometric growth would leave up to half the
       block unused on a source with hundreds of thousands of lines. */
    m_newline_offsets.reserve(count_newlines(indexable));

    usize scan_position = 0;
    while (scan_position < indexable_length) {
      let const remaining = indexable.substring(scan_position);
      let const newline = remaining.find_character('\n');
      if (!newline.has_value()) break;
      m_newline_offsets.push(static_cast<u32>(scan_position + *newline));
      scan_position += *newline + 1;
    }
  }

  fn invalidate() wontthrow -> void
  {
    m_source_data = nullptr;
    m_source_length = 0;
    m_newline_offsets.release();
  }

  pure fn locate(usize position) const wontthrow -> source_line_position
  {
    usize low = 0;
    usize high = m_newline_offsets.count();
    while (low < high) {
      const usize mid = low + (high - low) / 2;
      if (m_newline_offsets[mid] < position)
        low = mid + 1;
      else
        high = mid;
    }

    const usize line_start = low == 0 ? 0 : m_newline_offsets[low - 1] + 1;
    const usize line_end = low == m_newline_offsets.count()
                               ? m_source_length
                               : m_newline_offsets[low];
    return source_line_position{low, line_start, line_end};
  }

private:
  static pure fn count_newlines(StringView text) wontthrow -> usize
  {
    usize newline_count = 0;
    usize scan_position = 0;
    while (scan_position < text.count()) {
      let const newline = text.substring(scan_position).find_character('\n');
      if (!newline.has_value()) break;
      newline_count++;
      scan_position += *newline + 1;
    }

    return newline_count;
  }

  const char *m_source_data{nullptr};
  usize m_source_length{0};
  ArrayList<u32> m_newline_offsets;
};

static thread_local LineNumberCache LINE_NUMBER_CACHE{};

fn source_line_position_at(StringView source, usize position) throws
    -> source_line_position
{
  LINE_NUMBER_CACHE.ensure_built_for(source);
  return LINE_NUMBER_CACHE.locate(position);
}

fn line_number_at(StringView source, usize position) throws -> usize
{
  return source_line_position_at(source, position).line_number + 1;
}

fn invalidate_line_number_cache() wontthrow -> void
{
  LINE_NUMBER_CACHE.invalidate();
}

static fn skip_ascii_whitespace(StringView text, usize &offset) wontthrow
    -> void
{
  while (offset < text.length && is_ascii_whitespace(text.data[offset]))
    offset++;
}

struct parsed_integer_magnitude
{
  u64 magnitude;
  bool is_negative;
  bool has_overflowed;
};

static fn parse_magnitude_in_base(StringView text, int_base base) throws
    -> ErrorOr<parsed_integer_magnitude>;

fn parse_decimal_i64(StringView text, bool *out_of_range) throws -> ErrorOr<i64>
{
  let const parsed = TRY(parse_magnitude_in_base(text, int_base::decimal));

  if (out_of_range != nullptr)
    *out_of_range = parsed.has_overflowed ||
                    parsed.magnitude > static_cast<u64>(INT64_MAX) +
                                           (parsed.is_negative ? 1u : 0u);

  return saturate_signed_magnitude(parsed.magnitude, parsed.is_negative,
                                   parsed.has_overflowed);
}

fn parse_decimal_u64(StringView text) throws -> ErrorOr<u64>
{
  let const parsed = TRY(parse_magnitude_in_base(text, int_base::decimal));
  if (parsed.is_negative || parsed.has_overflowed) {
    return Error{"integer value out of range"};
  }
  return parsed.magnitude;
}

fn parse_decimal_f64(const String &text) throws -> ErrorOr<f64>
{
  let const start = text.c_str();
  char *end = nullptr;
  errno = 0;
  let const parsed_value = ::strtold(start, &end);
  if (end == start || end != start + text.length()) {
    return Error{"invalid number"};
  }

  let digits = start;
  while (*digits == ' ' || *digits == '\t' || *digits == '\n' ||
         *digits == '\r' || *digits == '\f' || *digits == '\v')
  {
    digits++;
  }
  if (*digits == '+' || *digits == '-') {
    digits++;
  }
  if (digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X')) {
    return Error{"invalid number"};
  }

  let const magnitude = __builtin_fabsl(parsed_value);
  if (errno == ERANGE &&
      (magnitude == 0.0L || !__builtin_isfinite(parsed_value)))
  {
    return Error{"number value out of range"};
  }
  if (__builtin_isfinite(parsed_value) &&
      magnitude > static_cast<long double>(__DBL_MAX__))
  {
    return Error{"number value out of range"};
  }

  let const narrowed_value = static_cast<f64>(parsed_value);
  if (parsed_value != 0.0L && narrowed_value == 0.0) {
    return Error{"number value out of range"};
  }

  return narrowed_value;
}

fn format_f64(f64 value, Allocator allocator) throws -> String
{
  char buffer[32];
  let const length = ::snprintf(buffer, sizeof(buffer), "%.17g", value);
  if (length < 0 || static_cast<usize>(length) >= sizeof(buffer)) {
    return String{allocator};
  }

  return String{
      allocator, StringView{buffer, static_cast<usize>(length)}
  };
}

fn parse_timeout_seconds_to_nanos(StringView text) throws -> ErrorOr<i64>
{
  usize offset = 0;
  skip_ascii_whitespace(text, offset);

  u64 whole_seconds = 0;
  bool has_overflowed = false;
  bool has_digits = false;
  while (offset < text.length && text.data[offset] >= '0' &&
         text.data[offset] <= '9')
  {
    let const digit = static_cast<u64>(text.data[offset] - '0');
    if (whole_seconds > (UINT64_MAX - digit) / 10)
      has_overflowed = true;
    else
      whole_seconds = whole_seconds * 10 + digit;
    has_digits = true;
    offset++;
  }

  i64 fractional_nanos = 0;
  if (offset < text.length && text.data[offset] == '.') {
    offset++;
    i64 digit_scale = 100'000'000;
    while (offset < text.length && text.data[offset] >= '0' &&
           text.data[offset] <= '9')
    {
      fractional_nanos += (text.data[offset] - '0') * digit_scale;
      digit_scale /= 10;
      has_digits = true;
      offset++;
    }
  }

  skip_ascii_whitespace(text, offset);
  if (!has_digits || offset != text.length) {
    return Error{"'" + text + "' is not a valid timeout"};
  }

  /* A whole-seconds part too large for the signed nanosecond result saturates
     to the maximum rather than overflowing. */
  constexpr u64 max_whole_seconds = INT64_MAX / 1'000'000'000;
  constexpr i64 max_fractional_nanos = INT64_MAX % 1'000'000'000;
  if (has_overflowed || whole_seconds > max_whole_seconds ||
      (whole_seconds == max_whole_seconds &&
       fractional_nanos > max_fractional_nanos))
  {
    return static_cast<i64>(INT64_MAX);
  }

  return static_cast<i64>(whole_seconds) * 1'000'000'000 + fractional_nanos;
}

static pure fn digit_value_in_base(char c, u32 radix) wontthrow -> i32
{
  u32 value;
  if (c >= '0' && c <= '9') {
    value = static_cast<u32>(c - '0');
  } else if (c >= 'a' && c <= 'z')
    value = static_cast<u32>(c - 'a') + 10;
  else if (c >= 'A' && c <= 'Z')
    value = static_cast<u32>(c - 'A') + 10;
  else
    return -1;

  return value < radix ? static_cast<i32>(value) : -1;
}

static fn parse_magnitude_in_base(StringView text, int_base base) throws
    -> ErrorOr<parsed_integer_magnitude>
{
  let const radix = static_cast<u32>(base);
  usize offset = 0;
  skip_ascii_whitespace(text, offset);

  bool is_negative = false;
  if (offset < text.length &&
      (text.data[offset] == '+' || text.data[offset] == '-'))
  {
    is_negative = text.data[offset] == '-';
    offset++;
  }

  if (base == int_base::hex && offset + 1 < text.length &&
      text.data[offset] == '0' &&
      (text.data[offset + 1] == 'x' || text.data[offset + 1] == 'X'))
  {
    offset += 2;
  } else if (base == int_base::binary && offset + 1 < text.length &&
             text.data[offset] == '0' &&
             (text.data[offset + 1] == 'b' || text.data[offset + 1] == 'B'))
  {
    offset += 2;
  }

  u64 magnitude = 0;
  bool has_digits = false;
  bool has_overflowed = false;
  usize digit_count = 0;
  while (offset < text.length) {
    let const digit = digit_value_in_base(text.data[offset], radix);
    if (digit < 0) break;
    has_digits = true;
    digit_count++;
    if (base == int_base::decimal && digit_count <= 19)
      magnitude = magnitude * radix + static_cast<u64>(digit);
    else if (magnitude > (UINT64_MAX - static_cast<u64>(digit)) / radix)
      has_overflowed = true;
    else
      magnitude = magnitude * radix + static_cast<u64>(digit);
    offset++;
  }

  skip_ascii_whitespace(text, offset);
  if (!has_digits || offset != text.length) return not_an_integer_error(text);

  return parsed_integer_magnitude{magnitude, is_negative, has_overflowed};
}

fn parse_integer_in_base(StringView text, int_base base,
                         bool *out_of_range) throws -> ErrorOr<i64>
{
  let const parsed = TRY(parse_magnitude_in_base(text, base));

  if (out_of_range != nullptr)
    *out_of_range = parsed.has_overflowed ||
                    parsed.magnitude > static_cast<u64>(INT64_MAX) +
                                           (parsed.is_negative ? 1u : 0u);

  return saturate_signed_magnitude(parsed.magnitude, parsed.is_negative,
                                   parsed.has_overflowed);
}

fn parse_integer_in_base_u64(StringView text, int_base base) throws
    -> ErrorOr<u64>
{
  let const parsed = TRY(parse_magnitude_in_base(text, base));
  if (parsed.is_negative || parsed.has_overflowed) {
    return Error{"integer value out of range"};
  }
  return parsed.magnitude;
}

fn expand_leading_tilde_path(StringView name) throws -> Maybe<String>
{
  if (name.is_empty() || name[0] != '~') return None;

  let const slash = name.find_character('/');
  let const user = slash.has_value() ? name.substring_of_length(1, *slash - 1)
                                     : name.substring(1);
  let home =
      user.is_empty() ? os::get_home_directory() : os::get_home_for_user(user);
  if (!home.has_value()) return None;

  let expanded = home.take();
  if (slash.has_value()) expanded.push_component(name.substring(*slash + 1));
  return String{expanded.text().view()};
}

fn decode_ansi_c_escapes(String &out, StringView body) throws -> void
{
  usize i = 0;
  while (i < body.length) {
    let const c = body[i];
    i++;

    if (c != '\\') {
      out.push(c);
      continue;
    }
    if (i >= body.length) {
      out.push('\\');
      break;
    }

    let const e = body[i];
    i++;
    switch (e) {
    case 'n': out.push('\n'); break;
    case 't': out.push('\t'); break;
    case 'r': out.push('\r'); break;
    case 'a': out.push('\a'); break;
    case 'b': out.push('\b'); break;
    case 'f': out.push('\f'); break;
    case 'v': out.push('\v'); break;
    case 'e':
    case 'E': out.push('\x1b'); break;
    case '\\': out.push('\\'); break;
    case '\'': out.push('\''); break;
    case '"': out.push('"'); break;
    case '?': out.push('?'); break;
    case 'x': {
      i32 value = 0;
      i32 digit_count = 0;
      while (digit_count < 2 && i < body.length) {
        let const digit = hex_digit_value(body[i]);
        if (!digit.has_value()) break;
        value = value * 16 + *digit;
        i++;
        digit_count++;
      }
      if (digit_count == 0) {
        out.push('\\');
        out.push('x');
      } else {
        out.push(static_cast<char>(value));
      }
    } break;
    case 'c': {
      if (i >= body.length) {
        out.push('\\');
        out.push('c');
        break;
      }
      let const target = body[i];
      i++;
      if (target == '\\' && i < body.length && body[i] == '\\') {
        i++;
      }
      let const upper = (target >= 'a' && target <= 'z')
                            ? static_cast<char>(target - 'a' + 'A')
                            : target;
      const u8 control = upper == '?'
                             ? static_cast<u8>(0x7fu)
                             : static_cast<u8>(static_cast<u8>(upper) & 0x1fu);
      out.push(static_cast<char>(control));
    } break;
    case 'u':
    case 'U': {
      const i32 max_digit_count = e == 'u' ? 4 : 8;
      u32 codepoint = 0;
      i32 digit_count = 0;
      while (digit_count < max_digit_count && i < body.length) {
        let const digit = hex_digit_value(body[i]);
        if (!digit.has_value()) break;
        codepoint = codepoint * 16 + *digit;
        i++;
        digit_count++;
      }
      if (digit_count == 0) {
        out.push('\\');
        out.push(e);
      } else {
        append_utf8(out, codepoint);
      }
    } break;
    default:
      if (e >= '0' && e <= '7') {
        i32 value = e - '0';
        i32 digit_count = 1;
        while (digit_count < 3 && i < body.length && body[i] >= '0' &&
               body[i] <= '7')
        {
          value = value * 8 + (body[i] - '0');
          i++;
          digit_count++;
        }
        out.push(static_cast<char>(value));
      } else {
        out.push('\\');
        out.push(e);
      }
      break;
    }
  }
}

fn append_ansi_c_quote_if_needed(String &out, StringView arg) throws -> bool
{
  if (arg.is_empty()) {
    out += "''";
    return true;
  }

  bool has_control_byte = false;
  for (usize i = 0; i < arg.length; i++) {
    let const byte = static_cast<unsigned char>(arg[i]);
    if (byte < 0x20 || byte == 0x7f) {
      has_control_byte = true;
      break;
    }
  }
  if (!has_control_byte) return false;

  out += "$'";
  for (usize i = 0; i < arg.length; i++) {
    let const character = arg[i];
    switch (character) {
    case '\a': out += "\\a"; break;
    case '\b': out += "\\b"; break;
    case '\t': out += "\\t"; break;
    case '\n': out += "\\n"; break;
    case '\v': out += "\\v"; break;
    case '\f': out += "\\f"; break;
    case '\r': out += "\\r"; break;
    case '\x1b': out += "\\E"; break;
    case '\'': out += "\\'"; break;
    case '\\': out += "\\\\"; break;
    default: {
      let const byte = static_cast<unsigned char>(character);
      if (byte < 0x20 || byte == 0x7f) {
        out.push('\\');
        out.push(static_cast<char>('0' + ((byte >> 6) & 7)));
        out.push(static_cast<char>('0' + ((byte >> 3) & 7)));
        out.push(static_cast<char>('0' + (byte & 7)));
      } else {
        out.push(character);
      }
      break;
    }
    }
  }
  out += "'";
  return true;
}

} /* namespace utils */

} /* namespace koshka */
