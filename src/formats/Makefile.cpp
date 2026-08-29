#include "../ParserFormats.hpp"

namespace koshka {

fn parse_makefile_format(const parser_format_input &input,
                         parsed_format_document &document) throws -> void
{
  mimic_mood mood = mimic_mood::Posix;
  usize position = 0;
  while (position < input.source.length) {
    let const line_start = position;
    let const line = input.source.next_line(position);
    if (parser_format_has_substring(line, "SHELL") &&
        parser_format_has_substring(line, "="))
    {
      if (parser_format_has_substring(line, "bash"))
        mood = mimic_mood::Bash;
      else if (parser_format_has_substring(line, "kosh"))
        mood = mimic_mood::Default;
      else if (parser_format_has_substring(line, "sh"))
        mood = mimic_mood::Posix;
    }
    if (line.is_empty() || line[0] != '\t') continue;
    usize shell_start = 1;
    while (shell_start < line.length &&
           (line[shell_start] == '@' || line[shell_start] == '-' ||
            line[shell_start] == '+'))
      shell_start++;
    parser_format_add_fragment(document, input.source, line_start + shell_start,
                               line_start + line.length, mood);
  }
}

} // namespace koshka
