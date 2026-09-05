#pragma once

#include "Allocator.hpp"
#include "Arena.hpp"
#include "Completion.hpp"
#include "Containers.hpp"
#include "HashSet.hpp"
#include "String.hpp"
#include "StringView.hpp"

namespace koshka {

namespace utils {
struct decoded_shell_word;
}

namespace completion::internal {

extern BumpArena COMPLETION_ARENA;
extern BumpArena HIGHLIGHT_ARENA;
extern usize DEBUG_HIGHLIGHT_INPUT_BYTE_COUNT;

inline fn completion_allocator() wontthrow -> Allocator
{
  return bump_allocator(COMPLETION_ARENA);
}

static pure alwaysinline fn is_blank(char byte) wontthrow -> bool
{
  return byte == ' ' || byte == '\t';
}

static pure alwaysinline fn skip_blanks(StringView text, usize from) wontthrow
    -> usize
{
  while (from < text.length && is_blank(text[from]))
    from++;
  return from;
}

struct token_bounds
{
  usize start;
  usize end;
};

/* The directory part keeps its trailing separator so the basename joins back
   on. */
struct path_token
{
  StringView directory_part;
  StringView basename_part;
};

/* Primitives defined in Completion.cpp and reached from the cascade stages and
   the highlighter. */
pure fn quoted_run_end(StringView line, usize position) wontthrow -> usize;
pure fn find_token_bounds(StringView line, usize cursor) wontthrow
    -> token_bounds;
pure fn is_active_token_boundary(StringView line, usize position) wontthrow
    -> bool;
pure fn is_in_command_position(StringView line, usize token_start) wontthrow
    -> bool;
pure fn command_segment_start(StringView line, usize cursor) wontthrow -> usize;
pure fn split_path_token(StringView token) wontthrow -> path_token;
pure fn path_candidate_needs_quoting(StringView candidate) wontthrow -> bool;
fn quote_path_candidate(StringView candidate) throws -> String;
fn escape_path_candidate(StringView candidate) throws -> String;
fn rebuild_shell_syntax_candidate(StringView raw_token,
                                  const utils::decoded_shell_word &decoded_word,
                                  StringView decoded_candidate) throws
    -> String;
fn resolve_listing_directory(StringView directory_part,
                             const Path &base_directory, EvalContext &context,
                             bool is_leading_tilde_active,
                             bool is_leading_variable_active,
                             usize leading_variable_expansion_end) throws
    -> Path;
fn command_word_of(StringView line) wontthrow -> StringView;
pure fn token_has_glob_metacharacter(StringView token) wontthrow -> bool;
fn resolve_completion_alias(StringView command, EvalContext &context) throws
    -> String;
fn resolve_completion_command(StringView command, EvalContext &context) throws
    -> String;
fn split_completion_words(StringView line, usize cursor, usize &cword) throws
    -> ArrayList<String>;
pure fn word_is_function_name(StringView word) wontthrow -> bool;
pure fn word_defines_function(StringView line, usize word_end,
                              usize end) wontthrow -> bool;
fn advance_shell_keyword_state(StringView word, usize frame_depth,
                               completion::shell_lexical_state &state) throws
    -> Maybe<bool>;
fn scan_highlight_range(StringView line, usize begin, usize end,
                        EvalContext &context, ArrayList<highlight_span> &spans,
                        HashSet &line_variable_names,
                        const HashSet *known_function_names,
                        bool should_stop_at_closing_parenthesis = false) throws
    -> usize;

/* Defined in CompletionManpage.cpp. */
fn second_word_of(StringView line) wontthrow -> Maybe<StringView>;
fn manpage_text_for(StringView page_name, EvalContext &context) throws
    -> StringView;
fn help_text_of(StringView command, EvalContext &context) throws -> StringView;
fn complete_from_man_subcommands(StringView line, StringView token,
                                 usize token_start,
                                 completion::completion_mode mode,
                                 EvalContext &context) throws
    -> Maybe<ArrayList<String>>;
fn complete_from_manpage(StringView line, StringView token,
                         completion::completion_mode mode, EvalContext &context,
                         StringMap<String> &descriptions) throws
    -> Maybe<ArrayList<String>>;
fn complete_from_help(StringView line, StringView token, usize token_start,
                      completion::completion_mode mode, EvalContext &context,
                      StringMap<String> &descriptions) throws
    -> Maybe<ArrayList<String>>;
fn complete_from_help_subcommands(StringView line, StringView token,
                                  usize token_start,
                                  completion::completion_mode mode,
                                  EvalContext &context,
                                  StringMap<String> &descriptions) throws
    -> Maybe<ArrayList<String>>;

/* Defined in CompletionScan.cpp. */
fn complete_from_process_arguments(StringView line, StringView token,
                                   usize token_start,
                                   completion::completion_mode mode) throws
    -> Maybe<ArrayList<String>>;
fn complete_from_tools_with_targets(StringView line, StringView token,
                                    usize token_start,
                                    completion::completion_mode mode,
                                    EvalContext &context) throws
    -> Maybe<ArrayList<String>>;
fn complete_from_builtin_flags(StringView line, StringView token,
                               usize token_start, EvalContext &context) throws
    -> Maybe<ArrayList<String>>;
fn complete_from_spec(StringView line, StringView token, usize cursor,
                      completion::completion_mode mode, EvalContext &context,
                      StringMap<String> &descriptions) throws
    -> Maybe<ArrayList<String>>;
struct completion_command_range
{
  usize start;
  usize end;
};

struct shell_lexical_scan_target
{
  usize cursor;
  completion_command_range range;
  usize frame_depth{0};
  bool should_stop_at_token_boundary{false};
};

fn advance_shell_lexical_state(
    StringView source, usize end, completion::shell_lexical_state &state,
    shell_lexical_scan_target *target = nullptr) throws -> void;

fn command_substitution_range(StringView line, usize cursor) throws
    -> completion_command_range;

} /* namespace completion::internal */

} /* namespace koshka */
