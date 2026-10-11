/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements the hot simple-command execution path. It expands
 * aliases, prefix assignments, command words, arguments, and arrays, resolves
 * the command, applies redirections, dispatches builtins or programs, and
 * reports the resulting status. The split keeps hot dispatch outside
 * simple-command storage, formatting, analysis, and redirection construction.
 */

#include "Builtin.hpp"
#include "CLI.hpp"
#include "Errors.hpp"
#include "Eval.hpp"
#include "Expressions.hpp"
#include "ExpressionsInternal.hpp"
#include "Koshkit.hpp"
#include "Lexer.hpp"
#include "Optimizer.hpp"
#include "Platform.hpp"
#include "Tokens.hpp"
#include "Utils.hpp"
#include "base/Arena.hpp"
#include "base/Common.hpp"
#include "base/Debug.hpp"
#include "base/Trace.hpp"

namespace koshka {

namespace expressions {

using namespace internal;

fn internal::expand_command_aliases(
    EvalContext &cxt, ArrayList<String> &args,
    ArrayList<SourceLocation> &arg_locations) throws -> void
{
  if (!cxt.scope_store().has_aliases() ||
      !cxt.is_shopt_enabled("expand_aliases"))
    return;

  HashSet already_expanded{heap_allocator()};

  while (!args.is_empty()) {
    let const &word = args[0];

    if (already_expanded.contains(word.view())) break;

    let const body = cxt.scope_store().get_alias(word);
    if (!body.has_value()) break;
    already_expanded.add(word.view());
    LOG(Debug, "expanding the alias '%s'", word.c_str());

    let rebuilt = ArrayList<String>{heap_allocator()};
    let rebuilt_locations = ArrayList<SourceLocation>{heap_allocator()};
    let current = String{cxt.scratch_allocator()};
    let const &body_value = *body;
    let const body_location =
        !arg_locations.is_empty() ? arg_locations[0] : SourceLocation{};
    for (usize i = 0; i < body_value.count(); i++) {
      let const c = body_value[i];
      if (c == ' ' || c == '\t') {
        if (!current.is_empty()) {
          rebuilt.push(String{
              heap_allocator(), StringView{current.data(), current.count()}
          });
          rebuilt_locations.push(body_location);
          current.clear();
        }
      } else {
        current += c;
      }
    }
    if (!current.is_empty()) {
      rebuilt.push(String{
          heap_allocator(), StringView{current.data(), current.count()}
      });
      rebuilt_locations.push(body_location);
    }

    for (usize i = 1; i < args.count(); i++) {
      rebuilt.push(steal(args[i]));
      if (i < arg_locations.count())
        rebuilt_locations.push(arg_locations[i]);
      else
        rebuilt_locations.push(body_location);
    }

    args = steal(rebuilt);
    arg_locations = steal(rebuilt_locations);
  }
}

namespace {

static fn command_word_is_glob(const Word &word) wontthrow -> bool
{
  bool has_open_bracket = false;
  for (let const &segment : word.segments) {
    if (segment.kind != WordSegment::Kind::UnquotedText) continue;
    for (usize i = 0; i < segment.text.count(); i++) {
      let const c = segment.text[i];
      if (c == '*' || c == '?') {
        return true;
      }
      if (c == '[') has_open_bracket = true;
      if (c == ']' && has_open_bracket) {
        return true;
      }
    }
  }
  return false;
}

} /* namespace */

static fn command_runs_program(const ArrayList<String> &args,
                               const Maybe<Builtin::Kind> *literal_builtin,
                               mimic_mood mood) throws -> bool
{
  if (args.is_empty()) return true;

  let builtin = literal_builtin != nullptr ? *literal_builtin
                                           : search_builtin(args[0].view());
  if (builtin.has_value() && *builtin == Builtin::Kind::CommandBuiltin) {
    usize operand_index = 1;
    while (operand_index < args.count() && args[operand_index] == "-p")
      operand_index++;

    if (operand_index == args.count() ||
        args[operand_index].view().starts_with("-"))
    {
      return false;
    }

    builtin = search_builtin(args[operand_index].view());
  }

  return !builtin.has_value() || builtin_is_hidden_by_mood(*builtin, mood);
}

hot fn SimpleCommand::get_literal_command_lookup(
    const ArrayList<String> &program_args) const throws
    -> const literal_command_lookup *
{
  if (program_args.is_empty() || m_args.is_empty() ||
      m_args[0]->kind() != Token::Kind::Word)
  {
    return nullptr;
  }

  const Word &command_word =
      static_cast<const tokens::WordToken *>(m_args[0])->word();
  StringView literal_name;
  if (command_word.plain_literal_kind() != Word::PlainLiteral::NotPlain) {
    literal_name = command_word.constant_value();
  } else if (command_word.segments.count() == 1 &&
             command_word.segments[0].kind == WordSegment::Kind::UnquotedText)
  {
    literal_name = command_word.segments[0].text.view();
  } else {
    return nullptr;
  }

  if (program_args[0].view() != literal_name) return nullptr;

  if (!m_literal_command_lookup.has_value()) {
    m_literal_command_lookup = literal_command_lookup{
        search_builtin(literal_name), is_special_builtin_name(literal_name)};
  }

  return &*m_literal_command_lookup;
}

hot fn SimpleCommand::evaluate_impl(EvalContext &cxt,
                                    root_evaluation_mode mode) const throws
    -> status_result
{
  ASSERT(m_args.count() > 0 || !m_redirections.is_empty() ||
         m_local_vars.count() > 0 || !m_array_args.is_empty());

  cxt.source_store().set_current_location(source_location());

  let const should_run_command = publish_simple_command(cxt, *this, mode);
  if (!should_run_command) return cxt.execution_store().last_exit_status();

  let const is_async_command =
      is_async() && mode != root_evaluation_mode::PreparedAsyncCommand;
  if (is_async_command && cxt.runtime_state().get_mood() != mimic_mood::Default)
  {
    let const do_run_in_child = [](void *context, EvalContext &child_cxt)
                                    throws -> i64 {
      return static_cast<const SimpleCommand *>(context)
          ->evaluate_impl(child_cxt, root_evaluation_mode::PreparedAsyncCommand)
          .status;
    };

    let full_location = source_location();
    let const full_start_position =
        static_cast<u32>(full_source_start_position());
    if (full_start_position < full_location.position) {
      full_location.length += full_location.position - full_start_position;
      full_location.position = full_start_position;
    }

    return evaluate_async_with(
        cxt, do_run_in_child,
        const_cast<void *>(static_cast<const void *>(this)),
        cxt.source_text_in_span(full_location, full_source_end_position()));
  }

  if (!m_args.is_empty() && m_args[0]->kind() == Token::Kind::Word) {
    const Word &command_word =
        static_cast<const tokens::WordToken *>(m_args[0])->word();
    if (!m_command_word_is_glob.has_value())
      m_command_word_is_glob = command_word_is_glob(command_word);

    if (*m_command_word_is_glob) {
      let const location = m_args[0]->source_location();
      let const message =
          StringView{"A glob pattern in command position is rarely intended as "
                     "a command name"};
      let const note =
          StringView{"Quote it to run a literal name, or list the matches with "
                     "compgen -G"};
      if (cxt.runtime_state().get_mood() == mimic_mood::Default)
        throw ErrorWithLocationAndDetails{location, message, note};
      cxt.show_runtime_warning_at(location, message, note);
    }
  }

  if (cxt.runtime_state().should_echo()) {
    koshka::print(utils::merge_tokens_to_string(m_args) + "\n");
    koshka::flush();
  }

  let const args_mark = cxt.expansion_store().scratch_arena().mark();
  defer { cxt.expansion_store().scratch_arena().release(args_mark); };
  let const substitution_mark = cxt.mark_process_substitutions();
  let program_arg_locations =
      ArrayList<SourceLocation>{cxt.scratch_allocator()};
  let keyword_assignments =
      ArrayList<const tokens::Assignment *>{cxt.scratch_allocator()};
  const ArrayList<const Token *> *argument_tokens = &m_args;
  let filtered_argument_tokens =
      ArrayList<const Token *>{cxt.scratch_allocator()};
  if (cxt.runtime_state().option_is_enabled(shell_option_id::Keyword)) {
    for (let const token : m_args)
      if (token->kind() == Token::Kind::Assignment)
        keyword_assignments.push(
            static_cast<const tokens::Assignment *>(token));

    if (!keyword_assignments.is_empty()) {
      filtered_argument_tokens.reserve(m_args.count() -
                                       keyword_assignments.count());
      for (let const token : m_args)
        if (token->kind() != Token::Kind::Assignment)
          filtered_argument_tokens.push(token);
      argument_tokens = &filtered_argument_tokens;
    }
  }
  let program_args =
      cxt.process_args(*argument_tokens, &program_arg_locations,
                       argument_lifetime::Transient, argument_context::Command);
  defer { cxt.cleanup_process_substitutions(substitution_mark); };
  expand_command_aliases(cxt, program_args, program_arg_locations);

  if (!is_async_command && !cxt.job_table_store().is_in_pipeline_stage()) {
    utils::set_foreground_program_title(program_args, cxt);
  }

  if (!program_args.is_empty())
    cxt.guard_restricted_path(program_args[0].view(),
                              program_arg_locations.is_empty()
                                  ? source_location()
                                  : program_arg_locations[0],
                              restricted_path_use::Command);

  LOG(Info, "dispatching the command '%s' with %zu words",
      program_args.is_empty() ? "" : program_args[0].c_str(),
      program_args.count());

  FunctionBodyHandle command_function_storage{};
  if (!program_args.is_empty() && cxt.function_store().has_functions()) {
    if (let const *storage =
            cxt.function_store().find_storage(program_args[0].view());
        storage != nullptr)
    {
      command_function_storage = *storage;
    }
  }
  const Expression *command_word_function =
      command_function_storage.has_value() ? command_function_storage.get_body()
                                           : nullptr;

  let const is_bare_exec = program_args.count() == 1 &&
                           program_args[0] == "exec" &&
                           command_word_function == nullptr;

  if (is_bare_exec) {
    for (let const &redir : m_redirections) {
      if (redir.fd_allocation_name_token == nullptr)
        cxt.snapshot_subshell_descriptor(redir.fd);
      if (redir.is_dup_filename_allowed) cxt.snapshot_subshell_descriptor(2);
    }
  }

  let const *const literal_lookup = get_literal_command_lookup(program_args);
  const bool is_command_special_builtin =
      !program_args.is_empty() && command_word_function == nullptr &&
      (literal_lookup != nullptr
           ? literal_lookup->is_special
           : is_special_builtin_name(program_args[0].view()));

  Maybe<os::descriptor> redirect_in_fd;
  ArrayList<os::saved_descriptor> dup_saved_descriptors{
      cxt.scratch_allocator()};
  defer
  {
    for (usize i = dup_saved_descriptors.count(); i > 0; i--)
      os::restore_descriptor(dup_saved_descriptors[i - 1]);
  };
  defer
  {
    if (redirect_in_fd) os::close_fd(*redirect_in_fd);
  };

  Maybe<eval_state_snapshot> redirection_snapshot;
  if (!m_redirections.is_empty() && cxt.runtime_state().is_bash_compatible() &&
      command_word_function == nullptr &&
      redirections_can_change_state(m_redirections))
  {
    if (command_runs_program(
            program_args,
            literal_lookup != nullptr ? &literal_lookup->builtin : nullptr,
            cxt.runtime_state().get_mood()))
      redirection_snapshot = snapshot_child_redirection_state(cxt);
  }
  defer { discard_child_redirection_state(cxt, redirection_snapshot); };

  bool did_redirection_open_fail = false;
  try {
    for (let const &original_redir : m_redirections) {
      let redir = original_redir;
      let const r =
          resolve_redirection(redir, cxt, source_location(),
                              &did_redirection_open_fail, !is_bare_exec);

      redir.fd = allocate_redirection_descriptor(original_redir, r, cxt,
                                                 source_location(),
                                                 &did_redirection_open_fail);

      switch (r.kind) {
      case redirection_outcome::Heredoc: {
        let const body_fd = r.opened_fd;
        if (is_bare_exec) {
          cxt.snapshot_subshell_descriptor(redir.fd);
          koshka::flush();
          os::replace_descriptor(redir.fd, body_fd);
          if (!os::descriptor_is_shell_fd(body_fd, redir.fd))
            os::close_fd(body_fd);
          break;
        }

        if (redir.fd == 0) {
          if (redirect_in_fd) os::close_fd(*redirect_in_fd);
          redirect_in_fd = body_fd;
          break;
        }

        const bool is_body_target_fd =
            os::descriptor_is_shell_fd(body_fd, redir.fd);
        if (is_body_target_fd) {
          dup_saved_descriptors.push(
              os::saved_descriptor{.shell_fd = redir.fd, .was_open = false});
          break;
        }
        let const saved = os::save_and_replace_descriptor(redir.fd, body_fd);
        dup_saved_descriptors.push(saved);
        os::close_fd(body_fd);
        if (!saved.is_dup2_ok) {
          did_redirection_open_fail = true;
          throw ErrorWithLocation{redir.target->source_location(),
                                  "Bad file descriptor"};
        }
        break;
      }

      case redirection_outcome::BothStreams: {
        let const file_fd = r.opened_fd;
        koshka::flush();
        if (is_bare_exec) {
          cxt.snapshot_subshell_descriptor(1);
          cxt.snapshot_subshell_descriptor(2);
          let const did_replace_out = os::replace_descriptor(1, file_fd);
          let const did_replace_err = os::replace_descriptor(2, file_fd);
          os::close_fd(file_fd);
          if (!did_replace_out || !did_replace_err) {
            did_redirection_open_fail = true;
            throw ErrorWithLocation{redir.target->source_location(),
                                    "Bad file descriptor"};
          }
          break;
        }
        const os::saved_descriptor saved_out =
            os::save_and_replace_descriptor(1, file_fd);
        dup_saved_descriptors.push(saved_out);
        const os::saved_descriptor saved_err =
            os::save_and_replace_descriptor(2, file_fd);
        dup_saved_descriptors.push(saved_err);
        os::close_fd(file_fd);
        if (!saved_out.is_dup2_ok || !saved_err.is_dup2_ok) {
          did_redirection_open_fail = true;
          throw ErrorWithLocation{redir.target->source_location(),
                                  "Bad file descriptor"};
        }
        break;
      }

      case redirection_outcome::Duplicate: {
        let const from_fd = r.dup_from_fd;

        if (is_bare_exec) {
          cxt.snapshot_subshell_descriptor(redir.fd);
          koshka::flush();

          if (from_fd == Redirection::DUP_FD_CLOSE) {
            os::close_shell_fd(redir.fd);
            cxt.forget_coprocess_descriptor(redir.fd);
            break;
          }

          if (!os::replace_descriptor(redir.fd,
                                      os::descriptor_for_shell_fd(from_fd)))
          {
            let const location = redir.target != nullptr
                                     ? redir.target->source_location()
                                     : source_location();
            did_redirection_open_fail = true;
            throw ErrorWithLocation{location,
                                    String::from(from_fd, heap_allocator()) +
                                        ": Bad file descriptor"};
          }
          break;
        }

        if (from_fd == redir.fd) {
          break;
        }

        koshka::flush();

        if (from_fd == Redirection::DUP_FD_CLOSE) {
          let const saved = os::save_and_replace_descriptor(
              redir.fd, os::descriptor_for_shell_fd(redir.fd));
          dup_saved_descriptors.push(saved);

          if (!saved.was_open) break;

          if (!saved.is_dup2_ok) {
            did_redirection_open_fail = true;
            throw ErrorWithLocation{source_location(), "Bad file descriptor"};
          }

          os::close_fd(os::descriptor_for_shell_fd(redir.fd));
          break;
        }

        let const saved = os::save_and_replace_descriptor(
            redir.fd, os::descriptor_for_shell_fd(from_fd));
        dup_saved_descriptors.push(saved);
        if (!saved.is_dup2_ok) {
          let const location = redir.target != nullptr
                                   ? redir.target->source_location()
                                   : source_location();
          did_redirection_open_fail = true;
          throw ErrorWithLocation{location,
                                  String::from(from_fd, heap_allocator()) +
                                      ": Bad file descriptor"};
        }
        break;
      }

      case redirection_outcome::OpenedFile: {
        let const file_fd = r.opened_fd;
        if (is_bare_exec) {
          cxt.snapshot_subshell_descriptor(redir.fd);
          koshka::flush();
          let const was_replaced = os::replace_descriptor(redir.fd, file_fd);
          if (!os::descriptor_is_shell_fd(file_fd, redir.fd))
            os::close_fd(file_fd);
          if (!was_replaced) {
            did_redirection_open_fail = true;
            throw ErrorWithLocation{redir.target->source_location(),
                                    String::from(redir.fd, heap_allocator()) +
                                        ": Bad file descriptor"};
          }
          break;
        }

        if (redir.fd == 1 || redir.fd == 2) {
          koshka::flush();
        }
        const bool is_file_target_fd =
            os::descriptor_is_shell_fd(file_fd, redir.fd);
        if (is_file_target_fd) {
          dup_saved_descriptors.push(
              os::saved_descriptor{.shell_fd = redir.fd, .was_open = false});
        } else {
          let const saved = os::save_and_replace_descriptor(redir.fd, file_fd);
          dup_saved_descriptors.push(saved);
          if (!r.is_cached) os::close_fd(file_fd);
          if (!saved.is_dup2_ok) {
            did_redirection_open_fail = true;
            throw ErrorWithLocation{redir.target->source_location(),
                                    "Bad file descriptor"};
          }
        }
        break;
      }
      }
    }
    discard_child_redirection_state(cxt, redirection_snapshot);
  } catch (const TrapAbandonedRedirection &) {
    discard_child_redirection_state(cxt, redirection_snapshot);
    return cxt.execution_store().last_exit_status();
  } catch (const ErrorWithLocation &redirection_error) {
    discard_child_redirection_state(cxt, redirection_snapshot);
    if (!did_redirection_open_fail) throw;
    if (is_command_special_builtin) throw;
    if (redirection_error.is_script_fatal() &&
        command_word_function != nullptr &&
        cxt.runtime_state().is_bash_compatible())
    {
      throw;
    }

    show_message(redirection_error.to_string(
        cxt.source_store().current_source_view(), &cxt));
    let const redirection_status =
        cxt.runtime_state().is_bash_compatible() ? 1 : 2;
    cxt.execution_store().set_last_exit_status(redirection_status);
    cxt.publish_single_pipe_status(redirection_status);
    return redirection_status;
  }

  if (is_bare_exec) cxt.hold_process_substitutions(substitution_mark);

  let const do_apply_append = [&](StringView name, String &value_ref) throws {
    let appended = String{cxt.scratch_allocator()};
    if (let const existing = cxt.get_variable_value(name))
      appended.append(existing->view());
    let resolved_name = Maybe<String>{};
    if (cxt.variable_store().attributes().is_nameref(name)) rarely
      {
        resolved_name = cxt.resolve_nameref_base_for_write(name);
        name = resolved_name->view();
      }
    if (cxt.is_integer_variable(name)) {
      cxt.append_integer_expression(appended, value_ref.view());
      value_ref = cxt.evaluate_arithmetic_text(appended.view());
    } else {
      appended += value_ref;
      value_ref = steal(appended);
    }
  };
  let const do_trace_assignment =
      [&](StringView name, assignment_update_mode update_mode, StringView value)
          throws -> void {
    if (!cxt.runtime_state().should_echo_expanded()) return;
    let trace = String{cxt.scratch_allocator(), name};
    trace += update_mode == assignment_update_mode::Append ? "+=" : "=";
    append_shell_quoted_arg(trace, value);
    cxt.write_xtrace(trace.view());
  };
  let const do_trace_array_assignment =
      [&](const array_builtin_assignment &assignment,
          const ArrayList<String> &values) throws -> void {
    if (!cxt.runtime_state().should_echo_expanded()) return;
    let trace = String{cxt.scratch_allocator(), assignment.name.view()};
    trace +=
        assignment.update_mode == assignment_update_mode::Append ? "+=(" : "=(";
    for (usize i = 0; i < values.count(); i++) {
      if (i > 0) trace.push(' ');
      append_shell_quoted_arg(trace, values[i].view());
    }
    trace.push(')');
    cxt.write_xtrace(trace.view());
  };

  let const do_reject_readonly_assignment =
      [&](StringView name, expansion_error_reach reach) throws -> void {
    let error =
        ErrorWithLocation{source_location(), "Unable to assign '" + name +
                                                 "' because it is read only"};
    cxt.mark_expansion_error(error, reach);
    throw steal(error);
  };
  let const do_reject_readonly_target = [&](StringView name) throws {
    let resolved_name = Maybe<String>{};
    if (cxt.variable_store().attributes().is_nameref(name)) rarely
      {
        try {
          resolved_name = cxt.resolve_nameref_base_for_write(name);
        } catch (ErrorBase &error) {
          cxt.mark_expansion_error(error,
                                   expansion_error_reach::LineOrPosixScript);
          throw;
        }
        name = resolved_name->view();
      }
    if (cxt.is_readonly(name)) {
      do_reject_readonly_assignment(name,
                                    expansion_error_reach::LineOrPosixScript);
    }
  };

  let const do_apply_persistent_assignment =
      [&](const tokens::Assignment &assignment) throws {
        let const name = assignment.key().view();
        let value =
            cxt.expand_word_for_assignment(assignment.value_word(), true);
        if (let const bracket = name.find_character('[');
            bracket.has_value() && name[name.length - 1] == ']')
        {
          let const array_name = name.substring_of_length(0, *bracket);
          let const subscript = name.substring_of_length(
              *bracket + 1, name.length - *bracket - 2);
          do_trace_assignment(name, assignment.get_update_mode(), value.view());
          if (!cxt.is_circular_nameref(array_name))
            do_reject_readonly_target(array_name);
          try {
            cxt.assign_array_element(array_name, subscript, value.view(),
                                     assignment.get_update_mode());
          } catch (const Error &error) {
            relocate_if_unlocated(error, assignment.source_location());
          }
          return;
        }

        do_reject_readonly_target(name);
        do_trace_assignment(name, assignment.get_update_mode(), value.view());
        if (assignment.get_update_mode() == assignment_update_mode::Append)
          do_apply_append(name, value);
        cxt.set_shell_variable(name, value);
        if (cxt.runtime_state().export_all()) {
          cxt.record_environment_change(name);
          os::set_environment_variable(name, value.view());
          cxt.mark_exported(name);
        }
      };

  if (program_args.is_empty()) {
    for (let const &var : m_local_vars)
      do_apply_persistent_assignment(*var.token);
    for (let const assignment : keyword_assignments)
      do_apply_persistent_assignment(*assignment);
    for (let const &assignment : m_array_args) {
      if (!cxt.is_circular_nameref(assignment.name))
        do_reject_readonly_target(assignment.name);
      let subscript_flags = Bitset{heap_allocator()};
      ArrayList<String> values = cxt.process_args(
          assignment.elements, nullptr, argument_lifetime::Persistent,
          cxt.is_associative_array(assignment.name)
              ? argument_context::AssociativeLiteral
              : argument_context::ArrayLiteral,
          &subscript_flags);
      do_trace_array_assignment(assignment, values);
      cxt.assign_indexed_array_elements(
          assignment.name, values, assignment.update_mode, &subscript_flags);
    }
    let const do_token_ran_substitution = [&](const Token *token) {
      if (token == nullptr) return false;
      if (token->kind() == Token::Kind::Word)
        return static_cast<const tokens::WordToken *>(token)
            ->word()
            .runs_substitution();
      if (token->kind() == Token::Kind::Assignment)
        return static_cast<const tokens::Assignment *>(token)
            ->value_word()
            .runs_substitution();
      return false;
    };
    let ran_substitution = false;
    for (let const token : m_args)
      ran_substitution = ran_substitution || do_token_ran_substitution(token);
    for (let const &var : m_local_vars)
      ran_substitution =
          ran_substitution || var.get_value().runs_substitution();
    for (let const &assignment : m_array_args)
      for (let const token : assignment.elements)
        ran_substitution = ran_substitution || do_token_ran_substitution(token);
    if (!ran_substitution) cxt.execution_store().set_last_exit_status(0);
    cxt.publish_single_pipe_status(cxt.execution_store().last_exit_status());
    return cxt.execution_store().last_exit_status();
  }

  struct saved_env_var
  {
    String name;
    Maybe<String> previous_value;
    Maybe<String> previous_shell_value;
    Maybe<SourceLocation> previous_special_definition_location;
    bool did_overlay_shell_value;
    bool did_clear_circular_reference;
  };
  ArrayList<saved_env_var> saved_env{cxt.scratch_allocator()};
  saved_env.reserve(m_local_vars.count() + keyword_assignments.count());
  bool was_ifs_assigned = false;
  String saved_ifs_separators{cxt.scratch_allocator()};
  Maybe<ProgramResolver> saved_program_resolver{};
  Maybe<bool> previous_ignoreeof_state{};
  defer
  {
    for (usize i = saved_env.count(); i > 0; i--) {
      const saved_env_var &restore = saved_env[i - 1];
      if (restore.did_overlay_shell_value)
        cxt.restore_temporary_shell_variable(
            restore.name.view(), restore.previous_shell_value,
            restore.previous_special_definition_location);
      if (restore.previous_value)
        os::set_environment_variable(restore.name.view(),
                                     restore.previous_value->view());
      else
        os::unset_environment_variable(restore.name.view());
      cxt.sync_exported_after_restore(restore.name.view(),
                                      restore.previous_value.has_value());
      if (restore.did_clear_circular_reference) {
        cxt.variable_store().attributes().set(
            restore.name.view(), variable_attribute::Nameref, true);
      }
    }
    if (saved_program_resolver.has_value())
      cxt.program_resolver() = steal(*saved_program_resolver);
    if (was_ifs_assigned)
      cxt.variable_store().set_field_separators(saved_ifs_separators.view());
    if (previous_ignoreeof_state.has_value())
      cxt.runtime_state().set_option(shell_option_id::Ignoreeof,
                                     *previous_ignoreeof_state);
  };
  let const has_prefix_assignments =
      !m_local_vars.is_empty() || !keyword_assignments.is_empty();
  let const is_source_evaluating_builtin =
      has_prefix_assignments && !program_args.is_empty() &&
      command_word_function == nullptr &&
      (program_args[0] == "eval" || program_args[0] == "." ||
       program_args[0] == "source");
  let const is_prefix_assignment_persistent =
      has_prefix_assignments &&
      (is_command_special_builtin
           ? !cxt.runtime_state().is_bash_compatible() ||
                 cxt.runtime_state().is_posix_option_on()
           : is_source_evaluating_builtin &&
                 cxt.runtime_state().is_posix_option_on());
  let const do_apply_environment_assignment =
      [&](const tokens::Assignment &assignment) throws {
        let name = assignment.key().view();
        let resolved_name = Maybe<String>{};
        let did_clear_circular_reference = false;
        if (cxt.variable_store().attributes().is_nameref(name)) rarely
          {
            if (cxt.is_circular_nameref(name)) {
              cxt.warn_circular_nameref(name);
              if (is_prefix_assignment_persistent || name == "IFS") return;

              did_clear_circular_reference = true;
            } else {
              resolved_name = cxt.resolve_nameref_for_write(name);
              name = resolved_name->view();
            }
          }
        if (cxt.is_readonly(name)) {
          if (cxt.runtime_state().is_bash_compatible() &&
              !cxt.runtime_state().is_posix_option_on())
          {
            cxt.show_runtime_error_at(assignment.source_location(),
                                      "Unable to assign '" + name +
                                          "' because it is read only");
            return;
          }

          do_reject_readonly_assignment(
              name, is_command_special_builtin
                        ? expansion_error_reach::LineOrPosixScript
                        : expansion_error_reach::Line);
        }
        const bool is_read_field_separator =
            name == "IFS" && command_word_function == nullptr &&
            !program_args.is_empty() && program_args[0] == "read";
        Maybe<String> previous;
        if (!is_read_field_separator)
          previous = os::get_environment_variable(name);
        let expanded_value = String{cxt.scratch_allocator()};
        try {
          expanded_value =
              cxt.expand_word_for_assignment(assignment.value_word(), true);
        } catch (const Error &e) {
          relocate_if_unlocated(e, source_location());
        }
        do_trace_assignment(name, assignment.get_update_mode(),
                            expanded_value.view());
        if (assignment.get_update_mode() == assignment_update_mode::Append)
          do_apply_append(name, expanded_value);

        if (is_prefix_assignment_persistent) {
          cxt.set_shell_variable(name, expanded_value);
          if (cxt.runtime_state().export_all()) {
            cxt.record_environment_change(name);
            os::set_environment_variable(name, expanded_value.view());
            cxt.mark_exported(name);
          }
          return;
        }

        if (name == "IFS" && !was_ifs_assigned) {
          was_ifs_assigned = true;
          saved_ifs_separators =
              cxt.get_variable_value("IFS").value_or(String{" \t\n"});
        }

        let const is_path_name = utils::environment_name_is_path(name);
        if (is_path_name && !saved_program_resolver.has_value())
          saved_program_resolver =
              Maybe<ProgramResolver>{cxt.program_resolver()};

        if (!is_read_field_separator) {
          if (did_clear_circular_reference) {
            cxt.variable_store().attributes().set(
                name, variable_attribute::Nameref, false);
          }

          Maybe<String> previous_shell_value;
          Maybe<SourceLocation> previous_special_definition_location;
          let const is_locale_name = name.starts_with("LC_") || name == "LANG";
          let const did_overlay_shell_value =
              command_word_function != nullptr ||
              is_source_evaluating_builtin || is_locale_name;
          if (did_overlay_shell_value) {
            if (let const stored =
                    cxt.variable_store().shell_variables().find(name);
                stored.has_value())
            {
              previous_shell_value =
                  String{cxt.scratch_allocator(), stored->view()};
            }
            previous_special_definition_location =
                cxt.special_variable_definition_location(name);
            if (name == "IGNOREEOF" && !previous_ignoreeof_state.has_value()) {
              previous_ignoreeof_state = cxt.runtime_state().option_is_enabled(
                  shell_option_id::Ignoreeof);
            }
            cxt.set_shell_variable(name, expanded_value.view());
          }
          saved_env.push(saved_env_var{
              String{cxt.scratch_allocator(), name},
              steal(previous),
              steal(previous_shell_value), previous_special_definition_location,
              did_overlay_shell_value, did_clear_circular_reference
          });
          os::set_environment_variable(name, expanded_value.view());
          cxt.mark_exported(name);
        }
        if (is_path_name)
          cxt.program_resolver().assign_path(String{expanded_value.view()});
        if (name == "IFS")
          cxt.variable_store().set_field_separators(expanded_value.view());
      };
  for (let const &var : m_local_vars)
    do_apply_environment_assignment(*var.token);
  for (let const assignment : keyword_assignments)
    do_apply_environment_assignment(*assignment);
  cxt.write_xtrace(program_args);

  ASSERT(!program_args.is_empty());
  let const &program_name = program_args[0];
  let const last_argument = program_args.is_empty()
                                ? String{cxt.scratch_allocator()}
                                : program_args.back();

  let array_command_kind = assignment_builtin::None;
  if (!m_array_args.is_empty())
    array_command_kind = classify_assignment_builtin(program_args[0].view());

  if (const Expression *function_body = command_word_function;
      function_body != nullptr)
  {
    if (redirect_in_fd) {
      let const saved = os::save_and_replace_descriptor(0, *redirect_in_fd);
      dup_saved_descriptors.push(saved);
      os::close_fd(*redirect_in_fd);
      redirect_in_fd = koshka::None;
      if (!saved.is_dup2_ok)
        throw ErrorWithLocation{source_location(), "Bad file descriptor"};
    }

    let const do_call_function = [&]() throws -> i64 {
      let call_params = ArrayList<String>{heap_allocator()};
      call_params.reserve(program_args.count() - 1);
      for (usize i = 1; i < program_args.count(); i++)
        call_params.push_managed(program_args[i]);
      let bash_argument_frame_context = EvalContext::BashArgumentFrameContext{};
      cxt.enter_bash_function_argument_frame(bash_argument_frame_context,
                                             call_params);
      defer { cxt.leave_bash_argument_frame(bash_argument_frame_context); };
      let saved_params = steal(cxt.variable_store().positional_params());
      cxt.variable_store().positional_params() = steal(call_params);
      defer { cxt.variable_store().positional_params() = steal(saved_params); };

      let const untraced_debug_scope =
          UntracedTrapScope{cxt, UntracedTrapScope::Kind::Debug};
      let const untraced_err_scope =
          UntracedTrapScope{cxt, UntracedTrapScope::Kind::Err};
      let const untraced_return_scope =
          UntracedTrapScope{cxt, UntracedTrapScope::Kind::Return};

      cxt.enter_function_call(source_location());
      defer { cxt.leave_function_call(); };

      let const saved_loop_depth = cxt.execution_store().loop_depth();
      cxt.execution_store().loop_depth() = 0;
      defer { cxt.execution_store().loop_depth() = saved_loop_depth; };

      let const call_mark = cxt.expansion_store().scratch_arena().mark();
      defer { cxt.expansion_store().scratch_arena().release(call_mark); };

      cxt.enter_function_scope();
      cxt.push_function_call_name(program_name.view(),
                                  command_function_storage);
      defer
      {
        cxt.pop_function_call_name();
        cxt.leave_function_scope();
      };

      let const saved_terminal_exec =
          cxt.execution_store().terminal_exec_allowed();
      cxt.execution_store().terminal_exec_allowed() = false;
      defer
      {
        cxt.execution_store().terminal_exec_allowed() = saved_terminal_exec;
      };

      let const *const definition_info =
          command_function_storage.get_definition_info();
      let const should_swap_state =
          definition_info != nullptr &&
          !(definition_info->defining_state ==
            definition_state::from(cxt.runtime_state()));
      let const definition_scope = DefinitionStateScope{
          cxt,
          should_swap_state ? definition_info->defining_state
                            : definition_state::from(cxt.runtime_state()),
          definition_state_exit::PropagateMutations, should_swap_state};

      if (cxt.should_run_debug_trap()) {
        let const saved_call_location = cxt.source_store().current_location();
        let const was_control_flow_pending =
            cxt.control_flow_store().has_pending();

        cxt.source_store().set_current_location(
            function_body->source_location());
        cxt.run_named_trap(StringView{"DEBUG", 5});
        cxt.source_store().set_current_location(saved_call_location);

        if (!was_control_flow_pending && cxt.control_flow_store().has_pending())
        {
          return cxt.execution_store().last_exit_status();
        }
      }

      i64 function_ret = 0;
      try {
        function_ret = function_body->evaluate(cxt);
        if (cxt.should_run_return_trap()) {
          let const pending_kind = cxt.control_flow_store().has_pending()
                                       ? cxt.control_flow_store().pending().kind
                                       : control_flow::Kind::Normal;

          if (pending_kind != control_flow::Kind::Exit) {
            cxt.run_return_trap(pending_kind == control_flow::Kind::Return
                                    ? cxt.trap_store().status_before_return()
                                    : cxt.execution_store().last_exit_status());
          }
        }
      } catch (ErrorWithLocationAndDetails &error) {
        if (!error.was_rendered()) {
          let const trace_location = error.location();
          let const windowed = window_function_body_error(cxt, error);
          let const rendered_source =
              windowed.has_value() ? *windowed
                                   : cxt.source_store().current_source_view();
          show_message(error.to_string(rendered_source, &cxt));
          show_message(error.details_to_string(rendered_source, &cxt));
          cxt.print_source_backtrace(trace_location);
          error.set_rendered();
        }
        throw;
      } catch (ErrorWithLocation &error) {
        if (!error.was_rendered()) {
          let const trace_location = error.location();
          let const windowed = window_function_body_error(cxt, error);
          let const rendered_source =
              windowed.has_value() ? *windowed
                                   : cxt.source_store().current_source_view();
          show_message(error.to_string(rendered_source, &cxt));
          cxt.print_source_backtrace(trace_location);
          error.set_rendered();
        }
        throw;
      }

      if (cxt.control_flow_store().has_pending()) {
        let const kind = cxt.control_flow_store().pending().kind;
        if (kind == control_flow::Kind::Return) {
          function_ret = cxt.control_flow_store().pending().value;
          cxt.control_flow_store().clear();
        } else if (kind == control_flow::Kind::Break ||
                   kind == control_flow::Kind::Continue)
        {
          cxt.control_flow_store().clear();
        }
      }

      cxt.execution_store().set_last_argument(String{last_argument.view()});
      cxt.publish_single_pipe_status(static_cast<i32>(function_ret));
      SET_AND_RETURN_EXIT_STATUS(cxt, function_ret);
    };

    if (!is_async_command) return do_call_function();

    let const do_run_in_child = [](void *context, EvalContext &) -> i64 {
      return (*static_cast<decltype(do_call_function) *>(context))();
    };

    let expanded_child_source = String{cxt.scratch_allocator()};
    if (!os::can_fork_evaluator()) {
      for (usize i = 0; i < program_args.count(); i++) {
        if (i > 0) expanded_child_source.push(' ');
        append_shell_quoted_arg(expanded_child_source, program_args[i].view(),
                                true);
      }
    }

    return evaluate_async_with(
        cxt, do_run_in_child,
        const_cast<void *>(static_cast<const void *>(&do_call_function)),
        expanded_child_source.view());
  }

  let did_resolution_fail = false;
  i64 resolution_failure_status = 0;
  let const is_program_word_a_path =
      !program_args.is_empty() &&
      program_args[0].view().find_character('/').has_value();
  let const do_resolve_context = [&]() throws -> ExecContext {
    if (literal_lookup != nullptr && literal_lookup->builtin.has_value() &&
        !builtin_is_hidden_by_mood(*literal_lookup->builtin,
                                   cxt.runtime_state().get_mood()))
    {
      return ExecContext::make_from_resolved(
          source_location(),
          ResolvedCommand::from_builtin(*literal_lookup->builtin),
          steal(program_args), steal(program_arg_locations));
    }

    try {
      return ExecContext::make_from(
          source_location(), cxt.source_store().current_source_view(),
          steal(program_args),
          cxt.runtime_state().koshkit_utilities_are_reachable(),
          cxt.runtime_state().is_shopt_enabled(shopt_option_id::Checkhash),
          cxt.program_resolver(), steal(program_arg_locations),
          cxt.runtime_state().get_mood(),
          cxt.execution_store().shell_is_interactive() &&
              cxt.is_shopt_enabled("autocd"));
    } catch (CommandResolutionErrorWithLocation &e) {
      report_command_resolution_error(cxt, e);
      did_resolution_fail = true;
      resolution_failure_status = e.command_status();
      return ExecContext::make_from_unresolved(
          source_location(), static_cast<i32>(resolution_failure_status),
          StringView{});
    }
  };

  let ec = do_resolve_context();
  if (did_resolution_fail) {
    if (is_program_word_a_path ||
        cxt.runtime_state().get_mood() != mimic_mood::Posix)
    {
      cxt.job_table_store().forget_waited_jobs();
    }
    cxt.execution_store().set_last_exit_status(
        static_cast<i32>(resolution_failure_status));
    cxt.publish_single_pipe_status(static_cast<i32>(resolution_failure_status));
    return resolution_failure_status;
  }
  ec.has_stripped_array_operands = !m_array_args.is_empty();

  if (redirect_in_fd) ec.in_fd = redirect_in_fd.take();

  let const was_in_pipeline_stage =
      cxt.job_table_store().is_in_pipeline_stage();
  if (mode == root_evaluation_mode::PreparedAsyncCommand && ec.is_builtin())
    cxt.job_table_store().set_in_pipeline_stage(true);
  defer { cxt.job_table_store().set_in_pipeline_stage(was_in_pipeline_stage); };

  let const is_posix_regular_builtin =
      cxt.runtime_state().is_posix_mode() && ec.is_builtin() &&
      !is_command_special_builtin && ec.builtin_kind() != Builtin::Kind::Local;
  i32 ret = 0;
  try {
    ret = utils::execute_context(steal(ec), cxt,
                                 is_async_command ? execution_mode::Background
                                                  : execution_mode::Foreground);
  } catch (const InterruptErrorWithLocation &) {
    throw;
  } catch (ErrorWithLocation &error) {
    let const is_soft_posix_error =
        is_posix_regular_builtin && !error.is_line_discarding();
    if (error.is_script_fatal()) throw;
    if (!cxt.runtime_state().is_bash_compatible() && !is_soft_posix_error)
      throw;

    if (!error.was_rendered()) {
      let const trace_location = error.location();
      if (let const windowed = window_function_body_error(cxt, error);
          windowed.has_value())
      {
        show_message(error.to_string(*windowed, &cxt));
      } else {
        show_message(
            error.to_string(cxt.source_store().current_source_view(), &cxt));
      }
      cxt.print_source_backtrace(trace_location, false);
      error.set_rendered();
    }

    if (!is_soft_posix_error) throw;

    ret = 2;
  }
  cxt.execution_store().set_last_argument(String{last_argument.view()});

  if (!m_array_args.is_empty()) {
    let const is_local = array_command_kind == assignment_builtin::Local;
    let const is_declare = array_command_kind == assignment_builtin::Declare;
    let const is_function_local =
        is_declare && cxt.scope_store().local_scope_depth() > 0;
    let const is_export = array_command_kind == assignment_builtin::Export;
    let const is_readonly_kind =
        array_command_kind == assignment_builtin::Readonly;
    let is_readonly_request = is_readonly_kind;
    let is_export_request = is_export;
    let did_request_readonly_flag = false;
    let should_print_declaration = false;
    let is_global_request = false;
    let is_associative_request = false;
    let should_mark_integer = false;
    let should_unmark_integer = false;
    let should_mark_lowercase = false;
    let should_unmark_lowercase = false;
    let should_mark_uppercase = false;
    let should_unmark_uppercase = false;
    if (is_declare || is_local || is_readonly_kind) {
      let text_storage = String{heap_allocator()};
      for (let const arg : m_args) {
        let const text = borrowed_token_text(arg, text_storage);
        if (text.length < 2) continue;

        let const lead = text[0];
        if (lead != '-' && lead != '+') continue;

        let const is_set_request = lead == '-';

        for (usize i = 1; i < text.length; ++i) {
          switch (text[i]) {
          case 'r':
            if (is_set_request) did_request_readonly_flag = true;
            break;

          case 'p':
            if (is_set_request && !is_readonly_kind) {
              should_print_declaration = true;
            }
            break;

          case 'A':
            if (is_set_request) is_associative_request = true;
            break;

          case 'g':
            if (is_set_request) is_global_request = true;
            break;

          case 'i':
            should_mark_integer = is_set_request;
            should_unmark_integer = !is_set_request;
            break;

          case 'l':
            should_mark_lowercase = is_set_request;
            should_unmark_lowercase = !is_set_request;
            break;

          case 'u':
            should_mark_uppercase = is_set_request;
            should_unmark_uppercase = !is_set_request;
            break;

          case 'x':
            if (is_set_request) is_export_request = true;
            break;

          default: break;
          }
        }
      }
    }
    if (should_mark_lowercase && should_mark_uppercase) {
      should_mark_lowercase = false;
      should_mark_uppercase = false;
      should_unmark_lowercase = true;
      should_unmark_uppercase = true;
    }

    if (did_request_readonly_flag && !should_print_declaration) {
      is_readonly_request = true;
    }

    for (let const &assignment : m_array_args) {
      if (is_local || (is_function_local && !is_global_request)) {
        cxt.declare_local(assignment.name, true);
      }
      if (should_mark_integer)
        cxt.variable_store().attributes().mark_integer(assignment.name);
      if (should_unmark_integer)
        cxt.variable_store().attributes().unmark_integer(assignment.name);
      if (should_unmark_lowercase)
        cxt.variable_store().attributes().unmark_lowercase(assignment.name);
      if (should_unmark_uppercase)
        cxt.variable_store().attributes().unmark_uppercase(assignment.name);
      if (should_mark_lowercase)
        cxt.variable_store().attributes().mark_lowercase(assignment.name);
      if (should_mark_uppercase)
        cxt.variable_store().attributes().mark_uppercase(assignment.name);
      let subscript_flags = Bitset{heap_allocator()};
      ArrayList<String> values = cxt.process_args(
          assignment.elements, nullptr, argument_lifetime::Persistent,
          is_associative_request || cxt.is_associative_array(assignment.name)
              ? argument_context::AssociativeLiteral
              : argument_context::ArrayLiteral,
          &subscript_flags);
      do_trace_array_assignment(assignment, values);
      if (is_associative_request)
        cxt.declare_associative_array(assignment.name);
      cxt.assign_indexed_array_elements(
          assignment.name, values, assignment.update_mode, &subscript_flags);
      if (is_export_request) cxt.mark_exported(assignment.name);
      if (is_readonly_request)
        cxt.variable_store().attributes().mark_readonly(assignment.name);

      if (should_print_declaration) {
        let line = String{cxt.scratch_allocator()};
        if (append_variable_declaration(cxt, assignment.name, line)) {
          if (!os::write_all(KOSH_STDOUT, line.data(), line.count()))
            throw Error{"Unable to write to stdout: " +
                        os::last_system_error_message()};
        }
      }
    }
  }

  cxt.execution_store().set_last_exit_status(static_cast<i32>(ret));
  cxt.publish_single_pipe_status(static_cast<i32>(ret));
  return ret;
}

} /* namespace expressions */

} /* namespace koshka */
