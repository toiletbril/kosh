/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file declares completion results, matching and tab-selector modes,
 * selector-name conversion, lexical construct state, highlight spans, and the
 * incremental highlight cache. The editor and language server share this
 * interface. Individual providers remain private.
 */

#pragma once

#include "Highlight.hpp"
#include "StaticStringMap.hpp"
#include "base/Arena.hpp"
#include "base/Common.hpp"
#include "base/HashSet.hpp"
#include "base/Maybe.hpp"
#include "base/Path.hpp"
#include "base/String.hpp"
#include "base/StringMap.hpp"
#include "base/StringView.hpp"

namespace koshka {

class EvalContext;

/* The presentation the editor uses when a completion has several candidates.
   Interactive draws the shell's own bounded menu under the prompt. External
   launches the configured selector program. Plain prints the candidate list the
   way a terminal shell without an editor does. */
enum class tab_selector_mode : u8
{
  Interactive,
  External,
  Plain,
};

inline pure fn parse_tab_selector_name(StringView name) throws
    -> Maybe<tab_selector_mode>
{
  static constexpr static_string_entry<tab_selector_mode>
      TAB_SELECTOR_ENTRIES[] = {
          {SSK("interactive"), tab_selector_mode::Interactive},
          {SSK("external"),    tab_selector_mode::External   },
          {SSK("plain"),       tab_selector_mode::Plain      },
  };
  static constexpr StaticStringMap TAB_SELECTORS{TAB_SELECTOR_ENTRIES};
  return TAB_SELECTORS.find(name);
}

inline pure fn tab_selector_name(tab_selector_mode selector) wontthrow
    -> StringView
{
  switch (selector) {
  case tab_selector_mode::Interactive: return "interactive";
  case tab_selector_mode::External: return "external";
  case tab_selector_mode::Plain: return "plain";
  }
  return "interactive";
}

namespace completion {

enum class completion_mode : u8
{
  Ghost,
  Listing,
};

enum class command_match_mode : u8
{
  Prefix,
  Glob,
};

enum class completion_filesystem_mode : u8
{
  Files,
  Directories,
};

struct completion_result
{
  ArrayList<String> candidates;
  /* Keyed by the candidate text so it survives the candidate sort. Empty for a
     filesystem or command-name completion. */
  StringMap<String> descriptions{heap_allocator()};
  String longest_common_prefix;
  usize candidate_count;
  usize source_candidate_scan_count;
  usize materialized_candidate_count;
  usize token_start;
  usize token_end;
  /* Argument position completes against the filesystem instead. */
  bool is_command_position;
};

fn complete(StringView line, usize cursor, EvalContext &context,
            const Path &base_directory,
            const ArrayList<StringView> *extra_command_names = nullptr,
            bool should_complete_external_arguments_in_posix = false,
            completion_mode mode = completion_mode::Ghost) throws
    -> completion_result;

fn complete_command_names(
    StringView token, command_match_mode match_mode, EvalContext &context,
    const ArrayList<StringView> *extra_command_names = nullptr) throws
    -> ArrayList<String>;
fn complete_filesystem_names(StringView token, EvalContext &context,
                             const Path &base_directory) throws
    -> ArrayList<String>;

fn complete_command_names_by_prefix(StringView token,
                                    EvalContext &context) throws
    -> ArrayList<String>;
fn complete_filesystem_names_by_prefix(
    StringView token, EvalContext &context, const Path &base_directory,
    completion_filesystem_mode mode = completion_filesystem_mode::Files) throws
    -> ArrayList<String>;

class ScopedCompletionScratch
{
public:
  ScopedCompletionScratch() throws;
  ~ScopedCompletionScratch();

  ScopedCompletionScratch(const ScopedCompletionScratch &) = delete;
  ScopedCompletionScratch &operator=(const ScopedCompletionScratch &) = delete;

private:
  BumpArena::Mark m_saved;
};

/* The spans come back sorted by start and non-overlapping. */
enum class shell_lexical_frame_kind : u8
{
  command,
  backtick,
  arithmetic,
  parameter,
};

enum class highlight_construct : u8
{
  if_,
  while_until,
  for_,
  case_,
  function,
  conditional,
};

enum class highlight_construct_phase : u8
{
  condition,
  body,
  for_variable,
  for_in,
  for_do,
  function_name,
};

struct shell_lexical_construct
{
  usize frame_depth;
  highlight_construct kind;
  highlight_construct_phase phase;
};

struct shell_lexical_frame
{
  usize body_start;
  usize group_depth;
  usize case_depth{0};
  usize array_value_group_depth{0};
  shell_lexical_frame_kind kind;
  char parent_quote;
  bool has_seen_case_keyword{false};
  bool is_case_pattern_expected{false};
  bool is_command_position{true};
  bool is_in_array_value{false};
};

static_assert(sizeof(usize) != 8 || sizeof(shell_lexical_frame) == 40);

struct shell_pending_heredoc
{
  String delimiter;
  bool should_strip_tabs;
};

struct shell_lexical_state
{
  explicit shell_lexical_state(Allocator allocator)
      : frames{allocator}, pending_heredocs{allocator}, constructs{allocator},
        known_function_names{allocator}
  {}

  ArrayList<shell_lexical_frame> frames;
  ArrayList<shell_pending_heredoc> pending_heredocs;
  ArrayList<shell_lexical_construct> constructs;
  HashSet known_function_names;
  shell_lexical_frame root_frame{0, 0, 0, 0, shell_lexical_frame_kind::command,
                                 0};
  usize source_position{0};
  usize active_heredoc_index{0};
  char quote{0};
  bool is_in_ansi_c_quote{false};
  bool is_in_comment{false};
  bool is_in_heredoc{false};
};

class shell_highlight_cache
{
public:
  fn spans_for(StringView source, usize line_start, usize line_end,
               EvalContext &context) throws
      -> const ArrayList<highlight_span> *;

private:
  struct cached_line
  {
    usize start;
    usize end;
    ArrayList<highlight_span> spans{heap_allocator()};
  };

  StringView m_source{};
  ArrayList<shell_lexical_state> m_checkpoints{heap_allocator()};
  shell_lexical_state m_sequential_state{heap_allocator()};
  usize m_next_checkpoint_threshold{0};
  ArrayList<cached_line> m_lines{heap_allocator()};
};

fn highlight_line(StringView line, EvalContext &context) throws
    -> ArrayList<highlight_span>;

fn append_highlighted_range(String &output, StringView text,
                            const ArrayList<highlight_span> &spans,
                            usize range_start, usize range_end,
                            const highlight_theme &theme) throws -> void;
fn append_highlighted_source(String &output, StringView source,
                             EvalContext &context,
                             const highlight_theme &theme) throws -> void;

#if !defined NDEBUG
pure fn debug_highlight_input_byte_count() wontthrow -> usize;
pure fn debug_shell_lexical_scan_byte_count() wontthrow -> usize;
fn debug_diagnostic_cache_is_stable(EvalContext &context) throws -> bool;
#endif

/* The verdicts are cached per word and the cache drops when PATH changes. */
fn command_word_resolves(StringView line, EvalContext &context) throws -> bool;

} /* namespace completion */

} /* namespace koshka */
