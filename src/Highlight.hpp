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
