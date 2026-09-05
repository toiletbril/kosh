/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file identifies cmd, cmds, defer, sh, status, and if values in Taskfile
 * YAML as Bash shell fragments. The shared YAML extractor handles inline,
 * block, and sequence values and preserves their host positions.
 */

#include "../ParserFormats.hpp"

namespace koshka {

fn parse_taskfile_format(const parser_format_input &input,
                         parsed_format_document &document) throws -> void
{
  static const StringView KEYS[] = {"cmd", "cmds",   "defer",
                                    "sh",  "status", "if"};
  parser_format_extract_yaml_keys(document, input.source, KEYS, 6,
                                  mimic_mood::Bash);
}

} // namespace koshka
