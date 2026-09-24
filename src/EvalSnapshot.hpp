/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file defines the move-only evaluator state captured around isolated
 * execution, including shell options, directories, Bash argument frames,
 * completion specifications, and compiled regular expressions. These values
 * live here because isolated evaluation must restore them as one move-only
 * unit after the nested evaluator finishes.
 */

#pragma once

#include "Builtin.hpp"
#include "Errors.hpp"
#include "EvalTypes.hpp"
#include "MimicMood.hpp"
#include "Platform.hpp"
#include "ProgramResolver.hpp"
#include "base/Arena.hpp"
#include "base/Bitset.hpp"
#include "base/Common.hpp"
#include "base/Containers.hpp"
#include "base/Maybe.hpp"
#include "base/Path.hpp"

namespace koshka {

struct completion_spec
{
  String function_name{heap_allocator()};
  String word_list{heap_allocator()};
  bool should_use_default{false};
  RuntimeState defining_runtime;

  fn clone(Allocator allocator) const throws -> completion_spec
  {
    let copy = completion_spec{};
    copy.function_name = String{allocator, function_name.view()};
    copy.word_list = String{allocator, word_list.view()};
    copy.should_use_default = should_use_default;
    copy.defining_runtime = defining_runtime;
    return copy;
  }
};

struct eval_state_snapshot
{
  StringMap<String> shell_variables;
  StringMap<SourceLocation> special_variable_definition_locations;
  StringMap<ArrayList<String>> indexed_arrays;
  StringMap<completion_spec> completion_specs;
  Maybe<completion_spec> default_completion_spec;
  HashSet associative_names;
  StringMap<String> associative_values;
  StringMap<String> sparse_array_values;
  HashSet sparse_array_names;
  u64 shopt_option_overrides;
  u64 shopt_option_values;
  StringMap<FunctionBodyHandle> functions;
  StringMap<String> aliases;
  ArrayList<String> positional_params;
  u32 bash_argument_value_count;
  u32 bash_argument_frame_count;
  bool had_bash_argument_arrays;
  u8 bash_argument_frame_context_flags;
  String last_argument;
  ArrayList<String> directory_stack;
  os::DirectoryReference working_directory;
  u32 file_creation_mask;
  StringMap<String> traps;
  /* The nesting depth a DEBUG or ERR trap was installed at rides the snapshot
     beside the trap map, because the depth decides which frames the action
     reaches. */
  usize debug_trap_active_depth;
  usize err_trap_active_depth;
  bool did_reset_inherited_signal_traps;
  /* Variable attributes ride the snapshot, so a declaration inside a
     subshell does not leak its marks to the parent. */
  StringMap<u8> variable_attributes;
  StringMap<exported_name_value> exported_names;
  /* The length of the environment undo log when the snapshot was taken, the
     point restore_state rewinds the process environment back to. */
  usize environment_undo_mark;
  RuntimeState runtime;
  ProgramResolver program_resolver;
  u8 init_moods_sourcing;
  u8 initialized_moods;
  u8 disabled_bash_special_arrays;
  u8 unset_dynamic_readers;
  bool was_mood_set_explicitly;
  u64 mood_mutation_revision;
  u64 warning_mutation_revision;
  u64 diagnostics_mutation_revision;
  u64 annoying_diagnostics_mutation_revision;
  u64 random_state;
  i64 shell_start_time;
  i64 seconds_base;
  shell_option_mutations option_mutations;
  ArrayList<ArrayList<local_binding>> local_scopes;
  usize local_scope_depth;
  job_table_snapshot job_state;
  usize getopts_char_index;
  i64 getopts_last_optind;
  bool terminal_exec_allowed;
  /* The shell descriptors of the live coprocess ride the snapshot, so a
     coprocess started inside a subshell leaves the outer record alone. */
  i32 coprocess_read_fd;
  i32 coprocess_write_fd;
};

/* Owns one compiled regex and frees it on destruction, so the regex cache
   reclaims every entry when the table rehashes, clears, or is torn down. It is
   move-only, since two owners would each free the same compiled buffer. */
class CompiledRegex
{
public:
  CompiledRegex() = default;
  explicit CompiledRegex(os::compiled_regex compiled)
      : m_re(compiled), m_is_owned(true)
  {}
  ~CompiledRegex()
  {
    if (m_is_owned) os::free_regex(m_re);
  }
  CompiledRegex(CompiledRegex &&other) noexcept
      : m_re(other.m_re), m_is_owned(other.m_is_owned)
  {
    other.m_is_owned = false;
  }
  fn operator=(CompiledRegex &&other) noexcept -> CompiledRegex &
  {
    if (this != &other) {
      if (m_is_owned) os::free_regex(m_re);
      m_re = other.m_re;
      m_is_owned = other.m_is_owned;
      other.m_is_owned = false;
    }
    return *this;
  }
  CompiledRegex(const CompiledRegex &) = delete;
  CompiledRegex &operator=(const CompiledRegex &) = delete;

  fn get() wontthrow -> os::compiled_regex * { return &m_re; }

private:
  os::compiled_regex m_re{};
  bool m_is_owned{false};
};

} /* namespace koshka */
