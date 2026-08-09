#pragma once

#include "Common.hpp"
#include "Eval.hpp"
#include "HashSet.hpp"
#include "Highlight.hpp"
#include "Path.hpp"
#include "String.hpp"
#include "StringMap.hpp"
#include "StringView.hpp"

namespace shit {

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
            completion_mode mode = completion_mode::Ghost) throws
    -> completion_result;

fn complete_command_names(StringView token, command_match_mode match_mode,
                          EvalContext &context) throws -> ArrayList<String>;
fn complete_filesystem_names(StringView token, EvalContext &context,
                             const Path &base_directory) throws
    -> ArrayList<String>;

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
  shell_lexical_frame_kind kind;
  char parent_quote;
  usize case_depth{0};
  bool saw_case_keyword{false};
  bool case_pattern_expected{false};
  bool is_command_position{true};
};

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
  shell_lexical_frame root_frame{0, 0, shell_lexical_frame_kind::command, 0};
  usize source_position{0};
  usize active_heredoc_index{0};
  char quote{0};
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
  StringView m_source{};
  ArrayList<shell_lexical_state> m_checkpoints{heap_allocator()};
  shell_lexical_state m_sequential_state{heap_allocator()};
  usize m_next_checkpoint_threshold{0};
  ArrayList<highlight_span> m_spans{heap_allocator()};
  usize m_cached_line_start{0};
  usize m_cached_line_end{0};
  bool m_has_cached_line{false};
};

fn highlight_line(StringView line, EvalContext &context) throws
    -> ArrayList<highlight_span>;

fn append_highlighted_range(String &output, StringView text,
                            const ArrayList<highlight_span> &spans,
                            usize range_start, usize range_end,
                            const highlight_theme &theme) throws -> void;

#if !defined NDEBUG
pure fn debug_highlight_input_byte_count() wontthrow -> usize;
pure fn debug_shell_lexical_scan_byte_count() wontthrow -> usize;
fn debug_diagnostic_cache_is_stable(EvalContext &context) throws -> bool;
#endif

/* The verdicts are cached per word and the cache drops when PATH changes. */
fn command_word_resolves(StringView line, EvalContext &context) throws -> bool;

} /* namespace completion */

} /* namespace shit */
