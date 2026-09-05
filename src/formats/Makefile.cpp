/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file converts make recipe bodies and shell-function arguments into
 * mapped shell fragments using the bundled make parser. SHELL assignments
 * select Bash, POSIX, or Koshka parsing, and fragment metadata preserves
 * make-specific selection boundaries.
 */

#include "../Koshkit.hpp"
#include "../ParserFormats.hpp"

namespace koshka {

fn parse_makefile_format(const parser_format_input &input,
                         parsed_format_document &document) throws -> void
{
  mimic_mood mood = mimic_mood::Posix;
  usize position = 0;

  while (position < input.source.length) {
    let const line = input.source.next_line(position);
    if (parser_format_has_substring(line, "SHELL") &&
        parser_format_has_substring(line, "="))
    {
      if (parser_format_has_substring(line, "bash"))
        mood = mimic_mood::Bash;
      else if (parser_format_has_substring(line, "kosh"))
        mood = mimic_mood::Default;
      else if (parser_format_has_substring(line, "sh"))
        mood = mimic_mood::Posix;
    }
  }

  let const ranges =
      koshkit::parse_makefile_shell_sources(input.source, heap_allocator());

  for (let const &range : ranges) {
    let analysis_source =
        koshkit::makefile_shell_analysis_source(input.source, range);
    let const fragment_count = document.fragments.count();
    parser_format_add_fragment(
        document, input.source, range.start_position, range.end_position, mood,
        parser_format_codec::Direct, 0, steal(analysis_source));
    if (document.fragments.count() != fragment_count) {
      document.fragments.back().shell_source =
          String{input.source.substring_of_length(
              range.start_position, range.end_position - range.start_position)};
      document.fragments.back().should_select_end =
          range.kind == koshkit::make_shell_source_kind::ShellFunction;
    }
  }
}

} // namespace koshka
