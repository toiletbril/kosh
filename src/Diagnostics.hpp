#pragma once

#include "Containers.hpp"
#include "String.hpp"
#include "StringView.hpp"

namespace koshka {

enum class diagnostic_tier : u8
{
  Strict,
  Lenient,
  Annoying,
};

enum class diagnostic_delivery : u8
{
  Policy,
  Warning,
};

enum class diagnostic_id : u16
{
  sc1014,
  sc1035,
  sc1037,
  sc2002,
  sc2003,
  sc2004,
  sc2005,
  sc2006,
  sc2007,
  sc2009,
  sc2010,
  sc2013,
  sc2015,
  sc2016,
  sc2021,
  sc2024_glob,
  sc2024_redirection,
  sc2025,
  sc2030_assignment,
  sc2030_read,
  sc2031,
  sc2035,
  sc2038,
  sc2044,
  sc2045,
  sc2046,
  sc2048,
  sc2050,
  sc2051,
  sc2059,
  sc2060,
  sc2061,
  sc2062,
  sc2063,
  sc2064,
  sc2066,
  sc2067,
  sc2068,
  sc2069,
  sc2071,
  sc2074,
  sc2076,
  sc2077,
  sc2081,
  sc2086_expansion,
  sc2086_test,
  sc2088,
  sc2091,
  sc2093,
  sc2094,
  sc2095,
  sc2114,
  sc2115,
  sc2116,
  sc2124,
  sc2126,
  sc2129,
  sc2142,
  sc2144,
  sc2145,
  sc2146,
  sc2147,
  sc2155,
  sc2156,
  sc2157_string,
  sc2157,
  sc2162,
  sc2164,
  sc2165,
  sc2166,
  sc2167,
  sc2168,
  sc2170,
  sc2174,
  sc2181,
  sc2183,
  sc2184,
  sc2196,
  sc2197,
  sc2204,
  sc2207,
  sc2215,
  sc2216,
  sc2217,
  sc2221,
  sc2222,
  sc2229,
  sc2236,
  sc2237,
  sc2242,
  sc2244,
  sc2249,
  sc2257,
  sc2264,
  sc2268,
  sc2281,
  sc2283,
  sc2335,
  sc3014,
  sc3030,
  sc3037,
  sc3043,
  sc3044,
  sc3045,
  sc3046,
  arith_assign,
  assignment_prefix_read,
  byte_order_mark,
  exported_cdpath,
  external_arithmetic_input,
  external_array_subscript,
  malformed_glob,
  no_local,
  typeset_spelling,
  unresolved_command,
  unresolved_command_uncertain,
  use_before_assign,
  Count,
};

struct diagnostic_definition
{
  const char *slug;
  const char *summary;
  const char *message_template;
  Maybe<const char *> suggestion_template;
  Maybe<const char *> related_template;
  u16 shellcheck_code;
  diagnostic_tier tier;
  diagnostic_delivery delivery;
};

struct shellcheck_directive_span
{
  usize position;
  usize length;
};

struct shellcheck_suppression
{
  usize start_position;
  usize end_position;
  ArrayList<shellcheck_directive_span> directives;
};

extern const diagnostic_definition DIAGNOSTIC_DEFINITIONS[];

pure fn get_diagnostic_definition(diagnostic_id id) wontthrow
    -> const diagnostic_definition &;
pure fn get_diagnostic_count() wontthrow -> usize;
pure inline fn get_diagnostic_tier_name(diagnostic_tier tier) wontthrow
    -> StringView
{
  switch (tier) {
  case diagnostic_tier::Strict: return "strict";
  case diagnostic_tier::Lenient: return "lenient";
  case diagnostic_tier::Annoying: return "annoying";
  }
  ASSERT(false);
  return {};
}
fn format_diagnostic_template(
    const char *text_template,
    std::initializer_list<StringView> arguments = {}) throws -> String;
pure fn diagnostic_directive_disables(StringView comment,
                                      diagnostic_id id) wontthrow -> bool;

} /* namespace koshka */
