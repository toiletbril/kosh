/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file declares the evaluator interface, compact runtime option state,
 * resolved commands, command arguments, execution contexts, and EvalContext
 * storage. Expressions, builtins, startup, completion, and subshell transport
 * share these declarations.
 */

#pragma once

#include "Arena.hpp"
#include "Bitset.hpp"
#include "Builtin.hpp"
#include "Common.hpp"
#include "Containers.hpp"
#include "Errors.hpp"
#include "Maybe.hpp"
#include "MimicMood.hpp"
#include "Path.hpp"
#include "Platform.hpp"

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

  Kind kind{Kind::Program};
  Builtin::Kind builtin_kind{};
  Path program_path{};
  i32 unresolved_status{127};

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

fn compute_substring_bounds(i64 value_count, i64 offset, Maybe<i64> length,
                            substring_subject subject) throws
    -> substring_bounds;
pure fn shopt_option_index(StringView name) wontthrow -> Maybe<u8>;

enum class shopt_option_id : u8
{
  Checkhash,
  InheritErrexit,
  Lastpipe,
  LocalvarInherit,
  Progcomp,
  ProgcompAlias,
  Sourcepath,
};
pure fn shopt_option_index(shopt_option_id option) wontthrow -> u8;

class EvalContext
{
public:
  EvalContext(bool should_disable_path_expansion, bool should_echo,
              bool should_echo_expanded, bool shell_is_interactive,
              bool should_error_exit = false,
              String shell_name = String{heap_allocator()},
              ArrayList<String> positional_params = ArrayList<String>{
                  heap_allocator()});
  ~EvalContext();

  fn add_expansion() wontthrow -> void;
  fn add_evaluated_expression() wontthrow -> void;

  fn end_command() wontthrow -> void;

  /* Variable expand, tilde expand, field split, and glob each token. The
     expanded_locations out-parameter, when not null, is filled in parallel
     with the returned strings, so each field carries the source_location of
     the token it expanded from. A token that splits into many fields
     contributes one location per field. */
  fn process_args(const ArrayList<const Token *> &args,
                  argument_lifetime lifetime = argument_lifetime::Persistent,
                  argument_context context = argument_context::Command,
                  ArrayList<SourceLocation> *expanded_locations =
                      nullptr) throws -> ArrayList<String>;

  fn scratch_allocator() const wontthrow -> Allocator
  {
    return bump_allocator(m_scratch_arena);
  }
  mustuse fn scratch_mark() const wontthrow -> BumpArena::Mark
  {
    return m_scratch_arena.mark();
  }
  fn scratch_release(BumpArena::Mark saved) wontthrow -> void
  {
    m_scratch_arena.release(saved);
  }
  fn reset_scratch_arena() wontthrow -> void { m_scratch_arena.reset(); }

  fn set_shell_variable(StringView name, StringView value) throws -> void;
  fn get_program_resolver() wontthrow -> ProgramResolver &
  {
    return m_program_resolver;
  }
  pure fn get_program_resolver() const wontthrow -> const ProgramResolver &
  {
    return m_program_resolver;
  }

  fn seed_shell_identity_variables(bool bash_identity) throws -> void;

  fn set_shell_executable_path(StringView path) throws -> void
  {
    m_shell_executable_path = String{heap_allocator(), path};
  }
  pure fn shell_executable_path() const wontthrow -> StringView
  {
    return m_shell_executable_path.view();
  }
  fn materialize_kosh_identity() const throws -> Maybe<String>;
  fn next_random_u32() const wontthrow -> u32;

  fn unset_shell_variable(StringView name) throws -> void;

  fn unset_array_element(StringView name, StringView subscript) throws -> void;

  fn set_indexed_array(StringView name, ArrayList<String> values) throws
      -> void;
  fn publish_single_pipe_status(i32 status) throws -> void;
  fn append_indexed_array(StringView name, ArrayList<String> values) throws
      -> void;
  fn set_array_element(StringView name, usize index, StringView value) throws
      -> void;

  /* Assign one array element from a raw subscript, routing an associative name
     to a string key and an indexed name to an arithmetic index. The append form
     concatenates onto the current element. */
  fn assign_array_element(StringView name, StringView subscript,
                          StringView value, bool is_append) throws -> void;
  fn read_array_element_arithmetic_text(StringView name,
                                        StringView subscript) throws -> String;
  pure fn lookup_indexed_array(StringView name) const wontthrow
      -> const ArrayList<String> *
  {
    return m_indexed_arrays.find(name);
  }

  /* The bash associative arrays. The values live in one flat map under a
     composite name-and-key, the declared names are tracked separately. */
  fn declare_associative_array(StringView name) throws -> void;
  pure fn is_associative_array(StringView name) const wontthrow -> bool
  {
    return m_associative_names.contains(name);
  }
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

  /* The compiled form of an extended regex, reused across matches so a hot =~
     loop compiles each distinct pattern once. */
  fn cached_compiled_regex(StringView pattern) throws -> os::compiled_regex *;

  fn collect_array_subscripts(StringView name) const throws
      -> ArrayList<String>;

  fn clear_sparse_array(StringView name) throws -> void;

  /* Assign an array literal, honoring an explicit [index]=value element with a
     bare element taking the next index. An append continues after the highest
     set index. */
  fn assign_indexed_array_elements(StringView name,
                                   const ArrayList<String> &elements,
                                   bool is_append) throws -> void;

  fn record_environment_change(StringView name) throws -> void;

  fn mark_exported(StringView name) throws -> void;
  fn unmark_exported(StringView name) throws -> void;
  fn unexport_shell_variable(StringView name) throws -> void;
  pure fn is_exported(StringView name) const wontthrow -> bool;

  fn sync_exported_after_restore(StringView name, bool has_value) throws
      -> void;

  /* Set IFS and refresh the separator table together, so the table never drifts
     from the cached value. */
  fn set_field_separators(StringView value) throws -> void;
  pure fn field_separators() const wontthrow -> StringView
  {
    return m_field_separators.view();
  }
  fn get_variable_value(StringView name) const throws -> Maybe<String>;
  fn get_variable_value_checked(StringView name) const throws -> Maybe<String>;
  pure fn variable_requires_dynamic_lookup(StringView name) const wontthrow
      -> bool;

  fn append_dynamic_variable_names(ArrayList<StringView> &out) const throws
      -> void;

  /* The closest name the shell currently holds, or None when nothing is close
     enough. The walk covers every name the shell knows and runs only on the
     unset diagnostic path. */
  fn suggest_similar_variable_name(StringView name) const throws
      -> Maybe<String>;

  hot fn lookup_shell_variable(StringView name) const wontthrow
      -> const String *
  {
    return m_shell_variables.find(name);
  }

  hot pure fn has_variable_name(StringView name) const wontthrow -> bool
  {
    return m_shell_variables.find(name) != nullptr ||
           m_indexed_arrays.find(name) != nullptr ||
           m_associative_names.contains(name) ||
           m_exported_names.contains(name) ||
           variable_requires_dynamic_lookup(name);
  }

  pure fn positional_params() const wontthrow -> const ArrayList<String> &;
  fn set_positional_params(ArrayList<String> params) wontthrow -> void;
  pure fn shell_name() const wontthrow -> StringView
  {
    return m_shell_name.view();
  }

  fn directory_stack() wontthrow -> ArrayList<String> &;
  pure fn directory_stack() const wontthrow -> const ArrayList<String> &
  {
    return m_directory_stack;
  }

  /* Move the positional parameters out, so a function call saves the caller's
     without a deep copy and restores them by moving the saved list back. */
  fn take_positional_params() wontthrow -> ArrayList<String>;

  fn set_last_exit_status(i32 status) wontthrow -> void;
  pure fn last_exit_status() const wontthrow -> i32;

  fn set_last_argument(StringView value) throws -> void
  {
    m_last_argument = String{value};
  }

  fn set_last_command_duration_nanos(u64 nanos) wontthrow -> void;
  pure fn last_command_duration_nanos() const wontthrow -> u64;

  fn set_last_background_pid(i64 pid) wontthrow -> void;

  /* The job table tracks the background commands started with the & operator.
     register_job adds a running job and returns its id. update_jobs polls every
     job without blocking and marks the ones that finished or stopped. */
  fn register_job(os::process pid, StringView command,
                  i64 process_group_id = 0) throws -> i32;
  fn register_pipeline_job(const ArrayList<os::process> &processes,
                           os::process primary_process, StringView command,
                           i64 process_group_id) throws -> i32;
  fn register_stopped_job(os::process pid, StringView command, i32 status,
                          i64 process_group_id) throws -> i32;
  fn wait_for_job_processes(job &job, bool *was_stopped = nullptr) throws
      -> i32;
  fn notify_stopped_job(i32 id, StringView command) throws -> void;
  fn update_jobs() throws -> void;
  fn jobs() wontthrow -> ArrayList<job> &;
  fn find_job(i32 id) wontthrow -> job *;
  fn find_job_index_by_spec(StringView spec) throws -> Maybe<usize>;
  fn find_job_by_spec(StringView spec) throws -> job *;
  fn most_recent_job() wontthrow -> job *;
  fn forget_done_jobs() throws -> void;
  fn remove_job(i32 id) throws -> bool;

  fn notify_done_jobs() throws -> void;
  fn format_done_job_notifications(StringView line_ending) throws -> String;

  /* monitor mode is set -m, on by default in an interactive shell. It gates the
     terminal handoff so a non-interactive run never touches the tty. */
  fn set_monitor(bool enabled) wontthrow -> void;
  pure fn monitor() const wontthrow -> bool;

  /* notify mode is set -b. The prompt's wake hook reports a background job's
     completion immediately. */
  fn set_notify(bool enabled) wontthrow -> void;
  pure fn notify() const wontthrow -> bool;

  fn set_vi_mode(bool enabled) wontthrow -> void
  {
    m_runtime.set_option(shell_option_id::Vi, enabled);
    if (enabled) m_runtime.set_option(shell_option_id::Emacs, false);
  }
  pure fn vi_mode() const wontthrow -> bool
  {
    return m_runtime.option_is_enabled(shell_option_id::Vi);
  }
  fn set_emacs_mode(bool enabled) wontthrow -> void
  {
    m_runtime.set_option(shell_option_id::Emacs, enabled);
    if (enabled) m_runtime.set_option(shell_option_id::Vi, false);
  }
  pure fn emacs_mode() const wontthrow -> bool
  {
    return m_runtime.option_is_enabled(shell_option_id::Emacs);
  }

  fn register_function(StringView name, const FunctionBodyHandle &body_storage,
                       StringView definition_text, usize body_start_position,
                       SourceLocation definition_location) throws -> void;
  fn find_function_source(StringView name) const wontthrow -> const String *;
  fn function_definition_info_of(StringView name) const wontthrow
      -> const function_definition_info *;
  struct resolved_render_source
  {
    const String *text{nullptr};
    bool is_windowed{false};
    usize body_start_position{0};
    usize header_length{0};
    usize line_offset{0};
    u32 source_name_index{0};

    pure fn to_render_position(usize absolute_position) const wontthrow -> usize
    {
      return is_windowed
                 ? absolute_position - body_start_position + header_length
                 : absolute_position;
    }
  };
  pure fn resolve_render_source(const SourceLocation &location) const wontthrow
      -> resolved_render_source;
  mustuse fn sorted_function_names() const throws -> ArrayList<String>;
  fn find_function(StringView name) const wontthrow -> const Expression *;
  pure fn find_function_storage(StringView name) const wontthrow
      -> const FunctionBodyHandle *;
  pure fn has_functions() const wontthrow -> bool;
  pure fn function_storage_stats() const wontthrow -> function_arena_stats;
  pure fn has_aliases() const wontthrow -> bool;
  fn unset_function(StringView name) throws -> void;
  fn clear_functions() wontthrow -> void;

  fn function_names() const throws -> HashSet;
  template <typename Callback>
  fn for_each_function_name(Callback callback) const throws -> void
  {
    m_functions.for_each([&](StringView name, const FunctionBodyHandle &storage)
                             throws {
                               unused(storage);
                               callback(name);
                             });
  }

  fn register_completion_spec(StringView command, completion_spec spec) throws
      -> void;
  pure fn lookup_completion_spec(StringView command) const wontthrow
      -> const completion_spec *;
  fn register_default_completion_spec(completion_spec spec) throws -> void;
  pure fn default_completion_spec() const wontthrow -> const completion_spec *;
  pure fn completion_specs() const wontthrow
      -> const StringMap<completion_spec> &
  {
    return m_completion_specs;
  }
  /* out_exit_status receives the function's return status, so the engine sees
     the 124 a dynamic loader returns to request a retry. */
  fn run_completion_function(StringView function_name,
                             const ArrayList<String> &words, usize cword,
                             StringView line, usize point,
                             i32 *out_exit_status = nullptr) throws
      -> ArrayList<String>;
  /* allow_expansion off keeps the plain split with no shell expansion. */
  fn expand_wordlist_to_fields(StringView wordlist,
                               bool allow_expansion = true) throws
      -> ArrayList<String>;

  fn variable_names(Allocator result_allocator = heap_allocator()) const throws
      -> HashSet;
#if !defined NDEBUG
  pure fn debug_variable_name_enumeration_count() const wontthrow -> usize
  {
    return m_debug_variable_name_enumeration_count;
  }
#endif

  /* A signal condition installs the shell's handler. */
  fn set_trap(StringView condition, StringView action) throws -> void;
  fn remove_trap(StringView condition) throws -> void;
  pure fn traps() const wontthrow -> const StringMap<String> &;
  fn run_exit_trap() throws -> void;

  fn run_named_trap(StringView condition) throws -> void;
  pure fn has_debug_trap() const wontthrow -> bool { return m_has_debug_trap; }
  pure fn should_run_err_trap() const wontthrow -> bool
  {
    return m_runtime.option_is_enabled(shell_option_id::Errtrace) ||
           (m_function_call_depth == 0 && m_subshell_depth == 0 &&
            m_substitution_depth == 0);
  }
  pure fn should_run_return_trap() const wontthrow -> bool
  {
    return !is_posix_mode() &&
           (!is_bash_compatible() ||
            m_runtime.option_is_enabled(shell_option_id::Functrace));
  }

  /* Run the action of every signal whose flag the handler set, at the command
     boundary. A re-entrancy guard keeps a triggered signal from nesting. */
  fn run_pending_traps() throws -> void;
  fn has_exit_trap() const wontthrow -> bool;

  /* A subshell clears the inherited EXIT action on entry and fires its own on
     exit. */
  fn clear_inherited_exit_trap() throws -> void;
  fn run_subshell_exit_trap() throws -> void;

  fn mark_readonly(StringView name) throws -> void;
  fn unmark_readonly(StringView name) throws -> void;
  fn is_readonly(StringView name) const wontthrow -> bool;
  fn readonly_names() const throws -> ArrayList<String>;

  fn mark_integer(StringView name) throws -> void;
  fn unmark_integer(StringView name) throws -> void;
  fn is_integer_variable(StringView name) const wontthrow -> bool;
  fn mark_lowercase(StringView name) throws -> void;
  fn unmark_lowercase(StringView name) throws -> void;
  fn is_lowercase_variable(StringView name) const wontthrow -> bool;
  fn mark_uppercase(StringView name) throws -> void;
  fn unmark_uppercase(StringView name) throws -> void;
  fn is_uppercase_variable(StringView name) const wontthrow -> bool;
  /* The appended expression is parenthesized so its precedence stays
     self-contained. */
  fn append_integer_expression(String &joined,
                               StringView expression) const throws -> void;

  fn enter_function_scope() throws -> void;
  fn leave_function_scope() throws -> void;
  fn push_function_call_name(StringView name,
                             const FunctionBodyHandle &body_storage) throws
      -> void;
  fn pop_function_call_name() wontthrow -> void;
  /* The FUNCNAME frame list bash exposes, the function calls innermost first,
     one "source" per sourced file, and "main" at the bottom of a script run. */
  mustuse fn funcname_frame_count() const wontthrow -> usize;
  mustuse fn funcname_frame_at(usize index) const wontthrow -> StringView;
  /* A frame past the function calls reports zero. */
  mustuse fn funcname_line_at(usize index) const throws -> usize;
  /* A frame past the source stack reports an empty path. */
  mustuse fn funcname_source_at(usize index) const wontthrow -> StringView;
  /* The BASH_SOURCE frame list, the innermost sourced file first and the script
     name at the bottom. A frame past the stack reports an empty path. */
  mustuse fn bash_source_frame_at(usize index) const wontthrow -> StringView;
  mustuse fn bash_source_frame_count() const wontthrow -> usize;

  enum class CallStackVariable : u8
  {
    FunctionName,
    LineNumber,
    SourcePath,
  };
  mustuse fn call_stack_frame_count(CallStackVariable which) const wontthrow
      -> usize;
  mustuse fn call_stack_frame_text(CallStackVariable which, usize index,
                                   Allocator result_allocator) const throws
      -> String;

  mustuse fn
  line_number_at_location(const SourceLocation &location) const throws -> usize;
  fn set_script_run(bool is_script_run) wontthrow -> void
  {
    m_is_script_run = is_script_run;
  }
  pure fn is_script_run() const wontthrow -> bool { return m_is_script_run; }
  pure fn in_function_scope() const wontthrow -> bool;
  pure fn is_sourcing() const wontthrow -> bool
  {
    return m_source_depth > m_rejected_return_source_frames;
  }
  fn push_root_source_frame(const String *parent_source,
                            SourceLocation call_site,
                            bool is_only_root_source) throws -> void;
  fn pop_root_source_frame() wontthrow -> void;
  fn declare_local(StringView name, bool should_inherit_value) throws -> void;
  mustuse fn is_local_in_current_scope(StringView name) const wontthrow -> bool;

  fn set_alias(StringView name, StringView value) throws -> void;
  fn remove_alias(StringView name) throws -> bool;
  fn get_alias(StringView name) const throws -> Maybe<String>;
  fn alias_definitions() const throws -> ArrayList<String>;
  fn alias_names() const throws -> HashSet;
  template <typename Callback>
  fn for_each_alias_name(Callback callback) const throws -> void
  {
    m_aliases.for_each([&](StringView name, const String &value) throws {
      unused(value);
      callback(name);
    });
  }

  fn snapshot_state() throws -> eval_state_snapshot;
  fn restore_state(eval_state_snapshot snapshot) throws -> void;
  fn make_subshell_bootstrap() const throws -> os::subshell_bootstrap;
  fn apply_subshell_bootstrap(os::subshell_bootstrap bootstrap) throws -> void;

  fn enter_subshell() wontthrow -> void;
  fn leave_subshell() wontthrow -> void;
  pure fn get_subshell_depth() const wontthrow -> usize;
  fn set_subshell_depth(usize depth) wontthrow -> void;
  pure fn in_subshell() const wontthrow -> bool;
  /* Back the descriptor up before a bare exec moves it inside an in-process
     subshell, so leave_subshell restores it. The first backup per subshell
     wins. */
  fn snapshot_subshell_descriptor(i32 shell_fd) throws -> void;

  fn request_loop_control(control_flow::Kind kind, i64 level,
                          SourceLocation location) throws -> void;
  fn request_break(i64 level, SourceLocation location) throws -> void;
  fn request_continue(i64 level, SourceLocation location) throws -> void;
  fn request_return(i64 status, SourceLocation location) throws -> void;
  fn request_exit(i64 status, SourceLocation location) throws -> void;
  pure fn has_pending_control_flow() const wontthrow -> bool;
  fn pending_control_flow() wontthrow -> control_flow &;
  pure fn pending_control_flow() const wontthrow -> const control_flow &;
  fn clear_control_flow() wontthrow -> void;

  fn set_current_source(const String *source, String origin) wontthrow -> void;
  pure fn current_source() const wontthrow -> const String *;
  pure fn current_origin() const wontthrow -> const String &;
  fn set_current_history_event_number(Maybe<usize> number) wontthrow -> void;
  pure fn current_history_event_number() const wontthrow -> Maybe<usize>;
  pure fn history_recording_source_for(const Expression *root) const wontthrow
      -> Maybe<StringView>
  {
    if (root != m_history_recording_root) return None;
    return m_history_recording_source;
  }
  fn record_history_event(StringView command) throws -> bool;
  fn begin_history_transaction(ArrayList<String> &commands) throws -> void;
  fn end_history_transaction() wontthrow -> void;
  pure fn has_history_transaction() const wontthrow -> bool;
  fn begin_command_evaluation() wontthrow -> void;
  /* A frame at error_location is dropped. */
  fn print_source_backtrace(Maybe<SourceLocation> error_location = None,
                            bool should_defer_for_source_file = true) throws
      -> void;
  fn set_source_traces_enabled(bool enabled) wontthrow -> void
  {
    m_should_print_source_traces = enabled;
  }
  pure fn should_print_source_traces() const wontthrow -> bool
  {
    return m_should_print_source_traces;
  }

  fn set_diagnostic_highlight_cache(completion::shell_highlight_cache *cache)
      wontthrow -> completion::shell_highlight_cache *
  {
    let *previous = m_diagnostic_highlight_cache;
    m_diagnostic_highlight_cache = cache;
    return previous;
  }

  fn get_or_create_diagnostic_highlight_cache() throws
      -> completion::shell_highlight_cache *;
  fn reset_runtime_diagnostic_highlight_cache() wontthrow -> void;

  fn render_contained_substitution_error(const std::exception_ptr &error,
                                         StringView source) throws -> void;

  fn set_current_location(SourceLocation location) wontthrow -> void;

  fn set_shell_option_state(shell_option_id option, bool enabled) wontthrow
      -> void
  {
    m_runtime.set_option(option, enabled);
  }
  fn note_shell_option_mutation(shell_option_id option) wontthrow -> void
  {
    m_shell_option_mutations.note(option);
  }
  pure fn shell_option_state(shell_option_id option) const wontthrow -> bool
  {
    return m_runtime.option_is_enabled(option);
  }

  fn set_error_exit(bool enabled) wontthrow -> void;
  pure fn error_exit() const wontthrow -> bool;
  fn set_echo_expanded(bool enabled) wontthrow -> void;
  fn set_error_unset(bool enabled) wontthrow -> void;
  pure fn error_unset() const wontthrow -> bool;
  /* Marks the unset strictness as the script's own set -u rather than a mood
     seed, so the -W downgrade leaves it fatal. */
  fn set_error_unset_explicit(bool enabled) wontthrow -> void
  {
    m_runtime.set_error_unset_set_explicitly(enabled);
  }
  /* Mark a warning suppressed or not for the span of a construct. */
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
  fn set_warnings_enabled(bool enabled) wontthrow -> void
  {
    if (!enabled)
      m_runtime.warning_level = 0;
    else if (m_runtime.warning_level < 3)
      m_runtime.warning_level++;
  }
  fn note_warning_option_mutation() wontthrow -> void
  {
    m_warning_mutation_revision++;
  }
  pure fn warnings_enabled() const wontthrow -> bool
  {
    return m_runtime.warning_level > 0;
  }
  pure fn warning_level() const wontthrow -> u8
  {
    return m_runtime.warning_level;
  }
  fn set_warning_level(u8 level) wontthrow -> void
  {
    m_runtime.warning_level = level;
  }
  pure fn strict_diagnostics_are_warnings() const wontthrow -> bool
  {
    if (m_runtime.mood == mimic_mood::Default)
      return m_runtime.warning_level >= 3;
    return m_runtime.warning_level >= 1;
  }
  /* A reference to an unset variable, fatal under set -u, downgraded to a
     warning under -W unless the set -u was explicit, else expanded to empty. */
  fn report_unset_reference(StringView name) throws -> void;
  /* A suspicious runtime condition the strict default treats as fatal. Throws
     when fatal and not downgraded, warns under -W, returns otherwise. */
  fn warn_or_throw(bool fatal, bool explicitly_requested,
                   const SourceLocation &location, StringView message,
                   StringView note = {}) throws -> void;
  /* Renders a located runtime warning at the command being evaluated. The _at
     form takes a finer location inside that command. */
  cold fn show_runtime_warning(StringView message) wontthrow -> void;
  cold fn show_runtime_warning_at(SourceLocation location, StringView message,
                                  StringView note = {},
                                  bool should_ignore_disabled = false) wontthrow
      -> void;
  cold fn show_runtime_error_at(SourceLocation location,
                                StringView message) wontthrow -> void;
  /* The location of the $name or ${name spelling inside the command being
     evaluated. The statement location is the fallback when it is not found. */
  pure fn locate_variable_reference(StringView name) const wontthrow
      -> SourceLocation;

  fn set_pipefail(bool enabled) wontthrow -> void;
  pure fn pipefail() const wontthrow -> bool;
  /* Marks the pipeline strictness as the script's own set -o pipefail rather
     than a mood seed, so a later mood switch leaves it in place. */
  fn set_pipefail_explicit(bool enabled) wontthrow -> void
  {
    m_runtime.set_pipefail_set_explicitly(enabled);
  }

  fn set_no_clobber(bool enabled) wontthrow -> void;
  pure fn no_clobber() const wontthrow -> bool;
  fn set_export_all(bool enabled) wontthrow -> void;
  pure fn export_all() const wontthrow -> bool;
  fn set_no_glob(bool enabled) wontthrow -> void;
  pure fn no_glob() const wontthrow -> bool;
  fn set_no_exec(bool enabled) wontthrow -> void;
  pure fn no_exec() const wontthrow -> bool;
  fn set_extended_arithmetic(bool enabled) wontthrow -> void
  {
    m_runtime.set_option(shell_option_id::ExtendedArithmetic, enabled);
  }
  pure fn is_extended_arithmetic_enabled() const wontthrow -> bool
  {
    return m_runtime.option_is_enabled(shell_option_id::ExtendedArithmetic);
  }
  fn set_extended_arithmetic_explicit(bool enabled) wontthrow -> void
  {
    m_runtime.set_extended_arithmetic_set_explicitly(enabled);
  }
  fn set_koshkit(bool enabled) wontthrow -> void;
  pure fn koshkit() const wontthrow -> bool;
  pure fn koshkit_utilities_are_reachable() const wontthrow -> bool
  {
    return m_runtime.koshkit_utilities_are_reachable();
  }
  fn set_failglob(bool enabled) wontthrow -> void;
  pure fn failglob() const wontthrow -> bool;
  /* Marks the glob strictness as the script's own set -o failglob rather than
     a mood seed, so the -W downgrade leaves it fatal. */
  fn set_failglob_explicit(bool enabled) wontthrow -> void
  {
    m_runtime.set_failglob_set_explicitly(enabled);
  }
  /* True while a test or [ command expands its arguments, so an unmatched glob
     there stays a silent literal and the probe answers false rather than
     tripping failglob. */
  fn set_glob_exempt_for_test(bool enabled) wontthrow -> void
  {
    m_glob_exempt_for_test = enabled;
  }
  pure fn glob_exempt_for_test() const wontthrow -> bool
  {
    return m_glob_exempt_for_test;
  }
  /* The compgen -G probe, glob matches with failglob suppressed and a plain
     name reported only when the file exists. */
  fn expand_glob_lenient(StringView pattern) throws -> ArrayList<String>;

  pure fn is_bash_compatible() const wontthrow -> bool
  {
    return m_runtime.mood == mimic_mood::Bash ||
           m_runtime.mood == mimic_mood::BashPosix;
  }

  /* POSIX mood behaves like dash. The non-posix-breaking bash additions on in
     the default mood, such as the extended globs, read this to stay off here.
     BashPosix is bash in posix-option form, not the dash-like sh mood, so the
     bash additions and the [[ grammar stay on. */
  pure fn is_posix_mode() const wontthrow -> bool
  {
    return m_runtime.mood == mimic_mood::Posix;
  }

  pure fn is_posix_option_on() const wontthrow -> bool
  {
    return m_runtime.mood == mimic_mood::Posix ||
           m_runtime.mood == mimic_mood::BashPosix;
  }

  /* The mood the lexer reads. set_mood changes only the mood, so a caller that
     wants the strictness moved with it calls apply_strictness_for_mood after.
  */
  fn set_mood(mimic_mood mood) wontthrow -> void { m_runtime.mood = mood; }
  pure fn mood() const wontthrow -> mimic_mood { return m_runtime.mood; }

  /* The set -o posix form enters the BashPosix mood, and set +o posix steps
     down to bash when already in BashPosix or the dash-like Posix mood. A
     non-posix mood is left alone, since the mood is not a stack and the prior
     mood is not recoverable. The explicit mark and the strictness follow the
     switch the way set --mood does.
  */
  fn set_posix_mode_via_option(bool enable) wontthrow -> void
  {
    if (enable) {
      note_explicit_mood();
      set_mood(mimic_mood::BashPosix);
      apply_strictness_for_mood();
      return;
    }
    if (m_runtime.mood != mimic_mood::Posix &&
        m_runtime.mood != mimic_mood::BashPosix)
      return;
    note_explicit_mood();
    set_mood(mimic_mood::Bash);
    apply_strictness_for_mood();
  }

  fn set_execution_string(StringView text) throws -> void
  {
    m_execution_string = String{heap_allocator(), text};
    m_has_execution_string = true;
  }
  pure fn has_execution_string() const wontthrow -> bool
  {
    return m_has_execution_string;
  }

  fn set_current_command(String text) throws -> void
  {
    m_current_command = steal(text);
  }

  /* While listing makefile targets for completion, the bundled make parser
     leaves $(shell ...) unrun, so a tab never forks the makefile's commands and
     never blocks on a slow one. */
  fn set_make_shell_suppressed(bool suppressed) wontthrow -> void
  {
    m_make_shell_suppressed = suppressed;
  }
  pure fn make_shell_suppressed() const wontthrow -> bool
  {
    return m_make_shell_suppressed;
  }

  fn apply_strictness_for_mood() wontthrow -> void
  {
    let const strict = m_runtime.mood == mimic_mood::Default;
    if (!m_runtime.was_error_unset_set_explicitly())
      set_error_unset(strict && !is_completion_function_running());
    if (!m_runtime.was_pipefail_set_explicitly()) set_pipefail(strict);
    if (!m_runtime.was_failglob_set_explicitly())
      set_failglob(strict && !is_completion_function_running());
    if (!m_runtime.was_extended_arithmetic_set_explicitly())
      set_extended_arithmetic(strict);
  }

  friend class RuntimeState;
  fn enter_definition_state(const RuntimeState &defining_runtime) wontthrow
      -> function_runtime_state
  {
    let const previous = RuntimeState::capture(*this);
    m_runtime.mood = defining_runtime.mood;
    set_warning_level(defining_runtime.warning_level);
    m_runtime.set_diagnostics_disabled(
        defining_runtime.is_diagnostics_disabled());
    m_runtime.set_annoying_diagnostics_enabled(
        defining_runtime.is_annoying_diagnostics_enabled());
    apply_strictness_for_mood();
    return function_runtime_state{previous,
                                  RuntimeState::capture(*this),
                                  m_shell_option_mutations,
                                  m_shell_option_mutations.revision,
                                  m_mood_mutation_revision,
                                  m_warning_mutation_revision,
                                  m_diagnostics_mutation_revision,
                                  m_annoying_diagnostics_mutation_revision,
                                  m_was_mood_set_explicitly};
  }

  fn leave_definition_state(
      const function_runtime_state &state,
      definition_state_exit exit =
          definition_state_exit::PropagateMutations) wontthrow -> void
  {
    if (exit == definition_state_exit::RestoreCaller) {
      state.previous.restore(*this);
      m_shell_option_mutations = state.previous_shell_option_mutations;
      m_mood_mutation_revision = state.mood_mutation_revision;
      m_warning_mutation_revision = state.warning_mutation_revision;
      m_diagnostics_mutation_revision = state.diagnostics_mutation_revision;
      m_annoying_diagnostics_mutation_revision =
          state.annoying_diagnostics_mutation_revision;
      m_was_mood_set_explicitly = state.was_mood_set_explicitly;
      return;
    }

    let const finished = RuntimeState::capture(*this);
    let changed_options = state.entered.shell_options ^ finished.shell_options;
    if (state.mood_mutation_revision != m_mood_mutation_revision) {
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
      if (m_shell_option_mutations.touched_since(
              option_id, state.shell_option_mutation_revision))
        changed_options |= RuntimeState::option_mask(option_id);
    }
    let const merged_options =
        (state.previous.shell_options & ~changed_options) |
        (finished.shell_options & changed_options);

    state.previous.restore(*this);
    m_runtime.shell_options = merged_options;
    if (m_shell_option_mutations.touched_since(
            shell_option_id::Nounset, state.shell_option_mutation_revision))
      m_runtime.set_error_unset_set_explicitly(
          finished.was_error_unset_set_explicitly());
    if (m_shell_option_mutations.touched_since(
            shell_option_id::Pipefail, state.shell_option_mutation_revision))
      m_runtime.set_pipefail_set_explicitly(
          finished.was_pipefail_set_explicitly());
    if (m_shell_option_mutations.touched_since(
            shell_option_id::Failglob, state.shell_option_mutation_revision))
      m_runtime.set_failglob_set_explicitly(
          finished.was_failglob_set_explicitly());
    if (m_shell_option_mutations.touched_since(
            shell_option_id::ExtendedArithmetic,
            state.shell_option_mutation_revision))
      m_runtime.set_extended_arithmetic_set_explicitly(
          finished.was_extended_arithmetic_set_explicitly());
    if (state.mood_mutation_revision != m_mood_mutation_revision)
      m_runtime.mood = finished.mood;
    if (state.warning_mutation_revision != m_warning_mutation_revision)
      m_runtime.warning_level = finished.warning_level;
    if (state.diagnostics_mutation_revision != m_diagnostics_mutation_revision)
      m_runtime.set_diagnostics_disabled(finished.is_diagnostics_disabled());
    if (state.annoying_diagnostics_mutation_revision !=
        m_annoying_diagnostics_mutation_revision)
      m_runtime.set_annoying_diagnostics_enabled(
          finished.is_annoying_diagnostics_enabled());
  }

  /* The moods whose startup files are being sourced right now, a bit per mood.
     source_init_moods marks a flavor while it sources it and skips a flavor the
     bit already names, so a set --init-moods inside a sourced ~/.koshrc cannot
     re-source the same rc and recurse without end. */
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

  /* set --mood records that the user chose the mood, so the post-rc restore in
     main leaves a mood the rc selected in place. */
  fn note_explicit_mood() wontthrow -> void
  {
    m_was_mood_set_explicitly = true;
    m_mood_mutation_revision++;
  }
  pure fn was_mood_set_explicitly() const wontthrow -> bool
  {
    return m_was_mood_set_explicitly;
  }

  /* The moods whose startup files have finished sourcing this session, so set
     --init-moods with no value reports what loaded. */
  fn mark_mood_initialized(mimic_mood mood) wontthrow -> void
  {
    m_initialized_moods |= static_cast<u8>(1U << static_cast<u8>(mood));
  }
  pure fn mood_initialized(mimic_mood mood) const wontthrow -> bool
  {
    return (m_initialized_moods & (1U << static_cast<u8>(mood))) != 0;
  }

  fn note_diagnostics_option_mutation() wontthrow -> void
  {
    m_diagnostics_mutation_revision++;
  }
  fn note_annoying_diagnostics_option_mutation() wontthrow -> void
  {
    m_annoying_diagnostics_mutation_revision++;
  }

  fn set_mimicry(bool enabled) wontthrow -> void
  {
    m_runtime.set_option(shell_option_id::Mimicry, enabled);
  }
  pure fn mimicry() const wontthrow -> bool
  {
    return m_runtime.option_is_enabled(shell_option_id::Mimicry);
  }

  /* Run the script at the resolved program in-process in the matching mode.
     When isolated is true the run is contained in a snapshotted subshell, and
     when false the snapshot is skipped. */
  fn run_mimicked_script(ExecContext &ec, mimic_mood mode,
                         script_isolation isolation) throws -> i32;
  fn run_program_fallback(ExecContext &ec, mimic_mood mode,
                          script_isolation isolation) throws -> i32;
  pure fn extglob_enabled() const wontthrow -> bool
  {
    return m_runtime.mood != mimic_mood::Posix && is_shopt_enabled("extglob");
  }

  pure fn bash_dynamic_variables_enabled() const wontthrow -> bool
  {
    return m_runtime.mood != mimic_mood::Posix;
  }

  pure fn bash_additions_enabled() const wontthrow -> bool
  {
    return m_runtime.mood != mimic_mood::Posix;
  }

  fn set_shopt_option(StringView name, bool enabled) throws -> void
  {
    let const index = shopt_option_index(name);
    ASSERT(index.has_value(), "unknown shopt option");
    let const mask = u64{1} << *index;
    m_shopt_option_overrides |= mask;
    if (enabled)
      m_shopt_option_values |= mask;
    else
      m_shopt_option_values &= ~mask;
  }
  pure fn is_shopt_enabled(StringView name) const wontthrow -> bool
  {
    if (name == "restricted_shell") return is_restricted_shell();
    let const index = shopt_option_index(name);
    if (!index.has_value()) return false;
    let const mask = u64{1} << *index;
    if ((m_shopt_option_overrides & mask) != 0)
      return (m_shopt_option_values & mask) != 0;
    if (name == "extglob") return m_runtime.mood == mimic_mood::Default;
    if (name == "expand_aliases")
      return m_runtime.mood != mimic_mood::Bash || shell_is_interactive();
    return shopt_default_is_on(name);
  }
  pure fn is_shopt_enabled(shopt_option_id option) const wontthrow -> bool
  {
    let const mask = u64{1} << shopt_option_index(option);
    if ((m_shopt_option_overrides & mask) != 0)
      return (m_shopt_option_values & mask) != 0;
    switch (option) {
    case shopt_option_id::Progcomp:
    case shopt_option_id::Sourcepath: return true;
    default: return false;
    }
  }
  /* Whether bash ships the named shopt option enabled, the miss fallback for
     is_shopt_enabled. */
  static pure fn shopt_default_is_on(StringView name) wontthrow -> bool;

  fn enter_condition() wontthrow -> void;
  fn leave_condition() wontthrow -> void;
  pure fn in_condition() const wontthrow -> bool;

  /* The count of loops currently running, the cap the break and continue
     builtins clamp their level to. A function call and a subshell zero it. */
  fn enter_loop() wontthrow -> void;
  fn leave_loop() wontthrow -> void;
  pure fn loop_depth() const wontthrow -> usize;
  fn set_loop_depth(usize depth) wontthrow -> void;

  /* The run loop sets this before the final chunk when the shell will exit with
     that chunk's status and no EXIT trap is pending, so a terminal external
     command replaces the shell process instead of fork and wait. */
  fn set_terminal_exec_allowed(bool enabled) wontthrow -> void;

  fn set_completion_function_running(bool running) wontthrow -> void
  {
    m_is_completion_function_running = running;
  }
  pure fn is_completion_function_running() const wontthrow -> bool
  {
    return m_is_completion_function_running;
  }
  fn set_prompt_command_running(bool running) wontthrow -> void
  {
    m_is_prompt_command_running = running;
  }
  pure fn is_prompt_command_running() const wontthrow -> bool
  {
    return m_is_prompt_command_running;
  }
  fn get_prompt_command_arena() wontthrow -> BumpArena &
  {
    return m_prompt_command_arena;
  }
  fn get_prompt_command_cached_text() wontthrow -> String &
  {
    return m_prompt_command_cached_text;
  }
  pure fn get_prompt_command_cached_ast() const wontthrow -> Expression *
  {
    return m_prompt_command_cached_ast;
  }
  fn set_prompt_command_cached_ast(Expression *ast) wontthrow -> void
  {
    m_prompt_command_cached_ast = ast;
  }
  fn get_foreground_program_title_buffer() wontthrow -> String &
  {
    return m_foreground_program_title_buffer;
  }

  /* Whether a builtin is running as a stage of a multi-stage pipeline. exec
     reads it so exec in a pipeline stage spawns a child rather than replacing
     the whole shell. */
  fn set_in_pipeline_stage(bool in_stage) wontthrow -> void
  {
    m_is_in_pipeline_stage = in_stage;
  }
  pure fn is_in_pipeline_stage() const wontthrow -> bool
  {
    return m_is_in_pipeline_stage;
  }
  pure fn terminal_exec_allowed() const wontthrow -> bool;

  fn sorted_variable_assignments() const throws -> ArrayList<String>;

  fn expand_word_for_assignment(const Word &word) throws -> String;

  fn evaluate_arithmetic(StringView expression,
                         const SourceLocation *expression_base = nullptr) throws
      -> i64;
  fn evaluate_arithmetic_text(
      StringView expression,
      const SourceLocation *expression_base = nullptr) throws -> String;
  fn evaluate_calculator_arithmetic_text(
      StringView expression,
      const SourceLocation *expression_base = nullptr) throws -> String;
  fn evaluate_bc_arithmetic_text(StringView expression, u32 scale) throws
      -> String;
  fn evaluate_arithmetic_nonzero(
      StringView expression,
      const SourceLocation *expression_base = nullptr) throws -> bool;
  fn compare_arithmetic(StringView left, StringView right) throws -> i32;
  fn evaluate_arithmetic_cached_text(const WordSegment &segment) throws
      -> String;

  /* The same value as evaluate_arithmetic, but a substitution-free expression
     lexes its tokens once onto the segment and re-evaluates from them. */
  fn evaluate_arithmetic_cached(const WordSegment &segment) throws -> i64;

  /* The same value as evaluate_arithmetic, but it lexes the clause once into
     the caller-owned token store and re-evaluates from it. A complex clause or
     a lexing failure falls back to the char parser, and a clause holding a
     substitution skips the cache. */
  fn evaluate_arithmetic_cached_clause(
      StringView expression, ArrayList<arith_token> &tokens, bool &is_tokenized,
      bool &is_simple, const SourceLocation *source_location = nullptr) throws
      -> i64;
  fn evaluate_arithmetic_cached_clause_nonzero(
      StringView expression, ArrayList<arith_token> &tokens, bool &is_tokenized,
      bool &is_simple, const SourceLocation *source_location = nullptr) throws
      -> bool;

  /* Evaluate a [[ ]] conditional element list and report whether it is true.
     The operands expand without field splitting, == and != glob match their
     right side, < and > compare strings, and && and || join primaries. */
  fn evaluate_conditional(const ArrayList<conditional_element> &elements) throws
      -> bool;

  /* Expand a case pattern word the same way assignment context expands, plus a
     parallel mask of which output bytes may act as glob metacharacters, so a
     quoted metacharacter in the pattern matches literally. */
  fn expand_case_pattern_masked(const Word &word, Bitset &active_out) throws
      -> String;

  /* Run the source of a $(...) and return its standard output with trailing
     newlines stripped. The inner command runs in-process with state
     snapshotted. The filename, when given, backs the source locations the
     parsed AST carries, so its bytes must outlive the parse arena. */
  fn capture_command_substitution(
      const String &source, Maybe<StringView> filename = None,
      const SourceLocation *call_site = nullptr) throws -> String;

  /* Same capture, but the segment caches its parsed inner command so a $(...)
     in a loop body is lexed and parsed once and re-evaluated thereafter. */
  fn capture_command_substitution(const WordSegment &segment) throws -> String;

  /* Run the source of a ${ ...; } funsub and return its standard output with
     trailing newlines stripped, the bash 5.3 form. The body runs in the current
     shell with no snapshot, so its assignments persist. A break, continue, or
     return is consumed inside it, while an exit stays pending. */
  fn capture_function_substitution(const WordSegment &segment) throws -> String;
  fn push_substitution_source_frame(const WordSegment &segment,
                                    StringView origin) throws -> bool;
  fn push_substitution_source_frame(const SourceLocation &location,
                                    StringView origin) throws -> bool;

  /* The $(< file) shorthand reads the named file directly, when the
     substitution body is only an input redirection naming one word with no
     command. None when the body is anything else. */
  fn read_redirect_substitution(StringView source) throws -> Maybe<String>;

  /* Run a <(...) or >(...) process substitution. A pipe is opened, the inner
     command runs in a forked child on one end, and the shell keeps the other
     end open and returns its /dev/fd path. The descriptor and the child are
     recorded for later cleanup. */
  fn setup_process_substitution(const WordSegment &segment) throws -> String;
  /* Close the descriptors and reap the children of the process substitutions a
     command opened. Closing first sends SIGPIPE to a producer that has more to
     write, so it ends rather than blocking the reap. */
  mustuse fn mark_process_substitutions() const wontthrow
      -> process_substitution_mark;
  fn cleanup_process_substitutions(process_substitution_mark mark) wontthrow
      -> void;

  mustuse fn mark_loop_redirect_fds() const wontthrow -> loop_redirect_fd_mark;
  fn cleanup_loop_redirect_fds(loop_redirect_fd_mark mark) wontthrow -> void;
  mustuse fn find_loop_redirect_fd(i32 target_fd, const String &path,
                                   os::file_open_mode mode) const wontthrow
      -> Maybe<os::descriptor>;
  mustuse fn retain_loop_redirect_fd(i32 target_fd, const String &path,
                                     os::file_open_mode mode,
                                     os::descriptor fd) throws -> bool;

  fn run_captured_substitution(const Expression *ast,
                               const String &source) throws -> String;

  /* Lex, parse, and evaluate a chunk of source in this context, without
     capturing output or snapshotting state. A dot-source consumes a return at
     the top of the chunk and ends there, an eval leaves it pending, so
     consume_return is false for eval. */
  fn run_source(StringView source, StringView origin = "a sourced command",
                return_handling handling = return_handling::Consume,
                Maybe<SourceLocation> call_site = None,
                Maybe<StringView> filename = None,
                bool should_record_history = false) throws -> i32;
  fn resolve_source_path(StringView path,
                         bool should_expand_tilde = false) throws
      -> Maybe<Path>;

  /* Each throws a located error past the recursion cap. */
  fn enter_source(const SourceLocation &location) throws -> void;
  fn leave_source() wontthrow -> void;
  fn enter_function_call(const SourceLocation &location) throws -> void;
  fn leave_function_call() wontthrow -> void;
  fn enter_substitution() throws -> void;
  fn leave_substitution() wontthrow -> void;
  pure fn get_substitution_depth() const wontthrow -> usize;
  fn enter_parameter_expansion() throws -> void;
  fn leave_parameter_expansion() wontthrow -> void;

  /* getopts keeps the position inside the current argument here, so -abc is
     parsed one letter per call. last_optind detects an OPTIND reset. */
  pure fn getopts_char_index() const wontthrow -> usize;
  fn set_getopts_char_index(usize index) wontthrow -> void;
  pure fn getopts_last_optind() const wontthrow -> i64;
  fn set_getopts_last_optind(i64 optind) wontthrow -> void;

  fn clear_retained_sources() wontthrow -> void;

  fn retain_ast(Expression *ast) throws -> void;

  fn expand_heredoc_body(StringView body,
                         const SourceLocation *source_location = nullptr) throws
      -> String;

  fn expand_modifier_word(
      StringView word, bool remove_quotes = true,
      bool strip_escaped_literals = true,
      const SourceLocation *source_location = nullptr) throws -> String;

  /* active_out marks which output bytes may act as glob metacharacters, so
     ${x#pat} and ${x%pat} match literally. */
  fn expand_modifier_word_masked(
      StringView word, Bitset &active_out, bool remove_quotes = true,
      const SourceLocation *source_location = nullptr) throws -> String;

  /* is_pattern_word makes a backslash quote the following byte, the # and %
     rule. */
  fn expand_modifier_word_worker(StringView word, Bitset *active_out,
                                 bool remove_quotes, bool is_pattern_word,
                                 bool strip_escaped_literals,
                                 const SourceLocation *source_location) throws
      -> String;

  pure fn should_echo() const wontthrow -> bool;
  fn write_xtrace(StringView command) throws -> void;
  fn write_xtrace(const ArrayList<String> &args) throws -> void;
  fn set_echo(bool enabled) wontthrow -> void
  {
    m_runtime.set_option(shell_option_id::Verbose, enabled);
  }
  pure fn should_echo_expanded() const wontthrow -> bool;
  pure fn shell_is_interactive() const wontthrow -> bool;

  pure fn startup_finished() const wontthrow -> bool
  {
    return m_startup_finished;
  }
  fn set_startup_finished() wontthrow -> void
  {
    m_startup_finished = true;
    if (m_is_restricted_shell) activate_restricted_mode();
  }
  fn request_restricted_shell() wontthrow -> void
  {
    m_is_restricted_shell = true;
  }
  pure fn is_restricted_shell() const wontthrow -> bool
  {
    return m_is_restricted_shell;
  }
  fn activate_restricted_mode() wontthrow -> void
  {
    m_runtime.set_option(shell_option_id::Restricted, true);
  }
  pure fn restricted_enforcement_active() const wontthrow -> bool
  {
    return m_runtime.option_is_enabled(shell_option_id::Restricted);
  }
  fn guard_restricted_path(StringView path, const SourceLocation &location,
                           restricted_path_use use) const throws -> void;

  fn make_stats_string() const throws -> String;

  fn set_stats_enabled(bool enabled) wontthrow -> void;
  pure fn stats_enabled() const wontthrow -> bool;

  fn set_show_ast(bool enabled) wontthrow -> void;
  pure fn show_ast() const wontthrow -> bool;
  fn set_show_lexed_words(bool enabled) wontthrow -> void;
  pure fn show_lexed_words() const wontthrow -> bool;
  fn set_show_exit_code(bool enabled) wontthrow -> void;
  pure fn show_exit_code() const wontthrow -> bool;

  /* The granular memory report at exit, requested by --show-memory. */
  fn set_memory_stats_enabled(bool enabled) wontthrow -> void;
  pure fn memory_stats_enabled() const wontthrow -> bool;

  /* The --no-diagnostics skip, so set -o no-diagnostics flips the per-chunk
     analysis gate at runtime. */
  fn set_diagnostics_disabled(bool disabled) wontthrow -> void;
  pure fn diagnostics_disabled() const wontthrow -> bool;
  fn set_annoying_diagnostics_enabled(bool enabled) wontthrow -> void;
  pure fn annoying_diagnostics_enabled() const wontthrow -> bool;

  /* The startup facts set -o reports read-only, mirrored from the invocation
     flags once at startup and fixed for the session. */
  fn set_login_shell(bool enabled) wontthrow -> void;
  pure fn is_login_shell() const wontthrow -> bool;
  fn set_custom_rcfile(bool enabled) wontthrow -> void;
  pure fn has_custom_rcfile() const wontthrow -> bool;

  pure fn last_expressions_executed() const wontthrow -> usize;
  pure fn total_expressions_executed() const wontthrow -> usize;

  pure fn last_expansion_count() const wontthrow -> usize;
  pure fn total_expansion_count() const wontthrow -> usize;

  pure fn commands_evaluated() const wontthrow -> usize;
  pure fn peak_ast_arena_bytes() const wontthrow -> usize;

protected:
  bool m_is_login_shell{false};
  bool m_has_custom_rcfile{false};
  bool m_is_restricted_shell{false};
  usize m_expressions_executed_last{0};
  usize m_expressions_executed_total{0};
  usize m_expansions_last{0};
  usize m_expansions_total{0};
  usize m_commands_evaluated{0};
  usize m_command_evaluation_index{0};
  mutable usize m_git_branch_command_index{static_cast<usize>(-1)};
  mutable usize m_git_counts_command_index{static_cast<usize>(-1)};
  mutable String m_git_branch{heap_allocator()};
  mutable i32 m_git_ahead_count{0};
  mutable i32 m_git_behind_count{0};
  /* The largest live AST arena footprint seen at the end of any command. */
  usize m_peak_ast_arena_bytes{0};

  mutable BumpArena m_scratch_arena{};
  StringMap<String> m_shell_variables{heap_allocator()};
  StringMap<ArrayList<String>> m_indexed_arrays{heap_allocator()};
  StringMap<completion_spec> m_completion_specs{heap_allocator()};
  Maybe<completion_spec> m_default_completion_spec{};
  HashSet m_associative_names{heap_allocator()};
  StringMap<String> m_associative_values{heap_allocator()};
  /* An indexed array element whose subscript is past the dense limit, held by
     its name and decimal index so a sparse far subscript does not pad a huge
     dense gap. The name still reads as indexed. */
  StringMap<String> m_sparse_array_values{heap_allocator()};
  HashSet m_sparse_array_names{heap_allocator()};
  u64 m_shopt_option_overrides{0};
  u64 m_shopt_option_values{0};
  /* The compiled form of each [[ =~ ]] pattern, keyed by the pattern text, so a
     hot loop with a constant regex compiles it once and reuses it. */
  StringMap<CompiledRegex> m_regex_cache{heap_allocator()};
  /* The cached value of IFS, kept current by set_shell_variable, so word
     splitting does not look it up per word. */
  String m_field_separators{" \t\n"};

  /* A byte-indexed table that answers whether a character is a field separator
     in one load, instead of scanning IFS per byte. It is rebuilt whenever IFS
     changes. */
  u64 m_field_separator_bits[4]{};
  pure fn is_field_separator(char c) const wontthrow -> bool;
  i32 m_last_exit_status{0};

  u64 m_last_command_duration_nanos{0};

  String m_shell_name{heap_allocator()};
  String m_shell_executable_path{heap_allocator()};
  String m_last_argument{heap_allocator()};
  String m_execution_string{heap_allocator()};
  bool m_has_execution_string{false};
  String m_current_command{heap_allocator()};
  bool m_make_shell_suppressed{false};
  ArrayList<String> m_positional_params{heap_allocator()};
  /* The saved directories below the current one, back is the top of the stack.
     pushd appends the current directory, popd drops the back and moves to it.
   */
  ArrayList<String> m_directory_stack{heap_allocator()};
  Maybe<i64> m_last_background_pid{};
  StringMap<FunctionBodyHandle> m_functions{heap_allocator()};
  usize m_subshell_depth{0};
  /* The descriptors bare execs moved inside live in-process subshells, kept
     as a stack so leave_subshell unwinds its own depth's entries in reverse. */
  ArrayList<subshell_saved_descriptor> m_subshell_saved_descriptors{
      heap_allocator()};
  usize m_condition_depth{0};
  usize m_loop_depth{0};

  /* The prior values of process-environment names written while a subshell ran,
     rewound by restore_state on the subshell's exit. The log is appended to
     only while m_subshell_depth is above zero, so a top-level export pays
     nothing. */
  ArrayList<environment_undo_entry> m_environment_undo_log{heap_allocator()};
  /* The names currently in the process environment, kept in step with every
     environment write so an assignment tests membership in O(1). */
  HashSet m_exported_names{heap_allocator()};
#if !defined NDEBUG
  mutable usize m_debug_variable_name_enumeration_count{0};
#endif
  ArrayList<process_substitution> m_pending_process_substitutions{
      heap_allocator()};
  ArrayList<loop_redirect_fd> m_loop_redirect_fds{heap_allocator()};

  /* The nesting depth of dot-source and eval runs, and of function calls, each
     bounded so a runaway recursion errors with a located message rather than
     growing the native stack until the process is killed. */
  usize m_source_depth{0};
  usize m_rejected_return_source_frames{0};
  usize m_function_call_depth{0};
  usize m_substitution_depth{0};
  usize m_parameter_expansion_depth{0};

  /* Set once the startup files finish, so the per-command title is quiet while
     they run. */
  bool m_startup_finished{false};

  /* The pending non-local jump, Normal when none is pending. */
  control_flow m_control_flow{};
  /* The source and name of the text being evaluated, for caret formatting. */
  const String *m_current_source{nullptr};
  String m_current_origin{heap_allocator()};
  Maybe<usize> m_current_history_event_number{None};
  const Expression *m_history_recording_root{nullptr};
  StringView m_history_recording_source{};
  ArrayList<ArrayList<String> *> m_history_transaction_stack{heap_allocator()};

  /* The location in m_current_source of the command being evaluated, read by
     $LINENO for its line and by the runtime warnings for their caret. The whole
     location is kept so the filename the lexer stamped rides into a warning. */
  SourceLocation m_current_location{};

  /* The chain of sourced-file, eval, and substitution frames from the
     outermost down to the one running now, so an error deep in a nested source
     prints every call site. Each frame carries the call site and its parent
     text. */
  ArrayList<source_frame> m_source_frames{heap_allocator()};
  bool m_should_print_source_traces{true};
  completion::shell_highlight_cache *m_diagnostic_highlight_cache{nullptr};
  completion::shell_highlight_cache *m_runtime_diagnostic_highlight_cache{
      nullptr};

  ArrayList<Expression *> m_retained_source_asts{heap_allocator()};

  /* The source text of each eval and dot run is retained for escaped locations.
     The buffers are heap-owned pointers, not inline elements, so a
     nested run_source that grows the list never moves an earlier buffer and
     leaves m_current_source or a control_flow::source dangling. */
  ArrayList<String *> m_retained_sources{heap_allocator()};

  /* The mood and the diagnostic and strictness toggles, grouped as one runtime
     state so a scope that swaps them saves and restores the whole set with one
     RuntimeState copy. failglob defaults on, the other toggles default off. */
  RuntimeState m_runtime{};
  ProgramResolver m_program_resolver{};
  u8 m_init_moods_sourcing{0};
  u8 m_initialized_moods{0};
  bool m_was_mood_set_explicitly{false};
  u64 m_mood_mutation_revision{0};
  u64 m_warning_mutation_revision{0};
  u64 m_diagnostics_mutation_revision{0};
  u64 m_annoying_diagnostics_mutation_revision{0};
  shell_option_mutations m_shell_option_mutations{};
  /* One bit per suppressible_warning value. */
  u32 m_suppressed_warnings{0};
  /* The nesting of mimicked scripts, bounded so a script that mimics another
     cannot recurse without limit. */
  usize m_mimicry_depth{0};
  /* The base $SECONDS counts from. */
  i64 m_shell_start_time{0};
  mutable u64 m_random_state{0};
  bool m_glob_exempt_for_test{false};
  usize m_getopts_char_index{1};
  i64 m_getopts_last_optind{0};
  StringMap<String> m_traps{heap_allocator()};
  bool m_has_debug_trap{false};
  bool m_exit_trap_ran{false};
  /* True while run_pending_traps is draining, so a signal delivered during a
     trap action does not nest a second drain. */
  bool m_running_traps{false};
  bool m_terminal_exec_allowed{false};
  bool m_is_completion_function_running{false};
  bool m_is_prompt_command_running{false};
  bool m_is_in_pipeline_stage{false};
  BumpArena m_prompt_command_arena{};
  String m_prompt_command_cached_text{heap_allocator()};
  Expression *m_prompt_command_cached_ast{nullptr};
  String m_foreground_program_title_buffer{heap_allocator()};

  fn install_trap_dispositions() throws -> void;

  StringMap<u8> m_variable_attributes{heap_allocator()};
  StringMap<String> m_aliases{heap_allocator()};
  /* One entry per active function call, holding the bindings a local shadowed.
   */
  ArrayList<ArrayList<local_binding>> m_local_scopes{heap_allocator()};
  usize m_local_scope_depth{0};
  ArrayList<String> m_function_call_names{heap_allocator()};
  ArrayList<FunctionBodyHandle> m_function_call_storages{heap_allocator()};
  /* The call-site location of each active function call, parallel to
     m_function_call_names, read by BASH_LINENO. */
  ArrayList<SourceLocation> m_function_call_locations{heap_allocator()};
  bool m_is_script_run{false};
  /* The count of source frames that carry a file path, for the FUNCNAME
     classification. */
  usize m_sourced_file_frames{0};

  ArrayList<job> m_jobs{heap_allocator()};
  ArrayList<os::process> m_detached_job_processes{heap_allocator()};
  i32 m_next_job_id{1};
  bool m_shell_is_interactive;

  fn option_flags_string() const throws -> String;

  fn expand_variable(StringView name) const throws -> String;

  /* Write a variable without the read-only check, for restoring a shadowed
     local on function return where a throw from a noexcept defer would
     terminate the shell. */
  fn assign_variable(StringView name, StringView value) throws -> void;

  pure fn variable_attributes(StringView name) const wontthrow -> u8;
  fn set_variable_attribute(StringView name, variable_attribute attribute,
                            bool is_enabled) throws -> void;
  fn apply_variable_case(StringView name, String &value) const wontthrow
      -> void;

  fn force_unset_shell_variable(StringView name) throws -> void;
  /* The unset peel, the bash upvar semantics. A local declared by a caller
     rather than the current scope restores that caller's saved value now and
     cancels the restore its scope pop would have run. Returns whether a binding
     was peeled. */
  fn peel_caller_local_binding(StringView name) throws -> bool;
  /* The one restore a saved local binding gets, the scalar, the arrays, and
     the integer mark, shared by the scope pop and the unset peel. */
  fn restore_local_binding(local_binding &binding) throws -> void;

  fn apply_parameter_expansion(StringView spec,
                               const SourceLocation *source_location = nullptr,
                               usize source_location_offset = 0) throws
      -> String;

  fn apply_substring_expansion(
      StringView name, StringView body,
      const SourceLocation *source_location = nullptr) throws -> String;
  fn apply_substring_to_value(
      StringView value, StringView body,
      const SourceLocation *source_location = nullptr) throws -> String;

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
  fn apply_parameter_transform_to_value(StringView value, char op,
                                        StringView name) throws -> String;
  fn apply_case_modification_to_value(
      StringView value, StringView spec,
      const SourceLocation *source_location = nullptr) throws -> String;
  fn apply_value_modifier(
      StringView value, StringView modifier,
      const SourceLocation *source_location = nullptr) throws -> String;

  fn apply_array_subscript(
      StringView name, StringView subscript,
      const SourceLocation *source_location = nullptr) throws -> String;
  fn array_negative_index_base(StringView name) const throws -> i64;

  fn apply_indirect_or_name_listing(StringView body) throws -> String;

  fn matching_prefix_names(StringView prefix) const throws -> ArrayList<String>;

  fn expand_word(const Word &word) throws -> ArrayList<glob_field>;

  fn expand_path_once(const glob_field &field, bool should_expand_files) throws
      -> ArrayList<glob_field>;
  fn expand_path_recurse(ArrayList<glob_field> fields) throws
      -> ArrayList<glob_field>;
  fn expand_path(glob_field field, const SourceLocation &location) throws
      -> ArrayList<String>;

  fn expand_tilde(WordSegment &leading_segment, bool word_continues,
                  bool stop_at_colon) const throws -> void;
  fn resolve_tilde_prefix(StringView name) const throws -> Maybe<String>;
  fn expand_colon_tildes(WordSegment &segment, bool word_continues) const throws
      -> void;
};

} /* namespace koshka */
