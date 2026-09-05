/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements shell support for bitbucket pipelines documents. It
 * finds embedded shell source, selects its dialect, and maps parsing,
 * formatting, and diagnostics back to the host file.
 */

#include "../ParserFormats.hpp"

namespace koshka {

fn parse_bitbucket_pipelines_format(const parser_format_input &input,
                                    parsed_format_document &document) throws
    -> void
{
  static const StringView KEYS[] = {"script", "after-script"};
  parser_format_extract_yaml_keys(document, input.source, KEYS, 2,
                                  mimic_mood::Posix);
}

} // namespace koshka
