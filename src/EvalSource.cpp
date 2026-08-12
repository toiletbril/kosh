#include "Arena.hpp"
#include "Cli.hpp"
#include "Common.hpp"
#include "Debug.hpp"
#include "Errors.hpp"
#include "Eval.hpp"
#include "Expressions.hpp"
#include "Lexer.hpp"
#include "Parser.hpp"
#include "Path.hpp"
#include "Platform.hpp"
#include "ResolvedCommand.hpp"
#include "Trace.hpp"
#include "Utils.hpp"

namespace koshka {

static constexpr usize MAX_MIMICRY_DEPTH = 16;

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
  fallback_context.m_mimicry_depth = m_mimicry_depth;
  fallback_context.set_shell_executable_path(shell_executable_path());
  fallback_context.set_koshkit(koshkit());
  fallback_context.set_mimicry(mimicry());
  fallback_context.set_warning_level(warning_level());
  fallback_context.set_diagnostics_disabled(diagnostics_disabled());
  fallback_context.set_source_traces_enabled(should_print_source_traces());
  fallback_context.m_source_frames.reserve(m_source_frames.count());
  for (let const &frame : m_source_frames) {
    fallback_context.m_source_frames.push(
        source_frame{String{frame.origin.view()}, frame.call_site,
                     frame.parent_source, String{frame.source_path.view()},
                     frame.is_cli_root, frame.is_only_root_source});
    fallback_context.m_source_frames.back().was_printed = frame.was_printed;
  }
  defer
  {
    let const shared_frame_count =
        m_source_frames.count() < fallback_context.m_source_frames.count()
            ? m_source_frames.count()
            : fallback_context.m_source_frames.count();
    for (usize frame_index = 0; frame_index < shared_frame_count; frame_index++)
      m_source_frames[frame_index].was_printed =
          m_source_frames[frame_index].was_printed ||
          fallback_context.m_source_frames[frame_index].was_printed;
  };
  return fallback_context.run_mimicked_script(ec, mode, isolation);
}

fn EvalContext::run_mimicked_script(ExecContext &ec, mimic_mood mode,
                                    script_isolation isolation) throws -> i32
{
  let const isolated = isolation == script_isolation::Isolated;
  defer { ec.close_fds(); };

  if (m_mimicry_depth >= MAX_MIMICRY_DEPTH)
    throw ErrorWithLocation{ec.source_location(),
                            "Unable to mimic '" + ec.program() +
                                "' because the script nesting is too deep"};
  if (AST_ARENA == nullptr)
    throw ErrorWithLocation{ec.source_location(), "Unable to mimic '" +
                                                      ec.program() +
                                                      "' outside of a parse"};
  let const ast_mark = AST_ARENA->mark();
  defer { AST_ARENA->release(ast_mark); };

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

  m_runtime.mood = mode;
  m_runtime.set_option(shell_option_id::Restricted, false);
  m_is_restricted_shell = false;
  let const do_restore_restricted_shell = [&]() {
    m_is_restricted_shell = was_restricted_shell;
  };
  LOG(Debug, "mimicking the script '%s'%s", ec.program().c_str(),
      isolated ? " in an isolated subshell" : "");
  let const previous_script_run = m_is_script_run;
  m_is_script_run = true;

  /* A mimicked script runs with the strictness of the mood it mimics, so a bash
     or sh script clears nounset, pipefail, and failglob while a kosh script
     keeps the strict default. */
  let const is_mimic_strict = mode == mimic_mood::Default;
  set_error_unset(is_mimic_strict);
  set_pipefail(is_mimic_strict);
  set_failglob(is_mimic_strict);
  LOG(Debug, "seeded the strict options for the %s mimicked run",
      is_mimic_strict ? "kosh" : "lax");

  let const script_filename = ec.program_path().text().view();
  m_source_frames.push(source_frame{String{ec.program().view()},
                                    ec.source_location(), current_source(),
                                    String{script_filename}, false, false});
  defer { m_source_frames.pop_back(); };
  let parser = Parser{
      Lexer{String{contents->view()}, *AST_ARENA, false, script_filename,
            mood()}
  };

  let previous_shell_name = String{m_shell_name};
  let params = ArrayList<String>{heap_allocator()};
  params.reserve(ec.args().count() - 1);
  for (usize i = 1; i < ec.args().count(); i++)
    params.push_managed(ec.args()[i].view());

  let const previous_source = m_current_source;
  let const previous_origin = m_current_origin;
  let const previous_location = m_current_location;

  /* A standard descriptor with no staged redirect is backed up too, since the
     script may move it with an exec redirection that a fork would contain. */
  let saved_fds = ArrayList<os::saved_descriptor>{heap_allocator()};
  saved_fds.push(ec.in_fd.has_value()
                     ? os::save_and_replace_descriptor(0, *ec.in_fd)
                     : os::save_descriptor(0));
  saved_fds.push(ec.out_fd.has_value()
                     ? os::save_and_replace_descriptor(1, *ec.out_fd)
                     : os::save_descriptor(1));
  saved_fds.push(ec.err_fd.has_value()
                     ? os::save_and_replace_descriptor(2, *ec.err_fd)
                     : os::save_descriptor(2));
  let const do_restore_fds = [&]() {
    for (usize i = saved_fds.count(); i > 0; i--)
      os::restore_descriptor(saved_fds[i - 1]);
  };
  let const do_render_error = [&](std::exception_ptr error) {
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
    bool was_error_rendered = false;
    if (error && !is_interrupt) {
      set_last_exit_status(1);
      do_render_error(error);
      was_error_rendered = true;
    }
    if (!is_interrupt) {
      try {
        if (is_subshell)
          run_subshell_exit_trap();
        else
          run_exit_trap();
      } catch (...) {
        if (!error) {
          error = std::current_exception();
          is_interrupt = mimicked_error_is_interrupt(error);
        }
      }
    }
    if (error && !is_interrupt && !was_error_rendered) {
      do_render_error(error);
    }
    return is_interrupt;
  };

  /* The kernel hands a shebang interpreter the resolved script path, so $0 and
     BASH_SOURCE read that path rather than the word as typed. */
  m_shell_name =
      String{heap_allocator(), ec.should_use_fallback_argv0
                                   ? ec.args()[0].view()
                                   : ec.program_path().text().view()};
  set_current_source(&*contents, String{ec.program().view()});
  m_current_location = SourceLocation{};
  m_mimicry_depth++;

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
    m_mimicry_depth--;
    do_restore_fds();
    do_restore_restricted_shell();
    if (error) {
      if (is_interrupt) throw InterruptErrorWithLocation{previous_location};

      return 1;
    }
    return last_exit_status();
  }

  /* The isolated run snapshots the mutable state and runs in a subshell, so the
     script's cd, exports, functions, and exit do not leak to the parent. */
  let snapshot = snapshot_state();
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
  m_mimicry_depth--;
  do_restore_fds();

  let const status = last_exit_status();
  defer
  {
    restore_state(steal(snapshot));
    set_current_source(previous_source, previous_origin);
    m_current_location = previous_location;
    previous_runtime.restore(*this);
    do_restore_restricted_shell();
    m_is_script_run = previous_script_run;
    m_shell_name = steal(previous_shell_name);
  };

  if (error) {
    if (is_interrupt) throw InterruptErrorWithLocation{previous_location};

    return 1;
  }
  return status;
}

pure fn EvalContext::shopt_default_is_on(StringView name) wontthrow -> bool
{
  static constexpr PackedStringKey KEYS[] = {
      SSK("progcomp"),
      SSK("promptvars"),
      SSK("sourcepath"),
      SSK("extquote"),
      SSK("complete_fullquote"),
      SSK("hostcomplete"),
      SSK("cmdhist"),
      SSK("checkwinsize"),
      SSK("force_fignore"),
      SSK("globasciiranges"),
      SSK("globskipdots"),
      SSK("expand_aliases"),
      SSK("interactive_comments"),
  };
  static constexpr StaticStringSet DEFAULT_ON_SHOPT_NAMES{KEYS};
  return DEFAULT_ON_SHOPT_NAMES.contains(name);
}

fn EvalContext::run_source(StringView source, StringView origin,
                           return_handling handling,
                           Maybe<SourceLocation> call_site,
                           Maybe<StringView> filename,
                           bool should_record_history) throws -> i32
{
  let normalized_source = String{source};
  normalized_source.normalize_crlf_line_endings();
  source = normalized_source.view();

  let const consume_return = handling == return_handling::Consume;
  let const reject_return = handling == return_handling::Reject;
  if (AST_ARENA == nullptr) throw Error{"Cannot run source outside of a parse"};

  LOG(Debug, "running source '%.*s' of %zu bytes at depth %zu",
      static_cast<int>(origin.length), origin.data, source.length,
      m_source_depth);

  /* Bound the source and eval nesting so a file that sources itself errors here
     rather than exhausting memory. */
  enter_source(call_site ? *call_site : SourceLocation{0, 0});
  defer { leave_source(); };

  let const parent_source = call_site ? m_current_source : nullptr;

  m_source_frames.push(source_frame{
      String{origin},
      call_site ? *call_site : SourceLocation{0, 0},
      parent_source,
      filename.has_value() ? String{*filename}
      : String{heap_allocator()},
      false, false
  });
  let const frame_is_sourced_file =
      filename.has_value() && !filename->is_empty();
  if (frame_is_sourced_file) m_sourced_file_frames++;
  if (reject_return) m_rejected_return_source_frames++;
  defer
  {
    if (reject_return) m_rejected_return_source_frames--;
    if (frame_is_sourced_file) m_sourced_file_frames--;
    m_source_frames.pop_back();
  };

  /* Retain an owned copy of the filename, so the views the lexer stamps onto
     every location stay valid after a control-flow jump carries a stamped
     location out to the top level. */
  Maybe<StringView> stable_filename = None;
  if (filename.has_value()) {
    let const retained_filename = new String{*filename};
    m_retained_sources.push(retained_filename);
    stable_filename = retained_filename->view();
  }

  try {
    let parser = Parser{
        Lexer{String{source}, *AST_ARENA, false, stable_filename, mood()}
    };

    let const ast = parser.construct_ast();
    ASSERT(ast != nullptr);
    m_retained_source_asts.push(ast);

    /* Keep a copy of the source alive for as long as the AST, so a control-flow
       jump made inside it can point a caret at the right text after this call
       returns. */
    let const retained_source = new String{steal(normalized_source)};
    m_retained_sources.push(retained_source);
    source = retained_source->view();

    let const previous_history_recording_root = m_history_recording_root;
    let const previous_history_recording_source = m_history_recording_source;
    if (should_record_history) {
      m_history_recording_root = ast;
      m_history_recording_source = source;
    }
    defer
    {
      m_history_recording_root = previous_history_recording_root;
      m_history_recording_source = previous_history_recording_source;
    };

    let const previous_source = m_current_source;
    let const previous_origin = m_current_origin;
    let const previous_location = m_current_location;
    set_current_source(retained_source, String{origin});
    m_current_location = SourceLocation{};
    defer
    {
      set_current_source(previous_source, previous_origin);
      m_current_location = previous_location;
    };

    ast->evaluate(*this);
    /* A return at the top of a sourced file or an eval returns from that source
       with its status. Break, continue, and exit keep propagating. */
    if (consume_return && has_pending_control_flow() &&
        pending_control_flow().kind == control_flow::Kind::Return)
    {
      let const source_status = static_cast<i32>(pending_control_flow().value);
      clear_control_flow();
      set_last_exit_status(source_status);
      return source_status;
    }
    return last_exit_status();
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

fn EvalContext::clear_retained_sources() wontthrow -> void
{
  LOG(All, "dropping %zu retained sources and %zu retained asts",
      m_retained_sources.count(), m_retained_source_asts.count());
  m_retained_source_asts.clear();

  /* A stashed source view or location may index a buffer freed just below, so
     both drop to the unlocated rendering. */
  for (process_substitution &sub : m_pending_process_substitutions) {
    sub.source = StringView{};
    sub.location = SourceLocation{};
  }

  if (has_pending_control_flow()) {
    pending_control_flow().source = nullptr;
    pending_control_flow().location = SourceLocation{};
  }

  for (String *source : m_retained_sources)
    delete source;
  m_retained_sources.clear();

  /* A just-freed buffer can be reissued at the same address and length, so the
     caches keyed on that are dropped to keep them from serving a stale index.
   */
  utils::invalidate_line_number_cache();
  reset_runtime_diagnostic_highlight_cache();

  m_current_source = nullptr;
  m_current_origin.clear();
}

fn EvalContext::retain_ast(Expression *ast) throws -> void
{
  m_retained_source_asts.push(ast);
}

fn EvalContext::expand_heredoc_body(
    StringView body, const SourceLocation *source_location) throws -> String
{
  LOG(Debug, "expanding a heredoc body of %zu bytes", body.length);
  return expand_modifier_word(body, false, false, source_location);
}

} // namespace koshka
