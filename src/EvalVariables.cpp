#include "Arena.hpp"
#include "Cli.hpp"
#include "Colors.hpp"
#include "Common.hpp"
#include "Completion.hpp"
#include "Debug.hpp"
#include "Errors.hpp"
#include "Eval.hpp"
#include "Expressions.hpp"
#include "Koshkit.hpp"
#include "Lexer.hpp"
#include "Parser.hpp"
#include "Path.hpp"
#include "Platform.hpp"
#include "ResolvedCommand.hpp"
#include "StaticStringMap.hpp"
#include "Toiletline.hpp"
#include "Trace.hpp"
#include "Utils.hpp"

namespace koshka {

struct ansi_color_variable
{
  const char *name;
  const char *escape;
};

static constexpr ansi_color_variable KOSH_ANSI_COLORS[] = {
    {"KOSH_ANSI_BLACK",          "\x1b[30m"},
    {"KOSH_ANSI_RED",            "\x1b[31m"},
    {"KOSH_ANSI_GREEN",          "\x1b[32m"},
    {"KOSH_ANSI_YELLOW",         "\x1b[33m"},
    {"KOSH_ANSI_BLUE",           "\x1b[34m"},
    {"KOSH_ANSI_MAGENTA",        "\x1b[35m"},
    {"KOSH_ANSI_CYAN",           "\x1b[36m"},
    {"KOSH_ANSI_WHITE",          "\x1b[37m"},
    {"KOSH_ANSI_BRIGHT_BLACK",   "\x1b[90m"},
    {"KOSH_ANSI_BRIGHT_RED",     "\x1b[91m"},
    {"KOSH_ANSI_BRIGHT_GREEN",   "\x1b[92m"},
    {"KOSH_ANSI_BRIGHT_YELLOW",  "\x1b[93m"},
    {"KOSH_ANSI_BRIGHT_BLUE",    "\x1b[94m"},
    {"KOSH_ANSI_BRIGHT_MAGENTA", "\x1b[95m"},
    {"KOSH_ANSI_BRIGHT_CYAN",    "\x1b[96m"},
    {"KOSH_ANSI_BRIGHT_WHITE",   "\x1b[97m"},
    {"KOSH_ANSI_BOLD",           "\x1b[1m" },
    {"KOSH_ANSI_DIM",            "\x1b[2m" },
    {"KOSH_ANSI_RESET",          "\x1b[0m" },
};

static fn ansi_escape_for_color(StringView name) throws -> Maybe<StringView>
{
  for (let const &color : KOSH_ANSI_COLORS)
    if (StringView{color.name} == name) return StringView{color.escape};
  return None;
}

enum class dynamic_var : u8
{
  IFS,
  LINENO,
  KOSH_GIT_BRANCH,
  KOSH_GIT_AHEAD,
  KOSH_GIT_BEHIND,
  KOSH_IDENTITY,

  RANDOM,
  SECONDS,
  SHELLOPTS,
  SRANDOM,
  EPOCHSECONDS,
  EPOCHREALTIME,
  EUID,
  BASHPID,
  BASH_MONOSECONDS,
  BASH_ARGV0,
  BASH_EXECUTION_STRING,
  BASH_SUBSHELL,
  BASH_SOURCE,
  BASH_LINENO,
  BASH_COMMAND,
  PPID,
  UID,
  HOSTNAME,
  HOSTTYPE,
  GROUPS,
  MACHTYPE,
  OSTYPE,
  FUNCNAME,
};

struct dynamic_variable_info
{
  StringView name;
  dynamic_var kind;
  bool is_process_sensitive;
  bool is_bash_only_diagnostic;
};

#define DYNAMIC_VARIABLE(name, kind, process_sensitive, bash_only)             \
  {                                                                            \
    SSK(name),                                                                 \
    {                                                                          \
      StringView{name, sizeof(name) - 1}, dynamic_var::kind,                   \
          process_sensitive, bash_only                                         \
    }                                                                          \
  }

constexpr static_string_entry<dynamic_variable_info> ALWAYS_DYNAMIC_ENTRIES[] =
    {
        DYNAMIC_VARIABLE("IFS", IFS, false, false),
        DYNAMIC_VARIABLE("LINENO", LINENO, false, false),
        DYNAMIC_VARIABLE("KOSH_GIT_BRANCH", KOSH_GIT_BRANCH, false, false),
        DYNAMIC_VARIABLE("KOSH_GIT_AHEAD", KOSH_GIT_AHEAD, false, false),
        DYNAMIC_VARIABLE("KOSH_GIT_BEHIND", KOSH_GIT_BEHIND, false, false),
        DYNAMIC_VARIABLE("KOSH_IDENTITY", KOSH_IDENTITY, false, false),
};
constexpr StaticStringMap ALWAYS_DYNAMIC{ALWAYS_DYNAMIC_ENTRIES};

constexpr static_string_entry<dynamic_variable_info> BASH_DYNAMIC_ENTRIES[] = {
    DYNAMIC_VARIABLE("BASH_COMMAND", BASH_COMMAND, false, true),
    DYNAMIC_VARIABLE("BASH_EXECUTION_STRING", BASH_EXECUTION_STRING, false,
                     true),
    DYNAMIC_VARIABLE("BASH_LINENO", BASH_LINENO, false, true),
    DYNAMIC_VARIABLE("BASH_MONOSECONDS", BASH_MONOSECONDS, false, true),
    DYNAMIC_VARIABLE("BASH_SOURCE", BASH_SOURCE, false, true),
    DYNAMIC_VARIABLE("BASH_SUBSHELL", BASH_SUBSHELL, false, true),
    DYNAMIC_VARIABLE("BASH_ARGV0", BASH_ARGV0, false, true),
    DYNAMIC_VARIABLE("BASHPID", BASHPID, true, true),
    DYNAMIC_VARIABLE("EPOCHREALTIME", EPOCHREALTIME, false, true),
    DYNAMIC_VARIABLE("EPOCHSECONDS", EPOCHSECONDS, false, true),
    DYNAMIC_VARIABLE("EUID", EUID, false, true),
    DYNAMIC_VARIABLE("FUNCNAME", FUNCNAME, false, true),
    DYNAMIC_VARIABLE("GROUPS", GROUPS, false, true),
    DYNAMIC_VARIABLE("HOSTNAME", HOSTNAME, false, true),
    DYNAMIC_VARIABLE("HOSTTYPE", HOSTTYPE, false, true),
    DYNAMIC_VARIABLE("MACHTYPE", MACHTYPE, false, true),
    DYNAMIC_VARIABLE("OSTYPE", OSTYPE, false, true),
    DYNAMIC_VARIABLE("PPID", PPID, true, true),
    DYNAMIC_VARIABLE("RANDOM", RANDOM, true, true),
    DYNAMIC_VARIABLE("SECONDS", SECONDS, false, true),
    DYNAMIC_VARIABLE("SHELLOPTS", SHELLOPTS, false, true),
    DYNAMIC_VARIABLE("SRANDOM", SRANDOM, true, true),
    DYNAMIC_VARIABLE("UID", UID, false, true),
};
constexpr StaticStringMap BASH_DYNAMIC{BASH_DYNAMIC_ENTRIES};

#undef DYNAMIC_VARIABLE

pure fn is_runtime_dynamic_variable_name(StringView name) wontthrow -> bool
{
  if (ALWAYS_DYNAMIC.find(name).has_value() ||
      BASH_DYNAMIC.find(name).has_value())
    return true;

  for (let const &color : KOSH_ANSI_COLORS)
    if (StringView{color.name} == name) return true;

  return false;
}

pure fn is_bash_only_dynamic_variable_name(StringView name) wontthrow -> bool
{
  let const info = BASH_DYNAMIC.find(name);
  return info.has_value() && info->is_bash_only_diagnostic;
}

pure fn is_process_dynamic_variable_name(StringView name) wontthrow -> bool
{
  let const info = BASH_DYNAMIC.find(name);
  return info.has_value() && info->is_process_sensitive;
}

constexpr pure fn is_dynamic_first_byte(char c) wontthrow -> bool
{
  switch (c) {
  case 'B':
  case 'E':
  case 'F':
  case 'G':
  case 'H':
  case 'I':
  case 'K':
  case 'L':
  case 'M':
  case 'O':
  case 'P':
  case 'R':
  case 'S':
  case 'U': return true;
  default: return false;
  }
}

pure fn EvalContext::variable_requires_dynamic_lookup(
    StringView name) const wontthrow -> bool
{
  if (name.is_empty() || !is_dynamic_first_byte(name[0])) {
    return false;
  }
  if (ALWAYS_DYNAMIC.find(name).has_value()) return true;
  if (name[0] == 'K' && name.starts_with("KOSH_ANSI_")) {
    return true;
  }

  return bash_dynamic_variables_enabled() &&
         BASH_DYNAMIC.find(name).has_value();
}

hot fn EvalContext::get_variable_value(StringView name) const throws
    -> Maybe<String>
{
  let const first_byte = name.is_empty() ? '\0' : name[0];

  if (name.count() == 1) {
    switch (first_byte) {
    case '?': return String::from(m_last_exit_status, heap_allocator());
    case '$': return String::from(os::get_shell_process_id(), heap_allocator());
    case '!':
      return m_last_background_pid
                 ? String::from(*m_last_background_pid, heap_allocator())
                 : String{heap_allocator()};
    case '-': return option_flags_string();
    case '#':
      return String::from(m_positional_params.count(), heap_allocator());
    case '0': return String{heap_allocator(), m_shell_name};
    case '_': return String{heap_allocator(), m_last_argument.view()};

    case '*':
    case '@': {
      let separator = ' ';
      let has_separator = true;
      if (first_byte == '*') {
        let const &ifs = m_field_separators;
        has_separator = !ifs.is_empty();
        if (has_separator) separator = ifs.first_character();
      }
      let joined = String{heap_allocator()};
      usize joined_length = 0;
      for (usize i = 0; i < m_positional_params.count(); i++)
        joined_length += m_positional_params[i].count();
      if (has_separator && m_positional_params.count() > 1) {
        joined_length += m_positional_params.count() - 1;
      }
      joined.reserve(joined_length);
      for (usize i = 0; i < m_positional_params.count(); i++) {
        if (i > 0 && has_separator) {
          joined.push(separator);
        }
        joined.append(m_positional_params[i].view());
      }
      return joined;
    }

    default: break;
    }
  }

  if (first_byte >= '0' && first_byte <= '9') {
    if (name.is_all_decimal_digits()) {
      /* A positional beyond the count is unset rather than empty, so
         ${1-default} takes its default. */
      if (name.count() > 9) return None;
      let const parsed_index = name.to<i64>();
      if (parsed_index.is_error()) return None;
      let const index = static_cast<usize>(parsed_index.value());
      if (index >= 1 && index <= m_positional_params.count()) {
        ASSERT(index - 1 < m_positional_params.count());
        return m_positional_params[index - 1];
      }
      return None;
    }
  }

  if (let const *stored = m_shell_variables.find(name); stored != nullptr)
    return *stored;

  /* A read of an array name with no scalar yields element zero, the way bash
     treats $a as ${a[0]}. */
  if (m_indexed_arrays.count() != 0)
    if (let const *array = m_indexed_arrays.find(name); array != nullptr) {
      if (array->is_empty()) return koshka::None;
      return array->front();
    }

  if (is_local_in_current_scope(name)) return koshka::None;

  /* The store lookup above wins, so IFS= reads back empty while the unset
     default reads back space-tab-newline, keeping the IFS save/restore idiom
     round-trip. A name whose first byte holds no dynamic variable falls
     straight through to the environment. */
  if (is_dynamic_first_byte(first_byte)) {
    if (let const info = ALWAYS_DYNAMIC.find(name); info.has_value()) {
      switch (info->kind) {
      case dynamic_var::IFS:
        return String{heap_allocator(), m_field_separators.view()};
      case dynamic_var::LINENO:
        return String::from(line_number_at_location(m_current_location),
                            heap_allocator());
      case dynamic_var::KOSH_GIT_BRANCH: {
        if (m_git_branch_command_index != m_command_evaluation_index) {
          m_git_branch = utils::current_git_branch();
          m_git_branch_command_index = m_command_evaluation_index;
        }
        return String{heap_allocator(), m_git_branch.view()};
      }
      case dynamic_var::KOSH_GIT_AHEAD:
      case dynamic_var::KOSH_GIT_BEHIND: {
        if (m_git_counts_command_index != m_command_evaluation_index) {
          utils::git_status(m_git_branch, m_git_ahead_count,
                            m_git_behind_count);
          m_git_branch_command_index = m_command_evaluation_index;
          m_git_counts_command_index = m_command_evaluation_index;
        }

        switch (info->kind) {
        case dynamic_var::KOSH_GIT_AHEAD:
          return m_git_ahead_count > 0
                     ? String::from(m_git_ahead_count, heap_allocator())
                     : String{heap_allocator()};
        case dynamic_var::KOSH_GIT_BEHIND:
          return m_git_behind_count > 0
                     ? String::from(m_git_behind_count, heap_allocator())
                     : String{heap_allocator()};
        default:
          unreachable("the git count variable must be KOSH_GIT_AHEAD or "
                      "KOSH_GIT_BEHIND");
        }
      }
      case dynamic_var::KOSH_IDENTITY: return materialize_kosh_identity();
      default: break;
      }
    }

    if (first_byte == 'K' && name.starts_with("KOSH_ANSI_")) {
      if (let const escape = ansi_escape_for_color(name)) {
        if (!colors::stdout_wants_color()) return String{heap_allocator()};
        return String{heap_allocator(), *escape};
      }
    }

    if (bash_dynamic_variables_enabled()) {
      if (let const info = BASH_DYNAMIC.find(name); info.has_value()) {
        switch (info->kind) {
        case dynamic_var::RANDOM:
          if (!m_random_seeded) {
            std::srand(static_cast<unsigned>(m_shell_start_time) ^
                       static_cast<unsigned>(os::get_shell_process_id()));
            m_random_seeded = true;
          }
          return String::from(static_cast<usize>(std::rand() % 32768),
                              heap_allocator());
        case dynamic_var::SECONDS:
          return String::from(static_cast<i64>(std::time(nullptr)) -
                                  m_shell_start_time,
                              heap_allocator());
        case dynamic_var::SHELLOPTS: {
          return enabled_shell_option_names(*this);
        }
        case dynamic_var::SRANDOM: {
          let const value = static_cast<u32>(os::realtime_microseconds()) ^
                            (static_cast<u32>(std::rand()) << 16) ^
                            static_cast<u32>(std::rand());
          return String::from(static_cast<i64>(value), heap_allocator());
        }
        case dynamic_var::EPOCHSECONDS:
          return String::from(static_cast<i64>(std::time(nullptr)),
                              heap_allocator());
        case dynamic_var::EPOCHREALTIME: {
          let const microseconds = os::realtime_microseconds();
          char fraction[8];
          std::snprintf(
              fraction, sizeof(fraction), "%06llu",
              static_cast<unsigned long long>(microseconds % 1000000ULL));
          return String::from(static_cast<i64>(microseconds / 1000000ULL),
                              heap_allocator()) +
                 "." + StringView{fraction};
        }
        case dynamic_var::EUID:
          return String::from(os::get_effective_user_id(), heap_allocator());
        case dynamic_var::BASHPID:
          return String::from(os::get_current_process_id(), heap_allocator());
        case dynamic_var::BASH_MONOSECONDS:
          return String::from(
              static_cast<i64>(os::monotonic_nanos() / 1000000ULL),
              heap_allocator());
        case dynamic_var::BASH_ARGV0:
          return String{heap_allocator(), m_shell_name.view()};
        case dynamic_var::BASH_EXECUTION_STRING:
          if (!m_execution_string.is_empty())
            return String{heap_allocator(), m_execution_string.view()};
          break;
        case dynamic_var::BASH_SUBSHELL:
          return String::from(static_cast<i64>(m_subshell_depth),
                              heap_allocator());
        case dynamic_var::BASH_SOURCE:
          for (usize i = m_source_frames.count(); i > 0; i--) {
            let const &path = m_source_frames[i - 1].source_path;
            if (!path.is_empty()) {
              return String{heap_allocator(), path.view()};
            }
          }
          if (funcname_frame_count() > 0) {
            let const *info =
                m_function_definition_infos.find(funcname_frame_at(0));
            if (info != nullptr && !info->filename.is_empty()) {
              return String{heap_allocator(), info->filename.view()};
            }
          }
          if (m_is_script_run)
            return String{heap_allocator(), m_shell_name.view()};
          return String{heap_allocator()};
        case dynamic_var::BASH_LINENO:
          if (funcname_frame_count() > 0)
            return String::from(funcname_line_at(0), heap_allocator());
          return koshka::None;
        case dynamic_var::BASH_COMMAND:
          if (!m_current_command.is_empty())
            return String{heap_allocator(), m_current_command.view()};
          break;
        case dynamic_var::PPID:
          return String::from(os::get_parent_process_id(), heap_allocator());
        case dynamic_var::UID:
          return String::from(os::get_real_user_id(), heap_allocator());
        case dynamic_var::HOSTNAME:
          if (let host = os::get_hostname(); host.has_value())
            return steal(*host);
          return String{heap_allocator()};
        case dynamic_var::HOSTTYPE: return os::machine_type();
        case dynamic_var::GROUPS:
          return String::from(os::get_real_group_id(), heap_allocator());
        case dynamic_var::MACHTYPE:
          return os::machine_type() + "-unknown-linux-gnu";
        case dynamic_var::OSTYPE:
          return String{heap_allocator(), os::ostype_name()};
        case dynamic_var::FUNCNAME:
          if (funcname_frame_count() > 0)
            return String{heap_allocator(), funcname_frame_at(0)};
          return koshka::None;
        default: break;
        }
      }
    }
  }

  if (let const env = os::get_environment_variable(name))
    return String{heap_allocator(), env->view()};
  return koshka::None;
}

fn EvalContext::append_dynamic_variable_names(
    ArrayList<StringView> &out) const throws -> void
{
  for (let const &entry : ALWAYS_DYNAMIC.entries)
    out.push(entry.value.name);

  for (let const &color : KOSH_ANSI_COLORS)
    out.push(StringView{color.name});

  if (!bash_dynamic_variables_enabled()) return;

  for (let const &entry : BASH_DYNAMIC.entries)
    out.push(entry.value.name);
}

} /* namespace koshka */
