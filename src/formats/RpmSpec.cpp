/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file extracts POSIX shell bodies from RPM preparation, build,
 * installation, check, transaction, and trigger sections. Each fragment ends
 * at the next unescaped section directive.
 */

#include "../ParserFormats.hpp"

namespace koshka {

static pure fn is_script_section(StringView line) wontthrow -> bool
{
  static const StringView SECTIONS[] = {
      "%prep",      "%build",     "%install",   "%check",        "%clean",
      "%pre",       "%post",      "%preun",     "%postun",       "%pretrans",
      "%posttrans", "%triggerin", "%triggerun", "%triggerpostun"};
  for (let const section : SECTIONS)
    if (line.starts_with(section)) return true;

  return false;
}

fn parse_rpm_spec_format(const parser_format_input &input,
                         parsed_format_document &document) throws -> void
{
  usize position = 0;
  while (position < input.source.length) {
    let const line = input.source.next_line(position);
    if (!is_script_section(line)) continue;
    let const shell_start = position;
    usize shell_end = input.source.length;
    while (position < input.source.length) {
      let const next_start = position;
      let const next_line = input.source.next_line(position);
      if (!next_line.is_empty() && next_line[0] == '%' &&
          !next_line.starts_with("%%"))
      {
        shell_end = next_start;
        break;
      }
    }
    parser_format_add_fragment(document, input.source, shell_start, shell_end,
                               mimic_mood::Posix);
  }
}

} // namespace koshka
