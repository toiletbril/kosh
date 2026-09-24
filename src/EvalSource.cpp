/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file parses and evaluates source text, sourced files, heredoc bodies,
 * and scripts delegated to compatibility shells. It owns source isolation,
 * mood initialization, retained syntax trees, path resolution, and fallback
 * execution. The split confines recursive source lifetimes and compatibility
 * delegation outside ordinary evaluator operations.
 */

#include "base/Arena.hpp"
#include "CLI.hpp"
#include "base/Common.hpp"
#include "base/Debug.hpp"
#include "Errors.hpp"
#include "Eval.hpp"
#include "Expressions.hpp"
#include "Lexer.hpp"
#include "Parser.hpp"
#include "base/Path.hpp"
#include "Platform.hpp"
#include "base/Trace.hpp"
#include "Utils.hpp"

namespace koshka {

static constexpr usize MAX_MIMICRY_DEPTH = 16;

enum class mimicked_error_status_mode : u8
{
  Default,
  Posix,
};

static fn mimicked_error_is_interrupt(const std::exception_ptr &error) throws
    -> bool
{
  if (error == nullptr) return false;

  try {
    std::rethrow_exception(error);
  } catch (const InterruptErrorWithLocation &) {
    return true;
  } catch (...) {
    return false;
  }
}

static fn mimicked_error_status(const std::exception_ptr &error,
                                mimicked_error_status_mode mode) throws -> i32
{
  ASSERT(error != nullptr);

  try {
    std::rethrow_exception(error);
  } catch (const ErrorBase &caught_error) {
    let const status = caught_error.command_status();
    return static_cast<i32>(
        status == 1 && mode == mimicked_error_status_mode::Posix &&
                caught_error.is_script_fatal()
            ? 2
            : status);
  } catch (...) {
    return 1;
  }
}

fn EvalContext::run_program_fallback(ExecContext &ec, mimic_mood mode,
                                     script_isolation isolation) throws -> i32
{
  struct saved_environment_variable
  {
    String name;
    String value;
  };

  let saved_environment =
      ArrayList<saved_environment_variable>{heap_allocator()};
  if (ec.should_use_empty_environment) {
    let const environment_names = os::environment_names();
    saved_environment.reserve(environment_names.count());
    for (let const &name : environment_names) {
      if (let value = os::get_environment_variable(name.view())) {
        saved_environment.push(
            saved_environment_variable{String{name.view()}, value.take()});
      }
      os::unset_environment_variable(name.view());
    }
  }
  defer
  {
    for (let const &variable : saved_environment)
      os::set_environment_variable(variable.name.view(), variable.value.view());
  };

  let fallback_context = EvalContext{false, false, false, false};
  fallback_context.set_current_source(
      current_source(), String{heap_allocator(), current_origin().view()});
  fallback_context.source_store().set_mimicry_depth(
      source_store().mimicry_depth());
  fallback_context.source_store().m_retained_source_generation = source_store().m_retained_source_generation;
  fallback_context.set_shell_executable_path(String{shell_executable_path()});
  fallback_context.set_koshkit(koshkit());
  fallback_context.set_mimicry(mimicry());
  fallback_context.set_warning_level(warning_level());
  fallback_context.set_diagnostics_disabled(diagnostics_disabled());
  fallback_context.set_source_traces_enabled(should_print_source_traces());
  fallback_context.source_store().m_source_frames.reserve(source_store().m_source_frames.count());
  for (let const &frame : source_store().m_source_frames) {
    fallback_context.source_store().m_source_frames.push(source_frame{
        String{frame.origin.view()}, frame.call_site, frame.parent_source,
        frame.parent_source_generation, String{frame.source_path.view()},
        frame.is_cli_root, frame.is_only_root_source});
    fallback_context.source_store().m_source_frames.back().function_call_depth =
        frame.function_call_depth;
    fallback_context.source_store().m_source_frames.back().was_printed = frame.was_printed;
    fallback_context.source_store().m_source_frames.back().should_defer_trace =
        frame.should_defer_trace;
    fallback_context.source_store().m_source_frames.back().has_deferred_trace =
        frame.has_deferred_trace;
    fallback_context.source_store().m_source_frames.back().deferred_trace_location =
        frame.deferred_trace_location;
  }
  defer
  {
    let const shared_frame_count =
        source_store().m_source_frames.count() < fallback_context.source_store().m_source_frames.count()
            ? source_store().m_source_frames.count()
            : fallback_context.source_store().m_source_frames.count();
    for (usize frame_index = 0; frame_index < shared_frame_count; frame_index++)
    {
      source_store().m_source_frames[frame_index].was_printed =
          source_store().m_source_frames[frame_index].was_printed ||
          fallback_context.source_store().m_source_frames[frame_index].was_printed;
      source_store().m_source_frames[frame_index].has_deferred_trace =
          source_store().m_source_frames[frame_index].has_deferred_trace ||
          fallback_context.source_store().m_source_frames[frame_index].has_deferred_trace;
      if (fallback_context.source_store().m_source_frames[frame_index]
              .deferred_trace_location.has_value())
      {
        source_store().m_source_frames[frame_index].deferred_trace_location =
            fallback_context.source_store().m_source_frames[frame_index]
                .deferred_trace_location;
      }
    }
  };
  return fallback_context.run_mimicked_script(ec, mode, isolation);
}

fn EvalContext::run_mimicked_script(ExecContext &ec, mimic_mood mode,
                                    script_isolation isolation) throws -> i32
{
  let const isolated = isolation == script_isolation::Isolated;
  defer { ec.close_fds(); };

  if (source_store().mimicry_depth() >= MAX_MIMICRY_DEPTH)
    throw ErrorWithLocation{ec.source_location(),
                            "Unable to mimic '" + ec.program() +
                                "' because the script nesting is too deep"};
  if (parse_arena() == nullptr)
    throw ErrorWithLocation{ec.source_location(), "Unable to mimic '" +
                                                      ec.program() +
                                                      "' outside of a parse"};
  let const ast_mark = parse_arena()->mark();
  defer { parse_arena()->release(ast_mark); };

  let contents = ec.program_path().read_entire_file();
  if (!contents.has_value())
    throw ErrorWithLocation{ec.source_location(),
                            "Unable to mimic '" + ec.program() +
                                "' because the script could not be read"};

  const usize binary_scan_limit = 128;
  let const head = contents->view();
  let const scan_length =
      head.length < binary_scan_limit ? head.length : binary_scan_limit;
  let const sample = head.substring_of_length(0, scan_length);
  let const first_line_break = sample.find_character('\n');
  let const first_line_length = first_line_break.value_or(sample.length);
  if (sample.substring_of_length(0, first_line_length)
          .find_character('\0')
          .has_value())
  {
    LOG(Debug,
        "a NUL byte before the first line break marks '%s' as a binary file",
        ec.program().c_str());
    let file_command = String{"file "};
    append_shell_quoted_arg(file_command, ec.program().view());
    let details =
        String{"The file is binary and the system has refused execution. "};
    details += "Use `";
    details += file_command;
    details += "` to check the file type.";
    let const source = current_source();
    show_message(ErrorWithLocationAndDetails{
        ec.source_location(),
        "Cannot execute `" + ec.program_path().text() + "` as a shell script.",
        steal(details)}
                     .to_string(source != nullptr ? source->view()
                                                  : StringView{},
                                this));
    return 126;
  }

  contents->normalize_crlf_line_endings();

  let const previous_runtime = m_runtime;
  let const was_restricted_shell = m_is_restricted_shell;
  let const previous_script_run = source_store().is_script_run();
  let previous_shell_name = String{m_shell_name};
  let const previous_source = source_store().m_current_source;
  let const previous_origin = source_store().m_current_origin;
  let const previous_location = source_store().m_current_location;
  let isolated_snapshot = Maybe<eval_state_snapshot>{};
  if (isolated) isolated_snapshot = snapshot_state();

  bool should_restore_isolated_state = isolated;
  let const do_restore_auxiliary_state = [&]() throws {
    set_current_source(previous_source, previous_origin);
    source_store().m_current_location = previous_location;
    previous_runtime.restore(*this);
    m_is_restricted_shell = was_restricted_shell;
    source_store().set_script_run(previous_script_run);
    m_shell_name = steal(previous_shell_name);
  };
  defer
  {
    if (should_restore_isolated_state) {
      try {
        restore_state(steal(*isolated_snapshot));
        do_restore_auxiliary_state();
      } catch (...) {
        LOG(Debug, "restoring an interrupted mimicked script failed");
      }
    }
  };

  m_runtime.mood = mode;
  LOG(Debug, "mimicking the script '%s'%s", ec.program().c_str(),
      isolated ? " in an isolated subshell" : "");
  source_store().set_script_run(true);

  /* A mimicked script runs with the strictness of the mood it mimics, so a bash
     or sh script clears nounset, pipefail, and failglob while a kosh script
     keeps the strict default. */
  let const is_mimic_strict = mode == mimic_mood::Default;
  set_error_unset(is_mimic_strict);
  set_pipefail(is_mimic_strict);
  set_failglob(is_mimic_strict);
  LOG(Debug, "seeded the strict options for the %s mimicked run",
      is_mimic_strict ? "kosh" : "lax");

  let const script_filename = ec.program_path().view();
  source_store().m_source_frames.push(source_frame{String{ec.program().view()},
                                    ec.source_location(), current_source(),
                                    source_generation_for(current_source()),
                                    String{script_filename}, false, false});
  source_store().m_source_frames.back().should_defer_trace = true;
  source_store().m_source_frames.back().function_call_depth =
      function_store().call_names().count();
  defer
  {
    let &frame = source_store().m_source_frames.back();
    if (frame.has_deferred_trace) {
      try {
        print_source_backtrace(frame.deferred_trace_location, false);
      } catch (...) {
        LOG(Debug, "rendering a deferred source trace failed");
      }
    }
    source_store().m_source_frames.pop_back();
  };
  let parser = Parser{
      Lexer{contents->view(), *parse_arena(), false, script_filename, mood()}
  };

  let params = ArrayList<String>{heap_allocator()};
  params.reserve(ec.args().count() - 1);
  for (usize i = 1; i < ec.args().count(); i++)
    params.push_managed(ec.args()[i].view());

  /* A standard descriptor with no staged redirect is backed up too, since the
     script may move it with an exec redirection that a fork would contain. */
  let saved_fds = ArrayList<os::saved_descriptor>{heap_allocator()};
  bool should_restore_fds = true;
  let const do_restore_fds = [&]() {
    for (usize i = saved_fds.count(); i > 0; i--)
      os::restore_descriptor(saved_fds[i - 1]);
    should_restore_fds = false;
  };
  defer
  {
    if (should_restore_fds) do_restore_fds();
  };
  saved_fds.push(
      ec.in_fd.has_value()
          ? os::save_and_replace_descriptor_out_of_reach(0, *ec.in_fd)
          : os::save_descriptor_out_of_reach(0));
  saved_fds.push(
      ec.out_fd.has_value()
          ? os::save_and_replace_descriptor_out_of_reach(1, *ec.out_fd)
          : os::save_descriptor_out_of_reach(1));
  saved_fds.push(
      ec.err_fd.has_value()
          ? os::save_and_replace_descriptor_out_of_reach(2, *ec.err_fd)
          : os::save_descriptor_out_of_reach(2));
  let const do_render_error = [&](const std::exception_ptr &error) {
    try {
      std::rethrow_exception(error);
    } catch (const ErrorWithLocationAndDetails &detailed_error) {
      show_message(detailed_error.to_string(contents->view(), this));
      show_message(detailed_error.details_to_string(contents->view(), this));
      print_source_backtrace(detailed_error.location());
    } catch (const ErrorWithLocation &located_error) {
      show_message(located_error.to_string(contents->view(), this));
      print_source_backtrace(located_error.location());
    } catch (const Error &caught_error) {
      show_message(caught_error.to_string());
      print_source_backtrace();
    }
  };
  let const do_finish_script = [&](std::exception_ptr &error, bool is_subshell)
                                   throws -> bool {
    let is_interrupt = mimicked_error_is_interrupt(error);
    i32 final_status = last_exit_status();
    bool was_error_rendered = false;
    if (error && !is_interrupt) {
      final_status = mimicked_error_status(
          error, is_posix_mode() ? mimicked_error_status_mode::Posix
                                 : mimicked_error_status_mode::Default);
      set_last_exit_status(final_status);
      do_render_error(error);
      was_error_rendered = true;
    }
    if (!is_interrupt) {
      try {
        if (is_subshell) {
          if (let const requested_status = run_subshell_exit_trap();
              requested_status.has_value())
          {
            final_status = *requested_status;
          }
        } else {
          run_exit_trap();
        }
      } catch (...) {
        if (!error) {
          error = std::current_exception();
          is_interrupt = mimicked_error_is_interrupt(error);
          if (!is_interrupt)
            final_status = mimicked_error_status(
                error, is_posix_mode() ? mimicked_error_status_mode::Posix
                                       : mimicked_error_status_mode::Default);
        }
      }
    }
    if (error && !is_interrupt && !was_error_rendered) {
      do_render_error(error);
    }
    set_last_exit_status(final_status);
    return is_interrupt;
  };

  /* The kernel hands a shebang interpreter the resolved script path, so $0 and
     BASH_SOURCE read that path rather than the word as typed. */
  m_shell_name =
      String{heap_allocator(), ec.should_use_fallback_argv0
                                   ? ec.args()[0].view()
                                   : ec.program_path().view()};
  set_current_source(&*contents, String{ec.program().view()});
  source_store().m_current_location = SourceLocation{};
  source_store().mimicry_depth()++;
  bool should_leave_mimicry = true;
  defer
  {
    if (should_leave_mimicry) source_store().mimicry_depth()--;
  };

  let const do_evaluate_script = [&]() throws {
    let const was_terminal_exec_allowed = terminal_exec_allowed();
    defer { set_terminal_exec_allowed(was_terminal_exec_allowed); };

    loop
    {
      let const *ast = parser.construct_next_top_level_ast();
      if (ast == nullptr) break;
      set_terminal_exec_allowed(was_terminal_exec_allowed &&
                                parser.is_at_end());
      ast->evaluate(*this);
      if (has_pending_control_flow()) break;
    }
  };

  /* The terminal command the shell exits with needs no isolation, so the script
     runs against the current state with no snapshot. */
  if (!isolated) {
    set_positional_params(steal(params));
    seed_shell_identity_variables(mode == mimic_mood::Bash);
    std::exception_ptr error;
    try {
      do_evaluate_script();
    } catch (...) {
      error = std::current_exception();
    }
    let const is_interrupt = do_finish_script(error, false);
    source_store().mimicry_depth()--;
    should_leave_mimicry = false;
    do_restore_fds();
    if (error) {
      if (is_interrupt) throw InterruptErrorWithLocation{previous_location};

      return last_exit_status();
    }
    return last_exit_status();
  }

  set_positional_params(steal(params));
  seed_shell_identity_variables(mode == mimic_mood::Bash);
  enter_subshell();
  clear_inherited_exit_trap();
  std::exception_ptr error;
  try {
    do_evaluate_script();
  } catch (...) {
    error = std::current_exception();
  }
  if (has_pending_control_flow()) {
    if (pending_control_flow().kind == control_flow::Kind::Exit)
      set_last_exit_status(static_cast<i32>(pending_control_flow().value));
    clear_control_flow();
  }
  let const is_interrupt = do_finish_script(error, true);
  leave_subshell();
  source_store().mimicry_depth()--;
  should_leave_mimicry = false;
  do_restore_fds();

  let const status = last_exit_status();
  should_restore_isolated_state = false;
  restore_state(steal(*isolated_snapshot));
  do_restore_auxiliary_state();
  if (error) {
    if (is_interrupt) throw InterruptErrorWithLocation{previous_location};

    return status;
  }
  return status;
}

pure fn EvalContext::shopt_default_is_on(StringView name) wontthrow -> bool
{
  static constexpr PackedStringKey KEYS[] = {
      SSK("progcomp"),           SSK("promptvars"),
      SSK("sourcepath"),         SSK("extquote"),
      SSK("complete_fullquote"), SSK("hostcomplete"),
      SSK("checkwinsize"),       SSK("force_fignore"),
      SSK("globasciiranges"),    SSK("globskipdots"),
      SSK("expand_aliases"),     SSK("interactive_comments"),
  };
  static constexpr StaticStringSet DEFAULT_ON_SHOPT_NAMES{KEYS};
  return DEFAULT_ON_SHOPT_NAMES.contains(name);
}

fn EvalContext::run_source(StringView source, StringView origin,
                           Maybe<SourceLocation> call_site,
                           Maybe<StringView> filename,
                           Maybe<i32> *status_before_return,
                           const FunctionBodyHandle *cached_body,
                           return_handling handling,
                           history_recording history) throws -> i32
{
  if (cached_body != nullptr && (cached_body->get_body() == nullptr ||
                                 cached_body->get_source() == nullptr))
  {
    cached_body = nullptr;
  }

  let normalized_source = String{heap_allocator()};
  if (cached_body == nullptr) {
    normalized_source = String{source};
    normalized_source.normalize_crlf_line_endings();
    source = normalized_source.view();
  } else {
    source = cached_body->get_source()->view();
  }

  let const consume_return = handling == return_handling::Consume;
  let const reject_return = handling == return_handling::Reject;
  if (parse_arena() == nullptr)
    throw Error{"Cannot run source outside of a parse"};

  LOG(Debug, "running source '%.*s' of %zu bytes at depth %zu",
      static_cast<int>(origin.length), origin.data, source.length,
      source_store().source_depth());

  /* Bound the source and eval nesting so a file that sources itself errors here
     rather than exhausting memory. */
  enter_source(call_site ? *call_site : SourceLocation{0, 0});
  defer { leave_source(); };

  let const parent_source = call_site ? source_store().m_current_source : nullptr;
  let const frame_is_sourced_file =
      consume_return && filename.has_value() && !filename->is_empty();

  /* The window opens before the frame is entered, and its restore runs after
     the frame is left. The body of a sourced file the trace option does not
     follow runs without the DEBUG action the caller installed. */
  let saved_debug_action = saved_frame_trap{};
  if (frame_is_sourced_file) saved_debug_action = save_untraced_debug_trap();
  defer { restore_untraced_debug_trap(steal(saved_debug_action)); };

  source_store().m_source_frames.push(source_frame{
      String{origin},
      call_site ? *call_site : SourceLocation{0, 0},
      parent_source, source_generation_for(parent_source),
      filename.has_value() ? String{*filename}
      : String{heap_allocator()},
      false, false
  });
  source_store().m_source_frames.back().should_defer_trace = frame_is_sourced_file;
  source_store().m_source_frames.back().function_call_depth =
      function_store().call_names().count();
  if (reject_return)
    source_store().set_rejected_return_source_frames(
        source_store().rejected_return_source_frames() + 1);
  defer
  {
    if (reject_return)
      source_store().set_rejected_return_source_frames(
          source_store().rejected_return_source_frames() - 1);
    let &frame = source_store().m_source_frames.back();
    if (frame.has_deferred_trace) {
      try {
        print_source_backtrace(frame.deferred_trace_location, false);
      } catch (...) {
        LOG(Debug, "rendering a deferred source trace failed");
      }
    }
    source_store().m_source_frames.pop_back();
  };

  try {
    const Expression *ast = nullptr;
    const String *retained_source = nullptr;

    if (cached_body != nullptr) {
      ast = cached_body->get_body();
      retained_source = cached_body->get_source();
    } else {
      let parser = Parser{
          Lexer{source, *parse_arena(), false, filename, mood()}
      };

      let const parsed_ast = parser.construct_ast();
      ASSERT(parsed_ast != nullptr);
      source_store().m_retained_source_asts.reserve(source_store().m_retained_source_asts.count() + 1);
      source_store().m_retained_sources.reserve(source_store().m_retained_sources.count() + 1);

      /* Keep a copy of the source alive for as long as the AST, so a
         control-flow jump made inside it can point a caret at the right text
         after this call returns. */
      let const owned_source = heap_allocator().alloc_array<String>(1);
      if (owned_source == nullptr) throw std::bad_alloc{};
      try {
        new (owned_source) String{steal(normalized_source)};
      } catch (...) {
        heap_allocator().free_array(owned_source, 1);
        throw;
      }
      source_store().m_retained_sources.push(owned_source);
      source_store().m_retained_source_asts.push(parsed_ast);
      ast = parsed_ast;
      retained_source = owned_source;
    }
    source = retained_source->view();

    let const previous_history_recording_root = source_store().m_history_recording_root;
    let const previous_history_recording_source = source_store().m_history_recording_source;
    if (history == history_recording::Enabled) {
      source_store().m_history_recording_root = ast;
      source_store().m_history_recording_source = source;
    }
    defer
    {
      source_store().m_history_recording_root = previous_history_recording_root;
      source_store().m_history_recording_source = previous_history_recording_source;
    };

    let const previous_source = source_store().m_current_source;
    let const previous_origin = source_store().m_current_origin;
    let const previous_location = source_store().m_current_location;
    set_current_source(retained_source, String{origin});
    source_store().m_current_location = SourceLocation{};
    defer
    {
      set_current_source(previous_source, previous_origin);
      source_store().m_current_location = previous_location;
    };

    ast->evaluate(*this);
    /* A return at the top of a sourced file or an eval returns from that source
       with its status. Break, continue, and exit keep propagating. */
    if (consume_return && has_pending_control_flow() &&
        pending_control_flow().kind == control_flow::Kind::Return)
    {
      let const source_status = static_cast<i32>(pending_control_flow().value);
      if (status_before_return != nullptr)
        *status_before_return = trap_store().m_status_before_return;

      clear_control_flow();
      set_last_exit_status(source_status);
      return source_status;
    }
    return last_exit_status();
  } catch (const InterruptErrorWithLocation &) {
    /* An interrupt ends the whole shell command. It passes through the sourced
       file, the eval, and the trap action that was running. */
    throw;
  } catch (const ErrorWithLocationAndDetails &detailed_error) {
    show_message(detailed_error.to_string(source, this));
    show_message(detailed_error.details_to_string(source, this));
    print_source_backtrace(detailed_error.location());
    return static_cast<i32>(detailed_error.command_status());
  } catch (const ErrorWithLocation &located_error) {
    show_message(located_error.to_string(source, this));
    print_source_backtrace(located_error.location());
    return static_cast<i32>(located_error.command_status());
  } catch (const Error &caught_error) {
    show_message(caught_error.to_string());
    print_source_backtrace();
    return static_cast<i32>(caught_error.command_status());
  }
}

fn EvalContext::resolve_source_path(StringView path,
                                    bool should_expand_tilde) throws
    -> Maybe<Path>
{
  let expanded_path = String{heap_allocator(), path};
  if (should_expand_tilde && path.starts_with("~")) {
    let const slash = path.find_character('/');
    let const prefix_end = slash.value_or(path.length);
    let const prefix = path.substring_of_length(1, prefix_end - 1);
    if (let directory = resolve_tilde_prefix(prefix); directory.has_value()) {
      expanded_path = directory.take();
      if (slash.has_value()) {
        if (expanded_path.is_empty() || expanded_path.back() != '/')
          expanded_path.push('/');
        expanded_path.append(path.substring(*slash + 1));
      }
      path = expanded_path.view();
    }
  }
  let source_path = Path{path};
  if (os::has_directory_separator(path)) return source_path;
  if (!is_shopt_enabled(shopt_option_id::Sourcepath)) return source_path;

  let const path_matches =
      get_program_resolver().search(path, ProgramResolver::SearchMode::First,
                                    ProgramResolver::Requirement::Regular,
                                    ProgramResolver::CachePolicy::Bypass);
  if (!path_matches.is_empty()) return path_matches[0].clone();
  if (is_posix_mode()) return None;

  return source_path;
}

fn EvalContext::clear_retained_sources() wontthrow -> void
{
  LOG(All, "dropping %zu retained sources and %zu retained asts",
      source_store().m_retained_sources.count(), source_store().m_retained_source_asts.count());

#if !defined NDEBUG
  for (let const &frame : source_store().m_source_frames) {
    if (frame.parent_source == nullptr) continue;
    for (let const *retained : source_store().m_retained_sources) {
      ASSERT(retained != frame.parent_source,
             "a live source frame still borrows a dropped source");
    }
  }
#endif

  source_store().m_retained_source_asts.clear();

  /* A stashed source view or location may index a buffer freed just below, so
     both drop to the unlocated rendering. */
  for (process_substitution &sub :
       expansion_store().pending_process_substitutions()) {
    sub.source = StringView{};
    sub.location = SourceLocation{};
  }

  if (has_pending_control_flow()) {
    pending_control_flow().source = nullptr;
    pending_control_flow().location = SourceLocation{};
  }

  for (String *source : source_store().m_retained_sources) {
    source->~String();
    heap_allocator().free_array(source, 1);
  }
  source_store().m_retained_sources.clear();

  /* A just-freed buffer can be reissued at the same address and length, so the
     caches keyed on that are dropped to keep them from serving a stale index.
   */
  utils::invalidate_line_number_cache();
  reset_runtime_diagnostic_highlight_cache();

  source_store().m_current_source = nullptr;
  source_store().m_current_source_generation = EXTERNAL_SOURCE_GENERATION;
  source_store().m_current_origin.clear();
  source_store().m_retained_source_generation++;
}

pure fn EvalContext::get_retained_source_generation() const wontthrow -> u64
{
  return source_store().m_retained_source_generation;
}

pure fn
EvalContext::scan_source_generation(const String *source) const wontthrow -> u64
{
  if (source == nullptr) return EXTERNAL_SOURCE_GENERATION;

  for (let const *retained : source_store().m_retained_sources) {
    if (retained == source) return source_store().m_retained_source_generation;
  }

  return EXTERNAL_SOURCE_GENERATION;
}

pure fn EvalContext::source_generation_for(const String *source) const wontthrow
    -> u64
{
  if (source == source_store().m_current_source) return source_store().m_current_source_generation;

  return scan_source_generation(source);
}

pure fn EvalContext::borrowed_frame_source(
    const source_frame &frame) const wontthrow -> const String *
{
  if (frame.parent_source_generation != EXTERNAL_SOURCE_GENERATION &&
      frame.parent_source_generation != source_store().m_retained_source_generation)
  {
    return nullptr;
  }

  return frame.parent_source;
}

fn EvalContext::retain_ast(Expression *ast) throws -> void
{
  source_store().m_retained_source_asts.push(ast);
}

fn EvalContext::expand_heredoc_body(
    StringView body, const SourceLocation *source_location) throws -> String
{
  LOG(Debug, "expanding a heredoc body of %zu bytes", body.length);
  return expand_modifier_word(body, false, false, source_location);
}

} /* namespace koshka */
