/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements terminal color policy. It caches descriptor-sensitive
 * color decisions and emits the escape sequences used by diagnostics and
 * interactive output.
 */

#include "CliColors.hpp"

#include "Platform.hpp"
#include "Trace.hpp"

namespace koshka {

namespace colors {

static fn make_shell_highlight_theme() wontthrow -> highlight_theme
{
  let theme = highlight_theme{};
  theme.reset = ansi::RESET;
  theme.set_style(highlight_role::comment, ansi::DIM);
  theme.set_style(highlight_role::operator_, ansi::BOLD_MAGENTA);
  theme.set_style(highlight_role::string, ansi::BRIGHT_GREEN);
  theme.set_style(highlight_role::heredoc, ansi::BRIGHT_GREEN);
  theme.set_style(highlight_role::heredoc_delimiter, ansi::BOLD_BRIGHT_GREEN);
  theme.set_style(highlight_role::variable, ansi::BRIGHT_CYAN);
  theme.set_style(highlight_role::assignment_name, ansi::BRIGHT_CYAN);
  theme.set_style(highlight_role::unset_variable,
                  ansi::RED_CURLY_YELLOW_UNDERLINE);
  theme.set_style(highlight_role::flag, ansi::ITALIC);
  theme.set_style(highlight_role::keyword, ansi::BOLD_MAGENTA);
  theme.set_style(highlight_role::invalid_syntax,
                  ansi::BOLD_RED_CURLY_YELLOW_UNDERLINE);
  theme.set_style(highlight_role::function_name, ansi::BRIGHT_BLUE);
  theme.set_style(highlight_role::resolved_command, ansi::BLUE);
  theme.set_style(highlight_role::partial_command, ansi::BRIGHT_BLUE);
  theme.set_style(highlight_role::unknown_command,
                  ansi::RED_CURLY_YELLOW_UNDERLINE);
  theme.set_style(highlight_role::existing_path, ansi::BRIGHT_CYAN);
  theme.set_style(highlight_role::partial_path, ansi::CYAN);
  theme.set_style(highlight_role::invalid_path,
                  ansi::RED_CURLY_YELLOW_UNDERLINE);
  theme.set_style(highlight_role::url, ansi::BOLD_WHITE);
  theme.set_style(highlight_role::glob, ansi::YELLOW);
  return theme;
}

static fn make_noninteractive_highlight_theme() wontthrow -> highlight_theme
{
  let theme = make_shell_highlight_theme();
  theme.set_style(highlight_role::unset_variable, ansi::BRIGHT_RED);
  theme.set_style(highlight_role::invalid_syntax, ansi::BOLD_BRIGHT_RED);
  theme.set_style(highlight_role::unknown_command, ansi::BRIGHT_RED);
  theme.set_style(highlight_role::invalid_path, ansi::BRIGHT_RED);
  return theme;
}

static fn make_printed_source_highlight_theme() wontthrow -> highlight_theme
{
  let theme = make_noninteractive_highlight_theme();
  theme.set_style(highlight_role::unset_variable,
                  theme.style_for(highlight_role::variable));
  theme.set_style(highlight_role::unknown_command,
                  theme.style_for(highlight_role::resolved_command));
  theme.set_style(highlight_role::partial_command,
                  theme.style_for(highlight_role::resolved_command));
  return theme;
}

static fn make_diagnostic_highlight_theme() wontthrow -> highlight_theme
{
  let theme = make_printed_source_highlight_theme();
  theme.set_style(highlight_role::partial_path, ansi::BRIGHT_RED);
  return theme;
}

const highlight_theme SHELL_HIGHLIGHT_THEME = make_shell_highlight_theme();
const highlight_theme NONINTERACTIVE_HIGHLIGHT_THEME =
    make_noninteractive_highlight_theme();
const highlight_theme PRINTED_SOURCE_HIGHLIGHT_THEME =
    make_printed_source_highlight_theme();
const highlight_theme DIAGNOSTIC_HIGHLIGHT_THEME =
    make_diagnostic_highlight_theme();

static fn color_is_suppressed_by_environment() throws -> bool
{
  if (let const no_color = os::get_environment_variable("NO_COLOR");
      no_color.has_value() && !no_color->is_empty())
  {
    LOG(Info, "suppressing color because NO_COLOR is set");
    return true;
  }

  if (let const term = os::get_environment_variable("TERM");
      term.has_value() && term->view() == StringView{"dumb"})
  {
    LOG(Info, "suppressing color because TERM is dumb");
    return true;
  }

  return false;
}

/* isatty is a syscall and diagnostic rendering asks once per message. The
   answer is kept until a redirection rebinds a standard descriptor. */
struct cached_terminal_answer
{
  u64 epoch{static_cast<u64>(-1)};
  bool is_a_terminal{false};

  template <typename Probe>
  fn get(Probe probe) wontthrow -> bool
  {
    let const current_epoch = os::get_descriptor_epoch();
    if (epoch != current_epoch) {
      epoch = current_epoch;
      is_a_terminal = probe();
    }

    return is_a_terminal;
  }
};

static cached_terminal_answer STDOUT_TERMINAL_ANSWER{};
static cached_terminal_answer STDERR_TERMINAL_ANSWER{};

fn stdout_wants_color() throws -> bool
{
  return terminal_wants_color(stdout_is_a_terminal());
}

fn stderr_wants_color() throws -> bool
{
  return terminal_wants_color(stderr_is_a_terminal());
}

fn stdout_is_a_terminal() wontthrow -> bool
{
  return STDOUT_TERMINAL_ANSWER.get(os::is_stdout_a_tty);
}

fn stderr_is_a_terminal() wontthrow -> bool
{
  return STDERR_TERMINAL_ANSWER.get(os::is_stderr_a_tty);
}

fn terminal_wants_color(bool output_is_terminal) throws -> bool
{
  return output_is_terminal && !color_is_suppressed_by_environment();
}

fn terminal_supports_styled_underlines() throws -> bool { return false; }

} /* namespace colors */

pure fn highlight_role_name(highlight_role role) wontthrow -> StringView
{
  switch (role) {
  case highlight_role::comment: return "comment";
  case highlight_role::operator_: return "operator";
  case highlight_role::string: return "string";
  case highlight_role::heredoc: return "heredoc";
  case highlight_role::heredoc_delimiter: return "heredoc-delimiter";
  case highlight_role::variable: return "variable";
  case highlight_role::assignment_name: return "assignment-name";
  case highlight_role::unset_variable: return "unset-variable";
  case highlight_role::flag: return "flag";
  case highlight_role::keyword: return "keyword";
  case highlight_role::invalid_syntax: return "invalid-syntax";
  case highlight_role::function_name: return "function-name";
  case highlight_role::resolved_command: return "resolved-command";
  case highlight_role::partial_command: return "partial-command";
  case highlight_role::unknown_command: return "unknown-command";
  case highlight_role::existing_path: return "existing-path";
  case highlight_role::partial_path: return "partial-path";
  case highlight_role::invalid_path: return "invalid-path";
  case highlight_role::url: return "url";
  case highlight_role::glob: return "glob";
  case highlight_role::count: break;
  }

  return "unknown";
}

} /* namespace koshka */
