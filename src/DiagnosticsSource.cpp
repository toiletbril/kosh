#include "Builtin.hpp"
#include "DiagnosticsChecksInternal.hpp"
#include "Lexer.hpp"
#include "PackedStringKey.hpp"
#include "StaticStringMap.hpp"
#include "Tokens.hpp"
#include "Utils.hpp"

namespace koshka {

namespace expressions {

namespace {

/* Which quoting a byte sits inside, which decides whether a homoglyph is read
   as syntax or as text. */
enum class source_scan_state : u8
{
  Normal,
  SingleQuoted,
  DoubleQuoted,
  Comment,
};

constexpr u8 SCAN_ACTS_NORMAL = 1U << 0U;
constexpr u8 SCAN_ACTS_SINGLE_QUOTED = 1U << 1U;
constexpr u8 SCAN_ACTS_DOUBLE_QUOTED = 1U << 2U;
constexpr u8 SCAN_ACTS_COMMENT = 1U << 3U;
constexpr u8 SCAN_ACTS_EVERYWHERE = SCAN_ACTS_NORMAL | SCAN_ACTS_SINGLE_QUOTED |
                                    SCAN_ACTS_DOUBLE_QUOTED | SCAN_ACTS_COMMENT;

struct source_scan_table
{
  u8 acting_states[256];
};

/* The states in which a byte changes the scan below. A run of bytes that acts
   in no state is stepped over at once, which keeps a long word, a long comment,
   and a long quoted string off the per-byte dispatch. */
consteval fn build_source_scan_table() -> source_scan_table
{
  source_scan_table table{};

  for (usize byte = 0x80; byte < 256; byte++)
    table.acting_states[byte] = SCAN_ACTS_EVERYWHERE;

  table.acting_states[static_cast<u8>('\r')] = SCAN_ACTS_EVERYWHERE;
  table.acting_states[static_cast<u8>('\n')] = SCAN_ACTS_EVERYWHERE;
  table.acting_states[static_cast<u8>('\\')] =
      SCAN_ACTS_NORMAL | SCAN_ACTS_SINGLE_QUOTED | SCAN_ACTS_DOUBLE_QUOTED;
  table.acting_states[static_cast<u8>('\'')] =
      SCAN_ACTS_NORMAL | SCAN_ACTS_SINGLE_QUOTED;
  table.acting_states[static_cast<u8>('"')] =
      SCAN_ACTS_NORMAL | SCAN_ACTS_DOUBLE_QUOTED;

  for (let const byte : "#< \t;&|()")
    table.acting_states[static_cast<u8>(byte)] |= SCAN_ACTS_NORMAL;

  return table;
}

constexpr source_scan_table SOURCE_SCAN = build_source_scan_table();

alwaysinline pure fn skip_plain_bytes(StringView source, usize at,
                                      u8 acting_state) wontthrow -> usize
{
  while (at + 1 < source.length &&
         (SOURCE_SCAN.acting_states[static_cast<u8>(source[at + 1])] &
          acting_state) == 0)
  {
    at++;
  }

  return at;
}

enum class homoglyph_kind : u8
{
  None,
  SingleQuote,
  DoubleQuote,
  Dash,
  Space,
};

constexpr u64 HIGH_BITS = 0x8080808080808080ULL;
constexpr u64 LOW_BITS = 0x0101010101010101ULL;
constexpr u64 CARRIAGE_RETURNS = 0x0d0d0d0d0d0d0d0dULL;
constexpr u64 BACKSLASHES = 0x5c5c5c5c5c5c5c5cULL;

alwaysinline pure fn chunk_holds_byte(u64 chunk, u64 repeated) wontthrow -> u64
{
  let const differences = chunk ^ repeated;
  return (differences - LOW_BITS) & ~differences & HIGH_BITS;
}

alwaysinline pure fn chunk_holds_scanned_byte(u64 chunk) wontthrow -> bool
{
  return ((chunk & HIGH_BITS) | chunk_holds_byte(chunk, CARRIAGE_RETURNS) |
          chunk_holds_byte(chunk, BACKSLASHES)) != 0;
}

/* Whether the source holds a byte the classification below could report. A
   script is almost always plain ASCII, so this eight-byte-at-a-time answer
   keeps the classifying walk off the common path. */
pure fn source_holds_scanned_byte(StringView source) wontthrow -> bool
{
  usize at = 0;
  while (at + sizeof(u64) <= source.length) {
    u64 chunk = 0;
    __builtin_memcpy(&chunk, source.data + at, sizeof(u64));
    if (chunk_holds_scanned_byte(chunk)) return true;
    at += sizeof(u64);
  }

  for (; at < source.length; at++) {
    let const byte = static_cast<u8>(source[at]);
    if (byte >= 0x80 || byte == '\r' || byte == '\\') {
      return true;
    }
  }

  return false;
}

pure fn classify_codepoint(u32 codepoint) wontthrow -> homoglyph_kind
{
  switch (codepoint) {
  case 0x2018:
  case 0x2019:
  case 0x201a:
  case 0x201b:
  case 0x2032:
  case 0x2035: return homoglyph_kind::SingleQuote;

  case 0x201c:
  case 0x201d:
  case 0x201e:
  case 0x201f:
  case 0x2033:
  case 0x2036: return homoglyph_kind::DoubleQuote;

  case 0x2010:
  case 0x2011:
  case 0x2012:
  case 0x2013:
  case 0x2014:
  case 0x2015:
  case 0x2043:
  case 0x2212:
  case 0xfe58:
  case 0xfe63:
  case 0xff0d: return homoglyph_kind::Dash;

  case 0x00a0:
  case 0x2000:
  case 0x2001:
  case 0x2002:
  case 0x2003:
  case 0x2004:
  case 0x2005:
  case 0x2006:
  case 0x2007:
  case 0x2008:
  case 0x2009:
  case 0x200a:
  case 0x200b:
  case 0x202f:
  case 0x205f:
  case 0x3000:
  case 0xfeff: return homoglyph_kind::Space;

  default: return homoglyph_kind::None;
  }
}

/* A slanted single quote inside a double-quoted string, and a slanted double
   quote inside a single-quoted string, are the literal typography upstream
   allows, so they answer None. */
pure fn homoglyph_diagnostic(homoglyph_kind kind,
                             source_scan_state state) wontthrow
    -> Maybe<diagnostic_id>
{
  if (state == source_scan_state::Comment) return None;

  switch (kind) {
  case homoglyph_kind::SingleQuote:
    if (state == source_scan_state::Normal) return diagnostic_id::sc1110;
    if (state == source_scan_state::SingleQuoted) return diagnostic_id::sc1112;
    return None;

  case homoglyph_kind::DoubleQuote:
    if (state == source_scan_state::Normal) return diagnostic_id::sc1110;
    if (state == source_scan_state::DoubleQuoted) return diagnostic_id::sc1111;
    return None;

  case homoglyph_kind::Dash:
    if (state == source_scan_state::Normal) return diagnostic_id::sc1100;
    return None;

  case homoglyph_kind::Space:
    if (state == source_scan_state::Normal) return diagnostic_id::sc1018;
    return None;

  default: return None;
  }
}

fn codepoint_spelling(u32 codepoint) throws -> String
{
  constexpr char HEX_DIGITS[] = "0123456789ABCDEF";

  let spelling = String{"U+"};
  for (let shift = codepoint > 0xffff ? 20 : 12; shift >= 0; shift -= 4)
    spelling.push(HEX_DIGITS[(codepoint >> shift) & 0xf]);

  return spelling;
}

pure fn byte_precedes_comment(char byte) wontthrow -> bool
{
  switch (byte) {
  case ' ':
  case '\t':
  case '\n':
  case '\r':
  case ';':
  case '&':
  case '|':
  case '(':
  case ')': return true;

  default: return false;
  }
}

pure fn byte_ends_here_document_delimiter(char byte) wontthrow -> bool
{
  switch (byte) {
  case ' ':
  case '\t':
  case '\n':
  case '\r':
  case ';':
  case '&':
  case '|':
  case '<':
  case '>':
  case '(':
  case ')': return true;

  default: return false;
  }
}

/* The offset just past the here-document terminator. A body holds prose, where
   a slanted quote or a Unicode dash is ordinary text. */
pure fn skip_here_document(StringView source, usize at) wontthrow -> usize
{
  usize cursor = at + 2;
  if (cursor < source.length && source[cursor] == '-') {
    cursor++;
  }

  while (cursor < source.length &&
         (source[cursor] == ' ' || source[cursor] == '\t'))
    cursor++;

  usize delimiter_start = cursor;
  usize delimiter_end;

  if (cursor < source.length &&
      (source[cursor] == '\'' || source[cursor] == '"'))
  {
    let const quote = source[cursor];
    cursor++;
    delimiter_start = cursor;
    while (cursor < source.length && source[cursor] != quote)
      cursor++;
    delimiter_end = cursor;
  } else {
    while (cursor < source.length &&
           !byte_ends_here_document_delimiter(source[cursor]))
      cursor++;
    delimiter_end = cursor;
  }

  if (delimiter_end == delimiter_start) return at + 2;

  let const delimiter = source.substring_of_length(
      delimiter_start, delimiter_end - delimiter_start);

  while (cursor < source.length && source[cursor] != '\n')
    cursor++;

  while (cursor < source.length) {
    cursor++;

    usize line_start = cursor;
    while (line_start < source.length && source[line_start] == '\t')
      line_start++;

    usize line_end = line_start;
    while (line_end < source.length && source[line_end] != '\n')
      line_end++;

    if (line_end - line_start == delimiter.length &&
        source.substring_of_length(line_start, delimiter.length) == delimiter)
    {
      return line_end;
    }

    cursor = line_end;
  }

  return source.length;
}

pure fn byte_is_ascii_letter(char byte) wontthrow -> bool
{
  return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z');
}

/* The letters whose escape reads as a control byte in other languages. */
pure fn escape_names_control_byte(char byte) wontthrow -> bool
{
  switch (byte) {
  case 'n':
  case 'r':
  case 't': return true;

  default: return false;
  }
}

fn escape_spelling(char escaped) throws -> String
{
  let spelling = String{"\\"};
  spelling.push(escaped);

  return spelling;
}

/* Whether the line ending just above the given line start carries a
   continuation, which is what makes a commented-out backslash matter. */
pure fn line_above_continues(StringView source, usize line_start) wontthrow
    -> bool
{
  if (line_start < 2) return false;
  if (source[line_start - 1] != '\n') return false;

  usize ending = line_start - 1;
  if (source[ending - 1] == '\r') ending--;

  return ending > 0 && source[ending - 1] == '\\';
}

} /* namespace */

fn check_source_bytes(AnalysisContext &actx, StringView source) throws -> void
{
  if (!source_holds_scanned_byte(source)) return;

  let state = source_scan_state::Normal;
  let was_carriage_return_reported = false;
  let is_command_position = true;
  usize comment_line_start = 0;
  usize line_start = 0;
  usize at = 0;

  while (at < source.length) {
    let const byte = static_cast<u8>(source[at]);

    if (byte >= 0x80) {
      let const decoded = utils::decode_utf8(source, at, 0);
      let const id =
          homoglyph_diagnostic(classify_codepoint(decoded.value), state);

      /* A leading byte-order mark is already reported as its own finding. */
      if (id.has_value() && at != 0) {
        let const spelling = codepoint_spelling(decoded.value);
        actx.report_diagnostic(*id, SourceLocation{at, decoded.length},
                               {spelling.view()});
      }

      at += decoded.length;
      continue;
    }

    if (byte == '\r' && !was_carriage_return_reported) {
      was_carriage_return_reported = true;
      actx.report_diagnostic(diagnostic_id::sc1017, SourceLocation{at, 1});
    }

    switch (state) {
    case source_scan_state::Normal:
      switch (byte) {
      case '\'':
        state = source_scan_state::SingleQuoted;
        is_command_position = false;
        break;

      case '"':
        state = source_scan_state::DoubleQuoted;
        is_command_position = false;
        break;

      case '\\': {
        let const escaped = at + 1 < source.length ? source[at + 1] : '\0';

        if (escaped == ' ' || escaped == '\t') {
          usize blank_end = at + 1;
          while (blank_end < source.length &&
                 (source[blank_end] == ' ' || source[blank_end] == '\t'))
          {
            blank_end++;
          }

          if (blank_end < source.length && source[blank_end] == '\n') {
            actx.report_diagnostic(diagnostic_id::sc1101,
                                   SourceLocation{at, blank_end - at});
          }
        } else if (byte_is_ascii_letter(escaped) && !is_command_position) {
          let const spelling = escape_spelling(escaped);
          actx.report_diagnostic(
              escape_names_control_byte(escaped) ? diagnostic_id::sc1012
                                                 : diagnostic_id::sc1001,
              SourceLocation{at, 2},
              {spelling.view(), source.substring_of_length(at + 1, 1)});
        }

        is_command_position = false;
        at++;
        break;
      }

      case '#':
        if (at == 0 || byte_precedes_comment(source[at - 1])) {
          state = source_scan_state::Comment;
          comment_line_start = line_start;
        }
        break;

      case '<':
        is_command_position = false;
        if (at + 2 < source.length && source[at + 1] == '<' &&
            source[at + 2] != '<')
        {
          at = skip_here_document(source, at);
          line_start = at;
          while (line_start > 0 && source[line_start - 1] != '\n')
            line_start--;
          continue;
        }
        break;

      case ' ':
      case '\t':
      case '\r': break;

      case '\n':
      case ';':
      case '&':
      case '|':
      case '(':
      case ')': is_command_position = true; break;

      default:
        is_command_position = false;
        at = skip_plain_bytes(source, at, SCAN_ACTS_NORMAL);
        break;
      }
      break;

    case source_scan_state::SingleQuoted:
      switch (byte) {
      case '\'': state = source_scan_state::Normal; break;

      /* The backslash carries no meaning here, so the byte behind it is read
         as source and the scan does not step over it. */
      case '\\':
        if (at + 1 < source.length) {
          if (source[at + 1] == '\'') {
            actx.report_diagnostic(diagnostic_id::sc1003,
                                   SourceLocation{at, 2});
          } else if (source[at + 1] == '\n' && at > 0) {
            /* The caret reaches back one byte because a span that opens on a
               continuation is rendered against the line below it. */
            actx.report_diagnostic(diagnostic_id::sc1004,
                                   SourceLocation{at - 1, 2});
          }
        }
        break;

      default:
        at = skip_plain_bytes(source, at, SCAN_ACTS_SINGLE_QUOTED);
        break;
      }
      break;

    case source_scan_state::DoubleQuoted:
      switch (byte) {
      case '"': state = source_scan_state::Normal; break;
      case '\\': at++; break;

      default:
        at = skip_plain_bytes(source, at, SCAN_ACTS_DOUBLE_QUOTED);
        break;
      }
      break;

    case source_scan_state::Comment:
      if (byte != '\n') {
        at = skip_plain_bytes(source, at, SCAN_ACTS_COMMENT);
        break;
      }

      if (at > 1 && source[at - 1] == '\\' &&
          line_above_continues(source, comment_line_start))
      {
        actx.report_diagnostic(diagnostic_id::sc1143,
                               SourceLocation{at - 2, 2});
      }

      state = source_scan_state::Normal;
      is_command_position = true;
      break;
    }

    /* An escape case consumes its escaped byte, so the last consumed byte is
       read rather than the byte the state switch dispatched on. */
    at++;
    if (source[at - 1] == '\n') line_start = at;
  }
}

namespace {

constexpr PackedStringKey KNOWN_SHELL_KEYS[] = {
    SSK("ash"),  SSK("bash"), SSK("bosh"),  SSK("busybox"), SSK("dash"),
    SSK("kosh"), SSK("ksh"),  SSK("ksh88"), SSK("ksh93"),   SSK("mksh"),
    SSK("posh"), SSK("sh"),   SSK("yash"),  SSK("zsh"),
};
constexpr StaticStringSet KNOWN_SHELLS{KNOWN_SHELL_KEYS};

pure fn path_base_name(StringView path) wontthrow -> StringView
{
  usize at = path.length;
  while (at > 0 && path[at - 1] != '/')
    at--;

  return path.substring(at);
}

/* One left-to-right reader over the shebang line, so the interpreter, its
   parameters, and their spans come from one walk. */
struct shebang_word_reader
{
  StringView line;
  usize at{0};
  usize word_start{0};

  fn read_next_word() wontthrow -> StringView
  {
    while (at < line.length && lexer::is_whitespace(line[at]))
      at++;

    word_start = at;
    while (at < line.length && !lexer::is_whitespace(line[at]))
      at++;

    return line.substring_of_length(word_start, at - word_start);
  }

  pure fn word_location() const wontthrow -> SourceLocation
  {
    return SourceLocation{word_start, at - word_start};
  }
};

/* Whether a later line in the leading comment block opens with the shebang
   bytes. A misplaced shebang sits under a copyright header, so the walk stops
   at the first line that is neither blank nor a comment. */
pure fn header_holds_shebang(StringView source, usize first_line_end) wontthrow
    -> bool
{
  usize at = first_line_end;

  while (at < source.length) {
    at++;

    usize line_start = at;
    while (line_start < source.length &&
           lexer::is_whitespace(source[line_start]))
      line_start++;

    if (line_start + 1 < source.length && source[line_start] == '#' &&
        source[line_start + 1] == '!')
    {
      return true;
    }

    if (line_start < source.length && source[line_start] != '#' &&
        source[line_start] != '\n')
    {
      return false;
    }

    while (at < source.length && source[at] != '\n')
      at++;
  }

  return false;
}

} /* namespace */

fn check_shebang(AnalysisContext &actx, StringView source,
                 bool is_named_script_file) throws -> void
{
  usize line_end = 0;
  while (line_end < source.length && source[line_end] != '\n')
    line_end++;

  let const first_line = source.substring_of_length(0, line_end);

  usize at = 0;
  while (at < first_line.length && lexer::is_whitespace(first_line[at]))
    at++;

  let const indent_length = at;

  /* A leading `!` is the negation operator, so only a following path reads as
     a mistyped shebang. */
  if (at < first_line.length && first_line[at] == '!') {
    let const is_swapped = at + 2 < first_line.length &&
                           first_line[at + 1] == '#' &&
                           first_line[at + 2] == '/';

    if (is_swapped) {
      actx.report_diagnostic(diagnostic_id::sc1084, SourceLocation{at, 2});
      return;
    }

    if (at + 1 < first_line.length && first_line[at + 1] == '/')
      actx.report_diagnostic(diagnostic_id::sc1104, SourceLocation{at, 1});

    return;
  }

  let const has_hash = at < first_line.length && first_line[at] == '#';
  usize bang_at = has_hash ? at + 1 : at;
  while (bang_at < first_line.length &&
         lexer::is_whitespace(first_line[bang_at]))
    bang_at++;

  let const has_bang =
      has_hash && bang_at < first_line.length && first_line[bang_at] == '!';

  if (!has_bang) {
    if (header_holds_shebang(source, line_end)) {
      actx.report_diagnostic(diagnostic_id::sc1128, SourceLocation{at, 1});
      return;
    }

    /* A comment naming an absolute path is the shebang written without its
       bang. */
    if (has_hash && bang_at < first_line.length && first_line[bang_at] == '/') {
      actx.report_diagnostic(diagnostic_id::sc1113, SourceLocation{at, 1});
      return;
    }

    /* A script without a shebang runs correctly, so the missing interpreter is
       reported only when diagnostics were asked for. */
    if (is_named_script_file && actx.warning_level != 0)
      actx.report_diagnostic(diagnostic_id::sc2148, SourceLocation{0, 1});

    return;
  }

  if (indent_length != 0)
    actx.report_diagnostic(diagnostic_id::sc1114,
                           SourceLocation{0, indent_length});

  if (bang_at != at + 1) {
    actx.report_diagnostic(diagnostic_id::sc1115,
                           SourceLocation{at + 1, bang_at - at - 1});
  }

  let reader = shebang_word_reader{first_line, bang_at + 1, bang_at + 1};
  let const interpreter = reader.read_next_word();
  let const interpreter_location = reader.word_location();

  if (interpreter.is_empty()) return;

  if (interpreter[0] != '/') {
    actx.report_diagnostic(diagnostic_id::sc2239, interpreter_location,
                           {interpreter});
  }

  if (interpreter[interpreter.length - 1] == '/') {
    actx.report_diagnostic(diagnostic_id::sc2246, interpreter_location,
                           {interpreter});
  }

  let const interpreter_name = path_base_name(interpreter);
  let const names_env = interpreter_name == StringView{"env"};

  usize parameter_count = 0;
  let shell_argument = StringView{};
  let shell_argument_location = interpreter_location;
  let has_split_string_flag = false;

  for (;;) {
    let const word = reader.read_next_word();
    if (word.is_empty()) break;

    if (shell_argument.is_empty() && word[0] != '-') {
      shell_argument = word;
      shell_argument_location = reader.word_location();
    }

    if (word == StringView{"-S"} ||
        word.starts_with(StringView{"--split-string"}))
      has_split_string_flag = true;

    parameter_count++;
  }

  /* `env -S` splits its own argument, so the words after it are not separate
     shebang parameters. */
  if (parameter_count > 1 && !(names_env && has_split_string_flag))
    actx.report_diagnostic(diagnostic_id::sc2096, interpreter_location);

  let const shell_name =
      names_env ? path_base_name(shell_argument) : interpreter_name;
  let const shell_name_location =
      names_env ? shell_argument_location : interpreter_location;

  if (shell_name.is_empty()) return;

  if (!KNOWN_SHELLS.contains(shell_name)) {
    actx.report_diagnostic(diagnostic_id::sc1008, shell_name_location,
                           {shell_name});
    return;
  }

  if (shell_name == StringView{"dash"} || shell_name == StringView{"sh"})
    actx.is_posix_sh_shebang = true;
}

namespace {

constexpr PackedStringKey DIRECTIVE_KEY_KEYS[] = {
    SSK("disable"), SSK("enable"), SSK("external-sources"),
    SSK("shell"),   SSK("source"), SSK("source-path"),
};
constexpr StaticStringSet DIRECTIVE_KEYS{DIRECTIVE_KEY_KEYS};

/* A word that continues the command above it, so a directive placed before it
   covers nothing. */
constexpr PackedStringKey CLAUSE_KEYWORD_KEYS[] = {
    SSK("do"),   SSK("done"), SSK("elif"), SSK("else"),
    SSK("esac"), SSK("fi"),   SSK("then"), SSK("}"),
};
constexpr StaticStringSet CLAUSE_KEYWORDS{CLAUSE_KEYWORD_KEYS};

constexpr usize DIRECTIVE_KEYWORD_LENGTH = 10;

pure fn find_line_start(StringView source, usize position) wontthrow -> usize
{
  usize at = position;
  while (at > 0 && source[at - 1] != '\n')
    at--;

  return at;
}

pure fn only_blanks_precede(StringView source, usize line_start,
                            usize position) wontthrow -> bool
{
  for (usize at = line_start; at < position; at++) {
    if (!lexer::is_whitespace(source[at])) return false;
  }

  return true;
}

/* The first word below the directive, with blank lines and further comments
   skipped. */
pure fn read_word_below_directive(StringView source, usize after) wontthrow
    -> StringView
{
  usize at = after;

  loop
  {
    while (at < source.length &&
           (lexer::is_whitespace(source[at]) || source[at] == '\n'))
    {
      at++;
    }

    if (at >= source.length) return {};

    if (source[at] != '#') break;

    while (at < source.length && source[at] != '\n')
      at++;
  }

  let const word_start = at;
  while (at < source.length && !lexer::is_whitespace(source[at]) &&
         source[at] != '\n')
  {
    at++;
  }

  return source.substring_of_length(word_start, at - word_start);
}

/* The last line above the directive that carries a command, with blank lines
   and further comments skipped. Trailing blanks are dropped so the terminator
   is the final byte. */
pure fn read_line_above_directive(StringView source, usize line_start) wontthrow
    -> StringView
{
  usize end = line_start;

  while (end > 0) {
    end--;

    let const start = find_line_start(source, end);
    usize content_start = start;
    while (content_start < end && lexer::is_whitespace(source[content_start]))
      content_start++;

    usize content_end = end;
    while (content_end > content_start &&
           lexer::is_whitespace(source[content_end - 1]))
      content_end--;

    if (content_start < content_end && source[content_start] != '#')
      return source.substring_of_length(content_start,
                                        content_end - content_start);

    end = start;
  }

  return {};
}

pure fn line_opens_case_branch(StringView line) wontthrow -> bool
{
  if (line.length >= 2 && line[line.length - 1] == ';' &&
      line[line.length - 2] == ';')
  {
    return true;
  }

  if (line.length >= 2 && line[line.length - 1] == '&' &&
      line[line.length - 2] == ';')
  {
    return true;
  }

  if (line.length >= 2 && line[line.length - 1] == 'n' &&
      line[line.length - 2] == 'i' &&
      (line.length == 2 || lexer::is_whitespace(line[line.length - 3])))
  {
    return true;
  }

  return false;
}

pure fn word_holds_case_pattern(StringView word) wontthrow -> bool
{
  for (usize at = 0; at < word.length; at++) {
    if (word[at] == '(') return false;
    if (word[at] == ')') return true;
  }

  return false;
}

/* SC1107 and SC1125, read from the tokens after the directive keyword. One
   finding closes the scan, since a malformed directive is usually followed by
   prose that would report again on every word. */
fn check_directive_body(AnalysisContext &actx, StringView source,
                        shellcheck_directive_span span,
                        usize body_position) throws -> void
{
  let const comment_end = span.position + span.length;
  usize at = span.position + body_position;

  while (at < comment_end) {
    while (at < comment_end && lexer::is_whitespace(source[at]))
      at++;

    if (at >= comment_end) return;

    let const token_start = at;
    while (at < comment_end && !lexer::is_whitespace(source[at]))
      at++;

    let const token = source.substring_of_length(token_start, at - token_start);

    usize separator_position = 0;
    while (separator_position < token.length &&
           token[separator_position] != '=')
      separator_position++;

    if (separator_position == token.length) {
      actx.report_diagnostic(diagnostic_id::sc1125,
                             SourceLocation{token_start, token.length},
                             {token});
      return;
    }

    let const key = token.substring_of_length(0, separator_position);

    if (!DIRECTIVE_KEYS.contains(key)) {
      actx.report_diagnostic(diagnostic_id::sc1107,
                             SourceLocation{token_start, key.length}, {key});
      return;
    }
  }
}

} /* namespace */

fn check_shellcheck_directives(
    AnalysisContext &actx, StringView source,
    const ArrayList<shellcheck_directive_span> &directives) throws -> void
{
  usize previous_position = static_cast<usize>(-1);

  for (let const &directive : directives) {
    if (directive.position == previous_position) continue;
    previous_position = directive.position;

    usize body_position = 1;
    while (body_position < directive.length &&
           lexer::is_whitespace(source[directive.position + body_position]))
    {
      body_position++;
    }
    body_position += DIRECTIVE_KEYWORD_LENGTH;

    check_directive_body(actx, source, directive, body_position);

    let const line_start = find_line_start(source, directive.position);

    if (!only_blanks_precede(source, line_start, directive.position)) {
      actx.report_diagnostic(
          diagnostic_id::sc1126,
          SourceLocation{directive.position, directive.length});
      continue;
    }

    let const word_below = read_word_below_directive(
        source, directive.position + directive.length);

    if (CLAUSE_KEYWORDS.contains(word_below)) {
      actx.report_diagnostic(
          diagnostic_id::sc1123,
          SourceLocation{directive.position, directive.length}, {word_below});
      continue;
    }

    if (word_holds_case_pattern(word_below) &&
        line_opens_case_branch(read_line_above_directive(source, line_start)))
    {
      actx.report_diagnostic(
          diagnostic_id::sc1124,
          SourceLocation{directive.position, directive.length});
    }
  }
}

fn check_heredoc_terminators(
    AnalysisContext &actx, StringView source,
    const ArrayList<heredoc_terminator_miss> &misses) throws -> void
{
  for (let const &miss : misses) {
    let const terminator =
        source.substring_of_length(miss.position, miss.length);

    switch (miss.kind) {
    case heredoc_miss_kind::IndentedTerminator:
      actx.report_diagnostic(diagnostic_id::sc1039,
                             SourceLocation{miss.position, miss.length},
                             {terminator});
      break;

    case heredoc_miss_kind::TabIndentedTerminator:
      actx.report_diagnostic(diagnostic_id::sc1040,
                             SourceLocation{miss.position, miss.length},
                             {terminator});
      break;

    case heredoc_miss_kind::TrailingBlankTerminator:
      actx.report_diagnostic(diagnostic_id::sc1118,
                             SourceLocation{miss.position, miss.length},
                             {terminator});
      break;
    }
  }
}

namespace {

struct unassigned_read
{
  StringView name;
  SourceLocation location;
};

/* The assigned name a read comes closest to, with the assignment that recorded
   it. */
struct resembling_assignment
{
  StringView name;
  SourceLocation location;
};

pure fn fold_name_byte(char byte) wontthrow -> char
{
  return byte >= 'a' && byte <= 'z' ? static_cast<char>(byte - ('a' - 'A'))
                                    : byte;
}

/* Whether two names differ only in letter case and in underscore placement,
   which is the shape a misspelled reference of an assigned name takes. */
pure fn names_resemble_each_other(StringView left, StringView right) wontthrow
    -> bool
{
  usize at_left = 0;
  usize at_right = 0;

  loop
  {
    while (at_left < left.length && left[at_left] == '_')
      at_left++;
    while (at_right < right.length && right[at_right] == '_')
      at_right++;

    if (at_left == left.length || at_right == right.length) {
      break;
    }
    if (fold_name_byte(left[at_left]) != fold_name_byte(right[at_right]))
      return false;

    at_left++;
    at_right++;
  }

  return at_left == left.length && at_right == right.length;
}

/* The same edit distance the command name suggestion spends, so a mistyped
   variable and a mistyped command are judged by one rule. */
pure fn names_are_near_misspellings(StringView left, StringView right) wontthrow
    -> bool
{
  let const budget = utils::suggestion_distance_budget(right.length);
  return utils::NameSuggestion::is_correction(
      utils::bounded_osa_distance(left, right, budget), right.length);
}

} /* namespace */

fn check_command_name_assignments(AnalysisContext &actx) throws -> void
{
  for (let const &assignment : actx.command_name_assignments) {
    if (actx.command_position_names.contains(assignment.name.view())) continue;

    actx.report_diagnostic(diagnostic_id::sc2209, assignment.location,
                           {assignment.name.view(), assignment.value.view()});
  }
}

fn check_unassigned_variable_reads(AnalysisContext &actx) throws -> void
{
  if (actx.reads_before_assignment.count() == 0) return;
  if (!actx.should_report(diagnostic_id::sc2154) &&
      !actx.should_report(diagnostic_id::sc2153))
  {
    return;
  }

  let reads = ArrayList<unassigned_read>{heap_allocator()};
  actx.reads_before_assignment.for_each(
      [&reads](StringView name, const SourceLocation &location)
          throws -> void { reads.push(unassigned_read{name, location}); });

  reads.sort([](const unassigned_read &left, const unassigned_read &right) {
    return left.location.position < right.location.position;
  });

  for (let const &read : reads) {
    resembling_assignment resembled{};
    resembling_assignment misspelled{};
    let const do_match = [&read, &resembled, &misspelled](
                             StringView assigned,
                             const SourceLocation &location) throws -> void {
      if (!resembled.name.is_empty()) return;
      if (assigned == read.name) return;

      if (names_resemble_each_other(assigned, read.name)) {
        resembled = resembling_assignment{assigned, location};
        return;
      }

      if (misspelled.name.is_empty() &&
          names_are_near_misspellings(assigned, read.name))
      {
        misspelled = resembling_assignment{assigned, location};
      }
    };

    actx.assigned_names_so_far.for_each(do_match);
    actx.global_assigned_names.for_each(do_match);
    actx.function_local_names.for_each(do_match);

    if (resembled.name.is_empty()) resembled = misspelled;

    if (resembled.name.is_empty() && actx.eval_context != nullptr) {
      let const suggestion =
          actx.eval_context->suggest_similar_variable_name(read.name);
      if (suggestion.has_value() &&
          actx.eval_context->is_exported(suggestion->view()))
      {
        continue;
      }
    }

    if (resembled.name.is_empty()) {
      actx.report_diagnostic(diagnostic_id::sc2154, read.location, {read.name});
    } else {
      actx.report_diagnostic(diagnostic_id::sc2153, read.location,
                             {read.name, resembled.name}, resembled.location);
    }
  }
}

namespace {

constexpr usize NO_DEFINITION_INDEX = ~usize{0};

/* Every definition and call the script gave one name, gathered once so the
   sweep no longer costs definitions times calls on a large script. */
struct function_name_summary
{
  usize first_definition_index{NO_DEFINITION_INDEX};
  u32 first_definition_position{0};
  bool has_call_with_arguments{false};
  bool has_call_without_arguments{false};
};

fn build_function_name_summaries(const AnalysisContext &actx) throws
    -> StringMap<function_name_summary>
{
  StringMap<function_name_summary> summaries{heap_allocator()};
  summaries.reserve(actx.function_definitions.count());

  for (usize index = 0; index < actx.function_definitions.count(); index++) {
    let const &definition = actx.function_definitions[index];
    let &summary = summaries.get_or_create(definition.name, {});
    if (summary.first_definition_index != NO_DEFINITION_INDEX) continue;

    summary.first_definition_index = index;
    summary.first_definition_position = definition.location.position;
  }

  for (let const &call : actx.function_calls) {
    let const summary = summaries.find(call.name);
    if (summary == nullptr) continue;

    if (call.has_arguments) {
      summary->has_call_with_arguments = true;
    } else {
      summary->has_call_without_arguments = true;
    }
  }

  return summaries;
}

fn check_function_argument_use(AnalysisContext &actx,
                               const function_definition_record &definition,
                               const function_name_summary &summary) throws
    -> void
{
  /* A definition no call reaches may belong to a sourced library, where the
     caller lives outside this file. */
  if (summary.has_call_with_arguments || !summary.has_call_without_arguments) {
    return;
  }

  actx.report_diagnostic(diagnostic_id::sc2120, definition.location,
                         {definition.name});

  for (let const &call : actx.function_calls) {
    if (call.name != definition.name) continue;

    actx.report_diagnostic(diagnostic_id::sc2119, call.location,
                           {definition.name}, definition.location);
  }
}

fn check_call_before_definition(
    AnalysisContext &actx,
    const StringMap<function_name_summary> &summaries) throws -> void
{
  for (let const &call : actx.function_calls) {
    if (call.is_inside_function_body) continue;

    /* A name that is also a builtin runs the builtin until the definition is
       reached, so the earlier call is not a forward reference. */
    if (search_builtin(call.name).has_value()) continue;

    let const summary = summaries.find(call.name);
    if (summary == nullptr) continue;
    if (call.location.position >= summary->first_definition_position) continue;

    actx.report_diagnostic(
        diagnostic_id::sc2218, call.location, {call.name},
        actx.function_definitions[summary->first_definition_index].location);
  }
}

} /* namespace */

fn check_function_argument_dataflow(AnalysisContext &actx) throws -> void
{
  if (actx.function_definitions.is_empty()) return;

  let const should_check_argument_use =
      actx.should_report(diagnostic_id::sc2119) ||
      actx.should_report(diagnostic_id::sc2120);

  /* An interactive chunk runs against a live shell whose functions the file
     never defines, so the order the file states is not the order that runs. */
  let const should_check_definition_order =
      !actx.should_silence_unresolved_commands &&
      actx.should_report(diagnostic_id::sc2218);

  if (!should_check_argument_use && !should_check_definition_order) return;

  let const summaries = build_function_name_summaries(actx);

  if (should_check_argument_use) {
    for (usize index = 0; index < actx.function_definitions.count(); index++) {
      let const &definition = actx.function_definitions[index];
      if (!definition.has_positional_reads) continue;

      /* A redefinition is judged by the first body the file gives the name. */
      let const summary = summaries.find(definition.name);
      if (summary == nullptr || summary->first_definition_index != index)
        continue;

      check_function_argument_use(actx, definition, *summary);
    }
  }

  if (should_check_definition_order)
    check_call_before_definition(actx, summaries);
}

} /* namespace expressions */

} /* namespace koshka */
