#include "../ParserFormats.hpp"

namespace koshka {

fn parse_dev_container_format(const parser_format_input &input,
                              parsed_format_document &document) throws -> void
{
  static const StringView KEYS[] = {"initializeCommand",
                                    "onCreateCommand",
                                    "updateContentCommand",
                                    "postCreateCommand",
                                    "postStartCommand",
                                    "postAttachCommand",
                                    "waitFor"};
  parser_format_extract_json_keys(document, input.source, KEYS, 7,
                                  mimic_mood::Posix);
}

} // namespace koshka
