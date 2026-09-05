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

#include "Arena.hpp"
#include "Builtin.hpp"
#include "Cli.hpp"
#include "Common.hpp"
#include "Debug.hpp"
#include "Errors.hpp"
#include "Eval.hpp"
#include "Expressions.hpp"
#include "ExpressionsInternal.hpp"
#include "Koshkit.hpp"
#include "Lexer.hpp"
#include "Optimizer.hpp"
#include "Platform.hpp"
#include "Tokens.hpp"
#include "Trace.hpp"
#include "Utils.hpp"

namespace koshka {

namespace expressions {

using namespace internal;

namespace {

/* Replace a command word that names an alias with the alias body. The body is
   split on whitespace, and a name already expanded is not expanded again so a
   self-referential alias terminates. A quoted space inside the body is not
   preserved, since the full tokenizer is not re-run. */
fn expand_command_aliases(EvalContext &cxt, ArrayList<String> &args,
                          ArrayList<SourceLocation> &arg_locations) throws
    -> void
{
  if (!cxt.has_aliases() || !cxt.is_shopt_enabled("expand_aliases")) return;

  HashSet already_expanded{heap_allocator()};

  while (!args.is_empty()) {
    let const &word = args[0];

    if (already_expanded.contains(word.view())) break;

    let const body = cxt.get_alias(word);
    if (!body.has_value()) break;
    already_expanded.add(word.view());
    LOG(Debug, "expanding the alias '%s'", word.c_str());

    let rebuilt = ArrayList<String>{heap_allocator()};
    let rebuilt_locations = ArrayList<SourceLocation>{heap_allocator()};
    let current = String{cxt.scratch_allocator()};
    let const &body_value = *body;
    /* An alias body word has no span in this command, so it inherits the
       command word's location. */
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

/* Whether the command word is itself a glob pattern. The lone [ that opens a
   test command carries no closing ] in the same word and is left alone. */
static fn command_word_is_glob(const Word &word) wontthrow -> bool
{
  bool has_open_bracket = false;
  for (const WordSegment &segment : word.segments) {
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

hot fn SimpleCommand::evaluate_impl(EvalContext &cxt) const throws -> i64
{
  /* A command may have no words when it is only a redirection or only
     assignments, so those still run below. */
  ASSERT(m_args.count() > 0 || !m_redirections.is_empty() ||
         m_local_vars.count() > 0 || !m_array_args.is_empty());

  cxt.set_current_location(source_location());

  if (cxt.bash_dynamic_variables_enabled())
    cxt.set_current_command(utils::merge_tokens_to_string(m_args));

  if (cxt.has_debug_trap() && !cxt.is_posix_mode())
    cxt.run_named_trap(StringView{"DEBUG", 5});

  /* The check reads the typed command word before its expansion, so a pattern
     that happens to match a single file is still caught. */
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
      if (cxt.mood() == mimic_mood::Default)
        throw ErrorWithLocationAndDetails{location, message, note};
      cxt.show_runtime_warning_at(location, message, note);
    }
  }

  if (cxt.should_echo()) {
    koshka::print(utils::merge_tokens_to_string(m_args) + "\n");
    koshka::flush();
  }

  let const args_mark = cxt.scratch_mark();
  defer { cxt.scratch_release(args_mark); };
  /* The mark is taken before the expansion so this command reaps only the
     process substitution it opens, leaving an enclosing command's for that
     command to reap. */
  let const substitution_mark = cxt.mark_process_substitutions();
  let program_arg_locations =
      ArrayList<SourceLocation>{cxt.scratch_allocator()};
  let program_args =
      cxt.process_args(m_args, argument_lifetime::Transient,
                       argument_context::Command, &program_arg_locations);
  defer { cxt.cleanup_process_substitutions(substitution_mark); };
  expand_command_aliases(cxt, program_args, program_arg_locations);

  if (!is_async() && !cxt.is_in_pipeline_stage())
    utils::set_foreground_program_title(program_args, cxt);

  if (!program_args.is_empty())
    cxt.guard_restricted_path(program_args[0].view(),
                              program_arg_locations.is_empty()
                                  ? source_location()
                                  : program_arg_locations[0],
                              restricted_path_use::Command);

  LOG(Info, "dispatching the command '%s' with %zu words",
      program_args.is_empty() ? "" : program_args[0].c_str(),
      program_args.count());

  /* A bare exec, exec with no further argument, applies its redirections to the
     shell's own descriptors for good. A function named exec shadows it. */
  FunctionBodyHandle command_function_storage{};
  if (!program_args.is_empty() && cxt.has_functions()) {
    if (let const *storage = cxt.find_function_storage(program_args[0].view());
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

  /* A POSIX special builtin not shadowed by a function exits the shell on a
     redirection error and keeps a prefix assignment, so it is computed once and
     read on both paths. */
  const bool is_command_special_builtin =
      !program_args.is_empty() && command_word_function == nullptr &&
      is_special_builtin_name(program_args[0].view());

  /* A heredoc on the standard input passes its staged descriptor through this
     slot, and the guard closes it on any path that does not hand it off. */
  Maybe<os::descriptor> redirect_in_fd;
  /* The standard fds are routed in source order so a later 2>&1 copies the
     descriptor its source points at now rather than the one a deferred slot
     would place last. */
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

  /* Set true just before a redirection resource failure throws, so the catch
     tells it apart from a fatal expansion error in a target word. */
  bool did_redirection_open_fail = false;
  try {
    for (let const &original_redir : m_redirections) {
      let redir = original_redir;
      let const r = resolve_redirection(redir, cxt, source_location(),
                                        &did_redirection_open_fail,
                                        /*allow_fd_memoization=*/!is_bare_exec);

      redir.fd = allocate_redirection_descriptor(original_redir, r, cxt,
                                                 source_location(),
                                                 &did_redirection_open_fail);

      switch (r.kind) {
      case redirection_outcome::Heredoc: {
        let const body_fd = r.opened_fd;
        /* Inside an in-process subshell the move is backed up first, so it
           stays contained the way a fork would contain it. */
        if (is_bare_exec) {
          cxt.snapshot_subshell_descriptor(redir.fd);
          koshka::flush();
          os::replace_descriptor(redir.fd, body_fd);
          if (!os::descriptor_is_shell_fd(body_fd, redir.fd))
            os::close_fd(body_fd);
          break;
        }

        /* A numbered heredoc such as 3<<EOF targets descriptor N, staged onto
           the real shell fd N around the command and restored afterward. */
        if (redir.fd == 0) {
          if (redirect_in_fd) os::close_fd(*redirect_in_fd);
          redirect_in_fd = body_fd;
          break;
        }

        /* The temp file already lands on fd N when mkstemp handed back that
           number, so the collision is handled directly and the restore closes
           fd N, which was free before mkstemp claimed it. */
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
        /* The filename lands on the standard output and the standard error
           follows it, the pair bash builds for csh >&file. */
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

        /* Inside an in-process subshell the move is backed up and contained at
           the subshell's end. The flush keeps buffered output on the original
           descriptor before it moves. */
        if (is_bare_exec) {
          cxt.snapshot_subshell_descriptor(redir.fd);
          koshka::flush();

          if (from_fd == Redirection::DUP_FD_CLOSE) {
            os::close_shell_fd(redir.fd);
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

        /* A cross-route such as 2>&1 points the real shell descriptor at the
           target in source order so a later file redirect on the source does
           not change what the copy already captured. */
        koshka::flush();

        if (from_fd == Redirection::DUP_FD_CLOSE) {
          let const saved = os::save_and_replace_descriptor(
              redir.fd, os::descriptor_for_shell_fd(redir.fd));
          dup_saved_descriptors.push(saved);
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
        /* The dup2 onto fd N replaces whatever fd N held, so a second exec onto
           the same number closes the earlier file rather than leaking it. The
           flush keeps buffered output on the original descriptor. */
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

        /* Staged onto the real shell fd N in source order so a later 2>&1
           copies the descriptor fd N points at now. A redirect onto fd 1 or 2
           mutates the shell's own stdout or stderr, so it is flushed first. */
        if (redir.fd == 1 || redir.fd == 2) {
          koshka::flush();
        }
        const bool is_file_target_fd =
            os::descriptor_is_shell_fd(file_fd, redir.fd);
        if (is_file_target_fd) {
          /* open returned fd N itself, so the collision is recorded for restore
             without a close. */
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
  } catch (const ErrorWithLocation &redirection_error) {
    /* Only an open or dup failure is caught here. An expansion error in a
       target word stays fatal. */
    if (!did_redirection_open_fail) throw;
    /* A special builtin's redirection error exits a non-interactive shell, so
       it is not recovered. The defers above put the partial redirections
       back. */
    if (is_command_special_builtin) throw;
    const String *source = cxt.current_source();
    show_message(redirection_error.to_string(
        source != nullptr ? source->view() : StringView{}, &cxt));
    /* bash reports a redirection failure with status 1 and dash with 2. */
    let const redirection_status = cxt.is_bash_compatible() ? 1 : 2;
    cxt.set_last_exit_status(redirection_status);
    cxt.publish_single_pipe_status(redirection_status);
    return redirection_status;
  }

  /* The append form reads the current value from the shell store first so a
     non-exported shell variable still contributes. An integer name evaluates
     the join to its decimal here. */
  let const do_apply_append = [&](StringView name, String &value_ref) throws {
    let appended = String{cxt.scratch_allocator()};
    if (let const existing = cxt.get_variable_value(name))
      appended.append(existing->view());
    if (cxt.is_integer_variable(name)) {
      cxt.append_integer_expression(appended, value_ref.view());
      value_ref = cxt.evaluate_arithmetic_text(appended.view());
    } else {
      appended += value_ref;
      value_ref = steal(appended);
    }
  };
  let const do_trace_assignment = [&](StringView name, bool is_append,
                                      StringView value) throws -> void {
    if (!cxt.should_echo_expanded()) return;
    let trace = String{cxt.scratch_allocator(), name};
    trace += is_append ? "+=" : "=";
    append_shell_quoted_arg(trace, value);
    cxt.write_xtrace(trace.view());
  };
  let const do_trace_array_assignment =
      [&](const array_builtin_assignment &assignment,
          const ArrayList<String> &values) throws -> void {
    if (!cxt.should_echo_expanded()) return;
    let trace = String{cxt.scratch_allocator(), assignment.name.view()};
    trace += assignment.is_append ? "+=(" : "=(";
    for (usize i = 0; i < values.count(); i++) {
      if (i > 0) trace.push(' ');
      append_shell_quoted_arg(trace, values[i].view());
    }
    trace.push(')');
    cxt.write_xtrace(trace.view());
  };

  /* An expansion may drop every word. A command-less line still carries its
     assignments, which persist in the current shell. */
  if (program_args.is_empty()) {
    for (let const &var : m_local_vars) {
      let const name = var.get_name();
      let value = cxt.expand_word_for_assignment(var.get_value());
      do_trace_assignment(name, var.is_append(), value.view());
      if (var.is_append()) do_apply_append(name, value);
      cxt.set_shell_variable(name, value);
      if (cxt.export_all()) {
        cxt.record_environment_change(name);
        os::set_environment_variable(name, value.view());
        cxt.mark_exported(name);
      }
    }
    /* Bare array assignments apply after the scalars in source order. */
    for (let const &assignment : m_array_args) {
      if (cxt.is_readonly(assignment.name))
        throw Error{"Unable to assign '" + assignment.name +
                    "' because it is read only"};
      ArrayList<String> values =
          cxt.process_args(assignment.elements, argument_lifetime::Persistent,
                           argument_context::ArrayLiteral);
      do_trace_array_assignment(assignment, values);
      cxt.assign_indexed_array_elements(assignment.name, values,
                                        assignment.is_append);
    }
    /* A value that ran a command substitution leaves the status of the last
       one. A line with no substitution resets to 0. */
    let const do_token_ran_substitution = [&](const Token *token) {
      if (token == nullptr || token->kind() != Token::Kind::Word) {
        return false;
      }
      return static_cast<const tokens::WordToken *>(token)
          ->word()
          .runs_substitution();
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
    if (!ran_substitution) cxt.set_last_exit_status(0);
    cxt.publish_single_pipe_status(cxt.last_exit_status());
    return cxt.last_exit_status();
  }

  /* A prefix assignment before a special builtin persists after the command as
     a regular shell variable. A per-command assignment otherwise applies to the
     environment for this command, restored on every exit path. */
  struct saved_env_var
  {
    String name;
    Maybe<String> previous_value;
  };
  ArrayList<saved_env_var> saved_env{cxt.scratch_allocator()};
  saved_env.reserve(m_local_vars.count());
  /* A prefix IFS=... drives the shell's own word splitting for this command
     through the live separator cache. The effective separators are saved before
     the first such prefix and restored on exit. */
  bool was_ifs_assigned = false;
  String saved_ifs_separators{cxt.scratch_allocator()};
  Maybe<ProgramResolver> saved_program_resolver{};
  /* The assignments apply left to right, each committed before the next is
     expanded, so a later value reads an earlier same-line one. */
  for (let const &var : m_local_vars) {
    let const name = var.get_name();
    if (cxt.is_readonly(name))
      throw Error{"Unable to assign '" + name + "' because it is read only"};
    const bool is_read_field_separator =
        name == "IFS" && command_word_function == nullptr &&
        !program_args.is_empty() && program_args[0] == "read";
    Maybe<String> previous;
    if (!is_read_field_separator) previous = os::get_environment_variable(name);
    let expanded_value = String{cxt.scratch_allocator()};
    try {
      expanded_value = cxt.expand_word_for_assignment(var.get_value());
    } catch (const ErrorWithLocation &) {
      throw;
    } catch (const Error &e) {
      relocate_error(e, source_location());
    }
    do_trace_assignment(name, var.is_append(), expanded_value.view());
    if (var.is_append()) do_apply_append(name, expanded_value);

    /* A special builtin keeps the assignment outside the bash mood, so it
       commits to the store. The bash mood drops it after the command, so it
       falls to the temporary path instead. */
    if (is_command_special_builtin && !cxt.is_bash_compatible()) {
      cxt.set_shell_variable(name, expanded_value);
      if (cxt.export_all()) {
        cxt.record_environment_change(name);
        os::set_environment_variable(name, expanded_value.view());
        cxt.mark_exported(name);
      }
      continue;
    }

    if (!is_read_field_separator) {
      saved_env.push(saved_env_var{String{name}, steal(previous)});
      os::set_environment_variable(name, expanded_value.view());
      cxt.mark_exported(name);
    }
    /* The resolver reads its own MAYBE_PATH, so a prefix PATH=... must update
       it for the environment write to change the search order. */
    if (name == "PATH") {
      if (!saved_program_resolver.has_value())
        saved_program_resolver =
            Maybe<ProgramResolver>{cxt.get_program_resolver()};
      cxt.get_program_resolver().assign_path(String{expanded_value.view()});
    }
    /* The value before the first IFS prefix is saved once, so a name repeated
       on the line still reverts to where it began. */
    if (name == "IFS") {
      if (!was_ifs_assigned) {
        was_ifs_assigned = true;
        saved_ifs_separators =
            cxt.get_variable_value("IFS").value_or(String{" \t\n"});
      }
      cxt.set_field_separators(expanded_value.view());
    }
  }
  cxt.write_xtrace(program_args);
  defer
  {
    /* The restore runs newest first, so a name spelled more than once restores
       to the value it held before the first of its assignments. */
    for (usize i = saved_env.count(); i > 0; i--) {
      const saved_env_var &restore = saved_env[i - 1];
      if (restore.previous_value)
        os::set_environment_variable(restore.name.view(),
                                     restore.previous_value->view());
      else
        os::unset_environment_variable(restore.name.view());
      cxt.sync_exported_after_restore(restore.name.view(),
                                      restore.previous_value.has_value());
    }
    if (saved_program_resolver.has_value())
      cxt.get_program_resolver() = steal(*saved_program_resolver);
    if (was_ifs_assigned) cxt.set_field_separators(saved_ifs_separators.view());
  };

  ASSERT(!program_args.is_empty());
  let const &program_name = program_args[0];
  let const last_argument = program_args.is_empty()
                                ? String{cxt.scratch_allocator()}
                                : program_args.back();

  /* The command name is classified for the array-argument application below,
     since the argument vector moves into the exec context before that point. */
  let array_command_kind = assignment_builtin::None;
  if (!m_array_args.is_empty())
    array_command_kind = classify_assignment_builtin(program_args[0].view());

  if (const Expression *function_body = command_word_function;
      function_body != nullptr)
  {
    /* An input redirection on the call lands on the real fd 0 for the body's
       duration, so the in-process body and every child it spawns read the
       staged bytes. */
    if (redirect_in_fd) {
      let const saved = os::save_and_replace_descriptor(0, *redirect_in_fd);
      dup_saved_descriptors.push(saved);
      os::close_fd(*redirect_in_fd);
      redirect_in_fd = koshka::None;
      if (!saved.is_dup2_ok)
        throw ErrorWithLocation{source_location(), "Bad file descriptor"};
    }

    let call_params = ArrayList<String>{heap_allocator()};
    call_params.reserve(program_args.count() - 1);
    for (usize i = 1; i < program_args.count(); i++)
      call_params.push_managed(program_args[i]);
    let bash_argument_frame_context = EvalContext::BashArgumentFrameContext{};
    cxt.enter_bash_function_argument_frame(bash_argument_frame_context,
                                           call_params);
    defer { cxt.leave_bash_argument_frame(bash_argument_frame_context); };
    let saved_params = cxt.take_positional_params();
    cxt.set_positional_params(steal(call_params));
    defer { cxt.set_positional_params(steal(saved_params)); };

    /* Bound the call nesting so a function that recurses without a base case
       errors with a caret here rather than exhausting the native stack. */
    cxt.enter_function_call(source_location());
    defer { cxt.leave_function_call(); };

    /* A loop in the caller is not the body's to break, so the body starts with
       a fresh loop count. */
    let const saved_loop_depth = cxt.loop_depth();
    cxt.set_loop_depth(0);
    defer { cxt.set_loop_depth(saved_loop_depth); };

    /* Registered first so it runs last, after the scope pop restores the
       locals. */
    let const call_mark = cxt.scratch_mark();
    defer { cxt.scratch_release(call_mark); };

    cxt.enter_function_scope();
    cxt.push_function_call_name(program_name.view(), command_function_storage);
    defer
    {
      cxt.pop_function_call_name();
      cxt.leave_function_scope();
    };

    /* A command at the tail of the body must not exec the shell in place, since
       the call's cleanup has to run after the body. */
    let const saved_terminal_exec = cxt.terminal_exec_allowed();
    cxt.set_terminal_exec_allowed(false);
    defer { cxt.set_terminal_exec_allowed(saved_terminal_exec); };

    /* The body runs in the mood and diagnostics state the function was defined
       in, so a function defined in bash mood runs bash even after a later set
       --mood. The swap only happens when the defining state differs from the
       live state. */
    let const *const definition_info =
        command_function_storage.get_definition_info();
    let const needs_state_swap =
        definition_info != nullptr &&
        (definition_info->defining_runtime.mood != cxt.mood() ||
         definition_info->defining_runtime.warning_level !=
             cxt.warning_level() ||
         definition_info->defining_runtime.is_diagnostics_disabled() !=
             cxt.diagnostics_disabled() ||
         definition_info->defining_runtime.is_annoying_diagnostics_enabled() !=
             cxt.annoying_diagnostics_enabled());
    Maybe<function_runtime_state> saved_runtime_state = None;
    if (needs_state_swap) {
      saved_runtime_state =
          cxt.enter_definition_state(definition_info->defining_runtime);
    }
    defer
    {
      if (saved_runtime_state.has_value())
        cxt.leave_definition_state(*saved_runtime_state);
    };

    /* A located error thrown from the body is rendered here while the stack
       still names the function, since the top-level handler cannot reach the
       definition file once this frame unwinds. window_function_body_error
       rebases the position onto the definition copy. The error is marked
       rendered so the top-level handler keeps the status without printing it
       twice. */
    let const previous_function_arena = FUNCTION_ARENA;
    if (command_function_storage.has_value())
      FUNCTION_ARENA = command_function_storage.get_arena();
    defer { FUNCTION_ARENA = previous_function_arena; };

    i64 function_ret = 0;
    try {
      function_ret = function_body->evaluate(cxt);
      if (cxt.should_run_return_trap())
        cxt.run_named_trap(StringView{"RETURN", 6});
    } catch (ErrorWithLocationAndDetails &error) {
      if (!error.was_rendered())
        if (let const windowed = window_function_body_error(cxt, error);
            windowed.has_value())
        {
          show_message(error.to_string(*windowed, &cxt));
          show_message(error.details_to_string(*windowed, &cxt));
          error.set_rendered();
        }
      throw;
    } catch (ErrorWithLocation &error) {
      if (!error.was_rendered())
        if (let const windowed = window_function_body_error(cxt, error);
            windowed.has_value())
        {
          show_message(error.to_string(*windowed, &cxt));
          error.set_rendered();
        }
      throw;
    }

    /* A return supplies the status. A break or continue is scoped to a loop
       inside this function and is consumed here. An exit stays pending for the
       shell. */
    if (cxt.has_pending_control_flow()) {
      let const kind = cxt.pending_control_flow().kind;
      if (kind == control_flow::Kind::Return) {
        function_ret = cxt.pending_control_flow().value;
        cxt.clear_control_flow();
      } else if (kind == control_flow::Kind::Break ||
                 kind == control_flow::Kind::Continue)
      {
        cxt.clear_control_flow();
      }
    }

    cxt.set_last_argument(last_argument.view());
    cxt.publish_single_pipe_status(static_cast<i32>(function_ret));
    SET_AND_RETURN_EXIT_STATUS(cxt, function_ret);
  }

  Maybe<ExecContext> resolved_ec;
  try {
    let const *source = cxt.current_source();
    resolved_ec = ExecContext::make_from(
        source_location(), source != nullptr ? source->view() : StringView{},
        steal(program_args), cxt.mood(), cxt.koshkit_utilities_are_reachable(),
        cxt.is_shopt_enabled(shopt_option_id::Checkhash),
        cxt.get_program_resolver(), steal(program_arg_locations));
  } catch (const CommandResolutionErrorWithLocation &e) {
    report_command_resolution_error(cxt, e);
    let const status = e.command_status();
    cxt.set_last_exit_status(static_cast<i32>(status));
    cxt.publish_single_pipe_status(static_cast<i32>(status));
    return status;
  }
  let ec = resolved_ec.take();

  /* The exec context now owns and closes the staged input descriptor. The
     stdout and stderr redirects already took effect on the real shell fds. */
  if (redirect_in_fd) ec.in_fd = redirect_in_fd.take();

  let const ret = utils::execute_context(
      steal(ec), cxt,
      is_async() ? execution_mode::Background : execution_mode::Foreground);
  cxt.set_last_argument(last_argument.view());

  /* An assignment builtin with NAME=(...) array arguments applies them after it
     runs, in the scope the builtin selects. The builtin ran first, so a local
     outside a function has already errored and the elements never reach here.
   */
  if (!m_array_args.is_empty()) {
    let const is_local = array_command_kind == assignment_builtin::Local;
    let const is_declare = array_command_kind == assignment_builtin::Declare;
    let const is_function_local = is_declare && cxt.in_function_scope();
    let const is_export = array_command_kind == assignment_builtin::Export;
    /* The -r flag sits in the builtin's arguments, so it is read off them. */
    let is_readonly_request =
        array_command_kind == assignment_builtin::Readonly;
    /* The -A flag routes to the string-keyed store rather than the indexed
       one. */
    let is_associative_request = false;
    let should_mark_integer = false;
    let should_unmark_integer = false;
    let should_mark_lowercase = false;
    let should_unmark_lowercase = false;
    let should_mark_uppercase = false;
    let should_unmark_uppercase = false;
    if (is_declare || is_local) {
      for (let const arg : m_args) {
        let const text = arg->raw_string();
        if (text.length() >= 2 &&
            (text.view()[0] == '-' || text.view()[0] == '+'))
        {
          if (!is_readonly_request && text.view()[0] == '-' &&
              text.view().find_character('r').has_value())
          {
            is_readonly_request = true;
          }
          if (text.view()[0] == '-' &&
              text.view().find_character('A').has_value())
            is_associative_request = true;
          if (text.view().find_character('i').has_value()) {
            should_mark_integer = text.view()[0] == '-';
            should_unmark_integer = text.view()[0] == '+';
          }
          if (text.view().find_character('l').has_value()) {
            should_mark_lowercase = text.view()[0] == '-';
            should_unmark_lowercase = text.view()[0] == '+';
          }
          if (text.view().find_character('u').has_value()) {
            should_mark_uppercase = text.view()[0] == '-';
            should_unmark_uppercase = text.view()[0] == '+';
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
    for (let const &assignment : m_array_args) {
      if (is_local || is_function_local) {
        cxt.declare_local(assignment.name, true);
      }
      if (should_mark_integer) cxt.mark_integer(assignment.name);
      if (should_unmark_integer) cxt.unmark_integer(assignment.name);
      if (should_unmark_lowercase) cxt.unmark_lowercase(assignment.name);
      if (should_unmark_uppercase) cxt.unmark_uppercase(assignment.name);
      if (should_mark_lowercase) cxt.mark_lowercase(assignment.name);
      if (should_mark_uppercase) cxt.mark_uppercase(assignment.name);
      ArrayList<String> values =
          cxt.process_args(assignment.elements, argument_lifetime::Persistent,
                           argument_context::ArrayLiteral);
      do_trace_array_assignment(assignment, values);
      if (is_associative_request) {
        /* A bare element with no bracketed key becomes a key with an empty
           value. */
        cxt.declare_associative_array(assignment.name);
        for (let const &element : values) {
          let const text = element.view();
          if (!text.is_empty() && text[0] == '[') {
            if (let const close = text.find_character(']');
                close.has_value() && *close + 1 < text.length &&
                text[*close + 1] == '=')
            {
              cxt.set_associative_element(
                  assignment.name, text.substring_of_length(1, *close - 1),
                  text.substring(*close + 2));
              continue;
            }
          }
          cxt.set_associative_element(assignment.name, text, StringView{});
        }
      } else {
        cxt.assign_indexed_array_elements(assignment.name, values,
                                          assignment.is_append);
      }
      if (is_export) cxt.mark_exported(assignment.name);
      if (is_readonly_request) cxt.mark_readonly(assignment.name);
    }
  }

  cxt.set_last_exit_status(static_cast<i32>(ret));
  cxt.publish_single_pipe_status(static_cast<i32>(ret));
  return ret;
}

} /* namespace expressions */

} /* namespace koshka */
