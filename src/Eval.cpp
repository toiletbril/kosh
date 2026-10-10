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

#include "CLI.hpp"
#include "CLIColors.hpp"
#include "Completion.hpp"
#include "Errors.hpp"
#include "Expressions.hpp"
#include "Koshconf.hpp"
#include "Koshkit.hpp"
#include "Lexer.hpp"
#include "Parser.hpp"
#include "Platform.hpp"
#include "Toiletline.hpp"
#include "Utils.hpp"
#include "base/Arena.hpp"
#include "base/Common.hpp"
#include "base/Debug.hpp"
#include "base/Path.hpp"
#include "base/StaticStringMap.hpp"
#include "base/Trace.hpp"

namespace koshka {

static pure fn is_prompt_special_variable(StringView name) wontthrow -> bool
{
  return name == "PROMPT_COMMAND" ||
         (name.length == 3 && name[0] == 'P' && name[1] == 'S' &&
          name[2] >= '0' && name[2] <= '4');
}

EvalContext::EvalContext(startup_options options, String shell_name,
                         ArrayList<String> positional_params)
    : EvalContextState(steal(positional_params), options.is_interactive,
                       steal(shell_name))
{
  runtime_state().set_no_glob(options.should_disable_path_expansion);
  runtime_state().set_echo(options.should_echo);
  runtime_state().set_echo_expanded(options.should_echo_expanded);
  runtime_state().set_error_exit(options.should_error_exit);
  runtime_state().set_option(shell_option_id::Emacs, options.is_interactive);
  if (options.is_interactive)
    runtime_state().set_option(shell_option_id::Vi, false);
  runtime_state().set_option(shell_option_id::History, options.is_interactive);
  runtime_state().set_option(shell_option_id::Histexpand,
                             options.is_interactive);

  dynamic_runtime_store().shell_start_time() =
      static_cast<i64>(std::time(nullptr));
  trap_store().startup_ignored_signals() = os::get_entry_ignored_signals();

  os::for_each_environment_name(this, [](opaque *context, StringView name) {
    static_cast<EvalContext *>(context)->mark_exported(name);
  });
}

DiagnosticsStore::~DiagnosticsStore() { reset_runtime_highlight_cache(); }

fn DiagnosticsStore::reset_runtime_highlight_cache() wontthrow -> void
{
  if (m_runtime_diagnostic_highlight_cache == nullptr) return;

  m_runtime_diagnostic_highlight_cache->~shell_highlight_cache();
  heap_allocator().free_array(m_runtime_diagnostic_highlight_cache, 1);
  m_runtime_diagnostic_highlight_cache = nullptr;
}

fn EvalContext::get_or_create_diagnostic_highlight_cache() throws
    -> completion::shell_highlight_cache *
{
  if (diagnostics_store().diagnostic_highlight_cache() != nullptr)
    return diagnostics_store().diagnostic_highlight_cache();

  if (diagnostics_store().runtime_diagnostic_highlight_cache() == nullptr) {
    let const cache =
        heap_allocator().alloc_array<completion::shell_highlight_cache>(1);
    if (cache == nullptr) throw std::bad_alloc{};
    try {
      diagnostics_store().runtime_diagnostic_highlight_cache() =
          new (cache) completion::shell_highlight_cache{};
    } catch (...) {
      heap_allocator().free_array(cache, 1);
      throw;
    }
  }

  return diagnostics_store().runtime_diagnostic_highlight_cache();
}

fn EvalContext::reset_runtime_diagnostic_highlight_cache() wontthrow -> void
{
  diagnostics_store().reset_runtime_highlight_cache();
}

fn RuntimeState::capture(const EvalContext &context) wontthrow -> RuntimeState
{
  return context.runtime_state();
}

fn RuntimeState::restore(EvalContext &context) const wontthrow -> void
{
  context.runtime_state() = *this;
  publish_path_compatibility();
}

RuntimeStateScope::RuntimeStateScope(EvalContext &context)
    : m_context(context), m_saved(RuntimeState::capture(context))
{}

RuntimeStateScope::~RuntimeStateScope() { m_saved.restore(m_context); }

DefinitionStateScope::DefinitionStateScope(EvalContext &context,
                                           const definition_state &state,
                                           definition_state_exit exit,
                                           bool should_enter) wontthrow
    : m_context(context),
      m_exit(exit)
{
  if (should_enter) m_saved = context.enter_definition_state(state);
}

DefinitionStateScope::~DefinitionStateScope()
{
  if (m_saved.has_value()) m_context.leave_definition_state(*m_saved, m_exit);
}

fn EvalContext::end_command() wontthrow -> void
{
  let const used = arena_store().parse_arena() != nullptr
                       ? arena_store().parse_arena()->bytes_used()
                       : 0;
  evaluation_metrics_store().end_command(used);
}

fn EvalContext::record_history_event(StringView command) throws -> bool
{
  if (history_recorder().has_transaction()) {
    history_recorder().append_to_transaction(command);
    return true;
  }

  toiletline::set_history_limit(
      variable_store().history_limit("KOSH_HISTORY_SIZE", 4096));
  return toiletline::append_history_event(command).has_value();
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
  let is_glob_ignore_name = false;

  switch (first_byte) {
  case 'G': is_glob_ignore_name = name == "GLOBIGNORE"; break;
  case 'I':
    is_field_separator_name = name == "IFS";
    is_ignoreeof_name = name == "IGNOREEOF";
    break;
  case 'P':
  case 'p': is_path_name = utils::environment_name_is_path(name); break;
  case 'L':
    if (name.starts_with("LC_") || name == "LANG") {
      variable_store().set_locale_scalar_possible();
    }
    break;
  default: break;
  }
  let const is_pipestatus_name = first_byte == 'P' && name == "PIPESTATUS";

  if (environment_store().confined_write_depth() > 0) rarely
    {
      let const previous = variable_store().shell_variables().find(name);
      let saved = Maybe<String>{};
      if (previous.has_value()) saved = String{previous->view()};
      let const saved_definition = special_variable_definition_location(name);

      environment_store().confined_write_log().push(
          environment_undo_entry{String{name}, steal(saved), saved_definition});
    }

  if (is_field_separator_name) variable_store().set_field_separators(value);
  if (write_dynamic_variable(name, value)) return;

  if (is_path_name) program_resolver().assign_path(String{value});
  if (is_ignoreeof_name) {
    runtime_state().set_option(shell_option_id::Ignoreeof, true);
  }

  variable_store().shell_variables().set(name, value);
  if (is_pipestatus_name) variable_store().set_pipestatus_scalar_possible(true);
  if (is_glob_ignore_name) {
    runtime_state().set_glob_ignore_assigned(true);
    if (!value.is_empty()) set_shopt_option("dotglob", true);
  }
  if (is_prompt_special_variable(name))
    variable_store().special_variable_definition_locations().set(
        name, source_store().current_location());
  if (is_exported(name)) {
    if (execution_store().subshell_depth() > 0)
      environment_store().environment_undo_log().push(environment_undo_entry{
          String{name}, os::get_environment_variable(name), None});
    os::set_environment_variable(name, value);
  }
}

fn EvalContext::restore_temporary_shell_variable(
    StringView name, const Maybe<String> &previous_value,
    Maybe<SourceLocation> previous_definition_location) throws -> void
{
  if (previous_value.has_value()) {
    variable_store().shell_variables().set(name, previous_value->view());
    variable_store().set_pipestatus_scalar_possible(true);
  } else
    variable_store().shell_variables().erase(name);
  if (is_prompt_special_variable(name)) {
    if (previous_definition_location.has_value())
      variable_store().special_variable_definition_locations().set(
          name, *previous_definition_location);
    else
      variable_store().special_variable_definition_locations().erase(name);
  }
}

fn EvalContext::begin_confined_variable_writes() wontthrow -> usize
{
  LOG(Debug, "confining variable writes above mark %zu",
      environment_store().confined_write_log().count());

  if (environment_store().confined_write_depth() == 0) {
    environment_store().confined_clock() = dynamic_runtime_store().get_clock();
    environment_store().was_confined_ignoreeof_enabled() =
        runtime_state().option_is_enabled(shell_option_id::Ignoreeof);
  }

  environment_store().confined_write_depth()++;
  return environment_store().confined_write_log().count();
}

fn EvalContext::rollback_confined_variable_writes(usize mark) wontthrow -> void
{
  ASSERT(environment_store().confined_write_depth() > 0);
  environment_store().confined_write_depth()--;
  LOG(Debug, "rewinding %zu confined variable writes",
      environment_store().confined_write_log().count() - mark);

  while (environment_store().confined_write_log().count() > mark) {
    try {
      let const &entry = environment_store().confined_write_log().back();
      let const name = entry.name.view();
      restore_temporary_shell_variable(
          name, entry.previous_value,
          entry.previous_special_definition_location);
      let const restored = entry.previous_value.has_value()
                               ? entry.previous_value->view()
                               : StringView{};

      if (name == "IFS") {
        variable_store().set_field_separators(
            entry.previous_value.has_value() ? restored : " \t\n");
      }
      if (utils::environment_name_is_path(name)) {
        program_resolver().assign_path(entry.previous_value.has_value()
                                           ? Maybe<String>{String{restored}}
                                           : Maybe<String>{});
      }
      if (is_exported(name)) {
        if (entry.previous_value.has_value())
          os::set_environment_variable(name, restored);
        else
          os::unset_environment_variable(name);
      }
    } catch (...) {
      LOG(Info, "a confined variable write could not be rewound");
    }

    environment_store().confined_write_log().pop_back();
  }

  if (environment_store().confined_write_depth() == 0) {
    dynamic_runtime_store().set_clock(environment_store().confined_clock());
    runtime_state().set_option(
        shell_option_id::Ignoreeof,
        environment_store().was_confined_ignoreeof_enabled());
  }
}

fn EvalContext::guard_restricted_path(StringView path,
                                      const SourceLocation &location,
                                      restricted_path_use use) const throws
    -> void
{
  if (!runtime_state().option_is_enabled(shell_option_id::Restricted)) return;
  if (use != restricted_path_use::History &&
      !os::has_directory_separator(path))
  {
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
        location, "History file operands are forbidden in a restricted shell"};
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
  let const attribute_bits = variable_store().attributes().get_bits(name);
  if ((attribute_bits & static_cast<u8>(variable_attribute::Nameref)) != 0)
    rarely
    {
      let const target = resolve_nameref_for_write(name);
      if (target.view() == name) {
        try {
          if (value == name)
            bind_self_nameref(name, scope_store().has_current_local(name));
          else
            bind_nameref(name, value);
        } catch (ErrorBase &error) {
          mark_expansion_error(error, expansion_error_reach::LineOrPosixScript);
          throw;
        }
        return;
      }

      if (let const bracket = target.view().find_character('[');
          bracket.has_value())
      {
        assign_array_element(target.view().substring_of_length(0, *bracket),
                             target.view().substring_of_length(
                                 *bracket + 1, target.count() - *bracket - 2),
                             value, assignment_update_mode::Replace);
        return;
      }

      set_shell_variable(target.view(), value);
      return;
    }

  if (is_implicitly_readonly(name) ||
      (attribute_bits & static_cast<u8>(variable_attribute::Readonly)) != 0)
  {
    throw Error{"Unable to assign '" + name + "' because it is read only"};
  }

  if (is_write_discarded_dynamic_variable(name)) return;

  if (name == BASH_ALIASES_VARIABLE &&
      is_bash_special_array_active(bash_special_array_id::Aliases))
  {
    scope_store().set_alias("0", value);
    return;
  }
  if (is_bash_directory_stack_special(name)) return;

  if (!variable_store().shell_variables().find(name).has_value() &&
      ((variable_store().indexed_arrays().count() != 0 &&
        variable_store().indexed_arrays().find(name).has_value()) ||
       variable_store().associative_arrays().has(name)))
  {
    assign_array_element(name, "0", value, assignment_update_mode::Replace);
    return;
  }

  if (is_implicitly_integer(name) ||
      (attribute_bits & static_cast<u8>(variable_attribute::Integer)) != 0)
    rarely
    {
      let result = String{scratch_allocator(), "0"};
      if (value.length != 0) {
        try {
          result = evaluate_arithmetic_text(value);
        } catch (ErrorBase &error) {
          let const was_marked =
              error.is_line_discarding() || error.is_script_fatal();
          mark_expansion_error(error, expansion_error_reach::Line);
          if (!was_marked && error.is_line_discarding()) {
            error.set_top_level_line_discarding();
          }

          throw;
        }
      }
      assign_variable(name, result.view());
      return;
    }

  if ((attribute_bits & (static_cast<u8>(variable_attribute::Lowercase) |
                         static_cast<u8>(variable_attribute::Uppercase))) != 0)
    rarely
    {
      let adjusted = String{scratch_allocator(), value};
      variable_store().attributes().apply_case(name, adjusted);
      assign_variable(name, adjusted.view());
      return;
    }

  assign_variable(name, value);
}

fn EvalContext::seed_shell_identity_variables(
    shell_identity_mode identity_mode) throws -> void
{
  switch (identity_mode) {
  case shell_identity_mode::Bash: {
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
    set_shell_variable("BASH", execution_store().get_shell_executable_path());
    if (!get_variable_value("COMP_WORDBREAKS").has_value())
      set_shell_variable("COMP_WORDBREAKS", StringView{" \t\n\"'><=;|&(:"});
    return;
  }
  case shell_identity_mode::Native: break;
  }
  LOG(Info, "clearing the bash identity variables for a non-bash mood");
  if (variable_store().shell_variables().find("BASH_VERSION").has_value() ||
      os::has_environment_variable("BASH_VERSION"))
  {
    force_unset_shell_variable("BASH_VERSION");
  }
  if (variable_store().shell_variables().find("BASH").has_value() ||
      os::has_environment_variable("BASH"))
  {
    force_unset_shell_variable("BASH");
  }
}

fn EvalContext::materialize_kosh_identity() const throws -> Maybe<String>
{
  let const identity =
      utils::kosh_identity(execution_store().get_shell_executable_path());
  if (identity.has_value()) return String{heap_allocator(), *identity};
  return None;
}

fn EvalContext::prepare_child_environment() const throws -> void
{
  unused(materialize_kosh_identity());
  if (!is_exported(KOSHCONF_VARIABLE_NAME)) return;

  os::set_environment_variable(KOSHCONF_VARIABLE_NAME,
                               encode_koshconf_blob(*this).view());
}

fn EvalContext::unset_shell_variable(StringView name) throws -> void
{
  if (variable_store().attributes().is_nameref(name) &&
      !unbind_circular_nameref(name))
    rarely
    {
      let const target = resolve_nameref_for_write(name);
      if (let const bracket = target.view().find_character('[');
          bracket.has_value())
      {
        unset_array_element(target.view().substring_of_length(0, *bracket),
                            target.view().substring_of_length(
                                *bracket + 1, target.count() - *bracket - 2));
        return;
      }
      if (target.view() != name) {
        unset_shell_variable(target.view());
        return;
      }
    }

  if (is_readonly(name))
    throw Error{"Unable to unset '" + name + "' because it is read only"};

  if (is_bash_argument_array(name))
    throw Error{String{name} + ": cannot unset"};

  if (!runtime_state().is_shopt_enabled(shopt_option_id::LocalvarUnset) &&
      peel_caller_local_binding(name))
  {
    return;
  }

  let const should_disable_bash_aliases =
      name == BASH_ALIASES_VARIABLE &&
      is_bash_special_array_active(bash_special_array_id::Aliases);
  let const should_disable_bash_directory_stack =
      is_bash_directory_stack_special(name);
  force_unset_shell_variable(name);
  variable_store().indexed_arrays().erase(name);
  clear_sparse_array(name);
  clear_associative_array(name);
  if (should_disable_bash_aliases)
    disable_bash_special_array(bash_special_array_id::Aliases);
  if (should_disable_bash_directory_stack)
    disable_bash_special_array(bash_special_array_id::DirectoryStack);
  variable_store().attributes().erase(name);
}

fn EvalContext::disable_ignoreeof() throws -> void
{
  force_unset_shell_variable("IGNOREEOF");
  variable_store().attributes().erase("IGNOREEOF");
}

fn EvalContext::peel_caller_local_binding(StringView name) throws -> bool
{
  if (scope_store().local_scope_depth() < 2) return false;
  if (scope_store().has_current_local(name)) return false;

  for (usize frame_index = scope_store().local_scope_depth() - 1;
       frame_index-- > 0;)
  {
    ArrayList<local_binding> &frame = scope_store().local_scopes()[frame_index];
    for (usize i = frame.count(); i-- > 0;) {
      let &binding = frame[i];
      if (binding.name.view() != name || binding.is_self_reference) continue;
      LOG(Debug, "peeling the local binding of '%.*s' from caller frame %zu",
          static_cast<int>(name.length), name.data, frame_index);

      restore_local_binding(binding);

      frame.remove(i);
      return true;
    }
  }
  return false;
}

fn EvalContext::assign_caller_binding_of_circular_nameref(
    StringView name, StringView value) throws -> bool
{
  if (scope_store().local_scope_depth() == 0) return false;
  if (!variable_store().attributes().is_nameref(name)) return false;
  if (!is_circular_nameref(name)) return false;

  ArrayList<local_binding> &frame = scope_store().current_local_scope();
  for (usize i = frame.count(); i-- > 0;) {
    let &binding = frame[i];
    if (binding.name.view() != name || binding.is_self_reference) continue;

    let const nameref_bit = static_cast<u8>(variable_attribute::Nameref);
    if ((binding.previous_attributes & nameref_bit) != 0 ||
        binding.previous_indexed_array.has_value() ||
        binding.previous_was_associative)
    {
      return false;
    }

    warn_circular_nameref(name);
    binding.previous_value = String{heap_allocator(), value};

    return true;
  }

  return false;
}

fn EvalContext::assign_global_beneath_locals(StringView name, StringView value,
                                              bool should_append) throws -> bool
{
  for (usize frame_index = 0; frame_index < scope_store().local_scope_depth();
       frame_index++)
  {
    ArrayList<local_binding> &frame = scope_store().local_scopes()[frame_index];
    for (let &binding : frame) {
      if (binding.name.view() != name || binding.is_self_reference) continue;

      if (binding.previous_attributes != 0 ||
          binding.previous_indexed_array.has_value() ||
          binding.previous_was_associative)
      {
        return false;
      }

      let next_value = String{heap_allocator()};
      if (should_append && binding.previous_value.has_value())
        next_value.append(binding.previous_value->view());
      next_value.append(value);
      binding.previous_value = steal(next_value);

      return true;
    }
  }

  return false;
}

fn EvalContext::restore_local_binding(local_binding &binding) throws -> void
{
  if (binding.previous_value.has_value())
    assign_variable(binding.name, *binding.previous_value);
  else
    force_unset_shell_variable(binding.name);
  if (is_prompt_special_variable(binding.name.view())) {
    if (binding.previous_special_definition_location.has_value())
      variable_store().special_variable_definition_locations().set(
          binding.name.view(), *binding.previous_special_definition_location);
    else
      variable_store().special_variable_definition_locations().erase(
          binding.name.view());
  }
  if (binding.previous_indexed_array.has_value())
    variable_store().indexed_arrays().set(
        binding.name.view(), steal(*binding.previous_indexed_array));
  else
    variable_store().indexed_arrays().erase(binding.name.view());
  let const was_restricted =
      runtime_state().option_is_enabled(shell_option_id::Restricted);
  runtime_state().set_option(shell_option_id::Restricted, false);
  variable_store().attributes().erase(binding.name.view());
  defer
  {
    runtime_state().set_option(shell_option_id::Restricted, was_restricted);
    if (binding.previous_attributes != 0)
      variable_store().attributes().set_bits(binding.name.view(),
                                             binding.previous_attributes);
    else
      variable_store().attributes().erase(binding.name.view());
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
  let resolved_name = Maybe<String>{};
  if (variable_store().attributes().is_nameref(name)) rarely
    {
      resolved_name = resolve_nameref_whole_variable_for_write(name, false);
      name = resolved_name->view();
    }
  if (is_readonly(name))
    throw Error{"Unable to assign '" + name + "' because it is read only"};
  if (is_write_discarded_dynamic_variable(name)) return;
  if (is_bash_directory_stack_special(name)) {
    for (usize index = 0; index < values.count(); index++)
      set_bash_directory_stack_element(index, values[index].view());
    return;
  }
  variable_store().attributes().unmark_declared(name);
  if (variable_store().attributes().has_case(name))
    rarely for (let &value : values) variable_store().attributes().apply_case(
        name, value);
  if (variable_store().associative_arrays().has(name)) {
    throw Error{"Unable to convert the associative array '" + name +
                "' to an indexed array"};
  }
  if (is_integer_variable(name))
    for (let &value : values)
      value = evaluate_arithmetic_text(value.view());
  variable_store().shell_variables().erase(name);
  if (is_exported(name)) {
    record_environment_change(name);
    os::unset_environment_variable(name);
  }
  clear_sparse_array(name);
  variable_store().indexed_arrays().set(name, steal(values));
}

fn EvalContext::publish_pipe_statuses(ArrayList<String> values) throws -> void
{
  if (is_readonly("PIPESTATUS")) {
    if (let current = variable_store().indexed_arrays().find("PIPESTATUS");
        current.has_value())
      *current.value() = steal(values);

    return;
  }

  set_indexed_array("PIPESTATUS", steal(values));
}

fn EvalContext::publish_single_pipe_status(i32 status) throws -> void
{
  static const StringView PIPESTATUS_NAME{"PIPESTATUS", 10};
  static const u64 PIPESTATUS_HASH = hash_bytes(PIPESTATUS_NAME);
  let existing = variable_store().indexed_arrays().find_hashed(PIPESTATUS_NAME,
                                                               PIPESTATUS_HASH);
  if (!existing.has_value() && is_readonly("PIPESTATUS")) return;

  if (existing.has_value() && existing->count() == 1 &&
      (variable_store().sparse_arrays().names().count() == 0 ||
       !variable_store().sparse_arrays().has("PIPESTATUS")))
  {
    if (variable_store().is_pipestatus_scalar_possible()) {
      variable_store().shell_variables().erase("PIPESTATUS");
      variable_store().set_pipestatus_scalar_possible(false);
    }
    char status_text_buffer[32];
    let const status_text = utils::int_to_text_into(status, status_text_buffer,
                                                    sizeof(status_text_buffer));
    if ((*existing.value())[0] != status_text)
      (*existing.value())[0] = String{existing->allocator(), status_text};
    return;
  }

  variable_store().shell_variables().erase("PIPESTATUS");
  clear_sparse_array("PIPESTATUS");
  let &values = variable_store().indexed_arrays().get_or_create(
      "PIPESTATUS", ArrayList<String>{heap_allocator()});
  values.clear();
  values.push(String::from(status, values.allocator()));
}

wontreturn fn throw_script_fatal(StringView message, StringView note) throws
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

cold fn EvalContext::show_runtime_warning_at(
    SourceLocation location, StringView message, StringView note,
    bool should_ignore_disabled) wontthrow -> void
{
  if (runtime_state().is_diagnostics_disabled() && !should_ignore_disabled)
    return;
  let const trace_location = location;
  try {
    let const resolved_source = resolve_render_source(location);
    let const line_offset =
        resolved_source.is_windowed ? resolved_source.line_offset : isize{0};
    location = resolved_source.rebase(location);
    if (resolved_source.text == nullptr ||
        location.position > resolved_source.text->count())
    {
      show_message(WarningWithDetails{message, note}.to_string());
      return;
    }
    let warning = WarningWithLocationAndDetails{location, message, note};
    if (resolved_source.is_windowed) warning.set_line_offset(line_offset);
    show_message(warning.to_string(resolved_source.text->view(), this));
    print_source_backtrace(trace_location);
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
    let const line_offset =
        resolved_source.is_windowed ? resolved_source.line_offset : isize{0};
    location = resolved_source.rebase(location);
    if (resolved_source.text == nullptr ||
        location.position > resolved_source.text->count())
    {
      show_message(Error{message}.to_string());
      return;
    }
    let error = ErrorWithLocation{location, message};
    if (resolved_source.is_windowed) error.set_line_offset(line_offset);
    show_message(error.to_string(resolved_source.text->view(), this));
    print_source_backtrace(trace_location);
  } catch (...) {
    LOG(Debug, "formatting a runtime error failed, the error is swallowed");
  }
}

pure fn EvalContext::locate_variable_reference(StringView name) const wontthrow
    -> SourceLocation
{
  let fallback = source_store().current_location();
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

fn EvalContext::mark_expansion_error(
    ErrorBase &error, expansion_error_reach reach) const wontthrow -> void
{
  if (error.is_script_fatal() || error.is_line_discarding()) {
    return;
  }
  if (expansion_store().is_expanding_here_document()) return;

  if (reach != expansion_error_reach::Line &&
      runtime_state().is_posix_option_on() &&
      !execution_store().shell_is_interactive())
  {
    error.set_script_fatal();
    error.set_line_discarding();
    return;
  }

  if (reach != expansion_error_reach::CommandOrPosixScript &&
      runtime_state().is_bash_compatible())
  {
    error.set_line_discarding();
  }
}

fn EvalContext::report_unset_reference(StringView name) throws -> void
{
  if (runtime_control_store().is_warning_suppressed(
          suppressible_warning::UnsetReference))
    return;

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
  if (runtime_state().error_unset() &&
      (runtime_state().was_error_unset_set_explicitly() || !should_demote) &&
      !runtime_control_store().is_warning_suppressed(
          suppressible_warning::UnsetTestOperand))
  {
    let const message = "Unable to expand '" + String{name} +
                        "' because the parameter is not set";

    let const reference = locate_variable_reference(name);
    if (reference.position == source_store().current_location().position &&
        reference.length == source_store().current_location().length)
    {
      throw_script_fatal(String{message}, empty_expansion_note.view());
    }

    ErrorWithLocationAndDetails error{reference, message,
                                      empty_expansion_note.view()};
    error.set_script_fatal();
    throw steal(error);
  }
  if (execution_store().completion_function_running()) return;
  if (runtime_control_store().is_warning_suppressed(
          suppressible_warning::UnsetTestOperand))
    return;

  if (runtime_state().error_unset() || should_demote) {
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
  if (execution_store().completion_function_running()) return;
  if ((fatal || should_demote) && !runtime_state().is_diagnostics_disabled() &&
      source_store().current_source() != nullptr)
  {
    try {
      let warning = WarningWithLocationAndDetails{location, message, note};
      show_message(
          warning.to_string(source_store().current_source()->view(), this));
      print_source_backtrace(location);
    } catch (...) {
      LOG(Debug, "showing a located warning failed, the error is swallowed");
    }
  }
}

fn EvalContext::force_unset_shell_variable(StringView name) throws -> void
{
  LOG(All, "removing variable '%.*s' from the store and the environment",
      static_cast<int>(name.length), name.data);
  variable_store().shell_variables().erase(name);
  if (is_prompt_special_variable(name))
    variable_store().special_variable_definition_locations().erase(name);
  if (is_exported(name)) {
    record_environment_change(name);
    os::unset_environment_variable(name);
    unmark_exported(name);
  }
  switch (name.is_empty() ? '\0' : name[0]) {
  case 'I':
    if (name == "IFS") variable_store().set_field_separators(" \t\n");
    if (name == "IGNOREEOF")
      runtime_state().set_option(shell_option_id::Ignoreeof, false);
    break;
  case 'G':
    if (name == "GLOBIGNORE") {
      runtime_state().set_glob_ignore_assigned(false);
      set_shopt_option("dotglob", false);
    }
    break;
  case 'P':
  case 'p':
    if (utils::environment_name_is_path(name))
      program_resolver().assign_path(os::get_environment_variable("PATH"));
    break;
  default: break;
  }
}

pure fn EvalContext::special_variable_definition_location(
    StringView name) const wontthrow -> Maybe<SourceLocation>
{
  let const location =
      variable_store().special_variable_definition_locations().find(name);
  if (!location.has_value()) return None;
  return *location.value();
}

fn EvalContext::record_environment_change(StringView name) throws -> void
{
  if (execution_store().subshell_depth() == 0) return;
  environment_store().environment_undo_log().push(environment_undo_entry{
      String{name}, os::get_environment_variable(name), None});
}

static constexpr usize EXPORTED_NAME_FOLD_BYTES = 64;

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

template <typename Value>
static fn store_exported_name(StringMap<Value> &names, StringView key,
                              StringView spelling) throws -> void
{
  if constexpr (os::ENVIRONMENT_IS_CASE_SENSITIVE) {
    unused(spelling);
    names.set(key, Nothing{});
  } else {
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
    store_exported_name(variable_store().exported_names(), name, name);
    return;
  }

  char folded[EXPORTED_NAME_FOLD_BYTES];
  let spill = String{heap_allocator()};
  store_exported_name(variable_store().exported_names(),
                      fold_exported_name(name, folded, spill), name);
}

fn EvalContext::unmark_exported(StringView name) throws -> void
{
  if constexpr (os::ENVIRONMENT_IS_CASE_SENSITIVE) {
    variable_store().exported_names().erase(name);
    return;
  }

  char folded[EXPORTED_NAME_FOLD_BYTES];
  let spill = String{heap_allocator()};
  variable_store().exported_names().erase(
      fold_exported_name(name, folded, spill));
}

enum class analysis_word : u8
{
  Mimicry,
  Warnings,
  NoAnnoying,
  NoDiagnostics,
};

static constexpr static_string_entry<analysis_word> ANALYSIS_WORD_ENTRIES[] = {
    {SSK("mimicry"),        analysis_word::Mimicry      },
    {SSK("warnings"),       analysis_word::Warnings     },
    {SSK("no-annoying"),    analysis_word::NoAnnoying   },
    {SSK("no-diagnostics"), analysis_word::NoDiagnostics},
};
static constexpr StaticStringMap ANALYSIS_WORDS{ANALYSIS_WORD_ENTRIES};

static pure fn analysis_word_name(analysis_word word) wontthrow -> StringView
{
  switch (word) {
  case analysis_word::Mimicry: return StringView{"mimicry"};
  case analysis_word::Warnings: return StringView{"warnings"};
  case analysis_word::NoAnnoying: return StringView{"no-annoying"};
  case analysis_word::NoDiagnostics: return StringView{"no-diagnostics"};
  }

  return StringView{};
}

fn inheritable_analysis_state::decode(StringView text) throws
    -> inheritable_analysis_state
{
  let result = inheritable_analysis_state{};

  text.for_each_ascii_whitespace_word([&](StringView token) throws {
    let const separator = token.find_character('=');
    let const name = separator.has_value()
                         ? token.substring_of_length(0, *separator)
                         : token;
    let const word = ANALYSIS_WORDS.find(name);
    if (!word.has_value()) return;

    switch (*word) {
    case analysis_word::Mimicry:
      if (!separator.has_value()) result.is_mimicry_enabled = true;
      break;
    case analysis_word::NoAnnoying:
      if (!separator.has_value()) result.reporting.is_annoying_disabled = true;
      break;
    case analysis_word::NoDiagnostics:
      if (!separator.has_value())
        result.reporting.is_diagnostics_disabled = true;
      break;
    case analysis_word::Warnings:
      if (separator.has_value() && token.length == *separator + 2 &&
          token[*separator + 1] >= '1' && token[*separator + 1] <= '3')
      {
        result.reporting.warning_level =
            static_cast<u8>(token[*separator + 1] - '0');
      }
      break;
    }
  });

  return result;
}

fn inheritable_analysis_state::from_environment() throws
    -> inheritable_analysis_state
{
  let const text = os::get_environment_variable(ENVIRONMENT_NAME);
  if (!text.has_value()) return inheritable_analysis_state{};

  return decode(text->view());
}

fn inheritable_analysis_state::encode(String &text) const throws -> void
{
  if (is_mimicry_enabled) {
    text += analysis_word_name(analysis_word::Mimicry);
    text += " ";
  }
  if (reporting.warning_level > 0) {
    text += analysis_word_name(analysis_word::Warnings);
    text += "=";
    text += static_cast<char>('0' + reporting.warning_level);
    text += " ";
  }
  if (reporting.is_annoying_disabled) {
    text += analysis_word_name(analysis_word::NoAnnoying);
    text += " ";
  }
  if (reporting.is_diagnostics_disabled) {
    text += analysis_word_name(analysis_word::NoDiagnostics);
    text += " ";
  }

  if (!text.is_empty()) text.pop_back();
}

fn EvalContext::sync_analysis_environment() throws -> void
{
  static constexpr StringView NAME =
      inheritable_analysis_state::ENVIRONMENT_NAME;
  let text = String{scratch_allocator()};
  runtime_state().get_inheritable_analysis_state().encode(text);

  let const current = os::get_environment_variable(NAME);
  if (current.has_value() ? current->view() == text.view() : text.is_empty()) {
    return;
  }

  record_environment_change(NAME);

  if (text.is_empty()) {
    os::unset_environment_variable(NAME);
    return;
  }

  os::set_environment_variable(NAME, text.view());
}

fn EvalContext::unexport_shell_variable(StringView name) throws -> void
{
  let const has_shell_binding =
      variable_store().shell_variables().find(name).has_value() ||
      variable_store().indexed_arrays().find(name).has_value() ||
      variable_store().associative_arrays().has(name) ||
      scope_store().has_current_local(name) ||
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
    return variable_store().exported_names().find(name).has_value();

  char folded[EXPORTED_NAME_FOLD_BYTES];
  let spill = String{heap_allocator()};
  return variable_store()
      .exported_names()
      .find(fold_exported_name(name, folded, spill))
      .has_value();
}

fn EvalContext::sync_exported_after_restore(StringView name,
                                            bool has_value) throws -> void
{
  if (has_value)
    mark_exported(name);
  else
    unmark_exported(name);
}

pure fn EvalContext::is_bash_argument_array(StringView name) const wontthrow
    -> bool
{
  return runtime_state().bash_dynamic_variables_enabled() &&
         (name == BASH_ARGUMENT_COUNT_VARIABLE ||
          name == BASH_ARGUMENT_VALUE_VARIABLE);
}

fn EvalContext::initialize_bash_argument_arrays(
    bool should_include_current_frame) const throws -> void
{
  if (variable_store().bash_arguments().is_active()) return;

  let const context = variable_store().bash_arguments().get_context();
  let values = ArrayList<String>{heap_allocator()};
  let frame_counts = ArrayList<u32>{heap_allocator()};
  if (should_include_current_frame) {
    let const is_source_frame =
        context != nullptr &&
        context->has_flag(BashArgumentFrameFlag::IsSource);
    let const has_source_arguments =
        is_source_frame &&
        context->has_flag(BashArgumentFrameFlag::HasSourceArguments);
    let const uses_source_path = is_source_frame && !has_source_arguments;
    let const argument_count =
        uses_source_path ? usize{1}
                         : variable_store().positional_params().count();
    values.reserve(argument_count);
    frame_counts.reserve(1);
    if (uses_source_path) {
      values.push_managed(context->source_path);
    } else {
      for (let const &argument : variable_store().positional_params())
        values.push_managed(argument.view());
    }
    frame_counts.push(static_cast<u32>(argument_count));
  }

  variable_store().bash_arguments().activate(steal(values),
                                             steal(frame_counts));
}

fn EvalContext::append_current_bash_argument_frame() const throws -> void
{
  let const context = variable_store().bash_arguments().get_context();
  ASSERT(context != nullptr);
  let const is_source_frame =
      context->has_flag(BashArgumentFrameFlag::IsSource);
  let const has_source_arguments =
      context->has_flag(BashArgumentFrameFlag::HasSourceArguments);
  if (is_source_frame && !has_source_arguments) {
    variable_store().bash_arguments().push_frame(context->source_path);
  } else {
    variable_store().bash_arguments().push_frame(
        variable_store().positional_params());
  }
}

fn EvalContext::enter_bash_function_argument_frame(
    BashArgumentFrameContext &frame_context,
    const ArrayList<String> &arguments) throws -> void
{
  let &stack = variable_store().bash_arguments();
  frame_context.previous = stack.get_context();
  frame_context.source_path = {};
  frame_context.flags = 0;

  if (runtime_state().bash_dynamic_variables_enabled() &&
      runtime_state().is_shopt_enabled(shopt_option_id::Extdebug))
  {
    initialize_bash_argument_arrays(true);
    stack.push_frame(arguments);
    frame_context.set_flag(BashArgumentFrameFlag::DidEnter);
  }

  stack.set_context(&frame_context);
}

fn EvalContext::enter_bash_source_argument_frame(
    BashArgumentFrameContext &frame_context, const ArrayList<String> *arguments,
    StringView source_path) throws -> void
{
  let &stack = variable_store().bash_arguments();
  frame_context.previous = stack.get_context();
  frame_context.source_path = source_path;
  frame_context.flags = 0;
  frame_context.set_flag(BashArgumentFrameFlag::IsSource);
  if (arguments != nullptr)
    frame_context.set_flag(BashArgumentFrameFlag::HasSourceArguments);

  if (runtime_state().bash_dynamic_variables_enabled()) {
    if (runtime_state().is_shopt_enabled(shopt_option_id::Extdebug)) {
      initialize_bash_argument_arrays(true);
      if (arguments != nullptr)
        stack.push_frame(*arguments);
      else
        stack.push_frame(source_path);
      frame_context.set_flag(BashArgumentFrameFlag::DidEnter);
    } else if (arguments == nullptr) {
      initialize_bash_argument_arrays(stack.get_context() == nullptr);
      stack.push_frame(source_path);
      frame_context.set_flag(BashArgumentFrameFlag::DidEnter);
    } else if (!stack.is_active()) {
      initialize_bash_argument_arrays(false);
      if (stack.get_context() == nullptr) stack.push_frame(*arguments);
    }
  }

  stack.set_context(&frame_context);
}

fn EvalContext::leave_bash_argument_frame(
    BashArgumentFrameContext &frame_context) wontthrow -> void
{
  let &stack = variable_store().bash_arguments();
  ASSERT(stack.get_context() == &frame_context);
  stack.set_context(frame_context.previous);
  if (!frame_context.has_flag(BashArgumentFrameFlag::DidEnter)) return;

  stack.pop_frame();
}

fn EvalContext::enter_function_scope() throws -> void
{
  if (scope_store().local_scope_depth() == scope_store().local_scopes().count())
    scope_store().local_scopes().push(
        ArrayList<local_binding>{heap_allocator()});
  ASSERT(scope_store()
             .local_scopes()[scope_store().local_scope_depth()]
             .is_empty());
  let &saved_options = scope_store().saved_scope_shell_options();
  while (saved_options.count() <= scope_store().local_scope_depth())
    saved_options.push(None);
  saved_options[scope_store().local_scope_depth()] = None;
  scope_store().local_scope_depth()++;
  LOG(Debug, "entered function scope, local scope depth now %zu",
      scope_store().local_scope_depth());
}

fn EvalContext::leave_function_scope() throws -> void
{
  if (scope_store().local_scope_depth() == 0) return;

  ASSERT(scope_store().local_scope_depth() <=
         scope_store().local_scopes().count());
  let &scope = scope_store().current_local_scope();
  LOG(Debug, "leaving function scope, restoring %zu shadowed locals",
      scope.count());
  for (usize i = scope.count(); i > 0; i--) {
    ASSERT(i - 1 < scope.count());
    if (!scope[i - 1].is_self_reference) restore_local_binding(scope[i - 1]);
  }
  scope.clear();
  let &saved_options =
      scope_store()
          .saved_scope_shell_options()[scope_store().local_scope_depth() - 1];
  if (saved_options.has_value()) {
    let const restricted_mask =
        RuntimeState::option_mask(shell_option_id::Restricted);
    let const current_options = runtime_state().get_shell_options();
    runtime_state().set_shell_options(
        (*saved_options & ~restricted_mask) |
        (current_options & restricted_mask));
    saved_options = None;
  }
  scope_store().local_scope_depth()--;
  constexpr usize RETAINED_LOCAL_SCOPE_COUNT = 16;
  if (scope_store().local_scopes().count() > RETAINED_LOCAL_SCOPE_COUNT &&
      scope_store().local_scopes().count() > scope_store().local_scope_depth())
  {
    scope_store().local_scopes().remove(scope_store().local_scopes().count() -
                                        1);
  }
}

fn EvalContext::push_function_call_name(
    StringView name, const FunctionBodyHandle &body_storage) throws -> void
{
  function_store().call_frames().push(function_call_frame{
      String{heap_allocator(), name},
      body_storage,
      source_store().current_source(), source_store().current_location(),
      false
  });
}

fn EvalContext::pop_function_call_name() wontthrow -> void
{
  function_store().call_frames().pop_back();
}

pure fn EvalContext::script_source_frame_index() const wontthrow -> Maybe<usize>
{
  if (!source_store().is_script_run()) return None;

  for (usize i = 0; i < source_store().source_frames().count(); i++) {
    let const &frame = source_store().source_frames()[i];
    if (!frame.has_bash_source_row()) continue;

    if (frame.source_path.view() == execution_store().get_shell_name())
      return i;

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

  if (source_store().is_script_run() && !script_source_index.has_value()) {
    if (target == 0) return MergedFrame{MergedFrame::Kind::Main, 0};
    emitted_count = 1;
  }

  let const function_count = function_store().call_frames().count();
  usize function_index = 0;
  usize source_index = 0;

  loop
  {
    while (source_index < source_store().source_frames().count() &&
           !source_store().source_frames()[source_index].has_bash_source_row())
    {
      source_index++;
    }

    let const has_source =
        source_index < source_store().source_frames().count();
    let const has_function = function_index < function_count;
    if (!has_source && !has_function) break;

    if (has_source &&
        source_store().source_frames()[source_index].function_call_depth <=
            function_index)
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
  if (function_store().call_frames().is_empty()) return 0;
  return bash_source_frame_count();
}

fn EvalContext::funcname_frame_at(usize index) const wontthrow -> StringView
{
  let const frame = merged_frame_at(index);
  switch (frame.kind) {
  case MergedFrame::Kind::Function:
    return function_store().call_frames()[frame.storage_index].name.view();
  case MergedFrame::Kind::Source: return StringView{"source"};
  case MergedFrame::Kind::Main: break;
  }

  return StringView{"main"};
}

fn EvalContext::line_number_at_location(
    const SourceLocation &location, const String *fallback_source,
    Maybe<usize> fallback_call_depth) const throws -> usize
{
  let const &line_bases = source_store().embedded_sources();
  let site = location;
  let site_source = fallback_source != nullptr
                        ? fallback_source
                        : source_store().current_source();
  let site_depth = fallback_source != nullptr
                       ? fallback_call_depth
                       : Maybe<usize>{function_store().call_frames().count()};
  Maybe<usize> site_depth_floor = None;
  usize preceding_line_count = 0;
  usize search_limit = line_bases.count();
  while (search_limit > 0) {
    usize found = search_limit;
    for (usize index = search_limit; index > 0; index--) {
      let const &base = line_bases[index - 1];
      if (base.body != nullptr && base.body == site_source &&
          (!site_depth.has_value() || base.function_call_depth == *site_depth))
      {
        found = index - 1;
        break;
      }
    }
    if (found == search_limit) break;

    let const &base = line_bases[found];
    preceding_line_count +=
        utils::line_number_at(site_source->view(), site.position) - 1 +
        source_store().preceding_line_count_of(site_source);
    site = base.parent_location;
    site_source = base.parent;
    site_depth = base.function_call_depth;
    site_depth_floor = base.call_depth_floor;
    search_limit = found;
  }

  if (!site_depth_floor.has_value() && fallback_source != nullptr &&
      site_depth.has_value())
  {
    usize frame_limit = 0;
    while (frame_limit < source_store().source_frames().count() &&
           source_store().source_frames()[frame_limit].function_call_depth <=
               *site_depth)
    {
      frame_limit++;
    }
    site_depth_floor = source_depth_floor(frame_limit);
  }

  let const resolved_source =
      site_depth_floor.has_value()
          ? resolve_render_source(site, site_source, *site_depth,
                                  *site_depth_floor)
          : resolve_render_source(site, site_source);
  usize line = 1;
  if (resolved_source.text != nullptr) {
    const usize render_position =
        resolved_source.to_render_position(site.position);
    let const render_line = static_cast<isize>(
        utils::line_number_at(resolved_source.text->view(), render_position));
    line = static_cast<usize>(render_line + resolved_source.line_offset) +
           resolved_source.enclosing_line_count;
  }
  return line + preceding_line_count;
}

fn EvalContext::funcname_line_at(usize index) const throws -> usize
{
  let const frame = merged_frame_at(index);
  switch (frame.kind) {
  case MergedFrame::Kind::Function: {
    let const &call_frame = function_store().call_frames()[frame.storage_index];
    return line_number_at_location(call_frame.location, call_frame.source,
                                   frame.storage_index);
  }
  case MergedFrame::Kind::Source: {
    let const &source = source_store().source_frames()[frame.storage_index];
    return line_number_at_location(source.call_site,
                                   borrowed_frame_source(source),
                                   source.function_call_depth);
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
    let const *info = function_store()
                          .call_frames()[frame.storage_index]
                          .storage.get_definition_info();
    if (info != nullptr) {
      if (let const name = source_name_at(info->source_name_index);
          name.has_value() &&
          source_identity_kind_at(info->source_name_index) ==
              source_identity_kind::File)
      {
        return *name;
      }
    }

    return execution_store().get_shell_name();
  }
  case MergedFrame::Kind::Source:
    return source_store()
        .source_frames()[frame.storage_index]
        .source_path.view();
  case MergedFrame::Kind::Main: break;
  }

  return execution_store().get_shell_name();
}

pure fn EvalContext::bash_source_frame_count(
    Maybe<usize> script_source_index) const wontthrow -> usize
{
  usize frame_count = function_store().call_frames().count();

  for (usize i = 0; i < source_store().source_frames().count(); i++) {
    if (source_store().source_frames()[i].has_bash_source_row()) frame_count++;
  }

  if (source_store().is_script_run() && !script_source_index.has_value())
    frame_count++;

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
    let const context = variable_store().bash_arguments().get_context();
    let const should_include_current_frame =
        context != nullptr ? context->has_flag(BashArgumentFrameFlag::IsSource)
                           : function_store().call_frames().is_empty();
    initialize_bash_argument_arrays(should_include_current_frame);
    ASSERT(variable_store().bash_arguments().is_active());
    return which == DynamicArray::ArgumentCount
               ? variable_store().bash_arguments().frame_counts().count()
               : variable_store().bash_arguments().values().count();
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
    ASSERT(variable_store().bash_arguments().is_active());
    let const &frame_counts = variable_store().bash_arguments().frame_counts();
    ASSERT(index < frame_counts.count());
    let const storage_index = frame_counts.count() - 1 - index;
    return String::from(frame_counts[storage_index], result_allocator);
  }
  case DynamicArray::ArgumentValue: {
    unused(dynamic_array_element_count(which));
    ASSERT(variable_store().bash_arguments().is_active());
    let const &values = variable_store().bash_arguments().values();
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

static pure fn dot_parent_steps(StringView word) wontthrow -> Maybe<usize>
{
  if (word.length < 2) return None;

  for (usize i = 0; i < word.length; i++)
    if (word[i] != '.') return None;

  return word.length - 1;
}

fn ExecContext::make_from(const SourceLocation &location, StringView source,
                          ArrayList<String> &&args,
                          bool are_koshkit_utilities_reachable,
                          bool should_check_hash,
                          ProgramResolver &program_resolver,
                          ArrayList<SourceLocation> &&arg_locations,
                          mimic_mood mood, bool should_autocd) throws
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

  if (should_autocd && args.count() == 1 && !resolved_builtin.has_value() &&
      (!resolved_program_path.has_value() ||
       resolved_program_path->is_directory()) &&
      (!are_koshkit_utilities_reachable ||
       !koshkit::find_util(program.view()).has_value()))
  {
    String directory_operand{heap_allocator()};
    bool should_rewrite_to_cd = false;

    if (let parent_step_count = dot_parent_steps(program.view());
        parent_step_count.has_value())
    {
      let directory_path = Path{"..", heap_allocator()};

      for (usize i = 1; i < *parent_step_count; i++)
        directory_path.append("..");

      if (!directory_path.is_directory()) {
        let const directory_message = StringView{"The directory '"} +
                                      directory_path.view() +
                                      "' does not exist";
        throw CommandResolutionErrorWithLocation{resolution_location,
                                                 directory_message.view()};
      }

      directory_operand = String{directory_path.view()};
      should_rewrite_to_cd = true;
    } else if (Path{program.view()}.is_directory()) {
      directory_operand = steal(args[0]);
      should_rewrite_to_cd = true;
    }

    if (should_rewrite_to_cd) {
      args[0] = String{"cd"};
      args.push(String{"--"});
      args.push(steal(directory_operand));
      arg_locations.push(resolution_location);
      arg_locations.push(resolution_location);
      return {location, ResolvedCommand::from_builtin(Builtin::Kind::Cd),
              steal(args), steal(arg_locations)};
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

fn ExecContext::make_from_resolved(
    SourceLocation location, ResolvedCommand kind, ArrayList<String> &&args,
    ArrayList<SourceLocation> &&arg_locations) throws -> ExecContext
{
  ASSERT(args.count() > 0);
  return {steal(location), steal(kind), steal(args), steal(arg_locations)};
}

fn ExecContext::make_from_unresolved(const SourceLocation &location,
                                     i32 resolution_status,
                                     StringView diagnostic) throws
    -> ExecContext
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
