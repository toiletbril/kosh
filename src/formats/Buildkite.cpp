/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file identifies command and commands values in Buildkite YAML as Bash
 * shell fragments. The shared YAML extractor handles inline, block, and
 * sequence values and preserves their host positions.
 */

#include "../ParserFormats.hpp"

namespace koshka {

fn parse_buildkite_format(const parser_format_input &input,
                          parsed_format_document &document) throws -> void
{
  static const StringView KEYS[] = {"command", "commands"};
  parser_format_extract_yaml_keys(document, input.source, KEYS, 2,
                                  mimic_mood::Bash);
}

} // namespace koshka
