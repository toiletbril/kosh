/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements state evaluation. It applies the corresponding shell
 * semantics through EvalContext while preserving state, source locations,
 * and allocation ownership.
 */

#include "Arena.hpp"
#include "Cli.hpp"
#include "Common.hpp"
#include "Debug.hpp"
#include "Errors.hpp"
#include "Eval.hpp"
#include "Expressions.hpp"
#include "Koshkit.hpp"
#include "Lexer.hpp"
#include "Parser.hpp"
#include "Path.hpp"
#include "Platform.hpp"
#include "StaticStringMap.hpp"
#include "Toiletline.hpp"
#include "Trace.hpp"
#include "Utils.hpp"

namespace koshka {

fn EvalContext::enter_subshell() wontthrow -> void
{
  m_subshell_depth++;
  LOG(Debug, "entered a subshell, depth now %zu", m_subshell_depth);
}

pure fn EvalContext::get_subshell_depth() const wontthrow -> usize
{
  return m_subshell_depth;
}

fn EvalContext::set_subshell_depth(usize depth) wontthrow -> void
{
  m_subshell_depth = depth;
}

fn EvalContext::leave_subshell() wontthrow -> void
{
  ASSERT(m_subshell_depth > 0);
  /* Stacked exec moves unwind newest first so the descriptors land back in
     order. */
  while (!m_subshell_saved_descriptors.is_empty() &&
         m_subshell_saved_descriptors.back().depth == m_subshell_depth)
  {
    LOG(Debug, "restoring descriptor %d a subshell exec moved at depth %zu",
        m_subshell_saved_descriptors.back().saved.shell_fd, m_subshell_depth);
    os::restore_descriptor(m_subshell_saved_descriptors.back().saved);
    m_subshell_saved_descriptors.remove(m_subshell_saved_descriptors.count() -
                                        1);
  }
  m_subshell_depth--;
  LOG(Debug, "left a subshell, depth now %zu", m_subshell_depth);
}

fn EvalContext::snapshot_subshell_descriptor(i32 shell_fd) throws -> void
{
  if (m_subshell_depth == 0) return;
  for (let const &entry : m_subshell_saved_descriptors) {
    if (entry.depth == m_subshell_depth && entry.saved.shell_fd == shell_fd) {
      return;
    }
  }
  LOG(Debug,
      "backing up descriptor %d before a subshell exec moves it at depth %zu",
      shell_fd, m_subshell_depth);
  m_subshell_saved_descriptors.push(subshell_saved_descriptor{
      m_subshell_depth, os::save_descriptor(shell_fd)});
}

pure fn EvalContext::in_subshell() const wontthrow -> bool
{
  return m_subshell_depth > 0;
}

fn EvalContext::request_loop_control(control_flow::Kind kind, i64 level,
                                     SourceLocation location) throws -> void
{
  if (m_loop_depth == 0) {
    LOG(Debug, "loop control requested outside a loop, ignored");
    return;
  }
  if (static_cast<usize>(level) > m_loop_depth)
    level = static_cast<i64>(m_loop_depth);
  LOG(All, "loop control requested, level %lld of depth %zu", (long long) level,
      m_loop_depth);
  m_control_flow = control_flow{kind, level, location, m_current_source,
                                String{m_current_origin}};
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
  m_control_flow = control_flow{control_flow::Kind::Return, status, location,
                                m_current_source, String{m_current_origin}};
}

fn EvalContext::request_exit(i64 status, SourceLocation location) throws -> void
{
  LOG(Debug, "exit requested, status %lld", (long long) status);
  m_control_flow = control_flow{control_flow::Kind::Exit, status, location,
                                m_current_source, String{m_current_origin}};
}

pure fn EvalContext::has_pending_control_flow() const wontthrow -> bool
{
  return m_control_flow.kind != control_flow::Kind::Normal;
}

fn EvalContext::pending_control_flow() wontthrow -> control_flow &
{
  return m_control_flow;
}

pure fn EvalContext::pending_control_flow() const wontthrow
    -> const control_flow &
{
  return m_control_flow;
}

fn EvalContext::clear_control_flow() wontthrow -> void
{
  m_control_flow.kind = control_flow::Kind::Normal;
}

fn EvalContext::set_current_source(const String *source,
                                   String origin) wontthrow -> void
{
  reset_runtime_diagnostic_highlight_cache();
  m_current_source = source;
  m_current_origin = steal(origin);
}

pure fn EvalContext::current_source() const wontthrow -> const String *
{
  return m_current_source;
}

pure fn EvalContext::current_origin() const wontthrow -> const String &
{
  return m_current_origin;
}

fn EvalContext::set_current_history_event_number(Maybe<usize> number) wontthrow
    -> void
{
  m_current_history_event_number = steal(number);
}

pure fn EvalContext::current_history_event_number() const wontthrow
    -> Maybe<usize>
{
  return m_current_history_event_number;
}

fn EvalContext::push_root_source_frame(const String *parent_source,
                                       SourceLocation call_site,
                                       bool is_only_root_source) throws -> void
{
  m_source_frames.push(source_frame{
      String{heap_allocator(), StringView{"the command line"}},
      call_site,
      parent_source, String{heap_allocator()},
      true, is_only_root_source
  });
}

fn EvalContext::pop_root_source_frame() wontthrow -> void
{
  if (!m_source_frames.is_empty()) m_source_frames.pop_back();
}

fn EvalContext::print_source_backtrace(Maybe<SourceLocation> error_location,
                                       bool should_defer_for_source_file) throws
    -> void
{
  if (!m_should_print_source_traces) return;

  if (should_defer_for_source_file) {
    for (usize i = m_source_frames.count(); i > 0; i--) {
      let &frame = m_source_frames[i - 1];
      if (!frame.should_defer_trace) continue;
      if (error_location.has_value()) {
        let const error_source_name = error_location->get_filename();
        if (!error_source_name.has_value() ||
            *error_source_name != frame.source_path.view())
        {
          break;
        }
      }
      frame.has_deferred_trace = true;
      frame.deferred_trace_location = error_location;
      return;
    }
  }

  let const do_location_match = [](SourceLocation left, SourceLocation right) {
    return left.has_same_source_as(right) && left.position == right.position &&
           left.length == right.length;
  };
  let const do_frame_repeat_error = [&](const source_frame &frame) {
    return error_location.has_value() &&
           do_location_match(frame.call_site, *error_location);
  };
  let const do_frame_identity_match = [&](const source_frame &left,
                                          const source_frame &right) {
    return left.is_cli_root == right.is_cli_root &&
           do_location_match(left.call_site, right.call_site) &&
           left.origin == right.origin &&
           left.source_path == right.source_path &&
           left.parent_source_length == right.parent_source_length &&
           (left.parent_source == right.parent_source ||
            (left.parent_source != nullptr && right.parent_source != nullptr &&
             left.parent_source->view() == right.parent_source->view()));
  };
  let const do_frame_render = [&](const source_frame &frame) {
    return frame.parent_source != nullptr && !do_frame_repeat_error(frame);
  };

  let has_traceable_source_frame = false;
  for (let const &frame : m_source_frames)
    if (frame.parent_source != nullptr &&
        (!frame.is_cli_root || !frame.is_only_root_source))
    {
      has_traceable_source_frame = true;
      break;
    }
  if (!has_traceable_source_frame) return;

  for (usize i = m_source_frames.count(); i > 0; i--) {
    let &frame = m_source_frames[i - 1];
    if (!do_frame_render(frame) || frame.was_printed) {
      continue;
    }

    let is_repeated_frame = false;
    for (usize other_index = m_source_frames.count(); other_index > i;
         other_index--)
    {
      let const &other = m_source_frames[other_index - 1];
      if (!do_frame_render(other) || !do_frame_identity_match(frame, other)) {
        continue;
      }
      is_repeated_frame = true;
      break;
    }
    if (is_repeated_frame) {
      frame.was_printed = true;
      continue;
    }

    let const sourced_here = TraceWithLocation{frame.call_site};
    show_message(sourced_here.to_string(*frame.parent_source, this));
    frame.was_printed = true;
  }
}

fn EvalContext::set_current_location(SourceLocation location) wontthrow -> void
{
  m_current_location = location;
}

/* TODO: these caps are hand-tuned below the observed native overflow point.
   Query the actual stack size per platform, getrlimit RLIMIT_STACK on POSIX and
   the thread stack on Windows, and derive the caps from it. */
static constexpr usize MAX_SOURCE_DEPTH = 400;
static constexpr usize MAX_FUNCTION_CALL_DEPTH = 900;
/* Command substitution spends the most native frames per level, a sanitizer
   build overflows past two hundred so the cap stays well below. */
static constexpr usize MAX_SUBSTITUTION_DEPTH = 64;
static constexpr usize MAX_PARAMETER_EXPANSION_DEPTH = 256;

static fn guard_located_depth(usize current_depth, usize cap,
                              [[maybe_unused]] const char *what,
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
  guard_located_depth(m_source_depth, MAX_SOURCE_DEPTH, "source", location);
  m_source_depth++;
}

fn EvalContext::leave_source() wontthrow -> void
{
  ASSERT(m_source_depth > 0);
  m_source_depth--;
}

fn EvalContext::enter_function_call(const SourceLocation &location) throws
    -> void
{
  guard_located_depth(m_function_call_depth, MAX_FUNCTION_CALL_DEPTH,
                      "function call", location);
  m_function_call_depth++;
  LOG(Debug, "entered function call depth %zu", m_function_call_depth);
}

fn EvalContext::leave_function_call() wontthrow -> void
{
  ASSERT(m_function_call_depth > 0);
  m_function_call_depth--;
}

fn EvalContext::enter_substitution() throws -> void
{
  if (m_substitution_depth >= MAX_SUBSTITUTION_DEPTH) {
    LOG(Debug, "substitution depth %zu exceeds cap %zu", m_substitution_depth,
        MAX_SUBSTITUTION_DEPTH);
    throw Error{"Command substitution nested too deeply"};
  }
  m_substitution_depth++;
}

fn EvalContext::leave_substitution() wontthrow -> void
{
  ASSERT(m_substitution_depth > 0);
  m_substitution_depth--;
}

pure fn EvalContext::get_substitution_depth() const wontthrow -> usize
{
  return m_substitution_depth;
}

fn EvalContext::enter_parameter_expansion() throws -> void
{
  if (m_parameter_expansion_depth >= MAX_PARAMETER_EXPANSION_DEPTH) {
    LOG(Debug, "parameter expansion depth %zu exceeds cap %zu",
        m_parameter_expansion_depth, MAX_PARAMETER_EXPANSION_DEPTH);
    throw Error{"Parameter expansion nested too deeply"};
  }
  m_parameter_expansion_depth++;
}

fn EvalContext::leave_parameter_expansion() wontthrow -> void
{
  ASSERT(m_parameter_expansion_depth > 0);
  m_parameter_expansion_depth--;
}

fn EvalContext::set_error_exit(bool enabled) wontthrow -> void
{
  LOG(Info, "the errexit option flips to %s", enabled ? "on" : "off");
  m_runtime.set_option(shell_option_id::Errexit, enabled);
}

pure fn EvalContext::error_exit() const wontthrow -> bool
{
  return m_runtime.option_is_enabled(shell_option_id::Errexit);
}

fn EvalContext::set_echo_expanded(bool enabled) wontthrow -> void
{
  LOG(Info, "the xtrace option flips to %s", enabled ? "on" : "off");
  m_runtime.set_option(shell_option_id::Xtrace, enabled);
}

fn EvalContext::set_error_unset(bool enabled) wontthrow -> void
{
  LOG(Info, "the nounset option flips to %s", enabled ? "on" : "off");
  m_runtime.set_option(shell_option_id::Nounset, enabled);
}

pure fn EvalContext::error_unset() const wontthrow -> bool
{
  return m_runtime.option_is_enabled(shell_option_id::Nounset);
}

fn EvalContext::set_pipefail(bool enabled) wontthrow -> void
{
  LOG(Info, "the pipefail option flips to %s", enabled ? "on" : "off");
  m_runtime.set_option(shell_option_id::Pipefail, enabled);
}

pure fn EvalContext::pipefail() const wontthrow -> bool
{
  return m_runtime.option_is_enabled(shell_option_id::Pipefail);
}

fn EvalContext::set_no_clobber(bool enabled) wontthrow -> void
{
  LOG(Info, "the noclobber option flips to %s", enabled ? "on" : "off");
  m_runtime.set_option(shell_option_id::Noclobber, enabled);
}

pure fn EvalContext::no_clobber() const wontthrow -> bool
{
  return m_runtime.option_is_enabled(shell_option_id::Noclobber);
}

fn EvalContext::set_export_all(bool enabled) wontthrow -> void
{
  LOG(Info, "the allexport option flips to %s", enabled ? "on" : "off");
  m_runtime.set_option(shell_option_id::Allexport, enabled);
}

pure fn EvalContext::export_all() const wontthrow -> bool
{
  return m_runtime.option_is_enabled(shell_option_id::Allexport);
}

fn EvalContext::set_stats_enabled(bool enabled) wontthrow -> void
{
  m_runtime.set_option(shell_option_id::ShowStats, enabled);
}

pure fn EvalContext::stats_enabled() const wontthrow -> bool
{
  return m_runtime.option_is_enabled(shell_option_id::ShowStats);
}

fn EvalContext::set_no_glob(bool enabled) wontthrow -> void
{
  LOG(Info, "the noglob option flips to %s", enabled ? "on" : "off");
  m_runtime.set_option(shell_option_id::Noglob, enabled);
}

pure fn EvalContext::no_glob() const wontthrow -> bool
{
  return m_runtime.option_is_enabled(shell_option_id::Noglob);
}

fn EvalContext::set_no_exec(bool enabled) wontthrow -> void
{
  LOG(Info, "the noexec option flips to %s", enabled ? "on" : "off");
  m_runtime.set_option(shell_option_id::Noexec, enabled);
}

pure fn EvalContext::no_exec() const wontthrow -> bool
{
  return m_runtime.option_is_enabled(shell_option_id::Noexec);
}

fn EvalContext::set_koshkit(bool enabled) wontthrow -> void
{
  LOG(Info, "the koshkit option flips to %s", enabled ? "on" : "off");
  m_runtime.set_option(shell_option_id::Koshkit, enabled);
}

pure fn EvalContext::koshkit() const wontthrow -> bool
{
  return m_runtime.option_is_enabled(shell_option_id::Koshkit);
}

fn EvalContext::set_failglob(bool enabled) wontthrow -> void
{
  LOG(Info, "the failglob option flips to %s", enabled ? "on" : "off");
  m_runtime.set_option(shell_option_id::Failglob, enabled);
}

pure fn EvalContext::failglob() const wontthrow -> bool
{
  return m_runtime.option_is_enabled(shell_option_id::Failglob);
}

fn EvalContext::enter_condition() wontthrow -> void { m_condition_depth++; }

fn EvalContext::leave_condition() wontthrow -> void
{
  ASSERT(m_condition_depth > 0);
  m_condition_depth--;
}

pure fn EvalContext::in_condition() const wontthrow -> bool
{
  return m_condition_depth > 0;
}

fn EvalContext::enter_loop() wontthrow -> void { m_loop_depth++; }

fn EvalContext::leave_loop() wontthrow -> void
{
  ASSERT(m_loop_depth > 0);
  m_loop_depth--;
}

/* The count is bounded so a target past the bound reopens every iteration
   the way bash does, instead of exhausting the descriptor table. */
static constexpr usize MAX_LOOP_REDIRECT_FDS = 16;

fn EvalContext::mark_loop_redirect_fds() const wontthrow
    -> loop_redirect_fd_mark
{
  return {m_loop_redirect_fds.count()};
}

fn EvalContext::cleanup_loop_redirect_fds(loop_redirect_fd_mark mark) wontthrow
    -> void
{
  for (usize i = m_loop_redirect_fds.count(); i > mark.count; i--)
    os::close_fd(m_loop_redirect_fds[i - 1].fd);

  while (m_loop_redirect_fds.count() > mark.count)
    m_loop_redirect_fds.remove(m_loop_redirect_fds.count() - 1);
}

fn EvalContext::find_loop_redirect_fd(i32 target_fd, const String &path,
                                      os::file_open_mode mode) const wontthrow
    -> Maybe<os::descriptor>
{
  for (let const &entry : m_loop_redirect_fds) {
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
  if (m_loop_redirect_fds.count() >= MAX_LOOP_REDIRECT_FDS) return false;

  m_loop_redirect_fds.push(loop_redirect_fd{
      target_fd, mode, String{heap_allocator(), path.view()},
        fd
  });
  return true;
}

pure fn EvalContext::loop_depth() const wontthrow -> usize
{
  return m_loop_depth;
}

fn EvalContext::set_loop_depth(usize depth) wontthrow -> void
{
  m_loop_depth = depth;
}

fn EvalContext::set_terminal_exec_allowed(bool enabled) wontthrow -> void
{
  m_terminal_exec_allowed = enabled;
}

pure fn EvalContext::terminal_exec_allowed() const wontthrow -> bool
{
  return m_terminal_exec_allowed;
}

pure fn EvalContext::getopts_char_index() const wontthrow -> usize
{
  return m_getopts_char_index;
}

fn EvalContext::set_getopts_char_index(usize index) wontthrow -> void
{
  m_getopts_char_index = index;
}

pure fn EvalContext::getopts_last_optind() const wontthrow -> i64
{
  return m_getopts_last_optind;
}

fn EvalContext::set_getopts_last_optind(i64 optind) wontthrow -> void
{
  m_getopts_last_optind = optind;
}

fn EvalContext::suggest_similar_variable_name(StringView name) const throws
    -> Maybe<String>
{
  if (name.is_empty()) return None;

  let suggestion = utils::NameSuggestion{name};
  m_shell_variables.for_each(
      [&suggestion](StringView candidate, const String &)
          throws -> void { suggestion.consider(candidate); });
  m_indexed_arrays.for_each(
      [&suggestion](StringView candidate, const ArrayList<String> &)
          throws -> void { suggestion.consider(candidate); });
  m_associative_names.for_each(
      [&suggestion](StringView candidate)
          throws -> void { suggestion.consider(candidate); });
  m_exported_names.for_each([&suggestion](StringView candidate) throws -> void {
    suggestion.consider(candidate);
  });

  let dynamic_names = ArrayList<StringView>{heap_allocator()};
  append_dynamic_variable_names(dynamic_names);
  for (let const dynamic_name : dynamic_names)
    suggestion.consider(dynamic_name);

  return suggestion.take_suggestion();
}

fn EvalContext::sorted_variable_assignments() const throws -> ArrayList<String>
{
  let assignments = ArrayList<String>{heap_allocator()};
  assignments.reserve(m_shell_variables.count());
  m_shell_variables.for_each([&](StringView name, const String &value) {
    let entry = String{heap_allocator(), name};
    entry.push('=');
    entry.append(value);
    assignments.push(steal(entry));
  });
  assignments.sort();
  return assignments;
}

fn EvalContext::clear_functions() wontthrow -> void { m_functions.clear(); }

fn EvalContext::snapshot_state() throws -> eval_state_snapshot
{
  let working_directory = os::reference_current_directory();
  if (!working_directory.is_valid())
    throw Error{"Could not preserve the current working directory"};

  let snapshot = eval_state_snapshot{m_shell_variables,
                                     m_indexed_arrays,
                                     m_completion_specs,
                                     m_default_completion_spec,
                                     m_associative_names,
                                     m_associative_values,
                                     m_sparse_array_values,
                                     m_sparse_array_names,
                                     m_shopt_option_overrides,
                                     m_shopt_option_values,
                                     m_functions,
                                     m_aliases,
                                     m_positional_params,
                                     m_last_argument,
                                     m_directory_stack,
                                     steal(working_directory),
                                     os::get_file_creation_mask(),
                                     m_traps,
                                     m_variable_attributes,
                                     m_exported_names,
                                     m_environment_undo_log.count(),
                                     RuntimeState::capture(*this),
                                     m_program_resolver,
                                     m_init_moods_sourcing,
                                     m_initialized_moods,
                                     m_was_mood_set_explicitly,
                                     m_mood_mutation_revision,
                                     m_warning_mutation_revision,
                                     m_diagnostics_mutation_revision,
                                     m_annoying_diagnostics_mutation_revision,
                                     m_random_state,
                                     m_shell_option_mutations,
                                     m_local_scopes,
                                     m_local_scope_depth,
                                     m_last_background_pid,
                                     m_getopts_char_index,
                                     m_getopts_last_optind,
                                     m_terminal_exec_allowed,
                                     steal(m_jobs),
                                     steal(m_detached_job_processes),
                                     m_next_job_id};
  m_next_job_id = 1;
  return snapshot;
}

fn EvalContext::restore_state(eval_state_snapshot snapshot) throws -> void
{
  LOG(Debug, "restoring the evaluator state after a subshell or substitution");
  m_shell_variables = steal(snapshot.shell_variables);
  m_indexed_arrays = steal(snapshot.indexed_arrays);
  m_completion_specs = steal(snapshot.completion_specs);
  m_default_completion_spec = steal(snapshot.default_completion_spec);
  m_associative_names = steal(snapshot.associative_names);
  m_associative_values = steal(snapshot.associative_values);
  m_sparse_array_values = steal(snapshot.sparse_array_values);
  m_sparse_array_names = steal(snapshot.sparse_array_names);
  m_shopt_option_overrides = snapshot.shopt_option_overrides;
  m_shopt_option_values = snapshot.shopt_option_values;
  m_functions = steal(snapshot.functions);
  m_aliases = steal(snapshot.aliases);
  m_positional_params = steal(snapshot.positional_params);
  m_last_argument = steal(snapshot.last_argument);
  m_directory_stack = steal(snapshot.directory_stack);

  snapshot.runtime.restore(*this);
  m_program_resolver = steal(snapshot.program_resolver);
  m_init_moods_sourcing = snapshot.init_moods_sourcing;
  m_initialized_moods = snapshot.initialized_moods;
  m_was_mood_set_explicitly = snapshot.was_mood_set_explicitly;
  m_mood_mutation_revision = snapshot.mood_mutation_revision;
  m_warning_mutation_revision = snapshot.warning_mutation_revision;
  m_diagnostics_mutation_revision = snapshot.diagnostics_mutation_revision;
  m_annoying_diagnostics_mutation_revision =
      snapshot.annoying_diagnostics_mutation_revision;
  m_random_state = snapshot.random_state;
  m_shell_option_mutations = snapshot.option_mutations;
  m_local_scopes = steal(snapshot.local_scopes);
  m_local_scope_depth = snapshot.local_scope_depth;
  m_last_background_pid = snapshot.last_background_pid;
  m_getopts_char_index = snapshot.getopts_char_index;
  m_getopts_last_optind = snapshot.getopts_last_optind;
  m_terminal_exec_allowed = snapshot.terminal_exec_allowed;

  for (let &child_job : m_jobs) {
    if (child_job.is_primary_process_active)
      m_detached_job_processes.push(child_job.pid);
    for (let const process : child_job.earlier_pipeline_processes)
      m_detached_job_processes.push(process);
  }
  snapshot.detached_job_processes.reserve(
      snapshot.detached_job_processes.count() +
      m_detached_job_processes.count());
  for (let const process : m_detached_job_processes)
    snapshot.detached_job_processes.push(process);
  m_jobs = steal(snapshot.jobs);
  m_detached_job_processes = steal(snapshot.detached_job_processes);
  m_next_job_id = snapshot.next_job_id;

  m_variable_attributes = steal(snapshot.variable_attributes);
  m_exported_names = steal(snapshot.exported_names);

  /* A signal the subshell trapped that the parent does not is returned to
     default before the parent's dispositions are reinstalled. */
  if (m_traps.count() != 0 || snapshot.traps.count() != 0) {
    m_traps.for_each([&](StringView condition, const String &action) {
      unused(action);
      if (condition == "EXIT") return;
      if (snapshot.traps.find(condition) != nullptr) return;
      if (let const number = os::signal_number_from_name(condition))
        os::clear_trap_handler(*number);
    });
    m_traps = steal(snapshot.traps);
    install_trap_dispositions();
  } else {
    m_traps = steal(snapshot.traps);
  }
  m_has_debug_trap = m_traps.find(StringView{"DEBUG", 5}) != nullptr;

  if (!os::restore_current_directory(snapshot.working_directory))
    LOG(Debug, "the subshell could not restore the working directory");
  os::set_file_creation_mask(snapshot.file_creation_mask);

  /* The logged environment writes revert newest first, before the PATH re-point
     below so an exported PATH reads its restored value. */
  LOG(Debug, "rewinding %zu environment writes made inside the subshell",
      m_environment_undo_log.count() - snapshot.environment_undo_mark);
  while (m_environment_undo_log.count() > snapshot.environment_undo_mark) {
    let const &entry = m_environment_undo_log.back();
    if (entry.previous_value)
      os::set_environment_variable(entry.name.view(),
                                   entry.previous_value->view());
    else
      os::unset_environment_variable(entry.name.view());
    m_environment_undo_log.pop_back();
  }

  if (let const *ifs = m_shell_variables.find(StringView{"IFS", 3});
      ifs != nullptr)
    set_field_separators(ifs->view());
  else
    set_field_separators(" \t\n");

  /* The exit status is intentionally not restored, a subshell propagates its
     last command's status to the parent. */
}

static constexpr u32 SUBSHELL_BOOTSTRAP_MAGIC = 0x4b534842U;
static constexpr u32 SUBSHELL_BOOTSTRAP_VERSION = 5U;
static constexpr u32 NO_BOOTSTRAP_PROCESS = UINT32_MAX;
static constexpr u8 SUBSHELL_BOOTSTRAP_RUNTIME_FLAGS = 0x3fU;

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

static fn append_subshell_bootstrap_runtime(String &output,
                                            const RuntimeState &runtime) throws
    -> void
{
  output.push(static_cast<char>(runtime.mood));
  output.push(static_cast<char>(runtime.warning_level));
  u8 flags = 0;
  if (runtime.is_diagnostics_disabled()) flags |= 1U << 0;
  if (runtime.is_annoying_diagnostics_enabled()) flags |= 1U << 1;
  if (runtime.was_error_unset_set_explicitly()) flags |= 1U << 2;
  if (runtime.was_pipefail_set_explicitly()) flags |= 1U << 3;
  if (runtime.was_failglob_set_explicitly()) flags |= 1U << 4;
  if (runtime.was_extended_arithmetic_set_explicitly()) flags |= 1U << 5;
  output.push(static_cast<char>(flags));
  append_subshell_bootstrap_u64(output, runtime.shell_options);
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

static fn read_subshell_bootstrap_runtime(subshell_bootstrap_reader &reader,
                                          RuntimeState &runtime) wontthrow
    -> bool
{
  let const mood = reader.read_u8();
  let const warning_level = reader.read_u8();
  let const flags = reader.read_u8();
  let const shell_options = reader.read_u64();
  static_assert(static_cast<u8>(shell_option_id::Count) < 64);
  let const valid_shell_options =
      (u64{1} << static_cast<u8>(shell_option_id::Count)) - 1U;
  if (!reader.is_valid || mood > static_cast<u8>(mimic_mood::BashPosix) ||
      warning_level > 3 || (flags & ~SUBSHELL_BOOTSTRAP_RUNTIME_FLAGS) != 0 ||
      (shell_options & ~valid_shell_options) != 0)
  {
    return false;
  }

  runtime.mood = static_cast<mimic_mood>(mood);
  runtime.warning_level = warning_level;
  runtime.shell_options = shell_options;
  runtime.set_diagnostics_disabled((flags & (1U << 0)) != 0);
  runtime.set_annoying_diagnostics_enabled((flags & (1U << 1)) != 0);
  runtime.set_error_unset_set_explicitly((flags & (1U << 2)) != 0);
  runtime.set_pipefail_set_explicitly((flags & (1U << 3)) != 0);
  runtime.set_failglob_set_explicitly((flags & (1U << 4)) != 0);
  runtime.set_extended_arithmetic_set_explicitly((flags & (1U << 5)) != 0);
  return true;
}

[[noreturn]] static fn invalid_subshell_bootstrap() throws -> void
{
  throw Error{"Invalid inherited shell state"};
}

fn EvalContext::make_subshell_bootstrap() const throws -> os::subshell_bootstrap
{
  let bootstrap = os::subshell_bootstrap{};
  String &source = bootstrap.payload;
  let names = ArrayList<String>{heap_allocator()};
  variable_names().for_each([&](StringView name) { names.push_managed(name); });
  names.sort();

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
    let const is_integer = is_integer_variable(name);
    let const is_lowercase = is_lowercase_variable(name);
    let const is_uppercase = is_uppercase_variable(name);
    let const is_read_only = is_readonly(name);
    let const is_exported_value = is_exported(name);
    let const *indexed = lookup_indexed_array(name);
    let const is_associative = is_associative_array(name);
    let const value = get_variable_value(name);
    if (indexed == nullptr && !is_associative && is_exported_value &&
        !is_integer && !is_lowercase && !is_uppercase && !is_read_only)
    {
      let const environment_value = os::get_environment_variable(name);
      if (environment_value.has_value()) continue;
    }

    source += is_associative       ? "declare -A"
              : indexed != nullptr ? "declare -a"
                                   : "declare -";
    if (is_integer) source.push('i');
    if (is_lowercase) source.push('l');
    if (is_uppercase) source.push('u');
    if (is_exported_value) source.push('x');
    if (indexed == nullptr && !is_associative && !is_integer && !is_lowercase &&
        !is_uppercase && !is_exported_value)
    {
      source.push('-');
    }
    source.push(' ');
    source.append(name);
    if (indexed == nullptr && !is_associative && value.has_value()) {
      source.push('=');
      append_shell_quoted_arg(source, value->view());
    }
    source.push('\n');

    if (indexed != nullptr || is_associative) {
      let const subscripts = collect_array_subscripts(name);
      let const values = collect_array_elements(name);
      ASSERT(subscripts.count() == values.count());
      for (usize index = 0; index < subscripts.count(); index++)
        do_append_assignment(name, subscripts[index].view(),
                             values[index].view());
    }
  }

  for (let const &name : sorted_function_names()) {
    let const *function_source = find_function_source(name.view());
    if (function_source == nullptr || function_source->is_empty()) {
      continue;
    }
    source.append(function_source->view());
    source.push('\n');
  }
  for (let const &definition : alias_definitions()) {
    source += "alias ";
    source.append(definition.view());
    source.push('\n');
  }
  traps().for_each([&](StringView condition, const String &action) throws {
    if (condition == "EXIT") return;
    source += "trap -- ";
    append_shell_quoted_arg(source, action.view());
    source.push(' ');
    append_shell_quoted_arg(source, condition);
    source.push('\n');
  });

  source += "set --";
  for (let const &parameter : positional_params()) {
    source.push(' ');
    append_shell_quoted_arg(source, parameter.view());
  }
  source.push('\n');

  let const working_directory = Path::current_directory();
  if (!directory_stack().is_empty()) {
    source += "builtin cd -- ";
    append_shell_quoted_arg(source, directory_stack()[0].view());
    source.push('\n');
    for (usize index = 1; index < directory_stack().count(); index++) {
      source += "pushd ";
      append_shell_quoted_arg(source, directory_stack()[index].view());
      source += " >/dev/null\n";
    }
    source += "pushd ";
    append_shell_quoted_arg(source, working_directory.text().view());
    source += " >/dev/null\n";
  } else {
    source += "builtin cd -- ";
    append_shell_quoted_arg(source, working_directory.text().view());
    source.push('\n');
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
  if (is_restricted_shell()) source += "set -r\n";

  if (source.count() > UINT32_MAX) throw std::bad_alloc{};
  bootstrap.source_length = static_cast<u32>(source.count());

  let body = String{heap_allocator()};
  append_subshell_bootstrap_text(body, m_last_argument.view());
  body.push(static_cast<char>(m_last_background_pid.has_value()));
  if (m_last_background_pid.has_value())
    append_subshell_bootstrap_i64(body, *m_last_background_pid);
  append_subshell_bootstrap_u64(body, m_random_state);
  append_subshell_bootstrap_u64(body, static_cast<u64>(m_getopts_char_index));
  append_subshell_bootstrap_i64(body, m_getopts_last_optind);
  append_subshell_bootstrap_i32(body, m_next_job_id);
  append_subshell_bootstrap_runtime(body, RuntimeState::capture(*this));
  append_subshell_bootstrap_u64(body, m_shopt_option_overrides);
  append_subshell_bootstrap_u64(body, m_shopt_option_values);
  append_subshell_bootstrap_u64(body, static_cast<u64>(m_function_call_depth));
  append_subshell_bootstrap_u64(body, static_cast<u64>(m_local_scope_depth));
  append_subshell_bootstrap_u32(
      body, static_cast<u32>(m_function_call_names.count()));
  for (let const &name : m_function_call_names)
    append_subshell_bootstrap_text(body, name.view());

  let completion_names = ArrayList<String>{heap_allocator()};
  m_completion_specs.for_each([&](StringView command, const completion_spec &) {
    completion_names.push_managed(command);
  });
  completion_names.sort();
  append_subshell_bootstrap_u32(body,
                                static_cast<u32>(completion_names.count()));

  let const do_append_completion_spec = [&](const completion_spec &spec)
                                            throws -> void {
    append_subshell_bootstrap_text(body, spec.function_name.view());
    append_subshell_bootstrap_text(body, spec.word_list.view());
    body.push(static_cast<char>(spec.should_use_default));
    append_subshell_bootstrap_runtime(body, spec.defining_runtime);
  };

  for (let const &command : completion_names) {
    let const *spec = m_completion_specs.find(command.view());
    ASSERT(spec != nullptr);
    append_subshell_bootstrap_text(body, command.view());
    do_append_completion_spec(*spec);
  }

  body.push(static_cast<char>(m_default_completion_spec.has_value()));
  if (m_default_completion_spec.has_value())
    do_append_completion_spec(*m_default_completion_spec);

  let const do_reference_process = [&](os::process process) throws -> u32 {
    if (bootstrap.processes.count() >= UINT32_MAX) throw std::bad_alloc{};
    let const process_index = static_cast<u32>(bootstrap.processes.count());
    bootstrap.processes.push(process);
    return process_index;
  };

  append_subshell_bootstrap_u32(body, static_cast<u32>(m_jobs.count()));
  for (let const &child_job : m_jobs) {
    append_subshell_bootstrap_i32(body, child_job.id);
    append_subshell_bootstrap_text(body, child_job.command.view());
    append_subshell_bootstrap_i64(body, child_job.process_id);
    append_subshell_bootstrap_i64(body, child_job.process_group_id);
    append_subshell_bootstrap_i32(body, child_job.last_status);
    append_subshell_bootstrap_i32(body, child_job.stopped_status);
    body.push(static_cast<char>(child_job.state));
    body.push(static_cast<char>(child_job.is_primary_process_active));
    body.push(static_cast<char>(child_job.has_unreported_state_change));
    append_subshell_bootstrap_u32(body,
                                  child_job.is_primary_process_active
                                      ? do_reference_process(child_job.pid)
                                      : NO_BOOTSTRAP_PROCESS);
    append_subshell_bootstrap_u32(
        body, static_cast<u32>(child_job.earlier_pipeline_processes.count()));
    for (let const process : child_job.earlier_pipeline_processes)
      append_subshell_bootstrap_u32(body, do_reference_process(process));
  }

  append_subshell_bootstrap_u32(
      body, static_cast<u32>(m_detached_job_processes.count()));
  for (let const process : m_detached_job_processes)
    append_subshell_bootstrap_u32(body, do_reference_process(process));

  if (body.count() > UINT32_MAX) throw std::bad_alloc{};
  append_subshell_bootstrap_u32(source, SUBSHELL_BOOTSTRAP_MAGIC);
  append_subshell_bootstrap_u32(source, SUBSHELL_BOOTSTRAP_VERSION);
  append_subshell_bootstrap_u32(source, static_cast<u32>(body.count()));
  source.append(body.view());
  return bootstrap;
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

  let last_argument = String{heap_allocator(), reader.read_text()};
  bool has_last_background_pid = false;
  if (!read_subshell_bootstrap_bool(reader, has_last_background_pid))
    invalid_subshell_bootstrap();
  let last_background_pid = Maybe<i64>{None};
  if (has_last_background_pid) last_background_pid = reader.read_i64();
  let const random_state = reader.read_u64();
  let const getopts_char_index_bits = reader.read_u64();
  let const getopts_last_optind = reader.read_i64();
  let const next_job_id = reader.read_i32();
  let runtime = RuntimeState{};
  if (!read_subshell_bootstrap_runtime(reader, runtime))
    invalid_subshell_bootstrap();
  let const shopt_option_overrides = reader.read_u64();
  let const shopt_option_values = reader.read_u64();
  let const function_call_depth = reader.read_u64();
  let const local_scope_depth = reader.read_u64();
  let function_call_names = ArrayList<String>{heap_allocator()};
  let const function_call_name_count = static_cast<usize>(reader.read_u32());
  if (!reader.is_valid || function_call_name_count > function_call_depth ||
      function_call_name_count > reader.get_remaining_length() / sizeof(u32))
  {
    invalid_subshell_bootstrap();
  }
  function_call_names.reserve(function_call_name_count);
  for (usize index = 0; index < function_call_name_count; index++)
    function_call_names.push(String{heap_allocator(), reader.read_text()});
  if (getopts_char_index_bits == 0 || getopts_char_index_bits > SIZE_MAX ||
      next_job_id < 1 || (shopt_option_values & ~shopt_option_overrides) != 0 ||
      function_call_depth > MAX_FUNCTION_CALL_DEPTH ||
      local_scope_depth > MAX_FUNCTION_CALL_DEPTH)
  {
    invalid_subshell_bootstrap();
  }
  let const getopts_char_index = static_cast<usize>(getopts_char_index_bits);

  let completion_specs = StringMap<completion_spec>{heap_allocator()};
  let const completion_spec_count = static_cast<usize>(reader.read_u32());
  constexpr usize MINIMUM_COMPLETION_SPEC_BYTES = 24;
  if (!reader.is_valid ||
      completion_spec_count >
          reader.get_remaining_length() / MINIMUM_COMPLETION_SPEC_BYTES)
  {
    invalid_subshell_bootstrap();
  }
  completion_specs.reserve(completion_spec_count);
  let const do_read_completion_spec = [&](completion_spec &spec)
                                          throws -> bool {
    let const function_name = reader.read_text();
    let const word_list = reader.read_text();
    bool should_use_default = false;
    if (!read_subshell_bootstrap_bool(reader, should_use_default) ||
        !read_subshell_bootstrap_runtime(reader, spec.defining_runtime))
    {
      return false;
    }
    spec.function_name = String{heap_allocator(), function_name};
    spec.word_list = String{heap_allocator(), word_list};
    spec.should_use_default = should_use_default;
    return true;
  };

  for (usize spec_index = 0; spec_index < completion_spec_count; spec_index++) {
    let const command = reader.read_text();
    let spec = completion_spec{};
    if (!reader.is_valid || !do_read_completion_spec(spec) ||
        completion_specs.find(command) != nullptr)
    {
      invalid_subshell_bootstrap();
    }
    completion_specs.set(command, steal(spec));
  }

  bool has_default_completion_spec = false;
  if (!read_subshell_bootstrap_bool(reader, has_default_completion_spec))
    invalid_subshell_bootstrap();
  let default_completion_spec = Maybe<completion_spec>{None};
  if (has_default_completion_spec) {
    let spec = completion_spec{};
    if (!do_read_completion_spec(spec)) invalid_subshell_bootstrap();
    default_completion_spec = steal(spec);
  }

  let jobs = ArrayList<job>{heap_allocator()};
  let const job_count = static_cast<usize>(reader.read_u32());
  constexpr usize MINIMUM_JOB_BYTES = 43;
  if (!reader.is_valid ||
      job_count > reader.get_remaining_length() / MINIMUM_JOB_BYTES)
  {
    invalid_subshell_bootstrap();
  }
  jobs.reserve(job_count);
  let process_references = ArrayList<u32>{heap_allocator()};
  i32 previous_job_id = 0;

  for (usize job_index = 0; job_index < job_count; job_index++) {
    let child_job = job{};
    child_job.id = reader.read_i32();
    let const command = reader.read_text();
    child_job.command = String{heap_allocator(), command};
    child_job.process_id = reader.read_i64();
    child_job.process_group_id = reader.read_i64();
    child_job.last_status = reader.read_i32();
    child_job.stopped_status = reader.read_i32();
    let const state = reader.read_u8();
    bool is_primary_process_active = false;
    bool has_unreported_state_change = false;
    if (!read_subshell_bootstrap_bool(reader, is_primary_process_active) ||
        !read_subshell_bootstrap_bool(reader, has_unreported_state_change) ||
        child_job.id <= previous_job_id || child_job.id >= next_job_id ||
        child_job.process_group_id < 0 ||
        state > static_cast<u8>(job::State::Done))
    {
      invalid_subshell_bootstrap();
    }
    previous_job_id = child_job.id;
    child_job.state = static_cast<job::State>(state);
    child_job.is_primary_process_active = is_primary_process_active;
    child_job.has_unreported_state_change = has_unreported_state_change;

    let const primary_process_index = reader.read_u32();
    if (!reader.is_valid ||
        (is_primary_process_active &&
         primary_process_index == NO_BOOTSTRAP_PROCESS) ||
        (!is_primary_process_active &&
         primary_process_index != NO_BOOTSTRAP_PROCESS) ||
        (child_job.state == job::State::Done && is_primary_process_active))
    {
      invalid_subshell_bootstrap();
    }
    child_job.pid = KOSH_INVALID_PROCESS;
    if (is_primary_process_active)
      process_references.push(primary_process_index);

    let const earlier_process_count = static_cast<usize>(reader.read_u32());
    if (!reader.is_valid ||
        earlier_process_count > reader.get_remaining_length() / sizeof(u32))
    {
      invalid_subshell_bootstrap();
    }
    if ((child_job.state == job::State::Done && earlier_process_count != 0) ||
        (child_job.state != job::State::Done && !is_primary_process_active &&
         earlier_process_count == 0))
    {
      invalid_subshell_bootstrap();
    }

    child_job.earlier_pipeline_processes.reserve(earlier_process_count);
    for (usize process_index = 0; process_index < earlier_process_count;
         process_index++)
    {
      process_references.push(reader.read_u32());
      child_job.earlier_pipeline_processes.push(KOSH_INVALID_PROCESS);
    }
    jobs.push(steal(child_job));
  }

  let detached_processes = ArrayList<os::process>{heap_allocator()};
  let const detached_process_count = static_cast<usize>(reader.read_u32());
  if (!reader.is_valid ||
      detached_process_count > reader.get_remaining_length() / sizeof(u32))
  {
    invalid_subshell_bootstrap();
  }
  detached_processes.reserve(detached_process_count);
  for (usize process_index = 0; process_index < detached_process_count;
       process_index++)
  {
    process_references.push(reader.read_u32());
    detached_processes.push(KOSH_INVALID_PROCESS);
  }

  if (!reader.is_valid || reader.position != encoded.length ||
      process_references.count() != bootstrap.processes.count())
  {
    invalid_subshell_bootstrap();
  }
  let referenced_processes = Bitset{heap_allocator()};
  referenced_processes.reset(bootstrap.processes.count());
  for (let const process_index : process_references) {
    if (process_index >= bootstrap.processes.count() ||
        referenced_processes[process_index])
    {
      invalid_subshell_bootstrap();
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

  let replay_runtime = runtime;
  replay_runtime.set_option(shell_option_id::Allexport, false);
  replay_runtime.set_option(shell_option_id::Errexit, false);
  replay_runtime.set_option(shell_option_id::Noexec, false);
  replay_runtime.set_option(shell_option_id::Nounset, false);
  replay_runtime.set_option(shell_option_id::Restricted, false);
  replay_runtime.set_option(shell_option_id::Verbose, false);
  replay_runtime.set_option(shell_option_id::Xtrace, false);
  replay_runtime.restore(*this);
  let const is_restricted =
      runtime.option_is_enabled(shell_option_id::Restricted);
  run_source(bootstrap.payload.view().substring_of_length(
                 0, static_cast<usize>(bootstrap.source_length)),
             "inherited shell state");
  if (is_restricted) request_restricted_shell();
  runtime.restore(*this);

  m_last_argument = steal(last_argument);
  m_last_background_pid = last_background_pid;
  m_random_state = random_state;
  m_getopts_char_index = getopts_char_index;
  m_getopts_last_optind = getopts_last_optind;
  m_shopt_option_overrides = shopt_option_overrides;
  m_shopt_option_values = shopt_option_values;
  m_function_call_depth = static_cast<usize>(function_call_depth);
  for (usize scope = 0; scope < static_cast<usize>(local_scope_depth); scope++)
    enter_function_scope();
  for (let const &name : function_call_names) {
    let const *storage = find_function_storage(name.view());
    if (storage == nullptr) invalid_subshell_bootstrap();
    push_function_call_name(name.view(), *storage);
  }
  m_completion_specs = steal(completion_specs);
  m_default_completion_spec = steal(default_completion_spec);
  m_jobs = steal(jobs);
  m_detached_job_processes = steal(detached_processes);
  bootstrap.release_process_ownership();
  m_next_job_id = next_job_id;
}

fn EvalContext::option_flags_string() const throws -> String
{
  return enabled_shell_option_letters(*this);
}

fn EvalContext::set_last_exit_status(i32 status) wontthrow -> void
{
  m_last_exit_status = status;
}

fn EvalContext::set_last_command_duration_nanos(u64 nanos) wontthrow -> void
{
  m_last_command_duration_nanos = nanos;
}

pure fn EvalContext::last_command_duration_nanos() const wontthrow -> u64
{
  return m_last_command_duration_nanos;
}

pure fn EvalContext::last_exit_status() const wontthrow -> i32
{
  return m_last_exit_status;
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
    /* The quoted "${!prefix@}" per-name field form is produced in the
       field-expansion path, this string return cannot carry field boundaries.
     */
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
    if (error_unset())
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
  let stats_text = String{heap_allocator()};

  /* Stats print before end_command runs the rollup, so the live arena is
     sampled here. */
  const usize live_ast_arena_bytes =
      AST_ARENA != nullptr ? AST_ARENA->bytes_used() : 0;
  usize peak_ast_arena_bytes = m_peak_ast_arena_bytes;
  if (live_ast_arena_bytes > peak_ast_arena_bytes)
    peak_ast_arena_bytes = live_ast_arena_bytes;

  stats_text += "[Stats\n";

  stats_text += EXPRESSION_DOUBLE_AST_INDENT;
  stats_text += "Commands evaluated: " +
                String::from(m_commands_evaluated + 1, heap_allocator());
  stats_text += '\n';
  stats_text += EXPRESSION_DOUBLE_AST_INDENT;
  stats_text +=
      "Expansions: " + String::from(last_expansion_count(), heap_allocator());
  stats_text += '\n';
  stats_text += EXPRESSION_DOUBLE_AST_INDENT;
  stats_text += "Nodes evaluated: " +
                String::from(last_expressions_executed(), heap_allocator());
  stats_text += '\n';
  stats_text += EXPRESSION_DOUBLE_AST_INDENT;
  stats_text += "Total expansions: " +
                String::from(total_expansion_count(), heap_allocator());
  stats_text += '\n';
  stats_text += EXPRESSION_DOUBLE_AST_INDENT;
  stats_text += "Total nodes evaluated: " +
                String::from(total_expressions_executed(), heap_allocator());
  stats_text += '\n';
  stats_text += EXPRESSION_DOUBLE_AST_INDENT;
  stats_text += "AST arena bytes: " +
                String::from(live_ast_arena_bytes, heap_allocator());
  stats_text += '\n';
  stats_text += EXPRESSION_DOUBLE_AST_INDENT;
  stats_text += "Peak AST arena bytes: " +
                String::from(peak_ast_arena_bytes, heap_allocator());
  stats_text += '\n';

  stats_text += "]";

  return stats_text;
}

pure fn EvalContext::should_echo() const wontthrow -> bool
{
  return m_runtime.option_is_enabled(shell_option_id::Verbose);
}

pure fn EvalContext::should_echo_expanded() const wontthrow -> bool
{
  return m_runtime.option_is_enabled(shell_option_id::Xtrace);
}

pure fn EvalContext::shell_is_interactive() const wontthrow -> bool
{
  return m_shell_is_interactive;
}

fn EvalContext::set_show_ast(bool enabled) wontthrow -> void
{
  m_runtime.set_option(shell_option_id::ShowAst, enabled);
}

pure fn EvalContext::show_ast() const wontthrow -> bool
{
  return m_runtime.option_is_enabled(shell_option_id::ShowAst);
}

fn EvalContext::set_show_lexed_words(bool enabled) wontthrow -> void
{
  m_runtime.set_option(shell_option_id::ShowLexedWords, enabled);
}

pure fn EvalContext::show_lexed_words() const wontthrow -> bool
{
  return m_runtime.option_is_enabled(shell_option_id::ShowLexedWords);
}

fn EvalContext::set_show_exit_code(bool enabled) wontthrow -> void
{
  m_runtime.set_option(shell_option_id::ShowExitCode, enabled);
}

pure fn EvalContext::show_exit_code() const wontthrow -> bool
{
  return m_runtime.option_is_enabled(shell_option_id::ShowExitCode);
}

fn EvalContext::set_memory_stats_enabled(bool enabled) wontthrow -> void
{
  m_runtime.set_option(shell_option_id::ShowMemory, enabled);
}

pure fn EvalContext::memory_stats_enabled() const wontthrow -> bool
{
  return m_runtime.option_is_enabled(shell_option_id::ShowMemory);
}

fn EvalContext::set_diagnostics_disabled(bool disabled) wontthrow -> void
{
  m_runtime.set_diagnostics_disabled(disabled);
}

pure fn EvalContext::diagnostics_disabled() const wontthrow -> bool
{
  return m_runtime.is_diagnostics_disabled();
}

fn EvalContext::set_annoying_diagnostics_enabled(bool enabled) wontthrow -> void
{
  m_runtime.set_annoying_diagnostics_enabled(enabled);
}

pure fn EvalContext::annoying_diagnostics_enabled() const wontthrow -> bool
{
  return m_runtime.is_annoying_diagnostics_enabled();
}

fn EvalContext::set_login_shell(bool enabled) wontthrow -> void
{
  m_is_login_shell = enabled;
}

pure fn EvalContext::is_login_shell() const wontthrow -> bool
{
  return m_is_login_shell;
}

fn EvalContext::set_custom_rcfile(bool enabled) wontthrow -> void
{
  m_has_custom_rcfile = enabled;
}

pure fn EvalContext::has_custom_rcfile() const wontthrow -> bool
{
  return m_has_custom_rcfile;
}

pure fn EvalContext::last_expressions_executed() const wontthrow -> usize
{
  return m_expressions_executed_last;
}

pure fn EvalContext::total_expressions_executed() const wontthrow -> usize
{
  return m_expressions_executed_total + m_expressions_executed_last;
}

pure fn EvalContext::last_expansion_count() const wontthrow -> usize
{
  return m_expansions_last;
}

pure fn EvalContext::total_expansion_count() const wontthrow -> usize
{
  return m_expansions_total + m_expansions_last;
}

pure fn EvalContext::commands_evaluated() const wontthrow -> usize
{
  return m_commands_evaluated;
}

pure fn EvalContext::peak_ast_arena_bytes() const wontthrow -> usize
{
  return m_peak_ast_arena_bytes;
}

/* The arithmetic engine, the ArithmeticParser, the cached-token fast path, and
   the EvalContext arithmetic methods, lives in EvalArithmetic.cpp. */

} /* namespace koshka */
