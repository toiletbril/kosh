/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements shell support for dockerfile documents. It finds
 * embedded shell source, selects its dialect, and maps parsing, formatting,
 * and diagnostics back to the host file.
 */

#include "../ParserFormats.hpp"

namespace koshka {

enum class docker_instruction_kind : u8
{
  None,
  Shell,
  Run,
  Command,
  Entrypoint,
  Healthcheck,
};

static pure fn docker_instruction(StringView line,
                                  usize &content_position) wontthrow
    -> docker_instruction_kind
{
  usize position = 0;
  while (position < line.length &&
         (line[position] == ' ' || line[position] == '\t'))
    position++;

  let const instruction_start = position;
  while (position < line.length && line[position] != ' ' &&
         line[position] != '\t')
    position++;
  let const instruction =
      line.substring_of_length(instruction_start, position - instruction_start);
  let kind = docker_instruction_kind::None;
  if (!instruction.is_empty()) {
    switch (instruction[0]) {
    case 'C':
    case 'c':
      if (parser_format_ascii_equal(instruction, "CMD"))
        kind = docker_instruction_kind::Command;
      break;
    case 'E':
    case 'e':
      if (parser_format_ascii_equal(instruction, "ENTRYPOINT"))
        kind = docker_instruction_kind::Entrypoint;
      break;
    case 'H':
    case 'h':
      if (parser_format_ascii_equal(instruction, "HEALTHCHECK"))
        kind = docker_instruction_kind::Healthcheck;
      break;
    case 'R':
    case 'r':
      if (parser_format_ascii_equal(instruction, "RUN"))
        kind = docker_instruction_kind::Run;
      break;
    case 'S':
    case 's':
      if (parser_format_ascii_equal(instruction, "SHELL"))
        kind = docker_instruction_kind::Shell;
      break;
    default: break;
    }
  }
  if (kind == docker_instruction_kind::None) return kind;

  while (position < line.length &&
         (line[position] == ' ' || line[position] == '\t'))
    position++;
  content_position = position;

  return kind;
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
    let const instruction = docker_instruction(line, content_position);
    if (instruction == docker_instruction_kind::Shell) {
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
        instruction == docker_instruction_kind::Run ||
        instruction == docker_instruction_kind::Command ||
        instruction == docker_instruction_kind::Entrypoint;
    if (instruction == docker_instruction_kind::Healthcheck) {
      let const remainder = line.substring(content_position);
      usize command_position = 0;
      if (docker_instruction(remainder, command_position) ==
          docker_instruction_kind::Command)
      {
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
