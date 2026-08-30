#include "../ParserFormats.hpp"

namespace koshka {

fn parse_makefile_format(const parser_format_input &input,
                         parsed_format_document &document) throws -> void
{
  let analysis_source = String{heap_allocator()};
  analysis_source.reserve(input.source.length);
  usize analysis_position = 0;
  while (analysis_position < input.source.length) {
    let const line_start = analysis_position;
    let const line = input.source.next_line(analysis_position);
    if (line.is_empty() || line[0] != '\t') {
      analysis_source.append(line);
    } else {
      usize line_position = 0;
      while (line_position < line.length) {
        if (line[line_position] != '$' || line_position + 1 == line.length) {
          analysis_source.push(line[line_position++]);
          continue;
        }

        let const next_byte = line[line_position + 1];
        if (next_byte == '$') {
          analysis_source.push(' ');
          analysis_source.push('$');
          line_position += 2;
          continue;
        }
        if (next_byte != '(' && next_byte != '{') {
          analysis_source.append("  ");
          line_position += 2;
          continue;
        }

        let const closing_byte = next_byte == '(' ? ')' : '}';
        usize expansion_depth = 1;
        usize expansion_end_position = line_position + 2;
        while (expansion_end_position < line.length && expansion_depth != 0) {
          if (line[expansion_end_position] == '$' &&
              expansion_end_position + 1 < line.length &&
              line[expansion_end_position + 1] == next_byte)
          {
            expansion_depth++;
            expansion_end_position += 2;
            continue;
          }
          if (line[expansion_end_position++] == closing_byte) expansion_depth--;
        }

        analysis_source.append_repeated(' ',
                                        expansion_end_position - line_position);
        line_position = expansion_end_position;
      }
    }
    if (analysis_position > line_start + line.length)
      analysis_source.push('\n');
  }

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
    let const fragment_count = document.fragments.count();
    parser_format_add_fragment(document, analysis_source.view(),
                               line_start + shell_start,
                               line_start + line.length, mood);
    if (document.fragments.count() != fragment_count) {
      document.fragments.back().shell_source =
          String{input.source.substring_of_length(line_start + shell_start,
                                                  line.length - shell_start)};
    }
  }
}

} // namespace koshka
