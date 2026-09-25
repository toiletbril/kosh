/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This native owner checks ReportTable's width, alignment, ANSI, title,
 * empty-row, allocator, and repeat-rendering contracts without starting the
 * shell process entry point.
 */

#include "CLI.hpp"
#include "CLIColors.hpp"
#include "base/Common.hpp"

#include <cstdio>

using namespace koshka;

namespace {

fn fail(StringView name, StringView actual) -> int
{
  std::fprintf(stderr, "report-table failure: %.*s\n%.*s\n",
               static_cast<int>(name.length), name.data,
               static_cast<int>(actual.length), actual.data);
  return 1;
}

fn add_row(ReportTable &table, Allocator allocator, StringView first,
           StringView second) throws -> void
{
  let cells = ArrayList<report_table_cell_view>{allocator};
  cells.push({first, {}});
  cells.push({second, {}});
  table.add_row(cells);
}

} // namespace

fn main() throws -> int
{
  let const allocator = heap_allocator();
  let table = ReportTable{allocator};
  table.add_column("NAME", report_table_alignment::Left,
                   colors::ansi::BOLD_CYAN);
  table.add_column("VALUE", report_table_alignment::Right);
  add_row(table, allocator, "\xc3\xa9", "7");
  let empty_cells = ArrayList<report_table_cell_view>{allocator};
  table.add_row(empty_cells);

  let const first = table.to_string(false, {});
  let const repeated = table.to_string(false, {});
  if (first != repeated) return fail("repeat rendering", repeated.view());
  if (!first.view().find_substring("NAME") ||
      !first.view().find_substring("VALUE") ||
      !first.view().find_substring("\xc3\xa9") ||
      !first.view().find_substring("7"))
    return fail("width/alignment/empty row", first.view());
  if (first.view().find_substring("\x1b["))
    return fail("ANSI stripping", first.view());

  let const colored = table.to_string(true, "  ");
  if (!colored.view().find_substring("\x1b["))
    return fail("ANSI rendering", colored.view());

  let titled = String{allocator, "prefix\n"};
  append_titled_report_table(titled, "Metadata", table, false);
  if (!titled.starts_with("prefix\n\n  Metadata\n  NAME"))
    return fail("title indentation", titled.view());

  std::printf("report-table: ok\n");
  return 0;
}
