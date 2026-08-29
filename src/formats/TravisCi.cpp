#include "../ParserFormats.hpp"

namespace koshka {

fn parse_travis_ci_format(const parser_format_input &input,
                          parsed_format_document &document) throws -> void
{
  static const StringView KEYS[] = {
      "before_install", "install",       "before_script", "script",
      "after_success",  "after_failure", "before_deploy", "deploy",
      "after_deploy",   "after_script"};
  parser_format_extract_yaml_keys(document, input.source, KEYS, 10,
                                  mimic_mood::Bash);
}

} // namespace koshka
