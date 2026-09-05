/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements snapshot evaluation. It applies the corresponding
 * shell semantics through EvalContext while preserving state, source
 * locations, and allocation ownership.
 */

#pragma once

#include "Arena.hpp"
#include "Bitset.hpp"
#include "Builtin.hpp"
#include "Common.hpp"
#include "Containers.hpp"
#include "Errors.hpp"
#include "EvalTypes.hpp"
#include "Maybe.hpp"
#include "MimicMood.hpp"
#include "Path.hpp"
#include "Platform.hpp"
#include "ProgramResolver.hpp"

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
  String last_argument;
  ArrayList<String> directory_stack;
  os::DirectoryReference working_directory;
  u32 file_creation_mask;
  StringMap<String> traps;
  /* The read-only and integer name sets ride the snapshot too, so a readonly or
     a declare -i inside a subshell dies with the child rather than leaking its
     mark to the parent. */
  HashSet readonly_names;
  HashSet integer_names;
  HashSet exported_names;
  /* The length of the environment undo log when the snapshot was taken, the
     point restore_state rewinds the process environment back to. */
  usize environment_undo_mark;
  RuntimeState runtime;
  ProgramResolver program_resolver;
  u8 init_moods_sourcing;
  u8 initialized_moods;
  bool was_mood_set_explicitly;
  u64 mood_mutation_revision;
  u64 warning_mutation_revision;
  u64 diagnostics_mutation_revision;
  u64 annoying_diagnostics_mutation_revision;
  u64 random_state;
  shell_option_mutations option_mutations;
  ArrayList<ArrayList<local_binding>> local_scopes;
  usize local_scope_depth;
  Maybe<i64> last_background_pid;
  usize getopts_char_index;
  i64 getopts_last_optind;
  bool terminal_exec_allowed;
  ArrayList<job> jobs;
  ArrayList<os::process> detached_job_processes;
  i32 next_job_id;
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
