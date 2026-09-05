/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file extracts lifecycle command strings from Dev Container JSON as POSIX
 * shell fragments. The adapter covers initialize, creation, content update,
 * start, and attach commands and preserves JSON escape and host-position
 * mapping.
 */

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
