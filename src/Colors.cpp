#include "Colors.hpp"

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

fn stdout_wants_color() throws -> bool
{
  return terminal_wants_color(os::is_stdout_a_tty());
}

fn stderr_wants_color() throws -> bool
{
  return terminal_wants_color(os::is_stderr_a_tty());
}

fn terminal_wants_color(bool output_is_terminal) throws -> bool
{
  return output_is_terminal && !color_is_suppressed_by_environment();
}

fn terminal_supports_styled_underlines() throws -> bool
{
  static constexpr bool IS_STYLED_UNDERLINE_SUPPORT_ENABLED = false;
  if (!IS_STYLED_UNDERLINE_SUPPORT_ENABLED) return false;

  let const term = os::get_environment_variable("TERM");
  if (!term.has_value() || term->is_empty()) return false;

  static const StringView SUPPORTED_TERMS[] = {
      "alacritty",      "alacritty-direct",
      "contour",        "contour-direct",
      "foot",           "foot-direct",
      "foot-extra",     "ghostty",
      "ghostty-direct", "kitty",
      "kitty-direct",   "rio",
      "rio-direct",     "wezterm",
      "wezterm-direct", "xterm-ghostty",
      "xterm-kitty",
  };
  for (let const supported : SUPPORTED_TERMS)
    if (term->view() == supported) return true;

  return false;
}

} /* namespace colors */

pure fn highlight_role_name(highlight_role role) wontthrow -> StringView
{
  switch (role) {
  case highlight_role::comment: return "comment";
  case highlight_role::operator_: return "operator";
  case highlight_role::string: return "string";
  case highlight_role::heredoc: return "heredoc";
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
