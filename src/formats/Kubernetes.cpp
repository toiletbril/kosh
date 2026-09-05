/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements shell support for kubernetes documents. It finds
 * embedded shell source, selects its dialect, and maps parsing, formatting,
 * and diagnostics back to the host file.
 */

#include "../ParserFormats.hpp"

namespace koshka {

fn parse_kubernetes_format(const parser_format_input &input,
                           parsed_format_document &document) throws -> void
{
  if (!parser_format_has_substring(input.source, "-c") &&
      !parser_format_has_substring(input.source, "- c"))
  {
    return;
  }

  bool has_explicit_shell = false;
  mimic_mood mood = mimic_mood::Posix;
  if (parser_format_has_substring(input.source, "/bin/bash") ||
      parser_format_has_substring(input.source, "- bash"))
  {
    has_explicit_shell = true;
    mood = mimic_mood::Bash;
  } else if (parser_format_has_substring(input.source, "/bin/sh") ||
             parser_format_has_substring(input.source, "- sh"))
  {
    has_explicit_shell = true;
  } else if (parser_format_has_substring(input.source, "kosh")) {
    has_explicit_shell = true;
    mood = mimic_mood::Default;
  }
  if (!has_explicit_shell) return;

  static const StringView KEYS[] = {"args"};
  parser_format_extract_yaml_keys(document, input.source, KEYS, 1, mood);
}

} // namespace koshka
