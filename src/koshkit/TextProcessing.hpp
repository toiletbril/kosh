/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file declares position-range and tab-stop operations shared by cut,
 * expand, and unexpand. The interface gives those utilities one definition of
 * range selection and tab advancement.
 */

#pragma once

#include "../Allocator.hpp"
#include "../ArrayList.hpp"
#include "../Maybe.hpp"
#include "../StringView.hpp"

namespace koshka::koshkit {

struct text_position_range
{
  usize first;
  usize last;

  pure fn operator<(const text_position_range &other) const wontthrow->bool
  {
    if (first != other.first) return first < other.first;
    return last < other.last;
  }
};

fn parse_text_position_ranges(StringView text, Allocator allocator) throws
    -> Maybe<ArrayList<text_position_range>>;
pure fn text_position_is_selected(
    usize one_based_position,
    const ArrayList<text_position_range> &ranges) wontthrow -> bool;

fn parse_tab_stop_list(StringView text, Allocator allocator) throws
    -> Maybe<ArrayList<usize>>;
pure fn next_tab_column(usize column,
                        const ArrayList<usize> &tab_stops) wontthrow -> usize;

} // namespace koshka::koshkit
