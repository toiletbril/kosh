/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file extracts command strings from VS Code task JSON as POSIX shell
 * fragments. JSON decoding and encoding preserve escape sequences and host
 * positions during analysis and formatting.
 */

#include "../ParserFormats.hpp"

namespace koshka {

fn parse_vscode_tasks_format(const parser_format_input &input,
                             parsed_format_document &document) throws -> void
{
  static const parser_format_json_key KEYS[] = {
      {"command", false}
  };
  parser_format_extract_json_keys(document, input.source, KEYS, 1,
                                  mimic_mood::Posix);
}

} // namespace koshka
