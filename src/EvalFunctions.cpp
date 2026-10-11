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

#include "Eval.hpp"
#include "Expressions.hpp"
#include "Lexer.hpp"
#include "Parser.hpp"
#include "Platform.hpp"
#include "Utils.hpp"
#include "base/Common.hpp"
#include "base/Debug.hpp"
#include "base/PackedStringKey.hpp"
#include "base/StaticStringMap.hpp"
#include "base/Trace.hpp"

namespace koshka {

namespace {

constexpr StringView TRAP_SOURCE_NAME{"trap"};

constexpr PackedStringKey RESTRICTED_READONLY_KEYS[] = {
    SSK("SHELL"),
    SSK("PATH"),
    SSK("ENV"),
    SSK("BASH_ENV"),
    SSK("KOSH_HISTORY_FILE"),
    SSK("KOSH_HISTORY_SIZE"),
    SSK("KOSH_CALC_HISTORY"),
    SSK("KOSH_DIRECTORY_HISTORY")};
constexpr StaticStringSet RESTRICTED_READONLY_NAMES{RESTRICTED_READONLY_KEYS};

constexpr PackedStringKey BASH_IMPLICIT_READONLY_KEYS[] = {
    SSK("BASHOPTS"), SSK("SHELLOPTS"), SSK("EUID"), SSK("PPID"), SSK("UID")};
constexpr StaticStringSet BASH_IMPLICIT_READONLY_NAMES{
    BASH_IMPLICIT_READONLY_KEYS};

constexpr PackedStringKey BASH_IMPLICIT_INTEGER_KEYS[] = {
    SSK("BASHPID"), SSK("EUID"),    SSK("HISTCMD"), SSK("OPTIND"), SSK("PPID"),
    SSK("RANDOM"),  SSK("SECONDS"), SSK("SRANDOM"), SSK("UID"),
};
constexpr StaticStringSet BASH_IMPLICIT_INTEGER_NAMES{
    BASH_IMPLICIT_INTEGER_KEYS};

constexpr PackedStringKey NAMED_TRAP_CONDITION_KEYS[] = {
    SSK("DEBUG"),
    SSK("ERR"),
    SSK("RETURN"),
    SSK("CHLD"),
};
constexpr StaticStringSet NAMED_TRAP_CONDITIONS{NAMED_TRAP_CONDITION_KEYS};

pure fn running_trap_bit(StringView condition) wontthrow -> u8
{
  let const index = NAMED_TRAP_CONDITIONS.find_index(condition);
  ASSERT(index.has_value());

  return static_cast<u8>(1u << *index);
}

} /* namespace */

fn EvalContext::register_function(StringView name,
                                  const FunctionBodyHandle &body_storage,
                                  StringView definition_text,
                                  usize body_start_position,
                                  SourceLocation definition_location) throws
    -> void
{
  ASSERT(body_storage.has_value());
  ASSERT(body_storage.get_body() != nullptr);

  if (function_store().readonly().contains(name)) {
    throw Error{"Unable to redefine '" + name +
                "' because it is a read only function"};
  }

  let info = function_definition_info{};
  info.body_start_position = body_start_position;
  info.header_length = name.length + StringView{" () \n"}.length;
  info.source_name_index = definition_location.source_name_index;
  if (source_store().current_source() != nullptr && !definition_text.is_empty())
  {
    let const defining_view = source_store().current_source()->view();
    let const body_location = SourceLocation{
        body_start_position, 0, definition_location.source_name_index};
    let const body_site =
        resolve_rendered_site(defining_view, body_location, 0, false, this);
    let const rendered_body_line =
        static_cast<isize>(utils::line_number_at(body_site.source,
                                                 body_site.location.position)) +
        body_site.line_offset;
    let const counted_body_line =
        static_cast<isize>(line_number_at_location(body_location));
    info.line_offset = rendered_body_line - 1;
    info.enclosing_line_count =
        counted_body_line > rendered_body_line
            ? static_cast<usize>(counted_body_line - rendered_body_line)
            : usize{0};
    let const in_place_body_line = static_cast<isize>(
        utils::line_number_at(defining_view, body_start_position));
    info.is_numbered_apart = info.enclosing_line_count != 0 ||
                             in_place_body_line != rendered_body_line;
    if (body_site.location.source_name_index != 0)
      info.source_name_index = body_site.location.source_name_index;

    let const body_end_position =
        body_start_position + definition_text.length - info.header_length;
    if (body_start_position <= defining_view.length &&
        body_end_position >= body_start_position &&
        body_end_position <= defining_view.length)
    {
      let const before_body =
          defining_view.substring_of_length(0, body_start_position);
      let const last_newline = before_body.find_last_character('\n');
      let const line_start =
          last_newline.has_value() ? *last_newline + 1 : usize{0};
      let const after_body = defining_view.substring(body_end_position);
      let const line_length =
          after_body.find_character('\n').value_or(after_body.length);
      info.line_prefix =
          String{heap_allocator(), before_body.substring(line_start)};
      info.line_suffix = String{heap_allocator(),
                                after_body.substring_of_length(0, line_length)};
    }
  }

  if (source_store().current_source() != nullptr &&
      definition_location.position < source_store().current_source()->count())
  {
    info.definition_line = line_number_at_location(definition_location);
  }

  info.body_name_index = definition_location.source_name_index;
  info.defining_state = definition_state::from(runtime_state());
  body_storage.set_definition(definition_text, info);

  LOG(Info, "registering function '%.*s' with a %zu byte definition",
      static_cast<int>(name.length), name.data, definition_text.length);
  function_store().definitions().set(name, body_storage);
}

fn EvalContext::function_definition_info_of(StringView name) const wontthrow
    -> const function_definition_info *
{
  let const storage = function_store().definitions().find(name);
  return storage.has_value() ? storage->get_definition_info() : nullptr;
}

pure fn EvalContext::source_depth_floor(usize frame_limit) const wontthrow
    -> usize
{
  for (usize index = frame_limit; index > 0; index--) {
    let const &candidate = source_store().source_frames()[index - 1];
    if (candidate.is_source_changing) return candidate.function_call_depth;
  }

  return 0;
}

pure fn EvalContext::resolve_render_source(
    const SourceLocation &location, const String *fallback_source,
    usize call_depth_limit, usize call_depth_floor) const wontthrow
    -> resolved_render_source
{
  let resolved_source = resolved_render_source{};
  resolved_source.text = fallback_source != nullptr
                             ? fallback_source
                             : source_store().current_source();
  resolved_source.line_offset = static_cast<isize>(
      source_store().preceding_line_count_of(resolved_source.text));

  if (function_store().call_frames().is_empty()) return resolved_source;

  let const first_depth =
      function_store().call_frames().count() < call_depth_limit
          ? function_store().call_frames().count()
          : call_depth_limit;
  let const lowest_depth =
      call_depth_floor == static_cast<usize>(-1)
          ? source_depth_floor(source_store().source_frames().count())
          : call_depth_floor;
  for (usize depth = first_depth; depth > lowest_depth; depth--) {
    let const &storage = function_store().call_frames()[depth - 1].storage;
    let const *info = storage.get_definition_info();
    if (info == nullptr) continue;

    let const *copy = storage.get_source();
    if (copy == nullptr || copy->count() <= info->header_length) continue;

    if (location.source_name_index != info->body_name_index) continue;

    let const body_length = copy->count() - info->header_length;
    if (location.position < info->body_start_position ||
        location.position >= info->body_start_position + body_length)
    {
      continue;
    }

    let const *rendered_source = resolved_source.text;
    if (!info->is_numbered_apart && rendered_source != nullptr &&
        rendered_source->count() >= info->body_start_position + body_length &&
        rendered_source->view().substring_of_length(info->body_start_position,
                                                    body_length) ==
            copy->view().substring(info->header_length))
    {
      continue;
    }

    if (!info->has_render_source) {
      info->render_source = info->line_prefix;
      info->render_source.append(copy->view().substring(info->header_length));
      info->render_source.append(info->line_suffix.view());
      info->has_render_source = true;
    }

    resolved_source.text = &info->render_source;
    resolved_source.is_windowed = true;
    resolved_source.body_start_position = info->body_start_position;
    resolved_source.header_length = info->line_prefix.count();
    resolved_source.line_offset = info->line_offset;
    resolved_source.enclosing_line_count = info->enclosing_line_count;
    resolved_source.source_name_index = info->source_name_index;
    return resolved_source;
  }

  return resolved_source;
}

pure fn EvalContext::resolve_current_function_window(
    StringView rendered_source, const SourceLocation &location) const wontthrow
    -> resolved_render_source
{
  let const *current = source_store().current_source();
  if (current == nullptr || rendered_source.data != current->view().data ||
      rendered_source.length != current->view().length)
  {
    return resolved_render_source{};
  }

  let resolved_source = resolve_render_source(location);
  if (!resolved_source.is_windowed) return resolved_render_source{};

  return resolved_source;
}

pure fn EvalContext::source_text_in_span(const SourceLocation &location,
                                         usize end_position) const wontthrow
    -> StringView
{
  let const resolved_source = resolve_render_source(location);
  if (resolved_source.text == nullptr) return {};

  let const view = resolved_source.text->view();
  let const location_end = location.position + usize{location.length};
  let const start = resolved_source.to_render_position(location.position);
  let const stop = resolved_source.to_render_position(
      end_position > location_end ? end_position : location_end);
  if (start >= stop || stop > view.length) return {};

  return view.substring_of_length(start, stop - start);
}

pure fn EvalContext::function_storage_stats() const wontthrow
    -> function_arena_stats
{
  return live_function_storage_stats();
}

fn EvalContext::unset_function(StringView name) throws -> void
{
  if (function_store().readonly().contains(name)) {
    throw Error{"Unable to unset '" + name +
                "' because it is a read only function"};
  }

  LOG(Info, "unsetting function '%.*s'", static_cast<int>(name.length),
      name.data);
  function_store().definitions().erase(name);
}

fn EvalContext::mark_function_readonly(StringView name) throws -> void
{
  LOG(Info, "marking function '%.*s' read only", static_cast<int>(name.length),
      name.data);
  function_store().readonly().add(name);
}

fn EvalContext::variable_names(Allocator result_allocator) const throws
    -> HashSet
{
  let names = HashSet{result_allocator};
  variable_store().shell_variables().for_each(
      [&](StringView name, const String &value) {
        unused(value);
        names.add(name);
      });
  variable_store().indexed_arrays().for_each(
      [&](StringView name, const ArrayList<String> &value) {
        unused(value);
        names.add(name);
      });
  variable_store().associative_arrays().names().for_each(
      [&](StringView name) { names.add(name); });
  if (runtime_state().bash_dynamic_variables_enabled()) {
    names.add(BASH_ARGUMENT_COUNT_VARIABLE);
    names.add(BASH_ARGUMENT_VALUE_VARIABLE);
  }
  if (is_bash_special_array_active(bash_special_array_id::Aliases))
    names.add(BASH_ALIASES_VARIABLE);
  if (is_bash_directory_stack_special(DIRSTACK_VARIABLE))
    names.add(DIRSTACK_VARIABLE);
#if !defined NDEBUG
  dynamic_runtime_store().debug_variable_name_enumeration_count() +=
      names.count();
#endif
  return names;
}

fn EvalContext::cached_trap_body(StringView condition, StringView action) throws
    -> FunctionBodyHandle
{
  if (let const cached = trap_store().cached_bodies().find(condition);
      cached.has_value())
  {
    let const *cached_source = cached->get_source();
    if (cached->get_body() != nullptr && cached_source != nullptr &&
        cached_source->view() == action)
    {
      return *cached.value();
    }
  }

  if (action.find_character('\r').has_value()) return FunctionBodyHandle{};

  LOG(Debug, "parsing the '%.*s' trap action of %zu bytes for reuse",
      static_cast<int>(condition.length), condition.data, action.length);

  let body_storage = FunctionBodyHandle::create(action.length);
  body_storage.set_definition(action, function_definition_info{});

  let const *stored_action = body_storage.get_source();
  ASSERT(stored_action != nullptr);

  let parser = Parser{
      Lexer{stored_action->view(), *body_storage.get_arena(), TRAP_SOURCE_NAME,
            runtime_state().get_mood(),
            ParseSession::AllocationKind::FunctionBody}
  };
  let const parsed_action = parser.construct_ast();
  if (parsed_action == nullptr) return FunctionBodyHandle{};

  body_storage.set_body(parsed_action);
  trap_store().cached_bodies().set(condition, body_storage);

  return body_storage;
}

fn EvalContext::run_named_trap(StringView condition,
                               const SourceLocation *trigger_location) throws
    -> void
{
  trap_store().last_trap_action_status() = 0;

  let const condition_bit = running_trap_bit(condition);
  if (trap_store().is_condition_running(condition_bit)) return;
  let const trap = trap_store().find_active(condition);
  if (!trap.has_value()) return;

  let const action = trap->action_text.view();

  let const was_terminal_exec_allowed =
      execution_store().terminal_exec_allowed();
  execution_store().terminal_exec_allowed() = false;
  defer
  {
    execution_store().terminal_exec_allowed() = was_terminal_exec_allowed;
  };

  let const trigger_site = trigger_location != nullptr
                               ? *trigger_location
                               : source_store().current_location();
  let const saved_exit_status = execution_store().last_exit_status();
  let const action_scope = TrapActionScope::enter_condition(
      trap_store(), condition_bit, line_number_at_location(trigger_site),
      source_store().source_frames().count() + 1, function_store().call_depth(),
      saved_exit_status);

  let const current_pipe_statuses =
      variable_store().indexed_arrays().find("PIPESTATUS");
  let const has_saved_pipe_statuses = current_pipe_statuses.has_value();
  ArrayList<String> saved_pipe_statuses{heap_allocator()};
  if (has_saved_pipe_statuses)
    saved_pipe_statuses = current_pipe_statuses->clone();

  let was_pipe_status_restored = false;
  defer
  {
    trap_store().last_trap_action_status() =
        execution_store().last_exit_status();
    execution_store().last_exit_status() = saved_exit_status;

    if (!was_pipe_status_restored) {
      restore_trap_pipe_statuses(has_saved_pipe_statuses,
                                 steal(saved_pipe_statuses));
    }
  };

  let const cached_action = cached_trap_body(condition, action);

  let const definition = trap_store().find_definition(condition);

  run_source(action, "the " + String{heap_allocator(), condition} + " trap",
             trigger_site, TRAP_SOURCE_NAME, nullptr,
             cached_action.has_value() ? &cached_action : nullptr,
             return_handling::Reject, history_recording::Disabled,
             definition.has_value() ? &*definition : nullptr);

  restore_trap_pipe_statuses(has_saved_pipe_statuses,
                             steal(saved_pipe_statuses));
  was_pipe_status_restored = true;
}

fn EvalContext::restore_trap_pipe_statuses(
    bool has_saved_pipe_statuses,
    ArrayList<String> saved_pipe_statuses) wontthrow -> void
{
  try {
    if (!has_saved_pipe_statuses) {
      variable_store().indexed_arrays().erase("PIPESTATUS");
      return;
    }

    if (let current = variable_store().indexed_arrays().find("PIPESTATUS");
        current.has_value())
    {
      *current.value() = steal(saved_pipe_statuses);
      return;
    }

    variable_store().indexed_arrays().set("PIPESTATUS",
                                          steal(saved_pipe_statuses));
  } catch (...) {
    LOG(Info, "the PIPESTATUS restore of a trap action could not allocate");
  }
}

fn EvalContext::run_return_trap(i32 status_before_return) throws -> void
{
  let const saved_exit_status = execution_store().last_exit_status();
  execution_store().last_exit_status() = status_before_return;
  defer { execution_store().last_exit_status() = saved_exit_status; };

  let frame_control_flow = steal(control_flow_store().pending());
  control_flow_store().clear();

  let action_control_flow = control_flow{};
  let did_action_return = false;
  while (true) {
    run_named_trap(StringView{"RETURN", 6});
    if (!control_flow_store().has_pending()) break;

    if (control_flow_store().pending().kind != control_flow::Kind::Return)
      return;

    LOG(Info, "the RETURN action returned with status %lld, firing again",
        (long long) control_flow_store().pending().value);
    action_control_flow = steal(control_flow_store().pending());
    control_flow_store().clear();
    did_action_return = true;
  }

  control_flow_store().set(did_action_return ? steal(action_control_flow)
                                             : steal(frame_control_flow));
}

fn EvalContext::reset_inherited_signal_traps() wontthrow -> void
{
  LOG(Debug, "the subshell holds its inherited signal actions unfired");
  trap_store().did_reset_inherited_signal_traps() = true;
}

pure fn EvalContext::did_reset_inherited_signal_traps() const wontthrow -> bool
{
  return trap_store().did_reset_inherited_signal_traps();
}

fn EvalContext::note_subshell_child_exit() wontthrow -> void
{
  if (trap_store().did_reset_inherited_signal_traps()) return;

  LOG(Debug, "counting the finished subshell as one reaped child");
  os::note_child_reaped();
}

fn EvalContext::discard_inherited_signal_traps() throws -> void
{
  if (!trap_store().did_reset_inherited_signal_traps()) return;

  trap_store().did_reset_inherited_signal_traps() = false;

  LOG(Info, "the subshell discards its inherited signal actions");
  trap_store().discard_signal_traps();
}

pure fn EvalContext::is_signal_ignored_at_startup(
    StringView condition) const wontthrow -> bool
{
  let const ignored = runtime_state().is_bash_compatible()
                          ? trap_store().startup_ignored_signals()
                          : 0;
  if (ignored == 0) return false;

  let const number = os::signal_number_from_name(condition);
  if (!number.has_value()) return false;
  if (*number < 1 || *number > os::ENTRY_IGNORED_SIGNAL_LIMIT) return false;

  return (ignored & (u64{1} << (*number - 1))) != 0;
}

fn EvalContext::capture_trap_definition(const SourceLocation &location,
                                        StringView action) throws
    -> trap_definition
{
  let definition = trap_definition{
      String{heap_allocator(), action},
      String{heap_allocator()},
      location, 0,
      true
  };
  let const resolved_source = resolve_render_source(location);
  if (resolved_source.text == nullptr) return definition;

  let const source = resolved_source.text->view();
  let const position = resolved_source.to_render_position(location.position);
  if (position >= source.length) return definition;

  let const line_position = utils::source_line_position_at(source, position);
  let const line_source = source.substring_of_length(
      line_position.line_start,
      line_position.line_end - line_position.line_start);
  let const available_length =
      line_source.length - (position - line_position.line_start);

  definition.line_source = String{heap_allocator(), line_source};
  definition.location = SourceLocation{
      position - line_position.line_start,
      location.length < available_length ? location.length : available_length,
      resolved_source.is_windowed ? resolved_source.source_name_index
                                  : location.source_name_index};
  definition.line_offset = static_cast<isize>(line_position.line_number) +
                           resolved_source.line_offset;

  return definition;
}

fn EvalContext::set_trap(StringView condition, StringView action,
                         Maybe<SourceLocation> definition_location) throws
    -> void
{
  if (is_signal_ignored_at_startup(condition)) {
    LOG(Info, "keeping '%.*s' ignored because the shell inherited it ignored",
        static_cast<int>(condition.length), condition.data);
    return;
  }

  LOG(Info, "setting a trap for '%.*s' with a %zu byte action",
      static_cast<int>(condition.length), condition.data, action.length);
  discard_inherited_signal_traps();
  if (definition_location.has_value()) {
    trap_store().set(condition,
                     capture_trap_definition(*definition_location, action));
  } else {
    trap_store().set_action(condition, action);
  }

  enum class pseudo_condition : u8
  {
    Debug,
    Error,
    Exit,
  };
  static constexpr static_string_entry<pseudo_condition> PSEUDO_ENTRIES[] = {
      {SSK("DEBUG"), pseudo_condition::Debug},
      {SSK("ERR"),   pseudo_condition::Error},
      {SSK("EXIT"),  pseudo_condition::Exit },
  };
  static constexpr StaticStringMap PSEUDO_CONDITIONS{PSEUDO_ENTRIES};
  if (let const pseudo = PSEUDO_CONDITIONS.find(condition); pseudo.has_value())
  {
    switch (*pseudo) {
    case pseudo_condition::Debug:
      trap_store().debug_trap_active_depth() = nesting_depth();
      break;
    case pseudo_condition::Error:
      trap_store().err_trap_active_depth() = nesting_depth();
      break;
    case pseudo_condition::Exit: return;
    }
  }
  if (let const number = os::signal_number_from_name(condition)) {
    if (action.is_empty())
      os::set_trap_ignore(*number);
    else
      os::set_trap_handler(*number);
  }
}

fn EvalContext::remove_trap(StringView condition) throws -> void
{
  if (is_signal_ignored_at_startup(condition)) {
    LOG(Info, "keeping '%.*s' ignored because the shell inherited it ignored",
        static_cast<int>(condition.length), condition.data);
    return;
  }

  LOG(Info, "removing the trap for '%.*s'", static_cast<int>(condition.length),
      condition.data);
  discard_inherited_signal_traps();
  trap_store().reset(condition);
  if (condition == "EXIT") return;
  if (let const number = os::signal_number_from_name(condition))
    os::clear_trap_handler(*number);
}

fn EvalContext::save_untraced_trap(StringView condition,
                                   shell_option_id trace_option,
                                   usize *active_depth) throws
    -> saved_frame_trap
{
  saved_frame_trap saved{};
  if (active_depth != nullptr) saved.active_depth = *active_depth;

  if (runtime_state().option_is_enabled(trace_option)) return saved;

  let const trap = trap_store().find(condition);
  if (!trap.has_value()) return saved;

  LOG(Info, "taking a %zu byte '%.*s' action away from an untraced body",
      trap->action_text.count(), static_cast<int>(condition.length),
      condition.data);
  saved.definition = trap_definition{*trap.value()};
  trap_store().reset(condition);

  return saved;
}

fn EvalContext::restore_untraced_trap(StringView condition,
                                      saved_frame_trap &&saved,
                                      usize *active_depth) wontthrow -> void
{
  if (!saved.definition.has_value()) return;
  if (trap_store().find(condition).has_value()) return;

  LOG(Info, "restoring the '%.*s' action an untraced body ran without",
      static_cast<int>(condition.length), condition.data);
  try {
    trap_store().set(condition, steal(*saved.definition));
  } catch (...) {
    LOG(Info, "the '%.*s' action of an untraced body could not be restored",
        static_cast<int>(condition.length), condition.data);
    return;
  }

  if (active_depth != nullptr) *active_depth = saved.active_depth;
}

UntracedTrapScope::UntracedTrapScope(EvalContext &context, Kind kind,
                                     bool should_apply) throws
    : m_context(context),
      m_kind(kind)
{
  if (!should_apply) return;

  switch (kind) {
  case Kind::Debug:
    m_saved = context.save_untraced_trap(
        StringView{"DEBUG", 5}, shell_option_id::Functrace,
        &context.trap_store().debug_trap_active_depth());
    break;
  case Kind::Err:
    m_saved = context.save_untraced_trap(
        StringView{"ERR", 3}, shell_option_id::Errtrace,
        &context.trap_store().err_trap_active_depth());
    break;
  case Kind::Return:
    if (context.runtime_state().is_bash_compatible()) {
      m_saved = context.save_untraced_trap(StringView{"RETURN", 6},
                                           shell_option_id::Functrace, nullptr);
    }
    break;
  }
}

UntracedTrapScope::~UntracedTrapScope()
{
  switch (m_kind) {
  case Kind::Debug:
    m_context.restore_untraced_trap(
        StringView{"DEBUG", 5}, steal(m_saved),
        &m_context.trap_store().debug_trap_active_depth());
    break;
  case Kind::Err:
    m_context.restore_untraced_trap(
        StringView{"ERR", 3}, steal(m_saved),
        &m_context.trap_store().err_trap_active_depth());
    break;
  case Kind::Return:
    m_context.restore_untraced_trap(StringView{"RETURN", 6}, steal(m_saved),
                                    nullptr);
    break;
  }
}

fn EvalContext::install_trap_dispositions() throws -> void
{
  LOG(Info, "reinstalling the dispositions of %zu traps", trap_store().count());
  trap_store().list([&](StringView condition, const trap_definition &trap) {
    if (condition == "EXIT") return;
    if (let const number = os::signal_number_from_name(condition)) {
      if (trap.action_text.is_empty())
        os::set_trap_ignore(*number);
      else
        os::set_trap_handler(*number);
    }
  });
}

fn EvalContext::run_pending_traps() throws -> void
{
  let const saved_exit_status = execution_store().last_exit_status();
  let const action_scope =
      TrapActionScope::enter(trap_store(), saved_exit_status);

  let const was_terminal_exec_allowed =
      execution_store().terminal_exec_allowed();
  execution_store().terminal_exec_allowed() = false;
  defer
  {
    execution_store().terminal_exec_allowed() = was_terminal_exec_allowed;
  };

  os::SIGNAL_PENDING = 0;

  let const child_condition = StringView{"CHLD", 4};
  let const reaped_child_count = os::take_reaped_child_count();
  if (trap_store().did_reset_inherited_signal_traps()) {
    os::clear_reaped_child_arrival();
  } else if (trap_store().find_active(child_condition).has_value()) {
    trap_store().pending_child_trap_count() += reaped_child_count;
  } else {
    os::clear_reaped_child_arrival();
  }

  let const current_pipe_statuses =
      variable_store().indexed_arrays().find("PIPESTATUS");
  let const has_saved_pipe_statuses = current_pipe_statuses.has_value();
  ArrayList<String> saved_pipe_statuses{heap_allocator()};
  if (has_saved_pipe_statuses)
    saved_pipe_statuses = current_pipe_statuses->clone();

  let was_pipe_status_restored = false;
  defer
  {
    execution_store().last_exit_status() = saved_exit_status;

    if (!was_pipe_status_restored) {
      restore_trap_pipe_statuses(has_saved_pipe_statuses,
                                 steal(saved_pipe_statuses));
    }
  };

  for (i32 number = os::take_pending_signal(); number != 0;
       number = os::take_pending_signal())
  {
    let const name = os::signal_name_from_number(number);
    if (!name.has_value()) continue;
    if (name->view() == "CHLD") continue;
    if (trap_store().did_reset_inherited_signal_traps()) continue;

    if (let const trap = trap_store().find_active(name->view());
        trap.has_value())
    {
      let const action = trap->action_text.view();

      LOG(Info, "running the trap action for signal '%s'", name->c_str());
      let const cached_action = cached_trap_body(name->view(), action);

      let const definition = trap_store().find_definition(name->view());

      run_source(action, "the " + *name + " trap",
                 source_store().current_location(), TRAP_SOURCE_NAME, nullptr,
                 cached_action.has_value() ? &cached_action : nullptr,
                 return_handling::Reject, history_recording::Disabled,
                 definition.has_value() ? &*definition : nullptr);
    }

    if (control_flow_store().has_pending()) break;
  }

  let const child_bit = running_trap_bit(child_condition);
  if (trap_store().pending_child_trap_count() > 0 &&
      !trap_store().did_reset_inherited_signal_traps() &&
      !trap_store().is_condition_running(child_bit) &&
      os::has_reaped_child_arrival())
  {
    if (let const installed = trap_store().find_active(child_condition);
        installed.has_value())
    {
      let const action =
          String{heap_allocator(), installed->action_text.view()};
      let const fire_count = trap_store().pending_child_trap_count();

      trap_store().mark_condition_running(child_bit);
      defer { trap_store().unmark_condition_running(child_bit); };

      let const child_definition =
          trap_store().find_definition(child_condition);
      let const child_body = cached_trap_body(child_condition, action.view());
      let const *cached_child_body =
          child_body.has_value() ? &child_body : nullptr;

      u32 fired_count = 0;
      defer
      {
        if (fired_count > trap_store().pending_child_trap_count())
          trap_store().pending_child_trap_count() = 0;
        else
          trap_store().pending_child_trap_count() -= fired_count;

        os::clear_reaped_child_arrival();
      };

      for (; fired_count < fire_count; fired_count++) {
        if (control_flow_store().has_pending()) break;

        LOG(Info, "running the trap action for signal 'CHLD'");
        run_source(action.view(), "the CHLD trap",
                   source_store().current_location(), TRAP_SOURCE_NAME, nullptr,
                   cached_child_body, return_handling::Reject,
                   history_recording::Disabled,
                   child_definition.has_value() ? &*child_definition : nullptr);
      }
    }
  }

  restore_trap_pipe_statuses(has_saved_pipe_statuses,
                             steal(saved_pipe_statuses));
  was_pipe_status_restored = true;
}

cold fn EvalContext::run_exit_trap(Maybe<i32> final_status) throws -> void
{
  if (trap_store().exit_trap_ran()) return;
  trap_store().exit_trap_ran() = true;

  if (final_status.has_value())
    execution_store().last_exit_status() = *final_status;

  os::INTERRUPT_REQUESTED = 0;

  let const saved_exit_status = execution_store().last_exit_status();
  let const action_scope =
      TrapActionScope::enter(trap_store(), saved_exit_status);

  let const current_pipe_statuses =
      variable_store().indexed_arrays().find("PIPESTATUS");
  let const has_saved_pipe_statuses = current_pipe_statuses.has_value();
  ArrayList<String> saved_pipe_statuses{heap_allocator()};
  if (has_saved_pipe_statuses)
    saved_pipe_statuses = current_pipe_statuses->clone();

  let was_pipe_status_restored = false;
  defer
  {
    execution_store().last_exit_status() = saved_exit_status;

    if (!was_pipe_status_restored) {
      restore_trap_pipe_statuses(has_saved_pipe_statuses,
                                 steal(saved_pipe_statuses));
    }
  };

  if (let const trap = trap_store().find_active(StringView{"EXIT", 4});
      trap.has_value())
  {
    LOG(Info, "running the EXIT trap action at shell exit");
    let const definition = trap_store().find_definition(StringView{"EXIT", 4});

    run_source(trap->action_text.view(), "the EXIT trap", None,
               TRAP_SOURCE_NAME, nullptr, nullptr, return_handling::Reject,
               history_recording::Disabled,
               definition.has_value() ? &*definition : nullptr);
  }

  restore_trap_pipe_statuses(has_saved_pipe_statuses,
                             steal(saved_pipe_statuses));
  was_pipe_status_restored = true;
}

fn EvalContext::has_exit_trap() const wontthrow -> bool
{
  return trap_store().find_active(StringView{"EXIT", 4}).has_value();
}

fn EvalContext::clear_inherited_exit_trap() throws -> void
{
  if (runtime_state().is_bash_compatible()) {
    trap_store().hold_exit_trap_for_listing();
    return;
  }

  trap_store().reset(StringView{"EXIT", 4});
}

cold fn EvalContext::run_subshell_exit_trap() throws -> Maybe<i32>
{
  let const saved_exit_status = execution_store().last_exit_status();
  let const action_scope =
      TrapActionScope::enter(trap_store(), saved_exit_status);

  let const current_pipe_statuses =
      variable_store().indexed_arrays().find("PIPESTATUS");
  let const has_saved_pipe_statuses = current_pipe_statuses.has_value();
  ArrayList<String> saved_pipe_statuses{heap_allocator()};
  if (has_saved_pipe_statuses)
    saved_pipe_statuses = current_pipe_statuses->clone();

  let requested_status = Maybe<i32>{None};
  let was_pipe_status_restored = false;
  defer
  {
    execution_store().last_exit_status() =
        requested_status.has_value() ? *requested_status : saved_exit_status;

    if (!was_pipe_status_restored) {
      restore_trap_pipe_statuses(has_saved_pipe_statuses,
                                 steal(saved_pipe_statuses));
    }
  };

  if (let const trap = trap_store().find_active(StringView{"EXIT", 4});
      trap.has_value())
  {
    LOG(Info, "running the EXIT trap action the subshell set at its end");
    let const definition = trap_store().find_definition(StringView{"EXIT", 4});

    run_source(trap->action_text.view(), "the EXIT trap", None,
               TRAP_SOURCE_NAME, nullptr, nullptr, return_handling::Reject,
               history_recording::Disabled,
               definition.has_value() ? &*definition : nullptr);

    if (control_flow_store().has_pending() &&
        control_flow_store().pending().kind == control_flow::Kind::Exit)
    {
      requested_status = static_cast<i32>(control_flow_store().pending().value);
      control_flow_store().clear();
    }
  }

  restore_trap_pipe_statuses(has_saved_pipe_statuses,
                             steal(saved_pipe_statuses));
  was_pipe_status_restored = true;

  return requested_status;
}

fn EvalContext::is_implicitly_readonly(StringView name) const wontthrow -> bool
{
  if (runtime_state().bash_dynamic_variables_enabled() &&
      BASH_IMPLICIT_READONLY_NAMES.contains(name))
  {
    return true;
  }

  if (runtime_state().option_is_enabled(shell_option_id::Restricted)) {
    if (utils::environment_name_is_path(name)) return true;
    if (RESTRICTED_READONLY_NAMES.contains(name)) return true;
  }

  return false;
}

fn EvalContext::is_readonly(StringView name) const wontthrow -> bool
{
  if (is_implicitly_readonly(name)) return true;

  return variable_store().attributes().is_readonly(name);
}

fn EvalContext::readonly_names() const throws
    -> SortedArrayList<String, order_comparator<String>>
{
  let out = ArrayList<String>{heap_allocator()};
  out.reserve(countof(RESTRICTED_READONLY_KEYS) +
              countof(BASH_IMPLICIT_READONLY_KEYS));
  variable_store().attributes().for_each_marked(
      variable_attribute::Readonly,
      [&](StringView name) { out.push_managed(name); });

  let const do_push_implicit = [&](const PackedStringKey &key) throws {
    let name = key.to_string();
    if (!variable_store().attributes().is_readonly(name.view()))
      out.push(steal(name));
  };

  if (runtime_state().bash_dynamic_variables_enabled())
    for (let const &key : BASH_IMPLICIT_READONLY_KEYS)
      do_push_implicit(key);

  if (runtime_state().option_is_enabled(shell_option_id::Restricted))
    for (let const &key : RESTRICTED_READONLY_KEYS)
      do_push_implicit(key);

  return steal(out).make_sorted(sort_order::ascending);
}

fn EvalContext::is_implicitly_integer(StringView name) const wontthrow -> bool
{
  return runtime_state().bash_dynamic_variables_enabled() &&
         !is_dynamic_reader_unset(name) &&
         BASH_IMPLICIT_INTEGER_NAMES.contains(name);
}

fn EvalContext::is_integer_variable(StringView name) const wontthrow -> bool
{
  if (is_implicitly_integer(name)) return true;

  return variable_store().attributes().is_integer(name);
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
