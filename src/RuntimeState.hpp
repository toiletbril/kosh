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
  ExtendedArithmetic,
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

private:
  enum class Flag : u8
  {
    DiagnosticsDisabled = 1U << 0,
    AnnoyingDiagnosticsEnabled = 1U << 1,
    ErrorUnsetExplicit = 1U << 2,
    PipefailExplicit = 1U << 3,
    FailglobExplicit = 1U << 4,
    ExtendedArithmeticExplicit = 1U << 5,
  };

  u8 m_flags{static_cast<u8>(Flag::AnnoyingDiagnosticsEnabled)};

public:
  u64 shell_options{option_mask(shell_option_id::ExtendedArithmetic) |
                    option_mask(shell_option_id::Failglob) |
                    option_mask(shell_option_id::Hashall) |
                    option_mask(shell_option_id::Braceexpand)};

  pure fn is_diagnostics_disabled() const wontthrow -> bool;
  fn set_diagnostics_disabled(bool enabled) wontthrow -> void;
  pure fn is_annoying_diagnostics_enabled() const wontthrow -> bool;
  fn set_annoying_diagnostics_enabled(bool enabled) wontthrow -> void;
  pure fn was_error_unset_set_explicitly() const wontthrow -> bool;
  fn set_error_unset_set_explicitly(bool enabled) wontthrow -> void;
  pure fn was_pipefail_set_explicitly() const wontthrow -> bool;
  fn set_pipefail_set_explicitly(bool enabled) wontthrow -> void;
  pure fn was_failglob_set_explicitly() const wontthrow -> bool;
  fn set_failglob_set_explicitly(bool enabled) wontthrow -> void;
  pure fn was_extended_arithmetic_set_explicitly() const wontthrow -> bool;
  fn set_extended_arithmetic_set_explicitly(bool enabled) wontthrow -> void;

  pure static constexpr fn option_mask(shell_option_id option) wontthrow -> u64
  {
    return u64{1} << static_cast<u8>(option);
  }

  pure fn option_is_enabled(shell_option_id option) const wontthrow -> bool
  {
    return (shell_options & option_mask(option)) != 0;
  }

  pure fn koshkit_utilities_are_reachable() const wontthrow -> bool
  {
    return option_is_enabled(shell_option_id::Koshkit) ||
           mood == mimic_mood::Default;
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

private:
  pure fn has_flag(Flag flag) const wontthrow -> bool
  {
    return (m_flags & static_cast<u8>(flag)) != 0;
  }
  fn set_flag(Flag flag, bool enabled) wontthrow -> void
  {
    if (enabled)
      m_flags |= static_cast<u8>(flag);
    else
      m_flags &= static_cast<u8>(~static_cast<u8>(flag));
  }
};

static_assert(sizeof(RuntimeState) == 16);

inline pure fn RuntimeState::is_diagnostics_disabled() const wontthrow -> bool
{
  return has_flag(Flag::DiagnosticsDisabled);
}

inline fn RuntimeState::set_diagnostics_disabled(bool enabled) wontthrow -> void
{
  set_flag(Flag::DiagnosticsDisabled, enabled);
}

inline pure fn RuntimeState::is_annoying_diagnostics_enabled() const wontthrow
    -> bool
{
  return has_flag(Flag::AnnoyingDiagnosticsEnabled);
}

inline fn RuntimeState::set_annoying_diagnostics_enabled(bool enabled) wontthrow
    -> void
{
  set_flag(Flag::AnnoyingDiagnosticsEnabled, enabled);
}

inline pure fn RuntimeState::was_error_unset_set_explicitly() const wontthrow
    -> bool
{
  return has_flag(Flag::ErrorUnsetExplicit);
}

inline fn RuntimeState::set_error_unset_set_explicitly(bool enabled) wontthrow
    -> void
{
  set_flag(Flag::ErrorUnsetExplicit, enabled);
}

inline pure fn RuntimeState::was_pipefail_set_explicitly() const wontthrow
    -> bool
{
  return has_flag(Flag::PipefailExplicit);
}

inline fn RuntimeState::set_pipefail_set_explicitly(bool enabled) wontthrow
    -> void
{
  set_flag(Flag::PipefailExplicit, enabled);
}

inline pure fn RuntimeState::was_failglob_set_explicitly() const wontthrow
    -> bool
{
  return has_flag(Flag::FailglobExplicit);
}

inline fn RuntimeState::set_failglob_set_explicitly(bool enabled) wontthrow
    -> void
{
  set_flag(Flag::FailglobExplicit, enabled);
}

inline pure fn
RuntimeState::was_extended_arithmetic_set_explicitly() const wontthrow -> bool
{
  return has_flag(Flag::ExtendedArithmeticExplicit);
}

inline fn
RuntimeState::set_extended_arithmetic_set_explicitly(bool enabled) wontthrow
    -> void
{
  set_flag(Flag::ExtendedArithmeticExplicit, enabled);
}

} /* namespace koshka */
