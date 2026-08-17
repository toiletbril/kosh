#include "Arena.hpp"
#include "Builtin.hpp"
#include "Colors.hpp"
#include "Completion.hpp"
#include "CompletionInternal.hpp"
#include "Debug.hpp"
#include "HashSet.hpp"
#include "Koshkit.hpp"
#include "Lexer.hpp"
#include "Path.hpp"
#include "Platform.hpp"
#include "Tokens.hpp"
#include "Trace.hpp"
#include "Utils.hpp"

namespace koshka {

namespace completion {

static fn highlight_line_with_lexical_state(
    StringView line, EvalContext &context,
    const shell_lexical_state *lexical_state) throws
    -> ArrayList<highlight_span>
{
#if !defined NDEBUG
  DEBUG_HIGHLIGHT_INPUT_BYTE_COUNT += line.length;
#endif
  HIGHLIGHT_ARENA.reset();
  let const arena = bump_allocator(HIGHLIGHT_ARENA);
  let spans = ArrayList<highlight_span>{arena};
  let line_variable_names = HashSet{arena};
  if (lexical_state != nullptr && lexical_state->is_in_heredoc) {
    if (!line.is_empty())
      spans.push(highlight_span{0, line.length, highlight_role::heredoc});
    return spans;
  }

  let synthetic_line = String{arena};
  usize prefix_length = 0;
  let const do_append_group_state = [&](const shell_lexical_frame &frame)
                                        throws -> void {
    for (usize group_index = 0; group_index < frame.group_depth; group_index++)
      synthetic_line.append("( ");
  };
  let const do_append_construct_state = [&](usize frame_depth) throws -> void {
    for (let const &construct : lexical_state->constructs) {
      if (construct.frame_depth != frame_depth) continue;
      switch (construct.kind) {
      case highlight_construct::if_:
        synthetic_line.append(construct.phase == highlight_construct_phase::body
                                  ? "if :; then "
                                  : "if :; ");
        break;
      case highlight_construct::while_until:
        synthetic_line.append(construct.phase == highlight_construct_phase::body
                                  ? "while :; do "
                                  : "while :; ");
        break;
      case highlight_construct::for_:
        switch (construct.phase) {
        case highlight_construct_phase::for_variable:
          synthetic_line.append("for ");
          break;
        case highlight_construct_phase::for_in:
          synthetic_line.append("for x\n");
          break;
        case highlight_construct_phase::for_do:
          synthetic_line.append("for x in a; ");
          break;
        default: synthetic_line.append("for x; do "); break;
        }
        break;
      case highlight_construct::conditional:
        synthetic_line.append("[[ x ");
        break;
      case highlight_construct::function:
        synthetic_line.append(construct.phase ==
                                      highlight_construct_phase::function_name
                                  ? "function "
                                  : "function f { ");
        break;
      case highlight_construct::case_: break;
      }
    }
  };
  let const do_append_frame_semantic_state =
      [&](const shell_lexical_frame &frame) throws -> void {
    if (frame.has_seen_case_keyword) {
      synthetic_line.append("case x ");
      return;
    }

    for (usize case_index = 0; case_index < frame.case_depth; case_index++) {
      synthetic_line.append("case x in ");
      if (case_index + 1 < frame.case_depth || !frame.is_case_pattern_expected)
      {
        synthetic_line.append("x) ");
      }
    }

    if (!frame.is_command_position && !frame.is_case_pattern_expected) {
      synthetic_line.append(": ");
    }
  };
  if (lexical_state != nullptr &&
      (!lexical_state->frames.is_empty() || lexical_state->quote != 0 ||
       !lexical_state->constructs.is_empty() ||
       !lexical_state->root_frame.is_command_position ||
       lexical_state->root_frame.group_depth > 0 ||
       lexical_state->root_frame.case_depth > 0 ||
       lexical_state->root_frame.has_seen_case_keyword ||
       lexical_state->root_frame.is_case_pattern_expected))
  {
    do_append_group_state(lexical_state->root_frame);
    do_append_construct_state(0);
    do_append_frame_semantic_state(lexical_state->root_frame);
    for (usize frame_index = 0; frame_index < lexical_state->frames.count();
         frame_index++)
    {
      let const &frame = lexical_state->frames[frame_index];
      if (frame.parent_quote != 0) synthetic_line.push(frame.parent_quote);
      switch (frame.kind) {
      case shell_lexical_frame_kind::command:
        synthetic_line.append("$(");
        break;
      case shell_lexical_frame_kind::backtick: synthetic_line.push('`'); break;
      case shell_lexical_frame_kind::arithmetic:
        synthetic_line.append("$((");
        break;
      case shell_lexical_frame_kind::parameter:
        synthetic_line.append("${");
        break;
      }
      do_append_group_state(frame);
      do_append_construct_state(frame_index + 1);
      if (frame.kind == shell_lexical_frame_kind::command ||
          frame.kind == shell_lexical_frame_kind::backtick)
      {
        do_append_frame_semantic_state(frame);
      }
    }
    if (lexical_state->quote != 0) synthetic_line.push(lexical_state->quote);
    prefix_length = synthetic_line.count();
    synthetic_line.append(line);
    line = synthetic_line.view();
  }

  scan_highlight_range(
      line, 0, line.length, context, spans, line_variable_names,
      lexical_state == nullptr ? nullptr
                               : &lexical_state->known_function_names);
  if (prefix_length == 0) return spans;

  usize retained_span_count = 0;
  for (usize span_index = 0; span_index < spans.count(); span_index++) {
    let span = spans[span_index];
    if (span.end <= prefix_length) continue;
    if (span.start < prefix_length) span.start = prefix_length;
    span.start -= prefix_length;
    span.end -= prefix_length;
    spans[retained_span_count++] = span;
  }
  while (spans.count() > retained_span_count)
    spans.pop_back();
  return spans;
}

fn highlight_line(StringView line, EvalContext &context) throws
    -> ArrayList<highlight_span>
{
  return highlight_line_with_lexical_state(line, context, nullptr);
}

fn append_highlighted_range(String &output, StringView text,
                            const ArrayList<highlight_span> &spans,
                            usize range_start, usize range_end,
                            const highlight_theme &theme) throws -> void
{
  if (range_end > text.length) range_end = text.length;
  if (range_start > range_end) range_start = range_end;

  let rendered_position = range_start;
  for (let const &span : spans) {
    if (span.end <= range_start) continue;
    if (span.start >= range_end) break;

    let const span_start = span.start < range_start ? range_start : span.start;
    let const span_end = span.end > range_end ? range_end : span.end;
    if (rendered_position < span_start)
      output += text.substring_of_length(rendered_position,
                                         span_start - rendered_position);

    let const style = theme.style_for(span.role);
    if (!style.is_empty()) output += style;
    output += text.substring_of_length(span_start, span_end - span_start);
    if (!style.is_empty()) output += theme.reset;
    rendered_position = span_end;
  }

  if (rendered_position < range_end)
    output += text.substring_of_length(rendered_position,
                                       range_end - rendered_position);
}

fn append_highlighted_source(String &output, StringView source,
                             EvalContext &context,
                             const highlight_theme &theme) throws -> void
{
  let cache = shell_highlight_cache{};
  usize line_start = 0;
  while (line_start < source.length) {
    let line_end = line_start;
    while (line_end < source.length && source[line_end] != '\n')
      line_end++;
    if (line_end < source.length) line_end++;

    let const *spans = cache.spans_for(source, line_start, line_end, context);
    let const line =
        source.substring_of_length(line_start, line_end - line_start);
    append_highlighted_range(output, line, *spans, 0, line.length, theme);
    line_start = line_end;
  }
}

static constexpr usize DIAGNOSTIC_CHECKPOINT_BYTE_INTERVAL = 4096;

fn shell_highlight_cache::spans_for(StringView source, usize line_start,
                                    usize line_end, EvalContext &context) throws
    -> const ArrayList<highlight_span> *
{
  let const source_changed =
      source.data != m_source.data || source.length != m_source.length;
  if (source_changed) {
    m_source = source;
    m_checkpoints.clear();
    m_lines.clear();
    m_sequential_state = shell_lexical_state{heap_allocator()};

    let state = shell_lexical_state{heap_allocator()};
    m_checkpoints.push(state);
    m_next_checkpoint_threshold = DIAGNOSTIC_CHECKPOINT_BYTE_INTERVAL;
  }

  usize cached_line_index = 0;
  usize cached_line_limit = m_lines.count();
  while (cached_line_index < cached_line_limit) {
    let const middle =
        cached_line_index + (cached_line_limit - cached_line_index) / 2;
    let const &line = m_lines[middle];
    if (line.start < line_start ||
        (line.start == line_start && line.end < line_end))
    {
      cached_line_index = middle + 1;
    } else {
      cached_line_limit = middle;
    }
  }
  if (cached_line_index < m_lines.count()) {
    let &line = m_lines[cached_line_index];
    if (line_start == line.start && line_end == line.end) return &line.spans;
  }

  let const do_advance_with_checkpoints = [&](shell_lexical_state &state,
                                              usize target) throws -> void {
    while (m_next_checkpoint_threshold < source.length &&
           m_next_checkpoint_threshold < target)
    {
      usize checkpoint_position = m_next_checkpoint_threshold;
      while (checkpoint_position < source.length &&
             source[checkpoint_position - 1] != '\n')
        checkpoint_position++;
      if (checkpoint_position >= source.length) {
        m_next_checkpoint_threshold = source.length;
        break;
      }
      if (checkpoint_position > target) break;
      advance_shell_lexical_state(source, checkpoint_position, state);
      m_checkpoints.push(state);
      m_next_checkpoint_threshold =
          checkpoint_position + DIAGNOSTIC_CHECKPOINT_BYTE_INTERVAL;
    }
    advance_shell_lexical_state(source, target, state);
  };

  let lexical_state = shell_lexical_state{heap_allocator()};
  const shell_lexical_state *line_state = &m_sequential_state;
  if (m_sequential_state.source_position != line_start) {
    usize checkpoint_index = 0;
    usize checkpoint_limit = m_checkpoints.count();
    while (checkpoint_index + 1 < checkpoint_limit) {
      let const middle =
          checkpoint_index + (checkpoint_limit - checkpoint_index) / 2;
      if (m_checkpoints[middle].source_position <= line_start)
        checkpoint_index = middle;
      else
        checkpoint_limit = middle;
    }

    lexical_state = m_checkpoints[checkpoint_index];
    do_advance_with_checkpoints(lexical_state, line_start);
    line_state = &lexical_state;
  }
  let const source_line =
      source.substring_of_length(line_start, line_end - line_start);
#if !defined NDEBUG
  LOG(All, "highlighting cached line at %zu with %zu frames and quote %d",
      line_state->source_position, line_state->frames.count(),
      line_state->quote);
  for (let const &construct : line_state->constructs)
    LOG(All, "reconstructing lexical construct %u in phase %u",
        static_cast<unsigned>(construct.kind),
        static_cast<unsigned>(construct.phase));
#endif
  let const generated =
      highlight_line_with_lexical_state(source_line, context, line_state);
  let cached = cached_line{line_start, line_end};
  cached.spans.reserve(generated.count());
  for (let const &span : generated)
    cached.spans.push(span);
  m_lines.push(steal(cached));
  for (usize index = m_lines.count() - 1; index > cached_line_index; index--)
    std::swap(m_lines[index], m_lines[index - 1]);
  if (line_state != &m_sequential_state)
    m_sequential_state = steal(lexical_state);
  let state_target = line_end;
  if (state_target < source.length && source[state_target] == '\n') {
    state_target++;
  }
  do_advance_with_checkpoints(m_sequential_state, state_target);
  return &m_lines[cached_line_index].spans;
}

#if !defined NDEBUG
pure fn debug_highlight_input_byte_count() wontthrow -> usize
{
  return DEBUG_HIGHLIGHT_INPUT_BYTE_COUNT;
}

fn debug_diagnostic_cache_is_stable(EvalContext &context) throws -> bool
{
  let cache = shell_highlight_cache{};
  let const source = String{
      "value=\"$(printf value\n) tail\"\nif true\nthen value='start\nend'; fi"};
  usize keyword_line_start = 0;
  usize string_line_start = 0;
  usize line_index = 0;
  for (usize position = 0; position < source.count(); position++) {
    if (source[position] != '\n') continue;
    line_index++;
    if (line_index == 3) keyword_line_start = position + 1;
    if (line_index == 4) string_line_start = position + 1;
  }

  let keyword_line_end = keyword_line_start;
  while (keyword_line_end < source.count() && source[keyword_line_end] != '\n')
    keyword_line_end++;
  let const *keyword_spans = cache.spans_for(source.view(), keyword_line_start,
                                             keyword_line_end, context);
  let has_keyword_span = false;
  for (let const &span : *keyword_spans) {
    LOG(All, "the cached diagnostic span covers %zu through %zu as role %u",
        span.start, span.end, static_cast<unsigned>(span.role));
    if (span.start == 0 && span.end == 4 &&
        span.role == highlight_role::keyword)
      has_keyword_span = true;
  }
  if (!has_keyword_span) return false;

  let const line_start = string_line_start;
  let const *first =
      cache.spans_for(source.view(), line_start, source.count(), context);
  let expected = ArrayList<highlight_span>{heap_allocator()};
  expected.reserve(first->count());
  for (let const &span : *first)
    expected.push(span);

  unused(highlight_line("printf reset", context));
  let const *repeated =
      cache.spans_for(source.view(), line_start, source.count(), context);
  if (repeated->count() != expected.count()) return false;
  for (usize index = 0; index < expected.count(); index++) {
    if ((*repeated)[index].start != expected[index].start ||
        (*repeated)[index].end != expected[index].end ||
        (*repeated)[index].role != expected[index].role)
    {
      return false;
    }
  }

  let function_cache = shell_highlight_cache{};
  let const function_source = String{"finish() { :; }\nfinish"};
  let const function_call_start = function_source.view().find_character('\n');
  if (!function_call_start.has_value()) return false;
  let const *function_spans =
      function_cache.spans_for(function_source.view(), *function_call_start + 1,
                               function_source.count(), context);
  let has_resolved_function_call = false;
  for (let const &span : *function_spans)
    if (span.start == 0 && span.end == 6 &&
        span.role == highlight_role::resolved_command)
      has_resolved_function_call = true;
  if (!has_resolved_function_call) return false;

  let const other_source = String{"echo ok"};
  let const *invalidated =
      cache.spans_for(other_source.view(), 0, other_source.count(), context);
  if (invalidated->count() != expected.count()) return true;
  for (usize index = 0; index < expected.count(); index++) {
    if ((*invalidated)[index].start != expected[index].start ||
        (*invalidated)[index].end != expected[index].end ||
        (*invalidated)[index].role != expected[index].role)
    {
      return true;
    }
  }

  return false;
}
#endif

} /* namespace completion */

} /* namespace koshka */
