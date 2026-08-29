#include "../ParserFormats.hpp"

namespace koshka {

fn parse_compose_format(const parser_format_input &input,
                        parsed_format_document &document) throws -> void
{
  static const StringView KEYS[] = {"command", "entrypoint", "test",
                                    "dockerfile_inline"};
  parser_format_extract_yaml_keys(document, input.source, KEYS, 4,
                                  mimic_mood::Posix);
}

} // namespace koshka
