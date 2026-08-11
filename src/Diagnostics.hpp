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
  sc1019,
  sc1026,
  sc1029,
  sc1035,
  sc1037,
  sc1106,
  sc2000,
  sc2001,
  sc2002,
  sc2003,
  sc2004,
  sc2005,
  sc2006,
  sc2007,
  sc2008,
  sc2009,
  sc2010,
  sc2011,
  sc2012,
  sc2013,
  sc2014,
  sc2015,
  sc2016,
  sc2018,
  sc2019,
  sc2020,
  sc2021,
  sc2022,
  sc2024_glob,
  sc2024_redirection,
  sc2025,
  sc2028,
  sc2029,
  sc2030_assignment,
  sc2030_read,
  sc2031,
  sc2032,
  sc2033,
  sc2035,
  sc2036,
  sc2037,
  sc2038,
  sc2044,
  sc2045,
  sc2046,
  sc2048,
  sc2049,
  sc2050,
  sc2051,
  sc2053,
  sc2055,
  sc2056,
  sc2057,
  sc2058,
  sc2059,
  sc2060,
  sc2061,
  sc2062,
  sc2063,
  sc2064,
  sc2065,
  sc2066,
  sc2067,
  sc2068,
  sc2069,
  sc2070,
  sc2071,
  sc2072,
  sc2073,
  sc2074,
  sc2076,
  sc2077,
  sc2078,
  sc2081,
  sc2086_expansion,
  sc2086_test,
  sc2088,
  sc2091,
  sc2093,
  sc2094,
  sc2095,
  sc2099,
  sc2100,
  sc2103,
  sc2104,
  sc2105,
  sc2107,
  sc2108,
  sc2109,
  sc2110,
  sc2114,
  sc2115,
  sc2116,
  sc2117,
  sc2121,
  sc2122,
  sc2123,
  sc2124,
  sc2125,
  sc2126,
  sc2129,
  sc2130,
  sc2142,
  sc2143,
  sc2144,
  sc2145,
  sc2146,
  sc2147,
  sc2151,
  sc2152,
  sc2155,
  sc2156,
  sc2157_string,
  sc2157,
  sc2158,
  sc2159,
  sc2160,
  sc2161,
  sc2162,
  sc2163,
  sc2164,
  sc2165,
  sc2166,
  sc2167,
  sc2168,
  sc2170,
  sc2171,
  sc2172,
  sc2173,
  sc2174,
  sc2176,
  sc2177,
  sc2181,
  sc2182,
  sc2183,
  sc2184,
  sc2185,
  sc2193,
  sc2196,
  sc2197,
  sc2198,
  sc2199,
  sc2200,
  sc2201,
  sc2202,
  sc2203,
  sc2204,
  sc2205,
  sc2207,
  sc2208,
  sc2210,
  sc2212,
  sc2215,
  sc2216,
  sc2217,
  sc2219,
  sc2221,
  sc2222,
  sc2224,
  sc2225,
  sc2226,
  sc2229,
  sc2230,
  sc2232,
  sc2233,
  sc2234,
  sc2236,
  sc2237,
  sc2240,
  sc2241,
  sc2242,
  sc2243,
  sc2244,
  sc2245,
  sc2249,
  sc2252,
  sc2255,
  sc2257,
  sc2264,
  sc2267,
  sc2268,
  sc2281,
  sc2283,
  sc2284,
  sc2335,
  sc3001,
  sc3002,
  sc3003,
  sc3006,
  sc3012,
  sc3013,
  sc3014,
  sc3017,
  sc3018,
  sc3019,
  sc3020,
  sc3021,
  sc3022,
  sc3023,
  sc3024,
  sc3025,
  sc3026,
  sc3028,
  sc3030,
  sc3031,
  sc3034,
  sc3035,
  sc3037,
  sc3038,
  sc3039,
  sc3043,
  sc3044,
  sc3045,
  sc3046,
  sc3047,
  sc3048,
  sc3049,
  sc3050,
  sc3053,
  sc3054,
  sc3055,
  sc3056,
  sc3057,
  sc3060,
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

enum class shellcheck_selector_kind
{
  All,       /* every catalog entry is disabled */
  Slug,      /* one native or numbered variant is named */
  Code,      /* every variant under one ShellCheck code is named */
  CodeRange, /* every code in [code_start, code_end) is named */
};

/* The slug is a span because the parser's source copy is released before the
   analysis stage reads the suppressions. */
struct shellcheck_selector
{
  shellcheck_selector_kind kind{shellcheck_selector_kind::All};
  shellcheck_directive_span slug{0, 0};
  u16 code_start{0};
  u16 code_end{0};
};

struct shellcheck_suppression
{
  usize start_position;
  usize end_position;
  ArrayList<shellcheck_selector> selectors;
};

/* The name is owned because the parser releases its source copy before
   analysis runs. */
struct analysis_scope_definition
{
  String name;
  bool is_alias;
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
fn collect_shellcheck_selectors(
    StringView source, shellcheck_directive_span comment_span,
    ArrayList<shellcheck_selector> &selectors) throws -> void;
pure fn shellcheck_selector_disables(const shellcheck_selector &selector,
                                     StringView source,
                                     diagnostic_id id) wontthrow -> bool;

enum class command_name_id : u16
{
  Unknown,
  Alias,
  Arch,
  Basename,
  Break,
  Builtin,
  Cd,
  Chmod,
  Chown,
  Colon,
  Command,
  Continue,
  Cp,
  Date,
  Declare,
  Dirname,
  Dot,
  DoubleBracket,
  Echo,
  Egrep,
  Eval,
  Exec,
  Exit,
  Export,
  Expr,
  False,
  Fgrep,
  Find,
  Getopts,
  Grep,
  Hostname,
  Id,
  Kill,
  Let,
  Ln,
  Local,
  Ls,
  Mapfile,
  Mkdir,
  Mv,
  Printf,
  Ps,
  Pwd,
  Read,
  Readarray,
  Readonly,
  Return,
  Rm,
  Rmdir,
  Sed,
  Seq,
  Set,
  SingleBracket,
  Sleep,
  Source,
  Ssh,
  Su,
  Sudo,
  Test,
  Touch,
  Tr,
  Trap,
  True,
  Tty,
  Typeset,
  Uname,
  Unlink,
  Unset,
  Wc,
  Which,
  Whoami,
  Xargs,
};

constexpr u32 COMMAND_GROUP_TEST = 1u << 0;
constexpr u32 COMMAND_GROUP_DECLARATION_BUILTIN = 1u << 1;
constexpr u32 COMMAND_GROUP_ASSIGNMENT_BUILTIN = 1u << 2;
constexpr u32 COMMAND_GROUP_RUNTIME_DEFINER = 1u << 3;
constexpr u32 COMMAND_GROUP_VARIABLE_PROBE = 1u << 4;
constexpr u32 COMMAND_GROUP_VARIABLE_TARGET = 1u << 5;
constexpr u32 COMMAND_GROUP_NON_STDIN_READER = 1u << 6;
constexpr u32 COMMAND_GROUP_ENVIRONMENT_NEUTRAL = 1u << 7;
constexpr u32 COMMAND_GROUP_PATTERN_MATCHER = 1u << 8;

struct analysis_command_info
{
  command_name_id id;
  u32 group_flags;

  mustuse pure fn is_in_group(u32 group) const wontthrow -> bool
  {
    return (group_flags & group) != 0;
  }
};

fn get_analysis_command_info(StringView name) throws -> analysis_command_info;

} /* namespace koshka */
