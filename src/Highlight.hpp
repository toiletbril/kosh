/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file defines semantic highlight roles, spans, and output records
 * shared by completion, diagnostics, and interactive syntax coloring.
 */

#pragma once

#include "Common.hpp"
#include "StringView.hpp"

namespace koshka {

enum class highlight_role : u8
{
  comment,
  operator_,
  string,
  heredoc,
  heredoc_delimiter,
  variable,
  assignment_name,
  unset_variable,
  flag,
  keyword,
  invalid_syntax,
  function_name,
  resolved_command,
  partial_command,
  unknown_command,
  existing_path,
  partial_path,
  invalid_path,
  url,
  glob,
  count,
};

struct highlight_span
{
  usize start;
  usize end;
  highlight_role role;
};

struct highlight_theme
{
  StringView reset;
  StringView styles[static_cast<usize>(highlight_role::count)];

  pure fn style_for(highlight_role role) const wontthrow -> StringView
  {
    return styles[static_cast<usize>(role)];
  }

  constexpr fn set_style(highlight_role role, StringView style) wontthrow
      -> void
  {
    styles[static_cast<usize>(role)] = style;
  }
};

pure fn highlight_role_name(highlight_role role) wontthrow -> StringView;

} /* namespace koshka */
