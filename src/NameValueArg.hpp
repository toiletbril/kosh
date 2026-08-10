#pragma once

#include "StringView.hpp"

namespace koshka {

/* An argument split at its first '=', for the assignment builtins. The value is
   absent when no '=' is present, so a bare name reads differently from name=.
   Construct through from, which performs the split. */
class NameValueArg
{
public:
  static fn from(StringView arg) wontthrow -> NameValueArg
  {
    let const equals = arg.find_character('=');
    if (!equals.has_value()) return NameValueArg{arg, None};
    return NameValueArg{arg.substring_of_length(0, *equals),
                        arg.substring(*equals + 1)};
  }

  mustuse pure fn get_name() const wontthrow -> StringView { return m_name; }

  mustuse pure fn get_value() const wontthrow -> const Maybe<StringView> &
  {
    return m_value;
  }

private:
  NameValueArg(StringView name, Maybe<StringView> value) wontthrow
      : m_name(name),
        m_value(steal(value))
  {}

  StringView m_name;
  Maybe<StringView> m_value;
};

} // namespace koshka
