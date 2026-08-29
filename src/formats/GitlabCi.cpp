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
