#include "../ParserFormats.hpp"

namespace koshka {

static pure fn markdown_mood(StringView info) wontthrow -> Maybe<mimic_mood>
{
  info = info.trim_blanks();
  if (parser_format_ascii_equal(info, "bash") ||
      parser_format_ascii_equal(info, "shell") ||
      parser_format_ascii_equal(info, "shellscript"))
    return mimic_mood::Bash;
  if (parser_format_ascii_equal(info, "sh") ||
      parser_format_ascii_equal(info, "posix") ||
      parser_format_ascii_equal(info, "dash"))
    return mimic_mood::Posix;
  if (parser_format_ascii_equal(info, "kosh")) return mimic_mood::Default;

  return None;
}

fn parse_markdown_format(const parser_format_input &input,
                         parsed_format_document &document) throws -> void
{
  usize position = 0;
  while (position < input.source.length) {
    let const opener_start = position;
    let const line = input.source.next_line(position);
    usize fence_start = 0;
    while (fence_start < line.length && fence_start < 4 &&
           line[fence_start] == ' ')
      fence_start++;
    if (fence_start >= line.length ||
        (line[fence_start] != '`' && line[fence_start] != '~'))
      continue;
    let const fence_byte = line[fence_start];
    usize fence_length = 0;
    while (fence_start + fence_length < line.length &&
           line[fence_start + fence_length] == fence_byte)
      fence_length++;
    if (fence_length < 3) continue;
    let const mood = markdown_mood(line.substring(fence_start + fence_length));
    if (!mood.has_value()) continue;

    let const shell_start = position;
    usize shell_end = input.source.length;
    while (position < input.source.length) {
      let const closer_start = position;
      let const closer = input.source.next_line(position);
      usize closer_indent = 0;
      while (closer_indent < closer.length && closer_indent < 4 &&
             closer[closer_indent] == ' ')
        closer_indent++;
      usize closer_length = 0;
      while (closer_indent + closer_length < closer.length &&
             closer[closer_indent + closer_length] == fence_byte)
        closer_length++;
      if (closer_length < fence_length) continue;
      shell_end = closer_start;
      break;
    }
    if (shell_start < shell_end)
      parser_format_add_indented_fragment(document, input.source, shell_start,
                                          shell_end, fence_start, *mood);
    if (position <= opener_start) break;
  }
}

} // namespace koshka
