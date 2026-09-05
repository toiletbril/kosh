/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements shell dialect moods. It parses mood names and selects
 * syntax, analysis, and compatibility behavior for Koshka, Bash, POSIX sh,
 * and other shells.
 */

#pragma once

#include "Common.hpp"
#include "Maybe.hpp"
#include "StaticStringMap.hpp"
#include "StringView.hpp"

namespace koshka {

/* The mode a mimicked script runs in, chosen from its shebang. A sh or dash
   shebang gives Posix, a bash shebang gives Bash, and a kosh shebang gives
   Default. BashPosix is the bash mood reached through --posix or set -o posix,
   so a terminal that re-execs with --posix to inject its integration runs as
   bash with the bash identity and rc rather than the dash-like sh mood. */
enum class mimic_mood : u8
{
  Default,
  Posix,
  Bash,
  BashPosix,
};

fn detect_mimic_shell_from_source(StringView source) throws
    -> Maybe<mimic_mood>;

/* The extension carries a dot, the way Path::extension reports it. */
pure fn detect_mimic_shell_from_extension(StringView extension) throws
    -> Maybe<mimic_mood>;

inline pure fn parse_mood_name(StringView name) throws -> Maybe<mimic_mood>
{
  static constexpr static_string_entry<mimic_mood> MOOD_ENTRIES[] = {
      {SSK("kosh"),       mimic_mood::Default  },
      {SSK("default"),    mimic_mood::Default  },
      {SSK("bash"),       mimic_mood::Bash     },
      {SSK("sh"),         mimic_mood::Posix    },
      {SSK("posix"),      mimic_mood::Posix    },
      {SSK("dash"),       mimic_mood::Posix    },
      {SSK("bash-posix"), mimic_mood::BashPosix},
  };
  static constexpr StaticStringMap MOODS{MOOD_ENTRIES};
  return MOODS.find(name);
}

inline pure fn mood_name(mimic_mood mood) wontthrow -> StringView
{
  switch (mood) {
  case mimic_mood::Bash: return "bash";
  case mimic_mood::Posix: return "sh";
  case mimic_mood::BashPosix: return "bash-posix";
  case mimic_mood::Default: return "kosh";
  }
  return "kosh";
}

} /* namespace koshka */
