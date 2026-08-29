#include "../ParserFormats.hpp"

namespace koshka {

fn parse_ansible_format(const parser_format_input &input,
                        parsed_format_document &document) throws -> void
{
  static const StringView KEYS[] = {"shell", "ansible.builtin.shell",
                                    "ansible.legacy.shell", "raw"};
  parser_format_extract_yaml_keys(document, input.source, KEYS, 4,
                                  mimic_mood::Posix);
}

} // namespace koshka
