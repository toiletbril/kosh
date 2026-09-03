#include "Eval.hpp"

#include "Arena.hpp"
#include "Cli.hpp"
#include "Colors.hpp"
#include "Common.hpp"
#include "Completion.hpp"
#include "Debug.hpp"
#include "Errors.hpp"
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

EvalContext::EvalContext(bool should_disable_path_expansion, bool should_echo,
                         bool should_echo_expanded, bool shell_is_interactive,
                         bool should_error_exit, String shell_name,
                         ArrayList<String> positional_params)
    : m_shell_name(steal(shell_name)),
      m_positional_params(steal(positional_params)),
      m_shell_is_interactive(shell_is_interactive)
{
  set_no_glob(should_disable_path_expansion);
  set_echo(should_echo);
  set_echo_expanded(should_echo_expanded);
  set_error_exit(should_error_exit);
  set_emacs_mode(shell_is_interactive);
  set_field_separators(m_field_separators.view());

  m_shell_start_time = static_cast<i64>(std::time(nullptr));

  os::for_each_environment_name(&m_exported_names,
                                [](opaque *context, StringView name) {
                                  static_cast<HashSet *>(context)->add(name);
                                });
}

EvalContext::~EvalContext() { reset_runtime_diagnostic_highlight_cache(); }

fn EvalContext::get_or_create_diagnostic_highlight_cache() throws
    -> completion::shell_highlight_cache *
{
  if (m_diagnostic_highlight_cache != nullptr)
    return m_diagnostic_highlight_cache;

  if (m_runtime_diagnostic_highlight_cache == nullptr) {
    m_runtime_diagnostic_highlight_cache =
        new completion::shell_highlight_cache{};
  }

  return m_runtime_diagnostic_highlight_cache;
}

fn EvalContext::reset_runtime_diagnostic_highlight_cache() wontthrow -> void
{
  delete m_runtime_diagnostic_highlight_cache;
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

  if (AST_ARENA != nullptr) {
    let const used = AST_ARENA->bytes_used();
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

  unused(toiletline::history_append_event(command));
  return true;
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
  if (name == "IFS") set_field_separators(value);
  if (name == "PATH") m_program_resolver.assign_path(String{value});
  m_shell_variables.set(name, value);
  if (m_exported_names.contains(name)) {
    if (m_subshell_depth > 0)
      m_environment_undo_log.push(environment_undo_entry{
          String{name}, os::get_environment_variable(name)});
    os::set_environment_variable(name, value);
  }
}

fn EvalContext::set_field_separators(StringView value) throws -> void
{
  LOG(Debug, "caching %zu field separator bytes", value.length);
  /* The table is built before m_field_separators is touched, since value may
     alias the buffer the assignment below rewrites. */
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

hot pure fn EvalContext::is_field_separator(char c) const wontthrow -> bool
{
  let const byte = static_cast<u8>(c);
  return (m_field_separator_bits[byte >> 6] & (u64{1} << (byte & 63))) != 0;
}

fn EvalContext::guard_restricted_path(StringView path,
                                      const SourceLocation &location,
                                      restricted_path_use use) const throws
    -> void
{
  if (!restricted_enforcement_active() || !os::has_directory_separator(path))
    return;

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

  if (is_integer_variable(name)) [[unlikely]] {
    let const result = value.length == 0 ? String{scratch_allocator(), "0"}
                                         : evaluate_arithmetic_text(value);
    assign_variable(name, result.view());
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
  force_unset_shell_variable("BASH_VERSION");
  force_unset_shell_variable("BASH");
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

  if (peel_caller_local_binding(name)) return;

  force_unset_shell_variable(name);
  m_indexed_arrays.erase(name);
  clear_sparse_array(name);
  m_integer_names.remove(name);
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
  if (binding.previous_indexed_array.has_value())
    m_indexed_arrays.set(binding.name.view(),
                         steal(*binding.previous_indexed_array));
  else
    m_indexed_arrays.erase(binding.name.view());
  let const was_restricted = restricted_enforcement_active();
  m_runtime.set_option(shell_option_id::Restricted, false);
  m_readonly_names.remove(binding.name.view());
  defer
  {
    m_runtime.set_option(shell_option_id::Restricted, was_restricted);
    if (binding.previous_was_readonly)
      m_readonly_names.add(binding.name.view());
    else
      m_readonly_names.remove(binding.name.view());
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
  if (binding.previous_was_integer)
    m_integer_names.add(binding.name.view());
  else
    m_integer_names.remove(binding.name.view());

  if (binding.previous_was_exported) {
    m_exported_names.add(binding.name.view());
    if (binding.previous_value.has_value())
      os::set_environment_variable(binding.name, *binding.previous_value);
  } else if (is_exported(binding.name)) {
    m_exported_names.remove(binding.name.view());
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
  m_shell_variables.erase(name);
  clear_sparse_array(name);
  m_indexed_arrays.set(name, steal(values));
}

fn EvalContext::publish_single_pipe_status(i32 status) throws -> void
{
  if (is_readonly("PIPESTATUS"))
    throw Error{"Unable to assign 'PIPESTATUS' because it is read only"};

  if (let *values = m_indexed_arrays.find("PIPESTATUS");
      values != nullptr && values->count() == 1 &&
      !m_sparse_array_names.contains("PIPESTATUS"))
  {
    m_shell_variables.erase("PIPESTATUS");
    char status_text_buffer[32];
    let const status_text = utils::int_to_text_into(status, status_text_buffer,
                                                    sizeof(status_text_buffer));
    if ((*values)[0] != status_text)
      (*values)[0] = String{values->allocator(), status_text};
    return;
  }

  m_shell_variables.erase("PIPESTATUS");
  clear_sparse_array("PIPESTATUS");
  let &values = m_indexed_arrays.get_or_create(
      "PIPESTATUS", ArrayList<String>{heap_allocator()});
  values.clear();
  values.push(String::from(status, values.allocator()));
}

fn EvalContext::append_indexed_array(StringView name,
                                     ArrayList<String> values) throws -> void
{
  if (let *existing = m_indexed_arrays.find(name); existing != nullptr) {
    LOG(All, "appending %zu elements to the existing array '%.*s'",
        values.count(), static_cast<int>(name.length), name.data);
    if (is_readonly(name))
      throw Error{"Unable to assign '" + name + "' because it is read only"};
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
    throw error;
  }

  ErrorWithDetails error{message, note};
  error.set_script_fatal();
  throw error;
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
    usize line_offset = 0;
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
    usize line_offset = 0;
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
    throw error;
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
  record_environment_change(name);
  os::unset_environment_variable(name);
  unmark_exported(name);
  if (name == "IFS") set_field_separators(" \t\n");
  if (name == "PATH")
    m_program_resolver.assign_path(os::get_environment_variable("PATH"));
}

fn EvalContext::record_environment_change(StringView name) throws -> void
{
  if (m_subshell_depth == 0) return;
  m_environment_undo_log.push(
      environment_undo_entry{String{name}, os::get_environment_variable(name)});
}

fn EvalContext::mark_exported(StringView name) throws -> void
{
  LOG(All, "marking '%.*s' as exported", static_cast<int>(name.length),
      name.data);
  m_exported_names.add(name);
}

fn EvalContext::unmark_exported(StringView name) throws -> void
{
  m_exported_names.remove(name);
}

fn EvalContext::unexport_shell_variable(StringView name) throws -> void
{
  let const has_shell_binding = m_shell_variables.find(name) != nullptr ||
                                m_indexed_arrays.find(name) != nullptr ||
                                m_associative_names.contains(name) ||
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

pure fn EvalContext::is_exported(StringView name) const wontthrow -> bool
{
  return m_exported_names.contains(name);
}

fn EvalContext::sync_exported_after_restore(StringView name,
                                            bool has_value) throws -> void
{
  if (has_value)
    m_exported_names.add(name);
  else
    m_exported_names.remove(name);
}

pure fn EvalContext::positional_params() const wontthrow
    -> const ArrayList<String> &
{
  return m_positional_params;
}

fn EvalContext::set_positional_params(ArrayList<String> params) wontthrow
    -> void
{
  m_positional_params = steal(params);
}

fn EvalContext::directory_stack() wontthrow -> ArrayList<String> &
{
  return m_directory_stack;
}

fn EvalContext::take_positional_params() wontthrow -> ArrayList<String>
{
  return steal(m_positional_params);
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
  m_function_call_names.push(steal(owned_name));
  m_function_call_storages.push(body_storage);
  m_function_call_locations.push(m_current_location);
}

fn EvalContext::pop_function_call_name() wontthrow -> void
{
  if (!m_function_call_names.is_empty()) {
    m_function_call_names.remove(m_function_call_names.count() - 1);
    m_function_call_storages.remove(m_function_call_storages.count() - 1);
    m_function_call_locations.remove(m_function_call_locations.count() - 1);
  }
}

fn EvalContext::funcname_frame_count() const wontthrow -> usize
{
  if (m_function_call_names.is_empty()) return 0;
  return m_function_call_names.count() + m_sourced_file_frames +
         (m_is_script_run ? 1 : 0);
}

fn EvalContext::funcname_frame_at(usize index) const wontthrow -> StringView
{
  let const call_count = m_function_call_names.count();
  if (index < call_count)
    return m_function_call_names[call_count - 1 - index].view();
  if (index < call_count + m_sourced_file_frames) return StringView{"source"};
  return StringView{"main"};
}

fn EvalContext::line_number_at_location(
    const SourceLocation &location) const throws -> usize
{
  let const resolved_source = resolve_render_source(location);
  usize line = 1;
  if (resolved_source.text != nullptr) {
    const usize render_position =
        resolved_source.to_render_position(location.position);
    line =
        utils::line_number_at(resolved_source.text->view(), render_position) +
        (resolved_source.is_windowed ? resolved_source.line_offset : 0);
  }
  return line;
}

fn EvalContext::funcname_line_at(usize index) const throws -> usize
{
  /* A frame whose defining file was sourced and freed can misnumber, the
     innermost frame and a single-source script are exact. */
  let const call_count = m_function_call_names.count();
  if (index < call_count)
    return line_number_at_location(
        m_function_call_locations[call_count - 1 - index]);
  return 0;
}

pure fn EvalContext::funcname_source_at(usize index) const wontthrow
    -> StringView
{
  if (!m_source_frames.is_empty()) {
    let const source_index = m_source_frames.count() - 1;
    if (index <= source_index)
      return m_source_frames[source_index - index].source_path.view();
  }
  if (index < m_function_call_names.count()) {
    let const call_count = m_function_call_names.count();
    let const storage_index = call_count - 1 - index;
    let const *info =
        m_function_call_storages[storage_index].get_definition_info();
    if (info != nullptr) {
      if (let const name = source_name_at(info->source_name_index);
          name.has_value())
      {
        return *name;
      }
    }
  }
  return StringView{};
}

pure fn EvalContext::bash_source_frame_at(usize index) const wontthrow
    -> StringView
{
  if (index < m_function_call_names.count()) {
    let const call_count = m_function_call_names.count();
    let const storage_index = call_count - 1 - index;
    let const *info =
        m_function_call_storages[storage_index].get_definition_info();
    if (info != nullptr) {
      if (let const name = source_name_at(info->source_name_index);
          name.has_value())
      {
        return *name;
      }
    }

    return m_shell_name.view();
  }

  usize frame_index = m_function_call_names.count();
  let outermost_source_path = StringView{};
  for (usize i = m_source_frames.count(); i > 0; i--) {
    let const &path = m_source_frames[i - 1].source_path;
    if (path.is_empty()) continue;

    if (frame_index == index) return path.view();
    frame_index++;
    outermost_source_path = path.view();
  }

  /* The script closes the stack unless it is already the outermost source. */
  if (m_is_script_run && index == frame_index &&
      outermost_source_path != m_shell_name.view())
  {
    return m_shell_name.view();
  }

  return StringView{};
}

pure fn EvalContext::bash_source_frame_count() const wontthrow -> usize
{
  usize frame_count = m_function_call_names.count();

  let outermost_source_path = StringView{};
  for (usize i = m_source_frames.count(); i > 0; i--) {
    let const &path = m_source_frames[i - 1].source_path;
    if (path.is_empty()) continue;

    frame_count++;
    outermost_source_path = path.view();
  }

  if (m_is_script_run && outermost_source_path != m_shell_name.view())
    frame_count++;

  return frame_count;
}

fn EvalContext::call_stack_frame_count(CallStackVariable which) const wontthrow
    -> usize
{
  if (which == CallStackVariable::SourcePath) return bash_source_frame_count();

  return funcname_frame_count();
}

fn EvalContext::call_stack_frame_text(CallStackVariable which, usize index,
                                      Allocator result_allocator) const throws
    -> String
{
  switch (which) {
  case CallStackVariable::FunctionName:
    return String{result_allocator, funcname_frame_at(index)};
  case CallStackVariable::LineNumber:
    return String::from(funcname_line_at(index), result_allocator);
  case CallStackVariable::SourcePath:
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
    definition.append(StringView{"='", 2});
    definition.append(value);
    definition.push('\'');
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
    os::close_fd(*in_fd);
    in_fd.reset();
  }
  if (out_fd.has_value()) {
    os::close_fd(*out_fd);
    out_fd.reset();
  }
  if (err_fd.has_value()) {
    os::close_fd(*err_fd);
    err_fd.reset();
  }
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

    /* let is a bash extension absent from POSIX sh, the sh mood reports it not
       found the way dash does. */
    if (resolved_builtin == Builtin::Kind::Let && mood == mimic_mood::Posix) {
      resolved_builtin = None;
    }

    if (!resolved_builtin.has_value()) {
      let program_search_paths = program_resolver.search(
          program.view(), ProgramResolver::SearchMode::First,
          ProgramResolver::Requirement::Execution,
          ProgramResolver::CachePolicy::Remember);
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
                                i32 resolution_status) throws -> ExecContext
{
  let args = ArrayList<String>{heap_allocator()};
  args.push(String{heap_allocator()});
  let arg_locations = ArrayList<SourceLocation>{heap_allocator()};
  arg_locations.push(location);
  return {location, ResolvedCommand::from_unresolved(resolution_status),
          steal(args), steal(arg_locations)};
}

} /* namespace koshka */
