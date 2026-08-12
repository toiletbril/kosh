#pragma once

#include "Common.hpp"
#include "MimicMood.hpp"

namespace koshka {

class EvalContext;

enum class shell_option_id : u8
{
  Errexit,
  Xtrace,
  Nounset,
  Pipefail,
  Allexport,
  Noclobber,
  Noglob,
  Noexec,
  Koshkit,
  Monitor,
  Failglob,
  Notify,
  Vi,
  Emacs,
  Hashall,
  Verbose,
  Keyword,
  Errtrace,
  Functrace,
  Braceexpand,
  Physical,
  Mimicry,
  Privileged,
  Restricted,
  ShowAst,
  ShowLexedWords,
  ShowExitCode,
  ShowStats,
  ShowMemory,
  Count,
};

/* A snapshot of the mood and the diagnostic and strictness toggles, captured
   and restored as a unit so a scope that runs a body under a different mood
   saves and puts back the whole set with one call. */
class RuntimeState
{
public:
  mimic_mood mood{mimic_mood::Default};
  u8 warning_level{0};
  bool is_diagnostics_disabled{false};
  bool is_annoying_diagnostics_enabled{true};
  u64 shell_options{option_mask(shell_option_id::Failglob) |
                    option_mask(shell_option_id::Hashall) |
                    option_mask(shell_option_id::Braceexpand)};
  bool was_error_unset_set_explicitly{false};
  bool was_pipefail_set_explicitly{false};
  bool was_failglob_set_explicitly{false};

  pure static constexpr fn option_mask(shell_option_id option) wontthrow -> u64
  {
    return u64{1} << static_cast<u8>(option);
  }

  pure fn option_is_enabled(shell_option_id option) const wontthrow -> bool
  {
    return (shell_options & option_mask(option)) != 0;
  }

  fn set_option(shell_option_id option, bool enabled) wontthrow -> void
  {
    if (enabled)
      shell_options |= option_mask(option);
    else
      shell_options &= ~option_mask(option);
  }

  mustuse static fn capture(const EvalContext &context) wontthrow
      -> RuntimeState;
  fn restore(EvalContext &context) const wontthrow -> void;
};

} // namespace koshka
