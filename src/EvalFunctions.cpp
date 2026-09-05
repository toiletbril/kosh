/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements EvalContext storage for shell functions, traps,
 * readonly and numeric or case-converting variable attributes, and function
 * and variable inventories. These tables share evaluator scope and snapshot
 * lifetime, while scalar and array values remain in EvalVariables.cpp and
 * EvalArrays.cpp.
 */

#include "Common.hpp"
#include "Debug.hpp"
#include "Eval.hpp"
#include "Expressions.hpp"
#include "Platform.hpp"
#include "Trace.hpp"
#include "Utils.hpp"

namespace koshka {

namespace {

constexpr const char *RESTRICTED_READONLY_NAMES[] = {
    "SHELL", "PATH", "HISTFILE", "ENV", "BASH_ENV"};

}

fn EvalContext::register_function(StringView name,
                                  const FunctionBodyHandle &body_storage,
                                  StringView definition_text,
                                  usize body_start_position,
                                  SourceLocation definition_location) throws
    -> void
{
  ASSERT(body_storage.has_value());
  ASSERT(body_storage.get_body() != nullptr);

  let info = function_definition_info{};
  info.body_start_position = body_start_position;
  info.header_length = name.length + StringView{" () \n"}.length;
  if (m_current_source != nullptr && !definition_text.is_empty()) {
    let const body_line =
        utils::line_number_at(m_current_source->view(), body_start_position);
    info.line_offset = body_line > 2 ? body_line - 2 : 0;
  }
  info.source_name_index = definition_location.source_name_index;
  info.defining_runtime = RuntimeState::capture(*this);
  body_storage.set_definition(definition_text, info);

  LOG(Info, "registering function '%.*s' with a %zu byte definition",
      static_cast<int>(name.length), name.data, definition_text.length);
  m_functions.set(name, body_storage);
}

fn EvalContext::function_definition_info_of(StringView name) const wontthrow
    -> const function_definition_info *
{
  let const *storage = m_functions.find(name);
  return storage != nullptr ? storage->get_definition_info() : nullptr;
}

pure fn EvalContext::resolve_render_source(
    const SourceLocation &location) const wontthrow -> resolved_render_source
{
  let resolved_source = resolved_render_source{};
  resolved_source.text = m_current_source;

  if (m_function_call_names.is_empty()) return resolved_source;
  let const *storage = m_function_call_storages.is_empty()
                           ? nullptr
                           : &m_function_call_storages.back();
  let const *info =
      storage != nullptr ? storage->get_definition_info() : nullptr;
  if (info == nullptr) return resolved_source;
  let const *copy = storage->get_source();
  if (copy == nullptr || copy->count() <= info->header_length) {
    return resolved_source;
  }
  let const body_length = copy->count() - info->header_length;
  if (location.position < info->body_start_position ||
      location.position >= info->body_start_position + body_length)
  {
    return resolved_source;
  }

  resolved_source.text = copy;
  resolved_source.is_windowed = true;
  resolved_source.body_start_position = info->body_start_position;
  resolved_source.header_length = info->header_length;
  resolved_source.line_offset = info->line_offset;
  resolved_source.source_name_index = info->source_name_index;
  return resolved_source;
}

fn EvalContext::find_function_source(StringView name) const wontthrow
    -> const String *
{
  let const *storage = m_functions.find(name);
  return storage != nullptr ? storage->get_source() : nullptr;
}

fn EvalContext::sorted_function_names() const throws -> ArrayList<String>
{
  let out = ArrayList<String>{heap_allocator()};
  out.reserve(m_functions.count());
  m_functions.for_each([&](StringView name, const FunctionBodyHandle &) {
    out.push_managed(name);
  });
  out.sort();
  return out;
}

fn EvalContext::find_function(StringView name) const wontthrow
    -> const Expression *
{
  let const *storage = m_functions.find(name);
  return storage != nullptr ? storage->get_body() : nullptr;
}

pure fn EvalContext::find_function_storage(StringView name) const wontthrow
    -> const FunctionBodyHandle *
{
  return m_functions.find(name);
}

pure fn EvalContext::has_functions() const wontthrow -> bool
{
  return m_functions.count() != 0;
}

pure fn EvalContext::function_storage_stats() const wontthrow
    -> function_arena_stats
{
  return live_function_storage_stats();
}

fn EvalContext::unset_function(StringView name) throws -> void
{
  LOG(Info, "unsetting function '%.*s'", static_cast<int>(name.length),
      name.data);
  m_functions.erase(name);
}

fn EvalContext::function_names() const throws -> HashSet
{
  let names = HashSet{heap_allocator()};
  m_functions.for_each([&](StringView name, const FunctionBodyHandle &storage) {
    unused(storage);
    names.add(name);
  });
  return names;
}

fn EvalContext::variable_names(Allocator result_allocator) const throws
    -> HashSet
{
  let names = HashSet{result_allocator};
  m_shell_variables.for_each([&](StringView name, const String &value) {
    unused(value);
    names.add(name);
  });
  /* An indexed or associative array is a set variable too, so its name joins
     the scalar names. */
  m_indexed_arrays.for_each(
      [&](StringView name, const ArrayList<String> &value) {
        unused(value);
        names.add(name);
      });
  m_associative_names.for_each([&](StringView name) { names.add(name); });
#if !defined NDEBUG
  m_debug_variable_name_enumeration_count += names.count();
#endif
  return names;
}

fn EvalContext::run_named_trap(StringView condition) throws -> void
{
  if (m_running_traps) return;
  const String *action = m_traps.find(condition);
  if (action == nullptr || action->count() == 0) {
    return;
  }

  m_running_traps = true;
  defer { m_running_traps = false; };

  let const saved_exit_status = m_last_exit_status;
  run_source(action->view(),
             "the " + String{heap_allocator(), condition} + " trap");
  m_last_exit_status = saved_exit_status;
}

fn EvalContext::set_trap(StringView condition, StringView action) throws -> void
{
  LOG(Info, "setting a trap for '%.*s' with a %zu byte action",
      static_cast<int>(condition.length), condition.data, action.length);
  m_traps.set(condition, action);
  m_has_debug_trap = m_traps.find(StringView{"DEBUG", 5}) != nullptr;
  /* EXIT runs at the shell's end and needs no OS handler. An empty action
     installs the ignore disposition the way trap "" SIG asks. */
  if (condition == "EXIT") return;
  if (let const number = os::signal_number_from_name(condition)) {
    if (action.is_empty())
      os::set_trap_ignore(*number);
    else
      os::set_trap_handler(*number);
  }
}

fn EvalContext::remove_trap(StringView condition) throws -> void
{
  LOG(Info, "removing the trap for '%.*s'", static_cast<int>(condition.length),
      condition.data);
  m_traps.erase(condition);
  m_has_debug_trap = m_traps.find(StringView{"DEBUG", 5}) != nullptr;
  if (condition == "EXIT") return;
  if (let const number = os::signal_number_from_name(condition))
    os::clear_trap_handler(*number);
}

fn EvalContext::install_trap_dispositions() throws -> void
{
  LOG(Info, "reinstalling the dispositions of %zu traps", m_traps.count());
  m_traps.for_each([&](StringView condition, const String &action) {
    if (condition == "EXIT") return;
    if (let const number = os::signal_number_from_name(condition)) {
      if (action.is_empty())
        os::set_trap_ignore(*number);
      else
        os::set_trap_handler(*number);
    }
  });
}

fn EvalContext::run_pending_traps() throws -> void
{
  if (m_running_traps) return;
  m_running_traps = true;
  defer { m_running_traps = false; };

  /* The fast flag is cleared before the per-signal flags are consumed, so a
     signal that arrives during the drain re-sets it and the next boundary
     drains again rather than dropping the arrival. */
  os::SIGNAL_PENDING = 0;

  let const saved_exit_status = m_last_exit_status;

  for (i32 number = os::take_pending_signal(); number != 0;
       number = os::take_pending_signal())
  {
    let const name = os::signal_name_from_number(number);
    if (!name.has_value()) continue;
    if (let const *action = m_traps.find(name->view()); action != nullptr)
      if (action->count() > 0) {
        LOG(Info, "running the trap action for signal '%s'", name->c_str());
        run_source(action->view(), "the " + *name + " trap");
      }
  }

  m_last_exit_status = saved_exit_status;
}

pure fn EvalContext::traps() const wontthrow -> const StringMap<String> &
{
  return m_traps;
}

cold fn EvalContext::run_exit_trap() throws -> void
{
  if (m_exit_trap_ran) return;
  m_exit_trap_ran = true;

  /* A Ctrl-C that ended the last command leaves the interrupt flag set, so it
     is dropped before the action evaluates. */
  os::INTERRUPT_REQUESTED = 0;

  if (let const *action = m_traps.find(StringView{"EXIT", 4});
      action != nullptr)
    if (action->count() > 0) {
      LOG(Info, "running the EXIT trap action at shell exit");
      run_source(action->view(), "the EXIT trap");
    }
}

fn EvalContext::has_exit_trap() const wontthrow -> bool
{
  if (let const *action = m_traps.find(StringView{"EXIT", 4});
      action != nullptr)
    return action->count() > 0;
  return false;
}

fn EvalContext::clear_inherited_exit_trap() throws -> void
{
  m_traps.erase(StringView{"EXIT", 4});
}

cold fn EvalContext::run_subshell_exit_trap() throws -> void
{
  /* Only an EXIT action the subshell itself set is present, since the boundary
     cleared the inherited one on entry. It runs before restore_state returns
     the parent's traps. */
  if (let const *action = m_traps.find(StringView{"EXIT", 4});
      action != nullptr)
    if (action->count() > 0) {
      LOG(Info, "running the EXIT trap action the subshell set at its end");
      run_source(action->view(), "the EXIT trap");
    }
}

fn EvalContext::mark_readonly(StringView name) throws -> void
{
  set_variable_attribute(name, variable_attribute::Readonly, true);
}

fn EvalContext::unmark_readonly(StringView name) throws -> void
{
  set_variable_attribute(name, variable_attribute::Readonly, false);
}

fn EvalContext::is_readonly(StringView name) const wontthrow -> bool
{
  if (restricted_enforcement_active())
    for (let const restricted_name : RESTRICTED_READONLY_NAMES)
      if (name == restricted_name) return true;
  return (variable_attributes(name) &
          static_cast<u8>(variable_attribute::Readonly)) != 0;
}

fn EvalContext::readonly_names() const throws -> ArrayList<String>
{
  let out = ArrayList<String>{heap_allocator()};
  out.reserve(m_variable_attributes.count() +
              countof(RESTRICTED_READONLY_NAMES));
  m_variable_attributes.for_each([&](StringView name, u8 attributes) {
    if ((attributes & static_cast<u8>(variable_attribute::Readonly)) != 0)
      out.push_managed(name);
  });
  if (restricted_enforcement_active())
    for (let const restricted_name : RESTRICTED_READONLY_NAMES)
      if ((variable_attributes(restricted_name) &
           static_cast<u8>(variable_attribute::Readonly)) == 0)
        out.push_managed(restricted_name);
  out.sort();
  return out;
}

fn EvalContext::mark_integer(StringView name) throws -> void
{
  set_variable_attribute(name, variable_attribute::Integer, true);
}

fn EvalContext::unmark_integer(StringView name) throws -> void
{
  set_variable_attribute(name, variable_attribute::Integer, false);
}

fn EvalContext::is_integer_variable(StringView name) const wontthrow -> bool
{
  return (variable_attributes(name) &
          static_cast<u8>(variable_attribute::Integer)) != 0;
}

fn EvalContext::mark_lowercase(StringView name) throws -> void
{
  set_variable_attribute(name, variable_attribute::Uppercase, false);
  set_variable_attribute(name, variable_attribute::Lowercase, true);
}

fn EvalContext::unmark_lowercase(StringView name) throws -> void
{
  set_variable_attribute(name, variable_attribute::Lowercase, false);
}

fn EvalContext::is_lowercase_variable(StringView name) const wontthrow -> bool
{
  return (variable_attributes(name) &
          static_cast<u8>(variable_attribute::Lowercase)) != 0;
}

fn EvalContext::mark_uppercase(StringView name) throws -> void
{
  set_variable_attribute(name, variable_attribute::Lowercase, false);
  set_variable_attribute(name, variable_attribute::Uppercase, true);
}

fn EvalContext::unmark_uppercase(StringView name) throws -> void
{
  set_variable_attribute(name, variable_attribute::Uppercase, false);
}

fn EvalContext::is_uppercase_variable(StringView name) const wontthrow -> bool
{
  return (variable_attributes(name) &
          static_cast<u8>(variable_attribute::Uppercase)) != 0;
}

pure fn EvalContext::variable_attributes(StringView name) const wontthrow -> u8
{
  let const *attributes = m_variable_attributes.find(name);
  return attributes != nullptr ? *attributes : 0;
}

fn EvalContext::set_variable_attribute(StringView name,
                                       variable_attribute attribute,
                                       bool is_enabled) throws -> void
{
  let attributes = variable_attributes(name);
  let const mask = static_cast<u8>(attribute);
  if (is_enabled)
    attributes |= mask;
  else
    attributes &= static_cast<u8>(~mask);

  if (attributes == 0)
    m_variable_attributes.erase(name);
  else
    m_variable_attributes.set(name, attributes);
}

fn EvalContext::apply_variable_case(StringView name,
                                    String &value) const wontthrow -> void
{
  let const attributes = variable_attributes(name);
  if ((attributes & static_cast<u8>(variable_attribute::Lowercase)) != 0)
    value.lowercase_ascii();
  else if ((attributes & static_cast<u8>(variable_attribute::Uppercase)) != 0)
    value.uppercase_ascii();
}

fn EvalContext::append_integer_expression(String &joined,
                                          StringView expression) const throws
    -> void
{
  joined += '+';
  for (usize i = 0; i < expression.length; i++) {
    let const character = expression[i];
    if (character != ' ' && character != '\t' && character != '\n' &&
        character != '\r')
    {
      joined += '(';
      joined.append(expression.substring(i));
      joined += ')';
      return;
    }
  }
  joined += '0';
}

} /* namespace koshka */
