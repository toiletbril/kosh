/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements mutable evaluator state, including shell options,
 * control flow, source frames, recursion limits, loop resources, status
 * values, Bash argument frames, snapshots, and restoration. It also owns the
 * validated bootstrap format used by fresh subshell evaluators. The split
 * keeps state transitions and transport serialization separate from
 * expression execution.
 */

#include "Builtin.hpp"
#include "CLI.hpp"
#include "Errors.hpp"
#include "Eval.hpp"
#include "Expressions.hpp"
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

fn EvalContext::set_shopt_option(StringView name, bool is_enabled) throws
    -> void
{
  let const index = shopt_option_index(name);
  ASSERT(index.has_value(), "unknown shopt option");
  let const was_enabled = is_shopt_enabled(name);
  runtime_state().set_shopt_option(*index, is_enabled);

  if (*index == shopt_option_index(shopt_option_id::Extdebug) && is_enabled &&
      !was_enabled && runtime_state().bash_dynamic_variables_enabled())
  {
    let const context = variable_store().bash_arguments().get_context();
    if (!variable_store().bash_arguments().is_active() && context != nullptr &&
        !context->has_flag(BashArgumentFrameFlag::DidEnter))
    {
      initialize_bash_argument_arrays(false);
      append_current_bash_argument_frame();
      context->set_flag(BashArgumentFrameFlag::DidEnter);
    } else {
      initialize_bash_argument_arrays(true);
    }
  }
}

fn EvalContext::enter_subshell() wontthrow -> void
{
  execution_store().subshell_depth()++;
  dynamic_runtime_store().is_random_reseed_pending() = true;
  LOG(Debug, "entered a subshell, depth now %zu",
      execution_store().subshell_depth());
}

fn EvalContext::set_subshell_depth(usize depth) wontthrow -> void
{
  execution_store().subshell_depth() = depth;
  if (depth > 0) dynamic_runtime_store().is_random_reseed_pending() = true;
  lower_trap_depths_to_current();
}

fn EvalContext::leave_subshell() wontthrow -> void
{
  ASSERT(execution_store().subshell_depth() > 0);
  while (!subshell_store().saved_descriptors().is_empty() &&
         subshell_store().saved_descriptors().back().depth ==
             execution_store().subshell_depth())
  {
    LOG(Debug, "restoring descriptor %d a subshell exec moved at depth %zu",
        subshell_store().saved_descriptors().back().saved.shell_fd,
        execution_store().subshell_depth());
    os::restore_descriptor(subshell_store().saved_descriptors().back().saved);
    subshell_store().saved_descriptors().remove(
        subshell_store().saved_descriptors().count() - 1);
  }
  execution_store().subshell_depth()--;
  lower_trap_depths_to_current();
  LOG(Debug, "left a subshell, depth now %zu",
      execution_store().subshell_depth());
}

fn EvalContext::snapshot_subshell_descriptor(i32 shell_fd) throws -> void
{
  if (execution_store().subshell_depth() == 0) return;
  for (let const &entry : subshell_store().saved_descriptors()) {
    if (entry.depth == execution_store().subshell_depth() &&
        entry.saved.shell_fd == shell_fd)
    {
      return;
    }
  }
  LOG(Debug,
      "backing up descriptor %d before a subshell exec moves it at depth %zu",
      shell_fd, execution_store().subshell_depth());
  subshell_store().saved_descriptors().push(
      subshell_saved_descriptor{execution_store().subshell_depth(),
                                os::save_descriptor_out_of_reach(shell_fd)});
}

fn EvalContext::set_coprocess_descriptors(i32 read_fd, i32 write_fd,
                                          i64 process_id,
                                          StringView name) throws -> void
{
  LOG(Debug, "the live coprocess is read on %d and written on %d", read_fd,
      write_fd);
  subshell_store().coprocess() = coprocess_descriptors{
      read_fd, write_fd, process_id, String{heap_allocator(), name}
  };
}

fn EvalContext::forget_coprocess_descriptor(i32 shell_fd) throws -> void
{
  let &coprocess = subshell_store().coprocess();
  if (shell_fd < 0 || coprocess.process_id < 0) return;

  if (coprocess.read_fd == shell_fd) {
    coprocess.read_fd = -1;
    set_array_element(coprocess.name.view(), 0, "-1");
  }
  if (coprocess.write_fd == shell_fd) {
    coprocess.write_fd = -1;
    set_array_element(coprocess.name.view(), 1, "-1");
  }
}

fn EvalContext::release_finished_coprocess() throws -> void
{
  let &coprocess = subshell_store().coprocess();
  if (coprocess.process_id < 0) return;

  let &table = job_table_store();
  table.update_jobs();
  for (let const &entry : table.jobs()) {
    if (entry.process_id == coprocess.process_id &&
        entry.state != job::State::Done)
    {
      return;
    }
  }
  for (let const process : table.detached_job_processes())
    if (os::process_has_id(process, coprocess.process_id)) return;

  LOG(Debug, "releasing the finished coprocess '%s'", coprocess.name.c_str());
  for (let const shell_fd : {coprocess.read_fd, coprocess.write_fd}) {
    if (shell_fd >= 0) unused(os::close_shell_fd(shell_fd));
  }

  let name = steal(coprocess.name);
  coprocess = coprocess_descriptors{};
  unset_shell_variable(name.view());
  name += "_PID";
  unset_shell_variable(name.view());
}

fn EvalContext::hide_coprocess_descriptors() throws -> void
{
  if (!subshell_store().coprocess().has_any()) return;

  LOG(Debug, "taking the coprocess descriptors away at subshell depth %zu",
      execution_store().subshell_depth());

  let const coprocess = subshell_store().coprocess();
  for (let const shell_fd : {coprocess.read_fd, coprocess.write_fd}) {
    if (shell_fd >= 0) snapshot_subshell_descriptor(shell_fd);
  }

  for (let const shell_fd : {coprocess.read_fd, coprocess.write_fd}) {
    if (shell_fd >= 0) unused(os::close_shell_fd(shell_fd));
  }

  subshell_store().coprocess() = coprocess_descriptors{};
}

pure fn EvalContext::in_subshell() const wontthrow -> bool
{
  return execution_store().subshell_depth() > 0;
}

pure fn EvalContext::can_replace_process() const wontthrow -> bool
{
  let const has_trap_to_run =
      trap_store().count() > 0 &&
      (!trap_store().did_reset_inherited_signal_traps() || has_exit_trap() ||
       trap_store().has_err_trap());

  return execution_store().terminal_exec_allowed() &&
         execution_store().subshell_depth() ==
             execution_store().get_terminal_exec_subshell_depth() &&
         !has_trap_to_run && !runtime_state().show_exit_code() &&
         !runtime_state().stats_enabled() &&
         !runtime_state().memory_stats_enabled();
}

fn EvalContext::request_loop_control(control_flow::Kind kind, i64 level,
                                     SourceLocation location) throws -> void
{
  if (execution_store().loop_depth() == 0) {
    LOG(Debug, "loop control requested outside a loop, ignored");
    return;
  }
  LOG(All, "loop control requested, level %lld of depth %zu", (long long) level,
      execution_store().loop_depth());
  control_flow_store().request_loop_control(
      kind, level, execution_store().loop_depth(), location,
      source_store().current_source(), source_store().current_origin());
}

fn EvalContext::request_break(i64 level, SourceLocation location) throws -> void
{
  request_loop_control(control_flow::Kind::Break, level, location);
}

fn EvalContext::request_continue(i64 level, SourceLocation location) throws
    -> void
{
  request_loop_control(control_flow::Kind::Continue, level, location);
}

fn EvalContext::request_return(i64 status, SourceLocation location) throws
    -> void
{
  LOG(Debug, "return requested, status %lld", (long long) status);
  trap_store().status_before_return() = execution_store().last_exit_status();
  control_flow_store().request_return(status, location,
                                      source_store().current_source(),
                                      source_store().current_origin());
}

fn EvalContext::request_exit(i64 status, SourceLocation location) throws -> void
{
  LOG(Debug, "exit requested, status %lld", (long long) status);
  control_flow_store().request_exit(status, location,
                                    source_store().current_source(),
                                    source_store().current_origin());
}

fn EvalContext::set_current_source(const String *source,
                                   String origin) wontthrow -> void
{
  reset_runtime_diagnostic_highlight_cache();
  source_store().set_current_source(source, steal(origin),
                                    scan_source_generation(source));
}

fn EvalContext::set_fresh_source(const String *source, String origin) wontthrow
    -> void
{
  set_current_source(source, steal(origin));
  source_store().current_location() = SourceLocation{};
}

SourceScope::SourceScope(EvalContext &context) wontthrow
    : m_context(&context),
      m_source(context.source_store().current_source()),
      m_origin(context.source_store().current_origin()),
      m_location(context.source_store().current_location())
{}

SourceScope::SourceScope(EvalContext &context, const String *source,
                         String origin) wontthrow : SourceScope(context)
{
  context.set_current_source(source, steal(origin));
}

fn SourceScope::restore() wontthrow -> void
{
  if (!m_is_armed) return;

  m_is_armed = false;
  m_context->set_current_source(m_source, steal(m_origin));
  m_context->source_store().current_location() = m_location;
}

fn EvalContext::push_root_source_frame(const String *parent_source,
                                       SourceLocation call_site,
                                       source_frame_kind kind) throws -> void
{
  ASSERT(kind != source_frame_kind::Ordinary);

  source_store().source_frames().push(source_frame{
      String{heap_allocator(), StringView{"the command line"}},
      call_site,
      parent_source, source_generation_for(parent_source),
      String{heap_allocator()},
      kind
  });
}

fn EvalContext::pop_root_source_frame() wontthrow -> void
{
  if (!source_store().source_frames().is_empty())
    source_store().source_frames().pop_back();
}

struct backtrace_entry
{
  SourceLocation location{};
  const String *text{nullptr};
  isize line_offset{0};
  bool *was_printed{nullptr};
  source_frame *frame{nullptr};
  usize call_index{0};
  usize repeat_count{0};
  bool is_definition{false};
  bool is_repeat{false};
};

fn EvalContext::print_source_backtrace(Maybe<SourceLocation> error_location,
                                       bool should_defer_for_source_file,
                                       bool is_replay) throws -> void
{
  if (!diagnostics_store().source_traces_enabled()) return;
  if (source_store().source_frames().is_empty() &&
      function_store().call_frames().is_empty())
  {
    return;
  }

  source_frame *deferring_frame = nullptr;
  if (should_defer_for_source_file) {
    for (usize i = source_store().source_frames().count(); i > 0; i--) {
      let &frame = source_store().source_frames()[i - 1];
      if (!frame.should_defer_trace) continue;
      if (error_location.has_value()) {
        let const error_source_name = error_location->get_filename();
        if (!error_source_name.has_value() ||
            *error_source_name != frame.source_path.view())
        {
          break;
        }
      }
      deferring_frame = &frame;
      break;
    }
  }

  let const do_location_match = [](SourceLocation left, SourceLocation right) {
    return left.has_same_source_as(right) && left.position == right.position &&
           left.length == right.length;
  };
  let const *error_source = source_store().current_source();
  let error_site = Maybe<rendered_site>{};
  if (error_location.has_value() && error_source != nullptr) {
    error_site = resolve_rendered_site(error_source->view(), *error_location, 0,
                                       false, this);
  }

  let const do_site_repeat_error = [&](SourceLocation site,
                                       const resolved_render_source &resolved) {
    if (!error_location.has_value()) return false;

    if (!error_site.has_value() || resolved.text == nullptr) {
      return do_location_match(site, *error_location);
    }

    let const rendered =
        resolve_rendered_site(resolved.text->view(), resolved.rebase(site),
                              resolved.line_offset, true, this);

    return rendered.source.data == error_site->source.data &&
           rendered.location.position == error_site->location.position &&
           rendered.location.length == error_site->location.length;
  };
  let const do_frame_identity_match = [&](const source_frame &left,
                                          const source_frame &right) {
    let const *left_source = borrowed_frame_source(left);
    let const *right_source = borrowed_frame_source(right);

    return left.kind == right.kind &&
           do_location_match(left.call_site, right.call_site) &&
           left.origin == right.origin &&
           left.source_path == right.source_path &&
           (left_source == right_source ||
            (left_source != nullptr && right_source != nullptr &&
             left_source->view() == right_source->view()));
  };
  let entries = ArrayList<backtrace_entry>{heap_allocator()};
  let const do_add_site = [&](backtrace_entry entry,
                              const resolved_render_source &resolved) {
    if (resolved.text == nullptr) return;

    entry.text = resolved.text;
    entry.line_offset = resolved.line_offset;
    entry.location = resolved.rebase(entry.location);
    if (entry.location.position > resolved.text->count()) return;

    entries.push(entry);
  };

  usize deferral_index = static_cast<usize>(-1);
  usize frame_index = source_store().source_frames().count();
  usize call_index = function_store().call_frames().count();
  while (frame_index > 0 || call_index > 0) {
    let const is_call_next =
        call_index > 0 &&
        (frame_index == 0 || call_index > source_store()
                                              .source_frames()[frame_index - 1]
                                              .function_call_depth);
    if (is_call_next) {
      call_index--;
      let &call_frame = function_store().call_frames()[call_index];
      let const call_site = call_frame.location;
      if (call_frame.source == nullptr) continue;

      let const resolved =
          resolve_render_source(call_site, call_frame.source, call_index,
                                source_depth_floor(frame_index));
      if (do_site_repeat_error(call_site, resolved)) continue;

      let entry = backtrace_entry{};
      entry.location = call_site;
      entry.call_index = call_index;
      entry.was_printed = &call_frame.was_printed;
      do_add_site(entry, resolved);
      continue;
    }

    frame_index--;
    let &frame = source_store().source_frames()[frame_index];
    if (&frame == deferring_frame) deferral_index = entries.count();

    if (frame.definition != nullptr) {
      let entry = backtrace_entry{};
      entry.location = frame.definition->location;
      entry.line_offset = frame.definition->line_offset;
      entry.text = &frame.definition->line_source;
      entry.was_printed = &frame.was_definition_printed;
      entry.frame = &frame;
      entry.is_definition = true;
      entries.push(entry);
    }

    let const *frame_source = borrowed_frame_source(frame);
    if (frame_source == nullptr) continue;

    let const resolved = resolve_render_source(frame.call_site, frame_source,
                                               frame.function_call_depth,
                                               source_depth_floor(frame_index));
    if (do_site_repeat_error(frame.call_site, resolved)) continue;

    let entry = backtrace_entry{};
    entry.location = frame.call_site;
    entry.frame = &frame;
    entry.was_printed = &frame.was_printed;
    do_add_site(entry, resolved);
  }

  if (entries.is_empty() && deferring_frame == nullptr) return;

  let const do_entries_match = [&](const backtrace_entry &left,
                                   const backtrace_entry &right) {
    if (left.is_definition != right.is_definition ||
        !do_location_match(left.location, right.location) ||
        left.line_offset != right.line_offset)
    {
      return false;
    }
    if (left.frame != nullptr && right.frame != nullptr) {
      if (!do_frame_identity_match(*left.frame, *right.frame)) return false;
    } else if (left.frame != right.frame) {
      return false;
    }

    return left.text == right.text || left.text->view() == right.text->view();
  };

  for (usize i = 0; i < entries.count(); i++) {
    if (entries[i].is_repeat) continue;

    for (usize other = i + 1; other < entries.count(); other++) {
      if (entries[other].is_repeat ||
          !do_entries_match(entries[i], entries[other]))
        continue;

      entries[other].is_repeat = true;
      entries[i].repeat_count++;
      *entries[other].was_printed = true;
    }
  }

  Maybe<u32> last_name_index = None;
  if (!is_replay && error_location.has_value()) {
    last_name_index = error_location->source_name_index;
    if (let const resolved = resolve_render_source(*error_location);
        resolved.is_windowed &&
        resolved.source_name_index != error_location->source_name_index)
    {
      last_name_index = resolved.source_name_index;
    } else if (error_location->source_name_index == 0) {
      if (let const embedded_name = embedded_source_name_index();
          embedded_name.has_value())
      {
        last_name_index = *embedded_name;
      }
    }
  }

  for (usize entry_index = 0; entry_index < entries.count(); entry_index++) {
    let const &entry = entries[entry_index];
    if (entry_index == deferral_index) break;
    if (entry.is_repeat || *entry.was_printed) continue;

    let const location = entry.location;
    let mapped_location = location;
    let mapped_text = entry.text->view();
    unused(map_embedded_site(mapped_text, mapped_location));
    let const mapped_name = source_name_at(mapped_location.source_name_index);
    let const is_eval_text = mapped_name.has_value() && *mapped_name == "eval";
    let const should_hide_filename =
        !is_eval_text && last_name_index.has_value() &&
        *last_name_index == mapped_location.source_name_index;
    last_name_index = mapped_location.source_name_index;

    let repeat_message = String{heap_allocator()};
    if (entry.repeat_count != 0) {
      repeat_message += "repeated ";
      repeat_message += String::from(entry.repeat_count + 1, heap_allocator());
      repeat_message += " times";
    }

    let trace = TraceWithLocation{location, repeat_message.view()};
    trace.set_line_offset(entry.line_offset);
    if (should_hide_filename) trace.hide_filename();
    show_message(trace.to_string(entry.text->view(), this));
    if (entry.is_definition) {
      utils::invalidate_line_number_cache_for(entry.text->view());
      reset_runtime_diagnostic_highlight_cache();
    }
    *entry.was_printed = true;
  }

  if (deferring_frame != nullptr) {
    deferring_frame->has_deferred_trace = true;
    deferring_frame->deferred_trace_location = error_location;
  }
}

static constexpr usize MAX_SOURCE_DEPTH = 400;
static constexpr usize MAX_FUNCTION_CALL_DEPTH = 900;
static constexpr usize MAX_PARAMETER_EXPANSION_DEPTH = 256;

static fn guard_located_depth(usize current_depth, usize cap,
                              maybeunused const char *what,
                              const SourceLocation &location) throws -> void
{
  if (current_depth >= cap) {
    LOG(Debug, "%s depth %zu exceeds cap %zu", what, current_depth, cap);
    throw ErrorWithLocation{location,
                            "Maximum source/recursion depth exceeded"};
  }
}

fn EvalContext::enter_source(const SourceLocation &location) throws -> void
{
  guard_located_depth(source_store().source_depth(), MAX_SOURCE_DEPTH, "source",
                      location);
  source_store().set_source_depth(source_store().source_depth() + 1);
}

fn EvalContext::leave_source() wontthrow -> void
{
  ASSERT(source_store().source_depth() > 0);
  source_store().set_source_depth(source_store().source_depth() - 1);
}

fn EvalContext::enter_function_call(const SourceLocation &location) throws
    -> void
{
  guard_located_depth(function_store().call_depth(), MAX_FUNCTION_CALL_DEPTH,
                      "function call", location);
  evaluation_metrics_store().add_function_run(runtime_state().stats_enabled());
  function_store().call_depth()++;
  LOG(Debug, "entered function call depth %zu", function_store().call_depth());
}

fn EvalContext::leave_function_call() wontthrow -> void
{
  ASSERT(function_store().call_depth() > 0);
  function_store().call_depth()--;
  lower_trap_depths_to_current();
}

fn EvalContext::enter_substitution() throws -> void
{
  if (expansion_store().substitution_depth() >=
      lexer::MAX_SUBSTITUTION_NESTING_DEPTH)
  {
    LOG(Debug, "substitution depth %zu exceeds cap %zu",
        expansion_store().substitution_depth(),
        lexer::MAX_SUBSTITUTION_NESTING_DEPTH);
    throw Error{"Command substitution nested too deeply"};
  }
  expansion_store().substitution_depth()++;
}

fn EvalContext::leave_substitution() wontthrow -> void
{
  ASSERT(expansion_store().substitution_depth() > 0);
  expansion_store().substitution_depth()--;
  lower_trap_depths_to_current();
}

fn EvalContext::enter_parameter_expansion() throws -> void
{
  if (expansion_store().parameter_expansion_depth() >=
      MAX_PARAMETER_EXPANSION_DEPTH)
  {
    LOG(Debug, "parameter expansion depth %zu exceeds cap %zu",
        expansion_store().parameter_expansion_depth(),
        MAX_PARAMETER_EXPANSION_DEPTH);
    throw Error{"Parameter expansion nested too deeply"};
  }
  expansion_store().parameter_expansion_depth()++;
}

fn EvalContext::leave_parameter_expansion() wontthrow -> void
{
  ASSERT(expansion_store().parameter_expansion_depth() > 0);
  expansion_store().parameter_expansion_depth()--;
}

fn EvalContext::enter_loop() wontthrow -> void
{
  execution_store().loop_depth()++;
}

fn EvalContext::leave_loop() wontthrow -> void
{
  ASSERT(execution_store().loop_depth() > 0);
  execution_store().loop_depth()--;
}

static constexpr usize MAX_LOOP_REDIRECT_FDS = 16;

fn EvalContext::mark_loop_redirect_fds() const wontthrow
    -> loop_redirect_fd_mark
{
  return {expansion_store().loop_redirect_fds().count()};
}

fn EvalContext::cleanup_loop_redirect_fds(loop_redirect_fd_mark mark) wontthrow
    -> void
{
  for (usize i = expansion_store().loop_redirect_fds().count(); i > mark.count;
       i--)
    os::close_fd(expansion_store().loop_redirect_fds()[i - 1].fd);

  while (expansion_store().loop_redirect_fds().count() > mark.count)
    expansion_store().loop_redirect_fds().remove(
        expansion_store().loop_redirect_fds().count() - 1);
}

fn EvalContext::find_loop_redirect_fd(i32 target_fd, const String &path,
                                      os::file_open_mode mode) const wontthrow
    -> Maybe<os::descriptor>
{
  for (let const &entry : expansion_store().loop_redirect_fds()) {
    if (entry.target_fd == target_fd && entry.mode == mode &&
        entry.path == path)
    {
      return entry.fd;
    }
  }

  return None;
}

fn EvalContext::retain_loop_redirect_fd(i32 target_fd, const String &path,
                                        os::file_open_mode mode,
                                        os::descriptor fd) throws -> bool
{
  if (expansion_store().loop_redirect_fds().count() >= MAX_LOOP_REDIRECT_FDS)
    return false;

  expansion_store().loop_redirect_fds().push(loop_redirect_fd{
      target_fd, mode, String{heap_allocator(), path.view()},
        fd
  });
  return true;
}

fn EvalContext::suggest_similar_variable_name(StringView name) const throws
    -> Maybe<String>
{
  if (name.is_empty()) return None;

  let suggestion = utils::NameSuggestion{name};
  variable_store().shell_variables().for_each(
      [&suggestion](StringView candidate, const String &)
          throws -> void { suggestion.consider(candidate); });
  variable_store().indexed_arrays().for_each(
      [&suggestion](StringView candidate, const ArrayList<String> &)
          throws -> void { suggestion.consider(candidate); });
  variable_store().associative_arrays().names().for_each(
      [&suggestion](StringView candidate)
          throws -> void { suggestion.consider(candidate); });
  variable_store().exported_names().for_each(
      [&suggestion](StringView key, const auto &display_name) throws -> void {
        if constexpr (os::ENVIRONMENT_IS_CASE_SENSITIVE) {
          unused(display_name);
          suggestion.consider(key);
        } else {
          suggestion.consider(display_name.is_empty() ? key
                                                      : display_name.view());
        }
      });

  let dynamic_names = ArrayList<StringView>{heap_allocator()};
  append_dynamic_variable_names(dynamic_names);
  for (let const dynamic_name : dynamic_names)
    suggestion.consider(dynamic_name);

  return suggestion.take_suggestion();
}

fn EvalContext::sorted_variable_assignments() const throws
    -> SortedArrayList<String, order_comparator<String>>
{
  let assignments = ArrayList<String>{heap_allocator()};
  assignments.reserve(variable_store().shell_variables().count());
  variable_store().shell_variables().for_each(
      [&](StringView name, const String &value) {
        let entry = String{heap_allocator(), name};
        entry.push('=');
        entry.append(value);
        assignments.push(steal(entry));
      });
  return steal(assignments).make_sorted(sort_order::ascending);
}

fn EvalContext::snapshot_state() throws -> eval_state_snapshot
{
  let working_directory = os::reference_current_directory();
  if (!working_directory.is_valid())
    throw Error{"Could not preserve the current working directory"};

  return eval_state_snapshot{variable_store().snapshot(),
                             completion_store().snapshot(),
                             function_store().snapshot(),
                             scope_store().snapshot(),
                             execution_store().snapshot(),
                             trap_store().snapshot(),
                             runtime_control_store().snapshot(),
                             dynamic_runtime_store().get_clock(),
                             job_table_store().take_snapshot(),
                             expansion_store().get_getopts_cursor(),
                             subshell_store().coprocess(),
                             environment_store().snapshot(),
                             RuntimeState::capture(*this),
                             program_resolver(),
                             steal(working_directory),
                             os::get_file_creation_mask()};
}

fn VariableStore::snapshot() const throws -> variable_snapshot
{
  return variable_snapshot{
      m_variables,
      m_special_variable_definition_locations,
      m_associative_arrays,
      m_sparse_arrays,
      m_positional_params,
      m_directory_stack,
      m_exported_names,
      static_cast<u32>(m_bash_arguments.values().count()),
      static_cast<u32>(m_bash_arguments.frame_counts().count()),
      m_bash_arguments.is_active(),
      m_bash_arguments.get_context() != nullptr
          ? m_bash_arguments.get_context()->flags
          : u8{0},
      m_disabled_bash_special_arrays,
      m_unset_dynamic_readers};
}

fn VariableStore::restore(variable_snapshot snapshot) throws -> void
{
  m_variables = steal(snapshot.variables);
  m_is_pipestatus_scalar_possible = true;
  m_special_variable_definition_locations =
      steal(snapshot.special_variable_definition_locations);
  m_associative_arrays = steal(snapshot.associative_arrays);
  m_sparse_arrays = steal(snapshot.sparse_arrays);
  m_positional_params = steal(snapshot.positional_params);
  if (!snapshot.had_bash_argument_arrays) {
    m_bash_arguments.reset();
  } else {
    m_bash_arguments.truncate_to(snapshot.bash_argument_value_count,
                                 snapshot.bash_argument_frame_count);
  }
  if (m_bash_arguments.get_context() != nullptr)
    m_bash_arguments.get_context()->flags =
        snapshot.bash_argument_frame_context_flags;
  m_directory_stack = steal(snapshot.directory_stack);
  m_disabled_bash_special_arrays = snapshot.disabled_bash_special_arrays;
  m_unset_dynamic_readers = snapshot.unset_dynamic_readers;
  m_exported_names = steal(snapshot.exported_names);

  if (let const ifs = shell_variables().find(StringView{"IFS", 3});
      ifs.has_value())
    set_field_separators(ifs->view());
  else
    set_field_separators(" \t\n");
}

fn EnvironmentStore::restore(usize undo_mark) wontthrow -> void
{
  while (m_environment_undo_log.count() > undo_mark) {
    let const &entry = m_environment_undo_log.back();
    if (entry.previous_value)
      os::set_environment_variable(entry.name.view(),
                                   entry.previous_value->view());
    else
      os::unset_environment_variable(entry.name.view());
    m_environment_undo_log.pop_back();
  }
}

fn EvalContext::restore_state(eval_state_snapshot snapshot) throws -> void
{
  LOG(Debug, "restoring the evaluator state after a subshell or substitution");
  variable_store().restore(steal(snapshot.variables));
  completion_store().restore(steal(snapshot.completion));
  function_store().restore(steal(snapshot.functions));
  scope_store().restore(steal(snapshot.scopes));
  execution_store().restore(steal(snapshot.execution));
  snapshot.runtime.restore(*this);
  program_resolver() = steal(snapshot.program_resolver);
  runtime_control_store().restore(snapshot.control);
  dynamic_runtime_store().set_clock(snapshot.clock);
  job_table_store().restore_snapshot(steal(snapshot.jobs));
  expansion_store().set_getopts_cursor(snapshot.getopts);
  subshell_store().coprocess() = snapshot.coprocess;

  if (trap_store().count() != 0 || snapshot.traps.traps.count() != 0) {
    trap_store().list([&](StringView condition, const trap_definition &trap) {
      unused(trap);
      if (condition == "EXIT") return;
      if (snapshot.traps.traps.find(condition).has_value()) return;
      if (let const number = os::signal_number_from_name(condition))
        os::clear_trap_handler(*number);
    });
    trap_store().restore(steal(snapshot.traps));
    install_trap_dispositions();
  } else {
    trap_store().restore(steal(snapshot.traps));
  }

  if (!os::restore_current_directory(snapshot.working_directory))
    LOG(Debug, "the subshell could not restore the working directory");
  os::set_file_creation_mask(snapshot.file_creation_mask);

  LOG(Debug, "rewinding %zu environment writes made inside the subshell",
      environment_store().environment_undo_log().count() -
          snapshot.environment_undo_mark);
  environment_store().restore(snapshot.environment_undo_mark);
}

static constexpr u32 SUBSHELL_BOOTSTRAP_MAGIC = 0x4b534842U;
static constexpr u32 SUBSHELL_BOOTSTRAP_VERSION = 21U;
static constexpr u32 NO_BOOTSTRAP_PROCESS = UINT32_MAX;
static constexpr u32 NO_BARE_PROGRAM_PATH = UINT32_MAX;

static fn append_subshell_bootstrap_u32(String &output, u32 value) throws
    -> void
{
  for (usize byte_position = 0; byte_position < sizeof(value); byte_position++)
    output.push(static_cast<char>((value >> (byte_position * 8U)) & 0xffU));
}

static fn append_subshell_bootstrap_u64(String &output, u64 value) throws
    -> void
{
  for (usize byte_position = 0; byte_position < sizeof(value); byte_position++)
    output.push(static_cast<char>((value >> (byte_position * 8U)) & 0xffU));
}

static fn append_subshell_bootstrap_i32(String &output, i32 value) throws
    -> void
{
  u32 bits = 0;
  __builtin_memcpy(&bits, &value, sizeof(bits));
  append_subshell_bootstrap_u32(output, bits);
}

static fn append_subshell_bootstrap_i64(String &output, i64 value) throws
    -> void
{
  u64 bits = 0;
  __builtin_memcpy(&bits, &value, sizeof(bits));
  append_subshell_bootstrap_u64(output, bits);
}

static fn append_subshell_bootstrap_text(String &output, StringView text) throws
    -> void
{
  if (text.length > UINT32_MAX) throw std::bad_alloc{};
  append_subshell_bootstrap_u32(output, static_cast<u32>(text.length));
  output.append(text);
}

enum class wire_section : u8
{
  Execution = 1,
  Jobs,
  Clock,
  Getopts,
  Runtime,
  Variables,
  Startup,
  Functions,
  Scopes,
  Completion,
  Traps,
  Control,
  Programs,
  Origin,
  Diagnostics,
};

enum class local_binding_wire_flag : u8
{
  HasValue = 1U << 0,
  HasIndexedArray = 1U << 1,
  WasAssociative = 1U << 2,
  WasExported = 1U << 3,
  IsSelfReference = 1U << 4,
};

static constexpr u8 ALL_LOCAL_BINDING_WIRE_FLAGS = (1U << 5) - 1U;

enum class inherited_frame_wire_flag : u8
{
  HasSite = 1U << 0,
  WasPrinted = 1U << 1,
  IsSourceChanging = 1U << 2,
  ShouldDeferTrace = 1U << 3,
};

static constexpr u8 ALL_INHERITED_FRAME_WIRE_FLAGS = (1U << 4) - 1U;

template <class WriteFn>
static fn append_wire_section(String &output, wire_section section,
                              WriteFn do_write) throws -> void
{
  let payload = String{heap_allocator()};
  do_write(payload);
  if (payload.count() > UINT32_MAX) throw std::bad_alloc{};
  output.push(static_cast<char>(section));
  append_subshell_bootstrap_u32(output, static_cast<u32>(payload.count()));
  output.append(payload.view());
}

static fn find_wire_source_name(ArrayList<u32> &names, u32 name_index) throws
    -> u32
{
  if (name_index == 0) return 0;
  if (let const found = names.find(name_index); found.has_value()) {
    return static_cast<u32>(*found + 1);
  }

  names.push(name_index);
  return static_cast<u32>(names.count());
}

static fn append_wire_source_names(String &output,
                                   const ArrayList<u32> &names) throws -> void
{
  append_subshell_bootstrap_u32(output, static_cast<u32>(names.count()));
  for (let const name_index : names) {
    let const kind = source_identity_kind_at(name_index);
    output.push(static_cast<char>(kind));
    if (kind == source_identity_kind::File)
      append_subshell_bootstrap_text(
          output, source_name_at(name_index).value_or(StringView{}));
  }
}

fn dynamic_clock_state::append_wire(String &output) const throws -> void
{
  append_wire_section(output, wire_section::Clock, [&](String &payload) {
    append_subshell_bootstrap_u64(payload, random_state);
    append_subshell_bootstrap_i64(payload, shell_start_time);
    append_subshell_bootstrap_i64(payload, seconds_base);
  });
}

fn getopts_cursor::append_wire(String &output) const throws -> void
{
  append_wire_section(output, wire_section::Getopts, [&](String &payload) {
    append_subshell_bootstrap_u64(payload, static_cast<u64>(char_index));
    append_subshell_bootstrap_i64(payload, last_optind);
  });
}

fn definition_state::append_wire(String &output) const throws -> void
{
  output.push(static_cast<char>(mood));
  output.push(static_cast<char>(reporting.warning_level));
  output.push(static_cast<char>(reporting.is_annoying_disabled));
  output.push(static_cast<char>(reporting.is_diagnostics_disabled));
}

fn RuntimeState::append_wire(String &output) const throws -> void
{
  append_wire_section(output, wire_section::Runtime, [&](String &payload) {
    payload.push(static_cast<char>(m_mood));
    payload.push(static_cast<char>(m_reporting.warning_level));
    payload.push(static_cast<char>(m_tab_selector));
    payload.push(static_cast<char>(get_wire_flags()));
    append_subshell_bootstrap_u64(payload, m_shell_options);
    append_subshell_bootstrap_u64(payload, m_shopt.overrides);
    append_subshell_bootstrap_u64(payload, m_shopt.values);
  });
}

static constexpr u8 VALID_MOOD_MASK =
    static_cast<u8>((1U << (static_cast<u8>(mimic_mood::BashPosix) + 1U)) - 1U);
static constexpr u32 VALID_SUPPRESSED_WARNING_MASK =
    (u32{1} << (static_cast<u32>(suppressible_warning::UnsetTestOperand) +
                1U)) -
    1U;
static_assert(VALID_SUPPRESSED_WARNING_MASK <= UINT8_MAX);

fn RuntimeControlStore::append_wire(String &output) const throws -> void
{
  ASSERT((m_suppressed_warnings & ~VALID_SUPPRESSED_WARNING_MASK) == 0);
  append_wire_section(output, wire_section::Control, [&](String &payload) {
    payload.push(static_cast<char>(m_init_moods_sourcing));
    payload.push(static_cast<char>(m_initialized_moods));
    payload.push(static_cast<char>(m_suppressed_warnings));
  });
}

fn ExecutionStore::append_wire(String &output) const throws -> void
{
  append_wire_section(output, wire_section::Execution, [&](String &payload) {
    payload.push(static_cast<char>(m_execution_string.has_value()));
    if (m_execution_string.has_value())
      append_subshell_bootstrap_text(payload, m_execution_string->view());
    append_subshell_bootstrap_text(payload, m_last_argument.view());
  });
}

fn VariableStore::append_wire(String &output) const throws -> void
{
  append_wire_section(output, wire_section::Variables, [&](String &payload) {
    payload.push(static_cast<char>(m_disabled_bash_special_arrays));
    payload.push(static_cast<char>(m_unset_dynamic_readers));
    payload.push(static_cast<char>(m_bash_arguments.is_active()));
    if (m_bash_arguments.is_active()) {
      append_subshell_bootstrap_u32(
          payload, static_cast<u32>(m_bash_arguments.frame_counts().count()));
      for (let const argument_count : m_bash_arguments.frame_counts())
        append_subshell_bootstrap_u32(payload, argument_count);
      append_subshell_bootstrap_u32(
          payload, static_cast<u32>(m_bash_arguments.values().count()));
      for (let const &argument : m_bash_arguments.values())
        append_subshell_bootstrap_text(payload, argument.view());
    } else {
      append_subshell_bootstrap_u32(payload, 0);
      append_subshell_bootstrap_u32(payload, 0);
    }

    let const *context = m_bash_arguments.get_context();
    payload.push(static_cast<char>(context != nullptr));
    if (context != nullptr) {
      payload.push(static_cast<char>(context->flags));
      append_subshell_bootstrap_text(payload, context->source_path);
    }
  });
}

fn StartupStore::append_wire(String &output) const throws -> void
{
  append_wire_section(output, wire_section::Startup, [&](String &payload) {
    payload.push(static_cast<char>(m_is_restricted_shell));
    payload.push(static_cast<char>(m_is_login_shell));
    append_subshell_bootstrap_text(payload, m_init_moods.view());
  });
}

fn DiagnosticsStore::append_wire(String &output) const throws -> void
{
  append_wire_section(output, wire_section::Diagnostics, [&](String &payload) {
    payload.push(static_cast<char>(m_source_traces_enabled));
  });
}

fn FunctionStore::append_wire(String &output) const throws -> void
{
  append_wire_section(output, wire_section::Functions, [&](String &payload) {
    append_subshell_bootstrap_u64(payload, static_cast<u64>(m_call_depth));
    append_subshell_bootstrap_u32(payload,
                                  static_cast<u32>(m_call_frames.count()));
    for (let const &call_frame : m_call_frames)
      append_subshell_bootstrap_text(payload, call_frame.name.view());

    let names = ArrayList<u32>{heap_allocator()};
    let records = String{heap_allocator()};
    u32 record_count = 0;
    m_definitions.for_each([&](StringView name,
                               const FunctionBodyHandle &storage) throws {
      let const *info = storage.get_definition_info();
      let const *source = storage.get_source();
      if (info == nullptr || source == nullptr || source->is_empty() ||
          info->line_offset < 0 || info->line_offset > INT32_MAX ||
          info->enclosing_line_count > INT32_MAX ||
          info->definition_line > UINT32_MAX)
      {
        return;
      }

      append_subshell_bootstrap_text(records, name);
      append_subshell_bootstrap_u32(
          records, find_wire_source_name(names, info->source_name_index));
      append_subshell_bootstrap_u32(records,
                                    static_cast<u32>(info->line_offset));
      append_subshell_bootstrap_u32(
          records, static_cast<u32>(info->enclosing_line_count));
      append_subshell_bootstrap_u32(records,
                                    static_cast<u32>(info->definition_line));
      append_subshell_bootstrap_text(records, info->line_prefix.view());
      append_subshell_bootstrap_text(records, info->line_suffix.view());
      record_count++;
    });
    append_wire_source_names(payload, names);
    append_subshell_bootstrap_u32(payload, record_count);
    payload.append(records.view());
  });
}

fn ScopeStore::append_wire(String &output) const throws -> void
{
  append_wire_section(output, wire_section::Scopes, [&](String &payload) {
    append_subshell_bootstrap_u32(payload,
                                  static_cast<u32>(m_local_scope_depth));
    for (usize depth = 0; depth < m_local_scope_depth; depth++) {
      let const &frame = m_local_scopes[depth];
      append_subshell_bootstrap_u32(payload, static_cast<u32>(frame.count()));
      for (let const &binding : frame) {
        u8 flags = 0;
        if (binding.previous_value.has_value())
          flags |= static_cast<u8>(local_binding_wire_flag::HasValue);
        if (binding.previous_indexed_array.has_value())
          flags |= static_cast<u8>(local_binding_wire_flag::HasIndexedArray);
        if (binding.previous_was_associative)
          flags |= static_cast<u8>(local_binding_wire_flag::WasAssociative);
        if (binding.previous_was_exported)
          flags |= static_cast<u8>(local_binding_wire_flag::WasExported);
        if (binding.is_self_reference)
          flags |= static_cast<u8>(local_binding_wire_flag::IsSelfReference);

        append_subshell_bootstrap_text(payload, binding.name.view());
        payload.push(static_cast<char>(flags));
        payload.push(static_cast<char>(binding.previous_attributes));
        if (binding.previous_value.has_value())
          append_subshell_bootstrap_text(payload,
                                         binding.previous_value->view());
        if (binding.previous_indexed_array.has_value()) {
          append_subshell_bootstrap_u32(
              payload,
              static_cast<u32>(binding.previous_indexed_array->count()));
          for (let const &element : *binding.previous_indexed_array)
            append_subshell_bootstrap_text(payload, element.view());
        }
        if (binding.previous_was_associative) {
          ASSERT(binding.previous_associative_keys.count() ==
                 binding.previous_associative_values.count());
          append_subshell_bootstrap_u32(
              payload,
              static_cast<u32>(binding.previous_associative_keys.count()));
          for (usize index = 0;
               index < binding.previous_associative_keys.count(); index++)
          {
            append_subshell_bootstrap_text(
                payload, binding.previous_associative_keys[index].view());
            append_subshell_bootstrap_text(
                payload, binding.previous_associative_values[index].view());
          }
        }
        ASSERT(binding.previous_sparse_indices.count() ==
               binding.previous_sparse_values.count());
        append_subshell_bootstrap_u32(
            payload, static_cast<u32>(binding.previous_sparse_indices.count()));
        for (usize index = 0; index < binding.previous_sparse_indices.count();
             index++)
        {
          append_subshell_bootstrap_u64(
              payload,
              static_cast<u64>(binding.previous_sparse_indices[index]));
          append_subshell_bootstrap_text(
              payload, binding.previous_sparse_values[index].view());
        }
      }
    }
  });
}

fn TrapStore::append_wire(String &output) const throws -> void
{
  append_wire_section(output, wire_section::Traps, [&](String &payload) {
    append_subshell_bootstrap_u64(payload, m_startup_ignored_signals);
  });
}

fn ProgramResolver::append_wire(String &output) const throws -> void
{
  append_wire_section(output, wire_section::Programs, [&](String &payload) {
    append_subshell_bootstrap_u32(payload,
                                  static_cast<u32>(m_execution_cache.count()));
    m_execution_cache.for_each(
        [&](StringView name, const CacheEntry &entry) throws {
          append_subshell_bootstrap_text(payload, name);
          append_subshell_bootstrap_u32(
              payload, entry.bare_path_position.has_value()
                           ? static_cast<u32>(*entry.bare_path_position)
                           : NO_BARE_PROGRAM_PATH);
          append_subshell_bootstrap_u32(payload,
                                        static_cast<u32>(entry.paths.count()));
          for (let const &cached : entry.paths) {
            payload.push(static_cast<char>(cached.extension));
            append_subshell_bootstrap_text(payload, cached.path.view());
          }
        });
  });
}

fn CompletionStore::append_wire(String &output) const throws -> void
{
  append_wire_section(output, wire_section::Completion, [&](String &payload) {
    let collected_names = ArrayList<String>{heap_allocator()};
    m_specs.for_each([&](StringView command, const completion_spec &) {
      collected_names.push_managed(command);
    });
    let const names = steal(collected_names).make_sorted(sort_order::ascending);
    append_subshell_bootstrap_u32(payload, static_cast<u32>(names.count()));

    let const do_append_spec = [&](const completion_spec &spec) throws -> void {
      append_subshell_bootstrap_text(payload, spec.function_name.view());
      append_subshell_bootstrap_text(payload, spec.word_list.view());
      append_subshell_bootstrap_text(payload, spec.glob_pattern.view());
      append_subshell_bootstrap_text(payload, spec.filter_pattern.view());
      append_subshell_bootstrap_text(payload, spec.prefix.view());
      append_subshell_bootstrap_text(payload, spec.suffix.view());
      append_subshell_bootstrap_text(payload, spec.command.view());
      append_subshell_bootstrap_u32(payload, spec.action_mask);
      append_subshell_bootstrap_u32(payload, spec.option_mask);
      append_subshell_bootstrap_u32(payload, spec.argument_mask);
      spec.defining_state.append_wire(payload);
    };

    for (let const &command : names) {
      let const spec = m_specs.find(command.view());
      ASSERT(spec.has_value());
      append_subshell_bootstrap_text(payload, command.view());
      do_append_spec(*spec.value());
    }

    for (let const *slot_spec :
         {&m_default_spec, &m_empty_spec, &m_initial_spec})
    {
      payload.push(static_cast<char>(slot_spec->has_value()));
      if (slot_spec->has_value()) do_append_spec(**slot_spec);
    }
  });
}

fn JobTable::append_wire(String &output,
                         os::subshell_bootstrap &bootstrap) const throws -> void
{
  append_wire_section(output, wire_section::Jobs, [&](String &payload) {
    let const do_reference_process = [&](os::process process) throws -> u32 {
      if (bootstrap.processes.count() >= UINT32_MAX) throw std::bad_alloc{};
      let const process_index = static_cast<u32>(bootstrap.processes.count());
      bootstrap.processes.push(process);
      return process_index;
    };

    payload.push(static_cast<char>(m_last_background_pid.has_value()));
    if (m_last_background_pid.has_value())
      append_subshell_bootstrap_i64(payload, *m_last_background_pid);
    append_subshell_bootstrap_i32(payload, m_next_job_id);

    append_subshell_bootstrap_u32(payload, static_cast<u32>(m_jobs.count()));
    for (let const &child_job : m_jobs) {
      append_subshell_bootstrap_i32(payload, child_job.id);
      append_subshell_bootstrap_text(payload, child_job.command.view());
      append_subshell_bootstrap_i64(payload, child_job.process_id);
      append_subshell_bootstrap_i64(payload, child_job.process_group_id);
      append_subshell_bootstrap_i32(payload, child_job.last_status);
      append_subshell_bootstrap_i32(payload, child_job.stopped_status);
      append_subshell_bootstrap_i32(payload,
                                    child_job.termination.signal_number);
      payload.push(static_cast<char>(child_job.termination.did_dump_core));
      payload.push(static_cast<char>(child_job.state));
      payload.push(static_cast<char>(child_job.is_primary_process_active));
      payload.push(static_cast<char>(child_job.has_unreported_state_change));
      append_subshell_bootstrap_u32(payload,
                                    child_job.is_primary_process_active
                                        ? do_reference_process(child_job.pid)
                                        : NO_BOOTSTRAP_PROCESS);
      append_subshell_bootstrap_u32(
          payload,
          static_cast<u32>(child_job.earlier_pipeline_processes.count()));
      for (let const process : child_job.earlier_pipeline_processes)
        append_subshell_bootstrap_u32(payload, do_reference_process(process));
    }

    append_subshell_bootstrap_u32(
        payload, static_cast<u32>(m_detached_job_processes.count()));
    for (let const process : m_detached_job_processes)
      append_subshell_bootstrap_u32(payload, do_reference_process(process));
  });
}

struct subshell_bootstrap_reader
{
  StringView bytes;
  usize position{0};
  bool is_valid{true};

  pure fn get_remaining_length() const wontthrow -> usize
  {
    return position <= bytes.length ? bytes.length - position : 0;
  }

  pure fn is_fully_read() const wontthrow -> bool
  {
    return is_valid && position == bytes.length;
  }

  fn read_section(wire_section section,
                  subshell_bootstrap_reader &payload) wontthrow -> bool
  {
    let const identity = read_u8();
    let const length = static_cast<usize>(read_u32());
    if (!is_valid || identity != static_cast<u8>(section) ||
        length > get_remaining_length())
    {
      is_valid = false;
      return false;
    }

    payload =
        subshell_bootstrap_reader{bytes.substring_of_length(position, length)};
    position += length;
    return true;
  }

  fn read_u8() wontthrow -> u8
  {
    if (!is_valid || get_remaining_length() < 1) {
      is_valid = false;
      return 0;
    }

    return static_cast<u8>(bytes[position++]);
  }

  fn read_u32() wontthrow -> u32
  {
    if (!is_valid || get_remaining_length() < sizeof(u32)) {
      is_valid = false;
      return 0;
    }

    u32 value = 0;
    for (usize byte_position = 0; byte_position < sizeof(value);
         byte_position++)
      value |= static_cast<u32>(static_cast<u8>(bytes[position++]))
               << (byte_position * 8U);

    return value;
  }

  fn read_u64() wontthrow -> u64
  {
    if (!is_valid || get_remaining_length() < sizeof(u64)) {
      is_valid = false;
      return 0;
    }

    u64 value = 0;
    for (usize byte_position = 0; byte_position < sizeof(value);
         byte_position++)
      value |= static_cast<u64>(static_cast<u8>(bytes[position++]))
               << (byte_position * 8U);

    return value;
  }

  fn read_i32() wontthrow -> i32
  {
    let const bits = read_u32();
    i32 value = 0;
    __builtin_memcpy(&value, &bits, sizeof(value));
    return value;
  }

  fn read_i64() wontthrow -> i64
  {
    let const bits = read_u64();
    i64 value = 0;
    __builtin_memcpy(&value, &bits, sizeof(value));
    return value;
  }

  fn read_text() wontthrow -> StringView
  {
    let const text_length = static_cast<usize>(read_u32());
    if (!is_valid || text_length > get_remaining_length()) {
      is_valid = false;
      return {};
    }

    let const text = bytes.substring_of_length(position, text_length);
    position += text_length;
    return text;
  }
};

static fn read_subshell_bootstrap_bool(subshell_bootstrap_reader &reader,
                                       bool &value) wontthrow -> bool
{
  let const encoded = reader.read_u8();
  if (!reader.is_valid || encoded > 1) return false;
  value = encoded != 0;
  return true;
}

static fn read_wire_source_names(subshell_bootstrap_reader &reader,
                                 ArrayList<u32> &names) throws -> bool
{
  let const name_count = static_cast<usize>(reader.read_u32());
  if (!reader.is_valid || name_count > reader.get_remaining_length()) {
    return false;
  }

  names.reserve(name_count);
  for (usize index = 0; index < name_count; index++) {
    let const kind = reader.read_u8();
    if (kind == static_cast<u8>(source_identity_kind::CommandString)) {
      names.push(intern_source_name(COMMAND_STRING_SOURCE_NAME));
      continue;
    }

    let const name = reader.read_text();
    if (kind != static_cast<u8>(source_identity_kind::File) ||
        !reader.is_valid || name.is_empty())
    {
      return false;
    }

    names.push(intern_source_name(name));
  }

  return reader.is_valid;
}

static fn read_wire_source_name(subshell_bootstrap_reader &reader,
                                const ArrayList<u32> &names,
                                u32 &name_index) wontthrow -> bool
{
  let const position = static_cast<usize>(reader.read_u32());
  if (!reader.is_valid || position > names.count()) {
    return false;
  }

  name_index = position == 0 ? 0 : names[position - 1];
  return true;
}

fn dynamic_clock_state::from_wire(subshell_bootstrap_reader &reader,
                                  dynamic_clock_state &clock) wontthrow -> bool
{
  subshell_bootstrap_reader payload;
  if (!reader.read_section(wire_section::Clock, payload)) return false;

  clock.random_state = payload.read_u64();
  clock.shell_start_time = payload.read_i64();
  clock.seconds_base = payload.read_i64();
  return payload.is_fully_read();
}

fn getopts_cursor::from_wire(subshell_bootstrap_reader &reader,
                             getopts_cursor &cursor) wontthrow -> bool
{
  subshell_bootstrap_reader payload;
  if (!reader.read_section(wire_section::Getopts, payload)) return false;

  let const char_index_bits = payload.read_u64();
  cursor.last_optind = payload.read_i64();
  if (!payload.is_fully_read() || char_index_bits == 0 ||
      char_index_bits > SIZE_MAX)
  {
    return false;
  }

  cursor.char_index = static_cast<usize>(char_index_bits);
  return true;
}

fn definition_state::from_wire(subshell_bootstrap_reader &reader,
                               definition_state &state) wontthrow -> bool
{
  let const mood = reader.read_u8();
  let const warning_level = reader.read_u8();
  bool is_annoying_disabled = false;
  bool is_diagnostics_disabled = false;
  if (!reader.is_valid || mood > static_cast<u8>(mimic_mood::BashPosix) ||
      warning_level > 3 ||
      !read_subshell_bootstrap_bool(reader, is_annoying_disabled) ||
      !read_subshell_bootstrap_bool(reader, is_diagnostics_disabled))
  {
    return false;
  }

  state.mood = static_cast<mimic_mood>(mood);
  state.reporting = {warning_level, is_annoying_disabled,
                     is_diagnostics_disabled};
  return true;
}

fn RuntimeState::from_wire(subshell_bootstrap_reader &reader,
                           RuntimeState &runtime) wontthrow -> bool
{
  subshell_bootstrap_reader payload;
  if (!reader.read_section(wire_section::Runtime, payload)) return false;

  let const mood = payload.read_u8();
  let const warning_level = payload.read_u8();
  let const tab_selector = payload.read_u8();
  let const flags = payload.read_u8();
  let const shell_options = payload.read_u64();
  let const shopt = shopt_state{payload.read_u64(), payload.read_u64()};
  static_assert(static_cast<u8>(shell_option_id::Count) < 64);
  let const valid_shell_options =
      (u64{1} << static_cast<u8>(shell_option_id::Count)) - 1U;
  if (!payload.is_fully_read() ||
      mood > static_cast<u8>(mimic_mood::BashPosix) || warning_level > 3 ||
      tab_selector > static_cast<u8>(tab_selector_mode::Plain) ||
      (flags & ~ALL_FLAGS) != 0 ||
      (shell_options & ~valid_shell_options) != 0 || !shopt.is_valid())
  {
    return false;
  }

  runtime.m_mood = static_cast<mimic_mood>(mood);
  runtime.m_reporting.warning_level = warning_level;
  runtime.m_tab_selector = static_cast<tab_selector_mode>(tab_selector);
  runtime.set_wire_flags(flags);
  runtime.m_shell_options = shell_options;
  runtime.m_shopt = shopt;
  return true;
}

fn RuntimeControlStore::from_wire(subshell_bootstrap_reader &reader,
                                  runtime_control_wire &wire) wontthrow -> bool
{
  subshell_bootstrap_reader payload;
  if (!reader.read_section(wire_section::Control, payload)) return false;

  wire.init_moods_sourcing = payload.read_u8();
  wire.initialized_moods = payload.read_u8();
  wire.suppressed_warnings = payload.read_u8();
  return payload.is_fully_read() &&
         (wire.init_moods_sourcing & ~VALID_MOOD_MASK) == 0 &&
         (wire.initialized_moods & ~VALID_MOOD_MASK) == 0 &&
         (wire.suppressed_warnings & ~VALID_SUPPRESSED_WARNING_MASK) == 0;
}

fn ExecutionStore::from_wire(subshell_bootstrap_reader &reader,
                             execution_wire &wire) throws -> bool
{
  subshell_bootstrap_reader payload;
  if (!reader.read_section(wire_section::Execution, payload)) return false;

  bool has_execution_string = false;
  if (!read_subshell_bootstrap_bool(payload, has_execution_string))
    return false;
  if (has_execution_string)
    wire.execution_string = String{heap_allocator(), payload.read_text()};
  wire.last_argument = String{heap_allocator(), payload.read_text()};
  return payload.is_fully_read();
}

fn VariableStore::from_wire(subshell_bootstrap_reader &reader,
                            variable_wire &wire) throws -> bool
{
  subshell_bootstrap_reader payload;
  if (!reader.read_section(wire_section::Variables, payload)) return false;

  wire.disabled_bash_special_arrays = payload.read_u8();
  wire.unset_dynamic_readers = payload.read_u8();
  static_assert(static_cast<u8>(bash_special_array_id::Count) <= 8);
  constexpr u8 VALID_BASH_SPECIAL_ARRAY_MASK =
      (1U << static_cast<u8>(bash_special_array_id::Count)) - 1U;
  static_assert(static_cast<u8>(dynamic_reader_id::Count) <= 8);
  constexpr u8 VALID_DYNAMIC_READER_MASK =
      (1U << static_cast<u8>(dynamic_reader_id::Count)) - 1U;
  if (!payload.is_valid ||
      (wire.disabled_bash_special_arrays &
       static_cast<u8>(~VALID_BASH_SPECIAL_ARRAY_MASK)) != 0 ||
      (wire.unset_dynamic_readers &
       static_cast<u8>(~VALID_DYNAMIC_READER_MASK)) != 0)
  {
    return false;
  }

  if (!read_subshell_bootstrap_bool(payload, wire.has_bash_argument_arrays))
    return false;
  let const frame_count = static_cast<usize>(payload.read_u32());
  if (!payload.is_valid ||
      frame_count > MAX_FUNCTION_CALL_DEPTH + MAX_SOURCE_DEPTH + 1 ||
      frame_count > payload.get_remaining_length() / sizeof(u32))
  {
    return false;
  }
  wire.bash_argument_frame_counts.reserve(frame_count);
  u64 value_count_from_frames = 0;
  for (usize index = 0; index < frame_count; index++) {
    let const argument_count = payload.read_u32();
    value_count_from_frames += argument_count;
    if (value_count_from_frames > UINT32_MAX) return false;
    wire.bash_argument_frame_counts.push(argument_count);
  }
  let const value_count = static_cast<usize>(payload.read_u32());
  if (!payload.is_valid || value_count != value_count_from_frames ||
      value_count > payload.get_remaining_length() / sizeof(u32))
  {
    return false;
  }
  wire.bash_argument_values.reserve(value_count);
  for (usize index = 0; index < value_count; index++)
    wire.bash_argument_values.push(
        String{heap_allocator(), payload.read_text()});
  if (!wire.has_bash_argument_arrays &&
      (!wire.bash_argument_frame_counts.is_empty() ||
       !wire.bash_argument_values.is_empty()))
  {
    return false;
  }

  if (!read_subshell_bootstrap_bool(payload, wire.has_bash_argument_context))
    return false;
  if (wire.has_bash_argument_context) {
    constexpr u8 VALID_FRAME_FLAG_MASK =
        static_cast<u8>(BashArgumentFrameFlag::DidEnter) |
        static_cast<u8>(BashArgumentFrameFlag::IsSource) |
        static_cast<u8>(BashArgumentFrameFlag::HasSourceArguments);
    wire.bash_argument_context_flags = payload.read_u8();
    let const source_path = payload.read_text();
    if (!payload.is_valid || (wire.bash_argument_context_flags &
                              static_cast<u8>(~VALID_FRAME_FLAG_MASK)) != 0)
    {
      return false;
    }
    wire.bash_argument_source_path = String{heap_allocator(), source_path};
  }

  return payload.is_fully_read();
}

fn StartupStore::from_wire(subshell_bootstrap_reader &reader,
                           startup_wire &wire) throws -> bool
{
  subshell_bootstrap_reader payload;
  if (!reader.read_section(wire_section::Startup, payload)) return false;

  if (!read_subshell_bootstrap_bool(payload, wire.is_restricted_shell) ||
      !read_subshell_bootstrap_bool(payload, wire.is_login_shell))
  {
    return false;
  }
  wire.init_moods = String{heap_allocator(), payload.read_text()};
  return payload.is_fully_read();
}

fn DiagnosticsStore::from_wire(subshell_bootstrap_reader &reader,
                               bool &is_source_traces_enabled) wontthrow -> bool
{
  subshell_bootstrap_reader payload;
  if (!reader.read_section(wire_section::Diagnostics, payload)) return false;

  return read_subshell_bootstrap_bool(payload, is_source_traces_enabled) &&
         payload.is_fully_read();
}

fn FunctionStore::from_wire(subshell_bootstrap_reader &reader,
                            function_wire &wire) throws -> bool
{
  subshell_bootstrap_reader payload;
  if (!reader.read_section(wire_section::Functions, payload)) return false;

  let const call_depth = payload.read_u64();
  let const call_name_count = static_cast<usize>(payload.read_u32());
  if (!payload.is_valid || call_depth > MAX_FUNCTION_CALL_DEPTH ||
      call_name_count > call_depth ||
      call_name_count > payload.get_remaining_length() / sizeof(u32))
  {
    return false;
  }
  wire.call_depth = static_cast<usize>(call_depth);
  wire.call_names.reserve(call_name_count);
  for (usize index = 0; index < call_name_count; index++)
    wire.call_names.push(String{heap_allocator(), payload.read_text()});

  let names = ArrayList<u32>{heap_allocator()};
  if (!payload.is_valid || !read_wire_source_names(payload, names)) {
    return false;
  }

  let const record_count = static_cast<usize>(payload.read_u32());
  if (!payload.is_valid ||
      record_count > payload.get_remaining_length() / (7 * sizeof(u32)))
  {
    return false;
  }

  for (usize index = 0; index < record_count; index++) {
    let const name = payload.read_text();
    let info = function_definition_info{};
    if (!read_wire_source_name(payload, names, info.source_name_index)) {
      return false;
    }

    let const line_offset = payload.read_u32();
    let const enclosing_line_count = payload.read_u32();
    let const definition_line = payload.read_u32();
    let const line_prefix = payload.read_text();
    let const line_suffix = payload.read_text();
    if (!payload.is_valid || name.is_empty() || line_offset > INT32_MAX ||
        enclosing_line_count > INT32_MAX)
    {
      return false;
    }

    info.line_offset = static_cast<isize>(line_offset);
    info.enclosing_line_count = usize{enclosing_line_count};
    info.definition_line = usize{definition_line};
    info.line_prefix = String{heap_allocator(), line_prefix};
    info.line_suffix = String{heap_allocator(), line_suffix};
    wire.definition_origins.set(name, steal(info));
  }

  return payload.is_fully_read();
}

fn FunctionStore::apply_wire_definitions(function_wire &wire) throws -> void
{
  wire.definition_origins.for_each(
      [&](StringView name, function_definition_info &origin) throws {
        let const storage = m_definitions.find(name);
        if (!storage.has_value()) return;

        let const *replayed = storage->get_definition_info();
        let const *source = storage->get_source();
        if (replayed == nullptr || source == nullptr) {
          return;
        }

        origin.body_start_position = replayed->body_start_position;
        origin.header_length = replayed->header_length;
        origin.body_name_index = replayed->body_name_index;
        origin.defining_state = replayed->defining_state;
        storage->set_definition(source->view(), steal(origin));
      });
}

fn ScopeStore::from_wire(
    subshell_bootstrap_reader &reader,
    ArrayList<ArrayList<local_binding>> &local_scopes) throws -> bool
{
  subshell_bootstrap_reader payload;
  if (!reader.read_section(wire_section::Scopes, payload)) return false;

  let const depth = static_cast<usize>(payload.read_u32());
  if (!payload.is_valid || depth > MAX_FUNCTION_CALL_DEPTH ||
      depth > payload.get_remaining_length() / sizeof(u32))
  {
    return false;
  }

  constexpr usize MINIMUM_LOCAL_BINDING_BYTES = 10;
  constexpr usize MINIMUM_SPARSE_ELEMENT_BYTES = 12;
  constexpr u8 VALID_ATTRIBUTE_MASK =
      static_cast<u8>(variable_attribute::Readonly) |
      static_cast<u8>(variable_attribute::Integer) |
      static_cast<u8>(variable_attribute::Lowercase) |
      static_cast<u8>(variable_attribute::Uppercase) |
      static_cast<u8>(variable_attribute::Declared) |
      static_cast<u8>(variable_attribute::Nameref);
  let const do_read_texts = [&](ArrayList<String> &texts, usize text_count)
                                throws -> void {
    texts.reserve(text_count);
    for (usize index = 0; index < text_count; index++)
      texts.push(String{heap_allocator(), payload.read_text()});
  };

  local_scopes.reserve(depth);
  for (usize frame_index = 0; frame_index < depth; frame_index++) {
    let const binding_count = static_cast<usize>(payload.read_u32());
    if (!payload.is_valid || binding_count > payload.get_remaining_length() /
                                                 MINIMUM_LOCAL_BINDING_BYTES)
    {
      return false;
    }

    let frame = ArrayList<local_binding>{heap_allocator()};
    frame.reserve(binding_count);
    for (usize binding_index = 0; binding_index < binding_count;
         binding_index++)
    {
      let binding = local_binding{
          .name = String{heap_allocator(), payload.read_text()},
          .previous_value = None,
          .previous_special_definition_location = None,
          .previous_indexed_array = None,
          .previous_associative_keys = ArrayList<String>{heap_allocator()},
          .previous_associative_values = ArrayList<String>{heap_allocator()},
          .previous_sparse_indices = ArrayList<usize>{heap_allocator()},
          .previous_sparse_values = ArrayList<String>{heap_allocator()},
          .previous_attributes = 0,
          .previous_was_associative = false,
          .previous_was_exported = false,
          .is_self_reference = false,
      };
      let const flags = payload.read_u8();
      binding.previous_attributes = payload.read_u8();
      if (!payload.is_valid ||
          (flags & static_cast<u8>(~ALL_LOCAL_BINDING_WIRE_FLAGS)) != 0 ||
          (binding.previous_attributes &
           static_cast<u8>(~VALID_ATTRIBUTE_MASK)) != 0)
      {
        return false;
      }
      let const do_has_flag = [&](local_binding_wire_flag flag) {
        return (flags & static_cast<u8>(flag)) != 0;
      };
      binding.previous_was_associative =
          do_has_flag(local_binding_wire_flag::WasAssociative);
      binding.previous_was_exported =
          do_has_flag(local_binding_wire_flag::WasExported);
      binding.is_self_reference =
          do_has_flag(local_binding_wire_flag::IsSelfReference);

      if (do_has_flag(local_binding_wire_flag::HasValue))
        binding.previous_value = String{heap_allocator(), payload.read_text()};

      if (do_has_flag(local_binding_wire_flag::HasIndexedArray)) {
        let const element_count = static_cast<usize>(payload.read_u32());
        if (!payload.is_valid ||
            element_count > payload.get_remaining_length() / sizeof(u32))
        {
          return false;
        }
        let elements = ArrayList<String>{heap_allocator()};
        do_read_texts(elements, element_count);
        binding.previous_indexed_array = steal(elements);
      }

      if (binding.previous_was_associative) {
        let const pair_count = static_cast<usize>(payload.read_u32());
        if (!payload.is_valid ||
            pair_count > payload.get_remaining_length() / (2 * sizeof(u32)))
        {
          return false;
        }
        binding.previous_associative_keys.reserve(pair_count);
        binding.previous_associative_values.reserve(pair_count);
        for (usize index = 0; index < pair_count; index++) {
          binding.previous_associative_keys.push(
              String{heap_allocator(), payload.read_text()});
          binding.previous_associative_values.push(
              String{heap_allocator(), payload.read_text()});
        }
      }

      let const sparse_count = static_cast<usize>(payload.read_u32());
      if (!payload.is_valid || sparse_count > payload.get_remaining_length() /
                                                  MINIMUM_SPARSE_ELEMENT_BYTES)
      {
        return false;
      }
      binding.previous_sparse_indices.reserve(sparse_count);
      binding.previous_sparse_values.reserve(sparse_count);
      for (usize index = 0; index < sparse_count; index++) {
        let const sparse_index = payload.read_u64();
        if (sparse_index > SIZE_MAX) return false;
        binding.previous_sparse_indices.push(static_cast<usize>(sparse_index));
        binding.previous_sparse_values.push(
            String{heap_allocator(), payload.read_text()});
      }

      if (!payload.is_valid) return false;
      frame.push(steal(binding));
    }
    local_scopes.push(steal(frame));
  }

  return payload.is_fully_read();
}

fn TrapStore::from_wire(subshell_bootstrap_reader &reader,
                        u64 &startup_ignored_signals) wontthrow -> bool
{
  subshell_bootstrap_reader payload;
  if (!reader.read_section(wire_section::Traps, payload)) return false;

  startup_ignored_signals = payload.read_u64();
  return payload.is_fully_read();
}

fn ProgramResolver::from_wire(subshell_bootstrap_reader &reader,
                              StringMap<CacheEntry> &execution_cache) throws
    -> bool
{
  subshell_bootstrap_reader payload;
  if (!reader.read_section(wire_section::Programs, payload)) return false;

  let const entry_count = static_cast<usize>(payload.read_u32());
  constexpr usize MINIMUM_CACHE_ENTRY_BYTES = 12;
  constexpr usize MINIMUM_CACHED_PATH_BYTES = 5;
  if (!payload.is_valid ||
      entry_count > payload.get_remaining_length() / MINIMUM_CACHE_ENTRY_BYTES)
  {
    return false;
  }

  for (usize entry_index = 0; entry_index < entry_count; entry_index++) {
    let const name = payload.read_text();
    let const bare_path_position = payload.read_u32();
    let const path_count = static_cast<usize>(payload.read_u32());
    if (!payload.is_valid ||
        path_count >
            payload.get_remaining_length() / MINIMUM_CACHED_PATH_BYTES ||
        (bare_path_position != NO_BARE_PROGRAM_PATH &&
         bare_path_position >= path_count))
    {
      return false;
    }

    let entry = CacheEntry{};
    if (bare_path_position != NO_BARE_PROGRAM_PATH)
      entry.bare_path_position = static_cast<usize>(bare_path_position);
    entry.paths.reserve(path_count);
    for (usize path_index = 0; path_index < path_count; path_index++) {
      let const extension = payload.read_u8();
      let const path = payload.read_text();
      if (!payload.is_valid ||
          extension > static_cast<u8>(os::program_extension::Bat))
      {
        return false;
      }
      entry.paths.push(CachedPath{
          Path{path}, static_cast<os::program_extension>(extension)});
    }
    if (!execution_cache.insert(name, steal(entry))) return false;
  }

  return payload.is_fully_read();
}

fn CompletionStore::from_wire(subshell_bootstrap_reader &reader,
                              completion_snapshot &wire) throws -> bool
{
  subshell_bootstrap_reader payload;
  if (!reader.read_section(wire_section::Completion, payload)) return false;

  let const spec_count = static_cast<usize>(payload.read_u32());
  constexpr usize MINIMUM_COMPLETION_SPEC_BYTES = 48;
  if (!payload.is_valid || spec_count > payload.get_remaining_length() /
                                            MINIMUM_COMPLETION_SPEC_BYTES)
  {
    return false;
  }
  wire.specs.reserve(spec_count);
  let const do_read_spec = [&](completion_spec &spec) throws -> bool {
    let const function_name = payload.read_text();
    let const word_list = payload.read_text();
    let const glob_pattern = payload.read_text();
    let const filter_pattern = payload.read_text();
    let const prefix = payload.read_text();
    let const suffix = payload.read_text();
    let const command = payload.read_text();
    let const action_mask = payload.read_u32();
    let const option_mask = payload.read_u32();
    let const argument_mask = payload.read_u32();
    if (!payload.is_valid || (action_mask >> COMPGEN_ACTION_COUNT) != 0 ||
        (option_mask >> COMPLETION_OPTION_COUNT) != 0 ||
        (argument_mask >> COMPLETION_ARGUMENT_COUNT) != 0 ||
        !definition_state::from_wire(payload, spec.defining_state))
    {
      return false;
    }
    spec.function_name = String{heap_allocator(), function_name};
    spec.word_list = String{heap_allocator(), word_list};
    spec.glob_pattern = String{heap_allocator(), glob_pattern};
    spec.filter_pattern = String{heap_allocator(), filter_pattern};
    spec.prefix = String{heap_allocator(), prefix};
    spec.suffix = String{heap_allocator(), suffix};
    spec.command = String{heap_allocator(), command};
    spec.action_mask = action_mask;
    spec.option_mask = option_mask;
    spec.argument_mask = argument_mask;
    return true;
  };

  for (usize spec_index = 0; spec_index < spec_count; spec_index++) {
    let const command = payload.read_text();
    let spec = completion_spec{};
    if (!payload.is_valid || !do_read_spec(spec) ||
        wire.specs.find(command).has_value())
    {
      return false;
    }
    wire.specs.set(command, steal(spec));
  }

  for (let *slot_spec :
       {&wire.default_spec, &wire.empty_spec, &wire.initial_spec})
  {
    bool has_slot_spec = false;
    if (!read_subshell_bootstrap_bool(payload, has_slot_spec)) return false;
    if (!has_slot_spec) continue;

    let spec = completion_spec{};
    if (!do_read_spec(spec)) return false;
    *slot_spec = steal(spec);
  }

  return payload.is_fully_read();
}

fn JobTable::from_wire(subshell_bootstrap_reader &reader,
                       job_table_wire &wire) throws -> bool
{
  subshell_bootstrap_reader payload;
  if (!reader.read_section(wire_section::Jobs, payload)) return false;

  bool has_last_background_pid = false;
  if (!read_subshell_bootstrap_bool(payload, has_last_background_pid))
    return false;
  if (has_last_background_pid) wire.last_background_pid = payload.read_i64();
  wire.next_job_id = payload.read_i32();

  let const job_count = static_cast<usize>(payload.read_u32());
  constexpr usize MINIMUM_JOB_BYTES = 48;
  if (!payload.is_valid || wire.next_job_id < 1 ||
      job_count > payload.get_remaining_length() / MINIMUM_JOB_BYTES)
  {
    return false;
  }
  wire.jobs.reserve(job_count);
  i32 previous_job_id = 0;

  for (usize job_index = 0; job_index < job_count; job_index++) {
    let child_job = job{wire.jobs.allocator()};
    child_job.id = payload.read_i32();
    let const command = payload.read_text();
    child_job.command = String{wire.jobs.allocator(), command};
    child_job.process_id = payload.read_i64();
    child_job.process_group_id = payload.read_i64();
    child_job.last_status = payload.read_i32();
    child_job.stopped_status = payload.read_i32();
    child_job.termination.signal_number = payload.read_i32();
    if (!read_subshell_bootstrap_bool(payload,
                                      child_job.termination.did_dump_core))
    {
      return false;
    }

    let const state = payload.read_u8();
    bool is_primary_process_active = false;
    bool has_unreported_state_change = false;
    if (child_job.termination.signal_number < 0 ||
        child_job.termination.signal_number > 127 ||
        !read_subshell_bootstrap_bool(payload, is_primary_process_active) ||
        !read_subshell_bootstrap_bool(payload, has_unreported_state_change) ||
        child_job.id <= previous_job_id || child_job.id >= wire.next_job_id ||
        child_job.process_group_id < 0 ||
        state > static_cast<u8>(job::State::Done))
    {
      return false;
    }
    previous_job_id = child_job.id;
    child_job.state = static_cast<job::State>(state);
    child_job.is_primary_process_active = is_primary_process_active;
    child_job.has_unreported_state_change = has_unreported_state_change;

    let const primary_process_index = payload.read_u32();
    if (!payload.is_valid ||
        (is_primary_process_active &&
         primary_process_index == NO_BOOTSTRAP_PROCESS) ||
        (!is_primary_process_active &&
         primary_process_index != NO_BOOTSTRAP_PROCESS) ||
        (child_job.state == job::State::Done && is_primary_process_active))
    {
      return false;
    }
    child_job.pid = KOSH_INVALID_PROCESS;
    if (is_primary_process_active)
      wire.process_references.push(primary_process_index);

    let const earlier_process_count = static_cast<usize>(payload.read_u32());
    if (!payload.is_valid ||
        earlier_process_count > payload.get_remaining_length() / sizeof(u32))
    {
      return false;
    }
    if ((child_job.state == job::State::Done && earlier_process_count != 0) ||
        (child_job.state != job::State::Done && !is_primary_process_active &&
         earlier_process_count == 0))
    {
      return false;
    }

    child_job.earlier_pipeline_processes.reserve(earlier_process_count);
    for (usize process_index = 0; process_index < earlier_process_count;
         process_index++)
    {
      wire.process_references.push(payload.read_u32());
      child_job.earlier_pipeline_processes.push(KOSH_INVALID_PROCESS);
    }
    wire.jobs.push(steal(child_job));
  }

  let const detached_process_count = static_cast<usize>(payload.read_u32());
  if (!payload.is_valid ||
      detached_process_count > payload.get_remaining_length() / sizeof(u32))
  {
    return false;
  }
  wire.detached_processes.reserve(detached_process_count);
  for (usize process_index = 0; process_index < detached_process_count;
       process_index++)
  {
    wire.process_references.push(payload.read_u32());
    wire.detached_processes.push(KOSH_INVALID_PROCESS);
  }

  return payload.is_fully_read();
}

fn job_table_wire::bind_processes(
    const os::subshell_bootstrap &bootstrap) wontthrow -> bool
{
  if (process_references.count() != bootstrap.processes.count()) return false;

  let referenced_processes = Bitset{heap_allocator()};
  referenced_processes.reset(bootstrap.processes.count());
  for (let const process_index : process_references) {
    if (process_index >= bootstrap.processes.count() ||
        referenced_processes[process_index])
    {
      return false;
    }
    referenced_processes.set(process_index);
  }

  usize process_reference_position = 0;
  for (let &child_job : jobs) {
    if (child_job.is_primary_process_active)
      child_job.pid =
          bootstrap.processes[process_references[process_reference_position++]];
    for (let &process : child_job.earlier_pipeline_processes)
      process =
          bootstrap.processes[process_references[process_reference_position++]];
  }
  for (let &process : detached_processes)
    process =
        bootstrap.processes[process_references[process_reference_position++]];
  ASSERT(process_reference_position == process_references.count());

  return true;
}

fn JobTable::apply_wire(job_table_wire wire) wontthrow -> void
{
  m_last_background_pid = wire.last_background_pid;
  m_jobs = steal(wire.jobs);
  m_detached_job_processes = steal(wire.detached_processes);
  m_next_job_id = wire.next_job_id;
}

wontreturn static fn invalid_subshell_bootstrap() throws -> void
{
  throw Error{"Invalid inherited shell state"};
}

fn EvalContext::make_subshell_bootstrap() const throws -> os::subshell_bootstrap
{
  let bootstrap = os::subshell_bootstrap{};
  String &source = bootstrap.payload;
  let collected_names = ArrayList<String>{heap_allocator()};
  variable_names().for_each(
      [&](StringView name) { collected_names.push_managed(name); });
  let const names = steal(collected_names).make_sorted(sort_order::ascending);

  let const do_append_assignment = [&](StringView name, StringView subscript,
                                       StringView value) throws {
    source.append(name);
    source.push('[');
    append_shell_quoted_arg(source, subscript);
    source += "]=";
    append_shell_quoted_arg(source, value);
    source.push('\n');
  };

  for (let const &stored_name : names) {
    let const name = stored_name.view();
    if (is_bash_aliases_special(name) || is_bash_argument_array(name) ||
        is_bash_directory_stack_special(name))
    {
      continue;
    }
    if (variable_store().attributes().is_nameref(name)) {
      source += "declare -n ";
      source.append(name);
      if (let const target = variable_store().shell_variables().find(name);
          target.has_value())
      {
        source.push('=');
        append_shell_quoted_arg(source, target->view());
      }
      source.push('\n');
      continue;
    }
    let const is_integer = is_integer_variable(name);
    let const is_lowercase = variable_store().attributes().is_lowercase(name);
    let const is_uppercase = variable_store().attributes().is_uppercase(name);
    let const is_read_only = is_readonly(name);
    let const is_exported_value = is_exported(name);
    let const indexed = variable_store().indexed_arrays().find(name);
    let const is_associative = is_associative_array(name);
    let const value = get_variable_value(name);
    if (!indexed.has_value() && !is_associative && is_exported_value &&
        !is_integer && !is_lowercase && !is_uppercase && !is_read_only)
    {
      let const environment_value = os::get_environment_variable(name);
      if (environment_value.has_value() &&
          (!value.has_value() || *environment_value == *value))
      {
        continue;
      }
    }

    source += is_associative        ? "declare -A"
              : indexed.has_value() ? "declare -a"
                                    : "declare -";
    if (is_integer) source.push('i');
    if (is_lowercase) source.push('l');
    if (is_uppercase) source.push('u');
    if (is_exported_value) source.push('x');
    if (!indexed.has_value() && !is_associative && !is_integer &&
        !is_lowercase && !is_uppercase && !is_exported_value)
    {
      source.push('-');
    }
    source.push(' ');
    source.append(name);
    if (!indexed.has_value() && !is_associative && value.has_value()) {
      source.push('=');
      append_shell_quoted_arg(source, value->view());
    }
    source.push('\n');

    if (indexed.has_value() || is_associative) {
      let const subscripts = collect_array_subscripts(name);
      let const values = collect_array_elements(name);
      ASSERT(subscripts.count() == values.count());
      for (usize index = 0; index < subscripts.count(); index++)
        do_append_assignment(name, subscripts[index].view(),
                             values[index].view());
    }
  }

  variable_store().exported_names().for_each(
      [&](StringView key, const auto &display_name) throws -> void {
        let name = key;
        if constexpr (!os::ENVIRONMENT_IS_CASE_SENSITIVE) {
          if (!display_name.is_empty()) name = display_name.view();
        } else {
          unused(display_name);
        }
        if (!lexer::word_is_variable_name(name) || names.find(name).has_value())
          return;

        source += "export ";
        source.append(name);
        source.push('\n');
      });

  for (let const &name : function_store().sorted_names()) {
    let const *function_source = function_store().find_source(name.view());
    if (function_source == nullptr || function_source->is_empty()) {
      continue;
    }
    source.append(function_source->view());
    source.push('\n');
  }
  for (let const &definition : scope_store().alias_definitions()) {
    source += "alias ";
    source.append(definition.view());
    source.push('\n');
  }
  trap_store().list(
      [&](StringView condition, const trap_definition &trap) throws {
        if (condition == "EXIT") return;
        source += "trap -- ";
        append_shell_quoted_arg(source, trap.action_text.view());
        source.push(' ');
        append_shell_quoted_arg(source, condition);
        source.push('\n');
      });

  source += "set --";
  for (let const &parameter : variable_store().positional_params()) {
    source.push(' ');
    append_shell_quoted_arg(source, parameter.view());
  }
  source.push('\n');

  let const working_directory = logical_working_directory(*this);
  if (!variable_store().directory_stack().is_empty()) {
    source += "builtin cd -- ";
    append_shell_quoted_arg(source,
                            variable_store().directory_stack()[0].view());
    source.push('\n');
    for (usize index = 1; index < variable_store().directory_stack().count();
         index++)
    {
      source += "pushd ";
      append_shell_quoted_arg(source,
                              variable_store().directory_stack()[index].view());
      source += " >/dev/null\n";
    }
    source += "pushd ";
    append_shell_quoted_arg(source, working_directory.view());
    source += " >/dev/null\n";
  } else {
    source += "builtin cd -- ";
    append_shell_quoted_arg(source, working_directory.view());
    source.push('\n');
  }
  if (let const previous_directory = get_variable_value("OLDPWD");
      previous_directory.has_value())
  {
    source += "OLDPWD=";
    append_shell_quoted_arg(source, previous_directory->view());
    source.push('\n');
  } else {
    source += "unset OLDPWD\n";
  }

  char mask_text[8];
  std::snprintf(mask_text, sizeof(mask_text), "%04o",
                os::get_file_creation_mask());
  source += "umask ";
  source += mask_text;
  source.push('\n');
  for (let const &stored_name : names) {
    if (!is_readonly(stored_name.view())) continue;
    source += "readonly ";
    source.append(stored_name.view());
    source.push('\n');
  }
  if (source.count() > UINT32_MAX) throw std::bad_alloc{};
  bootstrap.source_length = static_cast<u32>(source.count());

  let body = String{heap_allocator()};
  execution_store().append_wire(body);
  job_table_store().append_wire(body, bootstrap);
  dynamic_runtime_store().get_clock().append_wire(body);
  expansion_store().get_getopts_cursor().append_wire(body);
  RuntimeState::capture(*this).append_wire(body);
  variable_store().append_wire(body);
  startup_store().append_wire(body);
  function_store().append_wire(body);
  scope_store().append_wire(body);
  completion_store().append_wire(body);
  trap_store().append_wire(body);
  runtime_control_store().append_wire(body);
  program_resolver().append_wire(body);
  diagnostics_store().append_wire(body);

  if (body.count() > UINT32_MAX) throw std::bad_alloc{};
  append_subshell_bootstrap_u32(source, SUBSHELL_BOOTSTRAP_MAGIC);
  append_subshell_bootstrap_u32(source, SUBSHELL_BOOTSTRAP_VERSION);
  append_subshell_bootstrap_u32(source, static_cast<u32>(body.count()));
  source.append(body.view());
  return bootstrap;
}

fn EvalContext::make_child_evaluator_state(
    os::subshell_bootstrap &bootstrap) const throws -> os::child_evaluator_state
{
  prepare_child_environment();

  let const should_launch_fresh_evaluator = !os::can_fork_evaluator();
  if (should_launch_fresh_evaluator && bootstrap.payload.is_empty())
    bootstrap = make_subshell_bootstrap();

  return os::child_evaluator_state{
      .bootstrap = should_launch_fresh_evaluator ? &bootstrap : nullptr,
      .shell_name = execution_store().get_shell_name(),
      .inherited =
          os::inherited_subshell_state{
                                       .previous_exit_status = execution_store().last_exit_status(),
                                       .shell_process_id = os::get_shell_process_id(),
                                       .shell_parent_process_id = os::get_shell_parent_process_id(),
                                       .subshell_depth = execution_store().subshell_depth() + 1,
                                       },
      .mood = runtime_state().get_mood(),
  };
}

fn EvalContext::set_child_source_origin(
    os::subshell_bootstrap &bootstrap, StringView child_source,
    const SourceLocation &launch_location) const throws -> void
{
  bootstrap.source_origin.clear();
  if (bootstrap.payload.is_empty()) return;

  let names = ArrayList<u32>{heap_allocator()};
  let const do_append_line = [&](String &output, u32 name_index,
                                 isize line_number, Maybe<usize> counted_line)
                                 throws -> bool {
    if (line_number < 1 || line_number > static_cast<isize>(UINT32_MAX)) {
      return false;
    }

    let const line = static_cast<usize>(line_number);
    let const counted = counted_line.value_or(line);
    let const extra_line_count =
        counted > line && counted - line <= UINT32_MAX ? counted - line : 0;
    append_subshell_bootstrap_u32(output,
                                  find_wire_source_name(names, name_index));
    append_subshell_bootstrap_u32(output, static_cast<u32>(line));
    append_subshell_bootstrap_u32(output, static_cast<u32>(extra_line_count));
    return true;
  };

  let const do_append_origin = [&](String &output) throws -> bool {
    if (child_source.is_empty()) return false;

    let rendered_source = child_source;
    let location = SourceLocation{0, child_source.length};
    let line_offset = isize{0};
    let counted_line = Maybe<usize>{None};
    let const *current = source_store().current_source();
    let const owner = resolve_render_source(launch_location);
    let const is_inside_owner =
        owner.text != nullptr && child_source.data >= owner.text->view().data &&
        child_source.data + child_source.length <=
            owner.text->view().data + owner.text->view().length;
    if (is_inside_owner) {
      rendered_source = owner.text->view();
      let const render_position =
          static_cast<usize>(child_source.data - owner.text->view().data);
      let const body_location = SourceLocation{
          owner.is_windowed ? render_position - owner.header_length +
                                  owner.body_start_position
                            : render_position,
          child_source.length, launch_location.source_name_index};
      location = owner.rebase(body_location);
      if (owner.is_windowed) line_offset = owner.line_offset;
      counted_line = line_number_at_location(
          body_location, current, function_store().call_frames().count());
    } else {
      for (usize index = source_store().embedded_sources().count(); index > 0;
           index--)
      {
        let const &base = source_store().embedded_sources()[index - 1];
        if (base.body == nullptr || base.body->view().data != child_source.data)
        {
          continue;
        }

        counted_line = line_number_at_location(location, base.body,
                                               base.function_call_depth);
        break;
      }
    }

    let const site = resolve_rendered_site(
        rendered_source, location, line_offset, owner.is_windowed, this);
    if (!is_inside_owner && site.source.data == child_source.data) {
      return false;
    }

    let const site_text = site.location.get_source_text(site.source);
    if (!site_text.has_value() || *site_text != child_source) {
      return false;
    }

    let const body_position = usize{site.location.position};
    let const before_body = site.source.substring_of_length(0, body_position);
    let const last_newline = before_body.find_last_character('\n');
    let const line_start =
        last_newline.has_value() ? *last_newline + 1 : usize{0};
    let const after_body =
        site.source.substring(body_position + child_source.length);
    let const suffix_length =
        after_body.find_character('\n').value_or(after_body.length);
    let const line_number =
        static_cast<isize>(utils::line_number_at(site.source, body_position)) +
        site.line_offset;
    if (!do_append_line(output, site.location.source_name_index, line_number,
                        counted_line))
    {
      return false;
    }

    append_subshell_bootstrap_text(output, before_body.substring(line_start));
    append_subshell_bootstrap_text(
        output, after_body.substring_of_length(0, suffix_length));
    return true;
  };

  let const do_append_frame_site =
      [&](String &output, const SourceLocation &call_site, const String *source,
          usize call_depth, usize depth_floor, Maybe<usize> counted_line)
          throws -> bool {
    if (source == nullptr) return false;

    let const resolved =
        resolve_render_source(call_site, source, call_depth, depth_floor);
    if (resolved.text == nullptr) return false;

    let const location = resolved.rebase(call_site);
    if (location.position > resolved.text->count()) return false;

    let const site = resolve_rendered_site(resolved.text->view(), location,
                                           resolved.line_offset, true, this);
    let const text = site.source;
    usize position = site.location.position;
    if (text.data == nullptr || position > text.count()) {
      return false;
    }

    if (position + 2 < text.count() && text[position] == '\\' &&
        text[position + 1] == '\n')
    {
      position += 2;
    } else if (position + 1 < text.count() && text[position] == '\n') {
      position++;
    }

    let const line_position = utils::source_line_position_at(text, position);
    let const line_number =
        static_cast<isize>(line_position.line_number + 1) + site.line_offset;
    if (!do_append_line(output, site.location.source_name_index, line_number,
                        counted_line))
    {
      return false;
    }

    let const column = position - line_position.line_start;
    let const line_end = line_position.line_end - line_position.line_start;
    let const length = usize{site.location.length} < line_end - column
                           ? usize{site.location.length}
                           : line_end - column;
    append_subshell_bootstrap_u32(output, static_cast<u32>(column));
    append_subshell_bootstrap_u32(output, static_cast<u32>(length));
    append_subshell_bootstrap_text(
        output, text.substring_of_length(line_position.line_start, line_end));
    return true;
  };

  let origin = String{heap_allocator()};
  let const has_origin = do_append_origin(origin);

  let const &call_frames = function_store().call_frames();
  let const &source_frames = source_store().source_frames();
  let frames = String{heap_allocator()};
  append_subshell_bootstrap_u32(frames, static_cast<u32>(call_frames.count()));
  append_subshell_bootstrap_u32(frames,
                                static_cast<u32>(source_frames.count()));
  for (usize call_index = 0; call_index < call_frames.count(); call_index++) {
    let const &call_frame = call_frames[call_index];
    usize frame_limit = 0;
    while (frame_limit < source_frames.count() &&
           source_frames[frame_limit].function_call_depth <= call_index)
    {
      frame_limit++;
    }

    let site = String{heap_allocator()};
    let const has_site =
        call_frame.source != nullptr &&
        do_append_frame_site(site, call_frame.location, call_frame.source,
                             call_index, source_depth_floor(frame_limit),
                             line_number_at_location(call_frame.location,
                                                     call_frame.source,
                                                     call_index));
    u8 flags = 0;
    if (call_frame.was_printed)
      flags |= static_cast<u8>(inherited_frame_wire_flag::WasPrinted);
    if (has_site) flags |= static_cast<u8>(inherited_frame_wire_flag::HasSite);
    frames.push(static_cast<char>(flags));
    frames.append(site.view());
  }

  for (usize frame_index = 0; frame_index < source_frames.count();
       frame_index++)
  {
    let const &frame = source_frames[frame_index];
    let const *frame_source = borrowed_frame_source(frame);
    let site = String{heap_allocator()};
    let const has_site =
        frame_source != nullptr &&
        do_append_frame_site(
            site, frame.call_site, frame_source, frame.function_call_depth,
            source_depth_floor(frame_index),
            frame.has_bash_source_row()
                ? Maybe<usize>{line_number_at_location(
                      frame.call_site, frame_source, frame.function_call_depth)}
                : Maybe<usize>{None});
    u8 flags = 0;
    if (frame.was_printed)
      flags |= static_cast<u8>(inherited_frame_wire_flag::WasPrinted);
    if (has_site) flags |= static_cast<u8>(inherited_frame_wire_flag::HasSite);
    if (frame.is_source_changing)
      flags |= static_cast<u8>(inherited_frame_wire_flag::IsSourceChanging);
    if (frame.should_defer_trace)
      flags |= static_cast<u8>(inherited_frame_wire_flag::ShouldDeferTrace);
    frames.push(static_cast<char>(flags));
    frames.push(static_cast<char>(frame.kind));
    append_subshell_bootstrap_u32(frames,
                                  static_cast<u32>(frame.function_call_depth));
    append_subshell_bootstrap_text(frames, frame.origin.view());
    append_subshell_bootstrap_text(frames, frame.source_path.view());
    frames.append(site.view());
  }

  append_wire_section(
      bootstrap.source_origin, wire_section::Origin, [&](String &payload) {
        append_wire_source_names(payload, names);
        payload.push(static_cast<char>(source_store().is_script_run()));
        payload.push(static_cast<char>(has_origin));
        payload.append(origin.view());
        payload.append(frames.view());
      });
}

fn EvalContext::register_inherited_source_origin(
    StringView origin, const String &contents, ArrayList<String> &windows,
    Maybe<StringView> &source_name) throws -> bool
{
  if (origin.is_empty()) return false;

  let reader = subshell_bootstrap_reader{origin};
  let payload = subshell_bootstrap_reader{StringView{}};
  let names = ArrayList<u32>{heap_allocator()};
  bool is_script_run = false;
  bool has_origin = false;
  if (!reader.read_section(wire_section::Origin, payload) ||
      !reader.is_fully_read() || !read_wire_source_names(payload, names) ||
      !read_subshell_bootstrap_bool(payload, is_script_run) ||
      !read_subshell_bootstrap_bool(payload, has_origin))
  {
    invalid_subshell_bootstrap();
  }

  u32 origin_name_index = 0;
  u32 origin_line_number = 0;
  u32 origin_extra_line_count = 0;
  let origin_prefix = StringView{};
  let origin_suffix = StringView{};
  if (has_origin) {
    if (!read_wire_source_name(payload, names, origin_name_index)) {
      invalid_subshell_bootstrap();
    }

    origin_line_number = payload.read_u32();
    origin_extra_line_count = payload.read_u32();
    origin_prefix = payload.read_text();
    origin_suffix = payload.read_text();
  }

  let const call_count = static_cast<usize>(payload.read_u32());
  let const source_frame_count = static_cast<usize>(payload.read_u32());
  if (!payload.is_valid || (has_origin && origin_line_number == 0) ||
      call_count > function_store().call_frames().count() ||
      source_frame_count > payload.get_remaining_length() / 14)
  {
    invalid_subshell_bootstrap();
  }

  ASSERT(windows.is_empty());
  windows.reserve(call_count + source_frame_count + 2);
  windows.push(String{heap_allocator()});
  let &extra_lines = windows.back();
  let const do_register_extra_lines = [&](const String &window,
                                          u32 extra_line_count, u32 name_index,
                                          usize call_depth) throws -> void {
    if (extra_line_count == 0) return;

    if (extra_lines.count() < extra_line_count) {
      extra_lines.append_repeated('\n', extra_line_count - extra_lines.count());
    }

    let const extra_lines_end = SourceLocation{extra_line_count, 0, name_index};
    source_store().push_embedded_source(
        embedded_source{StringView{}, &extra_lines, extra_lines_end, 0, &window,
                        call_depth, call_depth, false});
  };

  let const do_read_site = [&](usize call_depth, SourceLocation &location)
                               throws -> const String * {
    u32 name_index = 0;
    if (!read_wire_source_name(payload, names, name_index)) {
      invalid_subshell_bootstrap();
    }

    let const line_number = payload.read_u32();
    let const extra_line_count = payload.read_u32();
    let const column = payload.read_u32();
    let const length = payload.read_u32();
    let const line_text = payload.read_text();
    if (!payload.is_valid || line_number == 0 || column > line_text.length ||
        length > line_text.length - column)
    {
      invalid_subshell_bootstrap();
    }

    windows.push(String{heap_allocator(), line_text});
    let &window = windows.back();
    source_store().push_line_base(&window, usize{line_number} - 1);
    location = SourceLocation{column, length, name_index};
    do_register_extra_lines(window, extra_line_count, name_index, call_depth);
    return &window;
  };

  let const do_read_flags = [&]() throws -> u8 {
    let const flags = payload.read_u8();
    if (!payload.is_valid || (flags & ~ALL_INHERITED_FRAME_WIRE_FLAGS) != 0) {
      invalid_subshell_bootstrap();
    }

    return flags;
  };

  for (usize call_index = 0; call_index < call_count; call_index++) {
    let const flags = do_read_flags();
    let &call_frame = function_store().call_frames()[call_index];
    call_frame.was_printed =
        (flags & static_cast<u8>(inherited_frame_wire_flag::WasPrinted)) != 0;
    if ((flags & static_cast<u8>(inherited_frame_wire_flag::HasSite)) != 0)
      call_frame.source = do_read_site(call_index, call_frame.location);
  }

  for (usize frame_index = 0; frame_index < source_frame_count; frame_index++) {
    let const flags = do_read_flags();
    let const kind = payload.read_u8();
    let const function_call_depth = static_cast<usize>(payload.read_u32());
    let const frame_origin = payload.read_text();
    let const source_path = payload.read_text();
    let const previous_depth =
        source_store().source_frames().is_empty()
            ? usize{0}
            : source_store().source_frames().back().function_call_depth;
    if (!payload.is_valid ||
        kind > static_cast<u8>(source_frame_kind::SourcedFile) ||
        function_call_depth > function_store().call_frames().count() ||
        function_call_depth < previous_depth)
    {
      invalid_subshell_bootstrap();
    }

    let call_site = SourceLocation{};
    const String *parent_source = nullptr;
    if ((flags & static_cast<u8>(inherited_frame_wire_flag::HasSite)) != 0)
      parent_source = do_read_site(function_call_depth, call_site);

    let const parent_source_generation = source_generation_for(parent_source);
    let frame_origin_text = String{heap_allocator(), frame_origin};
    let frame_source_path = String{heap_allocator(), source_path};
    source_store().source_frames().push(
        source_frame{steal(frame_origin_text), call_site, parent_source,
                     parent_source_generation, steal(frame_source_path),
                     static_cast<source_frame_kind>(kind)});
    let &frame = source_store().source_frames().back();
    frame.function_call_depth = function_call_depth;
    frame.was_printed =
        (flags & static_cast<u8>(inherited_frame_wire_flag::WasPrinted)) != 0;
    frame.is_source_changing =
        (flags &
         static_cast<u8>(inherited_frame_wire_flag::IsSourceChanging)) != 0;
    frame.should_defer_trace =
        (flags &
         static_cast<u8>(inherited_frame_wire_flag::ShouldDeferTrace)) != 0;
  }

  if (!payload.is_fully_read()) invalid_subshell_bootstrap();

  source_store().set_script_run(is_script_run);
  if (!has_origin) return false;

  let const window_length =
      origin_prefix.length + contents.count() + origin_suffix.length;
  if (window_length > UINT32_MAX) invalid_subshell_bootstrap();

  source_name = source_name_at(origin_name_index);
  windows.push(String{heap_allocator()});
  let &window = windows.back();
  source_store().push_line_base(&window, usize{origin_line_number} - 1);
  window.reserve(window_length);
  window.append(origin_prefix);
  let const body_position = window.count();
  window.append(contents.view());
  window.append(origin_suffix);

  let const call_depth = function_store().call_frames().count();
  do_register_extra_lines(window, origin_extra_line_count, origin_name_index,
                          call_depth);
  source_store().push_embedded_source(embedded_source{
      contents.view(), &window,
      SourceLocation{body_position, contents.count(), origin_name_index},
      0,
      &contents, call_depth,
      source_depth_floor(source_store().source_frames().count())
  });

  return true;
}

fn EvalContext::apply_subshell_bootstrap(
    os::subshell_bootstrap bootstrap) throws -> void
{
  if (bootstrap.source_length > bootstrap.payload.count())
    invalid_subshell_bootstrap();
  let const encoded = bootstrap.payload.view().substring(
      static_cast<usize>(bootstrap.source_length));
  let reader = subshell_bootstrap_reader{encoded};
  if (reader.read_u32() != SUBSHELL_BOOTSTRAP_MAGIC ||
      reader.read_u32() != SUBSHELL_BOOTSTRAP_VERSION)
  {
    invalid_subshell_bootstrap();
  }
  let const body_length = static_cast<usize>(reader.read_u32());
  if (!reader.is_valid || body_length != reader.get_remaining_length())
    invalid_subshell_bootstrap();

  let execution = execution_wire{};
  let jobs = job_table_wire{};
  let clock = dynamic_clock_state{};
  let getopts = getopts_cursor{};
  let runtime = RuntimeState{};
  let variables = variable_wire{};
  let startup = startup_wire{};
  bool is_source_traces_enabled = true;
  let functions = function_wire{};
  let local_scopes = ArrayList<ArrayList<local_binding>>{heap_allocator()};
  let completion = completion_snapshot{
      StringMap<completion_spec>{heap_allocator()}, None, None, None};
  u64 startup_ignored_signals = 0;
  let control = runtime_control_wire{};
  let execution_cache =
      StringMap<ProgramResolver::CacheEntry>{heap_allocator()};
  if (!ExecutionStore::from_wire(reader, execution) ||
      !JobTable::from_wire(reader, jobs) ||
      !dynamic_clock_state::from_wire(reader, clock) ||
      !getopts_cursor::from_wire(reader, getopts) ||
      !RuntimeState::from_wire(reader, runtime) ||
      !VariableStore::from_wire(reader, variables) ||
      !StartupStore::from_wire(reader, startup) ||
      !FunctionStore::from_wire(reader, functions) ||
      !ScopeStore::from_wire(reader, local_scopes) ||
      !CompletionStore::from_wire(reader, completion) ||
      !TrapStore::from_wire(reader, startup_ignored_signals) ||
      !RuntimeControlStore::from_wire(reader, control) ||
      !ProgramResolver::from_wire(reader, execution_cache) ||
      !DiagnosticsStore::from_wire(reader, is_source_traces_enabled))
  {
    invalid_subshell_bootstrap();
  }

  if (!reader.is_fully_read() || !jobs.bind_processes(bootstrap)) {
    invalid_subshell_bootstrap();
  }

  let replay_runtime = runtime;
  replay_runtime.set_option(shell_option_id::Allexport, false);
  replay_runtime.set_option(shell_option_id::Errexit, false);
  replay_runtime.set_option(shell_option_id::Noexec, false);
  replay_runtime.set_option(shell_option_id::Nounset, false);
  replay_runtime.set_option(shell_option_id::Restricted, false);
  replay_runtime.set_option(shell_option_id::ShowAst, false);
  replay_runtime.set_option(shell_option_id::ShowLexedWords, false);
  replay_runtime.set_option(shell_option_id::Verbose, false);
  replay_runtime.set_option(shell_option_id::Xtrace, false);
  replay_runtime.restore(*this);
  variable_store().apply_wire_masks(variables);
  {
    trap_store().is_replaying_inherited_state() = true;
    defer { trap_store().is_replaying_inherited_state() = false; };
    run_source(bootstrap.payload.view().substring_of_length(
                   0, static_cast<usize>(bootstrap.source_length)),
               "inherited shell state");
  }
  function_store().apply_wire_definitions(functions);
  if (startup.is_restricted_shell) startup_store().request_restricted_shell();
  startup_store().set_login_shell(startup.is_login_shell);
  startup_store().set_init_moods(startup.init_moods.view());
  diagnostics_store().set_source_traces_enabled(is_source_traces_enabled);
  runtime.restore(*this);

  execution_store().apply_wire(steal(execution));
  dynamic_runtime_store().set_clock(clock);
  trap_store().startup_ignored_signals() = startup_ignored_signals;
  expansion_store().set_getopts_cursor(getopts);
  variable_store().apply_wire(steal(variables));
  function_store().apply_wire_depth(functions);
  lower_trap_depths_to_current();
  scope_store().apply_wire(steal(local_scopes));
  runtime_control_store().apply_wire(control);
  program_resolver().apply_wire(steal(execution_cache));
  for (let const &name : functions.call_names) {
    let const *storage = function_store().find_storage(name.view());
    push_function_call_name(
        name.view(), storage != nullptr ? *storage : FunctionBodyHandle{});
  }
  completion_store().restore(steal(completion));
  job_table_store().apply_wire(steal(jobs));
  job_table_store().inherit_parent_jobs(false);
  bootstrap.release_process_ownership();
}

fn EvalContext::option_flags_string() const throws -> String
{
  return enabled_shell_option_letters(*this);
}

fn EvalContext::apply_indirect_or_name_listing(StringView body) throws -> String
{
  LOG(All, "applying the indirect expansion '${!%.*s}'",
      static_cast<int>(body.length), body.data);
  if (body.is_empty()) return String{scratch_allocator()};

  if (body.length >= 4 && body[body.length - 1] == ']' &&
      (body[body.length - 2] == '@' || body[body.length - 2] == '*') &&
      body[body.length - 3] == '[' && lexer::is_variable_name_start(body[0]))
  {
    let const array_name = body.substring_of_length(0, body.length - 3);
    let const subscripts = collect_array_subscripts(array_name);
    let out = String{scratch_allocator()};
    for (usize i = 0; i < subscripts.count(); i++) {
      if (i > 0) out.push(' ');
      out.append(subscripts[i].view());
    }
    return out;
  }

  let const last = body[body.length - 1];
  if (last == '*' || last == '@') {
    let const prefix = body.substring_of_length(0, body.length - 1);
    let const names = matching_prefix_names(prefix);
    let out = String{scratch_allocator()};
    for (usize i = 0; i < names.count(); i++) {
      if (i > 0) out.push(' ');
      out.append(names[i].view());
    }
    return out;
  }

  let const target = get_variable_value(body);
  if (!target.has_value()) {
    if (runtime_state().error_unset())
      throw_script_fatal("Unable to expand '" + body +
                         "' because the parameter is not set");
    return String{scratch_allocator()};
  }
  let const target_view = target->view();
  if (let const bracket = target_view.find_character('[');
      bracket.has_value() && target_view[target_view.length - 1] == ']')
  {
    return apply_array_subscript(
        target_view.substring_of_length(0, *bracket),
        target_view.substring_of_length(*bracket + 1,
                                        target_view.length - *bracket - 2));
  }
  if (!get_variable_value(target_view).has_value())
    report_unset_reference(*target);
  return expand_variable(target_view);
}

cold fn EvalContext::make_stats_string() const throws -> String
{
  let const allocator = heap_allocator();
  let stats_text = String{allocator};

  let const append_line = [&](StringView name, StringView value) throws {
    stats_text += EXPRESSION_DOUBLE_AST_INDENT;
    stats_text += name;
    stats_text += ": ";
    stats_text += value;
    stats_text += '\n';
  };

  let const append_count_line = [&](StringView name, usize value) throws {
    append_line(name, String::from(value, allocator).view());
  };

  let const append_size_line = [&](StringView name, usize bytes) throws {
    let const value = koshkit::format_human_size(bytes, allocator);
    append_line(name, value.view());
  };

  const usize live_ast_arena_bytes =
      arena_store().parse_arena() != nullptr
          ? arena_store().parse_arena()->bytes_used()
          : 0;
  usize peak_ast_arena_bytes =
      evaluation_metrics_store().peak_ast_arena_bytes();
  if (live_ast_arena_bytes > peak_ast_arena_bytes)
    peak_ast_arena_bytes = live_ast_arena_bytes;

  stats_text += "[Stats\n";

  append_count_line("Commands evaluated",
                    evaluation_metrics_store().commands_evaluated() + 1);
  append_line("Last command duration",
              utils::format_duration_nanoseconds(
                  execution_store().last_command_duration_nanos(), allocator)
                  .view());
  append_count_line("Expansions",
                    evaluation_metrics_store().last_expansion_count());
  append_count_line("Nodes evaluated",
                    evaluation_metrics_store().last_expressions_executed());
  append_count_line("Total expansions",
                    evaluation_metrics_store().total_expansion_count());
  append_count_line("Total nodes evaluated",
                    evaluation_metrics_store().total_expressions_executed());
  append_size_line("AST arena used", live_ast_arena_bytes);
  append_size_line("AST arena peak", peak_ast_arena_bytes);
  if (arena_store().parse_arena() != nullptr)
    append_size_line("AST arena capacity",
                     arena_store().parse_arena()->bytes_capacity());

  let const function_stats = function_storage_stats();
  append_size_line("Function arenas used", function_stats.bytes_used);
  append_size_line("Function arenas capacity", function_stats.bytes_capacity);
  append_count_line("Function arena blocks", function_stats.block_count);
  append_count_line("Function destructors", function_stats.destructor_count);
  append_count_line("Shell variables",
                    variable_store().shell_variables().count());
  append_count_line("Functions", function_store().definitions().count());
  append_count_line("Function call depth", function_store().call_depth());
  append_count_line("Source frames", bash_source_frame_count());
  append_count_line("Builtins run",
                    evaluation_metrics_store().get_builtins_run());
  append_count_line("Functions run",
                    evaluation_metrics_store().get_functions_run());
  append_count_line("External commands run",
                    evaluation_metrics_store().get_external_commands_run());

  let const launch_counts = os::get_process_launch_counts();
  append_count_line("Forks", static_cast<usize>(launch_counts.fork_count));
  append_count_line("Execs", static_cast<usize>(launch_counts.exec_count));

  let const usage = os::read_own_resource_usage();
  let const do_append_optional_count = [&](StringView name,
                                           const Maybe<u64> &value) throws {
    if (value.has_value()) append_count_line(name, static_cast<usize>(*value));
  };
  let const do_append_optional_size = [&](StringView name,
                                          const Maybe<u64> &value) throws {
    if (value.has_value()) append_size_line(name, static_cast<usize>(*value));
  };
  let const do_append_optional_duration = [&](StringView name,
                                              const Maybe<u64> &value) throws {
    if (value.has_value())
      append_line(name,
                  utils::format_duration_nanoseconds(*value, allocator).view());
  };
  do_append_optional_size("Bytes read", usage.read_byte_count);
  do_append_optional_size("Bytes written", usage.written_byte_count);
  do_append_optional_count("Read calls", usage.read_call_count);
  do_append_optional_count("Write calls", usage.write_call_count);
  do_append_optional_duration("User time", usage.user_nanos);
  do_append_optional_duration("System time", usage.system_nanos);
  do_append_optional_size("Peak RSS", usage.peak_rss_bytes);
  do_append_optional_count("Voluntary context switches",
                           usage.voluntary_context_switch_count);
  do_append_optional_count("Involuntary context switches",
                           usage.involuntary_context_switch_count);
  do_append_optional_count("Minor page faults", usage.minor_fault_count);
  do_append_optional_count("Major page faults", usage.major_fault_count);
  do_append_optional_count("Page faults", usage.page_fault_count);

  os::malloc_heap_stats heap_stats{};
  if (os::read_malloc_heap_stats(heap_stats)) {
    append_size_line("Malloc heap in use", heap_stats.bytes_in_use);
    append_size_line("Malloc heap arena", heap_stats.arena_bytes);
    append_size_line("Malloc heap mapped", heap_stats.mapped_bytes);
  }

  stats_text += "]";

  return stats_text;
}

} /* namespace koshka */
