#include "../ParserFormats.hpp"

namespace koshka {

fn parse_github_actions_format(const parser_format_input &input,
                               parsed_format_document &document) throws -> void
{
  static const StringView KEYS[] = {"run"};
  parser_format_extract_yaml_keys(document, input.source, KEYS, 1,
                                  mimic_mood::Bash);
}

} // namespace koshka
