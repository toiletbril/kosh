#pragma once

#include "Allocator.hpp"
#include "Arena.hpp"
#include "Completion.hpp"
#include "Containers.hpp"
#include "String.hpp"
#include "StringView.hpp"

namespace koshka {

namespace completion {

extern BumpArena COMPLETION_ARENA;

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

/* Primitives defined in Completion.cpp and reached from the cascade stages and
   the highlighter. */
fn command_word_of(StringView line) wontthrow -> StringView;
pure fn token_has_glob_metacharacter(StringView token) wontthrow -> bool;
fn resolve_completion_alias(StringView command, EvalContext &context) throws
    -> String;
fn resolve_completion_command(StringView command, EvalContext &context) throws
    -> String;
fn split_completion_words(StringView line, usize cursor, usize &cword) throws
    -> ArrayList<String>;
pure fn word_looks_like_assignment(StringView word) wontthrow -> bool;
pure fn word_is_plain_identifier(StringView word) wontthrow -> bool;
pure fn word_defines_function(StringView line, usize word_end,
                              usize end) wontthrow -> bool;
fn advance_shell_keyword_state(StringView word, usize frame_depth,
                               shell_lexical_state &state) throws
    -> Maybe<bool>;

/* Defined in CompletionManpage.cpp. */
fn second_word_of(StringView line) wontthrow -> Maybe<StringView>;
fn complete_from_man_subcommands(StringView line, StringView token,
                                 usize token_start, completion_mode mode,
                                 EvalContext &context) throws
    -> Maybe<ArrayList<String>>;
fn complete_from_manpage(StringView line, StringView token,
                         completion_mode mode, EvalContext &context,
                         StringMap<String> &descriptions) throws
    -> Maybe<ArrayList<String>>;
fn complete_from_help(StringView line, StringView token, usize token_start,
                      completion_mode mode, EvalContext &context,
                      StringMap<String> &descriptions) throws
    -> Maybe<ArrayList<String>>;
fn complete_from_help_subcommands(StringView line, StringView token,
                                  usize token_start, completion_mode mode,
                                  EvalContext &context,
                                  StringMap<String> &descriptions) throws
    -> Maybe<ArrayList<String>>;

/* Defined in CompletionScan.cpp. */
fn complete_from_process_arguments(StringView line, StringView token,
                                   completion_mode mode) throws
    -> Maybe<ArrayList<String>>;
fn complete_from_tools_with_targets(StringView line, StringView token,
                                    usize token_start, completion_mode mode,
                                    EvalContext &context) throws
    -> Maybe<ArrayList<String>>;
fn complete_from_builtin_flags(StringView line, StringView token,
                               usize token_start, EvalContext &context) throws
    -> Maybe<ArrayList<String>>;
fn complete_from_spec(StringView line, StringView token, usize cursor,
                      completion_mode mode, EvalContext &context,
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
};

fn advance_shell_lexical_state(
    StringView source, usize end, shell_lexical_state &state,
    shell_lexical_scan_target *target = nullptr) throws -> void;

fn command_substitution_range(StringView line, usize cursor) throws
    -> completion_command_range;

} /* namespace completion */

} /* namespace koshka */
