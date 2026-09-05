/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements shell support for github actions documents. It finds
 * embedded shell source, selects its dialect, and maps parsing, formatting,
 * and diagnostics back to the host file.
 */

#include "../ParserFormats.hpp"

namespace koshka {

fn parse_github_actions_format(const parser_format_input &input,
                               parsed_format_document &document) throws -> void
{
  static const StringView KEYS[] = {"run"};
  parser_format_extract_yaml_keys(document, input.source, KEYS, 1,
                                  mimic_mood::Bash, true);
}

} // namespace koshka
