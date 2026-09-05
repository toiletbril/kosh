/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements the Toiletline interface for KOSH_NO_TOILETLINE builds.
 * The alternate implementation keeps those builds linked without compiling
 * the vendored editor or initializing terminal state.
 */

#include "CliColors.hpp"
#include "Errors.hpp"
#include "Eval.hpp"
#include "Toiletline.hpp"

#if defined KOSH_NO_TOILETLINE

namespace toiletline {

using koshka::String;
using koshka::StringView;

fn enable_completion(koshka::EvalContext &context) -> void { unused(context); }

fn disable_completion() -> void {}

fn completion_is_enabled() -> bool { return false; }

fn enter_calc_history() -> void {}

fn leave_calc_history() -> void {}

fn history_path() -> koshka::Maybe<koshka::Path> { return koshka::None; }

fn history_write() -> bool { return true; }

fn history_read() -> bool { return true; }

fn history_clear() -> bool { return true; }

fn set_history_enabled(bool is_enabled) -> void { unused(is_enabled); }

fn set_history_limit(usize entry_count) -> void { unused(entry_count); }

fn history_events(koshka::Allocator allocator)
    -> koshka::ArrayList<history_event>
{
  return koshka::ArrayList<history_event>{allocator};
}

fn relative_history_event(koshka::Allocator allocator, usize distance,
                          koshka::Maybe<usize> before_event_number)
    -> koshka::Maybe<history_event>
{
  unused(allocator);
  unused(distance);
  unused(before_event_number);
  return koshka::None;
}

fn numbered_history_event(koshka::Allocator allocator, usize number,
                          koshka::Maybe<usize> before_event_number)
    -> koshka::Maybe<history_event>
{
  unused(allocator);
  unused(number);
  unused(before_event_number);
  return koshka::None;
}

fn prefixed_history_event(koshka::Allocator allocator, StringView prefix,
                          koshka::Maybe<usize> before_event_number)
    -> koshka::Maybe<history_event>
{
  unused(allocator);
  unused(prefix);
  unused(before_event_number);
  return koshka::None;
}

fn containing_history_event(koshka::Allocator allocator, StringView text,
                            koshka::Maybe<usize> before_event_number)
    -> koshka::Maybe<history_event>
{
  unused(allocator);
  unused(text);
  unused(before_event_number);
  return koshka::None;
}

fn history_append_event(StringView command,
                        history_duplicate_policy duplicate_policy)
    -> koshka::Maybe<usize>
{
  unused(command);
  unused(duplicate_policy);
  return koshka::None;
}

fn history_rewrite_event(usize number, StringView expected,
                         StringView replacement) -> bool
{
  unused(number);
  unused(expected);
  unused(replacement);
  return false;
}

fn history_rewrite_event(usize number, StringView expected,
                         const koshka::ArrayList<koshka::String> &replacements)
    -> bool
{
  unused(number);
  unused(expected);
  unused(replacements);
  return false;
}

fn enable_job_notifications(koshka::EvalContext &context) -> void
{
  unused(context);
}

fn set_ghost_enabled(bool enabled) -> void { unused(enabled); }

fn set_highlight_enabled(bool enabled) -> void { unused(enabled); }

fn set_edit_mode(edit_mode mode) -> void { unused(mode); }

fn utf8_strlen(const String &s, usize count) -> usize
{
  return (count != static_cast<usize>(-1) && count < s.length()) ? count
                                                                 : s.length();
}

fn utf8_strnlen(const char *bytes, usize byte_count) -> usize
{
  unused(bytes);
  return byte_count;
}

fn display_width(StringView text) -> usize { return text.length; }

fn byte_offset_at_or_before_display_cell(StringView text, usize cell_position,
                                         usize &actual_cell_position) -> usize
{
  actual_cell_position =
      cell_position < text.length ? cell_position : text.length;
  return actual_cell_position;
}

fn is_active() -> bool { return false; }

fn initialize() -> void
{
  throw koshka::Error{
      "This build has no line editor, use '-c', '-s', or a file argument"};
}

fn exit(bool should_append_history, usize history_size_limit,
        usize history_file_size_limit) -> void
{
  unused(should_append_history);
  unused(history_size_limit);
  unused(history_file_size_limit);
}

fn set_title(StringView title) -> void { unused(title); }

fn set_idle_title() -> void {}

fn get_input(const String &prompt) -> input_result
{
  unused(prompt);
  throw koshka::Error{"This build has no line editor"};
}

fn set_input(const String &input) -> void { unused(input); }

fn enter_raw_mode() -> void {}

fn exit_raw_mode() -> void {}

fn emit_newlines(StringView buffer) -> void { unused(buffer); }

fn debug_allocation_failure() -> bool { return true; }

fn default_prompt_template() -> String
{
  let template_string = String{};
  let const should_use_color = koshka::colors::stdout_wants_color();

  if (should_use_color) {
    template_string += "[\\u@\\h${KOSH_GIT_BRANCH:+ (";
    template_string += koshka::colors::ansi::CYAN;
    template_string += "$KOSH_GIT_BRANCH";
    template_string += koshka::colors::ansi::RESET;
    template_string += ")} ";
    template_string += koshka::colors::ansi::GREEN;
    template_string += "\\P";
    template_string += koshka::colors::ansi::RESET;
  } else {
    template_string += "[\\u@\\h${KOSH_GIT_BRANCH:+ ($KOSH_GIT_BRANCH)} \\P";
  }
  template_string += "] ";
  return template_string;
}

fn build_prompt(koshka::EvalContext &context) -> String
{
  unused(context);
  throw koshka::Error{"This build has no line editor"};
}

fn expand_prompt_template(StringView prompt, koshka::EvalContext &context)
    -> String
{
  unused(context);
  return String{prompt};
}

fn render_ps0(koshka::EvalContext &context) -> String
{
  unused(context);
  return String{};
}

} /* namespace toiletline */

#endif /* KOSH_NO_TOILETLINE */
