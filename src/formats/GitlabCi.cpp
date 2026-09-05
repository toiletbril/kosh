/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file identifies before_script, script, after_script, and
 * pre_get_sources_script values in GitLab CI YAML as Bash shell fragments. The
 * shared YAML extractor preserves their host positions and honors nearby shell
 * selectors.
 */

#include "../ParserFormats.hpp"

namespace koshka {

fn parse_gitlab_ci_format(const parser_format_input &input,
                          parsed_format_document &document) throws -> void
{
  static const StringView KEYS[] = {"before_script", "script", "after_script",
                                    "pre_get_sources_script"};
  parser_format_extract_yaml_keys(document, input.source, KEYS, 4,
                                  mimic_mood::Bash);
}

} // namespace koshka
