/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file extracts string values from the scripts object in package.json as
 * POSIX shell fragments. JSON decoding and encoding preserve escape sequences
 * and host positions during analysis and formatting.
 */

#include "../ParserFormats.hpp"

namespace koshka {

static pure fn skip_json_space(StringView source, usize position) wontthrow
    -> usize
{
  while (position < source.length &&
         (source[position] == ' ' || source[position] == '\t' ||
          source[position] == '\n' || source[position] == '\r'))
    position++;

  return position;
}

static pure fn json_string_end(StringView source, usize position) wontthrow
    -> usize
{
  while (position < source.length && source[position] != '"') {
    if (source[position] == '\\' && position + 1 < source.length) position++;
    position++;
  }

  return position;
}

fn parse_package_json_format(const parser_format_input &input,
                             parsed_format_document &document) throws -> void
{
  usize position = 0;
  while (position < input.source.length) {
    position = skip_json_space(input.source, position);
    if (position >= input.source.length || input.source[position] != '"') {
      position++;
      continue;
    }
    let const key_start = position + 1;
    let const key_end = json_string_end(input.source, key_start);
    if (key_end >= input.source.length) return;
    position = skip_json_space(input.source, key_end + 1);
    if (position >= input.source.length || input.source[position] != ':')
      continue;
    position = skip_json_space(input.source, position + 1);
    if (input.source.substring_of_length(key_start, key_end - key_start) !=
            "scripts" ||
        position >= input.source.length || input.source[position] != '{')
      continue;

    position++;
    while (position < input.source.length) {
      position = skip_json_space(input.source, position);
      if (position >= input.source.length || input.source[position] == '}')
        return;
      if (input.source[position] != '"') {
        position++;
        continue;
      }
      let const script_key_end = json_string_end(input.source, position + 1);
      if (script_key_end >= input.source.length) return;
      position = skip_json_space(input.source, script_key_end + 1);
      if (position >= input.source.length || input.source[position] != ':')
        continue;
      position = skip_json_space(input.source, position + 1);
      if (position >= input.source.length || input.source[position] != '"')
        continue;
      let const value_start = position + 1;
      let const value_end = json_string_end(input.source, value_start);
      if (value_end >= input.source.length) return;
      parser_format_add_json_fragment(document, input.source, value_start,
                                      value_end, mimic_mood::Posix);
      position = value_end + 1;
    }
  }
}

} // namespace koshka
