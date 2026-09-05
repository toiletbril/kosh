/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements position lists and tab stops shared by cut, expand,
 * and unexpand. It parses and merges one-based ranges, tests selected
 * positions, validates ordered stops, and computes the next tab column.
 */

#include "TextProcessing.hpp"

namespace koshka::koshkit {

static fn parse_positive_position(StringView text, usize &position) wontthrow
    -> Maybe<usize>
{
  if (position == text.length || text[position] < '0' || text[position] > '9')
    return None;

  usize value = 0;
  while (position < text.length && text[position] >= '0' &&
         text[position] <= '9')
  {
    let const digit = static_cast<usize>(text[position] - '0');
    if (value > (SIZE_MAX - digit) / 10) return None;
    value = value * 10 + digit;
    position++;
  }
  if (value == 0) return None;

  return value;
}

fn parse_text_position_ranges(StringView text, Allocator allocator) throws
    -> Maybe<ArrayList<text_position_range>>
{
  if (text.is_empty()) return None;

  let ranges = ArrayList<text_position_range>{allocator};
  usize position = 0;

  while (position < text.length &&
         (text[position] == ' ' || text[position] == '\t'))
    position++;
  if (position == text.length) return None;

  while (position < text.length) {
    usize first = 1;
    usize last = SIZE_MAX;
    if (text[position] == '-') {
      position++;
      let const parsed_last = parse_positive_position(text, position);
      if (!parsed_last.has_value()) return None;
      last = *parsed_last;
    } else {
      let const parsed_first = parse_positive_position(text, position);
      if (!parsed_first.has_value()) return None;
      first = *parsed_first;
      last = first;
      if (position < text.length && text[position] == '-') {
        position++;
        if (position == text.length || text[position] == ',') {
          last = SIZE_MAX;
        } else {
          let const parsed_last = parse_positive_position(text, position);
          if (!parsed_last.has_value() || *parsed_last < first) return None;
          last = *parsed_last;
        }
      }
    }

    ranges.push({first, last});
    if (position == text.length) break;
    if (text[position] == ',') {
      position++;
    } else if (text[position] != ' ' && text[position] != '\t') {
      return None;
    }
    while (position < text.length &&
           (text[position] == ' ' || text[position] == '\t'))
      position++;
    if (position == text.length) break;
    if (text[position] == ',') return None;
  }

  ranges.sort();
  usize output_count = 0;
  for (let const range : ranges) {
    if (output_count != 0 && (ranges[output_count - 1].last == SIZE_MAX ||
                              range.first <= ranges[output_count - 1].last + 1))
    {
      if (range.last > ranges[output_count - 1].last)
        ranges[output_count - 1].last = range.last;
      continue;
    }

    ranges[output_count++] = range;
  }
  while (ranges.count() > output_count)
    ranges.pop_back();

  return ranges;
}

pure fn text_position_is_selected(
    usize one_based_position,
    const ArrayList<text_position_range> &ranges) wontthrow -> bool
{
  for (let const range : ranges) {
    if (one_based_position < range.first) return false;
    if (one_based_position <= range.last) return true;
  }

  return false;
}

fn parse_tab_stop_list(StringView text, Allocator allocator) throws
    -> Maybe<ArrayList<usize>>
{
  let stops = ArrayList<usize>{allocator};
  usize position = 0;

  while (position < text.length) {
    while (position < text.length &&
           (text[position] == ',' || text[position] == ' ' ||
            text[position] == '\t'))
      position++;
    if (position == text.length) break;

    let const stop = parse_positive_position(text, position);
    if (!stop.has_value()) return None;
    if (!stops.is_empty() && *stop <= stops.back()) return None;
    stops.push(*stop);

    if (position < text.length && text[position] != ',' &&
        text[position] != ' ' && text[position] != '\t')
      return None;
  }

  if (stops.is_empty()) return None;
  return stops;
}

pure fn next_tab_column(usize column,
                        const ArrayList<usize> &tab_stops) wontthrow -> usize
{
  if (tab_stops.is_empty()) return column + (8 - column % 8);

  for (let const stop : tab_stops)
    if (stop > column) return stop;

  if (tab_stops.count() == 1) {
    let const width = tab_stops[0];
    return column + (width - column % width);
  }

  return column;
}

} // namespace koshka::koshkit
