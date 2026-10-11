/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file declares the evaluator interface, compact runtime option state,
 * resolved commands, command arguments, execution contexts, and EvalContext
 * storage. It also declares the compact frame storage projected as Bash call
 * stack arrays. Expressions, builtins, startup, completion, and subshell
 * transport share these declarations, so this header is their common runtime
 * boundary.
 */

#pragma once

#include "Builtin.hpp"
#include "Completion.hpp"
#include "Errors.hpp"
#include "MimicMood.hpp"
#include "Platform.hpp"
#include "base/Arena.hpp"
#include "base/Bitset.hpp"
#include "base/Common.hpp"
#include "base/Containers.hpp"
#include "base/ErrorOr.hpp"
#include "base/Maybe.hpp"
#include "base/Path.hpp"
#include "base/Trace.hpp"

namespace koshka {

class EvalContext;

class ResolvedCommand
{
public:
  enum class Kind : u8
  {
    Builtin,
    Program,
    Unresolved,
  };

  Path program_path{};
  i32 unresolved_status{127};
  Kind kind{Kind::Program};
  Builtin::Kind builtin_kind{};

  mustuse static ResolvedCommand from_builtin(Builtin::Kind chosen_builtin)
  {
    ResolvedCommand resolved{};
    resolved.kind = Kind::Builtin;
    resolved.builtin_kind = chosen_builtin;
    return resolved;
  }

  mustuse static ResolvedCommand from_program(Path path)
  {
    ResolvedCommand resolved{};
    resolved.kind = Kind::Program;
    resolved.program_path = steal(path);
    return resolved;
  }

  mustuse static ResolvedCommand from_unresolved(i32 resolution_status)
  {
    ResolvedCommand resolved{};
    resolved.kind = Kind::Unresolved;
    resolved.unresolved_status = resolution_status;
    return resolved;
  }

  mustuse bool is_builtin() const { return kind == Kind::Builtin; }
  mustuse bool is_unresolved() const { return kind == Kind::Unresolved; }
};

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
  History,
  Histexpand,
  Ignoreeof,
  Nolog,
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
  ShowAllExitCodes,
  ShowStats,
  ShowMemory,
  Onecmd,
  SpaceAfterCompletion,
  HistoryPrefixSearch,
  InteractiveHints,
  InteractiveDiagnostics,
  AutoPair,
  TransientPrompt,
  ExtendedKeys,
  SpaceAfterDirectoryCompletion,
  TabCompletion,
  SyntaxHighlighting,
  DevNullCompatibility,
  SlashTmpCompatibility,
  Count,
};

enum class shopt_option_id : u8;

enum class bash_special_array_id : u8
{
  Aliases,
  DirectoryStack,
  Count,
};

enum class dynamic_reader_id : u8
{
  Seconds,
  Random,
  Count,
};

inline constexpr StringView BASH_ALIASES_VARIABLE{"BASH_ALIASES"};
inline constexpr StringView BASH_ARGUMENT_COUNT_VARIABLE{"BASH_ARGC"};
inline constexpr StringView BASH_ARGUMENT_VALUE_VARIABLE{"BASH_ARGV"};
inline constexpr StringView DIRSTACK_VARIABLE{"DIRSTACK"};

constexpr pure fn bash_special_array_mask(bash_special_array_id id) wontthrow
    -> u8
{
  return static_cast<u8>(1U << static_cast<u8>(id));
}

constexpr pure fn dynamic_reader_mask(dynamic_reader_id id) wontthrow -> u8
{
  return static_cast<u8>(1U << static_cast<u8>(id));
}

struct reporting_state
{
  u8 warning_level{0};
  bool is_annoying_disabled{false};
  bool is_diagnostics_disabled{false};

  pure fn operator==(const reporting_state &other) const wontthrow->bool
  {
    return warning_level == other.warning_level &&
           is_annoying_disabled == other.is_annoying_disabled &&
           is_diagnostics_disabled == other.is_diagnostics_disabled;
  }
};

struct inheritable_analysis_state
{
  static constexpr StringView ENVIRONMENT_NAME{"KOSH_ANALYSIS"};

  bool is_mimicry_enabled{false};
  reporting_state reporting;

  static fn decode(StringView text) throws -> inheritable_analysis_state;
  static fn from_environment() throws -> inheritable_analysis_state;
  fn encode(String &text) const throws -> void;
};

struct subshell_bootstrap_reader;

struct shopt_state
{
  u64 overrides{0};
  u64 values{0};

  pure fn is_valid() const wontthrow -> bool
  {
    return (values & ~overrides) == 0;
  }
};

struct dynamic_clock_state
{
  u64 random_state{0};
  i64 shell_start_time{0};
  i64 seconds_base{0};
  bool is_random_reseed_pending{false};

  fn append_wire(String &output) const throws -> void;
  static fn from_wire(subshell_bootstrap_reader &reader,
                      dynamic_clock_state &clock) wontthrow -> bool;
};

struct getopts_cursor
{
  usize char_index{1};
  i64 last_optind{0};

  fn append_wire(String &output) const throws -> void;
  static fn from_wire(subshell_bootstrap_reader &reader,
                      getopts_cursor &cursor) wontthrow -> bool;
};

class RuntimeState
{
public:
  fn append_wire(String &output) const throws -> void;
  static fn from_wire(subshell_bootstrap_reader &reader,
                      RuntimeState &runtime) wontthrow -> bool;

  pure fn is_diagnostics_disabled() const wontthrow -> bool;
  fn set_diagnostics_disabled(bool enabled) wontthrow -> void;
  pure fn is_annoying_diagnostics_enabled() const wontthrow -> bool;
  fn set_annoying_diagnostics_enabled(bool enabled) wontthrow -> void;
#define KOSH_RUNTIME_FLAG(getter, setter, flag)                                \
  pure fn getter() const wontthrow -> bool { return has_flag(Flag::flag); }    \
  fn setter(bool enabled) wontthrow -> void { set_flag(Flag::flag, enabled); }

  KOSH_RUNTIME_FLAG(was_error_unset_set_explicitly,
                    set_error_unset_set_explicitly, ErrorUnsetExplicit)
  KOSH_RUNTIME_FLAG(was_pipefail_set_explicitly, set_pipefail_set_explicitly,
                    PipefailExplicit)
  KOSH_RUNTIME_FLAG(was_failglob_set_explicitly, set_failglob_set_explicitly,
                    FailglobExplicit)
  KOSH_RUNTIME_FLAG(was_extended_arithmetic_set_explicitly,
                    set_extended_arithmetic_set_explicitly,
                    ExtendedArithmeticExplicit)
  KOSH_RUNTIME_FLAG(was_glob_ignore_assigned, set_glob_ignore_assigned,
                    GlobIgnoreAssigned)

#undef KOSH_RUNTIME_FLAG

  pure static constexpr fn option_mask(shell_option_id option) wontthrow -> u64
  {
    return u64{1} << static_cast<u8>(option);
  }

  pure fn option_is_enabled(shell_option_id option) const wontthrow -> bool
  {
    return (m_shell_options & option_mask(option)) != 0;
  }

  pure fn get_shell_options() const wontthrow -> u64 { return m_shell_options; }
  fn set_shell_options(u64 options) wontthrow -> void
  {
    m_shell_options = options;
    publish_path_compatibility();
  }

  fn publish_path_compatibility() const wontthrow -> void
  {
    os::set_path_compatibility(
        option_is_enabled(shell_option_id::DevNullCompatibility),
        option_is_enabled(shell_option_id::SlashTmpCompatibility));
  }

  pure fn koshkit_utilities_are_reachable() const wontthrow -> bool
  {
    if (option_is_enabled(shell_option_id::Restricted)) return false;

    return option_is_enabled(shell_option_id::Koshkit) ||
           m_mood == mimic_mood::Default;
  }

  pure fn is_bash_compatible() const wontthrow -> bool
  {
    return m_mood == mimic_mood::Bash || m_mood == mimic_mood::BashPosix;
  }

  pure fn is_posix_mode() const wontthrow -> bool
  {
    return m_mood == mimic_mood::Posix;
  }
  pure fn bash_dynamic_variables_enabled() const wontthrow -> bool
  {
    return !is_posix_mode();
  }
  pure fn bash_additions_enabled() const wontthrow -> bool
  {
    return !is_posix_mode();
  }

  pure fn is_posix_option_on() const wontthrow -> bool
  {
    return m_mood == mimic_mood::Posix || m_mood == mimic_mood::BashPosix;
  }

  fn set_mood(mimic_mood value) wontthrow -> void { m_mood = value; }
  pure fn get_mood() const wontthrow -> mimic_mood { return m_mood; }

  fn set_tab_selector(tab_selector_mode selector) wontthrow -> void
  {
    m_tab_selector = selector;
  }
  pure fn get_tab_selector() const wontthrow -> tab_selector_mode
  {
    return m_tab_selector;
  }

  fn set_mimicry(bool enabled) wontthrow -> void
  {
    set_option(shell_option_id::Mimicry, enabled);
  }
  pure fn is_mimicry_enabled() const wontthrow -> bool
  {
    return option_is_enabled(shell_option_id::Mimicry);
  }

  fn set_warning_level(u8 level) wontthrow -> void
  {
    m_reporting.warning_level = level;
  }
  pure fn get_warning_level() const wontthrow -> u8
  {
    return m_reporting.warning_level;
  }

  pure fn get_inheritable_analysis_state() const wontthrow
      -> inheritable_analysis_state
  {
    return {is_mimicry_enabled(), get_reporting_state()};
  }
  fn set_inheritable_analysis_state(
      const inheritable_analysis_state &state) wontthrow -> void
  {
    set_mimicry(state.is_mimicry_enabled);
    set_reporting_state(state.reporting);
  }
  pure fn get_reporting_state() const wontthrow -> reporting_state
  {
    return m_reporting;
  }
  fn set_reporting_state(const reporting_state &state) wontthrow -> void
  {
    m_reporting = state;
  }
  fn set_warnings_enabled(bool enabled) wontthrow -> void
  {
    if (!enabled)
      m_reporting.warning_level = 0;
    else if (m_reporting.warning_level < 3)
      m_reporting.warning_level++;
  }

  fn set_option(shell_option_id option, bool enabled) wontthrow -> void
  {
    LOG(Info, "option %u flips to %s", static_cast<unsigned>(option),
        enabled ? "on" : "off");
    if (enabled)
      m_shell_options |= option_mask(option);
    else
      m_shell_options &= ~option_mask(option);

    if (option == shell_option_id::DevNullCompatibility ||
        option == shell_option_id::SlashTmpCompatibility)
    {
      publish_path_compatibility();
    }
  }

  fn set_shopt_option(u8 index, bool enabled) wontthrow -> void
  {
    let const mask = u64{1} << index;
    m_shopt.overrides |= mask;
    if (enabled)
      m_shopt.values |= mask;
    else
      m_shopt.values &= ~mask;
  }

  pure fn is_shopt_option_overridden(u8 index) const wontthrow -> bool
  {
    return (m_shopt.overrides & (u64{1} << index)) != 0;
  }

  pure fn is_shopt_option_enabled(u8 index) const wontthrow -> bool
  {
    return (m_shopt.values & (u64{1} << index)) != 0;
  }

  pure fn is_shopt_enabled(shopt_option_id option) const wontthrow -> bool;

#define KOSH_RUNTIME_OPTION(setter, getter, option)                            \
  fn setter(bool enabled) wontthrow -> void                                    \
  {                                                                            \
    set_option(shell_option_id::option, enabled);                              \
  }                                                                            \
  pure fn getter() const wontthrow -> bool                                     \
  {                                                                            \
    return option_is_enabled(shell_option_id::option);                         \
  }

  KOSH_RUNTIME_OPTION(set_error_exit, error_exit, Errexit)
  KOSH_RUNTIME_OPTION(set_echo_expanded, should_echo_expanded, Xtrace)
  KOSH_RUNTIME_OPTION(set_error_unset, error_unset, Nounset)
  KOSH_RUNTIME_OPTION(set_pipefail, pipefail, Pipefail)
  KOSH_RUNTIME_OPTION(set_no_clobber, no_clobber, Noclobber)
  KOSH_RUNTIME_OPTION(set_export_all, export_all, Allexport)
  KOSH_RUNTIME_OPTION(set_no_glob, no_glob, Noglob)
  KOSH_RUNTIME_OPTION(set_no_exec, no_exec, Noexec)
  KOSH_RUNTIME_OPTION(set_extended_arithmetic, is_extended_arithmetic_enabled,
                      ExtendedArithmetic)
  KOSH_RUNTIME_OPTION(set_koshkit, koshkit, Koshkit)
  KOSH_RUNTIME_OPTION(set_failglob, failglob, Failglob)
  KOSH_RUNTIME_OPTION(set_echo, should_echo, Verbose)
  KOSH_RUNTIME_OPTION(set_stats_enabled, stats_enabled, ShowStats)
  KOSH_RUNTIME_OPTION(set_show_ast, show_ast, ShowAst)
  KOSH_RUNTIME_OPTION(set_show_lexed_words, show_lexed_words, ShowLexedWords)
  KOSH_RUNTIME_OPTION(set_show_all_exit_codes, show_all_exit_codes,
                      ShowAllExitCodes)
  KOSH_RUNTIME_OPTION(set_memory_stats_enabled, memory_stats_enabled,
                      ShowMemory)

#undef KOSH_RUNTIME_OPTION

  fn set_show_exit_code(bool enabled) wontthrow -> void
  {
    set_option(shell_option_id::ShowExitCode, enabled);
  }
  pure fn show_exit_code() const wontthrow -> bool
  {
    return option_is_enabled(shell_option_id::ShowExitCode) ||
           option_is_enabled(shell_option_id::ShowAllExitCodes);
  }

  mustuse static fn capture(const EvalContext &context) wontthrow
      -> RuntimeState;
  fn restore(EvalContext &context) const wontthrow -> void;

private:
  enum class Flag : u8
  {
    DiagnosticsDisabled = 1U << 0,
    AnnoyingDiagnosticsEnabled = 1U << 1,
    ErrorUnsetExplicit = 1U << 2,
    PipefailExplicit = 1U << 3,
    FailglobExplicit = 1U << 4,
    ExtendedArithmeticExplicit = 1U << 5,
    GlobIgnoreAssigned = 1U << 6,
  };

  static constexpr u8 REPORTING_FLAGS =
      static_cast<u8>(Flag::DiagnosticsDisabled) |
      static_cast<u8>(Flag::AnnoyingDiagnosticsEnabled);
  static constexpr u8 ALL_FLAGS =
      REPORTING_FLAGS | static_cast<u8>(Flag::ErrorUnsetExplicit) |
      static_cast<u8>(Flag::PipefailExplicit) |
      static_cast<u8>(Flag::FailglobExplicit) |
      static_cast<u8>(Flag::ExtendedArithmeticExplicit) |
      static_cast<u8>(Flag::GlobIgnoreAssigned);

  pure fn get_wire_flags() const wontthrow -> u8
  {
    let const reporting_flags =
        (m_reporting.is_diagnostics_disabled
             ? static_cast<u8>(Flag::DiagnosticsDisabled)
             : u8{0}) |
        (m_reporting.is_annoying_disabled
             ? u8{0}
             : static_cast<u8>(Flag::AnnoyingDiagnosticsEnabled));
    return static_cast<u8>((m_flags & ~REPORTING_FLAGS) | reporting_flags);
  }
  fn set_wire_flags(u8 flags) wontthrow -> void
  {
    m_flags = static_cast<u8>(flags & ~REPORTING_FLAGS);
    m_reporting.is_diagnostics_disabled =
        (flags & static_cast<u8>(Flag::DiagnosticsDisabled)) != 0;
    m_reporting.is_annoying_disabled =
        (flags & static_cast<u8>(Flag::AnnoyingDiagnosticsEnabled)) == 0;
  }
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

  mimic_mood m_mood{mimic_mood::Default};
  tab_selector_mode m_tab_selector{tab_selector_mode::Interactive};
  u8 m_flags{0};
  reporting_state m_reporting{};
  u64 m_shell_options{option_mask(shell_option_id::ExtendedArithmetic) |
                      option_mask(shell_option_id::Failglob) |
                      option_mask(shell_option_id::Hashall) |
                      option_mask(shell_option_id::HistoryPrefixSearch) |
                      option_mask(shell_option_id::InteractiveHints) |
                      option_mask(shell_option_id::InteractiveDiagnostics) |
                      option_mask(shell_option_id::ExtendedKeys) |
                      option_mask(shell_option_id::TabCompletion) |
                      option_mask(shell_option_id::SyntaxHighlighting) |
                      option_mask(shell_option_id::DevNullCompatibility) |
                      option_mask(shell_option_id::SlashTmpCompatibility) |
                      option_mask(shell_option_id::Braceexpand)};
  shopt_state m_shopt;
};

static_assert(sizeof(RuntimeState) == 32);

class RuntimeStateScope
{
public:
  explicit RuntimeStateScope(EvalContext &context);
  RuntimeStateScope(const RuntimeStateScope &) = delete;
  fn operator=(const RuntimeStateScope &) = delete;
  ~RuntimeStateScope();

private:
  EvalContext &m_context;
  RuntimeState m_saved;
};

struct definition_state
{
  mimic_mood mood{mimic_mood::Default};
  reporting_state reporting;

  static fn from(const RuntimeState &runtime) wontthrow -> definition_state
  {
    return {runtime.get_mood(), runtime.get_reporting_state()};
  }

  fn apply_to(RuntimeState &runtime) const wontthrow -> void
  {
    runtime.set_mood(mood);
    runtime.set_reporting_state(reporting);
  }

  pure fn operator==(const definition_state &other) const wontthrow->bool
  {
    return mood == other.mood && reporting == other.reporting;
  }

  fn append_wire(String &output) const throws -> void;
  static fn from_wire(subshell_bootstrap_reader &reader,
                      definition_state &state) wontthrow -> bool;
};

inline pure fn RuntimeState::is_diagnostics_disabled() const wontthrow -> bool
{
  return m_reporting.is_diagnostics_disabled;
}

inline fn RuntimeState::set_diagnostics_disabled(bool enabled) wontthrow -> void
{
  m_reporting.is_diagnostics_disabled = enabled;
}

inline pure fn RuntimeState::is_annoying_diagnostics_enabled() const wontthrow
    -> bool
{
  return !m_reporting.is_annoying_disabled;
}

inline fn RuntimeState::set_annoying_diagnostics_enabled(bool enabled) wontthrow
    -> void
{
  m_reporting.is_annoying_disabled = !enabled;
}

} /* namespace koshka */

#include "EvalOperations.hpp"
#include "EvalSnapshot.hpp"
#include "EvalTypes.hpp"
#include "ExecContext.hpp"
#include "ProgramResolver.hpp"

namespace koshka {

class EvalContext;

namespace completion {
class shell_highlight_cache;
} /* namespace completion */

class RuntimeControlStore
{
public:
  fn set_init_mood_sourcing(mimic_mood mood, bool active) wontthrow -> void
  {
    let const bit = static_cast<u8>(1U << static_cast<u8>(mood));
    if (active)
      m_init_moods_sourcing |= bit;
    else
      m_init_moods_sourcing &= static_cast<u8>(~bit);
  }

  pure fn init_mood_sourcing(mimic_mood mood) const wontthrow -> bool
  {
    return (m_init_moods_sourcing & (1U << static_cast<u8>(mood))) != 0;
  }

  fn mark_mood_initialized(mimic_mood mood) wontthrow -> void
  {
    m_initialized_moods |= static_cast<u8>(1U << static_cast<u8>(mood));
  }

  pure fn mood_initialized(mimic_mood mood) const wontthrow -> bool
  {
    return (m_initialized_moods & (1U << static_cast<u8>(mood))) != 0;
  }

  fn note_explicit_mood() wontthrow -> void
  {
    m_mutations.reporting.was_mood_set_explicitly = true;
    m_mutations.reporting.mood++;
  }

  pure fn was_mood_set_explicitly() const wontthrow -> bool
  {
    return m_mutations.reporting.was_mood_set_explicitly;
  }

  fn note_warning_option_mutation() wontthrow -> void
  {
    m_mutations.reporting.warning++;
  }

  fn note_diagnostics_option_mutation() wontthrow -> void
  {
    m_mutations.reporting.diagnostics++;
  }

  pure fn diagnostics_mutation_revision() const wontthrow -> u64
  {
    return m_mutations.reporting.diagnostics;
  }

  fn note_annoying_diagnostics_option_mutation() wontthrow -> void
  {
    m_mutations.reporting.annoying++;
  }

  fn set_warning_suppressed(suppressible_warning which, bool enabled) wontthrow
      -> void
  {
    let const bit = u32{1} << static_cast<u32>(which);
    if (enabled)
      m_suppressed_warnings |= bit;
    else
      m_suppressed_warnings &= ~bit;
  }

  pure fn is_warning_suppressed(suppressible_warning which) const wontthrow
      -> bool
  {
    return (m_suppressed_warnings & (u32{1} << static_cast<u32>(which))) != 0;
  }

  fn option_mutations() wontthrow -> shell_option_mutations &
  {
    return m_mutations.options;
  }

  pure fn option_mutations() const wontthrow -> const shell_option_mutations &
  {
    return m_mutations.options;
  }

  pure fn get_mutations() const wontthrow -> const control_mutations &
  {
    return m_mutations;
  }

  pure fn init_moods_sourcing_mask() const wontthrow -> u8
  {
    return m_init_moods_sourcing;
  }

  pure fn initialized_moods_mask() const wontthrow -> u8
  {
    return m_initialized_moods;
  }

  fn restore_snapshot_state(u8 init_moods_sourcing, u8 initialized_moods,
                            const control_mutations &mutations) wontthrow
      -> void
  {
    m_init_moods_sourcing = init_moods_sourcing;
    m_initialized_moods = initialized_moods;
    m_mutations = mutations;
  }

  pure fn snapshot() const wontthrow -> runtime_control_snapshot
  {
    return runtime_control_snapshot{m_init_moods_sourcing, m_initialized_moods,
                                    m_mutations};
  }

  fn restore(const runtime_control_snapshot &snapshot) wontthrow -> void
  {
    restore_snapshot_state(snapshot.init_moods_sourcing,
                           snapshot.initialized_moods, snapshot.mutations);
  }

  fn append_wire(String &output) const throws -> void;
  static fn from_wire(subshell_bootstrap_reader &reader,
                      runtime_control_wire &wire) wontthrow -> bool;
  fn apply_wire(const runtime_control_wire &wire) wontthrow -> void
  {
    m_init_moods_sourcing = wire.init_moods_sourcing;
    m_initialized_moods = wire.initialized_moods;
    m_suppressed_warnings = wire.suppressed_warnings;
  }

private:
  u8 m_init_moods_sourcing{0};
  u8 m_initialized_moods{0};
  u32 m_suppressed_warnings{0};
  control_mutations m_mutations;
};

class ScopeStore
{
public:
  fn aliases() wontthrow -> StringMap<String> & { return m_aliases; }
  pure fn aliases() const wontthrow -> const StringMap<String> &
  {
    return m_aliases;
  }
  fn local_scopes() wontthrow -> ArrayList<ArrayList<local_binding>> &
  {
    return m_local_scopes;
  }
  pure fn local_scopes() const wontthrow
      -> const ArrayList<ArrayList<local_binding>> &
  {
    return m_local_scopes;
  }
  fn saved_scope_shell_options() wontthrow -> ArrayList<Maybe<u64>> &
  {
    return m_saved_scope_shell_options;
  }
  fn local_scope_depth() wontthrow -> usize & { return m_local_scope_depth; }
  pure fn local_scope_depth() const wontthrow -> usize
  {
    return m_local_scope_depth;
  }
  fn current_local_scope() wontthrow -> ArrayList<local_binding> &
  {
    ASSERT(m_local_scope_depth != 0);
    ASSERT(m_local_scope_depth <= m_local_scopes.count());
    return m_local_scopes[m_local_scope_depth - 1];
  }
  pure fn current_local_scope() const wontthrow
      -> const ArrayList<local_binding> &
  {
    ASSERT(m_local_scope_depth != 0);
    ASSERT(m_local_scope_depth <= m_local_scopes.count());
    return m_local_scopes[m_local_scope_depth - 1];
  }
  pure fn has_current_local(StringView name) const wontthrow -> bool
  {
    if (m_local_scope_depth == 0) return false;
    for (let const &binding : current_local_scope())
      if (binding.name.view() == name) return !binding.is_self_reference;
    return false;
  }
  pure fn has_active_local(StringView name) const wontthrow -> bool
  {
    ASSERT(m_local_scope_depth <= m_local_scopes.count());
    for (usize frame_index = m_local_scope_depth; frame_index-- > 0;) {
      for (let const &binding : m_local_scopes[frame_index])
        if (binding.name.view() == name && !binding.is_self_reference)
          return true;
    }
    return false;
  }
  pure fn is_self_reference(StringView name) const wontthrow -> bool
  {
    ASSERT(m_local_scope_depth <= m_local_scopes.count());
    for (usize frame_index = m_local_scope_depth; frame_index-- > 0;) {
      for (let const &binding : m_local_scopes[frame_index])
        if (binding.name.view() == name) return binding.is_self_reference;
    }
    return false;
  }
  pure fn is_current_self_reference(StringView name) const wontthrow -> bool
  {
    if (m_local_scope_depth == 0) return false;
    for (let const &binding : current_local_scope())
      if (binding.name.view() == name) return binding.is_self_reference;
    return false;
  }
  fn forget_current_self_reference(StringView name) throws -> void
  {
    if (m_local_scope_depth == 0) return;
    let &scope = current_local_scope();
    for (usize i = 0; i < scope.count(); i++) {
      if (scope[i].name.view() != name) continue;
      if (scope[i].is_self_reference) scope.remove(i);
      return;
    }
  }

  fn set_alias(StringView name, StringView value) throws -> void
  {
    m_aliases.set(name, value);
  }
  fn remove_alias(StringView name) throws -> bool
  {
    if (!m_aliases.find(name).has_value()) return false;
    m_aliases.erase(name);
    return true;
  }
  pure fn has_aliases() const wontthrow -> bool
  {
    return m_aliases.count() != 0;
  }
  fn get_alias(StringView name) const throws -> Maybe<String>
  {
    if (let const value = m_aliases.find(name); value.has_value())
      return String{heap_allocator(), value->view()};
    return None;
  }
  pure fn find_alias(StringView name) const wontthrow -> Maybe<StringView>
  {
    if (let const value = m_aliases.find(name); value.has_value())
      return value->view();
    return None;
  }
  fn alias_definitions() const throws
      -> SortedArrayList<String, order_comparator<String>>
  {
    let out = ArrayList<String>{heap_allocator()};
    m_aliases.for_each([&out](StringView key, const String &value) {
      let definition = String{heap_allocator(), key};
      definition.push('=');
      append_shell_quoted_arg(definition, value.view());
      out.push(steal(definition));
    });
    return steal(out).make_sorted(sort_order::ascending);
  }
  fn alias_names() const throws -> HashSet
  {
    let out = HashSet{heap_allocator()};
    m_aliases.for_each([&out](StringView key, const String &value) {
      unused(value);
      out.add(key);
    });
    return out;
  }
  template <typename Callback>
  fn for_each_alias_name(Callback callback) const throws -> void
  {
    m_aliases.for_each([&](StringView name, const String &value) throws {
      unused(value);
      callback(name);
    });
  }

  fn snapshot() const throws -> scope_snapshot
  {
    return scope_snapshot{m_aliases, m_local_scopes, m_local_scope_depth};
  }
  fn restore(scope_snapshot snapshot) throws -> void
  {
    m_aliases = steal(snapshot.aliases);
    m_local_scopes = steal(snapshot.local_scopes);
    m_local_scope_depth = snapshot.local_scope_depth;
  }
  fn append_wire(String &output) const throws -> void;
  static fn from_wire(subshell_bootstrap_reader &reader,
                      ArrayList<ArrayList<local_binding>> &local_scopes) throws
      -> bool;
  fn apply_wire(ArrayList<ArrayList<local_binding>> local_scopes) wontthrow
      -> void
  {
    m_local_scope_depth = local_scopes.count();
    m_local_scopes = steal(local_scopes);
  }

private:
  StringMap<String> m_aliases{heap_allocator()};
  ArrayList<ArrayList<local_binding>> m_local_scopes{heap_allocator()};
  ArrayList<Maybe<u64>> m_saved_scope_shell_options{heap_allocator()};
  usize m_local_scope_depth{0};
};

class ExecutionStore
{
public:
  explicit ExecutionStore(bool shell_is_interactive,
                          String shell_name) wontthrow
      : m_shell_name(steal(shell_name)),
        m_shell_is_interactive(shell_is_interactive)
  {}

  pure fn get_shell_name() const wontthrow -> StringView
  {
    return m_shell_name.view();
  }
  fn set_shell_name(String shell_name) wontthrow -> void
  {
    m_shell_name = steal(shell_name);
  }
  pure fn get_shell_executable_path() const wontthrow -> StringView
  {
    return m_shell_executable_path.view();
  }
  fn set_shell_executable_path(String path) wontthrow -> void
  {
    m_shell_executable_path = steal(path);
  }
  pure fn get_last_argument() const wontthrow -> const String &
  {
    return m_last_argument;
  }
  fn set_last_argument(String argument) wontthrow -> void
  {
    m_last_argument = steal(argument);
  }
  pure fn has_execution_string() const wontthrow -> bool
  {
    return m_execution_string.has_value();
  }
  pure fn get_execution_string() const wontthrow -> StringView
  {
    return m_execution_string.has_value() ? m_execution_string->view()
                                          : StringView{};
  }
  fn set_execution_string(String text) wontthrow -> void
  {
    m_execution_string = steal(text);
  }
  pure fn get_command_number() const wontthrow -> usize
  {
    return m_command_number;
  }
  fn advance_command_number() wontthrow -> void { m_command_number++; }
  fn set_current_command(String command) wontthrow -> void
  {
    m_current_command = steal(command);
  }
  fn set_current_command(StringView command) throws -> void
  {
    m_current_command.clear();
    m_current_command += command;
  }
  pure fn get_current_command() const wontthrow -> StringView
  {
    return m_current_command.view();
  }
  fn snapshot() const throws -> execution_snapshot
  {
    return execution_snapshot{m_last_argument, m_terminal_exec_subshell_depth,
                              m_terminal_exec_allowed};
  }
  fn restore(execution_snapshot snapshot) wontthrow -> void
  {
    m_last_argument = steal(snapshot.last_argument);
    m_terminal_exec_subshell_depth = snapshot.terminal_exec_subshell_depth;
    m_terminal_exec_allowed = snapshot.terminal_exec_allowed;
  }
  fn append_wire(String &output) const throws -> void;
  static fn from_wire(subshell_bootstrap_reader &reader,
                      execution_wire &wire) throws -> bool;
  fn apply_wire(execution_wire wire) wontthrow -> void
  {
    m_execution_string = steal(wire.execution_string);
    m_last_argument = steal(wire.last_argument);
  }
  fn set_make_shell_suppressed(bool suppressed) wontthrow -> void
  {
    m_make_shell_suppressed = suppressed;
  }
  pure fn make_shell_suppressed() const wontthrow -> bool
  {
    return m_make_shell_suppressed;
  }

  fn last_exit_status() wontthrow -> i32 & { return m_last_exit_status; }
  fn set_last_exit_status(i32 status) wontthrow { m_last_exit_status = status; }
  pure fn last_exit_status() const wontthrow -> i32
  {
    return m_last_exit_status;
  }
  fn last_command_duration_nanos() wontthrow -> u64 &
  {
    return m_last_command_duration_nanos;
  }
  fn set_last_command_duration_nanos(u64 nanos) wontthrow
  {
    m_last_command_duration_nanos = nanos;
  }
  pure fn last_command_duration_nanos() const wontthrow -> u64
  {
    return m_last_command_duration_nanos;
  }
  fn subshell_depth() wontthrow -> usize & { return m_subshell_depth; }
  pure fn subshell_depth() const wontthrow -> usize { return m_subshell_depth; }
  fn condition_depth() wontthrow -> usize & { return m_condition_depth; }
  pure fn condition_depth() const wontthrow -> usize
  {
    return m_condition_depth;
  }
  fn loop_depth() wontthrow -> usize & { return m_loop_depth; }
  pure fn loop_depth() const wontthrow -> usize { return m_loop_depth; }
  fn terminal_exec_allowed() wontthrow -> bool &
  {
    return m_terminal_exec_allowed;
  }
  pure fn terminal_exec_allowed() const wontthrow -> bool
  {
    return m_terminal_exec_allowed;
  }
  pure fn get_terminal_exec_subshell_depth() const wontthrow -> usize
  {
    return m_terminal_exec_subshell_depth;
  }
  fn allow_terminal_exec_at_current_depth() wontthrow -> void
  {
    m_terminal_exec_subshell_depth = m_subshell_depth;
    m_terminal_exec_allowed = true;
  }
  fn completion_function_running() wontthrow -> bool &
  {
    return m_is_completion_function_running;
  }
  pure fn completion_function_running() const wontthrow -> bool
  {
    return m_is_completion_function_running;
  }
  fn should_mark_completion_directories() wontthrow -> bool &
  {
    return m_should_mark_completion_directories;
  }
  pure fn should_mark_completion_directories() const wontthrow -> bool
  {
    return m_should_mark_completion_directories;
  }
  pure fn get_completion_option_mask() const wontthrow -> u32
  {
    return m_completion_option_mask;
  }
  fn set_completion_option_mask(u32 option_mask) wontthrow -> void
  {
    m_completion_option_mask = option_mask;
  }
  pure fn get_completion_command_name() const wontthrow -> StringView
  {
    return m_completion_command_name.view();
  }
  fn set_completion_command_name(StringView command_name) throws -> void
  {
    m_completion_command_name = String{heap_allocator(), command_name};
  }
  fn prompt_command_running() wontthrow -> bool &
  {
    return m_is_prompt_command_running;
  }
  pure fn prompt_command_running() const wontthrow -> bool
  {
    return m_is_prompt_command_running;
  }
  fn pending_subshell_end_position() wontthrow -> u32 &
  {
    return m_pending_subshell_end_position;
  }
  fn should_elide_pending_subshell_fork() wontthrow -> bool &
  {
    return m_should_elide_pending_subshell_fork;
  }
  fn line_discard_root() wontthrow -> const Expression *&
  {
    return m_line_discard_root;
  }
  fn top_level_line_discard_root() wontthrow -> const Expression *&
  {
    return m_top_level_line_discard_root;
  }
  fn line_discard_source() wontthrow -> StringView &
  {
    return m_line_discard_source;
  }
  fn line_discard_status() wontthrow -> Maybe<i64> &
  {
    return m_line_discard_status;
  }
  fn line_discard_subshell_depth() wontthrow -> usize &
  {
    return m_line_discard_subshell_depth;
  }
  pure fn shell_is_interactive() const wontthrow -> bool
  {
    return m_shell_is_interactive;
  }
  fn set_shell_is_interactive(bool enabled) wontthrow -> void
  {
    m_shell_is_interactive = enabled;
  }

private:
  String m_shell_name{heap_allocator()};
  String m_shell_executable_path{heap_allocator()};
  String m_last_argument{heap_allocator()};
  Maybe<String> m_execution_string{None};
  String m_current_command{heap_allocator()};
  usize m_command_number{1};
  String m_completion_command_name{heap_allocator()};
  const Expression *m_line_discard_root{nullptr};
  const Expression *m_top_level_line_discard_root{nullptr};
  StringView m_line_discard_source{};
  Maybe<i64> m_line_discard_status{};
  usize m_line_discard_subshell_depth{0};
  u64 m_last_command_duration_nanos{0};
  usize m_subshell_depth{0};
  usize m_condition_depth{0};
  usize m_loop_depth{0};
  usize m_terminal_exec_subshell_depth{0};
  i32 m_last_exit_status{0};
  u32 m_pending_subshell_end_position{0};
  u32 m_completion_option_mask{0};
  bool m_make_shell_suppressed{false};
  bool m_should_elide_pending_subshell_fork{false};
  bool m_terminal_exec_allowed{false};
  bool m_is_completion_function_running{false};
  bool m_should_mark_completion_directories{false};
  bool m_is_prompt_command_running{false};
  bool m_shell_is_interactive{false};
};

class EvaluationMetricsStore
{
public:
  fn add_evaluated_expression(bool enabled) wontthrow -> void
  {
    if (enabled) m_expressions_executed_last++;
  }

  fn add_expansion(bool enabled) wontthrow -> void
  {
    if (enabled) m_expansions_last++;
  }

  fn add_builtin_run(bool is_enabled) wontthrow -> void
  {
    if (is_enabled) m_builtins_run++;
  }

  fn add_function_run(bool is_enabled) wontthrow -> void
  {
    if (is_enabled) m_functions_run++;
  }

  fn add_external_command_run(bool is_enabled) wontthrow -> void
  {
    if (is_enabled) m_external_commands_run++;
  }

  pure fn get_builtins_run() const wontthrow -> usize { return m_builtins_run; }
  pure fn get_functions_run() const wontthrow -> usize
  {
    return m_functions_run;
  }
  pure fn get_external_commands_run() const wontthrow -> usize
  {
    return m_external_commands_run;
  }

  fn end_command(usize live_ast_arena_bytes) wontthrow -> void
  {
    m_expansions_total += m_expansions_last;
    m_expressions_executed_total += m_expressions_executed_last;
    m_commands_evaluated++;
    if (live_ast_arena_bytes > m_peak_ast_arena_bytes)
      m_peak_ast_arena_bytes = live_ast_arena_bytes;
    m_expansions_last = 0;
    m_expressions_executed_last = 0;
  }

  fn begin_command_evaluation() wontthrow -> void
  {
    m_command_evaluation_index++;
  }

  pure fn last_expressions_executed() const wontthrow -> usize
  {
    return m_expressions_executed_last;
  }
  pure fn total_expressions_executed() const wontthrow -> usize
  {
    return m_expressions_executed_total + m_expressions_executed_last;
  }
  pure fn last_expansion_count() const wontthrow -> usize
  {
    return m_expansions_last;
  }
  pure fn total_expansion_count() const wontthrow -> usize
  {
    return m_expansions_total + m_expansions_last;
  }
  pure fn commands_evaluated() const wontthrow -> usize
  {
    return m_commands_evaluated;
  }
  pure fn peak_ast_arena_bytes() const wontthrow -> usize
  {
    return m_peak_ast_arena_bytes;
  }
  pure fn command_evaluation_index() const wontthrow -> usize
  {
    return m_command_evaluation_index;
  }

private:
  usize m_expressions_executed_last{0};
  usize m_expressions_executed_total{0};
  usize m_expansions_last{0};
  usize m_expansions_total{0};
  usize m_commands_evaluated{0};
  usize m_builtins_run{0};
  usize m_functions_run{0};
  usize m_external_commands_run{0};
  usize m_command_evaluation_index{0};
  usize m_peak_ast_arena_bytes{0};
};

class GitStatusCache
{
public:
  pure fn is_branch_current(usize command_index) const wontthrow -> bool
  {
    return m_branch_command_index == command_index;
  }
  pure fn are_counts_current(usize command_index) const wontthrow -> bool
  {
    return m_counts_command_index == command_index;
  }

  fn refresh_branch(usize command_index,
                    StringView ceiling_directories) const throws -> void;
  fn refresh_status(usize command_index,
                    StringView ceiling_directories) const throws -> void;

  pure fn get_branch() const wontthrow -> StringView { return m_branch.view(); }
  pure fn get_ahead_count() const wontthrow -> i32 { return m_ahead_count; }
  pure fn get_behind_count() const wontthrow -> i32 { return m_behind_count; }

private:
  mutable usize m_branch_command_index{static_cast<usize>(-1)};
  mutable usize m_counts_command_index{static_cast<usize>(-1)};
  mutable String m_branch{heap_allocator()};
  mutable i32 m_ahead_count{0};
  mutable i32 m_behind_count{0};
};

class PromptCommandStore
{
public:
  fn get_arena() wontthrow -> BumpArena & { return m_arena; }
  fn get_cached_text() wontthrow -> String & { return m_cached_text; }
  pure fn get_cached_ast() const wontthrow -> Expression *
  {
    return m_cached_ast;
  }
  fn set_cached_ast(Expression *ast) wontthrow -> void { m_cached_ast = ast; }

private:
  BumpArena m_arena{};
  String m_cached_text{heap_allocator()};
  Expression *m_cached_ast{nullptr};
};

class ControlFlowStore
{
public:
  fn request_loop_control(control_flow::Kind kind, i64 level, usize loop_depth,
                          SourceLocation location, const String *source,
                          StringView origin) throws -> void
  {
    if (loop_depth == 0) return;
    if (static_cast<usize>(level) > loop_depth)
      level = static_cast<i64>(loop_depth);
    if (level < 1) level = 1;
    set(control_flow{level, source, String{origin}, location, kind});
  }
  fn request_return(i64 status, SourceLocation location, const String *source,
                    StringView origin) throws -> void
  {
    set(control_flow{status, source, String{origin}, location,
                     control_flow::Kind::Return});
  }
  fn request_exit(i64 status, SourceLocation location, const String *source,
                  StringView origin) throws -> void
  {
    set(control_flow{status, source, String{origin}, location,
                     control_flow::Kind::Exit});
  }
  fn set(control_flow value) throws -> void { m_pending = steal(value); }
  fn pending() wontthrow -> control_flow & { return m_pending; }
  pure fn pending() const wontthrow -> const control_flow &
  {
    return m_pending;
  }
  pure fn has_pending() const wontthrow -> bool
  {
    return m_pending.kind != control_flow::Kind::Normal;
  }
  pure fn has_pending_loop_jump() const wontthrow -> bool
  {
    return m_pending.kind == control_flow::Kind::Break ||
           m_pending.kind == control_flow::Kind::Continue;
  }
  fn clear() wontthrow -> void { m_pending.kind = control_flow::Kind::Normal; }

private:
  control_flow m_pending{};
};

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

enum class substring_subject : u8
{
  Scalar,
  List,
};

struct substring_bounds
{
  i64 start;
  i64 end;
};

struct quoted_empty_mark
{
  usize piece;
  usize offset;
};

enum class parameter_word_quoting : u8
{
  Unquoted,
  DoubleQuoted,
  HereDocument,
};

enum class expansion_error_reach : u8
{
  Line,
  LineOrPosixScript,
  CommandOrPosixScript,
};

enum class arithmetic_text_kind : u8
{
  Value,
  ShellSource,
};

fn compute_substring_bounds(i64 value_count, i64 offset, Maybe<i64> length,
                            substring_subject subject) throws
    -> substring_bounds;
pure fn shopt_option_index(StringView name) wontthrow -> Maybe<u8>;

enum class shopt_option_id : u8
{
  Autocd,
  Checkhash,
  ExpandAliases,
  Extdebug,
  Extglob,
  InheritErrexit,
  Lastpipe,
  LocalvarInherit,
  LocalvarUnset,
  Nullglob,
  PatsubReplacement,
  Progcomp,
  ProgcompAlias,
  Sourcepath,
};
pure fn shopt_option_index(shopt_option_id option) wontthrow -> u8;

inline pure fn
RuntimeState::is_shopt_enabled(shopt_option_id option) const wontthrow -> bool
{
  let const index = shopt_option_index(option);
  if (is_shopt_option_overridden(index)) return is_shopt_option_enabled(index);
  switch (option) {
  case shopt_option_id::PatsubReplacement:
  case shopt_option_id::Progcomp:
  case shopt_option_id::Sourcepath: return true;
  default: return false;
  }
}

enum class BashArgumentFrameFlag : u8
{
  DidEnter = 1 << 0,
  IsSource = 1 << 1,
  HasSourceArguments = 1 << 2,
};

struct BashArgumentFrameContext
{
  BashArgumentFrameContext *previous{nullptr};
  StringView source_path{};
  u8 flags{0};

  pure fn has_flag(BashArgumentFrameFlag flag) const wontthrow -> bool
  {
    return (flags & static_cast<u8>(flag)) != 0;
  }
  fn set_flag(BashArgumentFrameFlag flag) wontthrow -> void
  {
    flags |= static_cast<u8>(flag);
  }
};

class BashArgumentStack
{
public:
  pure fn is_active() const wontthrow -> bool { return m_is_active; }
  pure fn values() const wontthrow -> const ArrayList<String> &
  {
    return m_values;
  }
  pure fn frame_counts() const wontthrow -> const ArrayList<u32> &
  {
    return m_frame_counts;
  }
  pure fn get_context() const wontthrow -> BashArgumentFrameContext *
  {
    return m_context;
  }
  fn set_context(BashArgumentFrameContext *context) wontthrow -> void
  {
    m_context = context;
  }
  fn adopt_inherited_context(u8 flags, String source_path) wontthrow -> void
  {
    ASSERT(m_context == nullptr);
    m_inherited_source_path = steal(source_path);
    m_inherited_context.previous = nullptr;
    m_inherited_context.source_path = m_inherited_source_path.view();
    m_inherited_context.flags = flags;
    m_context = &m_inherited_context;
  }

  fn activate(ArrayList<String> values, ArrayList<u32> frame_counts) wontthrow
      -> void
  {
    ASSERT(!m_is_active);
    m_values = steal(values);
    m_frame_counts = steal(frame_counts);
    m_is_active = true;
  }

  fn reset() wontthrow -> void
  {
    m_values = ArrayList<String>{heap_allocator()};
    m_frame_counts = ArrayList<u32>{heap_allocator()};
    m_is_active = false;
  }

  fn push_frame(const ArrayList<String> &arguments) throws -> void
  {
    ASSERT(m_is_active);
    let const previous_value_count = m_values.count();
    m_values.reserve(previous_value_count + arguments.count());
    m_frame_counts.reserve(m_frame_counts.count() + 1);
    try {
      for (let const &argument : arguments)
        m_values.push_managed(argument.view());
    } catch (...) {
      while (m_values.count() > previous_value_count)
        m_values.pop_back();
      throw;
    }
    m_frame_counts.push(static_cast<u32>(arguments.count()));
  }

  fn push_frame(StringView argument) throws -> void
  {
    ASSERT(m_is_active);
    m_values.reserve(m_values.count() + 1);
    m_frame_counts.reserve(m_frame_counts.count() + 1);
    m_values.push_managed(argument);
    m_frame_counts.push(1);
  }

  fn pop_frame() wontthrow -> void
  {
    ASSERT(m_is_active);
    ASSERT(!m_frame_counts.is_empty());
    let const argument_count = m_frame_counts.back();
    m_frame_counts.pop_back();
    ASSERT(argument_count <= m_values.count());
    for (u32 index = 0; index < argument_count; index++)
      m_values.pop_back();
  }

  fn truncate_to(usize value_count, usize frame_count) wontthrow -> void
  {
    ASSERT(m_is_active);
    ASSERT(m_values.count() >= value_count);
    ASSERT(m_frame_counts.count() >= frame_count);
    while (m_values.count() > value_count)
      m_values.pop_back();
    while (m_frame_counts.count() > frame_count)
      m_frame_counts.pop_back();
  }

private:
  ArrayList<String> m_values{heap_allocator()};
  ArrayList<u32> m_frame_counts{heap_allocator()};
  BashArgumentFrameContext *m_context{nullptr};
  BashArgumentFrameContext m_inherited_context{};
  String m_inherited_source_path{heap_allocator()};
  bool m_is_active{false};
};

class ScalarVariables
{
public:
  explicit ScalarVariables(VariableTable *table) wontthrow : m_table{table} {}

  hot pure fn find(StringView name) const wontthrow -> Maybe<const String *>
  {
    let const entry = m_table->find(name);
    if (!entry.has_value() || !(*entry)->has_value) return None;
    return &(*entry)->value;
  }
  hot fn find(StringView name) wontthrow -> Maybe<String *>
  {
    let const entry = m_table->find(name);
    if (!entry.has_value() || !(*entry)->has_value) return None;
    return &(*entry)->value;
  }
  fn set(StringView name, StringView value) throws -> String *
  {
    return m_table->set_value(name, value);
  }
  fn erase(StringView name) throws -> void { m_table->erase_value(name); }
  pure fn count() const wontthrow -> usize { return m_table->value_count(); }

  template <typename Callback>
  fn for_each(Callback do_callback) const throws -> void
  {
    m_table->for_each([&](StringView name, const variable_entry &entry) throws {
      if (entry.has_value) do_callback(name, entry.value);
    });
  }

private:
  VariableTable *m_table;
};

class IndexedArrays
{
public:
  explicit IndexedArrays(VariableTable *table) wontthrow : m_table{table} {}

  hot pure fn find(StringView name) const wontthrow
      -> Maybe<const ArrayList<String> *>
  {
    let const entry = m_table->find(name);
    if (!entry.has_value() || !(*entry)->is_indexed_array) return None;
    return &(*entry)->elements;
  }
  hot fn find(StringView name) wontthrow -> Maybe<ArrayList<String> *>
  {
    let const entry = m_table->find(name);
    if (!entry.has_value() || !(*entry)->is_indexed_array) return None;
    return &(*entry)->elements;
  }
  fn set(StringView name, ArrayList<String> elements) throws
      -> ArrayList<String> *
  {
    return m_table->set_elements(name, steal(elements));
  }
  fn get_or_create(StringView name, ArrayList<String> default_elements) throws
      -> ArrayList<String> &
  {
    if (let const existing = find(name); existing.has_value())
      return **existing;

    return *m_table->set_elements(name, steal(default_elements));
  }
  fn erase(StringView name) throws -> void { m_table->erase_elements(name); }
  pure fn count() const wontthrow -> usize
  {
    return m_table->indexed_array_count();
  }

  template <typename Callback>
  fn for_each(Callback do_callback) const throws -> void
  {
    m_table->for_each([&](StringView name, const variable_entry &entry) throws {
      if (entry.is_indexed_array) do_callback(name, entry.elements);
    });
  }

private:
  VariableTable *m_table;
};

class VariableAttributes
{
public:
  explicit VariableAttributes(VariableTable *table) wontthrow : m_table{table}
  {}

  pure fn get_bits(StringView name) const wontthrow -> u8
  {
    let const entry = m_table->find(name);
    return entry.has_value() ? (*entry)->attribute_bits : u8{0};
  }
  fn set_bits(StringView name, u8 bits) throws -> void
  {
    m_table->set_attribute_bits(name, bits);
  }
  fn erase(StringView name) throws -> void
  {
    m_table->set_attribute_bits(name, 0);
  }

  pure fn has(StringView name, variable_attribute attribute) const wontthrow
      -> bool
  {
    return (get_bits(name) & static_cast<u8>(attribute)) != 0;
  }
  fn set(StringView name, variable_attribute attribute, bool is_enabled) throws
      -> void
  {
    let const mask = static_cast<u8>(attribute);

    if (is_enabled) {
      m_table->note_attribute_bits(mask);
      m_table->get_or_create(name).attribute_bits |= mask;
      return;
    }

    let const entry = m_table->find(name);
    if (!entry.has_value()) return;

    (*entry)->attribute_bits &= static_cast<u8>(~mask);
    m_table->release_if_unused(name, **entry);
  }

  pure fn is_readonly(StringView name) const wontthrow -> bool
  {
    return has(name, variable_attribute::Readonly);
  }
  pure fn is_declared(StringView name) const wontthrow -> bool
  {
    return has(name, variable_attribute::Declared);
  }
  pure fn is_integer(StringView name) const wontthrow -> bool
  {
    return has(name, variable_attribute::Integer);
  }
  pure fn is_lowercase(StringView name) const wontthrow -> bool
  {
    return has(name, variable_attribute::Lowercase);
  }
  pure fn is_uppercase(StringView name) const wontthrow -> bool
  {
    return has(name, variable_attribute::Uppercase);
  }
  pure fn has_case(StringView name) const wontthrow -> bool
  {
    return (get_bits(name) &
            (static_cast<u8>(variable_attribute::Lowercase) |
             static_cast<u8>(variable_attribute::Uppercase))) != 0;
  }
  hot pure fn has_namerefs() const wontthrow -> bool
  {
    return m_table->has_namerefs();
  }
  hot pure fn is_nameref(StringView name) const wontthrow -> bool
  {
    return m_table->has_namerefs() && has(name, variable_attribute::Nameref);
  }

  fn mark_readonly(StringView name) throws -> void
  {
    set(name, variable_attribute::Readonly, true);
  }
  fn unmark_readonly(StringView name) throws -> void
  {
    set(name, variable_attribute::Readonly, false);
  }
  fn mark_declared(StringView name) throws -> void
  {
    set(name, variable_attribute::Declared, true);
  }
  fn unmark_declared(StringView name) throws -> void
  {
    if (m_table->has_declared_marks())
      set(name, variable_attribute::Declared, false);
  }
  fn mark_integer(StringView name) throws -> void
  {
    set(name, variable_attribute::Integer, true);
  }
  fn unmark_integer(StringView name) throws -> void
  {
    set(name, variable_attribute::Integer, false);
  }
  fn mark_lowercase(StringView name) throws -> void
  {
    set(name, variable_attribute::Uppercase, false);
    set(name, variable_attribute::Lowercase, true);
  }
  fn unmark_lowercase(StringView name) throws -> void
  {
    set(name, variable_attribute::Lowercase, false);
  }
  fn mark_uppercase(StringView name) throws -> void
  {
    set(name, variable_attribute::Lowercase, false);
    set(name, variable_attribute::Uppercase, true);
  }
  fn unmark_uppercase(StringView name) throws -> void
  {
    set(name, variable_attribute::Uppercase, false);
  }

  fn apply_case(StringView name, String &value) const wontthrow -> void
  {
    let const bits = get_bits(name);
    if ((bits & static_cast<u8>(variable_attribute::Lowercase)) != 0)
      value.lowercase_ascii();
    else if ((bits & static_cast<u8>(variable_attribute::Uppercase)) != 0)
      value.uppercase_ascii();
  }

  template <typename Callback>
  fn for_each_marked(variable_attribute attribute,
                     Callback do_callback) const throws -> void
  {
    m_table->for_each([&](StringView name, const variable_entry &entry) throws {
      if ((entry.attribute_bits & static_cast<u8>(attribute)) != 0)
        do_callback(name);
    });
  }
  template <typename Callback>
  fn for_each_name(Callback do_callback) const throws -> void
  {
    m_table->for_each([&](StringView name, const variable_entry &entry) throws {
      if (entry.attribute_bits != 0) do_callback(name);
    });
  }

  fn entries() const throws -> StringMap<u8>
  {
    let marked = StringMap<u8>{heap_allocator()};
    for_each_name([&](StringView name) throws {
      marked.set(name, get_bits(name));
    });
    return marked;
  }

private:
  VariableTable *m_table;
};

class VariableStore
{
public:
  VariableStore() = default;
  explicit VariableStore(ArrayList<String> positional_params)
      : m_positional_params(steal(positional_params))
  {}

  fn set_field_separators(StringView value) throws -> void
  {
    for (u64 &bits : m_field_separator_bits)
      bits = 0;
    for (usize i = 0; i < value.length; i++) {
      let const byte = static_cast<u8>(value.data[i]);
      m_field_separator_bits[byte >> 6] |= u64{1} << (byte & 63);
    }
    if (value.data != m_field_separators.data()) {
      m_field_separators.clear();
      m_field_separators.append(value);
    }
  }

  pure fn field_separators() const wontthrow -> StringView
  {
    return m_field_separators.view();
  }

  fn shell_variables() wontthrow -> ScalarVariables
  {
    return ScalarVariables{&m_variables};
  }
  pure fn shell_variables() const wontthrow -> const ScalarVariables
  {
    return ScalarVariables{const_cast<VariableTable *>(&m_variables)};
  }
  hot pure fn find_plain_scalar(StringView name) const wontthrow
      -> Maybe<const String *>
  {
    let const entry = m_variables.find(name);
    if (!entry.has_value() || !(*entry)->has_value) return None;
    if (((*entry)->attribute_bits &
         static_cast<u8>(variable_attribute::Nameref)) != 0)
      rarely return None;

    return &(*entry)->value;
  }
  hot pure fn find_variable(StringView name) const wontthrow
      -> Maybe<variable_entry *>
  {
    return m_variables.find(name);
  }
  pure fn is_pipestatus_scalar_possible() const wontthrow -> bool
  {
    return m_is_pipestatus_scalar_possible;
  }
  fn set_pipestatus_scalar_possible(bool is_possible) wontthrow -> void
  {
    m_is_pipestatus_scalar_possible = is_possible;
  }
  pure fn is_locale_scalar_possible() const wontthrow -> bool
  {
    return m_is_locale_scalar_possible;
  }
  fn set_locale_scalar_possible() wontthrow -> void
  {
    m_is_locale_scalar_possible = true;
  }
  fn history_limit(StringView name, usize fallback) const wontthrow -> usize
  {
    let const value = shell_variables().find(name);
    if (!value.has_value()) return fallback;
    let const parsed = value->view().to<i64>();
    if (parsed.is_error() || parsed.value() < 0) return fallback;
    return static_cast<usize>(parsed.value());
  }
  fn special_variable_definition_locations() wontthrow
      -> StringMap<SourceLocation> &
  {
    return m_special_variable_definition_locations;
  }
  pure fn special_variable_definition_locations() const wontthrow
      -> const StringMap<SourceLocation> &
  {
    return m_special_variable_definition_locations;
  }

  hot pure fn is_field_separator(char c) const wontthrow -> bool
  {
    let const byte = static_cast<u8>(c);
    return (m_field_separator_bits[byte >> 6] & (u64{1} << (byte & 63))) != 0;
  }
  hot pure fn has_non_ascii_field_separators() const wontthrow -> bool
  {
    return (m_field_separator_bits[2] | m_field_separator_bits[3]) != 0;
  }

  fn indexed_arrays() wontthrow -> IndexedArrays
  {
    return IndexedArrays{&m_variables};
  }
  pure fn indexed_arrays() const wontthrow -> const IndexedArrays
  {
    return IndexedArrays{const_cast<VariableTable *>(&m_variables)};
  }
  fn associative_arrays() wontthrow -> CompositeKeyArrays &
  {
    return m_associative_arrays;
  }
  pure fn associative_arrays() const wontthrow -> const CompositeKeyArrays &
  {
    return m_associative_arrays;
  }
  fn sparse_arrays() wontthrow -> CompositeKeyArrays &
  {
    return m_sparse_arrays;
  }
  pure fn sparse_arrays() const wontthrow -> const CompositeKeyArrays &
  {
    return m_sparse_arrays;
  }
  fn exported_names() wontthrow -> StringMap<exported_name_value> &
  {
    return m_exported_names;
  }
  pure fn exported_names() const wontthrow
      -> const StringMap<exported_name_value> &
  {
    return m_exported_names;
  }
  fn attributes() wontthrow -> VariableAttributes
  {
    return VariableAttributes{&m_variables};
  }
  pure fn attributes() const wontthrow -> const VariableAttributes
  {
    return VariableAttributes{const_cast<VariableTable *>(&m_variables)};
  }
  fn variables() wontthrow -> VariableTable & { return m_variables; }
  fn positional_params() wontthrow -> ArrayList<String> &
  {
    return m_positional_params;
  }
  pure fn positional_params() const wontthrow -> const ArrayList<String> &
  {
    return m_positional_params;
  }
  fn directory_stack() wontthrow -> ArrayList<String> &
  {
    return m_directory_stack;
  }
  pure fn directory_stack() const wontthrow -> const ArrayList<String> &
  {
    return m_directory_stack;
  }
  fn bash_arguments() const wontthrow -> BashArgumentStack &
  {
    return m_bash_arguments;
  }
  fn disabled_bash_special_arrays() wontthrow -> u8 &
  {
    return m_disabled_bash_special_arrays;
  }
  pure fn disabled_bash_special_arrays() const wontthrow -> u8
  {
    return m_disabled_bash_special_arrays;
  }
  fn unset_dynamic_readers() wontthrow -> u8 &
  {
    return m_unset_dynamic_readers;
  }
  pure fn unset_dynamic_readers() const wontthrow -> u8
  {
    return m_unset_dynamic_readers;
  }

  fn snapshot() const throws -> variable_snapshot;
  fn restore(variable_snapshot snapshot) throws -> void;
  fn append_wire(String &output) const throws -> void;
  static fn from_wire(subshell_bootstrap_reader &reader,
                      variable_wire &wire) throws -> bool;
  fn apply_wire_masks(const variable_wire &wire) wontthrow -> void
  {
    m_disabled_bash_special_arrays = wire.disabled_bash_special_arrays;
    m_unset_dynamic_readers = wire.unset_dynamic_readers;
  }
  fn apply_wire(variable_wire wire) wontthrow -> void
  {
    m_bash_arguments.reset();
    if (wire.has_bash_argument_arrays)
      m_bash_arguments.activate(steal(wire.bash_argument_values),
                                steal(wire.bash_argument_frame_counts));
    if (wire.has_bash_argument_context)
      m_bash_arguments.adopt_inherited_context(
          wire.bash_argument_context_flags,
          steal(wire.bash_argument_source_path));
  }

private:
  String m_field_separators{" \t\n"};
  u64 m_field_separator_bits[4]{(u64{1} << ' ') | (u64{1} << '\t') |
                                (u64{1} << '\n')};
  VariableTable m_variables;
  StringMap<SourceLocation> m_special_variable_definition_locations{
      heap_allocator()};
  CompositeKeyArrays m_associative_arrays;
  CompositeKeyArrays m_sparse_arrays;
  StringMap<exported_name_value> m_exported_names{heap_allocator()};
  ArrayList<String> m_positional_params{heap_allocator()};
  ArrayList<String> m_directory_stack{heap_allocator()};
  mutable BashArgumentStack m_bash_arguments;
  u8 m_disabled_bash_special_arrays{0};
  u8 m_unset_dynamic_readers{0};
  bool m_is_pipestatus_scalar_possible{true};
  bool m_is_locale_scalar_possible{false};
};

class CompletionStore
{
public:
  fn register_spec(StringView command, completion_spec spec) throws -> void
  {
    m_specs.set(command, steal(spec));
  }
  fn register_slot_spec(completion_spec_slot slot, completion_spec spec) throws
      -> void
  {
    get_slot(slot) = steal(spec);
  }
  pure fn lookup_spec(StringView command) const wontthrow
      -> const completion_spec *
  {
    return m_specs.find(command).value_or(nullptr);
  }
  fn lookup_spec(StringView command) wontthrow -> completion_spec *
  {
    return m_specs.find(command).value_or(nullptr);
  }
  pure fn get_slot_spec(completion_spec_slot slot) const wontthrow
      -> const completion_spec *
  {
    let const &spec = get_slot(slot);
    return spec.has_value() ? &*spec : nullptr;
  }
  fn get_slot_spec(completion_spec_slot slot) wontthrow -> completion_spec *
  {
    let &spec = get_slot(slot);
    return spec.has_value() ? &*spec : nullptr;
  }
  fn remove_spec(StringView command) throws -> bool
  {
    if (!m_specs.find(command).has_value()) return false;

    m_specs.erase(command);
    return true;
  }
  fn remove_slot_spec(completion_spec_slot slot) wontthrow -> bool
  {
    let &spec = get_slot(slot);
    if (!spec.has_value()) return false;

    spec.reset();
    return true;
  }
  fn remove_all_specs() wontthrow -> void
  {
    m_specs.clear();
    m_default_spec.reset();
    m_empty_spec.reset();
    m_initial_spec.reset();
  }
  pure fn specs() const wontthrow -> const StringMap<completion_spec> &
  {
    return m_specs;
  }

  fn snapshot() const throws -> completion_snapshot
  {
    return completion_snapshot{m_specs, m_default_spec, m_empty_spec,
                               m_initial_spec};
  }
  fn restore(completion_snapshot snapshot) wontthrow -> void
  {
    m_specs = steal(snapshot.specs);
    m_default_spec = steal(snapshot.default_spec);
    m_empty_spec = steal(snapshot.empty_spec);
    m_initial_spec = steal(snapshot.initial_spec);
  }
  fn append_wire(String &output) const throws -> void;
  static fn from_wire(subshell_bootstrap_reader &reader,
                      completion_snapshot &wire) throws -> bool;

private:
  fn get_slot(completion_spec_slot slot) wontthrow -> Maybe<completion_spec> &
  {
    switch (slot) {
    case completion_spec_slot::Default: return m_default_spec;
    case completion_spec_slot::Empty: return m_empty_spec;
    case completion_spec_slot::Initial: return m_initial_spec;
    }
    unreachable();
  }
  pure fn get_slot(completion_spec_slot slot) const wontthrow
      -> const Maybe<completion_spec> &
  {
    switch (slot) {
    case completion_spec_slot::Default: return m_default_spec;
    case completion_spec_slot::Empty: return m_empty_spec;
    case completion_spec_slot::Initial: return m_initial_spec;
    }
    unreachable();
  }

  StringMap<completion_spec> m_specs{heap_allocator()};
  Maybe<completion_spec> m_default_spec{};
  Maybe<completion_spec> m_empty_spec{};
  Maybe<completion_spec> m_initial_spec{};
};

struct function_call_frame
{
  String name;
  FunctionBodyHandle storage;
  const String *source;
  SourceLocation location;
  bool was_printed{false};
};

class FunctionStore
{
public:
  template <typename Callback>
  fn for_each_name(Callback do_callback) const throws -> void
  {
    m_definitions.for_each([&](StringView name, const FunctionBodyHandle &)
                               throws { do_callback(name); });
  }
  fn find_source(StringView name) const wontthrow -> const String *
  {
    let const storage = m_definitions.find(name);
    return storage.has_value() ? storage->get_source() : nullptr;
  }
  mustuse fn sorted_names() const throws
      -> SortedArrayList<String, order_comparator<String>>
  {
    let out = ArrayList<String>{heap_allocator()};
    out.reserve(m_definitions.count());
    for_each_name([&](StringView name) { out.push_managed(name); });
    return steal(out).make_sorted(sort_order::ascending);
  }
  fn find_function(StringView name) const wontthrow -> Maybe<const Expression *>
  {
    let const storage = m_definitions.find(name);
    return storage.has_value() ? Maybe<const Expression *>{storage->get_body()}
                               : None;
  }
  pure fn find_storage(StringView name) const wontthrow
      -> const FunctionBodyHandle *
  {
    return m_definitions.find(name).value_or(nullptr);
  }
  pure fn has_functions() const wontthrow -> bool
  {
    return m_definitions.count() != 0;
  }
  pure fn is_readonly(StringView name) const wontthrow -> bool
  {
    return m_readonly.contains(name);
  }
  mustuse fn sorted_readonly_names() const throws
      -> SortedArrayList<String, order_comparator<String>>
  {
    let out = ArrayList<String>{heap_allocator()};
    out.reserve(m_readonly.count());
    m_readonly.for_each([&](StringView name) {
      if (find_function(name).has_value()) out.push_managed(name);
    });
    return steal(out).make_sorted(sort_order::ascending);
  }
  fn names() const throws -> HashSet
  {
    let names = HashSet{heap_allocator()};
    for_each_name([&](StringView name) { names.add(name); });
    return names;
  }
  fn definitions() wontthrow -> StringMap<FunctionBodyHandle> &
  {
    return m_definitions;
  }
  pure fn definitions() const wontthrow -> const StringMap<FunctionBodyHandle> &
  {
    return m_definitions;
  }
  fn readonly() wontthrow -> HashSet & { return m_readonly; }
  pure fn readonly() const wontthrow -> const HashSet & { return m_readonly; }
  fn call_depth() wontthrow -> usize & { return m_call_depth; }
  pure fn call_depth() const wontthrow -> const usize & { return m_call_depth; }
  fn call_frames() wontthrow -> ArrayList<function_call_frame> &
  {
    return m_call_frames;
  }
  pure fn call_frames() const wontthrow
      -> const ArrayList<function_call_frame> &
  {
    return m_call_frames;
  }

  fn snapshot() const throws -> StringMap<FunctionBodyHandle>
  {
    return m_definitions;
  }
  fn restore(StringMap<FunctionBodyHandle> definitions) wontthrow -> void
  {
    m_definitions = steal(definitions);
  }
  fn append_wire(String &output) const throws -> void;
  static fn from_wire(subshell_bootstrap_reader &reader,
                      function_wire &wire) throws -> bool;
  fn apply_wire_depth(const function_wire &wire) wontthrow -> void
  {
    m_call_depth = wire.call_depth;
  }
  fn apply_wire_definitions(function_wire &wire) throws -> void;

private:
  StringMap<FunctionBodyHandle> m_definitions{heap_allocator()};
  HashSet m_readonly{heap_allocator()};
  usize m_call_depth{0};
  ArrayList<function_call_frame> m_call_frames{heap_allocator()};
};

class TrapStore
{
public:
  fn set(StringView condition, trap_definition definition) throws -> void
  {
    m_traps.set(condition, steal(definition));
    m_held_exit_action = None;
    refresh_flags();
  }
  fn set_action(StringView condition, StringView action) throws -> void
  {
    set(condition, trap_definition{
                       String{heap_allocator(), action},
                       String{heap_allocator()},
                       SourceLocation{},
                       0
    });
  }
  fn reset(StringView condition) throws -> void
  {
    m_traps.erase(condition);
    m_held_exit_action = None;
    refresh_flags();
  }
  fn hold_exit_trap_for_listing() throws -> void
  {
    let const found = m_traps.find(StringView{"EXIT", 4});
    if (!found.has_value()) return;

    let held = String{heap_allocator(), found.value()->action_text.view()};
    m_traps.erase(StringView{"EXIT", 4});
    refresh_flags();
    m_held_exit_action = steal(held);
  }
  pure fn held_exit_action() const wontthrow -> const Maybe<String> &
  {
    return m_held_exit_action;
  }
  pure fn find(StringView condition) const wontthrow
      -> Maybe<const trap_definition *>
  {
    return m_traps.find(condition);
  }
  pure fn find_active(StringView condition) const wontthrow
      -> Maybe<const trap_definition *>
  {
    let const found = m_traps.find(condition);
    if (found.has_value() && found.value()->action_text.count() > 0) {
      return found;
    }

    return None;
  }
  pure fn find_definition(StringView condition) const wontthrow
      -> Maybe<trap_definition>
  {
    let const found = m_traps.find(condition);
    if (!found.has_value() || !found.value()->has_location) {
      return None;
    }

    try {
      return trap_definition{*found.value()};
    } catch (...) {
      return None;
    }
  }
  pure fn count() const wontthrow -> usize { return m_traps.count(); }
  template <class Fn>
  fn list(Fn callback) const throws -> void
  {
    m_traps.for_each(callback);
  }
  fn discard_signal_traps() throws -> void
  {
    ArrayList<String> discarded{heap_allocator()};
    m_traps.for_each([&](StringView condition, const trap_definition &trap) {
      unused(trap);
      if (!os::signal_number_from_name(condition).has_value()) return;
      discarded.push(String{heap_allocator(), condition});
    });

    for (let const &condition : discarded)
      m_traps.erase(condition.view());

    m_held_exit_action = None;
    refresh_flags();
  }
  fn snapshot() const throws -> trap_snapshot
  {
    return trap_snapshot{m_traps, m_install_state};
  }
  fn restore(trap_snapshot snapshot) throws -> void
  {
    m_traps = steal(snapshot.traps);
    m_install_state = snapshot.install;
    m_held_exit_action = None;
    refresh_flags();
  }
  fn append_wire(String &output) const throws -> void;
  static fn from_wire(subshell_bootstrap_reader &reader,
                      u64 &startup_ignored_signals) wontthrow -> bool;
  fn cached_bodies() wontthrow -> StringMap<FunctionBodyHandle> &
  {
    return m_cached_bodies;
  }
  pure fn cached_bodies() const wontthrow
      -> const StringMap<FunctionBodyHandle> &
  {
    return m_cached_bodies;
  }

  pure fn has_debug_trap() const wontthrow -> bool { return m_has_debug_trap; }
  pure fn has_err_trap() const wontthrow -> bool { return m_has_err_trap; }
  fn debug_trap_active_depth() wontthrow -> usize &
  {
    return m_install_state.debug_active_depth;
  }
  pure fn debug_trap_active_depth() const wontthrow -> usize
  {
    return m_install_state.debug_active_depth;
  }
  fn err_trap_active_depth() wontthrow -> usize &
  {
    return m_install_state.err_active_depth;
  }
  pure fn err_trap_active_depth() const wontthrow -> usize
  {
    return m_install_state.err_active_depth;
  }
  fn is_replaying_inherited_state() wontthrow -> bool &
  {
    return m_is_replaying_inherited_state;
  }
  pure fn is_replaying_inherited_state() const wontthrow -> bool
  {
    return m_is_replaying_inherited_state;
  }
  fn exit_trap_ran() wontthrow -> bool & { return m_exit_trap_ran; }
  pure fn exit_trap_ran() const wontthrow -> bool { return m_exit_trap_ran; }
  fn did_reset_inherited_signal_traps() wontthrow -> bool &
  {
    return m_install_state.did_reset_inherited_signal_traps;
  }
  pure fn did_reset_inherited_signal_traps() const wontthrow -> bool
  {
    return m_install_state.did_reset_inherited_signal_traps;
  }
  fn startup_ignored_signals() wontthrow -> u64 &
  {
    return m_startup_ignored_signals;
  }
  pure fn startup_ignored_signals() const wontthrow -> u64
  {
    return m_startup_ignored_signals;
  }
  fn pending_child_trap_count() wontthrow -> u32 &
  {
    return m_pending_child_trap_count;
  }
  pure fn pending_child_trap_count() const wontthrow -> u32
  {
    return m_pending_child_trap_count;
  }
  pure fn trap_action_depth() const wontthrow -> u32
  {
    return m_action_frame.depth;
  }
  pure fn action_frame() const wontthrow -> const trap_action_frame &
  {
    return m_action_frame;
  }
  pure fn is_condition_running(u8 condition_bit) const wontthrow -> bool
  {
    return (m_action_frame.running_conditions & condition_bit) != 0;
  }
  fn mark_condition_running(u8 condition_bit) wontthrow -> void
  {
    m_action_frame.running_conditions |= condition_bit;
  }
  fn unmark_condition_running(u8 condition_bit) wontthrow -> void
  {
    m_action_frame.running_conditions &= static_cast<u8>(~condition_bit);
  }
  fn last_trap_action_status() wontthrow -> i32 &
  {
    return m_last_trap_action_status;
  }
  pure fn last_trap_action_status() const wontthrow -> i32
  {
    return m_last_trap_action_status;
  }
  fn status_before_return() wontthrow -> i32 &
  {
    return m_status_before_return;
  }
  pure fn status_before_return() const wontthrow -> i32
  {
    return m_status_before_return;
  }

private:
  friend class TrapActionScope;

  mustuse fn enter_action(Maybe<i32> saved_exit_status) wontthrow
      -> trap_action_frame
  {
    let previous = m_action_frame;
    m_action_frame.depth += 1;
    m_action_frame.saved_exit_status = saved_exit_status;
    return previous;
  }
  mustuse fn enter_condition_action(u8 condition_bit, usize trigger_line_number,
                                    usize source_frame_count,
                                    usize function_depth,
                                    i32 saved_exit_status) wontthrow
      -> trap_action_frame
  {
    let previous = enter_action(saved_exit_status);
    m_action_frame.running_conditions |= condition_bit;
    m_action_frame.trigger_line_number = trigger_line_number;
    m_action_frame.source_frame_count = source_frame_count;
    m_action_frame.function_depth = function_depth;
    return previous;
  }
  mustuse fn leave_action_for_subshell() wontthrow -> trap_action_frame
  {
    let previous = m_action_frame;
    m_action_frame.depth = 0;
    m_action_frame.saved_exit_status = None;
    m_action_frame.running_conditions = 0;
    return previous;
  }
  fn restore_action_frame(const trap_action_frame &previous) wontthrow -> void
  {
    m_action_frame = previous;
  }
  fn refresh_flags() wontthrow -> void
  {
    m_has_debug_trap = m_traps.find(StringView{"DEBUG", 5}).has_value();
    m_has_err_trap = m_traps.find(StringView{"ERR", 3}).has_value();

    let const child_action = find_active(StringView{"CHLD", 4});
    let const arming = child_action.has_value()
                           ? os::child_trap_arming::Armed
                           : os::child_trap_arming::Disarmed;
    os::set_child_trap_armed(arming);
  }

  bool m_has_debug_trap{false};
  bool m_has_err_trap{false};
  trap_install_state m_install_state{};
  bool m_is_replaying_inherited_state{false};
  bool m_exit_trap_ran{false};
  u64 m_startup_ignored_signals{0};
  u32 m_pending_child_trap_count{0};
  trap_action_frame m_action_frame{};
  i32 m_last_trap_action_status{0};
  i32 m_status_before_return{0};
  StringMap<trap_definition> m_traps{heap_allocator()};
  Maybe<String> m_held_exit_action;
  StringMap<FunctionBodyHandle> m_cached_bodies{heap_allocator()};
};

class TrapActionScope
{
public:
  mustuse static fn enter(TrapStore &store,
                          Maybe<i32> saved_exit_status) wontthrow
      -> TrapActionScope
  {
    return TrapActionScope{store, store.enter_action(saved_exit_status)};
  }
  mustuse static fn
  enter_condition(TrapStore &store, u8 condition_bit, usize trigger_line_number,
                  usize source_frame_count, usize function_depth,
                  i32 saved_exit_status) wontthrow -> TrapActionScope
  {
    return TrapActionScope{
        store, store.enter_condition_action(condition_bit, trigger_line_number,
                                            source_frame_count, function_depth,
                                            saved_exit_status)};
  }
  mustuse static fn leave_for_subshell(TrapStore &store) wontthrow
      -> TrapActionScope
  {
    return TrapActionScope{store, store.leave_action_for_subshell()};
  }

  TrapActionScope(const TrapActionScope &) = delete;
  fn operator=(const TrapActionScope &)->TrapActionScope & = delete;
  ~TrapActionScope() { m_store.restore_action_frame(m_previous); }

private:
  TrapActionScope(TrapStore &store, const trap_action_frame &previous)
      : m_store(store), m_previous(previous)
  {}

  TrapStore &m_store;
  trap_action_frame m_previous;
};

class ExpansionStore
{
public:
  fn scratch_arena() const wontthrow -> BumpArena & { return m_scratch_arena; }
  fn scratch_allocator() const wontthrow -> Allocator
  {
    return bump_allocator(m_scratch_arena);
  }
  fn find_cached_regex(StringView key) wontthrow -> CompiledRegex *
  {
    return m_regex_cache.find(key).value_or(nullptr);
  }
  fn clear_regex_cache() wontthrow -> void { m_regex_cache.clear(); }
  fn store_regex(StringView key, CompiledRegex regex) throws -> CompiledRegex *
  {
    return m_regex_cache.set(key, steal(regex));
  }
  fn substitution_depth() wontthrow -> usize & { return m_substitution_depth; }
  pure fn substitution_depth() const wontthrow -> usize
  {
    return m_substitution_depth;
  }
  fn parameter_expansion_depth() wontthrow -> usize &
  {
    return m_parameter_expansion_depth;
  }
  pure fn parameter_expansion_depth() const wontthrow -> usize
  {
    return m_parameter_expansion_depth;
  }
  fn set_glob_exempt_for_test(bool enabled) wontthrow
  {
    m_glob_exempt_for_test = enabled;
  }
  pure fn glob_exempt_for_test() const wontthrow -> bool
  {
    return m_glob_exempt_for_test;
  }
  fn is_expanding_here_document() wontthrow -> bool &
  {
    return m_is_expanding_here_document;
  }
  fn is_expanding_assignment_value() wontthrow -> bool &
  {
    return m_is_expanding_assignment_value;
  }
  fn is_expanding_single_string() wontthrow -> bool &
  {
    return m_is_expanding_single_string;
  }
  pure fn is_expanding_assignment_value() const wontthrow -> bool
  {
    return m_is_expanding_assignment_value;
  }
  pure fn is_expanding_single_string() const wontthrow -> bool
  {
    return m_is_expanding_single_string;
  }
  pure fn is_expanding_here_document() const wontthrow -> bool
  {
    return m_is_expanding_here_document;
  }
  fn pending_process_substitutions() wontthrow
      -> ArrayList<process_substitution> &
  {
    return m_pending_process_substitutions;
  }
  pure fn pending_process_substitutions() const wontthrow
      -> const ArrayList<process_substitution> &
  {
    return m_pending_process_substitutions;
  }
  fn held_process_substitutions() wontthrow -> ArrayList<process_substitution> &
  {
    return m_held_process_substitutions;
  }
  pure fn held_process_substitutions() const wontthrow
      -> const ArrayList<process_substitution> &
  {
    return m_held_process_substitutions;
  }
  fn loop_redirect_fds() wontthrow -> ArrayList<loop_redirect_fd> &
  {
    return m_loop_redirect_fds;
  }
  pure fn loop_redirect_fds() const wontthrow
      -> const ArrayList<loop_redirect_fd> &
  {
    return m_loop_redirect_fds;
  }
  fn get_getopts_cursor() wontthrow -> getopts_cursor &
  {
    return m_getopts_cursor;
  }
  pure fn get_getopts_cursor() const wontthrow -> const getopts_cursor &
  {
    return m_getopts_cursor;
  }
  fn set_getopts_cursor(const getopts_cursor &cursor) wontthrow -> void
  {
    m_getopts_cursor = cursor;
  }
  fn regex_cache() wontthrow -> StringMap<CompiledRegex> &
  {
    return m_regex_cache;
  }
  pure fn regex_cache() const wontthrow -> const StringMap<CompiledRegex> &
  {
    return m_regex_cache;
  }

private:
  mutable BumpArena m_scratch_arena{};
  usize m_substitution_depth{0};
  usize m_parameter_expansion_depth{0};
  getopts_cursor m_getopts_cursor{};
  bool m_glob_exempt_for_test{false};
  bool m_is_expanding_here_document{false};
  bool m_is_expanding_assignment_value{false};
  bool m_is_expanding_single_string{false};
  ArrayList<process_substitution> m_pending_process_substitutions{
      heap_allocator()};
  ArrayList<process_substitution> m_held_process_substitutions{
      heap_allocator()};
  ArrayList<loop_redirect_fd> m_loop_redirect_fds{heap_allocator()};
  StringMap<CompiledRegex> m_regex_cache{heap_allocator()};
};

struct history_recording_mark
{
  const Expression *root{nullptr};
  StringView source{};
};

class HistoryRecorder
{
public:
  fn set_event_number(Maybe<usize> number) wontthrow -> void
  {
    m_event_number = steal(number);
  }
  pure fn get_event_number() const wontthrow -> Maybe<usize>
  {
    return m_event_number;
  }
  fn begin_transaction(ArrayList<String> &commands) throws -> void
  {
    m_transaction_stack.push(&commands);
  }
  fn end_transaction() wontthrow -> void
  {
    ASSERT(!m_transaction_stack.is_empty());
    m_transaction_stack.pop_back();
  }
  pure fn has_transaction() const wontthrow -> bool
  {
    return !m_transaction_stack.is_empty();
  }
  fn append_to_transaction(StringView command) throws -> void
  {
    ASSERT(!m_transaction_stack.is_empty());
    m_transaction_stack.back()->push(String{heap_allocator(), command});
  }
  pure fn get_mark() const wontthrow -> history_recording_mark
  {
    return m_mark;
  }
  fn begin_recording(const Expression *root, StringView source) wontthrow
      -> void
  {
    m_mark = history_recording_mark{root, source};
  }
  fn restore_mark(history_recording_mark mark) wontthrow -> void
  {
    m_mark = mark;
  }
  pure fn find_source_for(const Expression *root) const wontthrow
      -> Maybe<StringView>
  {
    if (root != m_mark.root) return None;
    return m_mark.source;
  }

private:
  Maybe<usize> m_event_number{None};
  history_recording_mark m_mark{};
  ArrayList<ArrayList<String> *> m_transaction_stack{heap_allocator()};
};

struct source_retention_mark
{
  usize ast_count{0};
  usize source_count{0};
};

class SourceRetention
{
public:
  pure fn get_mark() const wontthrow -> source_retention_mark
  {
    return source_retention_mark{m_asts.count(), m_sources.count()};
  }
  pure fn count() const wontthrow -> usize { return m_sources.count(); }
  fn reserve_one_more() throws -> void
  {
    m_asts.reserve(m_asts.count() + 1);
    m_sources.reserve(m_sources.count() + 1);
  }
  fn retain(String *source, Expression *ast) throws -> void
  {
    m_sources.push(source);
    m_asts.push(ast);
  }
  pure fn owns(const String *source) const wontthrow -> bool
  {
    for (let const *retained : m_sources) {
      if (retained == source) return true;
    }

    return false;
  }
  pure fn generation_of(const String *source) const wontthrow -> u64
  {
    return owns(source) ? m_generation : EXTERNAL_SOURCE_GENERATION;
  }
  pure fn get_generation() const wontthrow -> u64 { return m_generation; }
  fn set_generation(u64 generation) wontthrow -> void
  {
    m_generation = generation;
  }
  pure fn is_stale_generation(u64 generation) const wontthrow -> bool
  {
    return generation != EXTERNAL_SOURCE_GENERATION &&
           generation != m_generation;
  }
  fn release_to(source_retention_mark mark) wontthrow -> void;
  fn clear() wontthrow -> void;

private:
  fn free_sources_from(usize first) wontthrow -> void;

  ArrayList<Expression *> m_asts{heap_allocator()};
  ArrayList<String *> m_sources{heap_allocator()};
  u64 m_generation{0};
};

class SourceStore
{
public:
  fn set_current_source(const String *source, String origin,
                        u64 source_generation) wontthrow -> void
  {
    m_current_source = source;
    m_current_source_generation = source_generation;
    m_current_origin = steal(origin);
  }
  fn set_current_location(SourceLocation location) wontthrow -> void
  {
    m_current_location = location;
  }
  fn set_source_depth(usize depth) wontthrow -> void { m_source_depth = depth; }
  pure fn source_depth() const wontthrow -> usize { return m_source_depth; }
  fn set_rejected_return_source_frames(usize count) wontthrow -> void
  {
    m_rejected_return_source_frames = count;
  }
  pure fn rejected_return_source_frames() const wontthrow -> usize
  {
    return m_rejected_return_source_frames;
  }
  fn set_function_substitution_depth(usize depth) wontthrow -> void
  {
    m_function_substitution_depth = depth;
  }
  pure fn function_substitution_depth() const wontthrow -> usize
  {
    return m_function_substitution_depth;
  }
  fn set_script_run(bool is_script_run) wontthrow -> void
  {
    m_is_script_run = is_script_run;
  }
  pure fn is_script_run() const wontthrow -> bool { return m_is_script_run; }
  fn mimicry_depth() wontthrow -> usize & { return m_mimicry_depth; }
  pure fn mimicry_depth() const wontthrow -> usize { return m_mimicry_depth; }
  fn set_mimicry_depth(usize depth) wontthrow -> void
  {
    m_mimicry_depth = depth;
  }
  fn current_source() wontthrow -> const String *& { return m_current_source; }
  pure fn current_source() const wontthrow -> const String *
  {
    return m_current_source;
  }
  pure fn current_source_view() const wontthrow -> StringView
  {
    return m_current_source != nullptr ? m_current_source->view()
                                       : StringView{};
  }
  fn push_embedded_source(embedded_source source) throws -> void
  {
    m_embedded_sources.push(steal(source));
  }
  fn pop_embedded_source() wontthrow -> void { m_embedded_sources.pop_back(); }
  fn set_embedded_depth_floor(usize floor) wontthrow -> void
  {
    m_embedded_sources.back().call_depth_floor = floor;
  }
  pure fn embedded_sources() const wontthrow
      -> const ArrayList<embedded_source> &
  {
    return m_embedded_sources;
  }
  fn push_line_base(const String *source, usize preceding_line_count) throws
      -> void
  {
    m_line_bases.push(source_line_base{source, preceding_line_count});
  }
  pure fn preceding_line_count_of(const String *source) const wontthrow -> usize
  {
    for (let const &base : m_line_bases) {
      if (base.source == source) return base.preceding_line_count;
    }

    return 0;
  }
  fn current_origin() wontthrow -> String & { return m_current_origin; }
  pure fn current_origin() const wontthrow -> const String &
  {
    return m_current_origin;
  }
  fn current_location() wontthrow -> SourceLocation &
  {
    return m_current_location;
  }
  pure fn current_location() const wontthrow -> const SourceLocation &
  {
    return m_current_location;
  }
  fn source_frames() wontthrow -> ArrayList<source_frame> &
  {
    return m_source_frames;
  }
  pure fn source_frames() const wontthrow -> const ArrayList<source_frame> &
  {
    return m_source_frames;
  }
  fn current_source_generation() wontthrow -> u64 &
  {
    return m_current_source_generation;
  }
  pure fn current_source_generation() const wontthrow -> u64
  {
    return m_current_source_generation;
  }

private:
  const String *m_current_source{nullptr};
  String m_current_origin{heap_allocator()};
  SourceLocation m_current_location{};
  bool m_is_script_run{false};
  ArrayList<source_frame> m_source_frames{heap_allocator()};
  ArrayList<embedded_source> m_embedded_sources{heap_allocator()};
  ArrayList<source_line_base> m_line_bases{heap_allocator()};
  u64 m_current_source_generation{EXTERNAL_SOURCE_GENERATION};
  usize m_source_depth{0};
  usize m_rejected_return_source_frames{0};
  usize m_function_substitution_depth{0};
  usize m_mimicry_depth{0};
};

class ArenaStore
{
public:
  fn set_parse_arena(BumpArena *arena) wontthrow -> void
  {
    m_parse_arena = arena;
  }
  pure fn parse_arena() const wontthrow -> BumpArena * { return m_parse_arena; }
  fn set_function_arena(BumpArena *arena) wontthrow -> void
  {
    m_function_arena = arena;
  }
  pure fn function_arena() const wontthrow -> BumpArena *
  {
    return m_function_arena;
  }

private:
  BumpArena *m_parse_arena{nullptr};
  BumpArena *m_function_arena{nullptr};
};

class SubshellStore
{
public:
  fn coprocess() wontthrow -> coprocess_descriptors & { return m_coprocess; }
  pure fn coprocess() const wontthrow -> const coprocess_descriptors &
  {
    return m_coprocess;
  }
  fn saved_descriptors() wontthrow -> ArrayList<subshell_saved_descriptor> &
  {
    return m_saved_descriptors;
  }

private:
  coprocess_descriptors m_coprocess{};
  ArrayList<subshell_saved_descriptor> m_saved_descriptors{heap_allocator()};
};

class EnvironmentStore
{
public:
  fn environment_undo_log() wontthrow -> ArrayList<environment_undo_entry> &
  {
    return m_environment_undo_log;
  }
  fn confined_write_log() wontthrow -> ArrayList<environment_undo_entry> &
  {
    return m_confined_write_log;
  }
  fn confined_write_depth() wontthrow -> usize &
  {
    return m_confined_write_depth;
  }
  pure fn confined_write_depth() const wontthrow -> usize
  {
    return m_confined_write_depth;
  }
  fn confined_clock() wontthrow -> dynamic_clock_state &
  {
    return m_confined_clock;
  }
  pure fn confined_clock() const wontthrow -> const dynamic_clock_state &
  {
    return m_confined_clock;
  }
  fn was_confined_ignoreeof_enabled() wontthrow -> bool &
  {
    return m_was_confined_ignoreeof_enabled;
  }
  pure fn was_confined_ignoreeof_enabled() const wontthrow -> bool
  {
    return m_was_confined_ignoreeof_enabled;
  }

  pure fn snapshot() const wontthrow -> usize
  {
    return m_environment_undo_log.count();
  }
  fn restore(usize undo_mark) wontthrow -> void;

private:
  usize m_confined_write_depth{0};
  dynamic_clock_state m_confined_clock{};
  bool m_was_confined_ignoreeof_enabled{false};
  ArrayList<environment_undo_entry> m_environment_undo_log{heap_allocator()};
  ArrayList<environment_undo_entry> m_confined_write_log{heap_allocator()};
};

class StartupStore
{
public:
  pure fn is_login_shell() const wontthrow -> bool { return m_is_login_shell; }
  fn set_login_shell(bool enabled) wontthrow -> void
  {
    m_is_login_shell = enabled;
  }
  pure fn has_custom_rcfile() const wontthrow -> bool
  {
    return m_has_custom_rcfile;
  }
  fn set_custom_rcfile(bool enabled) wontthrow -> void
  {
    m_has_custom_rcfile = enabled;
  }
  pure fn startup_finished() const wontthrow -> bool
  {
    return m_startup_finished;
  }
  fn mark_startup_finished() wontthrow -> void { m_startup_finished = true; }
  pure fn is_restricted_shell() const wontthrow -> bool
  {
    return m_is_restricted_shell;
  }
  fn request_restricted_shell() wontthrow -> void
  {
    m_is_restricted_shell = true;
  }
  fn set_restricted_shell(bool enabled) wontthrow -> void
  {
    m_is_restricted_shell = enabled;
  }
  pure fn get_init_moods() const wontthrow -> StringView
  {
    return m_init_moods.view();
  }
  fn set_init_moods(StringView moods) throws -> void
  {
    m_init_moods = String{moods};
  }
  fn append_wire(String &output) const throws -> void;
  static fn from_wire(subshell_bootstrap_reader &reader,
                      startup_wire &wire) throws -> bool;

private:
  String m_init_moods{heap_allocator()};
  bool m_is_login_shell{false};
  bool m_has_custom_rcfile{false};
  bool m_is_restricted_shell{false};
  bool m_startup_finished{false};
};

class DiagnosticsStore
{
public:
  DiagnosticsStore() = default;
  DiagnosticsStore(const DiagnosticsStore &) = delete;
  DiagnosticsStore &operator=(const DiagnosticsStore &) = delete;
  ~DiagnosticsStore();

  fn reset_runtime_highlight_cache() wontthrow -> void;
  fn source_traces_enabled() const wontthrow -> bool
  {
    return m_source_traces_enabled;
  }
  fn set_source_traces_enabled(bool enabled) wontthrow -> void
  {
    m_source_traces_enabled = enabled;
  }
  fn append_wire(String &output) const throws -> void;
  static fn from_wire(subshell_bootstrap_reader &reader,
                      bool &is_source_traces_enabled) wontthrow -> bool;
  fn diagnostic_highlight_cache() wontthrow
      -> completion::shell_highlight_cache *&
  {
    return m_diagnostic_highlight_cache;
  }
  fn runtime_diagnostic_highlight_cache() wontthrow
      -> completion::shell_highlight_cache *&
  {
    return m_runtime_diagnostic_highlight_cache;
  }

private:
  bool m_source_traces_enabled{true};
  completion::shell_highlight_cache *m_diagnostic_highlight_cache{nullptr};
  completion::shell_highlight_cache *m_runtime_diagnostic_highlight_cache{
      nullptr};
};

class DynamicRuntimeStore
{
public:
  fn shell_start_time() wontthrow -> i64 & { return m_clock.shell_start_time; }
  pure fn shell_start_time() const wontthrow -> const i64 &
  {
    return m_clock.shell_start_time;
  }
  fn seconds_base() wontthrow -> i64 & { return m_clock.seconds_base; }
  pure fn seconds_base() const wontthrow -> const i64 &
  {
    return m_clock.seconds_base;
  }
  fn random_state() const wontthrow -> u64 & { return m_clock.random_state; }
  fn is_random_reseed_pending() const wontthrow -> bool &
  {
    return m_clock.is_random_reseed_pending;
  }
  fn get_random_reseed_count() const wontthrow -> u64 &
  {
    return m_random_reseed_count;
  }
  pure fn get_clock() const wontthrow -> const dynamic_clock_state &
  {
    return m_clock;
  }
  fn set_clock(const dynamic_clock_state &clock) wontthrow -> void
  {
    m_clock = clock;
  }
#if !defined NDEBUG
  fn debug_variable_name_enumeration_count() const wontthrow -> usize &
  {
    return m_debug_variable_name_enumeration_count;
  }
#endif

private:
  mutable dynamic_clock_state m_clock{};
  mutable u64 m_random_reseed_count{0};
#if !defined NDEBUG
  mutable usize m_debug_variable_name_enumeration_count{0};
#endif
};

class EvalContextState
{
public:
  EvalContextState(ArrayList<String> positional_params,
                   bool shell_is_interactive, String shell_name)
      : m_variable_store(steal(positional_params)),
        m_execution_store(shell_is_interactive, steal(shell_name))
  {}

  fn scratch_allocator() const wontthrow -> Allocator
  {
    return expansion_store().scratch_allocator();
  }
  fn arena_store() wontthrow -> ArenaStore & { return m_arena_store; }
  pure fn arena_store() const wontthrow -> const ArenaStore &
  {
    return m_arena_store;
  }
  fn subshell_store() wontthrow -> SubshellStore & { return m_subshell_store; }
  pure fn subshell_store() const wontthrow -> const SubshellStore &
  {
    return m_subshell_store;
  }
  fn environment_store() wontthrow -> EnvironmentStore &
  {
    return m_environment_store;
  }
  pure fn environment_store() const wontthrow -> const EnvironmentStore &
  {
    return m_environment_store;
  }
  fn startup_store() wontthrow -> StartupStore & { return m_startup_store; }
  pure fn startup_store() const wontthrow -> const StartupStore &
  {
    return m_startup_store;
  }
  fn diagnostics_store() wontthrow -> DiagnosticsStore &
  {
    return m_diagnostics_store;
  }
  pure fn diagnostics_store() const wontthrow -> const DiagnosticsStore &
  {
    return m_diagnostics_store;
  }
  fn program_resolver() wontthrow -> ProgramResolver &
  {
    return m_program_resolver;
  }
  pure fn program_resolver() const wontthrow -> const ProgramResolver &
  {
    return m_program_resolver;
  }
  fn dynamic_runtime_store() wontthrow -> DynamicRuntimeStore &
  {
    return m_dynamic_runtime_store;
  }
  pure fn dynamic_runtime_store() const wontthrow -> const DynamicRuntimeStore &
  {
    return m_dynamic_runtime_store;
  }
  fn trap_store() wontthrow -> TrapStore & { return m_trap_store; }
  pure fn trap_store() const wontthrow -> const TrapStore &
  {
    return m_trap_store;
  }
  fn expansion_store() wontthrow -> ExpansionStore &
  {
    return m_expansion_store;
  }
  pure fn expansion_store() const wontthrow -> const ExpansionStore &
  {
    return m_expansion_store;
  }
  fn history_recorder() wontthrow -> HistoryRecorder &
  {
    return m_history_recorder;
  }
  pure fn history_recorder() const wontthrow -> const HistoryRecorder &
  {
    return m_history_recorder;
  }
  fn source_retention() wontthrow -> SourceRetention &
  {
    return m_source_retention;
  }
  pure fn source_retention() const wontthrow -> const SourceRetention &
  {
    return m_source_retention;
  }
  fn source_store() wontthrow -> SourceStore & { return m_source_store; }
  pure fn source_store() const wontthrow -> const SourceStore &
  {
    return m_source_store;
  }
  fn runtime_control_store() wontthrow -> RuntimeControlStore &
  {
    return m_runtime_control_store;
  }
  pure fn runtime_control_store() const wontthrow -> const RuntimeControlStore &
  {
    return m_runtime_control_store;
  }
  fn runtime_state() wontthrow -> RuntimeState & { return m_runtime; }
  pure fn runtime_state() const wontthrow -> const RuntimeState &
  {
    return m_runtime;
  }
  fn scope_store() wontthrow -> ScopeStore & { return m_scope_store; }
  pure fn scope_store() const wontthrow -> const ScopeStore &
  {
    return m_scope_store;
  }
  fn execution_store() wontthrow -> ExecutionStore &
  {
    return m_execution_store;
  }
  pure fn execution_store() const wontthrow -> const ExecutionStore &
  {
    return m_execution_store;
  }
  fn evaluation_metrics_store() wontthrow -> EvaluationMetricsStore &
  {
    return m_evaluation_metrics_store;
  }
  pure fn evaluation_metrics_store() const wontthrow
      -> const EvaluationMetricsStore &
  {
    return m_evaluation_metrics_store;
  }
  fn git_status_cache() wontthrow -> GitStatusCache &
  {
    return m_git_status_cache;
  }
  pure fn git_status_cache() const wontthrow -> const GitStatusCache &
  {
    return m_git_status_cache;
  }
  fn completion_store() wontthrow -> CompletionStore &
  {
    return m_completion_store;
  }
  pure fn completion_store() const wontthrow -> const CompletionStore &
  {
    return m_completion_store;
  }
  fn prompt_command_store() wontthrow -> PromptCommandStore &
  {
    return m_prompt_command_store;
  }
  pure fn prompt_command_store() const wontthrow -> const PromptCommandStore &
  {
    return m_prompt_command_store;
  }
  fn control_flow_store() wontthrow -> ControlFlowStore &
  {
    return m_control_flow_store;
  }
  pure fn control_flow_store() const wontthrow -> const ControlFlowStore &
  {
    return m_control_flow_store;
  }
  fn function_store() wontthrow -> FunctionStore & { return m_function_store; }
  pure fn function_store() const wontthrow -> const FunctionStore &
  {
    return m_function_store;
  }
  fn variable_store() wontthrow -> VariableStore & { return m_variable_store; }
  pure fn variable_store() const wontthrow -> const VariableStore &
  {
    return m_variable_store;
  }
  fn job_table_store() wontthrow -> JobTable & { return m_job_table; }
  pure fn job_table_store() const wontthrow -> const JobTable &
  {
    return m_job_table;
  }

protected:
  StartupStore m_startup_store{};
  EvaluationMetricsStore m_evaluation_metrics_store{};
  GitStatusCache m_git_status_cache{};
  ArenaStore m_arena_store{};
  CompletionStore m_completion_store{};
  ExpansionStore m_expansion_store{};
  VariableStore m_variable_store{};
  ExecutionStore m_execution_store;
  FunctionStore m_function_store{};
  SubshellStore m_subshell_store{};
  EnvironmentStore m_environment_store{};
  DynamicRuntimeStore m_dynamic_runtime_store{};
  DiagnosticsStore m_diagnostics_store{};
  ControlFlowStore m_control_flow_store{};
  SourceStore m_source_store{};
  HistoryRecorder m_history_recorder{};
  SourceRetention m_source_retention{};
  RuntimeState m_runtime{};
  RuntimeControlStore m_runtime_control_store{};
  ProgramResolver m_program_resolver{};
  TrapStore m_trap_store{};
  PromptCommandStore m_prompt_command_store{};
  ScopeStore m_scope_store{};
  JobTable m_job_table{heap_allocator()};
};

class SourceScope
{
public:
  explicit SourceScope(EvalContext &context) wontthrow;
  SourceScope(EvalContext &context, const String *source,
              String origin) wontthrow;
  SourceScope(const SourceScope &) = delete;
  SourceScope &operator=(const SourceScope &) = delete;
  ~SourceScope() { restore(); }

  fn restore() wontthrow -> void;
  pure fn get_source() const wontthrow -> const String * { return m_source; }
  pure fn get_location() const wontthrow -> const SourceLocation &
  {
    return m_location;
  }

private:
  EvalContext *m_context;
  const String *m_source;
  String m_origin;
  SourceLocation m_location;
  bool m_is_armed{true};
};

class SubstitutionFrame
{
public:
  explicit SubstitutionFrame(EvalContext &context) wontthrow
      : m_context(context)
  {}
  SubstitutionFrame(const SubstitutionFrame &) = delete;
  fn operator=(const SubstitutionFrame &)->SubstitutionFrame & = delete;
  ~SubstitutionFrame();

  fn push_source_frame(const WordSegment &segment, StringView origin) throws
      -> void;
  fn push_source_frame(const SourceLocation &location, StringView origin) throws
      -> void;
  fn register_embedded(StringView inner, const SourceLocation &parent_location,
                       const String *body = nullptr) throws -> void;
  fn pop_source_frame() wontthrow -> void;

private:
  EvalContext &m_context;
  bool m_did_push_source_frame{false};
  bool m_did_register_embedded{false};
};

class UntracedTrapScope
{
public:
  enum class Kind : u8
  {
    Debug,
    Err,
    Return,
  };

  UntracedTrapScope(EvalContext &context, Kind kind,
                    bool should_apply = true) throws;
  UntracedTrapScope(const UntracedTrapScope &) = delete;
  fn operator=(const UntracedTrapScope &)->UntracedTrapScope & = delete;
  ~UntracedTrapScope();

private:
  EvalContext &m_context;
  saved_frame_trap m_saved;
  Kind m_kind;
};

class DefinitionStateScope
{
public:
  DefinitionStateScope(EvalContext &context, const definition_state &state,
                       definition_state_exit exit,
                       bool should_enter = true) wontthrow;
  DefinitionStateScope(const DefinitionStateScope &) = delete;
  fn operator=(const DefinitionStateScope &)->DefinitionStateScope & = delete;
  ~DefinitionStateScope();

private:
  EvalContext &m_context;
  Maybe<function_runtime_state> m_saved;
  definition_state_exit m_exit;
};

class EvalContext : public EvalContextState
{
public:
  EvalContext(startup_options options,
              String shell_name = String{heap_allocator()},
              ArrayList<String> positional_params = ArrayList<String>{
                  heap_allocator()});
  fn end_command() wontthrow -> void;

  fn process_args(const ArrayList<const Token *> &args,
                  ArrayList<SourceLocation> *expanded_locations = nullptr,
                  argument_lifetime lifetime = argument_lifetime::Persistent,
                  argument_context context = argument_context::Command,
                  Bitset *subscript_flags = nullptr) throws
      -> ArrayList<String>;

  fn set_shell_variable(StringView name, StringView value) throws -> void;
  pure fn special_variable_definition_location(StringView name) const wontthrow
      -> Maybe<SourceLocation>;
  fn disable_ignoreeof() throws -> void;
  fn restore_temporary_shell_variable(
      StringView name, const Maybe<String> &previous_value,
      Maybe<SourceLocation> previous_definition_location) throws -> void;
  fn begin_confined_variable_writes() wontthrow -> usize;
  fn rollback_confined_variable_writes(usize mark) wontthrow -> void;
  fn seed_shell_identity_variables(shell_identity_mode identity_mode) throws
      -> void;

  fn materialize_kosh_identity() const throws -> Maybe<String>;
  fn prepare_child_environment() const throws -> void;
  fn next_random_u32() const wontthrow -> u32;

  fn unset_shell_variable(StringView name) throws -> void;

  fn unset_array_element(StringView name, StringView subscript) throws -> void;

  fn set_indexed_array(StringView name, ArrayList<String> values) throws
      -> void;
  fn publish_single_pipe_status(i32 status) throws -> void;
  fn publish_pipe_statuses(ArrayList<String> values) throws -> void;
  fn set_array_element(StringView name, usize index, StringView value) throws
      -> void;

  fn assign_array_element(StringView name, StringView subscript,
                          StringView value,
                          assignment_update_mode update_mode) throws -> void;
  fn read_array_element_arithmetic_text(StringView name,
                                        StringView subscript) throws -> String;
  fn declare_associative_array(StringView name) throws -> void;
  fn is_valueless_array(StringView name) const throws -> bool;
  pure fn is_associative_array(StringView name) const wontthrow -> bool
  {
    return m_variable_store.associative_arrays().has(name) ||
           is_bash_aliases_special(name);
  }
  pure fn is_bash_special_array_active(bash_special_array_id id) const wontthrow
      -> bool
  {
    return runtime_state().bash_dynamic_variables_enabled() &&
           (m_variable_store.disabled_bash_special_arrays() &
            bash_special_array_mask(id)) == 0;
  }
  fn disable_bash_special_array(bash_special_array_id id) wontthrow -> void
  {
    m_variable_store.disabled_bash_special_arrays() |=
        bash_special_array_mask(id);
  }
  pure fn is_bash_aliases_special(StringView name) const wontthrow -> bool
  {
    return name == BASH_ALIASES_VARIABLE &&
           is_bash_special_array_active(bash_special_array_id::Aliases);
  }
  pure fn is_bash_directory_stack_special(StringView name) const wontthrow
      -> bool
  {
    return name == DIRSTACK_VARIABLE &&
           is_bash_special_array_active(
               bash_special_array_id::DirectoryStack) &&
           !scope_store().has_current_local(name);
  }
  fn get_bash_directory_stack_element(usize index,
                                      Allocator allocator) const throws
      -> Maybe<String>;
  fn set_bash_directory_stack_element(usize index, StringView value) throws
      -> void;
  fn set_associative_element(StringView name, StringView key,
                             StringView value) throws -> void;
  fn lookup_associative_element(StringView name, StringView key) const throws
      -> Maybe<String>;
  fn associative_keys(StringView name) const throws -> ArrayList<String>;
  fn associative_values(StringView name) const throws -> ArrayList<String>;
  fn clear_associative_array(StringView name) throws -> void;

  fn array_element_count(StringView name) const throws -> usize;
  fn collect_array_elements(StringView name) const throws -> ArrayList<String>;

  fn array_element_is_set(StringView name, StringView subscript) throws -> bool;

  fn cached_compiled_regex(StringView pattern) throws -> os::compiled_regex *;

  fn collect_array_subscripts(StringView name) const throws
      -> ArrayList<String>;

  fn clear_sparse_array(StringView name) throws -> void;

  fn assign_indexed_array_elements(
      StringView name, const ArrayList<String> &elements,
      assignment_update_mode update_mode,
      const Bitset *subscript_flags = nullptr) throws -> void;
  fn assign_associative_elements(StringView name,
                                 const ArrayList<String> &elements) throws
      -> void;

  fn record_environment_change(StringView name) throws -> void;

  fn mark_exported(StringView name) throws -> void;
  fn sync_analysis_environment() throws -> void;
  fn unmark_exported(StringView name) throws -> void;
  fn unexport_shell_variable(StringView name) throws -> void;
  fn is_exported(StringView name) const throws -> bool;

  fn sync_exported_after_restore(StringView name, bool has_value) throws
      -> void;

  fn get_variable_value(StringView name) const throws -> Maybe<String>;
  fn get_variable_value_checked(StringView name) const throws -> Maybe<String>;

  fn resolve_nameref(StringView name) const throws -> Maybe<String>;
  pure fn is_bound_nameref(StringView name) const wontthrow -> bool;
  pure fn has_generated_value(StringView name) const wontthrow -> bool;
  pure fn is_generated_nameref(StringView name) const wontthrow -> bool;
  fn warn_circular_nameref(StringView name) const throws -> void;
  fn is_circular_nameref(StringView name) const throws -> bool;
  fn unbind_circular_nameref(StringView name) throws -> bool;
  fn assign_caller_binding_of_circular_nameref(StringView name,
                                               StringView value) throws -> bool;
  fn assign_global_beneath_locals(StringView name, StringView value,
                                  bool should_append) throws -> bool;
  fn resolve_nameref_for_write(StringView name) throws -> String;
  fn resolve_nameref_base_for_write(StringView name) throws -> String;
  fn resolve_nameref_whole_variable_for_write(StringView name,
                                              bool should_discard_line) throws
      -> String;
  fn guard_nameref_name(StringView name) const throws -> void;
  fn bind_nameref(StringView name, StringView target) throws -> void;
  fn bind_self_nameref(StringView name, bool is_local) throws -> void;
  fn resolve_nameref_parameter(StringView spec) throws -> Maybe<String>;
  pure fn variable_requires_dynamic_lookup(StringView name) const wontthrow
      -> bool;

  pure fn is_dynamic_write_owner(StringView name) const wontthrow -> bool;

  fn write_dynamic_variable(StringView name, StringView value) throws -> bool;

  fn append_dynamic_variable_names(ArrayList<StringView> &out) const throws
      -> void;

  pure fn is_dynamic_reader_unset(StringView name) const wontthrow -> bool;

  fn unset_dynamic_reader(StringView name) wontthrow -> void;

  fn suggest_similar_variable_name(StringView name) const throws
      -> Maybe<String>;

  hot fn has_variable_name(StringView name) const throws -> bool
  {
    return m_variable_store.shell_variables().find(name).has_value() ||
           m_variable_store.indexed_arrays().find(name).has_value() ||
           m_variable_store.associative_arrays().has(name) ||
           is_exported(name) || variable_requires_dynamic_lookup(name);
  }

  fn notify_done_jobs() throws -> void;

  fn register_function(StringView name, const FunctionBodyHandle &body_storage,
                       StringView definition_text, usize body_start_position,
                       SourceLocation definition_location) throws -> void;
  fn function_definition_info_of(StringView name) const wontthrow
      -> const function_definition_info *;
  struct resolved_render_source
  {
    const String *text{nullptr};
    bool is_windowed{false};
    usize body_start_position{0};
    usize header_length{0};
    isize line_offset{0};
    usize enclosing_line_count{0};
    u32 source_name_index{0};

    pure fn to_render_position(usize absolute_position) const wontthrow -> usize
    {
      return is_windowed
                 ? absolute_position - body_start_position + header_length
                 : absolute_position;
    }

    pure fn rebase(SourceLocation location) const wontthrow -> SourceLocation
    {
      if (!is_windowed) return location;

      location.position =
          static_cast<u32>(to_render_position(location.position));
      location.source_name_index = source_name_index;

      return location;
    }
  };
  pure fn source_depth_floor(usize frame_limit) const wontthrow -> usize;
  pure fn resolve_render_source(
      const SourceLocation &location, const String *fallback_source = nullptr,
      usize call_depth_limit = static_cast<usize>(-1),
      usize call_depth_floor = static_cast<usize>(-1)) const wontthrow
      -> resolved_render_source;
  fn register_embedded_source(StringView inner,
                              const SourceLocation &parent_location,
                              const String *body = nullptr) throws -> bool;
  fn unregister_embedded_source() wontthrow -> void;
  pure fn embedded_source_name_index() const wontthrow -> Maybe<u32>;
  fn map_embedded_site(StringView &rendered_source,
                       SourceLocation &location) const wontthrow
      -> const String *;
  pure fn resolve_current_function_window(
      StringView rendered_source,
      const SourceLocation &location) const wontthrow -> resolved_render_source;
  pure fn source_text_in_span(const SourceLocation &location,
                              usize end_position) const wontthrow -> StringView;
  pure fn function_storage_stats() const wontthrow -> function_arena_stats;
  fn unset_function(StringView name) throws -> void;
  fn mark_function_readonly(StringView name) throws -> void;
  fn run_completion_function(StringView function_name, StringView command_name,
                             const ArrayList<String> &words, usize cword,
                             StringView line, usize point,
                             i32 *out_exit_status = nullptr,
                             bool should_mark_directories = false) throws
      -> ArrayList<String>;
  fn run_completion_command(StringView command, StringView command_name,
                            StringView word, StringView previous_word,
                            StringView line, usize point) throws
      -> ArrayList<String>;
  fn expand_wordlist_to_fields(StringView wordlist,
                               bool allow_expansion = true) throws
      -> ArrayList<String>;

  fn variable_names(Allocator result_allocator = heap_allocator()) const throws
      -> HashSet;
  fn set_trap(StringView condition, StringView action,
              Maybe<SourceLocation> definition_location = None) throws -> void;
  fn capture_trap_definition(const SourceLocation &location,
                             StringView action) throws -> trap_definition;
  fn remove_trap(StringView condition) throws -> void;
  fn discard_inherited_signal_traps() throws -> void;
  fn run_exit_trap(Maybe<i32> final_status = None) throws -> void;

  fn run_named_trap(StringView condition,
                    const SourceLocation *trigger_location = nullptr) throws
      -> void;
  fn cached_trap_body(StringView condition, StringView action) throws
      -> FunctionBodyHandle;
  fn run_return_trap(i32 status_before_return) throws -> void;
  fn restore_trap_pipe_statuses(bool has_saved_pipe_statuses,
                                ArrayList<String> saved_pipe_statuses) wontthrow
      -> void;
  pure fn should_run_err_trap() const wontthrow -> bool
  {
    return !trap_store().is_replaying_inherited_state() &&
           (runtime_state().option_is_enabled(shell_option_id::Errtrace) ||
            nesting_depth() <= trap_store().err_trap_active_depth());
  }
  pure fn nesting_depth() const wontthrow -> usize
  {
    return function_store().call_depth() + execution_store().subshell_depth() +
           expansion_store().substitution_depth();
  }
  pure fn should_run_debug_trap() const wontthrow -> bool
  {
    return trap_store().has_debug_trap() && !runtime_state().is_posix_mode() &&
           !trap_store().is_replaying_inherited_state() &&
           (runtime_state().option_is_enabled(shell_option_id::Functrace) ||
            nesting_depth() <= trap_store().debug_trap_active_depth());
  }
  fn lower_trap_depths_to_current() wontthrow -> void
  {
    let const depth = nesting_depth();
    if (trap_store().debug_trap_active_depth() > depth)
      trap_store().debug_trap_active_depth() = depth;
    if (trap_store().err_trap_active_depth() > depth)
      trap_store().err_trap_active_depth() = depth;
  }
  mustuse fn save_untraced_trap(StringView condition,
                                shell_option_id trace_option,
                                usize *active_depth) throws -> saved_frame_trap;
  fn restore_untraced_trap(StringView condition, saved_frame_trap &&saved,
                           usize *active_depth) wontthrow -> void;
  pure fn should_run_return_trap() const wontthrow -> bool
  {
    return !runtime_state().is_posix_mode();
  }
  pure fn trap_trigger_line_number() const wontthrow -> Maybe<usize>
  {
    return trap_store().action_frame().get_trigger_line_number(
        source_store().source_frames().count(), function_store().call_depth());
  }

  fn run_pending_traps() throws -> void;
  fn has_exit_trap() const wontthrow -> bool;

  fn clear_inherited_exit_trap() throws -> void;
  fn run_subshell_exit_trap() throws -> Maybe<i32>;

  fn reset_inherited_signal_traps() wontthrow -> void;
  pure fn did_reset_inherited_signal_traps() const wontthrow -> bool;

  pure fn is_signal_ignored_at_startup(StringView condition) const wontthrow
      -> bool;
  fn note_subshell_child_exit() wontthrow -> void;

  fn is_readonly(StringView name) const wontthrow -> bool;
  fn is_implicitly_readonly(StringView name) const wontthrow -> bool;
  fn is_implicitly_integer(StringView name) const wontthrow -> bool;
  fn readonly_names() const throws
      -> SortedArrayList<String, order_comparator<String>>;

  fn is_integer_variable(StringView name) const wontthrow -> bool;
  fn append_integer_expression(String &joined,
                               StringView expression) const throws -> void;

  fn enter_function_scope() throws -> void;
  fn leave_function_scope() throws -> void;
  fn push_function_call_name(StringView name,
                             const FunctionBodyHandle &body_storage) throws
      -> void;
  fn pop_function_call_name() wontthrow -> void;
  using BashArgumentFrameFlag = koshka::BashArgumentFrameFlag;
  using BashArgumentFrameContext = koshka::BashArgumentFrameContext;
  fn enter_bash_function_argument_frame(
      BashArgumentFrameContext &frame_context,
      const ArrayList<String> &arguments) throws -> void;
  fn enter_bash_source_argument_frame(BashArgumentFrameContext &frame_context,
                                      const ArrayList<String> *arguments,
                                      StringView source_path) throws -> void;
  fn leave_bash_argument_frame(
      BashArgumentFrameContext &frame_context) wontthrow -> void;
  struct MergedFrame
  {
    enum class Kind : u8
    {
      Function,
      Source,
      Main,
    };

    Kind kind{Kind::Main};
    usize storage_index{0};
  };
  mustuse fn merged_frame_at(usize index, usize total,
                             Maybe<usize> script_source_index) const wontthrow
      -> MergedFrame;
  mustuse fn merged_frame_at(usize index) const wontthrow -> MergedFrame;
  mustuse fn script_source_frame_index() const wontthrow -> Maybe<usize>;
  mustuse fn funcname_frame_count() const wontthrow -> usize;
  mustuse fn funcname_frame_at(usize index) const wontthrow -> StringView;
  mustuse fn funcname_line_at(usize index) const throws -> usize;
  mustuse fn bash_source_frame_at(usize index) const wontthrow -> StringView;
  mustuse fn bash_source_frame_count(
      Maybe<usize> script_source_index) const wontthrow -> usize;
  mustuse fn bash_source_frame_count() const wontthrow -> usize;

  enum class DynamicArray : u8
  {
    ArgumentCount,
    ArgumentValue,
    FunctionName,
    LineNumber,
    SourcePath,
  };
  mustuse fn dynamic_array_element_count(DynamicArray which) const throws
      -> usize;
  mustuse fn dynamic_array_element_text(DynamicArray which, usize index,
                                        Allocator result_allocator) const throws
      -> String;
  pure fn is_bash_argument_array(StringView name) const wontthrow -> bool;
  pure fn is_write_discarded_dynamic_variable(StringView name) const wontthrow
      -> bool;

  mustuse fn line_number_at_location(
      const SourceLocation &location, const String *fallback_source = nullptr,
      Maybe<usize> fallback_call_depth = None) const throws -> usize;
  fn push_root_source_frame(const String *parent_source,
                            SourceLocation call_site,
                            source_frame_kind kind) throws -> void;
  fn pop_root_source_frame() wontthrow -> void;
  pure fn scan_source_generation(const String *source) const wontthrow -> u64;
  pure fn source_generation_for(const String *source) const wontthrow -> u64;
  pure fn borrowed_frame_source(const source_frame &frame) const wontthrow
      -> const String *;
  fn declare_local(StringView name, bool should_inherit_value) throws -> void;
  fn declare_self_reference(StringView name) throws -> void;
  fn snapshot_state() throws -> eval_state_snapshot;
  fn restore_state(eval_state_snapshot snapshot) throws -> void;
  fn make_subshell_bootstrap() const throws -> os::subshell_bootstrap;
  fn make_child_evaluator_state(os::subshell_bootstrap &bootstrap) const throws
      -> os::child_evaluator_state;
  fn apply_subshell_bootstrap(os::subshell_bootstrap bootstrap) throws -> void;
  fn set_child_source_origin(os::subshell_bootstrap &bootstrap,
                             StringView child_source,
                             const SourceLocation &launch_location) const throws
      -> void;
  fn register_inherited_source_origin(StringView origin, const String &contents,
                                      ArrayList<String> &windows,
                                      Maybe<StringView> &source_name) throws
      -> bool;

  fn enter_subshell() wontthrow -> void;
  fn leave_subshell() wontthrow -> void;
  fn set_subshell_depth(usize depth) wontthrow -> void;
  pure fn in_subshell() const wontthrow -> bool;
  pure fn can_replace_process() const wontthrow -> bool;
  fn snapshot_subshell_descriptor(i32 shell_fd) throws -> void;

  fn set_coprocess_descriptors(i32 read_fd, i32 write_fd, i64 process_id,
                               StringView name) throws -> void;
  fn forget_coprocess_descriptor(i32 shell_fd) throws -> void;
  fn release_finished_coprocess() throws -> void;
  fn hide_coprocess_descriptors() throws -> void;

  fn request_loop_control(control_flow::Kind kind, i64 level,
                          SourceLocation location) throws -> void;
  fn request_break(i64 level, SourceLocation location) throws -> void;
  fn request_continue(i64 level, SourceLocation location) throws -> void;
  fn request_return(i64 status, SourceLocation location) throws -> void;
  fn request_exit(i64 status, SourceLocation location) throws -> void;

  fn set_current_source(const String *source, String origin) wontthrow -> void;
  fn set_fresh_source(const String *source, String origin) wontthrow -> void;
  mustuse fn capture_source_scope() wontthrow -> SourceScope
  {
    return SourceScope{*this};
  }
  mustuse fn enter_source_scope(const String *source, String origin) wontthrow
      -> SourceScope
  {
    return SourceScope{*this, source, steal(origin)};
  }
  fn record_history_event(StringView command) throws -> bool;
  fn print_source_backtrace(Maybe<SourceLocation> error_location = None,
                            bool should_defer_for_source_file = true,
                            bool is_replay = false) throws -> void;
  fn set_diagnostic_highlight_cache(completion::shell_highlight_cache *cache)
      wontthrow -> completion::shell_highlight_cache *
  {
    let *previous = diagnostics_store().diagnostic_highlight_cache();
    diagnostics_store().diagnostic_highlight_cache() = cache;
    return previous;
  }

  fn get_or_create_diagnostic_highlight_cache() throws
      -> completion::shell_highlight_cache *;
  fn reset_runtime_diagnostic_highlight_cache() wontthrow -> void;

  fn render_contained_substitution_error(const std::exception_ptr &error,
                                         StringView source) throws -> void;

  pure fn strict_diagnostics_are_warnings() const wontthrow -> bool
  {
    if (runtime_state().get_mood() == mimic_mood::Default)
      return runtime_state().get_warning_level() >= 3;
    return runtime_state().get_warning_level() >= 1;
  }
  fn report_unset_reference(StringView name) throws -> void;
  fn warn_or_throw(bool fatal, bool explicitly_requested,
                   const SourceLocation &location, StringView message,
                   StringView note = {}) throws -> void;
  cold fn show_runtime_warning_at(SourceLocation location, StringView message,
                                  StringView note = {},
                                  bool should_ignore_disabled = false) wontthrow
      -> void;
  cold fn show_runtime_error_at(SourceLocation location,
                                StringView message) wontthrow -> void;
  pure fn locate_variable_reference(StringView name) const wontthrow
      -> SourceLocation;

  fn expand_glob_lenient(StringView pattern) throws
      -> SortedArrayList<String, order_comparator<String>>;

  fn set_posix_mode_via_option(bool enable) wontthrow -> void
  {
    if (enable) {
      runtime_control_store().note_explicit_mood();
      select_mood(mimic_mood::BashPosix);
      return;
    }
    if (!runtime_state().is_posix_option_on()) return;
    runtime_control_store().note_explicit_mood();
    select_mood(mimic_mood::Bash);
  }

  fn select_mood(mimic_mood mood) wontthrow -> void
  {
    runtime_state().set_mood(mood);
    apply_strictness_for_mood();
  }

  fn apply_strictness_for_mood() wontthrow -> void
  {
    let const strict = runtime_state().get_mood() == mimic_mood::Default;
    let const is_completion_running =
        execution_store().completion_function_running();
    if (strict || !runtime_state().was_error_unset_set_explicitly())
      runtime_state().set_error_unset(strict && !is_completion_running);
    if (strict || !runtime_state().was_pipefail_set_explicitly())
      runtime_state().set_pipefail(strict);
    if (strict || !runtime_state().was_failglob_set_explicitly())
      runtime_state().set_failglob(strict && !is_completion_running);
    if (strict || !runtime_state().was_extended_arithmetic_set_explicitly())
      runtime_state().set_extended_arithmetic(strict);
  }

  friend class RuntimeState;
  fn enter_definition_state(const definition_state &defining_state) wontthrow
      -> function_runtime_state
  {
    let const previous = RuntimeState::capture(*this);
    defining_state.apply_to(runtime_state());
    apply_strictness_for_mood();
    return function_runtime_state{previous, RuntimeState::capture(*this),
                                  runtime_control_store().get_mutations()};
  }

  fn leave_definition_state(
      const function_runtime_state &state,
      definition_state_exit exit =
          definition_state_exit::PropagateMutations) wontthrow -> void
  {
    if (exit == definition_state_exit::RestoreCaller) {
      state.previous.restore(*this);
      runtime_control_store().restore_snapshot_state(
          runtime_control_store().init_moods_sourcing_mask(),
          runtime_control_store().initialized_moods_mask(),
          state.entry_mutations);
      return;
    }

    let const finished = RuntimeState::capture(*this);
    let const changed_reporting =
        runtime_control_store().get_mutations().reporting.changed_fields_since(
            state.entry_mutations.reporting);
    let const &options = runtime_control_store().option_mutations();
    let const entry_option_revision = state.entry_mutations.options.revision;
    let changed_options =
        state.entered.get_shell_options() ^ finished.get_shell_options();
    if (reporting_revisions::has_field(changed_reporting,
                                       reporting_field::Mood))
    {
      changed_options |= RuntimeState::option_mask(shell_option_id::Nounset);
      changed_options |= RuntimeState::option_mask(shell_option_id::Pipefail);
      changed_options |= RuntimeState::option_mask(shell_option_id::Failglob);
      changed_options |=
          RuntimeState::option_mask(shell_option_id::ExtendedArithmetic);
    }
    for (u8 option = 0; option < static_cast<u8>(shell_option_id::Count);
         option++)
    {
      let const option_id = static_cast<shell_option_id>(option);
      if (options.touched_since(option_id, entry_option_revision))
        changed_options |= RuntimeState::option_mask(option_id);
    }
    let const merged_options =
        (state.previous.get_shell_options() & ~changed_options) |
        (finished.get_shell_options() & changed_options);

    state.previous.restore(*this);
    runtime_state().set_shell_options(merged_options);
    if (options.touched_since(shell_option_id::Nounset, entry_option_revision))
      runtime_state().set_error_unset_set_explicitly(
          finished.was_error_unset_set_explicitly());
    if (options.touched_since(shell_option_id::Pipefail, entry_option_revision))
      runtime_state().set_pipefail_set_explicitly(
          finished.was_pipefail_set_explicitly());
    if (options.touched_since(shell_option_id::Failglob, entry_option_revision))
      runtime_state().set_failglob_set_explicitly(
          finished.was_failglob_set_explicitly());
    if (options.touched_since(shell_option_id::ExtendedArithmetic,
                              entry_option_revision))
      runtime_state().set_extended_arithmetic_set_explicitly(
          finished.was_extended_arithmetic_set_explicitly());
    if (reporting_revisions::has_field(changed_reporting,
                                       reporting_field::Mood))
      runtime_state().set_mood(finished.get_mood());
    if (reporting_revisions::has_field(changed_reporting,
                                       reporting_field::Warning))
      runtime_state().set_warning_level(finished.get_warning_level());
    if (reporting_revisions::has_field(changed_reporting,
                                       reporting_field::Diagnostics))
      runtime_state().set_diagnostics_disabled(
          finished.is_diagnostics_disabled());
    if (reporting_revisions::has_field(changed_reporting,
                                       reporting_field::Annoying))
      runtime_state().set_annoying_diagnostics_enabled(
          finished.is_annoying_diagnostics_enabled());
    if (runtime_state().get_mood() == mimic_mood::Default)
      apply_strictness_for_mood();
  }

  fn run_mimicked_script(ExecContext &ec, mimic_mood mode,
                         script_isolation isolation) throws -> i32;
  fn run_program_fallback(ExecContext &ec, mimic_mood mode,
                          script_isolation isolation) throws -> i32;
  pure fn get_extglob_mode() const wontthrow -> extglob_mode
  {
    return !runtime_state().is_posix_mode() && is_shopt_enabled("extglob")
               ? extglob_mode::Enabled
               : extglob_mode::Disabled;
  }

  fn get_glob_charset() const throws -> glob_charset;
  fn get_glob_charset_for(StringView subject) const throws -> glob_charset;
  fn first_field_separator() const throws -> StringView;
  hot fn get_glob_charset_for(const String &subject) const throws
      -> glob_charset
  {
    return subject.is_ascii() ? glob_charset::Bytes : get_glob_charset();
  }

  fn set_shopt_option(StringView name, bool is_enabled) throws -> void;
  pure fn is_shopt_enabled(StringView name) const wontthrow -> bool
  {
    let const index = shopt_option_index(name);
    if (!index.has_value()) return false;
    if (*index == shopt_option_index(shopt_option_id::Nullglob) &&
        runtime_state().get_mood() == mimic_mood::Default)
    {
      return false;
    }
    if (runtime_state().is_shopt_option_overridden(*index))
      return runtime_state().is_shopt_option_enabled(*index);
    if (*index == shopt_option_index(shopt_option_id::Extglob))
      return runtime_state().get_mood() == mimic_mood::Default;
    if (*index == shopt_option_index(shopt_option_id::Autocd)) return true;
    if (*index == shopt_option_index(shopt_option_id::ExpandAliases))
      return runtime_state().get_mood() != mimic_mood::Bash ||
             execution_store().shell_is_interactive();
    return shopt_default_is_on(name);
  }
  static pure fn shopt_default_is_on(StringView name) wontthrow -> bool;

  fn enter_loop() wontthrow -> void;
  fn leave_loop() wontthrow -> void;

  fn sorted_variable_assignments() const throws
      -> SortedArrayList<String, order_comparator<String>>;

  fn expand_word_for_assignment(const Word &word,
                                bool is_assignment_value = false) throws
      -> String;

  fn evaluate_arithmetic(StringView expression,
                         const SourceLocation *expression_base = nullptr,
                         bool should_discard_top_level_line = false) throws
      -> i64;
  fn mark_expansion_error(ErrorBase &error,
                          expansion_error_reach reach) const wontthrow -> void;
  fn evaluate_arithmetic_text(
      StringView expression, const SourceLocation *expression_base = nullptr,
      arithmetic_text_kind text_kind = arithmetic_text_kind::Value) throws
      -> String;
  fn evaluate_calculator_arithmetic_text(
      StringView expression,
      const SourceLocation *expression_base = nullptr) throws -> String;
  fn evaluate_bc_arithmetic_text(StringView expression, u32 scale) throws
      -> String;
  fn evaluate_arithmetic_nonzero(
      StringView expression, const SourceLocation *expression_base = nullptr,
      arithmetic_text_kind text_kind = arithmetic_text_kind::Value) throws
      -> bool;
  fn compare_arithmetic(StringView left, StringView right) throws -> i32;
  fn evaluate_arithmetic_cached_text(const WordSegment &segment) throws
      -> String;

  fn evaluate_arithmetic_cached_clause_nonzero(
      StringView expression, ArrayList<arith_token> &tokens, bool &is_tokenized,
      bool &is_simple, const SourceLocation *source_location = nullptr) throws
      -> bool;

  fn evaluate_conditional(const ArrayList<conditional_element> &elements) throws
      -> bool;

  fn expand_case_pattern_masked(const Word &word, Bitset &active_out) throws
      -> String;

  fn capture_command_substitution(
      const String &source, Maybe<StringView> filename = None,
      const SourceLocation *call_site = nullptr) throws -> String;

  fn capture_command_substitution(const WordSegment &segment) throws -> String;

  fn capture_function_substitution(const WordSegment &segment) throws -> String;
  fn capture_function_substitution(StringView text,
                                   const SourceLocation *call_site) throws
      -> String;
  fn push_substitution_source_frame(const WordSegment &segment,
                                    StringView origin) throws -> bool;
  fn push_substitution_source_frame(const SourceLocation &location,
                                    StringView origin) throws -> bool;

  fn read_redirect_substitution(StringView source) throws -> Maybe<String>;

  fn setup_process_substitution(const WordSegment &segment) throws -> String;
  fn setup_process_substitution(StringView text,
                                Maybe<SourceLocation> segment_location) throws
      -> String;
  mustuse fn mark_process_substitutions() const wontthrow
      -> process_substitution_mark;
  fn cleanup_process_substitutions(process_substitution_mark mark) wontthrow
      -> void;
  fn hold_process_substitutions(process_substitution_mark mark) wontthrow
      -> void;
  fn release_finished_held_process_substitutions() wontthrow -> void;
  fn wait_for_process_substitution(i64 process_id,
                                   bool should_block = true) wontthrow
      -> Maybe<i32>;
  pure fn is_pending_process_substitution(i64 process_id) const wontthrow
      -> bool;

  mustuse fn mark_loop_redirect_fds() const wontthrow -> loop_redirect_fd_mark;
  fn cleanup_loop_redirect_fds(loop_redirect_fd_mark mark) wontthrow -> void;
  mustuse fn find_loop_redirect_fd(i32 target_fd, const String &path,
                                   os::file_open_mode mode) const wontthrow
      -> Maybe<os::descriptor>;
  mustuse fn retain_loop_redirect_fd(i32 target_fd, const String &path,
                                     os::file_open_mode mode,
                                     os::descriptor fd) throws -> bool;

  fn run_captured_substitution(const Expression *ast, const String &source,
                               Maybe<SourceLocation> call_site) throws
      -> String;
  fn run_function_substitution(const Expression *ast, const String &source,
                               bool is_value_substitution) throws -> String;

  fn run_source(
      StringView source, StringView origin = "a sourced command",
      Maybe<SourceLocation> call_site = None, Maybe<StringView> filename = None,
      Maybe<i32> *status_before_return = nullptr,
      const FunctionBodyHandle *cached_body = nullptr,
      return_handling handling = return_handling::Consume,
      history_recording history = history_recording::Disabled,
      const trap_definition *definition = nullptr,
      syntax_error_reach syntax_reach = syntax_error_reach::Command) throws
      -> i32;
  fn resolve_source_path(StringView path,
                         source_tilde_expansion tilde_expansion =
                             source_tilde_expansion::Disabled) throws
      -> Maybe<Path>;

  fn enter_source(const SourceLocation &location) throws -> void;
  fn leave_source() wontthrow -> void;
  fn enter_function_call(const SourceLocation &location) throws -> void;
  fn leave_function_call() wontthrow -> void;
  fn enter_substitution() throws -> void;
  fn leave_substitution() wontthrow -> void;
  fn enter_parameter_expansion() throws -> void;
  fn leave_parameter_expansion() wontthrow -> void;

  fn clear_retained_sources() wontthrow -> void;

  fn expand_heredoc_body(StringView body,
                         const SourceLocation *source_location = nullptr) throws
      -> String;

  fn expand_modifier_word(
      StringView word, bool remove_quotes = true,
      bool strip_escaped_literals = true,
      const SourceLocation *source_location = nullptr,
      bool should_expand_process_substitution = false,
      parameter_word_quoting quoting = parameter_word_quoting::Unquoted) throws
      -> String;

  fn expand_modifier_word_masked(
      StringView word, Bitset &active_out, bool remove_quotes = true,
      const SourceLocation *source_location = nullptr) throws -> String;

  fn expand_modifier_word_fields(StringView word, bool is_outer_quoted,
                                 Bitset &active_out,
                                 ArrayList<usize> &break_out,
                                 ArrayList<quoted_empty_mark> &mark_out,
                                 const SourceLocation *source_location) throws
      -> String;

  class ModifierWordExpander;
  class ParameterExpander;

  fn expand_modifier_word_worker(StringView word, Bitset *active_out,
                                 bool remove_quotes, bool is_pattern_word,
                                 bool strip_escaped_literals,
                                 const SourceLocation *source_location) throws
      -> String;

  fn write_xtrace(StringView command) throws -> void;
  fn write_xtrace(const ArrayList<String> &args) throws -> void;
  fn set_startup_finished() wontthrow -> void
  {
    startup_store().mark_startup_finished();
    if (startup_store().is_restricted_shell())
      runtime_state().set_option(shell_option_id::Restricted, true);
  }
  fn guard_restricted_path(StringView path, const SourceLocation &location,
                           restricted_path_use use) const throws -> void;

  fn make_stats_string() const throws -> String;

protected:
  fn install_trap_dispositions() throws -> void;

  fn option_flags_string() const throws -> String;

  fn initialize_bash_argument_arrays(
      bool should_include_current_frame) const throws -> void;
  fn append_current_bash_argument_frame() const throws -> void;

  fn expand_variable(StringView name) const throws -> String;

  fn assign_variable(StringView name, StringView value,
                     Maybe<variable_entry *> entry = None) throws -> void;

  fn force_unset_shell_variable(StringView name) throws -> void;
  fn peel_caller_local_binding(StringView name) throws -> bool;
  fn restore_local_binding(local_binding &binding) throws -> void;

  fn apply_parameter_expansion(
      StringView spec, const SourceLocation *source_location = nullptr,
      usize source_location_offset = 0,
      bool should_expand_process_substitution = false,
      parameter_word_quoting quoting = parameter_word_quoting::Unquoted) throws
      -> String;

  fn apply_substring_expansion(
      StringView name, StringView body,
      const SourceLocation *source_location = nullptr) throws -> String;
  fn apply_substring_to_value(
      StringView value, StringView body,
      const SourceLocation *source_location = nullptr) throws -> String;
  fn compute_list_slice_bounds(StringView slice, i64 value_count,
                               const SourceLocation *source_location =
                                   nullptr) throws -> substring_bounds;
  fn compute_array_slice_bounds(
      StringView name, StringView slice, i64 element_count,
      const SourceLocation *source_location = nullptr) throws
      -> substring_bounds;
  fn join_list_slice(substring_bounds bounds, const ArrayList<String> &values,
                     Maybe<StringView> leading, bool is_star) const throws
      -> String;

  fn apply_pattern_replacement(
      StringView name, StringView spec,
      const SourceLocation *source_location = nullptr) throws -> String;

  fn pattern_replace_value(
      StringView value, StringView spec,
      const SourceLocation *source_location = nullptr) throws -> String;

  fn apply_case_modification(
      StringView name, StringView spec,
      const SourceLocation *source_location = nullptr) throws -> String;

  fn apply_parameter_transform(StringView name, char op) throws -> String;
  fn get_declaration_fields(StringView name) throws -> ArrayList<String>;
  fn apply_parameter_transform_to_value(StringView value, char op,
                                        StringView name) throws -> String;
  fn apply_case_modification_to_value(
      StringView value, StringView spec,
      const SourceLocation *source_location = nullptr) throws -> String;
  fn apply_value_modifier(
      StringView value, StringView modifier,
      const SourceLocation *source_location = nullptr) throws -> String;
  fn trim_positional_fields(bool is_star, bool is_quoted, StringView modifier,
                            const SourceLocation *source_location =
                                nullptr) throws -> ArrayList<String>;
  fn is_posix_positional_test_null(bool is_colon, char op,
                                   bool should_test_joined) wontthrow -> bool;

  fn apply_array_subscript(
      StringView name, StringView subscript,
      const SourceLocation *source_location = nullptr) throws -> String;
  fn read_literal_array_element(StringView name,
                                StringView subscript) const throws
      -> Maybe<String>;
  fn array_negative_index_base(StringView name) const throws -> i64;

  fn apply_indirect_or_name_listing(StringView body) throws -> String;

  fn matching_prefix_names(StringView prefix) const throws
      -> SortedArrayList<String, order_comparator<String>>;

  fn expand_word(const Word &word) throws -> ArrayList<glob_field>;

  fn expand_path_once(const glob_field &field,
                      glob_expansion_mode expansion_mode) throws
      -> ArrayList<glob_field>;
  fn expand_path_recurse(ArrayList<glob_field> fields) throws
      -> ArrayList<glob_field>;
  fn expand_path(glob_field field, const SourceLocation &location) throws
      -> SortedArrayList<String, order_comparator<String>>;

  fn expand_tilde(WordSegment &leading_segment, bool word_continues,
                  bool stop_at_colon) const throws -> void;
  fn resolve_tilde_prefix(StringView name) const throws -> Maybe<String>;
  fn expand_colon_tildes(WordSegment &segment, bool word_continues,
                         Maybe<usize> equals_position = None) const throws
      -> void;
};

} /* namespace koshka */
