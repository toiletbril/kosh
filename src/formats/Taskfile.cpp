/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements shell support for taskfile documents. It finds
 * embedded shell source, selects its dialect, and maps parsing, formatting,
 * and diagnostics back to the host file.
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
