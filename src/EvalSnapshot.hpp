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
#include "base/StaticStringMap.hpp"

namespace koshka {

enum class compgen_action : u8
{
  Alias,
  ArrayVar,
  Binding,
  Builtin,
  Command,
  Directory,
  Disabled,
  Enabled,
  Export,
  File,
  Function,
  Group,
  HelpTopic,
  Hostname,
  Job,
  Keyword,
  Running,
  Service,
  SetOpt,
  ShOpt,
  Signal,
  Stopped,
  User,
  Variable,
};

inline constexpr u32 COMPGEN_ACTION_COUNT =
    static_cast<u32>(compgen_action::Variable) + 1;

inline constexpr static_string_entry<compgen_action> COMPGEN_ACTION_ENTRIES[] =
    {
        {SSK("alias"),     compgen_action::Alias    },
        {SSK("arrayvar"),  compgen_action::ArrayVar },
        {SSK("binding"),   compgen_action::Binding  },
        {SSK("builtin"),   compgen_action::Builtin  },
        {SSK("command"),   compgen_action::Command  },
        {SSK("directory"), compgen_action::Directory},
        {SSK("disabled"),  compgen_action::Disabled },
        {SSK("enabled"),   compgen_action::Enabled  },
        {SSK("export"),    compgen_action::Export   },
        {SSK("file"),      compgen_action::File     },
        {SSK("function"),  compgen_action::Function },
        {SSK("group"),     compgen_action::Group    },
        {SSK("helptopic"), compgen_action::HelpTopic},
        {SSK("hostname"),  compgen_action::Hostname },
        {SSK("job"),       compgen_action::Job      },
        {SSK("keyword"),   compgen_action::Keyword  },
        {SSK("running"),   compgen_action::Running  },
        {SSK("service"),   compgen_action::Service  },
        {SSK("setopt"),    compgen_action::SetOpt   },
        {SSK("shopt"),     compgen_action::ShOpt    },
        {SSK("signal"),    compgen_action::Signal   },
        {SSK("stopped"),   compgen_action::Stopped  },
        {SSK("user"),      compgen_action::User     },
        {SSK("variable"),  compgen_action::Variable },
};

inline constexpr StaticStringMap COMPGEN_ACTIONS{COMPGEN_ACTION_ENTRIES};

inline constexpr pure fn compgen_action_bit(compgen_action action) wontthrow
    -> u32
{
  return 1U << static_cast<u32>(action);
}

inline constexpr compgen_action LETTER_COMPGEN_ACTIONS[] = {
    compgen_action::Alias,   compgen_action::Builtin,
    compgen_action::Command, compgen_action::Directory,
    compgen_action::Export,  compgen_action::File,
    compgen_action::Group,   compgen_action::Job,
    compgen_action::Keyword, compgen_action::Service,
    compgen_action::User,    compgen_action::Variable,
};

inline constexpr usize LETTER_COMPGEN_ACTION_COUNT =
    sizeof(LETTER_COMPGEN_ACTIONS) / sizeof(LETTER_COMPGEN_ACTIONS[0]);

inline constexpr pure fn compgen_letter_action_mask(const bool (
    &is_action_enabled)[LETTER_COMPGEN_ACTION_COUNT]) wontthrow -> u32
{
  u32 action_mask = 0;
  for (usize i = 0; i < LETTER_COMPGEN_ACTION_COUNT; i++) {
    if (is_action_enabled[i]) {
      action_mask |= compgen_action_bit(LETTER_COMPGEN_ACTIONS[i]);
    }
  }

  return action_mask;
}

enum class completion_option : u8
{
  BashDefault,
  Default,
  DirNames,
  FileNames,
  FullQuote,
  NoQuote,
  NoSort,
  NoSpace,
  PlusDirs,
};

inline constexpr u32 COMPLETION_OPTION_COUNT =
    static_cast<u32>(completion_option::PlusDirs) + 1;

inline constexpr static_string_entry<completion_option>
    COMPLETION_OPTION_ENTRIES[] = {
        {SSK("bashdefault"), completion_option::BashDefault},
        {SSK("default"),     completion_option::Default    },
        {SSK("dirnames"),    completion_option::DirNames   },
        {SSK("filenames"),   completion_option::FileNames  },
        {SSK("fullquote"),   completion_option::FullQuote  },
        {SSK("noquote"),     completion_option::NoQuote    },
        {SSK("nosort"),      completion_option::NoSort     },
        {SSK("nospace"),     completion_option::NoSpace    },
        {SSK("plusdirs"),    completion_option::PlusDirs   },
};

inline constexpr StaticStringMap COMPLETION_OPTIONS{COMPLETION_OPTION_ENTRIES};

inline pure fn completion_option_bit(completion_option option) wontthrow -> u32
{
  return 1U << static_cast<u32>(option);
}

enum class completion_argument : u8
{
  Glob,
  WordList,
  Prefix,
  Suffix,
  Filter,
  Command,
  Function,
};

inline constexpr u32 COMPLETION_ARGUMENT_COUNT =
    static_cast<u32>(completion_argument::Function) + 1;

inline pure fn completion_argument_bit(completion_argument argument) wontthrow
    -> u32
{
  return 1U << static_cast<u32>(argument);
}

enum class completion_spec_slot : u8
{
  Default,
  Empty,
  Initial,
};

struct completion_spec
{
  String function_name{heap_allocator()};
  String word_list{heap_allocator()};
  String glob_pattern{heap_allocator()};
  String filter_pattern{heap_allocator()};
  String prefix{heap_allocator()};
  String suffix{heap_allocator()};
  String command{heap_allocator()};
  u32 action_mask{0};
  u32 option_mask{0};
  u32 argument_mask{0};
  definition_state defining_state;

  pure fn has_action(compgen_action action) const wontthrow -> bool
  {
    return (action_mask & compgen_action_bit(action)) != 0;
  }

  pure fn has_option(completion_option option) const wontthrow -> bool
  {
    return (option_mask & completion_option_bit(option)) != 0;
  }

  pure fn has_argument(completion_argument argument) const wontthrow -> bool
  {
    return (argument_mask & completion_argument_bit(argument)) != 0;
  }

  fn clone(Allocator allocator) const throws -> completion_spec
  {
    let copy = completion_spec{};
    copy.function_name = String{allocator, function_name.view()};
    copy.word_list = String{allocator, word_list.view()};
    copy.glob_pattern = String{allocator, glob_pattern.view()};
    copy.filter_pattern = String{allocator, filter_pattern.view()};
    copy.prefix = String{allocator, prefix.view()};
    copy.suffix = String{allocator, suffix.view()};
    copy.command = String{allocator, command.view()};
    copy.action_mask = action_mask;
    copy.option_mask = option_mask;
    copy.argument_mask = argument_mask;
    copy.defining_state = defining_state;
    return copy;
  }
};

struct variable_snapshot
{
  VariableTable variables;
  StringMap<SourceLocation> special_variable_definition_locations;
  StringMap<ArrayList<String>> indexed_arrays;
  CompositeKeyArrays associative_arrays;
  CompositeKeyArrays sparse_arrays;
  ArrayList<String> positional_params;
  ArrayList<String> directory_stack;
  StringMap<exported_name_value> exported_names;
  u32 bash_argument_value_count;
  u32 bash_argument_frame_count;
  bool had_bash_argument_arrays;
  u8 bash_argument_frame_context_flags;
  u8 disabled_bash_special_arrays;
  u8 unset_dynamic_readers;
};

struct completion_snapshot
{
  StringMap<completion_spec> specs;
  Maybe<completion_spec> default_spec;
  Maybe<completion_spec> empty_spec;
  Maybe<completion_spec> initial_spec;
};

struct scope_snapshot
{
  StringMap<String> aliases;
  ArrayList<ArrayList<local_binding>> local_scopes;
  usize local_scope_depth;
};

struct execution_snapshot
{
  String last_argument;
  usize terminal_exec_subshell_depth;
  bool terminal_exec_allowed;
};

struct trap_snapshot
{
  StringMap<trap_definition> traps;
  trap_install_state install;
};

struct runtime_control_snapshot
{
  u8 init_moods_sourcing;
  u8 initialized_moods;
  control_mutations mutations;
};

struct runtime_control_wire
{
  u8 init_moods_sourcing{0};
  u8 initialized_moods{0};
  u32 suppressed_warnings{0};
};

struct eval_state_snapshot
{
  variable_snapshot variables;
  completion_snapshot completion;
  StringMap<FunctionBodyHandle> functions;
  scope_snapshot scopes;
  execution_snapshot execution;
  trap_snapshot traps;
  runtime_control_snapshot control;
  dynamic_clock_state clock;
  job_table_snapshot jobs;
  getopts_cursor getopts;
  coprocess_descriptors coprocess;
  usize environment_undo_mark;
  RuntimeState runtime;
  ProgramResolver program_resolver;
  os::DirectoryReference working_directory;
  u32 file_creation_mask;
};

struct variable_wire
{
  u8 disabled_bash_special_arrays{0};
  u8 unset_dynamic_readers{0};
  bool has_bash_argument_arrays{false};
  ArrayList<u32> bash_argument_frame_counts{heap_allocator()};
  ArrayList<String> bash_argument_values{heap_allocator()};
  bool has_bash_argument_context{false};
  u8 bash_argument_context_flags{0};
  String bash_argument_source_path{heap_allocator()};
};

struct execution_wire
{
  Maybe<String> execution_string{None};
  String last_argument{heap_allocator()};
};

struct startup_wire
{
  String init_moods{heap_allocator()};
  bool is_restricted_shell{false};
  bool is_login_shell{false};
};

struct function_wire
{
  usize call_depth{0};
  ArrayList<String> call_names{heap_allocator()};
  StringMap<function_definition_info> definition_origins{heap_allocator()};
};

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
