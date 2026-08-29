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
