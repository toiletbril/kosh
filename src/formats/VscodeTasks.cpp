#include "../ParserFormats.hpp"

namespace koshka {

fn parse_vscode_tasks_format(const parser_format_input &input,
                             parsed_format_document &document) throws -> void
{
  static const StringView KEYS[] = {"command"};
  parser_format_extract_json_keys(document, input.source, KEYS, 1,
                                  mimic_mood::Posix);
}

} // namespace koshka
