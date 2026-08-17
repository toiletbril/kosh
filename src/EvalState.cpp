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

fn EvalContext::enter_subshell() wontthrow -> void
{
  m_subshell_depth++;
  LOG(Debug, "entered a subshell, depth now %zu", m_subshell_depth);
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
  m_control_flow = control_flow{kind, level, steal(location), m_current_source,
                                String{m_current_origin}};
}

fn EvalContext::request_break(i64 level, SourceLocation location) throws -> void
{
  request_loop_control(control_flow::Kind::Break, level, steal(location));
}

fn EvalContext::request_continue(i64 level, SourceLocation location) throws
    -> void
{
  request_loop_control(control_flow::Kind::Continue, level, steal(location));
}

fn EvalContext::request_return(i64 status, SourceLocation location) throws
    -> void
{
  LOG(Debug, "return requested, status %lld", (long long) status);
  m_control_flow =
      control_flow{control_flow::Kind::Return, status, steal(location),
                   m_current_source, String{m_current_origin}};
}

fn EvalContext::request_exit(i64 status, SourceLocation location) throws -> void
{
  LOG(Debug, "exit requested, status %lld", (long long) status);
  m_control_flow =
      control_flow{control_flow::Kind::Exit, status, steal(location),
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
      steal(call_site), parent_source, String{heap_allocator()},
      true,
      is_only_root_source
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
      if (error_location.has_value() &&
          (!error_location->filename.has_value() ||
           *error_location->filename != frame.source_path.view()))
      {
        break;
      }
      frame.has_deferred_trace = true;
      frame.deferred_trace_location = error_location;
      return;
    }
  }

  let const do_location_match = [](SourceLocation left, SourceLocation right) {
    let const same_file =
        left.filename.has_value() == right.filename.has_value() &&
        (!left.filename.has_value() || *left.filename == *right.filename);
    return same_file && left.position == right.position &&
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
  m_current_location = steal(location);
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

fn EvalContext::clear_functions() wontthrow -> void
{
  m_functions.clear();
  m_function_sources.clear();
  m_function_definition_infos.clear();
}

fn EvalContext::snapshot_state() const throws -> eval_state_snapshot
{
  let working_directory = os::reference_current_directory();
  if (!working_directory.is_valid())
    throw Error{"Could not preserve the current working directory"};

  return eval_state_snapshot{m_shell_variables,
                             m_indexed_arrays,
                             m_associative_names,
                             m_associative_values,
                             m_sparse_array_values,
                             m_sparse_array_names,
                             m_shopt_options,
                             m_functions,
                             m_function_sources,
                             m_function_definition_infos,
                             m_aliases,
                             m_positional_params,
                             m_last_argument,
                             m_directory_stack,
                             steal(working_directory),
                             os::get_file_creation_mask(),
                             m_traps,
                             m_readonly_names,
                             m_integer_names,
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
                             m_shell_option_mutations};
}

fn EvalContext::restore_state(eval_state_snapshot snapshot) throws -> void
{
  LOG(Debug, "restoring the evaluator state after a subshell or substitution");
  m_shell_variables = steal(snapshot.shell_variables);
  m_indexed_arrays = steal(snapshot.indexed_arrays);
  m_associative_names = steal(snapshot.associative_names);
  m_associative_values = steal(snapshot.associative_values);
  m_sparse_array_values = steal(snapshot.sparse_array_values);
  m_sparse_array_names = steal(snapshot.sparse_array_names);
  m_shopt_options = steal(snapshot.shopt_options);
  m_functions = steal(snapshot.functions);
  m_function_sources = steal(snapshot.function_sources);
  m_function_definition_infos = steal(snapshot.function_definition_infos);
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

  m_readonly_names = steal(snapshot.readonly_names);
  m_integer_names = steal(snapshot.integer_names);
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
  const usize peak_ast_arena_bytes =
      live_ast_arena_bytes > m_peak_ast_arena_bytes ? live_ast_arena_bytes
                                                    : m_peak_ast_arena_bytes;

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
  m_runtime.is_diagnostics_disabled = disabled;
}

pure fn EvalContext::diagnostics_disabled() const wontthrow -> bool
{
  return m_runtime.is_diagnostics_disabled;
}

fn EvalContext::set_annoying_diagnostics_enabled(bool enabled) wontthrow -> void
{
  m_runtime.is_annoying_diagnostics_enabled = enabled;
}

pure fn EvalContext::annoying_diagnostics_enabled() const wontthrow -> bool
{
  return m_runtime.is_annoying_diagnostics_enabled;
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
