/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements shell support for justfile documents. It finds
 * embedded shell source, selects its dialect, and maps parsing, formatting,
 * and diagnostics back to the host file.
 */

#include "../ParserFormats.hpp"

namespace koshka {

fn parse_justfile_format(const parser_format_input &input,
                         parsed_format_document &document) throws -> void
{
  bool is_recipe = false;
  mimic_mood mood = mimic_mood::Posix;
  usize position = 0;
  while (position < input.source.length) {
    let const line_start = position;
    let const line = input.source.next_line(position);
    if (parser_format_has_substring(line, "set shell") &&
        parser_format_has_substring(line, "bash"))
      mood = mimic_mood::Bash;
    if (!line.is_empty() && line[0] != ' ' && line[0] != '\t') {
      is_recipe = line[line.length - 1] == ':' &&
                  !parser_format_has_substring(line, ":=");
      continue;
    }
    if (!is_recipe) continue;
    usize shell_start = 0;
    while (shell_start < line.length &&
           (line[shell_start] == ' ' || line[shell_start] == '\t'))
      shell_start++;
    if (shell_start == line.length) continue;
    if (shell_start < line.length && line[shell_start] == '@') shell_start++;
    parser_format_add_fragment(document, input.source, line_start + shell_start,
                               line_start + line.length, mood);
  }
}

} // namespace koshka
