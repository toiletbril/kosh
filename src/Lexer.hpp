/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file declares the per-parser arena session, stateful shell lexer,
 * heredoc storage, and shared lexical predicates. Parsing, formatting,
 * completion, and diagnostics share the interface. Token construction remains
 * in the lexer sources.
 */

#pragma once

#include "Diagnostics.hpp"
#include "MimicMood.hpp"
#include "Tokens.hpp"
#include "base/Common.hpp"
#include "base/Containers.hpp"
#include "base/String.hpp"
#include "base/StringView.hpp"

namespace koshka {

class BumpArena;

enum class heredoc_tab_policy : u8
{
  Preserve,
  Strip,
};

enum class heredoc_source_mapping : u8
{
  Contiguous,
  Transformed,
};

class ParseSession
{
public:
  enum class AllocationKind : u8
  {
    Syntax,
    FunctionBody,
  };

  explicit ParseSession(BumpArena &syntax_arena)
      : m_syntax_arena(&syntax_arena), m_active_arena(&syntax_arena)
  {}

  pure fn get_arena() const wontthrow -> BumpArena & { return *m_active_arena; }

  pure fn get_syntax_arena() const wontthrow -> BumpArena &
  {
    return *m_syntax_arena;
  }

  pure fn is_allocating_function_body() const wontthrow -> bool
  {
    return m_allocation_kind == AllocationKind::FunctionBody;
  }

  pure fn get_allocation_kind() const wontthrow -> AllocationKind
  {
    return m_allocation_kind;
  }

  pure fn source_name_index() const wontthrow -> u32
  {
    return m_source_name_index;
  }

  fn set_source_name_index(u32 index) wontthrow -> void
  {
    m_source_name_index = index;
  }

  pure fn mood() const wontthrow -> mimic_mood { return m_mood; }

  fn set_mood(mimic_mood mood) wontthrow -> void { m_mood = mood; }

  pure fn should_collect_debug_words() const wontthrow -> bool
  {
    return m_should_collect_debug_words;
  }

  fn set_should_collect_debug_words(bool should_collect) wontthrow -> void
  {
    m_should_collect_debug_words = should_collect;
  }

  pure fn should_collect_analysis_metadata() const wontthrow -> bool
  {
    return m_should_collect_analysis_metadata;
  }

  fn set_should_collect_analysis_metadata(bool should_collect) wontthrow -> void
  {
    m_should_collect_analysis_metadata = should_collect;
  }

  pure fn should_collect_shellcheck_directives() const wontthrow -> bool
  {
    return m_should_collect_shellcheck_directives;
  }

  fn set_should_collect_shellcheck_directives(bool should_collect) wontthrow
      -> void
  {
    m_should_collect_shellcheck_directives = should_collect;
  }

  fn set_arena(BumpArena &arena, AllocationKind allocation_kind) wontthrow
      -> void
  {
    m_active_arena = &arena;
    m_allocation_kind = allocation_kind;
  }

private:
  BumpArena *m_syntax_arena;
  BumpArena *m_active_arena;
  AllocationKind m_allocation_kind{AllocationKind::Syntax};
  u32 m_source_name_index{0};
  mimic_mood m_mood{mimic_mood::Default};
  bool m_should_collect_debug_words{false};
  bool m_should_collect_analysis_metadata{false};
  bool m_should_collect_shellcheck_directives{false};
};

struct heredoc_contents
{
  heredoc_contents(Allocator allocator, heredoc_source_mapping source_mapping)
      : text{allocator}, source_mapping{source_mapping}
  {}

  String text;
  usize source_position{0};
  usize source_end_position{0};
  heredoc_source_mapping source_mapping;
};

struct heredoc_pending
{
  String delimiter;
  heredoc_tab_policy tab_policy;
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
pure fn word_is_variable_name(StringView word) wontthrow -> bool;
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
        mimic_mood mood = mimic_mood::Default,
        ParseSession::AllocationKind allocation_kind =
            ParseSession::AllocationKind::Syntax);
  ~Lexer();

  pure fn mood() const wontthrow -> mimic_mood
  {
    return m_parse_session.mood();
  }

  pure fn is_bash_compatible() const wontthrow -> bool
  {
    return mood() == mimic_mood::Bash || mood() == mimic_mood::BashPosix;
  }

  /* Whether strict POSIX lexing is active. The default mood is neither bash nor
     POSIX, so a dash-rejected pure addition such as the NAME=(...) array
     literal stays on in the default mood and is suppressed only here. */
  pure fn is_posix_mode() const wontthrow -> bool
  {
    return mood() == mimic_mood::Posix;
  }

  /* The token-level bash additions, $'...' and <<< and |& and &>, ride every
     mood but POSIX under the pure-addition rule. EvalContext holds the same
     predicate for the additions the evaluator gates. */
  pure fn bash_additions_enabled() const wontthrow -> bool
  {
    return mood() != mimic_mood::Posix;
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
  pure fn arena_kind() const wontthrow -> ParseSession::AllocationKind
  {
    return m_parse_session.get_allocation_kind();
  }
  fn set_arena(BumpArena &arena,
               ParseSession::AllocationKind allocation_kind) wontthrow -> void;
  fn drop_peek_cache() wontthrow -> void;
  fn advance_past_last_peek() throws -> usize;

  fn set_should_collect_shellcheck_directives(bool should_collect) wontthrow
      -> void
  {
    m_parse_session.set_should_collect_shellcheck_directives(should_collect);
  }
  fn set_should_collect_analysis_metadata(bool should_collect) wontthrow -> void
  {
    m_parse_session.set_should_collect_analysis_metadata(should_collect);
  }
  fn take_shellcheck_directives() throws
      -> ArrayList<shellcheck_directive_span>;
  fn take_shellcheck_directive_spans() throws
      -> ArrayList<shellcheck_directive_span>;
  fn take_heredoc_terminator_misses() throws
      -> ArrayList<heredoc_terminator_miss>;

  fn register_heredoc(StringView delimiter, heredoc_tab_policy tab_policy)
      throws
      -> const heredoc_contents *;

protected:
  pure alwaysinline fn here(usize position, usize length) const wontthrow
      -> SourceLocation
  {
    return SourceLocation{position, length,
                          m_parse_session.source_name_index()};
  }

  fn peek_cache_is_live() const wontthrow -> bool;

  StringView m_source;
  ParseSession m_parse_session;
  /* The interned name of the file this source came from, or zero for an unnamed
     source such as an interactive line. It travels into every SourceLocation
     the lexer stamps. */
  usize m_cursor_position{0};
  usize m_cached_offset{0};

  /* The parser peeks the next token many times before it consumes one, and each
     peek would otherwise re-lex from the same position. */
  Token *m_peek_cache{nullptr};
#if !defined NDEBUG
  usize m_peek_cache_generation{0};
#endif

  ArrayList<Word> m_debug_words{heap_allocator()};
  usize m_last_collected_word_position{static_cast<usize>(-1)};

  bool m_last_shell_token_was_newline{false};
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
                       heredoc_tab_policy tab_policy, Emit emit_line) throws
      -> usize;

  fn lex_shell_token() throws -> Token *;

  fn skip_whitespace() throws -> void;
  fn advance_forward(usize offset) wontthrow -> usize;
  fn chop_character(usize offset = 0) wontthrow -> char;

  fn lex_identifier() throws -> Token *;
  fn lex_sentinel() throws -> Token *;
  fn lex_process_substitution(char direction) throws -> Token *;
};

} /* namespace koshka */
