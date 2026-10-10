/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file tokenizes shell source, including quotes, expansions,
 * assignments, redirections, process substitutions, and heredocs. It owns
 * the lexer cursor, token lookahead, source locations, analysis metadata
 * collection, and the nested syntax check of substitution bodies. LexerScan
 * provides the allocation-light balanced scanner shared by tokenization,
 * formatting, and highlighting.
 */

#include "Lexer.hpp"

#include "Errors.hpp"
#include "Parser.hpp"
#include "Toiletline.hpp"
#include "Tokens.hpp"
#include "Utils.hpp"
#include "base/Arena.hpp"
#include "base/Common.hpp"
#include "base/Debug.hpp"
#include "base/Trace.hpp"

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

pure fn word_is_variable_name(StringView word) wontthrow -> bool
{
  if (word.is_empty() || !is_variable_name_start(word[0])) return false;

  for (usize position = 1; position < word.length; position++) {
    if (!is_variable_name(word[position])) return false;
  }

  return true;
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
    let const byte = word[position];
    if (byte == '[') {
      bracket_depth++;
    } else if (byte == ']') {
      bracket_depth--;
    } else {
      position = skip_quoted_run(word, position);
    }
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

pure fn is_backtick_escape_stripped(char escaped,
                                    bool is_in_double_quotes) wontthrow -> bool
{
  switch (escaped) {
  case '`':
  case '$':
  case '\\': return true;
  case '"': return is_in_double_quotes;
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

Lexer::Lexer(StringView source, BumpArena &arena, Maybe<StringView> filename,
             mimic_mood mood, ParseSession::AllocationKind allocation_kind,
             debug_word_collection_mode debug_words)
    : m_source(source), m_parse_session(arena)
{
  m_parse_session.set_arena(arena, allocation_kind);
  m_parse_session.set_source_name_index(
      filename.has_value() ? intern_source_name(*filename) : 0);
  m_parse_session.set_mood(mood);
  m_parse_session.set_debug_word_collection_mode(debug_words);
  LOG(Debug, "starting a lexer over %zu bytes of source", m_source.length);
}

Lexer::~Lexer() = default;

fn Lexer::peek_shell_token() throws -> Token *
{
  if (peek_cache_is_live()) return m_peek_cache;

  skip_whitespace();
  Token *const token = lex_shell_token();
  m_peek_cache = token;
#if !defined NDEBUG
  m_peek_cache_generation = m_parse_session.get_arena().reset_generation();
#endif

  return token;
}

hot fn Lexer::next_shell_token() throws -> Token *
{
  let const is_peek_cache_live = peek_cache_is_live();
  if (!is_peek_cache_live) skip_whitespace();

  Token *const token = is_peek_cache_live ? m_peek_cache : lex_shell_token();
  ASSERT(token != nullptr);

  advance_past_last_peek();

  return token;
}

pure fn Lexer::source() const wontthrow -> StringView { return m_source; }

pure fn Lexer::source_name_index() const wontthrow -> u32
{
  return m_parse_session.source_name_index();
}

pure fn Lexer::cursor_position() const wontthrow -> usize
{
  return m_cursor_position;
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

pure fn Lexer::arena() const wontthrow -> BumpArena &
{
  return m_parse_session.get_arena();
}

fn Lexer::set_arena(BumpArena &arena,
                    ParseSession::AllocationKind allocation_kind) wontthrow
    -> void
{
  LOG(Debug, "switching the lexer arena and dropping the cached peek");
  m_parse_session.set_arena(arena, allocation_kind);
  drop_peek_cache();
}

fn Lexer::drop_peek_cache() wontthrow -> void { m_peek_cache = nullptr; }

fn Lexer::peek_cache_is_live() const wontthrow -> bool
{
  if (m_peek_cache == nullptr) return false;

#if !defined NDEBUG
  ASSERT(m_peek_cache_generation ==
             m_parse_session.get_arena().reset_generation(),
         "the arena was rewound without dropping the cached peek");
#endif

  return true;
}

hot fn Lexer::advance_past_last_peek() throws -> usize
{
  ASSERT(m_cursor_position + m_cached_offset <= m_source.length);

  let const result = advance_forward(m_cached_offset);
  m_cached_offset = 0;
  m_peek_cache = nullptr;

  if (m_last_shell_token_was_newline && !m_pending_heredocs.is_empty()) {
    m_last_shell_token_was_newline = false;
    collect_pending_heredocs();
  }

  return result;
}

cold fn Lexer::register_heredoc(StringView delimiter,
                                heredoc_tab_policy tab_policy,
                                bool should_expand) throws
    -> const heredoc_contents *
{
  let &arena = m_parse_session.get_arena();
  let contents = arena.create<heredoc_contents>(
      bump_allocator(arena), tab_policy == heredoc_tab_policy::Preserve
                                 ? heredoc_source_mapping::Contiguous
                                 : heredoc_source_mapping::Transformed);
  ASSERT(contents != nullptr);

  LOG(Debug, "registering a pending heredoc with delimiter '%.*s'",
      static_cast<int>(delimiter.length), delimiter.data);

  m_pending_heredocs.push(
      {String{delimiter}, contents, tab_policy, should_expand});

  return contents;
}

template <class Emit>
cold fn Lexer::walk_heredoc_body(usize start, StringView delimiter,
                                 heredoc_tab_policy tab_policy,
                                 Emit emit_line) throws -> usize
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
    let const line_remaining = m_source.substring(line_start);
    let const line_newline = line_remaining.find_character('\n');
    let const line_end_position =
        line_start + line_newline.value_or(line_remaining.length);
    let const has_newline = line_end_position < m_source.length;
    position = has_newline ? line_end_position + 1 : line_end_position;

    let const line_offset = line_start;
    let const line_length = line_end_position - line_start;
    usize stripped_offset = line_offset;
    usize stripped_length = line_length;
    if (tab_policy == heredoc_tab_policy::Strip) {
      while (stripped_length > 0 && m_source[stripped_offset] == '\t') {
        stripped_offset++;
        stripped_length--;
      }
    }

    let const stripped =
        m_source.substring_of_length(stripped_offset, stripped_length);
    let const is_delimiter = (delimiter == stripped);
    did_find_delimiter = did_find_delimiter || is_delimiter;

    if (m_parse_session.should_collect_analysis_metadata() && !is_delimiter &&
        !has_near_miss && line_length > delimiter.length)
    {
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
      if (pending.tab_policy == heredoc_tab_policy::Strip) {
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
                          pending.tab_policy, do_append_body_line);
    pending.contents->source_end_position = m_cursor_position;
    LOG(Debug, "capturing a heredoc body of %zu bytes for delimiter '%s'",
        collected.count(), pending.delimiter.c_str());
    pending.contents->text = steal(collected);
  }

  defer { m_pending_heredocs.clear(); };
  if (!should_validate_substitutions()) return;

  for (let const &pending : m_pending_heredocs) {
    if (!pending.should_expand) continue;

    validate_nested_expansions(pending.contents->source_position,
                               pending.contents->source_end_position -
                                   pending.contents->source_position,
                               true);
  }
}

hot flatten fn Lexer::lex_shell_token() throws -> Token *
{
  Token *token{};
  let const ch = chop_character();
  if (!has_character()) {
    token = m_parse_session.get_arena().create<tokens::EndOfFile>(
        here(m_cursor_position, 1));
    m_last_shell_token_was_newline = false;

    return token;
  }

  switch (ch) {
  case '<':
  case '>':
    token = chop_character(1) == '(' ? lex_identifier() : lex_sentinel();
    break;
  case '\n':
  case '|':
  case '(':
  case ')':
  case '&':
  case ';': token = lex_sentinel(); break;
  case ' ':
  case '\t':
    throw ErrorWithLocationAndDetails{
        here(m_cursor_position, 1), "Unexpected character",
        "the character is not valid in an unquoted word here"};
  default: token = lex_identifier(); break;
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

    switch (byte) {
    case '\\':
      if (chop_character(i + 1) == '\n') {
        i += 2;
        continue;
      }
      break;
    case '#': {
      let const comment_start = i;
      let const comment_remaining = m_source.substring(m_cursor_position + i);
      i += comment_remaining.find_character('\n').value_or(
          comment_remaining.length);
      if (m_parse_session.should_collect_analysis_metadata()) {
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

          if (m_parse_session.should_collect_shellcheck_directives())
            m_pending_shellcheck_directives.push(span);

          m_shellcheck_directive_spans.push(span);
        }
      }
      continue;
    }
    default: break;
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

hot alwaysinline fn Lexer::has_character(usize offset) const wontthrow -> bool
{
  return m_cursor_position + offset < m_source.length;
}

flatten hot alwaysinline fn Lexer::lex_identifier() throws -> Token *
{
  let word = Word{};
  word.segments = ArrayList<WordSegment>{bump_allocator(arena())};

  usize byte_count = 0;
  usize relative_last_quote_position = 0;

  bool should_escape = false;

  Maybe<char> quote_char;

  bool did_quote_enclose_content = false;

  let const do_append_char = [&word](WordSegment::Kind kind, char ch) {
    if (!word.segments.is_empty() && word.segments.back().kind == kind &&
        kind != WordSegment::Kind::VariableReference)
    {
      word.segments.back().text.push(ch);
    } else {
      word.segments.push(WordSegment{
          kind, SegmentText{heap_allocator(), StringView{&ch, 1}},
           false
      });
    }
  };

  let const do_append_run = [&word, this](WordSegment::Kind kind,
                                          StringView run) {
    if (!word.segments.is_empty() && word.segments.back().kind == kind &&
        kind != WordSegment::Kind::VariableReference)
    {
      word.segments.back().text.append(run);
    } else {
      word.segments.push(WordSegment{
          kind, SegmentText{bump_allocator(arena()), run},
           false
      });
    }
  };

  let const do_append_unquoted_run = [&do_append_run](StringView run) {
    do_append_run(WordSegment::Kind::UnquotedText, run);
  };

  let const do_word_is_plain_array_name = [&word]() -> bool {
    if (word.segments.count() != 1) return false;
    let const &segment = word.segments[0];
    if (segment.kind != WordSegment::Kind::UnquotedText ||
        segment.text.is_empty())
    {
      return false;
    }
    return lexer::word_is_variable_name(segment.text.view());
  };

  let const do_subscript_closes_with_assignment =
      [this](usize start) -> Maybe<usize> {
    usize offset = start + 1;
    usize depth = 1;
    while (depth > 0) {
      if (!has_character(offset)) return None;

      let const c = chop_character(offset);
      offset++;
      if (c == '[') {
        depth++;
      } else if (c == ']') {
        depth--;
      } else {
        let const skipped = lexer::skip_quoted_run(
            m_source.substring(m_cursor_position), offset - 1);
        if (skipped == offset - 1 && (c == '\'' || c == '"')) return None;

        offset = skipped + 1;
      }
    }
    let const after = chop_character(offset);
    if (after == '=' || (after == '+' && chop_character(offset + 1) == '='))
      return offset;
    return None;
  };

  usize extglob_depth = 0;

  usize element_subscript_end = 0;
  if (m_is_lexing_array_literal && chop_character(0) == '[') {
    let const remaining = m_source.substring(m_cursor_position);
    usize depth = 0;
    for (usize offset = 0; offset < remaining.length; offset++) {
      let const c = remaining[offset];
      if (c == '\n') break;

      if (c == '[') {
        depth++;
      } else if (c == ']') {
        depth--;
        if (depth == 0) {
          element_subscript_end = offset;
          break;
        }
      } else {
        offset = lexer::skip_quoted_run(remaining, offset);
      }
    }
  }

  loop
  {
    let const ch = chop_character(byte_count);
    let const is_at_end = !has_character(byte_count);

    let const is_inside_quote_or_escape =
        quote_char.has_value() || should_escape;
    if (!is_inside_quote_or_escape && byte_count < element_subscript_end &&
        lexer::is_whitespace(ch))
    {
      do_append_unquoted_run(
          m_source.substring_of_length(m_cursor_position + byte_count, 1));
      byte_count++;
      continue;
    }

    if (is_at_end ||
        (!is_inside_quote_or_escape && !lexer::is_part_of_identifier(ch)))
    {
      if (extglob_depth == 0 && (ch == '<' || ch == '>') &&
          chop_character(byte_count + 1) == '(')
      {
        byte_count = lex_process_substitution(word, byte_count);
        continue;
      }

      if (extglob_depth == 0 || is_at_end) break;

      if (ch == '(')
        extglob_depth++;
      else if (ch == ')')
        extglob_depth--;
      do_append_unquoted_run(
          m_source.substring_of_length(m_cursor_position + byte_count, 1));
      byte_count++;
      continue;
    }

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
        if (should_validate_substitutions()) {
          validate_nested_expansions(m_cursor_position + subscript_start,
                                     byte_count - subscript_start, false);
        }
        continue;
      }
    }

    if (!is_inside_quote_or_escape && lexer::is_extglob_operator(ch) &&
        chop_character(byte_count + 1) == '(')
    {
      do_append_unquoted_run(
          m_source.substring_of_length(m_cursor_position + byte_count, 2));
      byte_count += 2;
      extglob_depth++;
      continue;
    }

    if (!is_inside_quote_or_escape && lexer::is_plain_unquoted_run_byte(ch)) {
      let const run_start = byte_count;
      loop
      {
        byte_count++;
        let const next = chop_character(byte_count);
        if (!has_character(byte_count) || next == '[' ||
            !lexer::is_plain_unquoted_run_byte(next))
        {
          break;
        }
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
                                         SegmentText{}, false});
        quote_char.reset();
      } else {
        let const run_start = byte_count;
        let const quote_remaining =
            m_source.substring(m_cursor_position + byte_count);
        byte_count += quote_remaining.find_character('\'').value_or(
            quote_remaining.length);
        do_append_run(
            WordSegment::Kind::LiteralText,
            m_source.substring_of_length(m_cursor_position + run_start,
                                         byte_count - run_start));
        did_quote_enclose_content = true;
        continue;
      }
      byte_count++;
      continue;
    }

    if (ch == '\\') {
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
                                       SegmentText{}, false});
      quote_char.reset();
      byte_count++;
      continue;
    }

    if (is_in_double_quotes) did_quote_enclose_content = true;

    if (is_in_double_quotes && ch != '$' && ch != '`') {
      let const run_start = byte_count;
      while (true) {
        let const next = chop_character(byte_count);
        if (!has_character(byte_count) || next == '"' || next == '\\' ||
            next == '$' || next == '`')
        {
          break;
        }
        byte_count++;
      }
      do_append_run(WordSegment::Kind::DoubleQuotedText,
                    m_source.substring_of_length(m_cursor_position + run_start,
                                                 byte_count - run_start));
      continue;
    }

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

      if (next == '\'' && bash_additions_enabled() && !is_in_double_quotes) {
        byte_count++;
        let const ansi_body_start = byte_count;
        loop
        {
          if (!has_character(byte_count)) {
            throw ErrorWithLocationAndDetails{
                here(m_cursor_position, byte_count),
                "Unterminated $'...' string",
                here(m_cursor_position + byte_count, 1), "expected ' here"};
          }

          let const c = chop_character(byte_count);
          byte_count++;
          if (c == '\'') break;
          if (c == '\\' && has_character(byte_count)) byte_count++;
        }

        let decoded = String{heap_allocator()};
        utils::decode_ansi_c_escapes(
            decoded,
            m_source.substring_of_length(m_cursor_position + ansi_body_start,
                                         byte_count - ansi_body_start - 1));

        if (decoded.is_empty()) {
          word.segments.push(WordSegment{WordSegment::Kind::LiteralText,
                                         SegmentText{}, false});
        } else {
          do_append_run(WordSegment::Kind::LiteralText, decoded.view());
        }

        word.segments.back().was_ansi_c_quoted = true;
        continue;
      }

      if (next == '"' && !is_in_double_quotes) {
        if (is_posix_mode())
          do_append_run(WordSegment::Kind::UnquotedText, StringView{"$", 1});
        word.has_locale_translation_quote = true;
        continue;
      }

      let const is_bracket_arithmetic = next == '[' && bash_additions_enabled();
      if (is_bracket_arithmetic ||
          (next == '(' && chop_character(byte_count + 1) == '('))
      {
        byte_count += is_bracket_arithmetic ? 1 : 2;
        let arithmetic_start = byte_count;
        let closing_length = is_bracket_arithmetic ? usize{1} : usize{2};
        usize group_depth = 0;
        loop
        {
          let const c = chop_character(byte_count);
          if (is_bracket_arithmetic && is_in_double_quotes && c == '"') {
            arithmetic_start--;
            closing_length = 0;
            break;
          }
          if (!has_character(byte_count)) rarely
            {
              throw ErrorWithLocationAndDetails{
                  here(m_cursor_position, byte_count),
                  "Unterminated arithmetic expansion",
                  here(m_cursor_position + byte_count, 1),
                  is_bracket_arithmetic ? "expected ] here"
                                        : "expected )) here"};
            }
          if (c == '\\') {
            byte_count++;
            if (has_character(byte_count)) {
              byte_count++;
            }
          } else if (c == '\'' || c == '"') {
            let const quote = c;
            byte_count++;
            loop
            {
              if (!has_character(byte_count)) break;

              let const q = chop_character(byte_count);
              byte_count++;
              if (quote == '"' && q == '\\') {
                if (has_character(byte_count)) {
                  byte_count++;
                }
                continue;
              }
              if (q == quote) break;
            }
          } else if (c == '`') {
            byte_count++;
            loop
            {
              if (!has_character(byte_count)) break;

              let const b = chop_character(byte_count);
              byte_count++;
              if (b == '\\') {
                if (has_character(byte_count)) {
                  byte_count++;
                }
                continue;
              }
              if (b == '`') break;
            }
          } else if (c == '$' && chop_character(byte_count + 1) == '(') {
            byte_count++;
            byte_count++;
            usize paren_depth = 1;
            char nested_quote = 0;
            loop
            {
              if (!has_character(byte_count)) break;

              let const p = chop_character(byte_count);
              byte_count++;
              if (nested_quote != 0) {
                if (nested_quote == '"' && p == '\\') {
                  if (has_character(byte_count)) {
                    byte_count++;
                  }
                  continue;
                }
                if (p == nested_quote) nested_quote = 0;
                continue;
              }
              if (p == '\\') {
                if (has_character(byte_count)) {
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
          } else if (is_bracket_arithmetic && c == '[') {
            group_depth++;
            byte_count++;
          } else if (is_bracket_arithmetic && c == ']') {
            byte_count++;
            if (group_depth == 0) break;
            group_depth--;
          } else if (is_bracket_arithmetic) {
            byte_count++;
          } else if (c == '(') {
            group_depth++;
            byte_count++;
          } else if (c == ')' && group_depth > 0) {
            group_depth--;
            byte_count++;
          } else if (c == ')' && chop_character(byte_count + 1) == ')') {
            byte_count += 2;
            break;
          } else {
            byte_count++;
          }
        }
        let const body_length = byte_count - arithmetic_start - closing_length;
        word.segments.push(WordSegment{
            WordSegment::Kind::ArithmeticExpansion,
            SegmentText{bump_allocator(arena()),
                        m_source.substring_of_length(
                            m_cursor_position + arithmetic_start, body_length)},
            is_in_double_quotes
        });
        word.segments.back().set_source_span(m_cursor_position +
                                                 arithmetic_start,
                                             word.segments.back().text.count());
        if (should_validate_substitutions()) {
          validate_nested_expansions(m_cursor_position + arithmetic_start,
                                     body_length, false);
        }
        continue;
      }

      if (next == '(') {
        byte_count++;

        let const inner_start = m_cursor_position + byte_count;
        let const substitution_end =
            lexer::scan_balanced_shell_region(m_source, inner_start, ')');
        if (!substitution_end.has_value()) rarely
          {
            throw ErrorWithLocationAndDetails{
                here(m_cursor_position, m_source.count() - m_cursor_position),
                "Unterminated command substitution", here(m_source.count(), 1),
                "expected ) here"};
          }
        let inner =
            SegmentText{bump_allocator(arena()),
                        m_source.substring_of_length(
                            inner_start, *substitution_end - inner_start - 1)};
        byte_count = *substitution_end - m_cursor_position;
        word.segments.push(WordSegment{WordSegment::Kind::CommandSubstitution,
                                       steal(inner), is_in_double_quotes});
        word.segments.back().set_source_span(
            m_cursor_position + expansion_start, byte_count - expansion_start);
        if (should_validate_substitutions()) {
          validate_substitution_body(
              inner_start,
              m_source.substring_of_length(inner_start,
                                           *substitution_end - inner_start - 1),
              here(m_cursor_position + expansion_start,
                   byte_count - expansion_start));
        }
      } else if (next == '{') {
        byte_count++;
        bool is_function_substitution = false;
        bool is_value_substitution = false;
        if (bash_additions_enabled()) {
          let probe = chop_character(byte_count);
          while (probe == ' ' || probe == '\t' || probe == '\n') {
            is_function_substitution = true;
            byte_count++;
            probe = chop_character(byte_count);
          }
          if (!is_function_substitution && probe == '|') {
            is_function_substitution = true;
            is_value_substitution = true;
          }
        }
        let const name_start = byte_count;
        if (is_value_substitution) byte_count++;
        usize brace_depth = 1;
        char quote = 0;
        loop
        {
          if (!has_character(byte_count)) rarely
            {
              throw ErrorWithLocationAndDetails{
                  here(m_cursor_position + byte_count, 1),
                  "Unterminated variable expansion",
                  here(m_cursor_position + byte_count, 1), "expected } here"};
            }

          let const c = chop_character(byte_count);
          byte_count++;

          if (quote == '\'') {
            if (c == '\'') quote = 0;
            continue;
          }
          if (c == '\\') {
            if (has_character(byte_count)) {
              byte_count++;
            }
            continue;
          }
          if (quote == '"') {
            if (c == '"') quote = 0;
            continue;
          }
          if (c == '\'' && is_in_double_quotes && is_posix_option_on()) {
            continue;
          }
          if (c == '\'' || c == '"') {
            quote = c;
            continue;
          }
          if (c == '`') {
            loop
            {
              if (!has_character(byte_count)) break;

              let const b = chop_character(byte_count);
              byte_count++;
              if (b == '\\') {
                if (has_character(byte_count)) {
                  byte_count++;
                }
                continue;
              }
              if (b == '`') break;
            }
            continue;
          }
          if (c == '$' && chop_character(byte_count) == '(') {
            byte_count++;
            let const nested_end = lexer::scan_balanced_shell_region(
                m_source, m_cursor_position + byte_count, ')');
            byte_count = nested_end.has_value()
                             ? *nested_end - m_cursor_position
                             : m_source.count() - m_cursor_position;
            continue;
          }
          if (c == '$' && chop_character(byte_count) == '{') {
            brace_depth++;
            byte_count++;
            continue;
          }
          if (c == '{' && is_function_substitution) {
            brace_depth++;
            continue;
          }
          if (c == '}') {
            brace_depth--;
            if (brace_depth == 0) break;
            continue;
          }
        }
        let const name = m_source.substring_of_length(
            m_cursor_position + name_start, byte_count - name_start - 1);
        word.segments.push(WordSegment{
            is_function_substitution ? WordSegment::Kind::FunctionSubstitution
                                     : WordSegment::Kind::VariableReference,
            SegmentText{bump_allocator(arena()), name},
            is_in_double_quotes
        });
        let &expansion_segment = word.segments.back();
        if (is_function_substitution)
          expansion_segment.set_source_span(m_cursor_position + expansion_start,
                                            byte_count - expansion_start);
        else
          expansion_segment.set_source_span(m_cursor_position +
                                                expansion_start + 2,
                                            expansion_segment.text.count());
        if (should_validate_substitutions()) {
          if (is_function_substitution) {
            let const body_offset = is_value_substitution ? usize{1} : 0;
            validate_substitution_body(m_cursor_position + name_start +
                                           body_offset,
                                       name.substring(body_offset),
                                       here(m_cursor_position + expansion_start,
                                            byte_count - expansion_start));
          } else if (name.find_character('$').has_value() ||
                     name.find_character('`').has_value())
          {
            validate_nested_expansions(m_cursor_position + name_start,
                                       name.length, false, is_in_double_quotes);
          }
        }
      } else if (lexer::is_variable_name_start(next)) {
        let const name_start = byte_count;
        while (lexer::is_variable_name(chop_character(byte_count)))
          byte_count++;
        let const name = m_source.substring_of_length(
            m_cursor_position + name_start, byte_count - name_start);
        word.segments.push(WordSegment{
            WordSegment::Kind::VariableReference,
            SegmentText{bump_allocator(arena()), name},
            is_in_double_quotes,
            true
        });
        word.segments.back().set_source_span(m_cursor_position +
                                                 expansion_start + 1,
                                             word.segments.back().text.count());
      } else if (lexer::is_special_parameter_char(next) ||
                 lexer::is_number(next))
      {
        byte_count++;
        word.segments.push(WordSegment{
            WordSegment::Kind::VariableReference,
            SegmentText{bump_allocator(arena()), StringView{&next, 1}},
            is_in_double_quotes
        });
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
      let const relative_open_backtick_pos = byte_count;
      byte_count++;
      let const body_start = m_cursor_position + byte_count;
      let inner = String{heap_allocator()};
      bool has_stripped_escape = false;
      loop
      {
        if (!has_character(byte_count)) rarely
          {
            throw ErrorWithLocationAndDetails{
                here(m_cursor_position + relative_open_backtick_pos, 1),
                "Unterminated command substitution",
                here(m_cursor_position + byte_count, 1), "expected ` here"};
          }

        let const c = chop_character(byte_count);
        if (c == '`') {
          byte_count++;
          break;
        }
        if (c == '\\') {
          let const escaped = chop_character(byte_count + 1);
          if (lexer::is_backtick_escape_stripped(escaped, is_in_double_quotes))
          {
            if (!has_stripped_escape) {
              inner.append(m_source.substring_of_length(
                  body_start, m_cursor_position + byte_count - body_start));
              has_stripped_escape = true;
            }
            inner += escaped;
            byte_count += 2;
            continue;
          }
        }
        if (has_stripped_escape) inner += c;
        byte_count++;
      }
      let const body = has_stripped_escape
                           ? inner.view()
                           : m_source.substring_of_length(
                                 body_start,
                                 m_cursor_position + byte_count - 1 -
                                     body_start);
      word.segments.push(WordSegment{
          WordSegment::Kind::CommandSubstitution,
          SegmentText{bump_allocator(arena()), body},
          is_in_double_quotes
      });
      word.segments.back().set_source_span(
          m_cursor_position + relative_open_backtick_pos,
          byte_count - relative_open_backtick_pos);
      if (should_validate_substitutions()) {
        let const raw_body_position =
            m_cursor_position + relative_open_backtick_pos + 1;
        let const raw_body = m_source.substring_of_length(
            raw_body_position, byte_count - relative_open_backtick_pos - 2);
        validate_substitution_body(
            raw_body_position,
            raw_body.length == body.length ? raw_body : body,
            here(m_cursor_position + relative_open_backtick_pos,
                 byte_count - relative_open_backtick_pos));
      }
      continue;
    }

    do_append_char(is_in_double_quotes ? WordSegment::Kind::DoubleQuotedText
                                       : WordSegment::Kind::UnquotedText,
                   ch);
    byte_count++;
  }

  if (quote_char.has_value()) rarely
    {
      let expected_quote = String{heap_allocator()};
      expected_quote += "expected ";
      expected_quote += *quote_char;
      expected_quote += " here";
      throw ErrorWithLocationAndDetails{
          here(m_cursor_position + relative_last_quote_position,
               sub_sat(byte_count, relative_last_quote_position)),
          "Unterminated string literal",
          here(m_cursor_position + byte_count, 1), expected_quote};
    }

  if (should_escape && !quote_char.has_value() &&
      m_cursor_position + byte_count >= m_source.length &&
      (is_bash_compatible() || is_posix_mode()))
  {
    do_append_char(WordSegment::Kind::LiteralText, '\\');
    should_escape = false;
  }

  if (should_escape) rarely
    {
      throw ErrorWithLocationAndDetails{
          here(m_cursor_position + byte_count - 1, 1), "Nothing to escape",
          here(m_cursor_position + byte_count, 1), "expected a character here"};
    }

  let const actual_cursor_position = m_cursor_position;
  ASSERT(actual_cursor_position <= m_source.length);
  let &arena = m_parse_session.get_arena();

  if (m_parse_session.is_allocating_function_body()) {
    for (let &segment : word.segments)
      segment.is_substitution_cache_in_function_arena = true;
  }

  if (m_parse_session.should_collect_debug_words() &&
      m_cursor_position != m_last_collected_word_position)
  {
    m_debug_words.push(word);
    m_last_collected_word_position = m_cursor_position;
  }

  Token *token{};

  if (let assignment_split = word.get_assignment_split(bump_allocator(arena));
      assignment_split.has_value())
  {
    let const arena_allocator = bump_allocator(arena);
    assignment_split->name.move_to_allocator(arena_allocator);
    assignment_split->value.move_resources_to_arena(arena);
    token = arena.create<tokens::Assignment>(
        here(actual_cursor_position, byte_count), steal(assignment_split->name),
        steal(assignment_split->value), assignment_split->update_mode);
  } else if (word.segments.count() == 1 &&
             word.segments[0].kind == WordSegment::Kind::UnquotedText)
  {
    let const &word_text = word.segments[0].text;
    let const keyword =
        KEYWORDS.find(StringView{word_text.data(), word_text.count()});
    if (keyword.has_value() &&
        !(*keyword == Token::Kind::Time && is_posix_mode()))
    {
      switch (*keyword) {
        KW_SWITCH_CASES();
      default: unreachable("unhandled keyword of type %d", ENUM(*keyword));
      }
    }
  }

  if (token == nullptr) {
    token = tokens::create_word_token(
        arena, here(actual_cursor_position, byte_count), steal(word));
  }

  m_cached_offset = byte_count;

  return token;
}

hot alwaysinline fn Lexer::lex_sentinel() throws -> Token *
{
  ASSERT(has_character());
  let const ch = chop_character();
  let &arena = m_parse_session.get_arena();

  usize extra_length = 0;

  Token *token{};

#define TOKEN_CASE_ONE(byte, t)                                                \
  case byte: token = arena.create<tokens::t>(here(m_cursor_position, 1)); break;

#define TOKEN_CASE_TWO(byte, t, ch, t2)                                        \
  case byte: {                                                                 \
    if (chop_character(1) == ch) {                                             \
      token = arena.create<tokens::t2>(here(m_cursor_position, 2));            \
      extra_length++;                                                          \
    } else {                                                                   \
      token = arena.create<tokens::t>(here(m_cursor_position, 1));             \
    }                                                                          \
  } break;

#define TOKEN_CASE_THREE(byte, t, ch2, t2, ch3, t3)                            \
  case byte: {                                                                 \
    if (chop_character(1) == ch2) {                                            \
      token = arena.create<tokens::t2>(here(m_cursor_position, 2));            \
      extra_length++;                                                          \
    } else if (chop_character(1) == ch3) {                                     \
      token = arena.create<tokens::t3>(here(m_cursor_position, 2));            \
      extra_length++;                                                          \
    } else {                                                                   \
      token = arena.create<tokens::t>(here(m_cursor_position, 1));             \
    }                                                                          \
  } break;

  switch (ch) {
    TOKEN_CASE_ONE(')', RightParen);
    TOKEN_CASE_ONE('(', LeftParen);
  case ';': {
    if (chop_character(1) == ';') {
      if (chop_character(2) == '&') {
        token = arena.create<tokens::DoubleSemicolonAmpersand>(
            here(m_cursor_position, 3));
        extra_length += 2;
      } else {
        token =
            arena.create<tokens::DoubleSemicolon>(here(m_cursor_position, 2));
        extra_length++;
      }
    } else if (chop_character(1) == '&') {
      token =
          arena.create<tokens::SemicolonAmpersand>(here(m_cursor_position, 2));
      extra_length++;
    } else {
      token = arena.create<tokens::Semicolon>(here(m_cursor_position, 1));
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
  case '&': {
    if (bash_additions_enabled() && chop_character(1) == '>') {
      if (chop_character(2) == '>') {
        token = arena.create<tokens::AmpersandDoubleGreater>(
            here(m_cursor_position, 3));
        extra_length += 2;
      } else {
        token =
            arena.create<tokens::AmpersandGreater>(here(m_cursor_position, 2));
        extra_length++;
      }
    } else if (chop_character(1) == '&') {
      token = arena.create<tokens::DoubleAmpersand>(here(m_cursor_position, 2));
      extra_length++;
    } else {
      token = arena.create<tokens::Ampersand>(here(m_cursor_position, 1));
    }
  } break;

  case '|': {
    if (chop_character(1) == '|') {
      token = arena.create<tokens::DoublePipe>(here(m_cursor_position, 2));
      extra_length++;
    } else if (bash_additions_enabled() && chop_character(1) == '&') {
      token = arena.create<tokens::PipeAmpersand>(here(m_cursor_position, 2));
      extra_length++;
    } else {
      token = arena.create<tokens::Pipe>(here(m_cursor_position, 1));
    }
  } break;
    TOKEN_CASE_TWO('=', Equals, '=', DoubleEquals);

    TOKEN_CASE_THREE('>', Greater, '>', DoubleGreater, '=', GreaterEquals);

  case '<': {
    if (chop_character(1) == '<') {
      if (chop_character(2) == '<' && bash_additions_enabled()) {
        token = arena.create<tokens::TripleLess>(here(m_cursor_position, 3));
        extra_length += 2;
      } else {
        token = arena.create<tokens::DoubleLess>(here(m_cursor_position, 2));
        extra_length++;
      }
    } else if (chop_character(1) == '=') {
      token = arena.create<tokens::LessEquals>(here(m_cursor_position, 2));
      extra_length++;
    } else {
      token = arena.create<tokens::Less>(here(m_cursor_position, 1));
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

hot alwaysinline fn Lexer::lex_process_substitution(Word &word,
                                                    usize offset) throws
    -> usize
{
  let const open_position = m_cursor_position + offset;
  let const direction = m_source[open_position];
  let const inner_start = open_position + 2;
  let const substitution_end =
      lexer::scan_balanced_shell_region(m_source, inner_start, ')');
  if (!substitution_end.has_value()) rarely
    {
      throw ErrorWithLocationAndDetails{
          here(open_position, m_source.count() - open_position),
          "Unterminated process substitution", here(m_source.count(), 1),
          "expected ) here"};
    }
  let const byte_count = *substitution_end - open_position;
  let const body = m_source.substring_of_length(
      inner_start, *substitution_end - inner_start - 1);

  LOG(Debug, "capturing a process substitution of %zu bytes", byte_count);

  word.segments.push(WordSegment{
      WordSegment::Kind::ProcessSubstitution,
      SegmentText{bump_allocator(arena()), direction, body},
      false
  });
  word.segments.back().set_source_span(open_position, byte_count);
  if (should_validate_substitutions()) {
    validate_substitution_body(inner_start, body,
                               here(open_position, byte_count));
  }

  return offset + byte_count;
}

cold fn Lexer::record_substitution_error(
    const ErrorWithLocationAndDetails &error) throws -> void
{
  let const key = lexer::substitution_error_key{
      (static_cast<u64>(error.location().position) << 32) |
          static_cast<u64>(error.location().length),
      hash_bytes(error.message().view())};
  if (m_reported_substitution_error_keys.find(key).has_value()) return;

  m_reported_substitution_error_keys.push(key);
  m_substitution_errors.push(error);
}

cold fn Lexer::validate_substitution_body(
    usize body_position, StringView body,
    const SourceLocation &outer_location) throws -> void
{
  if (m_substitution_nesting_depth >= lexer::MAX_SUBSTITUTION_NESTING_DEPTH) {
    record_substitution_error(ErrorWithLocationAndDetails{
        outer_location, "Command substitution nested too deeply",
        StringView{}});
    return;
  }

  let &arena = m_parse_session.get_arena();
  let const arena_mark = arena.mark();
  defer { arena.release(arena_mark); };

  let const is_exact = body.data == m_source.data + body_position;
  let nested_lexer = Lexer{
      is_exact ? m_source.substring_of_length(0, body_position + body.length)
               : body,
      arena, source_name_at(m_parse_session.source_name_index()), mood(),
      m_parse_session.get_allocation_kind()};
  if (is_exact) nested_lexer.set_start_position(body_position);
  nested_lexer.set_substitution_validation_mode(
      substitution_validation_mode::Enabled, m_substitution_nesting_depth + 1);

  let nested_parser = Parser{steal(nested_lexer)};
  let nested_errors = ArrayList<ErrorWithLocationAndDetails>{heap_allocator()};
  let nested_messages = ArrayList<String>{heap_allocator()};
  nested_parser.set_error_collection(&nested_errors);
  unused(nested_parser.construct_ast(nested_messages, nullptr, nullptr));

  for (let const &error : nested_errors) {
    if (is_exact) {
      record_substitution_error(error);
      continue;
    }

    record_substitution_error(ErrorWithLocationAndDetails{
        outer_location, error.message().view(), error.detail_message()});
  }
}

cold fn Lexer::validate_nested_expansions(
    usize region_position, usize region_length, bool is_heredoc,
    bool is_region_in_double_quotes) throws -> void
{
  let const found =
      lexer::find_nested_substitutions(m_source, region_position, region_length,
                                       is_heredoc, is_region_in_double_quotes);
  for (let const &entry : found) {
    let const outer_location = here(entry.outer_position, entry.outer_length);
    if (entry.is_exact) {
      validate_substitution_body(
          entry.body_position,
          m_source.substring_of_length(entry.body_position, entry.body_length),
          outer_location);
    } else {
      validate_substitution_body(entry.body_position,
                                 entry.unescaped_body.view(), outer_location);
    }
  }
}

cold fn lexer::find_nested_substitutions(StringView source,
                                         usize region_position,
                                         usize region_length, bool is_heredoc,
                                         bool is_region_in_double_quotes) throws
    -> ArrayList<nested_substitution>
{
  let found = ArrayList<nested_substitution>{heap_allocator()};
  let const region = source.substring_of_length(region_position, region_length);
  if (!region.find_character('`').has_value() &&
      !region.find_character('$').has_value())
  {
    return found;
  }

  let is_in_single_quotes = false;
  let is_in_double_quotes = is_region_in_double_quotes;
  usize offset = 0;
  while (offset < region.length) {
    let const c = region[offset];
    if (c == '\\') {
      offset += 2;
      continue;
    }

    if (!is_heredoc) {
      if (is_in_single_quotes) {
        is_in_single_quotes = c != '\'';
        offset++;
        continue;
      }

      if (c == '\'' && !is_in_double_quotes) {
        is_in_single_quotes = true;
        offset++;
        continue;
      }

      if (c == '"') {
        is_in_double_quotes = !is_in_double_quotes;
        offset++;
        continue;
      }
    }

    if (c == '`') {
      let const body_start = offset + 1;
      let end = body_start;
      let const is_escape_in_double_quotes = !is_heredoc && is_in_double_quotes;
      let has_stripped_escape = false;
      while (end < region.length && region[end] != '`') {
        if (region[end] == '\\') {
          if (end + 1 < region.length &&
              lexer::is_backtick_escape_stripped(region[end + 1],
                                                 is_escape_in_double_quotes))
          {
            has_stripped_escape = true;
          }
          end++;
        }
        end++;
      }
      if (end >= region.length) return found;

      let const body = region.substring_of_length(body_start, end - body_start);
      let entry = nested_substitution{};
      entry.body_position = region_position + body_start;
      entry.body_length = body.length;
      entry.outer_position = region_position + offset;
      entry.outer_length = end + 1 - offset;
      entry.is_exact = !has_stripped_escape;
      if (has_stripped_escape) {
        for (usize index = 0; index < body.length; index++) {
          if (body[index] == '\\' && index + 1 < body.length &&
              lexer::is_backtick_escape_stripped(body[index + 1],
                                                 is_escape_in_double_quotes))
          {
            index++;
          }
          entry.unescaped_body += body[index];
        }
      }
      found.push(steal(entry));
      offset = end + 1;
      continue;
    }

    if (c != '$' || offset + 1 >= region.length || region[offset + 1] != '(') {
      offset++;
      continue;
    }

    if (offset + 2 < region.length && region[offset + 2] == '(') {
      offset += 3;
      continue;
    }

    let const body_start = region_position + offset + 2;
    let const body_end =
        lexer::scan_balanced_shell_region(source, body_start, ')');
    if (!body_end.has_value()) return found;

    let entry = nested_substitution{};
    entry.body_position = body_start;
    entry.body_length = *body_end - body_start - 1;
    entry.outer_position = region_position + offset;
    entry.outer_length = *body_end - region_position - offset;
    found.push(steal(entry));
    offset = *body_end - region_position;
  }

  return found;
}

cold fn lexer::find_segment_substitution(StringView source,
                                         const WordSegment &segment) throws
    -> Maybe<nested_substitution>
{
  let const position = static_cast<usize>(segment.source_position);
  let const length = static_cast<usize>(segment.source_length);
  if (length == 0 || position > source.length ||
      length > source.length - position)
  {
    return None;
  }

  let const text = segment.text.view();
  let entry = nested_substitution{};
  entry.outer_position = position;
  entry.outer_length = length;
  let body_text = text;
  switch (segment.kind) {
  case WordSegment::Kind::CommandSubstitution:
    if (source[position] == '`') {
      if (length < 2) return None;

      entry.body_position = position + 1;
      entry.body_length = length - 2;
    } else {
      if (length < 3) return None;

      entry.body_position = position + 2;
      entry.body_length = length - 3;
    }
    break;

  case WordSegment::Kind::ProcessSubstitution:
    if (length < 3 || text.is_empty()) return None;

    entry.body_position = position + 2;
    entry.body_length = length - 3;
    body_text = text.substring(1);
    break;

  case WordSegment::Kind::FunctionSubstitution:
    if (text.length + 1 > length) return None;

    entry.body_position = position + length - 1 - text.length;
    entry.body_length = text.length;
    if (!text.is_empty() && text[0] == '|') {
      entry.body_position++;
      entry.body_length--;
      body_text = text.substring(1);
    }
    break;

  default: return None;
  }

  entry.is_exact = entry.body_length == body_text.length &&
                   source.substring_of_length(entry.body_position,
                                              entry.body_length) == body_text;
  if (!entry.is_exact) entry.unescaped_body = String{body_text};

  return entry;
}

} /* namespace koshka */
