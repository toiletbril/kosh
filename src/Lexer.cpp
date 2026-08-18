#include "Lexer.hpp"

#include "Arena.hpp"
#include "Common.hpp"
#include "Debug.hpp"
#include "Errors.hpp"
#include "Toiletline.hpp"
#include "Tokens.hpp"
#include "Trace.hpp"
#include "Utils.hpp"

namespace koshka {

namespace lexer {

static constexpr char CEOF = static_cast<char>(EOF);

hot pure fn is_whitespace(char ch) wontthrow -> bool
{
  switch (ch) {
  case ' ':
  case '\t': return true;
  default: return false;
  }
}

hot pure fn is_number(char ch) wontthrow -> bool
{
  return ch >= '0' && ch <= '9';
}

hot pure fn is_shell_sentinel(char ch) wontthrow -> bool
{
  /* A brace is not a sentinel. POSIX recognizes '{' and '}' as reserved words
     only when a token is exactly '{' or '}', so 'a{b}c' lexes as one word. */
  switch (ch) {
  case '\n':
  case '|':
  case '(':
  case ')':
  case '&':
  case ';':
  case '<':
  case '>': return true;
  default: return false;
  }
}

hot pure fn is_part_of_identifier(char ch) wontthrow -> bool
{
  switch (ch) {
  case CEOF:
  case ' ':
  case '\t':
  case '\n':
  case '|':
  case '(':
  case ')':
  case '&':
  case ';':
  case '<':
  case '>': return false;
  default: return true;
  }
}

hot pure static fn is_plain_unquoted_run_byte(char ch) wontthrow -> bool
{
  switch (ch) {
  case CEOF:
  case ' ':
  case '\t':
  case '\n':
  case '|':
  case '(':
  case ')':
  case '&':
  case ';':
  case '<':
  case '>':
  case '$':
  case '`':
  case '\\':
  case '"':
  case '\'': return false;
  default: return true;
  }
}

hot pure fn is_string_quote(char ch) wontthrow -> bool
{
  /* A backtick opens a command substitution, not a string. */
  switch (ch) {
  case '"':
  case '\'': return true;
  default: return false;
  }
}

hot pure static fn is_ascii_letter(char ch) wontthrow -> bool
{
  return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
}

hot pure fn is_expandable_char(char ch) wontthrow -> bool
{
  switch (ch) {
  case '[':
  case '?':
  case '*': return true;
  default: return false;
  }
}

hot pure fn is_variable_name_start(char ch) wontthrow -> bool
{
  return is_ascii_letter(ch) || ch == '_';
}

hot pure fn is_variable_name(char ch) wontthrow -> bool
{
  return is_variable_name_start(ch) || is_number(ch);
}

pure fn word_looks_like_assignment(StringView word) wontthrow -> bool
{
  if (word.is_empty() || !is_variable_name_start(word[0])) return false;
  usize position = 1;
  while (position < word.length && is_variable_name(word[position]))
    position++;
  if (position < word.length && word[position] == '=') return true;
  if (position + 1 < word.length && word[position] == '+' &&
      word[position + 1] == '=')
    return true;
  if (position >= word.length || word[position] != '[') return false;

  usize bracket_depth = 1;
  position++;
  while (position < word.length && bracket_depth > 0) {
    if (word[position] == '[')
      bracket_depth++;
    else if (word[position] == ']')
      bracket_depth--;
    position++;
  }
  if (position < word.length && word[position] == '=') return true;

  return position + 1 < word.length && word[position] == '+' &&
         word[position + 1] == '=';
}

hot pure fn is_extglob_operator(char ch) wontthrow -> bool
{
  switch (ch) {
  case '?':
  case '*':
  case '+':
  case '@':
  case '!': return true;
  default: return false;
  }
}

hot pure fn is_special_parameter_char(char ch) wontthrow -> bool
{
  switch (ch) {
  case '?':
  case '!':
  case '#':
  case '$':
  case '*':
  case '@':
  case '-': return true;
  default: return false;
  }
}

} /* namespace lexer */

Lexer::Lexer(StringView source, BumpArena &arena,
             bool should_collect_debug_words, Maybe<StringView> filename,
             mimic_mood mood)
    : m_source(source), m_arena(&arena),
      m_source_name_index(filename.has_value() ? intern_source_name(*filename)
                                               : 0),
      m_mood(mood), m_should_collect_debug_words(should_collect_debug_words)
{
  LOG(Debug, "starting a lexer over %zu bytes of source", m_source.length);
}

Lexer::~Lexer() = default;

fn Lexer::peek_shell_token() throws -> Token *
{
  skip_whitespace();
  if (m_peek_cache != nullptr && m_peek_cache_position == m_cursor_position) {
    return m_peek_cache;
  }
  Token *const token = lex_shell_token();
  m_peek_cache = token;
  m_peek_cache_position = m_cursor_position;

  return token;
}

hot fn Lexer::next_shell_token() throws -> Token *
{
  skip_whitespace();

  Token *const token =
      (m_peek_cache != nullptr && m_peek_cache_position == m_cursor_position)
          ? m_peek_cache
          : lex_shell_token();
  ASSERT(token != nullptr);

  advance_past_last_peek();

  return token;
}

pure fn Lexer::source() const wontthrow -> StringView { return m_source; }

pure fn Lexer::cursor_position() const wontthrow -> usize
{
  return m_cursor_position;
}

fn Lexer::set_should_collect_shellcheck_directives(
    bool should_collect) wontthrow -> void
{
  m_should_collect_shellcheck_directives = should_collect;
}

fn Lexer::take_shellcheck_directives() throws
    -> ArrayList<shellcheck_directive_span>
{
  let directives = steal(m_pending_shellcheck_directives);
  m_pending_shellcheck_directives =
      ArrayList<shellcheck_directive_span>{heap_allocator()};
  return directives;
}

fn Lexer::take_shellcheck_directive_spans() throws
    -> ArrayList<shellcheck_directive_span>
{
  let spans = steal(m_shellcheck_directive_spans);
  m_shellcheck_directive_spans =
      ArrayList<shellcheck_directive_span>{heap_allocator()};
  return spans;
}

fn Lexer::take_heredoc_terminator_misses() throws
    -> ArrayList<heredoc_terminator_miss>
{
  let misses = steal(m_heredoc_terminator_misses);
  m_heredoc_terminator_misses =
      ArrayList<heredoc_terminator_miss>{heap_allocator()};
  return misses;
}

pure fn Lexer::is_at_source_end() const wontthrow -> bool
{
  return m_cursor_position >= m_source.length;
}

pure fn Lexer::debug_words() const wontthrow -> const ArrayList<Word> &
{
  return m_debug_words;
}

pure fn Lexer::arena() const wontthrow -> BumpArena & { return *m_arena; }

fn Lexer::set_arena(BumpArena &arena) wontthrow -> void
{
  LOG(Debug, "switching the lexer arena and dropping the cached peek");
  m_arena = &arena;
  /* The cached token lives in the old arena and must not survive the swap. */
  m_peek_cache = nullptr;
}

hot fn Lexer::advance_past_last_peek() throws -> usize
{
  ASSERT(m_cursor_position + m_cached_offset <= m_source.length);

  let const result = advance_forward(m_cached_offset);
  m_cached_offset = 0;

  /* The heredoc body sits on the lines after the newline, so it is collected
     once that newline is consumed. */
  if (m_last_shell_token_was_newline && !m_pending_heredocs.is_empty()) {
    m_last_shell_token_was_newline = false;
    collect_pending_heredocs();
  }

  return result;
}

cold fn Lexer::register_heredoc(StringView delimiter,
                                bool should_strip_tabs) throws
    -> const heredoc_contents *
{
  let contents = m_arena->create<heredoc_contents>(bump_allocator(*m_arena),
                                                   !should_strip_tabs);
  ASSERT(contents != nullptr);

  LOG(Debug, "registering a pending heredoc with delimiter '%.*s'",
      static_cast<int>(delimiter.length), delimiter.data);

  m_pending_heredocs.push({String{delimiter}, should_strip_tabs, contents});

  return contents;
}

/* A heredoc body is a run of raw text lines terminated by a line that holds the
   delimiter alone. The walker yields each line to the callback, which appends
   it as it sees fit and signals whether to continue. */
template <class Emit>
cold fn Lexer::walk_heredoc_body(usize start, StringView delimiter,
                                 bool should_strip_tabs, Emit emit_line) throws
    -> usize
{
  usize position = start;
  bool did_find_delimiter = false;
  bool has_near_miss = false;
  heredoc_terminator_miss near_miss{0, 0,
                                    heredoc_miss_kind::IndentedTerminator};

  loop
  {
    if (position >= m_source.length) break;
    let const line_start = position;
    usize line_end_position = line_start;
    while (line_end_position < m_source.length &&
           m_source[line_end_position] != '\n')
      line_end_position++;
    let const has_newline = line_end_position < m_source.length;
    position = has_newline ? line_end_position + 1 : line_end_position;

    let const line_offset = line_start;
    let const line_length = line_end_position - line_start;
    usize stripped_offset = line_offset;
    usize stripped_length = line_length;
    if (should_strip_tabs) {
      while (stripped_length > 0 && m_source[stripped_offset] == '\t') {
        stripped_offset++;
        stripped_length--;
      }
    }

    let const stripped =
        m_source.substring_of_length(stripped_offset, stripped_length);
    let const is_delimiter = (delimiter == stripped);
    did_find_delimiter = did_find_delimiter || is_delimiter;

    if (!is_delimiter && !has_near_miss && line_length > delimiter.length) {
      usize content_start = line_offset;
      usize content_end = line_offset + line_length;
      while (content_start < content_end && (m_source[content_start] == ' ' ||
                                             m_source[content_start] == '\t'))
      {
        content_start++;
      }
      while (content_end > content_start &&
             (m_source[content_end - 1] == ' ' ||
              m_source[content_end - 1] == '\t' ||
              m_source[content_end - 1] == '\r'))
      {
        content_end--;
      }

      if (content_end - content_start == delimiter.length &&
          m_source.substring_of_length(content_start, delimiter.length) ==
              delimiter)
      {
        let const leading_length = content_start - line_offset;
        let is_tab_indentation = leading_length > 0;
        for (usize at = line_offset; at < content_start; at++)
          is_tab_indentation = is_tab_indentation && m_source[at] == '\t';

        has_near_miss = true;
        near_miss = heredoc_terminator_miss{
            content_start, delimiter.length,
            leading_length == 0  ? heredoc_miss_kind::TrailingBlankTerminator
            : is_tab_indentation ? heredoc_miss_kind::TabIndentedTerminator
                                 : heredoc_miss_kind::IndentedTerminator};
      }
    }

    let const raw = m_source.substring_of_length(line_offset, line_length);
    if (!emit_line(raw, has_newline, is_delimiter)) break;
    if (!has_newline) break;
  }

  if (!did_find_delimiter && has_near_miss)
    m_heredoc_terminator_misses.push(near_miss);

  return position;
}

cold fn Lexer::collect_pending_heredocs() throws -> void
{
  LOG(Debug, "collecting %zu pending heredoc bodies",
      m_pending_heredocs.count());

  for (let &pending : m_pending_heredocs) {
    let collected = String{heap_allocator()};
    ASSERT(pending.contents != nullptr);
    pending.contents->source_position = m_cursor_position;
    let const do_append_body_line = [&](StringView line, bool,
                                        bool is_delimiter) -> bool {
      if (is_delimiter) return false;
      if (pending.should_strip_tabs) {
        usize offset = 0;
        while (offset < line.length && line[offset] == '\t')
          offset++;
        line = line.substring_of_length(offset, line.length - offset);
      }
      collected.append(line);
      collected += '\n';
      return true;
    };
    m_cursor_position =
        walk_heredoc_body(m_cursor_position, pending.delimiter.view(),
                          pending.should_strip_tabs, do_append_body_line);
    LOG(Debug, "capturing a heredoc body of %zu bytes for delimiter '%s'",
        collected.count(), pending.delimiter.c_str());
    pending.contents->text = steal(collected);
  }
  m_pending_heredocs.clear();
}

hot flatten fn Lexer::lex_shell_token() throws -> Token *
{
  Token *token{};
  if (let const ch = chop_character(); ch != lexer::CEOF) [[likely]] {
    if (lexer::is_shell_sentinel(ch)) {
      /* A < or > opens a process substitution only when a ( follows with no
         space. */
      if ((ch == '<' || ch == '>') && chop_character(1) == '(') {
        token = lex_process_substitution(ch);
      } else {
        token = lex_sentinel();
      }
    } else if (!lexer::is_whitespace(ch)) [[likely]] {
      token = lex_identifier();
    } else [[unlikely]] {
      throw ErrorWithLocationAndDetails{
          here(m_cursor_position, 1), "Unexpected character",
          "the character is not valid in an unquoted word here"};
    }
  } else {
    token = m_arena->create<tokens::EndOfFile>(here(m_cursor_position, 1));
  }

  ASSERT(token != nullptr);

  m_last_shell_token_was_newline = token->kind() == Token::Kind::Newline;

  return token;
}

hot flatten alwaysinline fn Lexer::skip_whitespace() throws -> void
{
  usize i = 0;
  loop
  {
    while (lexer::is_whitespace(chop_character(i)))
      i++;

    let const byte = chop_character(i);

    /* A backslash before a newline continues the line and both bytes vanish. A
       backslash before any other byte is left for the identifier lexer. */
    if (byte == '\\' && chop_character(i + 1) == '\n') {
      i += 2;
      continue;
    }
    /* The newline is left in place so it still terminates the command. */
    if (byte == '#') {
      let const comment_start = i;
      while (chop_character(i) != '\n' && chop_character(i) != lexer::CEOF)
        i++;
      /* The comment is classified whether or not a command is claiming
         directives, since the analysis stage reports a misplaced directive that
         no command would claim. The leading byte compare rejects an ordinary
         comment before anything is read. */
      {
        let comment = m_source.substring_of_length(
            m_cursor_position + comment_start, i - comment_start);
        usize content_position = 1;
        while (content_position < comment.length &&
               (comment[content_position] == ' ' ||
                comment[content_position] == '\t'))
        {
          content_position++;
        }
        let const directive_text = comment.substring(content_position);
        if (directive_text.starts_with(StringView{"shellcheck", 10}) &&
            (directive_text.length == 10 || directive_text[10] == ' ' ||
             directive_text[10] == '\t'))
        {
          let const span = shellcheck_directive_span{
              m_cursor_position + comment_start, i - comment_start};

          if (m_should_collect_shellcheck_directives)
            m_pending_shellcheck_directives.push(span);

          m_shellcheck_directive_spans.push(span);
        }
      }
      continue;
    }
    break;
  }
  advance_forward(i);
}

hot alwaysinline fn Lexer::advance_forward(usize offset) wontthrow -> usize
{
  ASSERT(m_cursor_position + offset <= m_source.length);
  m_cursor_position += offset;
  return offset;
}

hot alwaysinline fn Lexer::chop_character(usize offset) wontthrow -> char
{
  if (m_cursor_position + offset < m_source.length)
    return m_source[m_cursor_position + offset];

  return lexer::CEOF;
}

flatten hot alwaysinline fn Lexer::lex_identifier() throws -> Token *
{
  let word = Word{};

  usize byte_count = 0;
  usize relative_last_quote_position = 0;

  bool should_escape = false;

  Maybe<char> quote_char;

  /* An empty segment preserves an empty quoted field. */
  bool did_quote_enclose_content = false;

  /* A variable reference never merges, since each one carries its own name. */
  let const do_append_char = [&word](WordSegment::Kind kind, char ch) {
    if (!word.segments.is_empty() && word.segments.back().kind == kind &&
        kind != WordSegment::Kind::VariableReference)
    {
      word.segments.back().text += ch;
    } else {
      let single = String{heap_allocator()};
      single.push(ch);
      word.segments.push(WordSegment{kind, steal(single), false});
    }
  };

  let const do_append_unquoted_run = [&word](StringView run) {
    if (!word.segments.is_empty() &&
        word.segments.back().kind == WordSegment::Kind::UnquotedText)
    {
      word.segments.back().text.append(run);
    } else {
      let text = String{heap_allocator()};
      text.append(run);
      word.segments.push(
          WordSegment{WordSegment::Kind::UnquotedText, steal(text), false});
    }
  };

  let const do_word_is_plain_array_name = [&word]() -> bool {
    if (word.segments.count() != 1) return false;
    let const &segment = word.segments[0];
    if (segment.kind != WordSegment::Kind::UnquotedText ||
        segment.text.is_empty())
    {
      return false;
    }
    let const text = segment.text.view();
    if (!lexer::is_variable_name_start(text[0])) return false;
    for (usize i = 1; i < text.length; i++)
      if (!lexer::is_variable_name(text[i])) return false;

    return true;
  };

  let const do_subscript_closes_with_assignment =
      [this](usize start) -> Maybe<usize> {
    usize offset = start + 1;
    usize depth = 1;
    while (depth > 0) {
      let const c = chop_character(offset);
      if (c == lexer::CEOF) return None;
      offset++;
      if (c == '[')
        depth++;
      else if (c == ']')
        depth--;
    }
    let const after = chop_character(offset);
    if (after == '=' || (after == '+' && chop_character(offset + 1) == '='))
      return offset;
    return None;
  };

  let const do_scan_to_matched_close = [this](usize &offset, char open,
                                              char close) -> void {
    usize depth = 1;
    while (depth > 0) {
      let const c = chop_character(offset);
      if (c == lexer::CEOF) break;
      offset++;
      if (c == open)
        depth++;
      else if (c == close)
        depth--;
    }
  };

  loop
  {
    let const ch = chop_character(byte_count);

    let const is_inside_quote_or_escape =
        quote_char.has_value() || should_escape;
    if (!(is_inside_quote_or_escape && ch != lexer::CEOF) &&
        !lexer::is_part_of_identifier(ch))
    {
      break;
    }

    /* A NAME[subscript]= assignment keeps the subscript's operators in the word
       so a bitmask subscript such as key[a|b]=1 survives, while x[1|2] in
       argument position still splits. */
    if (!is_inside_quote_or_escape && ch == '[' &&
        do_word_is_plain_array_name())
    {
      if (Maybe<usize> close = do_subscript_closes_with_assignment(byte_count);
          close.has_value())
      {
        let const subscript_start = byte_count;
        byte_count = *close;
        do_append_unquoted_run(m_source.substring_of_length(
            m_cursor_position + subscript_start, byte_count - subscript_start));
        continue;
      }
    }

    /* An extended-glob group such as @(a|b) is captured whole so its (, nested
       |, and ) stay in the word for the matcher. */
    if (!is_inside_quote_or_escape && lexer::is_extglob_operator(ch) &&
        chop_character(byte_count + 1) == '(')
    {
      let const group_start = byte_count;
      byte_count += 2;
      do_scan_to_matched_close(byte_count, '(', ')');
      do_append_unquoted_run(m_source.substring_of_length(
          m_cursor_position + group_start, byte_count - group_start));
      continue;
    }

    if (!is_inside_quote_or_escape && lexer::is_plain_unquoted_run_byte(ch)) {
      let const run_start = byte_count;
      loop
      {
        byte_count++;
        let const next = chop_character(byte_count);
        /* The run stops before a '[' so the assignment-subscript capture above
           can protect the bracket group. */
        if (next == '[' || !lexer::is_plain_unquoted_run_byte(next)) break;
        if (lexer::is_extglob_operator(next) &&
            chop_character(byte_count + 1) == '(')
          break;
      }
      do_append_unquoted_run(m_source.substring_of_length(
          m_cursor_position + run_start, byte_count - run_start));
      continue;
    }

    if (should_escape) {
      should_escape = false;
      if (ch != '\n') do_append_char(WordSegment::Kind::LiteralText, ch);
      byte_count++;
      continue;
    }

    if (quote_char == '\'') {
      if (ch == '\'') {
        if (!did_quote_enclose_content)
          word.segments.push(WordSegment{WordSegment::Kind::LiteralText,
                                         String{heap_allocator()}, false});
        quote_char.reset();
      } else {
        do_append_char(WordSegment::Kind::LiteralText, ch);
        did_quote_enclose_content = true;
      }
      byte_count++;
      continue;
    }

    if (ch == '\\') {
      /* Inside double quotes a backslash only escapes $, `, ", \, and a
         newline, so "\n" is a backslash and an n. */
      if (quote_char == '"') {
        did_quote_enclose_content = true;
        let const escaped_next = chop_character(byte_count + 1);
        switch (escaped_next) {
        case '$':
        case '`':
        case '"':
        case '\\':
        case '\n': should_escape = true; break;
        default:
          do_append_char(WordSegment::Kind::DoubleQuotedText, '\\');
          break;
        }
        byte_count++;
        continue;
      }
      should_escape = true;
      byte_count++;
      continue;
    }

    let const is_in_double_quotes = quote_char == '"';

    if (is_in_double_quotes && ch == '"') {
      if (!did_quote_enclose_content)
        word.segments.push(WordSegment{WordSegment::Kind::DoubleQuotedText,
                                       String{heap_allocator()}, false});
      quote_char.reset();
      byte_count++;
      continue;
    }

    if (is_in_double_quotes) did_quote_enclose_content = true;

    if (!quote_char.has_value() && lexer::is_string_quote(ch)) {
      relative_last_quote_position = byte_count;
      did_quote_enclose_content = false;
      quote_char = ch;
      byte_count++;
      continue;
    }

    if (ch == '$') {
      let const expansion_start = byte_count;
      byte_count++;
      char next = chop_character(byte_count);

      /* $'...' is bash ANSI-C quoting, decoded here into a literal segment that
         neither expands nor globs. It rides every mood but POSIX. Inside double
         quotes the $' is literal, so bash leaves "$'x'" as the three bytes. */
      if (next == '\'' && bash_additions_enabled() && !is_in_double_quotes) {
        byte_count++;
        let ansi_body = String{heap_allocator()};
        loop
        {
          let const c = chop_character(byte_count);
          if (c == lexer::CEOF) {
            throw ErrorWithLocationAndDetails{
                here(m_cursor_position, byte_count),
                "Unterminated $'...' string",
                here(m_cursor_position + byte_count, 1), "expected ' here"};
          }
          byte_count++;
          if (c == '\'') break;
          ansi_body.push(c);
          if (c == '\\') {
            let const escaped = chop_character(byte_count);
            if (escaped == lexer::CEOF) break;
            byte_count++;
            ansi_body.push(escaped);
          }
        }

        let decoded = String{heap_allocator()};
        utils::decode_ansi_c_escapes(decoded, ansi_body.view());

        /* An empty $'' still produces one empty field, the way '' and "" do. */
        if (decoded.is_empty()) {
          word.segments.push(WordSegment{WordSegment::Kind::LiteralText,
                                         String{heap_allocator()}, false});
        } else {
          for (usize k = 0; k < decoded.count(); k++)
            do_append_char(WordSegment::Kind::LiteralText, decoded[k]);
        }

        word.segments.back().was_ansi_c_quoted = true;
        continue;
      }

      /* $"..." is bash locale translation. With no catalog it is the plain
         double-quoted string, so the dollar is dropped. It rides every mood but
         POSIX and only at the top level, since inside a double quote $" is a
         dollar then the close quote. */
      if (next == '"' && bash_additions_enabled() && !is_in_double_quotes) {
        continue;
      }

      if (next == '(') {
        byte_count++;

        /* $(( is arithmetic expansion, a subshell substitution needs the space
           of $( (cmd) ). */
        if (chop_character(byte_count) == '(') {
          byte_count++;
          let arithmetic = String{heap_allocator()};
          usize group_depth = 0;
          loop
          {
            let const c = chop_character(byte_count);
            if (c == lexer::CEOF) [[unlikely]] {
              throw ErrorWithLocationAndDetails{
                  here(m_cursor_position, byte_count),
                  "Unterminated arithmetic expansion",
                  here(m_cursor_position + byte_count, 1), "expected )) here"};
            }
            /* A backslash escape, a quoted span, a backtick run, and a nested
               $(...) are copied as balanced units so a ) inside them is text.
             */
            if (c == '\\') {
              arithmetic += c;
              byte_count++;
              let const escaped = chop_character(byte_count);
              if (escaped != lexer::CEOF) {
                arithmetic += escaped;
                byte_count++;
              }
            } else if (c == '\'' || c == '"') {
              let const quote = c;
              arithmetic += c;
              byte_count++;
              loop
              {
                let const q = chop_character(byte_count);
                if (q == lexer::CEOF) break;
                arithmetic += q;
                byte_count++;
                if (quote == '"' && q == '\\') {
                  let const escaped = chop_character(byte_count);
                  if (escaped != lexer::CEOF) {
                    arithmetic += escaped;
                    byte_count++;
                  }
                  continue;
                }
                if (q == quote) break;
              }
            } else if (c == '`') {
              arithmetic += c;
              byte_count++;
              loop
              {
                let const b = chop_character(byte_count);
                if (b == lexer::CEOF) break;
                arithmetic += b;
                byte_count++;
                if (b == '\\') {
                  let const escaped = chop_character(byte_count);
                  if (escaped != lexer::CEOF) {
                    arithmetic += escaped;
                    byte_count++;
                  }
                  continue;
                }
                if (b == '`') break;
              }
            } else if (c == '$' && chop_character(byte_count + 1) == '(') {
              arithmetic += c;
              byte_count++;
              arithmetic += chop_character(byte_count);
              byte_count++;
              usize paren_depth = 1;
              char nested_quote = 0;
              loop
              {
                let const p = chop_character(byte_count);
                if (p == lexer::CEOF) break;
                arithmetic += p;
                byte_count++;
                if (nested_quote != 0) {
                  if (nested_quote == '"' && p == '\\') {
                    let const escaped = chop_character(byte_count);
                    if (escaped != lexer::CEOF) {
                      arithmetic += escaped;
                      byte_count++;
                    }
                    continue;
                  }
                  if (p == nested_quote) nested_quote = 0;
                  continue;
                }
                if (p == '\\') {
                  let const escaped = chop_character(byte_count);
                  if (escaped != lexer::CEOF) {
                    arithmetic += escaped;
                    byte_count++;
                  }
                  continue;
                }
                if (p == '\'' || p == '"') {
                  nested_quote = p;
                } else if (p == '(') {
                  paren_depth++;
                } else if (p == ')') {
                  paren_depth--;
                  if (paren_depth == 0) break;
                }
              }
            } else if (c == '(') {
              group_depth++;
              arithmetic += c;
              byte_count++;
            } else if (c == ')' && group_depth > 0) {
              group_depth--;
              arithmetic += c;
              byte_count++;
            } else if (c == ')' && chop_character(byte_count + 1) == ')') {
              byte_count += 2;
              break;
            } else {
              arithmetic += c;
              byte_count++;
            }
          }
          word.segments.push(WordSegment{WordSegment::Kind::ArithmeticExpansion,
                                         steal(arithmetic),
                                         is_in_double_quotes});
          word.segments.back().set_source_span(
              m_cursor_position + expansion_start + 3,
              word.segments.back().text.count());
          continue;
        }

        let const inner_start = m_cursor_position + byte_count;
        let const substitution_end =
            lexer::scan_balanced_shell_region(m_source, inner_start, ')');
        if (!substitution_end.has_value()) [[unlikely]] {
          throw ErrorWithLocationAndDetails{
              here(m_cursor_position, m_source.count() - m_cursor_position),
              "Unterminated command substitution", here(m_source.count(), 1),
              "expected ) here"};
        }
        let inner = String{m_source.substring_of_length(
            inner_start, *substitution_end - inner_start - 1)};
        byte_count = *substitution_end - m_cursor_position;
        word.segments.push(WordSegment{WordSegment::Kind::CommandSubstitution,
                                       steal(inner), is_in_double_quotes});
        word.segments.back().set_source_span(
            m_cursor_position + expansion_start, byte_count - expansion_start);
      } else if (next == '{') {
        byte_count++;
        let name = String{heap_allocator()};
        /* A ${ followed by whitespace is the bash 5.3 funsub, a command body
           run in the current shell. The leading whitespace drops. */
        bool is_function_substitution = false;
        if (bash_additions_enabled()) {
          let probe = chop_character(byte_count);
          while (probe == ' ' || probe == '\t' || probe == '\n') {
            is_function_substitution = true;
            byte_count++;
            probe = chop_character(byte_count);
          }
        }
        /* Only a nested ${ raises the depth, so a bare { does not, matching
           dash. A nested $(...), backtick, quote, or escape shields its }. */
        usize brace_depth = 1;
        char quote = 0;
        loop
        {
          let const c = chop_character(byte_count);
          if (c == lexer::CEOF) [[unlikely]] {
            throw ErrorWithLocationAndDetails{
                here(m_cursor_position + byte_count, 1),
                "Unterminated variable expansion",
                here(m_cursor_position + byte_count, 1), "expected } here"};
          }
          byte_count++;

          if (quote == '\'') {
            if (c == '\'') quote = 0;
            name += c;
            continue;
          }
          if (c == '\\') {
            name += c;
            let const escaped = chop_character(byte_count);
            if (escaped != lexer::CEOF) {
              byte_count++;
              name += escaped;
            }
            continue;
          }
          if (quote == '"') {
            if (c == '"') quote = 0;
            name += c;
            continue;
          }
          if (c == '\'' || c == '"') {
            quote = c;
            name += c;
            continue;
          }
          if (c == '`') {
            name += c;
            loop
            {
              let const b = chop_character(byte_count);
              if (b == lexer::CEOF) break;
              byte_count++;
              name += b;
              if (b == '\\') {
                let const escaped = chop_character(byte_count);
                if (escaped != lexer::CEOF) {
                  byte_count++;
                  name += escaped;
                }
                continue;
              }
              if (b == '`') break;
            }
            continue;
          }
          if (c == '$' && chop_character(byte_count) == '(') {
            name += c;
            name += chop_character(byte_count);
            byte_count++;
            usize paren_depth = 1;
            char nested_quote = 0;
            loop
            {
              let const p = chop_character(byte_count);
              if (p == lexer::CEOF) break;
              byte_count++;
              name += p;
              if (nested_quote != 0) {
                if (nested_quote == '"' && p == '\\') {
                  let const escaped = chop_character(byte_count);
                  if (escaped != lexer::CEOF) {
                    byte_count++;
                    name += escaped;
                  }
                  continue;
                }
                if (p == nested_quote) nested_quote = 0;
                continue;
              }
              if (p == '\\') {
                let const escaped = chop_character(byte_count);
                if (escaped != lexer::CEOF) {
                  byte_count++;
                  name += escaped;
                }
                continue;
              }
              if (p == '\'' || p == '"') {
                nested_quote = p;
              } else if (p == '(') {
                paren_depth++;
              } else if (p == ')') {
                paren_depth--;
                if (paren_depth == 0) break;
              }
            }
            continue;
          }
          if (c == '$' && chop_character(byte_count) == '{') {
            brace_depth++;
            name += c;
            name += chop_character(byte_count);
            byte_count++;
            continue;
          }
          /* In a funsub body a bare { opens a brace group whose } must not
             close the substitution. */
          if (c == '{' && is_function_substitution) {
            brace_depth++;
            name += c;
            continue;
          }
          if (c == '}') {
            brace_depth--;
            if (brace_depth == 0) break;
            name += c;
            continue;
          }
          name += c;
        }
        word.segments.push(WordSegment{
            is_function_substitution ? WordSegment::Kind::FunctionSubstitution
                                     : WordSegment::Kind::VariableReference,
            steal(name), is_in_double_quotes});
        let &expansion_segment = word.segments.back();
        if (is_function_substitution)
          expansion_segment.set_source_span(m_cursor_position + expansion_start,
                                            byte_count - expansion_start);
        else
          expansion_segment.set_source_span(m_cursor_position +
                                                expansion_start + 2,
                                            expansion_segment.text.count());
      } else if (lexer::is_variable_name_start(next)) {
        let name = String{heap_allocator()};
        while (lexer::is_variable_name(next = chop_character(byte_count))) {
          name += next;
          byte_count++;
        }
        word.segments.push(WordSegment{WordSegment::Kind::VariableReference,
                                       steal(name), is_in_double_quotes, true});
        word.segments.back().set_source_span(m_cursor_position +
                                                 expansion_start + 1,
                                             word.segments.back().text.count());
      } else if (lexer::is_special_parameter_char(next) ||
                 lexer::is_number(next))
      {
        byte_count++;
        let special = String{heap_allocator()};
        special.push(next);
        word.segments.push(WordSegment{WordSegment::Kind::VariableReference,
                                       steal(special), is_in_double_quotes});
        word.segments.back().set_source_span(
            m_cursor_position + expansion_start + 1, 1);
      } else {
        do_append_char(is_in_double_quotes ? WordSegment::Kind::DoubleQuotedText
                                           : WordSegment::Kind::UnquotedText,
                       '$');
      }
      continue;
    }

    if (ch == '`') {
      /* The POSIX backquote unescaping strips a backslash before a backtick, a
         dollar sign, another backslash, or, inside double quotes, a double
         quote, so a \" inside a quoted backtick opens an inner quoted span. */
      let const relative_open_backtick_pos = byte_count;
      byte_count++;
      let inner = String{heap_allocator()};
      loop
      {
        let const c = chop_character(byte_count);
        if (c == lexer::CEOF) [[unlikely]] {
          throw ErrorWithLocationAndDetails{
              here(m_cursor_position + relative_open_backtick_pos, 1),
              "Unterminated command substitution",
              here(m_cursor_position + byte_count, 1), "expected ` here"};
        }
        if (c == '`') {
          byte_count++;
          break;
        }
        if (c == '\\') {
          let const escaped = chop_character(byte_count + 1);
          bool is_stripped_escape = false;
          switch (escaped) {
          case '`':
          case '$':
          case '\\': is_stripped_escape = true; break;
          case '"': is_stripped_escape = is_in_double_quotes; break;
          default: break;
          }

          if (is_stripped_escape) {
            inner += escaped;
            byte_count += 2;
            continue;
          }
        }
        inner += c;
        byte_count++;
      }
      word.segments.push(WordSegment{WordSegment::Kind::CommandSubstitution,
                                     steal(inner), is_in_double_quotes});
      word.segments.back().set_source_span(
          m_cursor_position + relative_open_backtick_pos,
          byte_count - relative_open_backtick_pos);
      continue;
    }

    do_append_char(is_in_double_quotes ? WordSegment::Kind::DoubleQuotedText
                                       : WordSegment::Kind::UnquotedText,
                   ch);
    byte_count++;
  }

  if (quote_char.has_value()) [[unlikely]] {
    let expected_quote = String{heap_allocator()};
    expected_quote += "expected ";
    expected_quote += *quote_char;
    expected_quote += " here";
    throw ErrorWithLocationAndDetails{
        here(m_cursor_position + relative_last_quote_position,
             sub_sat(byte_count, relative_last_quote_position)),
        "Unterminated string literal", here(m_cursor_position + byte_count, 1),
        expected_quote};
  }

  if (should_escape) [[unlikely]] {
    throw ErrorWithLocationAndDetails{
        here(m_cursor_position + byte_count - 1, 1), "Nothing to escape",
        here(m_cursor_position + byte_count, 1), "expected a character here"};
  }

  let const actual_cursor_position = m_cursor_position;
  ASSERT(actual_cursor_position <= m_source.length);

  let const is_cache_in_function_arena = m_arena == FUNCTION_ARENA;
  for (let &segment : word.segments)
    segment.is_substitution_cache_in_function_arena =
        is_cache_in_function_arena;

  if (m_should_collect_debug_words &&
      m_cursor_position != m_last_collected_word_position)
  {
    m_debug_words.push(word);
    m_last_collected_word_position = m_cursor_position;
  }

  Token *token{};

  if (let assignment_split = word.get_assignment_split();
      assignment_split.has_value())
  {
    assignment_split->value.segments.move_to_allocator(
        bump_allocator(*m_arena));
    token = m_arena->create<tokens::Assignment>(
        here(actual_cursor_position, byte_count), assignment_split->name,
        steal(assignment_split->value), assignment_split->is_append);
  } else if (word.segments.count() == 1 &&
             word.segments[0].kind == WordSegment::Kind::UnquotedText)
  {
    let const &word_text = word.segments[0].text;
    let const keyword =
        KEYWORDS.find(StringView{word_text.data(), word_text.count()});
    if (keyword.has_value()) {
      switch (*keyword) {
        KW_SWITCH_CASES();
      default: unreachable("unhandled keyword of type %d", ENUM(*keyword));
      }
    }
  }

  if (token == nullptr) {
    token = tokens::create_word_token(
        *m_arena, here(actual_cursor_position, byte_count), steal(word));
  }

  m_cached_offset = byte_count;

  return token;
}

hot alwaysinline fn Lexer::lex_sentinel() throws -> Token *
{
  let const ch = chop_character();
  ASSERT(ch != lexer::CEOF);

  usize extra_length = 0;

  Token *token{};

#define TOKEN_CASE_ONE(byte, t)                                                \
  case byte:                                                                   \
    token = m_arena->create<tokens::t>(here(m_cursor_position, 1));            \
    break;

#define TOKEN_CASE_TWO(byte, t, ch, t2)                                        \
  case byte: {                                                                 \
    if (chop_character(1) == ch) {                                             \
      token = m_arena->create<tokens::t2>(here(m_cursor_position, 2));         \
      extra_length++;                                                          \
    } else {                                                                   \
      token = m_arena->create<tokens::t>(here(m_cursor_position, 1));          \
    }                                                                          \
  } break;

#define TOKEN_CASE_THREE(byte, t, ch2, t2, ch3, t3)                            \
  case byte: {                                                                 \
    if (chop_character(1) == ch2) {                                            \
      token = m_arena->create<tokens::t2>(here(m_cursor_position, 2));         \
      extra_length++;                                                          \
    } else if (chop_character(1) == ch3) {                                     \
      token = m_arena->create<tokens::t3>(here(m_cursor_position, 2));         \
      extra_length++;                                                          \
    } else {                                                                   \
      token = m_arena->create<tokens::t>(here(m_cursor_position, 1));          \
    }                                                                          \
  } break;

  switch (ch) {
    TOKEN_CASE_ONE(')', RightParen);
    TOKEN_CASE_ONE('(', LeftParen);
  case ';': {
    if (chop_character(1) == ';') {
      if (chop_character(2) == '&') {
        token = m_arena->create<tokens::DoubleSemicolonAmpersand>(
            here(m_cursor_position, 3));
        extra_length += 2;
      } else {
        token = m_arena->create<tokens::DoubleSemicolon>(
            here(m_cursor_position, 2));
        extra_length++;
      }
    } else if (chop_character(1) == '&') {
      token = m_arena->create<tokens::SemicolonAmpersand>(
          here(m_cursor_position, 2));
      extra_length++;
    } else {
      token = m_arena->create<tokens::Semicolon>(here(m_cursor_position, 1));
    }
  } break;
    TOKEN_CASE_ONE('.', Dot);
    TOKEN_CASE_ONE('\n', Newline);
    TOKEN_CASE_ONE('+', Plus);
    TOKEN_CASE_ONE('-', Minus);
    TOKEN_CASE_ONE('*', Asterisk);
    TOKEN_CASE_ONE('/', Slash);
    TOKEN_CASE_ONE('%', Percent);
    TOKEN_CASE_ONE('~', Tilde);
    TOKEN_CASE_ONE('^', Cap);

    TOKEN_CASE_TWO('!', ExclamationMark, '=', ExclamationEquals);
  /* &> and &>> redirect both streams to a file, riding every mood but POSIX.
   */
  case '&': {
    if (bash_additions_enabled() && chop_character(1) == '>') {
      if (chop_character(2) == '>') {
        token = m_arena->create<tokens::AmpersandDoubleGreater>(
            here(m_cursor_position, 3));
        extra_length += 2;
      } else {
        token = m_arena->create<tokens::AmpersandGreater>(
            here(m_cursor_position, 2));
        extra_length++;
      }
    } else if (chop_character(1) == '&') {
      token =
          m_arena->create<tokens::DoubleAmpersand>(here(m_cursor_position, 2));
      extra_length++;
    } else {
      token = m_arena->create<tokens::Ampersand>(here(m_cursor_position, 1));
    }
  } break;

  /* |& is the shorthand for 2>&1 |, riding every mood but POSIX. */
  case '|': {
    if (chop_character(1) == '|') {
      token = m_arena->create<tokens::DoublePipe>(here(m_cursor_position, 2));
      extra_length++;
    } else if (bash_additions_enabled() && chop_character(1) == '&') {
      token =
          m_arena->create<tokens::PipeAmpersand>(here(m_cursor_position, 2));
      extra_length++;
    } else {
      token = m_arena->create<tokens::Pipe>(here(m_cursor_position, 1));
    }
  } break;
    TOKEN_CASE_TWO('=', Equals, '=', DoubleEquals);

    TOKEN_CASE_THREE('>', Greater, '>', DoubleGreater, '=', GreaterEquals);

  /* <<< is the bash here-string, riding every mood but POSIX where it stays
     << then <. */
  case '<': {
    if (chop_character(1) == '<') {
      if (chop_character(2) == '<' && bash_additions_enabled()) {
        token = m_arena->create<tokens::TripleLess>(here(m_cursor_position, 3));
        extra_length += 2;
      } else {
        token = m_arena->create<tokens::DoubleLess>(here(m_cursor_position, 2));
        extra_length++;
      }
    } else if (chop_character(1) == '=') {
      token = m_arena->create<tokens::LessEquals>(here(m_cursor_position, 2));
      extra_length++;
    } else {
      token = m_arena->create<tokens::Less>(here(m_cursor_position, 1));
    }
  } break;

  default: {
    let source_text = String{heap_allocator()};
    source_text += "Unknown operator '";
    source_text += ch;
    source_text += "'";
    throw ErrorWithLocation{here(m_cursor_position, 1), source_text};
  }
  }

  ASSERT(token != nullptr);

  m_cached_offset = 1 + extra_length;

  return token;
}

hot alwaysinline fn Lexer::lex_process_substitution(char direction) throws
    -> Token *
{
  let const open_position = m_cursor_position;
  usize byte_count = 2;

  /* The direction byte leads the segment text so the evaluator reads the pipe
     direction without a second field. */
  let inner = String{heap_allocator()};
  inner += direction;

  usize depth = 1;
  char quote = 0;
  loop
  {
    let const c = chop_character(byte_count);
    if (c == lexer::CEOF) [[unlikely]] {
      throw ErrorWithLocationAndDetails{
          here(open_position, byte_count), "Unterminated process substitution",
          here(open_position + byte_count, 1), "expected ) here"};
    }
    byte_count++;

    if (quote != 0) {
      if (c == quote) quote = 0;
      inner += c;
      continue;
    }
    if (c == '\\') {
      inner += c;
      let const escaped = chop_character(byte_count);
      if (escaped != lexer::CEOF) {
        byte_count++;
        inner += escaped;
      }
      continue;
    }
    if (c == '\'' || c == '"') {
      quote = c;
      inner += c;
      continue;
    }
    if (c == '(') {
      depth++;
      inner += c;
      continue;
    }
    if (c == ')') {
      depth--;
      if (depth == 0) break;
      inner += c;
      continue;
    }
    inner += c;
  }

  LOG(Debug, "capturing a process substitution of %zu bytes", byte_count);

  let word = Word{};
  word.segments.push(
      WordSegment{WordSegment::Kind::ProcessSubstitution, steal(inner), false});
  word.segments.back().set_source_span(open_position, byte_count);
  let t = tokens::create_word_token(*m_arena, here(open_position, byte_count),
                                    steal(word));
  m_cached_offset = byte_count;
  return t;
}

} /* namespace koshka */
