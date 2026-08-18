#pragma once

#include "Common.hpp"
#include "Containers.hpp"
#include "Diagnostics.hpp"
#include "MimicMood.hpp"
#include "String.hpp"
#include "StringView.hpp"
#include "Tokens.hpp"

namespace koshka {

class BumpArena;

struct heredoc_contents
{
  heredoc_contents(Allocator allocator, bool has_contiguous_source)
      : text{allocator}, has_contiguous_source{has_contiguous_source}
  {}

  String text;
  usize source_position{0};
  bool has_contiguous_source;
};

struct heredoc_pending
{
  String delimiter;
  bool should_strip_tabs;
  heredoc_contents *contents;
};

namespace lexer {

pure fn is_whitespace(char ch) wontthrow -> bool;
pure fn is_number(char ch) wontthrow -> bool;
pure fn is_shell_sentinel(char ch) wontthrow -> bool;
pure fn is_part_of_identifier(char ch) wontthrow -> bool;
pure fn is_string_quote(char ch) wontthrow -> bool;
pure fn is_expandable_char(char ch) wontthrow -> bool;
pure fn is_variable_name_start(char ch) wontthrow -> bool;
pure fn is_variable_name(char ch) wontthrow -> bool;
pure fn word_looks_like_assignment(StringView word) wontthrow -> bool;
pure fn is_extglob_operator(char ch) wontthrow -> bool;

fn scan_balanced_shell_region(StringView source, usize position,
                              char closing_byte) throws -> Maybe<usize>;

/* The quotes and the escapes of a heredoc delimiter word, so <<\EOF and <<'EOF'
   both terminate on EOF. */
fn unquote_heredoc_delimiter(StringView word, Allocator allocator) throws
    -> String;

/* Owned shell source is normalized before lexing, so a heredoc body line is
   matched against the delimiter without its CRLF carriage return. */
pure fn heredoc_line_content(StringView line) wontthrow -> StringView;

/* A special shell parameter named by a single punctuation byte, $? $! $# $$ $*
   $@ $- , distinct from a positional digit or an ordinary name. */
pure fn is_special_parameter_char(char ch) wontthrow -> bool;

} /* namespace lexer */

/* Only advance_past_last_peek, skip_whitespace, and advance_forward move the
 * internal cursor. */
class Lexer
{
public:
  Lexer(StringView source, BumpArena &arena,
        bool should_collect_debug_words = false,
        Maybe<StringView> filename = None,
        mimic_mood mood = mimic_mood::Default);
  ~Lexer();

  pure fn mood() const wontthrow -> mimic_mood { return m_mood; }

  pure fn is_bash_compatible() const wontthrow -> bool
  {
    return m_mood == mimic_mood::Bash || m_mood == mimic_mood::BashPosix;
  }

  /* Whether strict POSIX lexing is active. The default mood is neither bash nor
     POSIX, so a dash-rejected pure addition such as the NAME=(...) array
     literal stays on in the default mood and is suppressed only here. */
  pure fn is_posix_mode() const wontthrow -> bool
  {
    return m_mood == mimic_mood::Posix;
  }

  /* The token-level bash additions, $'...' and <<< and |& and &>, ride every
     mood but POSIX under the pure-addition rule. EvalContext holds the same
     predicate for the additions the evaluator gates. */
  pure fn bash_additions_enabled() const wontthrow -> bool
  {
    return m_mood != mimic_mood::Posix;
  }

  Lexer(Lexer &&) = default;
  Lexer &operator=(Lexer &&) = default;
  Lexer(const Lexer &) = delete;
  Lexer &operator=(const Lexer &) = delete;

  mustuse fn peek_shell_token() throws -> Token *;
  mustuse fn next_shell_token() throws -> Token *;

  pure fn source() const wontthrow -> StringView;
  pure fn cursor_position() const wontthrow -> usize;
  pure fn is_at_source_end() const wontthrow -> bool;
  pure fn debug_words() const wontthrow -> const ArrayList<Word> &;
  pure fn arena() const wontthrow -> BumpArena &;
  fn set_arena(BumpArena &arena) wontthrow -> void;
  fn advance_past_last_peek() throws -> usize;

  fn set_should_collect_shellcheck_directives(bool should_collect) wontthrow
      -> void;
  fn take_shellcheck_directives() throws
      -> ArrayList<shellcheck_directive_span>;
  fn take_shellcheck_directive_spans() throws
      -> ArrayList<shellcheck_directive_span>;
  fn take_heredoc_terminator_misses() throws
      -> ArrayList<heredoc_terminator_miss>;

  fn register_heredoc(StringView delimiter, bool should_strip_tabs) throws
      -> const heredoc_contents *;

protected:
  pure alwaysinline fn here(usize position, usize length) const wontthrow
      -> SourceLocation
  {
    return SourceLocation{position, length, m_filename};
  }

  StringView m_source;
  BumpArena *m_arena;
  /* The name of the file this source came from, or None for an unnamed source
     such as an interactive line. It travels into every SourceLocation the lexer
     stamps. */
  Maybe<StringView> m_filename{};
  mimic_mood m_mood{mimic_mood::Default};
  usize m_cursor_position{0};
  usize m_cached_offset{0};

  /* The parser peeks the next token many times before it consumes one, and each
     peek would otherwise re-lex from the same position. The last token is
     reused while the cursor has not moved. */
  Token *m_peek_cache{nullptr};
  usize m_peek_cache_position{0};

  bool m_should_collect_debug_words{false};
  ArrayList<Word> m_debug_words{heap_allocator()};
  usize m_last_collected_word_position{static_cast<usize>(-1)};

  bool m_last_shell_token_was_newline{false};
  bool m_should_collect_shellcheck_directives{false};
  ArrayList<shellcheck_directive_span> m_pending_shellcheck_directives{
      heap_allocator()};
  ArrayList<shellcheck_directive_span> m_shellcheck_directive_spans{
      heap_allocator()};
  ArrayList<heredoc_terminator_miss> m_heredoc_terminator_misses{
      heap_allocator()};
  /* Each body is allocated in the arena, so its address is stable and it
     outlives the lexer. A parsed redirection holds a pointer into one, and the
     arena reclaims the body when it reclaims the nodes that point at it. */
  ArrayList<heredoc_pending> m_pending_heredocs{heap_allocator()};
  fn collect_pending_heredocs() throws -> void;

  template <class Emit>
  fn walk_heredoc_body(usize start, StringView delimiter,
                       bool should_strip_tabs, Emit emit_line) throws -> usize;

  fn lex_shell_token() throws -> Token *;

  fn skip_whitespace() throws -> void;
  fn advance_forward(usize offset) wontthrow -> usize;
  fn chop_character(usize offset = 0) wontthrow -> char;

  fn lex_identifier() throws -> Token *;
  fn lex_sentinel() throws -> Token *;
  fn lex_process_substitution(char direction) throws -> Token *;
};

} /* namespace koshka */
