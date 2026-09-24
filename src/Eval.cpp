/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements shared EvalContext and ExecContext operations for
 * assignments, environments, diagnostics, history, scopes, aliases, Bash
 * argument-frame capture, command resolution, and builtin input and output.
 * These operations stay here because they coordinate state shared by several
 * expression and builtin families.
 */

#include "Eval.hpp"

#include "base/Arena.hpp"
#include "CLI.hpp"
#include "CLIColors.hpp"
#include "base/Common.hpp"
#include "Completion.hpp"
#include "base/Debug.hpp"
#include "Errors.hpp"
#include "Expressions.hpp"
#include "Koshkit.hpp"
#include "Lexer.hpp"
#include "Parser.hpp"
#include "base/Path.hpp"
#include "Platform.hpp"
#include "StaticStringMap.hpp"
#include "Toiletline.hpp"
#include "base/Trace.hpp"
#include "Utils.hpp"

namespace koshka {

static pure fn is_prompt_special_variable(StringView name) wontthrow -> bool
{
  return name == "PROMPT_COMMAND" ||
         (name.length == 3 && name[0] == 'P' && name[1] == 'S' &&
          name[2] >= '0' && name[2] <= '4');
}

EvalContext::EvalContext(bool should_disable_path_expansion, bool should_echo,
                         bool should_echo_expanded, bool shell_is_interactive,
                         bool should_error_exit, String shell_name,
                         ArrayList<String> positional_params)
    : m_variable_store(steal(positional_params)),
      m_shell_name(steal(shell_name)),
      m_shell_is_interactive(shell_is_interactive)
{
  set_no_glob(should_disable_path_expansion);
  set_echo(should_echo);
  set_echo_expanded(should_echo_expanded);
  set_error_exit(should_error_exit);
  set_emacs_mode(shell_is_interactive);
  set_shell_option_state(shell_option_id::History, shell_is_interactive);
  set_shell_option_state(shell_option_id::Histexpand, shell_is_interactive);
  set_field_separators(field_separators());

  m_shell_start_time = static_cast<i64>(std::time(nullptr));
  m_startup_ignored_signals = os::get_entry_ignored_signals();

  os::for_each_environment_name(this, [](opaque *context, StringView name) {
    static_cast<EvalContext *>(context)->mark_exported(name);
  });
}

EvalContext::~EvalContext()
{
  reset_bash_argument_arrays();
  reset_runtime_diagnostic_highlight_cache();
}

fn EvalContext::get_or_create_diagnostic_highlight_cache() throws
    -> completion::shell_highlight_cache *
{
  if (m_diagnostic_highlight_cache != nullptr)
    return m_diagnostic_highlight_cache;

  if (m_runtime_diagnostic_highlight_cache == nullptr) {
    let const cache =
        heap_allocator().alloc_array<completion::shell_highlight_cache>(1);
    if (cache == nullptr) throw std::bad_alloc{};
    try {
      m_runtime_diagnostic_highlight_cache =
          new (cache) completion::shell_highlight_cache{};
    } catch (...) {
      heap_allocator().free_array(cache, 1);
      throw;
    }
  }

  return m_runtime_diagnostic_highlight_cache;
}

fn EvalContext::reset_runtime_diagnostic_highlight_cache() wontthrow -> void
{
  if (m_runtime_diagnostic_highlight_cache == nullptr) return;
  m_runtime_diagnostic_highlight_cache->~shell_highlight_cache();
  heap_allocator().free_array(m_runtime_diagnostic_highlight_cache, 1);
  m_runtime_diagnostic_highlight_cache = nullptr;
}

fn RuntimeState::capture(const EvalContext &context) wontthrow -> RuntimeState
{
  return context.m_runtime;
}

fn RuntimeState::restore(EvalContext &context) const wontthrow -> void
{
  context.m_runtime = *this;
}

fn EvalContext::add_evaluated_expression() wontthrow -> void
{
  if (!stats_enabled()) return;
  m_expressions_executed_last++;
}

fn EvalContext::add_expansion() wontthrow -> void
{
  if (!stats_enabled()) return;
  m_expansions_last++;
}

fn EvalContext::end_command() wontthrow -> void
{
  m_expansions_total += m_expansions_last;
  m_expressions_executed_total += m_expressions_executed_last;
  m_commands_evaluated++;

  if (parse_arena() != nullptr) {
    let const used = parse_arena()->bytes_used();
    if (used > m_peak_ast_arena_bytes) m_peak_ast_arena_bytes = used;
  }

  m_expansions_last = m_expressions_executed_last = 0;
}

fn EvalContext::record_history_event(StringView command) throws -> bool
{
  if (!m_history_transaction_stack.is_empty()) {
    m_history_transaction_stack.back()->push(String{heap_allocator(), command});
    return true;
  }

  toiletline::set_history_limit(get_history_limit("KOSH_HISTORY_SIZE", 4096));
  return toiletline::append_history_event(command).has_value();
}

fn EvalContext::begin_history_transaction(ArrayList<String> &commands) throws
    -> void
{
  m_history_transaction_stack.push(&commands);
}

fn EvalContext::end_history_transaction() wontthrow -> void
{
  ASSERT(!m_history_transaction_stack.is_empty());
  m_history_transaction_stack.pop_back();
}

pure fn EvalContext::has_history_transaction() const wontthrow -> bool
{
  return !m_history_transaction_stack.is_empty();
}

fn EvalContext::begin_command_evaluation() wontthrow -> void
{
  m_command_evaluation_index++;
}

hot fn EvalContext::assign_variable(StringView name, StringView value) throws
    -> void
{
  LOG(All, "assigning variable '%.*s' to a value of %zu bytes",
      static_cast<int>(name.length), name.data, value.length);
  let const first_byte = name.is_empty() ? '\0' : name[0];
  let is_field_separator_name = false;
  let is_ignoreeof_name = false;
  let is_path_name = false;

  switch (first_byte) {
  case 'I':
    is_field_separator_name = name == "IFS";
    is_ignoreeof_name = name == "IGNOREEOF";
    break;
  case 'P':
  case 'p': is_path_name = utils::environment_name_is_path(name); break;
  default: break;
  }

  if (m_confined_write_depth > 0) [[unlikely]] {
    let const *previous = lookup_shell_variable(name);
    let saved = Maybe<String>{};
    if (previous != nullptr) saved = String{previous->view()};
    let const saved_definition = special_variable_definition_location(name);

    m_confined_write_log.push(environment_undo_entry{
        String{name}, steal(saved), saved_definition
    });
  }

  if (is_field_separator_name) set_field_separators(value);
  if (write_dynamic_variable(name, value)) return;

  if (is_path_name) m_program_resolver.assign_path(String{value});
  if (is_ignoreeof_name) {
    m_runtime.set_option(shell_option_id::Ignoreeof, true);
  }

  m_shell_variables.set(name, value);
  if (is_prompt_special_variable(name))
    m_special_variable_definition_locations.set(name, m_current_location);
  if (is_exported(name)) {
    if (m_subshell_depth > 0)
      m_environment_undo_log.push(environment_undo_entry{
          String{name}, os::get_environment_variable(name), None});
    os::set_environment_variable(name, value);
  }
}

fn EvalContext::restore_temporary_shell_variable(
    StringView name, const Maybe<String> &previous_value,
    Maybe<SourceLocation> previous_definition_location) throws -> void
{
  if (previous_value.has_value())
    m_shell_variables.set(name, previous_value->view());
  else
    m_shell_variables.erase(name);
  if (is_prompt_special_variable(name)) {
    if (previous_definition_location.has_value())
      m_special_variable_definition_locations.set(
          name, *previous_definition_location);
    else
      m_special_variable_definition_locations.erase(name);
  }
}

fn EvalContext::begin_confined_variable_writes() wontthrow -> usize
{
  LOG(Debug, "confining variable writes above mark %zu",
      m_confined_write_log.count());

  if (m_confined_write_depth == 0) {
    m_confined_seconds_base = m_seconds_base;
    m_confined_random_state = m_random_state;
    m_was_confined_ignoreeof_enabled =
        m_runtime.option_is_enabled(shell_option_id::Ignoreeof);
  }

  m_confined_write_depth++;
  return m_confined_write_log.count();
}

fn EvalContext::rollback_confined_variable_writes(usize mark) wontthrow -> void
{
  ASSERT(m_confined_write_depth > 0);
  m_confined_write_depth--;
  LOG(Debug, "rewinding %zu confined variable writes",
      m_confined_write_log.count() - mark);

  while (m_confined_write_log.count() > mark) {
    try {
      let const &entry = m_confined_write_log.back();
      let const name = entry.name.view();
      restore_temporary_shell_variable(
          name, entry.previous_value,
          entry.previous_special_definition_location);
      let const restored = entry.previous_value.has_value()
                               ? entry.previous_value->view()
                               : StringView{};

      if (name == "IFS") set_field_separators(restored);
      if (utils::environment_name_is_path(name))
        m_program_resolver.assign_path(String{restored});
      if (is_exported(name)) {
        if (entry.previous_value.has_value())
          os::set_environment_variable(name, restored);
        else
          os::unset_environment_variable(name);
      }
    } catch (...) {
      LOG(Info, "a confined variable write could not be rewound");
    }

    m_confined_write_log.pop_back();
  }

  if (m_confined_write_depth == 0) {
    m_seconds_base = m_confined_seconds_base;
    m_random_state = m_confined_random_state;
    m_runtime.set_option(shell_option_id::Ignoreeof,
                         m_was_confined_ignoreeof_enabled);
  }
}

fn EvalContext::set_field_separators(StringView value) throws -> void
{
  LOG(Debug, "caching %zu field separator bytes", value.length);
  m_variable_store.set_field_separators(value);
}

hot pure fn EvalContext::is_field_separator(char c) const wontthrow -> bool
{
  return m_variable_store.is_field_separator(c);
}

fn EvalContext::guard_restricted_path(StringView path,
                                      const SourceLocation &location,
                                      restricted_path_use use) const throws
    -> void
{
  if (!restricted_enforcement_active() || !os::has_directory_separator(path)) {
    return;
  }

  switch (use) {
  case restricted_path_use::Command:
    throw ErrorWithLocation{
        location,
        "Command names containing a directory separator are forbidden in a "
        "restricted shell"};
  case restricted_path_use::Source:
    throw ErrorWithLocation{
        location,
        "Source paths containing a directory separator are forbidden in a "
        "restricted shell"};
  case restricted_path_use::History:
    throw ErrorWithLocation{
        location,
        "History paths containing a directory separator are forbidden in a "
        "restricted shell"};
  case restricted_path_use::Hash:
    throw ErrorWithLocation{
        location,
        "hash -p paths containing a directory separator are forbidden in a "
        "restricted shell"};
  }
  unreachable("Unhandled restricted path use");
}

hot fn EvalContext::set_shell_variable(StringView name, StringView value) throws
    -> void
{
  if (is_readonly(name))
    throw Error{"Unable to assign '" + name + "' because it is read only"};

  if (is_write_discarded_dynamic_variable(name)) return;

  if (name == BASH_ALIASES_VARIABLE &&
      is_bash_special_array_active(bash_special_array_id::Aliases))
  {
    set_alias("0", value);
    return;
  }
  if (is_bash_directory_stack_special(name)) return;

  if (is_integer_variable(name)) [[unlikely]] {
    let const result = value.length == 0 ? String{scratch_allocator(), "0"}
                                         : evaluate_arithmetic_text(value);
    assign_variable(name, result.view());
    return;
  }

  if (is_lowercase_variable(name) || is_uppercase_variable(name)) [[unlikely]] {
    let adjusted = String{scratch_allocator(), value};
    apply_variable_case(name, adjusted);
    assign_variable(name, adjusted.view());
    return;
  }

  assign_variable(name, value);
}

fn EvalContext::seed_shell_identity_variables(bool is_bash_identity) throws
    -> void
{
  if (is_bash_identity) {
    LOG(Info, "seeding the bash identity variables");
    set_shell_variable("BASH_VERSION", "5.3.0(1)-kosh");
    let versinfo = ArrayList<String>{heap_allocator()};
    versinfo.push(String{"5"});
    versinfo.push(String{"3"});
    versinfo.push(String{"0"});
    versinfo.push(String{"1"});
    versinfo.push(String{"release"});
    versinfo.push(String{KOSH_OS_INFO});
    set_indexed_array("BASH_VERSINFO", steal(versinfo));
    set_shell_variable("BASH", m_shell_executable_path.view());
    /* A missing COMP_WORDBREAKS collapses every word into one and kills
       bash-completion. */
    if (!get_variable_value("COMP_WORDBREAKS").has_value())
      set_shell_variable("COMP_WORDBREAKS", StringView{" \t\n\"'><=;|&(:"});
    return;
  }
  LOG(Info, "clearing the bash identity variables for a non-bash mood");
  if (lookup_shell_variable("BASH_VERSION") != nullptr ||
      os::has_environment_variable("BASH_VERSION"))
  {
    force_unset_shell_variable("BASH_VERSION");
  }
  if (lookup_shell_variable("BASH") != nullptr ||
      os::has_environment_variable("BASH"))
  {
    force_unset_shell_variable("BASH");
  }
}

fn EvalContext::materialize_kosh_identity() const throws -> Maybe<String>
{
  let const identity = utils::kosh_identity(m_shell_executable_path.view());
  if (identity.has_value()) return String{heap_allocator(), *identity};
  return None;
}

fn EvalContext::unset_shell_variable(StringView name) throws -> void
{
  if (is_readonly(name))
    throw Error{"Unable to unset '" + name + "' because it is read only"};

  if (is_bash_argument_array(name))
    throw Error{String{name} + ": cannot unset"};

  if (peel_caller_local_binding(name)) return;

  let const should_disable_bash_aliases =
      name == BASH_ALIASES_VARIABLE &&
      is_bash_special_array_active(bash_special_array_id::Aliases);
  let const should_disable_bash_directory_stack =
      is_bash_directory_stack_special(name);
  force_unset_shell_variable(name);
  indexed_arrays().erase(name);
  clear_sparse_array(name);
  clear_associative_array(name);
  if (should_disable_bash_aliases)
    disable_bash_special_array(bash_special_array_id::Aliases);
  if (should_disable_bash_directory_stack)
    disable_bash_special_array(bash_special_array_id::DirectoryStack);
  variable_attributes().erase(name);
}

fn EvalContext::disable_ignoreeof() throws -> void
{
  force_unset_shell_variable("IGNOREEOF");
  variable_attributes().erase("IGNOREEOF");
}

fn EvalContext::peel_caller_local_binding(StringView name) throws -> bool
{
  if (m_local_scope_depth < 2) return false;
  if (is_local_in_current_scope(name)) return false;

  for (usize frame_index = m_local_scope_depth - 1; frame_index-- > 0;) {
    ArrayList<local_binding> &frame = m_local_scopes[frame_index];
    for (usize i = frame.count(); i-- > 0;) {
      let &binding = frame[i];
      if (binding.name.view() != name) continue;
      LOG(Debug, "peeling the local binding of '%.*s' from caller frame %zu",
          static_cast<int>(name.length), name.data, frame_index);

      restore_local_binding(binding);

      frame.remove(i);
      return true;
    }
  }
  return false;
}

fn EvalContext::restore_local_binding(local_binding &binding) throws -> void
{
  /* The scope pop runs this inside a noexcept defer, so a readonly name would
     throw from a destructor and terminate the shell. assign_variable and the
     stores below skip the readonly check. */
  if (binding.previous_value.has_value())
    assign_variable(binding.name, *binding.previous_value);
  else
    force_unset_shell_variable(binding.name);
  if (is_prompt_special_variable(binding.name.view())) {
    if (binding.previous_special_definition_location.has_value())
      m_special_variable_definition_locations.set(
          binding.name.view(), *binding.previous_special_definition_location);
    else
      m_special_variable_definition_locations.erase(binding.name.view());
  }
  if (binding.previous_indexed_array.has_value())
    indexed_arrays().set(binding.name.view(),
                         steal(*binding.previous_indexed_array));
  else
    indexed_arrays().erase(binding.name.view());
  let const was_restricted = restricted_enforcement_active();
  m_runtime.set_option(shell_option_id::Restricted, false);
  variable_attributes().erase(binding.name.view());
  defer
  {
    m_runtime.set_option(shell_option_id::Restricted, was_restricted);
    if (binding.previous_attributes != 0)
      variable_attributes().set(binding.name.view(),
                                binding.previous_attributes);
    else
      variable_attributes().erase(binding.name.view());
  };
  clear_sparse_array(binding.name.view());
  for (usize i = 0; i < binding.previous_sparse_indices.count(); i++)
    set_array_element(binding.name.view(), binding.previous_sparse_indices[i],
                      binding.previous_sparse_values[i].view());
  clear_associative_array(binding.name.view());
  if (binding.previous_was_associative)
    for (usize k = 0; k < binding.previous_associative_keys.count(); k++)
      set_associative_element(binding.name.view(),
                              binding.previous_associative_keys[k].view(),
                              binding.previous_associative_values[k].view());
  if (binding.previous_was_exported) {
    mark_exported(binding.name.view());
    if (binding.previous_value.has_value())
      os::set_environment_variable(binding.name, *binding.previous_value);
  } else if (is_exported(binding.name.view())) {
    unmark_exported(binding.name.view());
    os::unset_environment_variable(binding.name);
  }
}

fn EvalContext::set_indexed_array(StringView name,
                                  ArrayList<String> values) throws -> void
{
  LOG(All, "storing indexed array '%.*s' with %zu elements",
      static_cast<int>(name.length), name.data, values.count());
  if (is_readonly(name))
    throw Error{"Unable to assign '" + name + "' because it is read only"};
  if (is_write_discarded_dynamic_variable(name)) return;
  if (is_bash_directory_stack_special(name)) {
    for (usize index = 0; index < values.count(); index++)
      set_bash_directory_stack_element(index, values[index].view());
    return;
  }
  if (is_lowercase_variable(name) || is_uppercase_variable(name)) [[unlikely]]
    for (let &value : values)
      apply_variable_case(name, value);
  m_shell_variables.erase(name);
  clear_sparse_array(name);
  indexed_arrays().set(name, steal(values));
}

fn EvalContext::publish_pipe_statuses(ArrayList<String> values) throws -> void
{
  if (is_readonly("PIPESTATUS")) {
    if (let *current = indexed_arrays().find("PIPESTATUS"); current != nullptr)
      *current = steal(values);

    return;
  }

  set_indexed_array("PIPESTATUS", steal(values));
}

fn EvalContext::publish_single_pipe_status(i32 status) throws -> void
{
  let *existing = indexed_arrays().find("PIPESTATUS");
  if (existing == nullptr && is_readonly("PIPESTATUS")) return;

  if (existing != nullptr && existing->count() == 1 &&
      !sparse_array_names().contains("PIPESTATUS"))
  {
    m_shell_variables.erase("PIPESTATUS");
    char status_text_buffer[32];
    let const status_text = utils::int_to_text_into(status, status_text_buffer,
                                                    sizeof(status_text_buffer));
    if ((*existing)[0] != status_text)
      (*existing)[0] = String{existing->allocator(), status_text};
    return;
  }

  m_shell_variables.erase("PIPESTATUS");
  clear_sparse_array("PIPESTATUS");
  let &values = indexed_arrays().get_or_create(
      "PIPESTATUS", ArrayList<String>{heap_allocator()});
  values.clear();
  values.push(String::from(status, values.allocator()));
}

fn EvalContext::append_indexed_array(StringView name,
                                     ArrayList<String> values) throws -> void
{
  if (is_write_discarded_dynamic_variable(name)) return;

  if (let *existing = indexed_arrays().find(name); existing != nullptr) {
    LOG(All, "appending %zu elements to the existing array '%.*s'",
        values.count(), static_cast<int>(name.length), name.data);
    if (is_readonly(name))
      throw Error{"Unable to assign '" + name + "' because it is read only"};
    if (is_lowercase_variable(name) || is_uppercase_variable(name)) [[unlikely]]
      for (let &value : values)
        apply_variable_case(name, value);
    m_shell_variables.erase(name);
    for (let &element : values)
      existing->push(steal(element));
    return;
  }
  set_indexed_array(name, steal(values));
}

/* The script-fatal mark aborts the whole run, unlike the command-level errors
   the bash mood continues past. */
[[noreturn]] fn throw_script_fatal(StringView message, StringView note) throws
    -> void
{
  if (note.is_empty()) {
    Error error{message};
    error.set_script_fatal();
    throw steal(error);
  }

  ErrorWithDetails error{message, note};
  error.set_script_fatal();
  throw steal(error);
}

cold fn EvalContext::show_runtime_warning(StringView message) wontthrow -> void
{
  show_runtime_warning_at(m_current_location, message);
}

cold fn EvalContext::show_runtime_warning_at(
    SourceLocation location, StringView message, StringView note,
    bool should_ignore_disabled) wontthrow -> void
{
  if (diagnostics_disabled() && !should_ignore_disabled) return;
  let const trace_location = location;
  /* The stamped view may outlive its buffer once the defining command's sources
     are freed, so a windowed resolution swaps in the definition copy's owned
     filename. */
  try {
    let const resolved_source = resolve_render_source(location);
    isize line_offset = 0;
    if (resolved_source.is_windowed) {
      location.position = static_cast<u32>(
          resolved_source.to_render_position(location.position));
      location.source_name_index = resolved_source.source_name_index;
      line_offset = resolved_source.line_offset;
    }
    if (resolved_source.text == nullptr ||
        location.position > resolved_source.text->count())
    {
      show_message(WarningWithDetails{message, note}.to_string());
      return;
    }
    let warning = WarningWithLocationAndDetails{location, message, note};
    warning.set_line_offset(line_offset);
    show_message(warning.to_string(resolved_source.text->view(), this));
    if (!m_source_frames.is_empty()) print_source_backtrace(trace_location);
  } catch (...) {
    LOG(Debug, "formatting a runtime warning failed, the error is swallowed");
  }
}

cold fn EvalContext::show_runtime_error_at(SourceLocation location,
                                           StringView message) wontthrow -> void
{
  let const trace_location = location;
  try {
    let const resolved_source = resolve_render_source(location);
    isize line_offset = 0;
    if (resolved_source.is_windowed) {
      location.position = static_cast<u32>(
          resolved_source.to_render_position(location.position));
      location.source_name_index = resolved_source.source_name_index;
      line_offset = resolved_source.line_offset;
    }
    if (resolved_source.text == nullptr ||
        location.position > resolved_source.text->count())
    {
      show_message(Error{message}.to_string());
      return;
    }
    let error = ErrorWithLocation{location, message};
    error.set_line_offset(line_offset);
    show_message(error.to_string(resolved_source.text->view(), this));
    if (!m_source_frames.is_empty()) print_source_backtrace(trace_location);
  } catch (...) {
    LOG(Debug, "formatting a runtime error failed, the error is swallowed");
  }
}

pure fn EvalContext::locate_variable_reference(StringView name) const wontthrow
    -> SourceLocation
{
  let fallback = m_current_location;
  if (name.is_empty()) return fallback;
  let const resolved_source = resolve_render_source(fallback);
  if (resolved_source.text == nullptr) return fallback;
  let const source = resolved_source.text->view();

  usize scan_start = fallback.position;
  usize absolute_shift = 0;
  if (resolved_source.is_windowed) {
    scan_start = resolved_source.to_render_position(fallback.position);
    absolute_shift =
        resolved_source.body_start_position > resolved_source.header_length
            ? resolved_source.body_start_position -
                  resolved_source.header_length
            : 0;
  }
  if (scan_start >= source.length) return fallback;

  /* The byte after the name must end it so $FOO does not match $FOOBAR. */
  usize i = scan_start;
  while (i < source.length) {
    let const byte = source[i];
    if (byte == '\n' && (i == 0 || source[i - 1] != '\\')) {
      break;
    }
    if (byte != '$' || i + 1 >= source.length) {
      i++;
      continue;
    }
    usize name_start = i + 1;
    let const is_braced = source[name_start] == '{';
    if (is_braced) name_start++;
    if (name_start + name.length <= source.length &&
        source.substring_of_length(name_start, name.length) == name &&
        (name_start + name.length == source.length ||
         !lexer::is_variable_name(source[name_start + name.length])))
    {
      usize reference_end = name_start + name.length;
      if (is_braced && reference_end < source.length &&
          source[reference_end] == '}')
      {
        reference_end++;
      }
      return SourceLocation{i + absolute_shift, reference_end - i,
                            fallback.source_name_index};
    }
    i++;
  }

  /* Arithmetic reads a variable as a bare name, so a second pass takes the
     first name-delimited spelling. */
  usize k = scan_start;
  while (k + name.length <= source.length) {
    let const byte = source[k];
    if (byte == '\n' && (k == 0 || source[k - 1] != '\\')) {
      break;
    }
    if (source.substring_of_length(k, name.length) == name &&
        (k == 0 || !lexer::is_variable_name(source[k - 1])) &&
        (k + name.length == source.length ||
         !lexer::is_variable_name(source[k + name.length])))
    {
      return SourceLocation{k + absolute_shift, name.length,
                            fallback.source_name_index};
    }
    k++;
  }
  return fallback;
}

fn EvalContext::report_unset_reference(StringView name) throws -> void
{
  /* bash does not nounset on the operand of [[ -v name ]]. */
  if (is_warning_suppressed(suppressible_warning::UnsetReference)) return;

  let empty_expansion_note =
      "Replace it with ${" + String{name} + "-} if empty expansion is desired";

  if (Maybe<String> resembled = suggest_similar_variable_name(name);
      resembled.has_value())
  {
    empty_expansion_note = "The variable '" + *resembled +
                           "' is set, correct the spelling, or replace this "
                           "with ${" +
                           String{name} + "-} if empty expansion is desired";
  }

  let const should_demote = strict_diagnostics_are_warnings();
  if (error_unset() &&
      (m_runtime.was_error_unset_set_explicitly() || !should_demote) &&
      !is_warning_suppressed(suppressible_warning::UnsetTestOperand))
  {
    let const message = "Unable to expand '" + String{name} +
                        "' because the parameter is not set";

    let const reference = locate_variable_reference(name);
    if (reference.position == m_current_location.position &&
        reference.length == m_current_location.length)
    {
      throw_script_fatal(String{message}, empty_expansion_note.view());
    }

    ErrorWithLocationAndDetails error{reference, message,
                                      empty_expansion_note.view()};
    error.set_script_fatal();
    throw steal(error);
  }
  if (is_completion_function_running()) return;
  if (is_warning_suppressed(suppressible_warning::UnsetTestOperand)) return;

  if (error_unset() || should_demote) {
    show_runtime_warning_at(locate_variable_reference(name),
                            "The variable '" + String{name} +
                                "' is not set, it expands to empty",
                            empty_expansion_note.view());
  }
}

fn EvalContext::warn_or_throw(bool fatal, bool explicitly_requested,
                              const SourceLocation &location,
                              StringView message, StringView note) throws
    -> void
{
  let const should_demote = strict_diagnostics_are_warnings();
  if (fatal && (explicitly_requested || !should_demote)) {
    if (note.is_empty()) throw ErrorWithLocation{location, message};
    throw ErrorWithLocationAndDetails{location, message, note};
  }
  if (is_completion_function_running()) return;
  if ((fatal || should_demote) && !diagnostics_disabled() &&
      m_current_source != nullptr)
  {
    try {
      let warning = WarningWithLocationAndDetails{location, message, note};
      show_message(warning.to_string(m_current_source->view(), this));
    } catch (...) {
      LOG(Debug, "showing a located warning failed, the error is swallowed");
    }
  }
}

fn EvalContext::force_unset_shell_variable(StringView name) throws -> void
{
  LOG(All, "removing variable '%.*s' from the store and the environment",
      static_cast<int>(name.length), name.data);
  m_shell_variables.erase(name);
  if (is_prompt_special_variable(name))
    m_special_variable_definition_locations.erase(name);
  record_environment_change(name);
  os::unset_environment_variable(name);
  unmark_exported(name);
  if (name == "IFS") set_field_separators(" \t\n");
  if (utils::environment_name_is_path(name))
    m_program_resolver.assign_path(os::get_environment_variable("PATH"));
  if (name == "IGNOREEOF")
    m_runtime.set_option(shell_option_id::Ignoreeof, false);
}

pure fn EvalContext::special_variable_definition_location(
    StringView name) const wontthrow -> Maybe<SourceLocation>
{
  let const *location = m_special_variable_definition_locations.find(name);
  if (location == nullptr) return None;
  return *location;
}

fn EvalContext::record_environment_change(StringView name) throws -> void
{
  if (m_subshell_depth == 0) return;
  m_environment_undo_log.push(
      environment_undo_entry{
          String{name}, os::get_environment_variable(name), None
      });
}

static constexpr usize EXPORTED_NAME_FOLD_BYTES = 64;

/* A name that fits the buffer folds without touching an allocator, and a longer
   name folds into the caller's string. The result borrows from whichever of the
   two holds it. Both outlive the lookup. */
static fn fold_exported_name(StringView name,
                             char (&buffer)[EXPORTED_NAME_FOLD_BYTES],
                             String &spill) throws -> StringView
{
  if (name.length > EXPORTED_NAME_FOLD_BYTES) {
    spill.append(name);
    spill.lowercase_ascii();

    return spill.view();
  }

  for (usize position = 0; position < name.length; position++)
    buffer[position] = utils::ascii_to_lower(name[position]);

  return StringView{buffer, name.length};
}

/* The stored value keeps the original spelling only where the environment
   ignores case and folding changed the name. */
template <typename Value>
static fn store_exported_name(StringMap<Value> &names, StringView key,
                              StringView spelling) throws -> void
{
  if constexpr (os::ENVIRONMENT_IS_CASE_SENSITIVE) {
    unused(spelling);
    names.set(key, Nothing{});
  } else {
    /* The empty default costs no allocation. One probe both finds an existing
       name and places a new one. */
    let const previous_count = names.count();
    let &display_name = names.get_or_create(key, String{heap_allocator()});
    if (names.count() != previous_count && key != spelling) {
      display_name.append(spelling);
    }
  }
}

fn EvalContext::mark_exported(StringView name) throws -> void
{
  LOG(All, "marking '%.*s' as exported", static_cast<int>(name.length),
      name.data);
  if constexpr (os::ENVIRONMENT_IS_CASE_SENSITIVE) {
    store_exported_name(exported_names(), name, name);
    return;
  }

  char folded[EXPORTED_NAME_FOLD_BYTES];
  let spill = String{heap_allocator()};
  store_exported_name(exported_names(), fold_exported_name(name, folded, spill),
                      name);
}

fn EvalContext::unmark_exported(StringView name) throws -> void
{
  if constexpr (os::ENVIRONMENT_IS_CASE_SENSITIVE) {
    exported_names().erase(name);
    return;
  }

  char folded[EXPORTED_NAME_FOLD_BYTES];
  let spill = String{heap_allocator()};
  exported_names().erase(fold_exported_name(name, folded, spill));
}

fn EvalContext::unexport_shell_variable(StringView name) throws -> void
{
  let const has_shell_binding = m_shell_variables.find(name) != nullptr ||
                                indexed_arrays().find(name) != nullptr ||
                                associative_names().contains(name) ||
                                is_local_in_current_scope(name) ||
                                variable_requires_dynamic_lookup(name);
  let const environment_value =
      has_shell_binding ? Maybe<String>{} : os::get_environment_variable(name);
  record_environment_change(name);
  os::unset_environment_variable(name);
  unmark_exported(name);
  if (environment_value.has_value())
    assign_variable(name, environment_value->view());
}

fn EvalContext::is_exported(StringView name) const throws -> bool
{
  if constexpr (os::ENVIRONMENT_IS_CASE_SENSITIVE)
    return exported_names().find(name) != nullptr;

  char folded[EXPORTED_NAME_FOLD_BYTES];
  let spill = String{heap_allocator()};
  return exported_names().find(fold_exported_name(name, folded, spill)) !=
         nullptr;
}

fn EvalContext::sync_exported_after_restore(StringView name,
                                            bool has_value) throws -> void
{
  if (has_value)
    mark_exported(name);
  else
    unmark_exported(name);
}

fn EvalContext::set_positional_params(ArrayList<String> params) wontthrow
    -> void
{
  variable_store().positional_params() = steal(params);
}

fn EvalContext::take_positional_params() wontthrow -> ArrayList<String>
{
  return steal(variable_store().positional_params());
}

pure fn EvalContext::is_bash_argument_array(StringView name) const wontthrow
    -> bool
{
  return bash_dynamic_variables_enabled() &&
         (name == BASH_ARGUMENT_COUNT_VARIABLE ||
          name == BASH_ARGUMENT_VALUE_VARIABLE);
}

fn EvalContext::initialize_bash_argument_arrays(
    bool should_include_current_frame) const throws -> void
{
  if (m_bash_argument_arrays != nullptr) return;

  let values = ArrayList<String>{heap_allocator()};
  let frame_counts = ArrayList<u32>{heap_allocator()};
  if (should_include_current_frame) {
    let const is_source_frame = m_bash_argument_frame_context != nullptr &&
                                m_bash_argument_frame_context->has_flag(
                                    BashArgumentFrameFlag::IsSource);
    let const has_source_arguments =
        is_source_frame && m_bash_argument_frame_context->has_flag(
                               BashArgumentFrameFlag::HasSourceArguments);
    let const uses_source_path = is_source_frame && !has_source_arguments;
    let const argument_count =
        uses_source_path ? usize{1} : positional_params().count();
    values.reserve(argument_count);
    frame_counts.reserve(1);
    if (uses_source_path) {
      values.push_managed(m_bash_argument_frame_context->source_path);
    } else {
      for (let const &argument : positional_params())
        values.push_managed(argument.view());
    }
    frame_counts.push(static_cast<u32>(argument_count));
  }

  install_bash_argument_arrays(steal(values), steal(frame_counts));
}

fn EvalContext::install_bash_argument_arrays(
    ArrayList<String> values, ArrayList<u32> frame_counts) const throws -> void
{
  ASSERT(m_bash_argument_arrays == nullptr);
  let const storage = heap_allocator().alloc_array<BashArgumentArrayStorage>(1);
  if (storage == nullptr) throw std::bad_alloc{};
  try {
    new (storage) BashArgumentArrayStorage{steal(values), steal(frame_counts)};
  } catch (...) {
    heap_allocator().free_array(storage, 1);
    throw;
  }
  m_bash_argument_arrays = storage;
}

fn EvalContext::reset_bash_argument_arrays() const wontthrow -> void
{
  if (m_bash_argument_arrays == nullptr) return;
  m_bash_argument_arrays->~BashArgumentArrayStorage();
  heap_allocator().free_array(m_bash_argument_arrays, 1);
  m_bash_argument_arrays = nullptr;
}

fn EvalContext::append_bash_argument_frame(
    const ArrayList<String> &arguments) const throws -> void
{
  ASSERT(m_bash_argument_arrays != nullptr);
  let &values = m_bash_argument_arrays->values;
  let &frame_counts = m_bash_argument_arrays->frame_counts;
  let const previous_value_count = values.count();
  values.reserve(previous_value_count + arguments.count());
  frame_counts.reserve(frame_counts.count() + 1);
  try {
    for (let const &argument : arguments)
      values.push_managed(argument.view());
  } catch (...) {
    while (values.count() > previous_value_count)
      values.pop_back();
    throw;
  }
  frame_counts.push(static_cast<u32>(arguments.count()));
}

fn EvalContext::append_bash_argument_frame(StringView argument) const throws
    -> void
{
  ASSERT(m_bash_argument_arrays != nullptr);
  let &values = m_bash_argument_arrays->values;
  let &frame_counts = m_bash_argument_arrays->frame_counts;
  values.reserve(values.count() + 1);
  frame_counts.reserve(frame_counts.count() + 1);
  values.push_managed(argument);
  frame_counts.push(1);
}

fn EvalContext::append_current_bash_argument_frame() const throws -> void
{
  ASSERT(m_bash_argument_frame_context != nullptr);
  let const is_source_frame =
      m_bash_argument_frame_context->has_flag(BashArgumentFrameFlag::IsSource);
  let const has_source_arguments = m_bash_argument_frame_context->has_flag(
      BashArgumentFrameFlag::HasSourceArguments);
  if (is_source_frame && !has_source_arguments) {
    append_bash_argument_frame(m_bash_argument_frame_context->source_path);
  } else {
    append_bash_argument_frame(positional_params());
  }
}

fn EvalContext::enter_bash_function_argument_frame(
    BashArgumentFrameContext &frame_context,
    const ArrayList<String> &arguments) throws -> void
{
  frame_context.previous = m_bash_argument_frame_context;
  frame_context.source_path = {};
  frame_context.flags = 0;

  if (bash_dynamic_variables_enabled() &&
      is_shopt_enabled(shopt_option_id::Extdebug))
  {
    initialize_bash_argument_arrays(true);
    append_bash_argument_frame(arguments);
    frame_context.set_flag(BashArgumentFrameFlag::DidEnter);
  }

  m_bash_argument_frame_context = &frame_context;
}

fn EvalContext::enter_bash_source_argument_frame(
    BashArgumentFrameContext &frame_context, const ArrayList<String> *arguments,
    StringView source_path) throws -> void
{
  frame_context.previous = m_bash_argument_frame_context;
  frame_context.source_path = source_path;
  frame_context.flags = 0;
  frame_context.set_flag(BashArgumentFrameFlag::IsSource);
  if (arguments != nullptr)
    frame_context.set_flag(BashArgumentFrameFlag::HasSourceArguments);

  if (bash_dynamic_variables_enabled()) {
    if (is_shopt_enabled(shopt_option_id::Extdebug)) {
      initialize_bash_argument_arrays(true);
      if (arguments != nullptr)
        append_bash_argument_frame(*arguments);
      else
        append_bash_argument_frame(source_path);
      frame_context.set_flag(BashArgumentFrameFlag::DidEnter);
    } else if (arguments == nullptr) {
      initialize_bash_argument_arrays(m_bash_argument_frame_context == nullptr);
      append_bash_argument_frame(source_path);
      frame_context.set_flag(BashArgumentFrameFlag::DidEnter);
    } else if (m_bash_argument_arrays == nullptr) {
      initialize_bash_argument_arrays(false);
      if (m_bash_argument_frame_context == nullptr)
        append_bash_argument_frame(*arguments);
    }
  }

  m_bash_argument_frame_context = &frame_context;
}

fn EvalContext::leave_bash_argument_frame(
    BashArgumentFrameContext &frame_context) wontthrow -> void
{
  ASSERT(m_bash_argument_frame_context == &frame_context);
  m_bash_argument_frame_context = frame_context.previous;
  if (!frame_context.has_flag(BashArgumentFrameFlag::DidEnter)) return;

  ASSERT(m_bash_argument_arrays != nullptr);
  let &values = m_bash_argument_arrays->values;
  let &frame_counts = m_bash_argument_arrays->frame_counts;
  ASSERT(!frame_counts.is_empty());
  let const argument_count = frame_counts.back();
  frame_counts.pop_back();
  ASSERT(argument_count <= values.count());
  for (u32 index = 0; index < argument_count; index++)
    values.pop_back();
}

fn EvalContext::enter_function_scope() throws -> void
{
  if (m_local_scope_depth == m_local_scopes.count())
    m_local_scopes.push(ArrayList<local_binding>{heap_allocator()});
  ASSERT(m_local_scopes[m_local_scope_depth].is_empty());
  m_local_scope_depth++;
  LOG(Debug, "entered function scope, local scope depth now %zu",
      m_local_scope_depth);
}

fn EvalContext::leave_function_scope() throws -> void
{
  if (m_local_scope_depth == 0) return;

  /* Restore each shadowed binding in reverse, so a name declared local twice
     ends with the value it held before the function ran. */
  ASSERT(m_local_scope_depth <= m_local_scopes.count());
  let &scope = m_local_scopes[m_local_scope_depth - 1];
  LOG(Debug, "leaving function scope, restoring %zu shadowed locals",
      scope.count());
  for (usize i = scope.count(); i > 0; i--) {
    ASSERT(i - 1 < scope.count());
    restore_local_binding(scope[i - 1]);
  }
  scope.clear();
  m_local_scope_depth--;
  constexpr usize RETAINED_LOCAL_SCOPE_COUNT = 16;
  if (m_local_scopes.count() > RETAINED_LOCAL_SCOPE_COUNT &&
      m_local_scopes.count() > m_local_scope_depth)
  {
    m_local_scopes.remove(m_local_scopes.count() - 1);
  }
}

fn EvalContext::push_function_call_name(
    StringView name, const FunctionBodyHandle &body_storage) throws -> void
{
  let owned_name = String{heap_allocator(), name};
  m_function_call_names.reserve(m_function_call_names.count() + 1);
  m_function_call_storages.reserve(m_function_call_storages.count() + 1);
  m_function_call_locations.reserve(m_function_call_locations.count() + 1);
  m_function_call_sources.reserve(m_function_call_sources.count() + 1);
  m_function_call_names.push(steal(owned_name));
  m_function_call_storages.push(body_storage);
  m_function_call_locations.push(m_current_location);
  m_function_call_sources.push(m_current_source);
}

fn EvalContext::pop_function_call_name() wontthrow -> void
{
  if (!m_function_call_names.is_empty()) {
    m_function_call_names.remove(m_function_call_names.count() - 1);
    m_function_call_storages.remove(m_function_call_storages.count() - 1);
    m_function_call_locations.remove(m_function_call_locations.count() - 1);
    m_function_call_sources.remove(m_function_call_sources.count() - 1);
  }
}

pure fn EvalContext::script_source_frame_index() const wontthrow -> Maybe<usize>
{
  if (!m_is_script_run) return None;

  for (usize i = 0; i < m_source_frames.count(); i++) {
    let const &path = m_source_frames[i].source_path;
    if (path.is_empty()) continue;

    if (path.view() == m_shell_name.view()) return i;

    return None;
  }

  return None;
}

pure fn EvalContext::merged_frame_at(
    usize index, usize total, Maybe<usize> script_source_index) const wontthrow
    -> MergedFrame
{
  if (index >= total) return MergedFrame{MergedFrame::Kind::Main, 0};

  let const target = total - 1 - index;
  usize emitted_count = 0;

  if (m_is_script_run && !script_source_index.has_value()) {
    if (target == 0) return MergedFrame{MergedFrame::Kind::Main, 0};
    emitted_count = 1;
  }

  let const function_count = m_function_call_names.count();
  usize function_index = 0;
  usize source_index = 0;

  loop
  {
    while (source_index < m_source_frames.count() &&
           m_source_frames[source_index].source_path.is_empty())
    {
      source_index++;
    }

    let const has_source = source_index < m_source_frames.count();
    let const has_function = function_index < function_count;
    if (!has_source && !has_function) break;

    if (has_source &&
        m_source_frames[source_index].function_call_depth <= function_index)
    {
      if (emitted_count == target) {
        if (script_source_index.has_value() &&
            *script_source_index == source_index)
        {
          return MergedFrame{MergedFrame::Kind::Main, source_index};
        }

        return MergedFrame{MergedFrame::Kind::Source, source_index};
      }

      emitted_count++;
      source_index++;
      continue;
    }

    if (emitted_count == target)
      return MergedFrame{MergedFrame::Kind::Function, function_index};

    emitted_count++;
    function_index++;
  }

  return MergedFrame{MergedFrame::Kind::Main, 0};
}

pure fn EvalContext::merged_frame_at(usize index) const wontthrow -> MergedFrame
{
  let const script_source_index = script_source_frame_index();

  return merged_frame_at(index, bash_source_frame_count(script_source_index),
                         script_source_index);
}

fn EvalContext::funcname_frame_count() const wontthrow -> usize
{
  if (m_function_call_names.is_empty()) return 0;
  return bash_source_frame_count();
}

fn EvalContext::funcname_frame_at(usize index) const wontthrow -> StringView
{
  let const frame = merged_frame_at(index);
  switch (frame.kind) {
  case MergedFrame::Kind::Function:
    return m_function_call_names[frame.storage_index].view();
  case MergedFrame::Kind::Source: return StringView{"source"};
  case MergedFrame::Kind::Main: break;
  }

  return StringView{"main"};
}

fn EvalContext::line_number_at_location(
    const SourceLocation &location, const String *fallback_source) const throws
    -> usize
{
  let const resolved_source = resolve_render_source(location, fallback_source);
  usize line = 1;
  if (resolved_source.text != nullptr) {
    const usize render_position =
        resolved_source.to_render_position(location.position);
    let const render_line = static_cast<isize>(
        utils::line_number_at(resolved_source.text->view(), render_position));
    line = static_cast<usize>(render_line + (resolved_source.is_windowed
                                                 ? resolved_source.line_offset
                                                 : 0));
  }
  return line;
}

fn EvalContext::funcname_line_at(usize index) const throws -> usize
{
  let const frame = merged_frame_at(index);
  switch (frame.kind) {
  case MergedFrame::Kind::Function:
    return line_number_at_location(
        m_function_call_locations[frame.storage_index],
        m_function_call_sources[frame.storage_index]);
  case MergedFrame::Kind::Source: {
    let const &source = m_source_frames[frame.storage_index];
    return line_number_at_location(source.call_site,
                                   borrowed_frame_source(source));
  }
  case MergedFrame::Kind::Main: break;
  }

  return 0;
}

pure fn EvalContext::bash_source_frame_at(usize index) const wontthrow
    -> StringView
{
  let const script_source_index = script_source_frame_index();
  let const total = bash_source_frame_count(script_source_index);
  if (index >= total) return StringView{};

  let const frame = merged_frame_at(index, total, script_source_index);
  switch (frame.kind) {
  case MergedFrame::Kind::Function: {
    let const *info =
        m_function_call_storages[frame.storage_index].get_definition_info();
    if (info != nullptr) {
      if (let const name = source_name_at(info->source_name_index);
          name.has_value())
      {
        return *name;
      }
    }

    return m_shell_name.view();
  }
  case MergedFrame::Kind::Source:
    return m_source_frames[frame.storage_index].source_path.view();
  case MergedFrame::Kind::Main: break;
  }

  return m_shell_name.view();
}

pure fn EvalContext::bash_source_frame_count(
    Maybe<usize> script_source_index) const wontthrow -> usize
{
  usize frame_count = m_function_call_names.count();

  for (usize i = 0; i < m_source_frames.count(); i++) {
    if (!m_source_frames[i].source_path.is_empty()) frame_count++;
  }

  if (m_is_script_run && !script_source_index.has_value()) frame_count++;

  return frame_count;
}

pure fn EvalContext::bash_source_frame_count() const wontthrow -> usize
{
  return bash_source_frame_count(script_source_frame_index());
}

fn EvalContext::dynamic_array_element_count(DynamicArray which) const throws
    -> usize
{
  switch (which) {
  case DynamicArray::ArgumentCount:
  case DynamicArray::ArgumentValue: {
    let const should_include_current_frame =
        m_bash_argument_frame_context != nullptr
            ? m_bash_argument_frame_context->has_flag(
                  BashArgumentFrameFlag::IsSource)
            : m_function_call_names.is_empty();
    initialize_bash_argument_arrays(should_include_current_frame);
    ASSERT(m_bash_argument_arrays != nullptr);
    return which == DynamicArray::ArgumentCount
               ? m_bash_argument_arrays->frame_counts.count()
               : m_bash_argument_arrays->values.count();
  }
  case DynamicArray::SourcePath: return bash_source_frame_count();
  case DynamicArray::FunctionName: return funcname_frame_count();
  case DynamicArray::LineNumber: return bash_source_frame_count();
  }

  return 0;
}

fn EvalContext::dynamic_array_element_text(
    DynamicArray which, usize index, Allocator result_allocator) const throws
    -> String
{
  switch (which) {
  case DynamicArray::ArgumentCount: {
    unused(dynamic_array_element_count(which));
    ASSERT(m_bash_argument_arrays != nullptr);
    let const &frame_counts = m_bash_argument_arrays->frame_counts;
    ASSERT(index < frame_counts.count());
    let const storage_index = frame_counts.count() - 1 - index;
    return String::from(frame_counts[storage_index], result_allocator);
  }
  case DynamicArray::ArgumentValue: {
    unused(dynamic_array_element_count(which));
    ASSERT(m_bash_argument_arrays != nullptr);
    let const &values = m_bash_argument_arrays->values;
    ASSERT(index < values.count());
    return String{result_allocator, values[values.count() - 1 - index].view()};
  }
  case DynamicArray::FunctionName:
    return String{result_allocator, funcname_frame_at(index)};
  case DynamicArray::LineNumber:
    return String::from(funcname_line_at(index), result_allocator);
  case DynamicArray::SourcePath:
    return String{result_allocator, bash_source_frame_at(index)};
  }

  return String{result_allocator};
}

pure fn EvalContext::in_function_scope() const wontthrow -> bool
{
  return m_local_scope_depth != 0;
}

fn EvalContext::is_local_in_current_scope(StringView name) const wontthrow
    -> bool
{
  if (m_local_scope_depth == 0) return false;
  for (let const &binding : m_local_scopes[m_local_scope_depth - 1])
    if (binding.name.view() == name) return true;
  return false;
}

fn EvalContext::is_local_in_any_active_scope(StringView name) const wontthrow
    -> bool
{
  for (usize frame_index = m_local_scope_depth; frame_index-- > 0;)
    for (let const &binding : m_local_scopes[frame_index])
      if (binding.name.view() == name) return true;

  return false;
}

fn EvalContext::set_alias(StringView name, StringView value) throws -> void
{
  LOG(All, "setting alias '%.*s' to a %zu byte value",
      static_cast<int>(name.length), name.data, value.length);
  m_aliases.set(name, value);
}

fn EvalContext::remove_alias(StringView name) throws -> bool
{
  if (m_aliases.find(name) == nullptr) return false;
  LOG(All, "removing alias '%.*s'", static_cast<int>(name.length), name.data);
  m_aliases.erase(name);
  return true;
}

pure fn EvalContext::has_aliases() const wontthrow -> bool
{
  return m_aliases.count() != 0;
}

fn EvalContext::get_alias(StringView name) const throws -> Maybe<String>
{
  if (let const *value = m_aliases.find(name); value != nullptr)
    return String{heap_allocator(), value->view()};
  return None;
}

fn EvalContext::alias_definitions() const throws -> ArrayList<String>
{
  let out = ArrayList<String>{heap_allocator()};
  m_aliases.for_each([&out](StringView key, const String &value) {
    let definition = String{heap_allocator(), key};
    definition.push('=');
    append_shell_quoted_arg(definition, value.view());
    out.push(steal(definition));
  });
  out.sort();
  return out;
}

fn EvalContext::alias_names() const throws -> HashSet
{
  let out = HashSet{heap_allocator()};
  m_aliases.for_each([&out](StringView key, const String &value) {
    unused(value);
    out.add(key);
  });
  return out;
}

ExecContext::ExecContext(SourceLocation location, ResolvedCommand &&kind,
                         ArrayList<String> &&args,
                         ArrayList<SourceLocation> &&arg_locations)
    : m_kind(steal(kind)), m_location(steal(location)), m_args(steal(args)),
      m_arg_locations(steal(arg_locations))
{}

pure fn ExecContext::source_location() const wontthrow -> const SourceLocation &
{
  return m_location;
}

pure fn ExecContext::program() const wontthrow -> const String &
{
  ASSERT(!m_args.is_empty());
  return m_args[0];
}

pure fn ExecContext::args() const wontthrow -> const ArrayList<String> &
{
  return m_args;
}

pure fn ExecContext::arg_locations() const wontthrow
    -> const ArrayList<SourceLocation> &
{
  return m_arg_locations;
}

pure fn ExecContext::arg_location_at(usize index) const wontthrow
    -> SourceLocation
{
  if (index < m_arg_locations.count()) return m_arg_locations[index];
  return m_location;
}

pure fn ExecContext::is_builtin() const wontthrow -> bool
{
  return m_kind.is_builtin();
}

pure fn ExecContext::is_unresolved() const wontthrow -> bool
{
  return m_kind.is_unresolved();
}

pure fn ExecContext::get_unresolved_status() const wontthrow -> i32
{
  ASSERT(is_unresolved());
  return m_kind.unresolved_status;
}

pure fn ExecContext::get_unresolved_diagnostic() const wontthrow -> StringView
{
  ASSERT(is_unresolved());
  return m_unresolved_diagnostic.view();
}

pure fn ExecContext::program_path() const wontthrow -> const Path &
{
  ASSERT(!is_builtin());
  return m_kind.program_path;
}

fn ExecContext::set_program_path(Path path) throws -> void
{
  ASSERT(!is_builtin());
  m_kind.program_path = steal(path);
}

fn ExecContext::close_fds() throws -> void
{
  if (in_fd.has_value()) {
    if (!is_in_fd_borrowed) os::close_fd(*in_fd);
    in_fd.reset();
    is_in_fd_borrowed = false;
  }
  if (out_fd.has_value()) {
    if (!is_out_fd_borrowed) os::close_fd(*out_fd);
    out_fd.reset();
    is_out_fd_borrowed = false;
  }
  if (err_fd.has_value()) {
    if (!is_err_fd_borrowed) os::close_fd(*err_fd);
    err_fd.reset();
    is_err_fd_borrowed = false;
  }

  for (let const &binding : nonstandard_fds) {
    if (binding.file_fd != KOSH_INVALID_FD && !binding.is_file_borrowed) {
      os::close_fd(binding.file_fd);
    }
  }
  nonstandard_fds.clear();
}

pure fn ExecContext::builtin_kind() const wontthrow -> const Builtin::Kind &
{
  ASSERT(is_builtin());
  return m_kind.builtin_kind;
}

fn ExecContext::print_to_stdout(StringView s) const throws -> void
{
  if (!os::write_all(out_fd.value_or(KOSH_STDOUT), s.data, s.length)) {
    let const saved_errno = errno;
    if (saved_errno == EPIPE) throw BrokenPipeExit{};
    throw Error{"Unable to write to stdout: " +
                os::last_system_error_message()};
  }
}

fn ExecContext::print_to_stderr(StringView s) const throws -> void
{
  if (!os::write_all(err_fd.value_or(KOSH_STDERR), s.data, s.length)) {
    let const saved_errno = errno;
    if (saved_errno == EPIPE) throw BrokenPipeExit{};
    throw Error{"Unable to write to stderr: " +
                os::last_system_error_message()};
  }
}

fn ExecContext::make_from(const SourceLocation &location, StringView source,
                          ArrayList<String> &&args, mimic_mood mood,
                          bool are_koshkit_utilities_reachable,
                          bool should_check_hash,
                          ProgramResolver &program_resolver,
                          ArrayList<SourceLocation> &&arg_locations) throws
    -> ExecContext
{
  ASSERT(args.count() > 0);

  let const &program = args[0];
  let resolution_location =
      arg_locations.is_empty() ? location : arg_locations[0];
  let resolution_program = program.view();
  let is_missing_directory = false;

  Maybe<Builtin::Kind> resolved_builtin;
  Maybe<Path> resolved_program_path;
  Maybe<utils::unavailable_path_source_component> unavailable_component;

  if (!os::has_directory_separator(program.view())) {
    resolved_builtin = search_builtin(program.view());

    if (resolved_builtin.has_value() &&
        builtin_is_hidden_by_mood(*resolved_builtin, mood))
    {
      resolved_builtin = None;
    }

    if (!resolved_builtin.has_value()) {
      let program_search_paths = program_resolver.search(
          program.view(), ProgramResolver::SearchMode::First,
          ProgramResolver::Requirement::Execution,
          should_check_hash ? ProgramResolver::CachePolicy::Remember
                            : ProgramResolver::CachePolicy::RememberUnchecked);
      if (program_search_paths.count() > 0)
        resolved_program_path = steal(program_search_paths[0]);
    }
  } else {
    let const typed_program_path = Path{program.view()};
    if (typed_program_path.has_trailing_separator()) {
      let const raw_program_path =
          typed_program_path.to_absolute_without_normalizing();
      resolved_program_path = os::canonical_path(raw_program_path);
    } else {
      resolved_program_path = Path::canonicalize(program.view());
    }
    if (resolved_program_path.has_value() &&
        typed_program_path.has_trailing_separator() &&
        !resolved_program_path->is_directory())
    {
      throw CommandResolutionErrorWithLocation{
          resolution_location, "This file is not a directory", 126};
    }
    if (!resolved_program_path.has_value()) {
      let raw_program = program.view();
      if (let source_text = resolution_location.get_source_text(source))
        raw_program = *source_text;
      let const target = typed_program_path.to_absolute_without_normalizing();
      unavailable_component = utils::locate_first_unavailable_path_component(
          target, program.view(), raw_program, resolution_location,
          heap_allocator());
      if (unavailable_component.has_value()) {
        resolution_location = unavailable_component->location;
        resolution_program = unavailable_component->reported_prefix.view();
        is_missing_directory = !unavailable_component->is_final_component;
        if (unavailable_component->is_not_directory) {
          throw CommandResolutionErrorWithLocation{
              resolution_location, "This file is not a directory", 126};
        }
      }
    }
  }

  ResolvedCommand kind;
  if (!resolved_builtin) {
    if (resolved_program_path.has_value()) {
      LOG(Debug, "resolved '%s' to the program '%s'", program.c_str(),
          resolved_program_path->text().c_str());
      kind = ResolvedCommand::from_program(steal(*resolved_program_path));
    } else if (are_koshkit_utilities_reachable &&
               koshkit::find_util(program.view()).has_value())
    {
      LOG(Debug, "no program matches '%s', using the koshkit utility",
          program.c_str());
      kind = ResolvedCommand::from_builtin(Builtin::Kind::Koshkit);
    } else {
      LOG(Debug, "no builtin or program matches '%s'", program.c_str());
      if (is_missing_directory) {
        let const directory_message = StringView{"The directory '"} +
                                      resolution_program + "' does not exist";
        throw CommandResolutionErrorWithLocation{resolution_location,
                                                 directory_message.view()};
      }

      let const message =
          "The command '" + resolution_program + "' was not found";
      if (Maybe<String> suggestion = utils::suggest_command(
              program.view(), ArrayList<String>{heap_allocator()},
              &program_resolver))
      {
        let const hint = "Did you mean '" + *suggestion + "'?";
        throw CommandResolutionErrorWithLocationAndDetails{
            resolution_location, message.view(), hint.view()};
      }
      throw CommandResolutionErrorWithLocation{resolution_location,
                                               message.view()};
    }
  } else {
    LOG(Debug, "resolved '%s' to a builtin", program.c_str());
    kind = ResolvedCommand::from_builtin(*resolved_builtin);
  }

  return {location, steal(kind), steal(args), steal(arg_locations)};
}

fn ExecContext::from_resolved(SourceLocation location, ResolvedCommand kind,
                              ArrayList<String> &&args,
                              ArrayList<SourceLocation> &&arg_locations) throws
    -> ExecContext
{
  ASSERT(args.count() > 0);
  return {steal(location), steal(kind), steal(args), steal(arg_locations)};
}

fn ExecContext::make_unresolved(const SourceLocation &location,
                                i32 resolution_status,
                                StringView diagnostic) throws -> ExecContext
{
  let args = ArrayList<String>{heap_allocator()};
  args.push(String{heap_allocator()});
  let arg_locations = ArrayList<SourceLocation>{heap_allocator()};
  arg_locations.push(location);
  let context =
      ExecContext{location, ResolvedCommand::from_unresolved(resolution_status),
                  steal(args), steal(arg_locations)};
  context.m_unresolved_diagnostic = diagnostic;

  return context;
}

fn ExecContext::set_unresolved(i32 resolution_status,
                               StringView diagnostic) throws -> void
{
  m_kind = ResolvedCommand::from_unresolved(resolution_status);
  m_unresolved_diagnostic = diagnostic;
}

} /* namespace koshka */
