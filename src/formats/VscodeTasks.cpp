/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements shell support for vscode tasks documents. It finds
 * embedded shell source, selects its dialect, and maps parsing, formatting,
 * and diagnostics back to the host file.
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
