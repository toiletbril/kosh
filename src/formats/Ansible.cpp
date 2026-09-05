/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file identifies shell, ansible.builtin.shell,
 * ansible.legacy.shell, and raw values in Ansible YAML as POSIX shell
 * fragments. The shared YAML extractor preserves their playbook positions for
 * diagnostics and formatting.
 */

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
