#pragma once

#include "Common.hpp"
#include "Highlight.hpp"
#include "StringView.hpp"

namespace koshka {

namespace colors {

namespace ansi {
inline const StringView RESET = "\x1b[0m";
inline const StringView BOLD = "\x1b[1m";
inline const StringView DIM = "\x1b[2m";
inline const StringView ITALIC = "\x1b[3m";
inline const StringView INVERSE = "\x1b[7m";
inline const StringView RED = "\x1b[31m";
inline const StringView GREEN = "\x1b[32m";
inline const StringView YELLOW = "\x1b[33m";
inline const StringView BLUE = "\x1b[34m";
/* The high-intensity foreground colors, distinct from the bold attribute. */
inline const StringView BRIGHT_RED = "\x1b[91m";
inline const StringView BRIGHT_GREEN = "\x1b[92m";
inline const StringView BRIGHT_BLUE = "\x1b[94m";
inline const StringView BRIGHT_MAGENTA = "\x1b[95m";
inline const StringView BRIGHT_CYAN = "\x1b[96m";
inline const StringView BOLD_RED = "\x1b[1;31m";
inline const StringView BOLD_BRIGHT_RED = "\x1b[1;91m";
inline const StringView BOLD_GREEN = "\x1b[1;32m";
inline const StringView BOLD_YELLOW = "\x1b[1;33m";
inline const StringView CYAN = "\x1b[36m";
inline const StringView BOLD_MAGENTA = "\x1b[1;35m";
inline const StringView BOLD_CYAN = "\x1b[1;36m";
inline const StringView BOLD_WHITE = "\x1b[1;37m";
inline const StringView RED_CURLY_YELLOW_UNDERLINE = "\x1b[91;4:3;58:5:3m";
inline const StringView BOLD_RED_CURLY_YELLOW_UNDERLINE =
    "\x1b[1;91;4:3;58:5:3m";
} /* namespace ansi */

/* Whether color may be written to a stream. Color is on only when the stream is
   a terminal, NO_COLOR is unset or empty, and TERM is not dumb. The terminal
   answer is cached until a redirection rebinds the descriptor. A redirected
   stream never gains escapes. */
fn stdout_wants_color() throws -> bool;
fn stderr_wants_color() throws -> bool;
fn stderr_is_a_terminal() wontthrow -> bool;
fn terminal_wants_color(bool output_is_terminal) throws -> bool;
fn terminal_supports_styled_underlines() throws -> bool;

extern const highlight_theme SHELL_HIGHLIGHT_THEME;
extern const highlight_theme NONINTERACTIVE_HIGHLIGHT_THEME;
extern const highlight_theme PRINTED_SOURCE_HIGHLIGHT_THEME;
extern const highlight_theme DIAGNOSTIC_HIGHLIGHT_THEME;

} /* namespace colors */

} /* namespace koshka */
