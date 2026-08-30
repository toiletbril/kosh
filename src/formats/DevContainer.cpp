#include "../ParserFormats.hpp"

namespace koshka {

fn parse_dev_container_format(const parser_format_input &input,
                              parsed_format_document &document) throws -> void
{
  static const parser_format_json_key KEYS[] = {
      {"initializeCommand",    false},
      {"onCreateCommand",      true },
      {"updateContentCommand", true },
      {"postCreateCommand",    true },
      {"postStartCommand",     true },
      {"postAttachCommand",    true }
  };
  parser_format_extract_json_keys(document, input.source, KEYS, 6,
                                  mimic_mood::Posix);
}

} // namespace koshka
