#include "../ParserFormats.hpp"

namespace koshka {

static pure fn docker_instruction(StringView line, StringView wanted,
                                  usize &content_position) wontthrow -> bool
{
  usize position = 0;
  while (position < line.length &&
         (line[position] == ' ' || line[position] == '\t'))
    position++;
  if (position + wanted.length > line.length ||
      !parser_format_ascii_equal(
          line.substring_of_length(position, wanted.length), wanted))
    return false;
  position += wanted.length;
  if (position < line.length && line[position] != ' ' && line[position] != '\t')
    return false;
  while (position < line.length &&
         (line[position] == ' ' || line[position] == '\t'))
    position++;
  content_position = position;

  return true;
}

fn parse_dockerfile_format(const parser_format_input &input,
                           parsed_format_document &document) throws -> void
{
  mimic_mood mood = mimic_mood::Posix;
  usize position = 0;
  while (position < input.source.length) {
    let const line_start = position;
    let const line = input.source.next_line(position);
    usize content_position = 0;
    if (docker_instruction(line, "SHELL", content_position)) {
      let const shell = line.substring(content_position);
      if (parser_format_has_substring(shell, "bash"))
        mood = mimic_mood::Bash;
      else if (parser_format_has_substring(shell, "kosh"))
        mood = mimic_mood::Default;
      else if (parser_format_has_substring(shell, "sh"))
        mood = mimic_mood::Posix;
      continue;
    }

    bool is_shell_instruction =
        docker_instruction(line, "RUN", content_position) ||
        docker_instruction(line, "CMD", content_position) ||
        docker_instruction(line, "ENTRYPOINT", content_position);
    if (!is_shell_instruction &&
        docker_instruction(line, "HEALTHCHECK", content_position))
    {
      let const remainder = line.substring(content_position);
      usize command_position = 0;
      if (docker_instruction(remainder, "CMD", command_position)) {
        content_position += command_position;
        is_shell_instruction = true;
      }
    }
    if (!is_shell_instruction || content_position >= line.length ||
        line[content_position] == '[')
      continue;

    usize shell_end = line_start + line.length;
    let continued = !line.is_empty() && line[line.length - 1] == '\\';
    while (continued && position < input.source.length) {
      let const continuation = input.source.next_line(position);
      shell_end = position;
      continued = !continuation.is_empty() &&
                  continuation[continuation.length - 1] == '\\';
    }
    parser_format_add_fragment(document, input.source,
                               line_start + content_position, shell_end, mood);
  }
}

} // namespace koshka
