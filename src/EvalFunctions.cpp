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

fn EvalContext::register_function(StringView name, const Expression *body,
                                  StringView definition_text,
                                  usize body_start_position,
                                  SourceLocation definition_location) throws
    -> void
{
  LOG(Info, "registering function '%.*s' with a %zu byte definition",
      static_cast<int>(name.length), name.data, definition_text.length);
  m_functions.set(name, body);
  m_function_sources.set(name, definition_text);

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
  m_function_definition_infos.set(name, steal(info));
}

fn EvalContext::function_definition_info_of(StringView name) const wontthrow
    -> const function_definition_info *
{
  return m_function_definition_infos.find(name);
}

pure fn EvalContext::resolve_render_source(
    const SourceLocation &location) const wontthrow -> resolved_render_source
{
  let resolved_source = resolved_render_source{};
  resolved_source.text = m_current_source;

  if (m_function_call_names.is_empty()) return resolved_source;
  let const innermost = funcname_frame_at(0);
  let const *info = m_function_definition_infos.find(innermost);
  /* The body span check below decides whether to window, not the defining
     source pointer. A freed defining source can have its address reused by the
     current source, so a pointer compare would falsely read the body as
     belonging to the current source and drop the function's filename. */
  if (info == nullptr) return resolved_source;
  let const *copy = m_function_sources.find(innermost);
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
  return m_function_sources.find(name);
}

fn EvalContext::sorted_function_names() const throws -> ArrayList<String>
{
  let out = ArrayList<String>{heap_allocator()};
  out.reserve(m_functions.count());
  m_functions.for_each(
      [&](StringView name, const Expression *) { out.push_managed(name); });
  out.sort();
  return out;
}

fn EvalContext::find_function(StringView name) const wontthrow
    -> const Expression *
{
  if (let const *const *slot = m_functions.find(name); slot != nullptr)
    return *slot;
  return nullptr;
}

pure fn EvalContext::has_functions() const wontthrow -> bool
{
  return m_functions.count() != 0;
}

fn EvalContext::unset_function(StringView name) throws -> void
{
  LOG(Info, "unsetting function '%.*s'", static_cast<int>(name.length),
      name.data);
  m_functions.erase(name);
  m_function_sources.erase(name);
  m_function_definition_infos.erase(name);
}

fn EvalContext::function_names() const throws -> HashSet
{
  let names = HashSet{heap_allocator()};
  m_functions.for_each([&](StringView name, const Expression *body) {
    unused(body);
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
  m_readonly_names.add(name);
}

fn EvalContext::unmark_readonly(StringView name) throws -> void
{
  m_readonly_names.remove(name);
}

fn EvalContext::is_readonly(StringView name) const wontthrow -> bool
{
  if (restricted_enforcement_active())
    for (let const restricted_name : RESTRICTED_READONLY_NAMES)
      if (name == restricted_name) return true;
  return m_readonly_names.contains(name);
}

fn EvalContext::readonly_names() const throws -> ArrayList<String>
{
  let out = ArrayList<String>{heap_allocator()};
  out.reserve(m_readonly_names.count() + countof(RESTRICTED_READONLY_NAMES));
  m_readonly_names.for_each([&](StringView name) { out.push_managed(name); });
  if (restricted_enforcement_active())
    for (let const restricted_name : RESTRICTED_READONLY_NAMES)
      if (!m_readonly_names.contains(restricted_name))
        out.push_managed(restricted_name);
  out.sort();
  return out;
}

fn EvalContext::mark_integer(StringView name) throws -> void
{
  m_integer_names.add(name);
}

fn EvalContext::unmark_integer(StringView name) throws -> void
{
  m_integer_names.remove(name);
}

fn EvalContext::is_integer_variable(StringView name) const wontthrow -> bool
{
  return m_integer_names.contains(name);
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
